// HTTP de saída (HttpOutPool e cb.http). Sem rede externa: validação pura de endereços/URLs e um servidor httplib
// local (127.0.0.1) com o pool no modo de teste (http:// e loopback liberados só pelo construtor de teste).
#include <catch2/catch_test_macros.hpp>

#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "api_env.hpp"
#include "cbscript/http_out.hpp"

using namespace cbscript;

namespace {

// ------------------------------------------------------------------------------------------- servidor local

struct TestServer {
  std::unique_ptr<httplib::Server> svr;
  int port = 0;
  std::thread th;
  std::mutex mu;
  std::condition_variable cv;
  bool released = false;
  std::map<std::string, int> hits;
  std::vector<std::string> order;
  std::vector<std::string> seenHeaders;  // "nome: valor" do último pedido em /echo
  std::string lastBody, lastContentType, lastHost;

  explicit TestServer(std::unique_ptr<httplib::Server> s = std::make_unique<httplib::Server>()) : svr(std::move(s)) {
    auto hit = [this](const httplib::Request& rq) {
      std::lock_guard<std::mutex> lk(mu);
      ++hits[rq.path];
    };
    svr->Get("/hello", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_header("X-Test", "um");
      rs.set_content("oi", "text/plain");
    });
    auto echo = [this, hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      {
        std::lock_guard<std::mutex> lk(mu);
        seenHeaders.clear();
        for (auto& [k, v] : rq.headers) seenHeaders.push_back(k + ": " + v);
        lastBody = rq.body;
        lastContentType = rq.get_header_value("Content-Type");
        lastHost = rq.get_header_value("Host");
      }
      rs.status = 201;
      rs.set_content(rq.method + ":" + rq.body, "text/plain");
    };
    svr->Get("/echo", echo);
    svr->Post("/echo", echo);
    svr->Get("/status404", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.status = 404;
      rs.set_content("nao", "text/plain");
    });
    svr->Get("/big", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_content(std::string(size_t{2} << 20, 'x'), "application/octet-stream");
    });
    svr->Get("/bigchunked", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_chunked_content_provider("application/octet-stream", [](size_t off, httplib::DataSink& sink) {
        if (off >= (size_t{3} << 20)) {
          sink.done();
          return true;
        }
        std::string chunk(64 * 1024, 'y');
        return sink.write(chunk.data(), chunk.size());
      });
    });
    svr->Get("/exact", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_content(std::string(size_t{1} << 20, 'z'), "application/octet-stream");
    });
    svr->Get("/bighdr", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      for (int i = 0; i < 20; ++i) rs.set_header("X-H" + std::to_string(i), std::string(4000, 'h'));
      rs.set_content("ok", "text/plain");
    });
    svr->Get("/slow", [this, hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      std::unique_lock<std::mutex> lk(mu);
      cv.wait_for(lk, std::chrono::seconds(20), [this] { return released; });
      rs.set_content("tarde", "text/plain");
    });
    svr->Get(R"(/order/(\w+))", [this](const httplib::Request& rq, httplib::Response& rs) {
      std::unique_lock<std::mutex> lk(mu);
      order.push_back(rq.matches[1]);
      ++hits[rq.path];
      cv.wait_for(lk, std::chrono::seconds(20), [this] { return released; });
      rs.set_content("ok", "text/plain");
    });
    svr->Get("/redir-in", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_redirect("/hello");
    });
    svr->Get("/redir-out", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_redirect("https://evil.example.com/x?token=segredo");
    });
    svr->Get("/redir-port", [this, hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_redirect("http://localhost:" + std::to_string(port + 1) + "/hello");
    });
    svr->Get("/redir-ip", [this, hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      rs.set_redirect("http://127.0.0.1:" + std::to_string(port) + "/hello");
    });
    svr->Get(R"(/hop/(\d+))", [hit](const httplib::Request& rq, httplib::Response& rs) {
      hit(rq);
      const int n = std::stoi(rq.matches[1]);
      if (n == 0)
        rs.set_content("fim", "text/plain");
      else
        rs.set_redirect("/hop/" + std::to_string(n - 1));
    });
    port = svr->bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    th = std::thread([this] { svr->listen_after_bind(); });
    svr->wait_until_ready();
  }
  ~TestServer() {
    release();
    svr->stop();
    th.join();
  }
  void release() {
    std::lock_guard<std::mutex> lk(mu);
    released = true;
    cv.notify_all();
  }
  int hitCount(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = hits.find(path);
    return it == hits.end() ? 0 : it->second;
  }
  int totalHits() {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (auto& [k, v] : hits) n += v;
    return n;
  }
  std::vector<std::string> orderNow() {
    std::lock_guard<std::mutex> lk(mu);
    return order;
  }
  std::string base() const { return "http://localhost:" + std::to_string(port); }
  std::string allow() const { return "localhost:" + std::to_string(port); }
};

// Certificado autoassinado (EC P-256) para `dnsName`, gravado em PEM.
void makeCert(const fs::path& certPath, const fs::path& keyPath, const std::string& dnsName) {
  EVP_PKEY* pkey = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
  REQUIRE(pkey != nullptr);
  X509* x = X509_new();
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_getm_notBefore(x), -3600);
  X509_gmtime_adj(X509_getm_notAfter(x), 24 * 3600);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(dnsName.c_str()), -1,
                             -1, 0);
  X509_set_issuer_name(x, name);
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
  const std::string san = "DNS:" + dnsName;
  for (auto [nid, val] : {std::pair<int, const char*>{NID_subject_alt_name, san.c_str()},
                          std::pair<int, const char*>{NID_basic_constraints, "critical,CA:TRUE"}}) {
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, val);
    REQUIRE(ext != nullptr);
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
  }
  REQUIRE(X509_sign(x, pkey, EVP_sha256()) > 0);
  BIO* b = BIO_new_file(certPath.string().c_str(), "w");
  PEM_write_bio_X509(b, x);
  BIO_free(b);
  b = BIO_new_file(keyPath.string().c_str(), "w");
  PEM_write_bio_PrivateKey(b, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  BIO_free(b);
  X509_free(x);
  EVP_PKEY_free(pkey);
}

// ----------------------------------------------------------------------------------------------- utilitários

struct Waiter {
  std::shared_ptr<std::promise<HttpOutResult>> p = std::make_shared<std::promise<HttpOutResult>>();
  std::shared_future<HttpOutResult> f = p->get_future().share();
  HttpOutPool::Done done() {
    auto pp = p;
    return [pp](HttpOutResult r) { pp->set_value(std::move(r)); };
  }
  bool ready(std::chrono::milliseconds w = std::chrono::milliseconds(0)) const {
    return f.wait_for(w) == std::future_status::ready;
  }
  HttpOutResult get() const {
    return guarded("http result", [f = f] { return f.get(); });
  }
};

HttpOutRequest get(const std::string& url, std::vector<std::string> allow, uint64_t owner = 1) {
  HttpOutRequest r;
  r.url = url;
  r.allow = std::move(allow);
  r.owner = owner;
  return r;
}

HttpOutResult run(HttpOutPool& pool, HttpOutRequest r) {
  Waiter w;
  std::string err;
  if (pool.submit(std::move(r), w.done(), err) == 0) return HttpOutResult::fail("submit: " + err);
  return w.get();
}

struct LogSink {
  std::mutex mu;
  std::vector<std::string> lines;
  std::function<void(bool, const std::string&)> fn() {
    return [this](bool, const std::string& m) {
      std::lock_guard<std::mutex> lk(mu);
      lines.push_back(m);
    };
  }
  bool any(const std::string& sub) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& l : lines)
      if (contains(l, sub)) return true;
    return false;
  }
};

HttpOutOptions fastOpt() {
  HttpOutOptions o;
  o.timeout = std::chrono::milliseconds(3000);
  o.shutdownWait = std::chrono::milliseconds(500);
  return o;
}

std::string headerOf(const HttpOutResult& r, const std::string& name) {
  for (auto& [k, v] : r.headers)
    if (k == name) return v;
  return {};
}

}  // namespace

// ------------------------------------------------------------------------------------ validação pura (sem rede)

TEST_CASE("http: resolved addresses must be public (IPv4, IPv6 and embedded IPv4)") {
  const char* refused[] = {
      "127.0.0.1",      "127.255.255.254", "10.1.2.3",        "172.16.0.1",       "172.31.255.255",
      "192.168.1.1",    "100.64.0.1",      "100.127.255.254", "169.254.169.254",  "0.0.0.0",
      "0.1.2.3",        "224.0.0.1",       "239.255.255.250", "255.255.255.255",  "240.0.0.1",
      "192.0.2.10",     "198.51.100.7",    "203.0.113.9",     "198.18.0.1",       "198.19.255.1",
      "192.0.0.8",      "192.88.99.1",     "::1",             "::",               "fc00::1",
      "fd12:3456::1",   "fe80::1",         "febf::1",         "fec0::1",          "ff02::1",
      "::ffff:127.0.0.1", "::ffff:10.0.0.1", "::ffff:169.254.169.254", "::127.0.0.1", "::10.0.0.1",
      "64:ff9b::7f00:1", "64:ff9b::a00:1", "64:ff9b:1::1",   "2002:7f00:1::1",   "2002:c0a8:101::1",
      "2001:0:4136:e378:8000:63bf:f5ff:fffe",  // Teredo com cliente 10.0.0.1 (ofuscado)
      "2001:0:a00:1::1",                       // Teredo com servidor 10.0.0.1
      "2001:db8::1",    "100::1",          "::ffff:0:0",      "not-an-ip",        "",
  };
  for (const char* ip : refused) {
    INFO(ip);
    REQUIRE_FALSE(httpAddressRefusal(ip).empty());
  }
  const char* allowed[] = {"8.8.8.8", "1.1.1.1", "93.184.216.34", "172.32.0.1", "100.128.0.1", "2606:4700::1111",
                           "2a00:1450:4001::200e", "::ffff:8.8.8.8", "64:ff9b::808:808", "2002:808:808::1"};
  for (const char* ip : allowed) {
    INFO(ip);
    REQUIRE(httpAddressRefusal(ip).empty());
  }
  // Modo de teste: só o loopback é liberado.
  REQUIRE(httpAddressRefusal("127.0.0.1", true).empty());
  REQUIRE(httpAddressRefusal("::1", true).empty());
  REQUIRE_FALSE(httpAddressRefusal("10.0.0.1", true).empty());
  REQUIRE_FALSE(httpAddressRefusal("169.254.169.254", true).empty());
}

TEST_CASE("http: URLs must be https with a hostname, a sane target and an allowed port") {
  HttpUrl u;
  std::string err;
  REQUIRE(parseHttpUrl("https://API.Example.com/v1/x?a=1#frag", u, err));
  REQUIRE(u.scheme == "https");
  REQUIRE(u.host == "api.example.com");
  REQUIRE(u.port == 443);
  REQUIRE_FALSE(u.explicitPort);
  REQUIRE(u.target == "/v1/x?a=1");
  REQUIRE(u.forLog() == "https://api.example.com/v1/x");
  REQUIRE(parseHttpUrl("https://api.example.com", u, err));
  REQUIRE(u.target == "/");
  REQUIRE(parseHttpUrl("https://api.example.com:8443/", u, err));
  REQUIRE((u.port == 8443 && u.explicitPort));
  REQUIRE(parseHttpUrl("https://xn--bcher-kva.example/", u, err));

  const char* bad[] = {
      "http://api.example.com/",       "ftp://api.example.com/",         "api.example.com/x",
      "https://user:pw@api.example.com/", "https://1.2.3.4/",             "https://[::1]/",
      "https://2130706433/",           "https://0x7f.1/",                "https://127.1/",
      "https://api.example.com:0/",    "https://api.example.com:70000/", "https://api.example.com:/",
      "https://bücher.example/",       "https://api.example.com/a b",    "https://api.example.com/a\r\nX: y",
      "https://api..example.com/",     "https://.example.com/",          "https://api.example.com./",
      "https:///x",                    "https://*.example.com/",         "https://api.example.com\\x/",
      "https://-api.example.com/",     "https://api_x.example.com/",
  };
  for (const char* b : bad) {
    INFO(b);
    REQUIRE_FALSE(parseHttpUrl(b, u, err));
    REQUIRE_FALSE(err.empty());
  }
  REQUIRE_FALSE(parseHttpUrl("https://a.example.com/" + std::string(2100, 'a'), u, err));
  REQUIRE(contains(err, "2048"));
  // Modo de teste aceita http.
  REQUIRE(parseHttpUrl("http://localhost:8080/x", u, err, true));
  REQUIRE((u.scheme == "http" && u.port == 8080));
  REQUIRE(parseHttpUrl("http://localhost/x", u, err, true));
  REQUIRE(u.port == 80);
}

TEST_CASE("http: allowlist entries are exact lowercase hostnames with an optional port") {
  std::string n, err;
  REQUIRE(parseHttpAllowEntry("api.example.com", n, err));
  REQUIRE(n == "api.example.com");
  REQUIRE(parseHttpAllowEntry("api.example.com:8443", n, err));
  REQUIRE(n == "api.example.com:8443");
  REQUIRE(parseHttpAllowEntry("xn--bcher-kva.example", n, err));
  for (const char* b : {"", "*.example.com", "https://api.example.com", "api.example.com/x", "1.2.3.4", "[::1]",
                        "::1", "Api.Example.com", "bücher.example", "api..example.com", "api.example.com:",
                        "api.example.com:0", "api.example.com:65536", "a b.com", ".example.com", "example.com.",
                        "user@example.com", "2130706433"}) {
    INFO(b);
    REQUIRE_FALSE(parseHttpAllowEntry(b, n, err));
  }

  HttpUrl u;
  REQUIRE(parseHttpUrl("https://api.example.com/x", u, err));
  REQUIRE(httpAllowed({"api.example.com"}, u));
  REQUIRE_FALSE(httpAllowed({"example.com"}, u));        // nada de subdomínio implícito
  REQUIRE_FALSE(httpAllowed({"x.api.example.com"}, u));
  REQUIRE_FALSE(httpAllowed({"api.example.com:8443"}, u));  // entrada com porta vale só para ela
  REQUIRE(parseHttpUrl("https://api.example.com:443/x", u, err));
  REQUIRE(httpAllowed({"api.example.com"}, u));
  REQUIRE(parseHttpUrl("https://api.example.com:8443/x", u, err));
  REQUIRE_FALSE(httpAllowed({"api.example.com"}, u));
  REQUIRE(httpAllowed({"api.example.com:8443"}, u));
}

TEST_CASE("http: redirects resolve relative locations and never downgrade to http") {
  HttpUrl base, out;
  std::string err;
  REQUIRE(parseHttpUrl("https://api.example.com/a/b?q=1", base, err));
  REQUIRE(resolveHttpRedirect(base, "/c", out, err));
  REQUIRE((out.host == "api.example.com" && out.target == "/c"));
  REQUIRE(resolveHttpRedirect(base, "d?x=2", out, err));
  REQUIRE(out.target == "/a/d?x=2");
  REQUIRE(resolveHttpRedirect(base, "//other.example.com/z", out, err));
  REQUIRE((out.host == "other.example.com" && out.scheme == "https"));
  REQUIRE(resolveHttpRedirect(base, "https://other.example.com:8443/", out, err));
  REQUIRE(out.port == 8443);
  REQUIRE_FALSE(resolveHttpRedirect(base, "http://api.example.com/", out, err));
  REQUIRE(contains(err, "https"));
  REQUIRE_FALSE(resolveHttpRedirect(base, "http://api.example.com/", out, err, true));  // nem no modo de teste
  REQUIRE_FALSE(resolveHttpRedirect(base, "https://10.0.0.1/", out, err));
  REQUIRE_FALSE(resolveHttpRedirect(base, "", out, err));
  REQUIRE_FALSE(resolveHttpRedirect(base, "javascript:alert(1)", out, err));
}

TEST_CASE("http: the log form of a URL drops the query string") {
  REQUIRE(httpUrlForLog("https://api.example.com/p/x?token=segredo#f") == "https://api.example.com/p/x");
  REQUIRE(httpUrlForLog("https://api.example.com#f") == "https://api.example.com");
}

// --------------------------------------------------------------------------- pool contra o servidor local

TEST_CASE("http pool: requests are validated at submit") {
  HttpOutPool pool(fastOpt(), HttpOutPool::InsecureLoopbackForTests{});
  const std::vector<std::string> allow{"localhost:8080"};
  std::string err;
  auto refused = [&](HttpOutRequest r, const std::string& why) {
    INFO(why);
    err.clear();
    REQUIRE(pool.submit(std::move(r), [](HttpOutResult) {}, err) == 0);
    INFO(err);
    REQUIRE(contains(err, why));
  };
  refused(get("http://other.test:8080/", allow), "permissions.http");
  refused(get("http://localhost:8081/", allow), "permissions.http");
  refused(get("https://127.0.0.1:8080/", allow), "IP");
  for (const char* h : {"Host", "content-length", "Transfer-Encoding", "Connection", "Accept-Encoding"}) {
    auto r = get("http://localhost:8080/", allow);
    r.headers = {{h, "x"}};
    refused(r, "não pode ser definido");
  }
  {
    auto r = get("http://localhost:8080/", allow);
    r.headers = {{"X-A", "a\r\nX-Injetado: 1"}};
    refused(r, "inválido");
  }
  {
    auto r = get("http://localhost:8080/", allow);
    r.headers = {{"X A", "a"}};
    refused(r, "inválido");
  }
  {
    auto r = get("http://localhost:8080/", allow);
    r.headers = {{"X-A", std::string("a\0b", 3)}};
    refused(r, "inválido");
  }
  {
    auto r = get("http://localhost:8080/", allow);
    r.headers = {{"X-A", std::string(9000, 'a')}};
    refused(r, "8 KiB");
  }
  {
    auto r = get("http://localhost:8080/", allow);
    r.method = HttpMethod::Post;
    r.body = std::string((size_t{1} << 20) + 1, 'b');
    refused(r, "corpo");
  }
  {
    auto r = get("http://localhost:8080/", allow);
    r.method = HttpMethod::Post;
    r.contentType = "text/plain\r\nX: 1";
    refused(r, "inválido");
  }
  // Fora do modo de teste, http:// é recusado.
  HttpOutPool strict(fastOpt());
  REQUIRE(strict.submit(get("http://localhost:8080/", allow), [](HttpOutResult) {}, err) == 0);
  REQUIRE(contains(err, "https"));
  REQUIRE_FALSE(strict.testMode());
  REQUIRE(pool.testMode());
}

TEST_CASE("http pool: GET and POST reach the local server and return status, headers and body") {
  TestServer s;
  HttpOutPool pool(fastOpt(), HttpOutPool::InsecureLoopbackForTests{});
  auto r = run(pool, get(s.base() + "/hello", {s.allow()}));
  INFO(r.error);
  REQUIRE(r.ok);
  REQUIRE(r.status == 200);
  REQUIRE(r.body == "oi");
  REQUIRE(headerOf(r, "x-test") == "um");
  REQUIRE(headerOf(r, "content-type") == "text/plain");

  auto p = get(s.base() + "/echo?x=1", {s.allow()});
  p.method = HttpMethod::Post;
  p.body = "{\"a\":1}";
  p.contentType = "application/json";
  p.headers = {{"X-Token", "abc"}, {"Authorization", "Bearer t"}};
  auto r2 = run(pool, p);
  INFO(r2.error);
  REQUIRE(r2.ok);
  REQUIRE(r2.status == 201);
  REQUIRE(r2.body == "POST:{\"a\":1}");
  {
    std::lock_guard<std::mutex> lk(s.mu);
    REQUIRE(s.lastContentType == "application/json");
    REQUIRE(s.lastHost == s.allow());  // Host = nome original (a conexão vai para o IP conferido)
    bool token = false, ae = false;
    for (auto& h : s.seenHeaders) {
      token = token || h == "X-Token: abc";
      ae = ae || h == "Accept-Encoding: identity";
    }
    REQUIRE(token);
    REQUIRE(ae);
  }
  auto r3 = run(pool, get(s.base() + "/status404", {s.allow()}));
  REQUIRE(r3.ok);
  REQUIRE(r3.status == 404);
  REQUIRE(r3.body == "nao");
  auto m = pool.metrics();
  REQUIRE(m.requests >= 3);
  REQUIRE(m.threads == 4);
}

TEST_CASE("http pool: a domain outside the list never reaches the network") {
  TestServer s;
  HttpOutPool pool(fastOpt(), HttpOutPool::InsecureLoopbackForTests{});
  std::string err;
  REQUIRE(pool.submit(get(s.base() + "/hello", {"localhost:1"}), [](HttpOutResult) {}, err) == 0);
  REQUIRE(contains(err, "permissions.http"));
  REQUIRE(s.totalHits() == 0);
}

TEST_CASE("http pool: responses over 1 MiB are cut with an error") {
  TestServer s;
  HttpOutPool pool(fastOpt(), HttpOutPool::InsecureLoopbackForTests{});
  for (const char* path : {"/big", "/bigchunked"}) {
    INFO(path);
    auto r = run(pool, get(s.base() + path, {s.allow()}));
    REQUIRE_FALSE(r.ok);
    REQUIRE(contains(r.error, "grande demais"));
    REQUIRE(r.body.empty());
  }
  auto ok = run(pool, get(s.base() + "/exact", {s.allow()}));  // exatamente 1 MiB passa
  INFO(ok.error);
  REQUIRE(ok.ok);
  REQUIRE(ok.body.size() == (size_t{1} << 20));
  auto h = run(pool, get(s.base() + "/bighdr", {s.allow()}));
  REQUIRE_FALSE(h.ok);
  REQUIRE(contains(h.error, "cabeçalhos"));
}

TEST_CASE("http pool: a slow server hits the total timeout") {
  TestServer s;
  auto o = fastOpt();
  o.timeout = std::chrono::milliseconds(400);
  HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
  const auto t0 = std::chrono::steady_clock::now();
  auto r = run(pool, get(s.base() + "/slow", {s.allow()}));
  const auto dt = std::chrono::steady_clock::now() - t0;
  REQUIRE_FALSE(r.ok);
  REQUIRE(r.timedOut);
  REQUIRE(contains(r.error, "tempo esgotado"));
  REQUIRE(dt < std::chrono::milliseconds(3000));
  REQUIRE(pool.metrics().timeouts >= 1);
}

TEST_CASE("http pool: redirects are followed inside the list and refused outside it") {
  TestServer s;
  LogSink logs;
  auto o = fastOpt();
  o.log = logs.fn();
  HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
  auto in = run(pool, get(s.base() + "/redir-in", {s.allow()}));
  INFO(in.error);
  REQUIRE(in.ok);
  REQUIRE(in.body == "oi");

  auto out = run(pool, get(s.base() + "/redir-out", {s.allow()}));
  REQUIRE_FALSE(out.ok);
  REQUIRE(contains(out.error, "redirecion"));
  REQUIRE(contains(out.error, "permissions.http"));
  REQUIRE_FALSE(contains(out.error, "segredo"));

  auto port = run(pool, get(s.base() + "/redir-port", {s.allow()}));
  REQUIRE_FALSE(port.ok);
  REQUIRE(contains(port.error, "permissions.http"));

  auto ip = run(pool, get(s.base() + "/redir-ip", {s.allow()}));
  REQUIRE_FALSE(ip.ok);
  REQUIRE(contains(ip.error, "IP"));

  auto three = run(pool, get(s.base() + "/hop/3", {s.allow()}));
  INFO(three.error);
  REQUIRE(three.ok);
  REQUIRE(three.body == "fim");
  auto four = run(pool, get(s.base() + "/hop/4", {s.allow()}));
  REQUIRE_FALSE(four.ok);
  REQUIRE(contains(four.error, "redirecionamentos demais"));
  REQUIRE(s.hitCount("/hello") == 1);  // só o redirecionamento de dentro chegou ao destino
  REQUIRE_FALSE(logs.any("segredo"));
}

TEST_CASE("http pool: loopback and private addresses are refused outside test mode") {
  TestServer s;
  HttpOutPool pool(fastOpt());
  auto r = run(pool, get("https://localhost:" + std::to_string(s.port) + "/hello", {s.allow()}));
  REQUIRE_FALSE(r.ok);
  INFO(r.error);
  REQUIRE(contains(r.error, "loopback"));
  REQUIRE(s.totalHits() == 0);
  REQUIRE(pool.metrics().refused >= 1);
}

TEST_CASE("http pool: TLS verifies the certificate and the hostname") {
  TmpDir t;
  makeCert(t.p / "good.pem", t.p / "good.key", "localhost");
  makeCert(t.p / "other.pem", t.p / "other.key", "other.test");
  SECTION("certificado confiável para o nome") {
    TestServer s(std::make_unique<httplib::SSLServer>((t.p / "good.pem").string().c_str(),
                                                      (t.p / "good.key").string().c_str()));
    auto o = fastOpt();
    o.caFile = (t.p / "good.pem").string();
    HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
    auto r = run(pool, get("https://localhost:" + std::to_string(s.port) + "/hello", {s.allow()}));
    INFO(r.error);
    REQUIRE(r.ok);
    REQUIRE(r.body == "oi");
  }
  SECTION("certificado não confiável") {
    TestServer s(std::make_unique<httplib::SSLServer>((t.p / "good.pem").string().c_str(),
                                                      (t.p / "good.key").string().c_str()));
    auto o = fastOpt();
    o.caFile = (t.p / "other.pem").string();
    HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
    auto r = run(pool, get("https://localhost:" + std::to_string(s.port) + "/hello", {s.allow()}));
    REQUIRE_FALSE(r.ok);
    INFO(r.error);
    REQUIRE(contains(r.error, "certificado"));
    REQUIRE(s.hitCount("/hello") == 0);
  }
  SECTION("certificado de outro nome") {
    TestServer s(std::make_unique<httplib::SSLServer>((t.p / "other.pem").string().c_str(),
                                                      (t.p / "other.key").string().c_str()));
    auto o = fastOpt();
    o.caFile = (t.p / "other.pem").string();
    HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
    auto r = run(pool, get("https://localhost:" + std::to_string(s.port) + "/hello", {s.allow()}));
    REQUIRE_FALSE(r.ok);
    INFO(r.error);
    REQUIRE(contains(r.error, "certificado"));
    REQUIRE(s.hitCount("/hello") == 0);
  }
}

TEST_CASE("http pool: owners take turns and have their own caps") {
  TestServer s;
  auto o = fastOpt();
  o.threads = 1;
  o.timeout = std::chrono::milliseconds(8000);
  o.maxPendingPerOwner = 4;
  HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
  std::vector<Waiter> ws(5);
  std::string err;
  REQUIRE(pool.submit(get(s.base() + "/order/a1", {s.allow()}, 1), ws[0].done(), err) != 0);
  REQUIRE(waitUntil([&] { return s.orderNow().size() == 1; }));
  for (int i = 2; i <= 4; ++i)
    REQUIRE(pool.submit(get(s.base() + "/order/a" + std::to_string(i), {s.allow()}, 1), ws[static_cast<size_t>(i - 1)].done(), err) != 0);
  // Dono 1 está no teto (4 no pool).
  REQUIRE(pool.submit(get(s.base() + "/order/a5", {s.allow()}, 1), [](HttpOutResult) {}, err) == 0);
  REQUIRE(contains(err, "pendentes"));
  REQUIRE(pool.submit(get(s.base() + "/order/b1", {s.allow()}, 2), ws[4].done(), err) != 0);
  s.release();
  for (auto& w : ws) REQUIRE(w.get().ok);
  const auto ord = s.orderNow();
  REQUIRE(ord.size() == 5);
  REQUIRE(ord[0] == "a1");
  REQUIRE(ord[1] == "b1");  // a vez do outro dono vem antes do resto da fila do primeiro
}

TEST_CASE("http pool: bytes in flight per owner are capped") {
  TestServer s;
  auto o = fastOpt();
  o.maxBytesPerOwner = 512 * 1024;
  HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
  auto r = run(pool, get(s.base() + "/exact", {s.allow()}));
  REQUIRE_FALSE(r.ok);
  REQUIRE(contains(r.error, "em andamento"));
  auto p = get(s.base() + "/echo", {s.allow()});
  p.method = HttpMethod::Post;
  p.body = std::string(600 * 1024, 'b');
  std::string err;
  REQUIRE(pool.submit(p, [](HttpOutResult) {}, err) == 0);
  REQUIRE(contains(err, "em andamento"));
  // Outro dono não é afetado; o orçamento volta depois da resposta.
  auto ok = run(pool, get(s.base() + "/hello", {s.allow()}, 2));
  REQUIRE(ok.ok);
  auto again = run(pool, get(s.base() + "/hello", {s.allow()}, 1));
  REQUIRE(again.ok);
}

TEST_CASE("http pool: cancel drops a queued or running request without calling done") {
  TestServer s;
  auto o = fastOpt();
  o.threads = 1;
  HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
  std::atomic<int> calls{0};
  std::string err;
  const uint64_t running = pool.submit(get(s.base() + "/slow", {s.allow()}), [&](HttpOutResult) { ++calls; }, err);
  REQUIRE(running != 0);
  REQUIRE(waitUntil([&] { return s.hitCount("/slow") == 1; }));
  const uint64_t queued = pool.submit(get(s.base() + "/hello", {s.allow()}), [&](HttpOutResult) { ++calls; }, err);
  REQUIRE(queued != 0);
  pool.cancel(queued);
  pool.cancel(running);  // fecha o socket: a thread fica livre sem esperar o servidor
  auto next = run(pool, get(s.base() + "/hello", {s.allow()}));
  INFO(next.error);
  REQUIRE(next.ok);
  REQUIRE(calls.load() == 0);
  REQUIRE(s.hitCount("/hello") == 1);
}

TEST_CASE("http pool: destroying the pool with a black-holed request does not hang") {
  TestServer s;
  auto o = fastOpt();
  o.timeout = std::chrono::milliseconds(10000);
  o.shutdownWait = std::chrono::milliseconds(500);
  auto pool = std::make_unique<HttpOutPool>(o, HttpOutPool::InsecureLoopbackForTests{});
  Waiter w;
  std::string err;
  REQUIRE(pool->submit(get(s.base() + "/slow", {s.allow()}), w.done(), err) != 0);
  REQUIRE(waitUntil([&] { return s.hitCount("/slow") == 1; }));
  const auto t0 = std::chrono::steady_clock::now();
  pool.reset();
  REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000));
  if (w.ready(std::chrono::milliseconds(100))) REQUIRE_FALSE(w.get().ok);
}

TEST_CASE("http pool: errors and logs never carry the query, headers or body") {
  TestServer s;
  LogSink logs;
  auto o = fastOpt();
  o.timeout = std::chrono::milliseconds(300);
  o.log = logs.fn();
  HttpOutPool pool(o, HttpOutPool::InsecureLoopbackForTests{});
  auto r = get(s.base() + "/slow?token=segredo1", {s.allow()});
  r.headers = {{"Authorization", "Bearer segredo2"}};
  auto res = run(pool, r);
  REQUIRE_FALSE(res.ok);
  REQUIRE(contains(res.error, "/slow"));
  for (const char* sec : {"segredo1", "segredo2"}) {
    INFO(sec);
    REQUIRE_FALSE(contains(res.error, sec));
    REQUIRE_FALSE(logs.any(sec));
  }
}

// ------------------------------------------------------------------------------------------------ cb.http (API)

namespace {

struct HttpEnv : ApiEnv {
  std::unique_ptr<TestServer> srv = std::make_unique<TestServer>();
  std::shared_ptr<HttpOutPool> pool;
  explicit HttpEnv(HttpOutOptions o = fastOpt()) {
    pool = std::make_shared<HttpOutPool>(o, HttpOutPool::InsecureLoopbackForTests{});
    api.http = pool;
  }
  ~HttpEnv() {
    srv->release();
    if (host)
      guarded("destroy", [this] {
        host.reset();
        return 0;
      });
  }
  nlohmann::json perm() const { return {{"http", {srv->allow()}}}; }
  std::string lua(const std::string& code) const {
    std::string out = "local BASE = '" + srv->base() + "'\n" + code;
    return out;
  }
};

}  // namespace

TEST_CASE("api: cb.http.get/post resume the callback with status, headers and body") {
  HttpEnv env;
  env.addRes("a", env.lua(R"(
    cb.on('resourceStart', function()
      local r = cb.http.get(BASE .. '/hello', { headers = { ['X-Token'] = 'abc' } })
      cb.log.info('get ' .. r.status .. ' ' .. r.body .. ' ' .. r.headers['x-test'])
      local p = cb.http.post(BASE .. '/echo', 'corpo', { headers = { ['Content-Type'] = 'text/plain' } })
      cb.log.info('post ' .. p.status .. ' ' .. p.body)
      local j = cb.http.post(BASE .. '/echo', { a = 1 })
      cb.log.info('json ' .. j.body)
      local n = cb.http.get(BASE .. '/status404')
      cb.log.info('404 ' .. n.status .. ' ' .. n.body)
    end)
  )"), {}, nlohmann::json::object(), env.perm());
  env.boot();
  REQUIRE(env.waitLog("[a] 404 404 nao"));
  INFO(env.allLogs());
  REQUIRE(env.hasLog("[a] get 200 oi um"));
  REQUIRE(env.hasLog("[a] post 201 POST:corpo"));
  REQUIRE(env.hasLog("[a] json POST:{\"a\":1}"));
  std::lock_guard<std::mutex> lk(env.srv->mu);
  REQUIRE(env.srv->lastContentType == "application/json");
}

TEST_CASE("api: cb.http needs permissions.http, a configured pool and an allowed host") {
  SECTION("sem permissão") {
    HttpEnv env;
    env.addRes("a", env.lua(R"(
      cb.on('resourceStart', function()
        local ok, e = pcall(cb.http.get, BASE .. '/hello')
        cb.log.info('perm ' .. tostring(ok) .. ' ' .. e)
      end)
    )"));
    env.boot();
    REQUIRE(env.waitLog("perm false"));
    REQUIRE(env.hasLog("permissions.http"));
    REQUIRE(env.srv->totalHits() == 0);
  }
  SECTION("sem pool") {
    HttpEnv env;
    env.api.http = nullptr;
    env.addRes("a", env.lua(R"(
      cb.on('resourceStart', function()
        local ok, e = pcall(cb.http.get, BASE .. '/hello')
        cb.log.info('nohttp ' .. tostring(ok) .. ' ' .. e)
      end)
    )"), {}, nlohmann::json::object(), env.perm());
    env.boot();
    REQUIRE(env.waitLog("nohttp false"));
    REQUIRE(env.hasLog("não configurado"));
  }
  SECTION("host fora da lista e argumentos inválidos") {
    HttpEnv env;
    env.addRes("a", env.lua(R"(
      cb.on('resourceStart', function()
        local function try(...) local ok, e = pcall(...) cb.log.info('err ' .. tostring(ok) .. ' ' .. tostring(e)) end
        try(cb.http.get, 'http://localhost:1/x')
        try(cb.http.get, 42)
        try(cb.http.get, BASE .. '/hello', { headers = { Host = 'x' } })
        try(cb.http.get, BASE .. '/hello', { headers = { ['X-A'] = 1 } })
        try(cb.http.post, BASE .. '/echo', function() end)
        cb.log.info('fim')
      end)
      local ok, e = pcall(cb.http.get, BASE .. '/hello')
      cb.log.info('top ' .. tostring(ok) .. ' ' .. tostring(e))
    )"), {}, nlohmann::json::object(), env.perm());
    env.boot();
    REQUIRE(env.waitLog("[a] fim"));
    INFO(env.allLogs());
    REQUIRE(env.hasLog("permissions.http"));
    REQUIRE(env.hasLog("URL deve ser texto"));
    REQUIRE(env.hasLog("não pode ser definido"));
    REQUIRE(env.hasLog("cabeçalho"));
    REQUIRE(env.hasLog("corpo"));
    REQUIRE(env.hasLog("[a] top false"));
    REQUIRE(env.countLog("err true") == 0);
    REQUIRE(env.srv->totalHits() == 0);
  }
}

TEST_CASE("api: cb.http errors (timeout, too big) raise in the callback") {
  auto o = fastOpt();
  o.timeout = std::chrono::milliseconds(300);
  HttpEnv env(o);
  env.addRes("a", env.lua(R"(
    cb.on('resourceStart', function()
      local ok, e = pcall(cb.http.get, BASE .. '/slow?token=segredo')
      cb.log.info('slow ' .. tostring(ok) .. ' ' .. tostring(e))
      ok, e = pcall(cb.http.get, BASE .. '/big')
      cb.log.info('big ' .. tostring(ok) .. ' ' .. tostring(e))
    end)
  )"), {}, nlohmann::json::object(), env.perm());
  env.boot();
  REQUIRE(env.waitLog("[a] big false"));
  INFO(env.allLogs());
  REQUIRE(env.hasLog("[a] slow false"));
  REQUIRE(env.hasLog("tempo esgotado"));
  REQUIRE(env.hasLog("grande demais"));
  REQUIRE(env.countLog("segredo") == 0);
}

TEST_CASE("api: the script-side deadline answers even when the pool does not") {
  auto o = fastOpt();
  o.timeout = std::chrono::milliseconds(20000);
  o.queueTimeout = std::chrono::milliseconds(1000);
  HttpEnv env(o);
  env.api.httpGraceSec = 1.0;
  env.addRes("a", env.lua(R"(
    cb.on('resourceStart', function()
      local ok, e = pcall(cb.http.get, BASE .. '/slow')
      cb.log.info('slow ' .. tostring(ok) .. ' ' .. tostring(e))
    end)
  )"), {}, nlohmann::json::object(), env.perm());
  env.boot();
  REQUIRE(waitUntil([&] { return env.srv->hitCount("/slow") == 1; }));
  env.sync();
  REQUIRE_FALSE(env.hasLog("slow"));
  env.clock->advance(30.0);  // > 20 s + 1 s de fila + 1 s de folga
  REQUIRE(env.waitLog("[a] slow false"));
  REQUIRE(env.hasLog("tempo esgotado"));
  env.srv->release();
  env.sync();
  env.sync();
  REQUIRE(env.countLog("slow ") == 1);
}

TEST_CASE("api: stopping a resource discards its HTTP results") {
  HttpEnv env;
  env.addRes("a", env.lua(R"(
    cb.on('resourceStart', function()
      cb.timers.after(0, function()
        cb.http.get(BASE .. '/slow')
        cb.log.info('depois')
      end)
    end)
  )"), {}, nlohmann::json::object(), env.perm());
  env.boot();
  REQUIRE(waitUntil([&] { return env.srv->hitCount("/slow") == 1; }));
  REQUIRE(env.call(env.host->stopResource("a")));
  env.srv->release();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  env.sync();
  env.sync();
  REQUIRE_FALSE(env.hasLog("depois"));
  // Religado: a nova geração funciona normalmente.
  REQUIRE(env.call(env.host->startResource("a")));
  REQUIRE(env.waitLog("[a] depois"));
  REQUIRE(env.countLog("depois") == 1);
}

TEST_CASE("api: a command handler can reply after awaiting HTTP, and pending requests are capped") {
  HttpEnv env;
  env.api.maxHttpPending = 2;
  env.addRes("a", env.lua(R"(
    cb.commands.register('busca', function()
      return 'got=' .. cb.http.get(BASE .. '/hello').body
    end)
    cb.on('resourceStart', function()
      for i = 1, 4 do
        cb.timers.after(0, function()
          local ok, e = pcall(cb.http.get, BASE .. '/slow')
          if not ok then cb.log.info('pend ' .. tostring(e)) else cb.log.info('slowok') end
        end)
      end
    end)
  )"), {}, nlohmann::json::object(), env.perm());
  env.boot();
  REQUIRE(env.waitLog("pendentes"));
  REQUIRE(waitUntil([&] { return env.countLog("pend ") == 2; }));
  env.srv->release();
  REQUIRE(waitUntil([&] { return env.countLog("slowok") == 2; }));
  env.command(std::nullopt, "busca");
  REQUIRE(waitUntil([&] { return env.hasReply("got=oi"); }));
}

TEST_CASE("api: resuming a coroutine that waits for HTTP from outside raises an error in it") {
  HttpEnv env;
  env.addRes("a", env.lua(R"(
    local waiting
    cb.on('resourceStart', function()
      waiting = coroutine.running()
      local ok, e = pcall(cb.http.get, BASE .. '/slow')
      cb.log.info('voltou ' .. tostring(ok) .. ' ' .. tostring(e))
    end)
    cb.commands.register('cutuca', function()
      local ok = coroutine.resume(waiting, { status = 200 })
      return 'cutucou ' .. tostring(ok)
    end)
  )"), {}, nlohmann::json::object(), env.perm());
  env.boot();
  REQUIRE(waitUntil([&] { return env.srv->hitCount("/slow") == 1; }));
  env.command(std::nullopt, "cutuca");
  REQUIRE(waitUntil([&] { return env.hasReply("cutucou true"); }));
  REQUIRE(env.waitLog("[a] voltou false"));
  REQUIRE(env.hasLog("por fora"));
  env.srv->release();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  env.sync();
  env.sync();
  REQUIRE(env.countLog("voltou") == 1);
}
