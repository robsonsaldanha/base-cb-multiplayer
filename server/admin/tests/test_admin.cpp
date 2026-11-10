// Painel admin: login via Hub (state, ticket #admin, jti, admins), sessão, CSRF/Origin/Host, API e integração com
// World + ScriptHost + ScriptBridge reais.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "cbadmin/admin_server.hpp"
#include "cbscript/api.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/host.hpp"
#include "cbserver/auth.hpp"
#include "cbserver/script_bridge.hpp"
#include "ticket_helper.hpp"
#include "world_harness.hpp"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using nlohmann::json;

namespace {

constexpr const char* kAdmin = "123456789012345678";
constexpr const char* kOther = "223456789012345678";
constexpr const char* kServer = "srv_test";
constexpr int64_t kNow = 1800000000;

struct Tmp {
  fs::path p;
  Tmp() {
    static std::atomic<unsigned> n{0};
    std::random_device rd;
    p = fs::temp_directory_path() / ("cbadmin_" + std::to_string(rd()) + "_" + std::to_string(n++));
    fs::create_directories(p);
  }
  ~Tmp() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

void put(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

// Backend falso: registra o que o painel pediu.
struct Fake {
  std::mutex mu;
  std::vector<cbscript::ScriptToSim> sim;
  std::vector<std::string> audit;
  std::vector<cbscript::EvCommand> cmds;
  struct BanReq {
    std::string did, reason, by;
    int64_t dur;
  };
  std::vector<BanReq> bans;
  std::vector<std::pair<cbadmin::ResourceOp, std::string>> ops;
  std::shared_ptr<std::vector<cbscript::PlayerView>> players = std::make_shared<std::vector<cbscript::PlayerView>>();
  std::atomic<double> mono{1000};
  std::atomic<int64_t> unixTime{kNow};

  bool hasAudit(const std::string& a, const std::string& b = "") {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& l : audit)
      if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) return true;
    return false;
  }
  std::string allAudit() {
    std::lock_guard<std::mutex> lk(mu);
    std::string r;
    for (auto& l : audit) r += l + "\n";
    return r;
  }
};

cbadmin::AdminBackend fakeBackend(Fake& f, cbserver::KeyProvider& keys) {
  cbadmin::AdminBackend be;
  be.keys = &keys;
  be.unixNow = [&f] { return f.unixTime.load(); };
  be.monoNow = [&f] { return f.mono.load(); };
  be.players = [&f] {
    std::lock_guard<std::mutex> lk(f.mu);
    return cbscript::PlayersSnapshot(std::make_shared<const std::vector<cbscript::PlayerView>>(*f.players));
  };
  be.simCommand = [&f](cbscript::ScriptToSim c) {
    std::lock_guard<std::mutex> lk(f.mu);
    f.sim.push_back(std::move(c));
    return true;
  };
  be.resources = [] {
    cbscript::ResourceStatus a;
    a.name = "echo";
    a.state = cbscript::ResourceStatus::State::Running;
    return std::vector<cbscript::ResourceStatus>{a};
  };
  be.resourceOp = [&f](cbadmin::ResourceOp op, std::string name) {
    {
      std::lock_guard<std::mutex> lk(f.mu);
      f.ops.emplace_back(op, name);
    }
    std::promise<bool> p;
    p.set_value(name == "echo");
    return p.get_future();
  };
  be.scriptCommand = [&f](cbscript::EvCommand c) {
    auto rep = c.reply;
    {
      std::lock_guard<std::mutex> lk(f.mu);
      f.cmds.push_back(c);
    }
    if (rep) rep("oi de " + c.adminDid);
    return true;
  };
  be.ban = [&f](const std::string& did, const std::string& reason, int64_t dur, const std::string& by) {
    std::lock_guard<std::mutex> lk(f.mu);
    f.bans.push_back({did, reason, by, dur});
    return true;
  };
  be.metrics = [] { return json{{"tickP99Ms", 1.5}, {"players", 1}}; };
  be.audit = [&f](const std::string& l) {
    std::lock_guard<std::mutex> lk(f.mu);
    f.audit.push_back(l);
  };
  return be;
}

struct Resp {
  int status = 0;
  std::string body;
  httplib::Headers headers;
  json j() const { return json::parse(body, nullptr, false); }
  std::vector<std::string> cookies() const {
    std::vector<std::string> r;
    auto range = headers.equal_range("Set-Cookie");
    for (auto it = range.first; it != range.second; ++it) r.push_back(it->second);
    return r;
  }
  std::string header(const std::string& k) const {
    auto it = headers.find(k);
    return it == headers.end() ? std::string() : it->second;
  }
};

// Valor do cookie `name` num Set-Cookie (vazio se ausente).
std::string cookieValue(const Resp& r, const std::string& name) {
  for (auto& c : r.cookies())
    if (c.rfind(name + "=", 0) == 0) {
      auto end = c.find(';');
      return c.substr(name.size() + 1, end == std::string::npos ? std::string::npos : end - name.size() - 1);
    }
  return {};
}
std::string setCookieLine(const Resp& r, const std::string& name) {
  for (auto& c : r.cookies())
    if (c.rfind(name + "=", 0) == 0) return c;
  return {};
}

struct AdminEnv {
  testing_ticket::DevSigner signer;
  cbserver::StaticKeys keys{signer.keys()};
  Fake fake;
  Tmp logs;
  cbadmin::AdminOptions opt;
  std::unique_ptr<cbadmin::AdminServer> srv;
  int port = 0;
  std::string host;  // cabeçalho Host usado por padrão
  int jti = 0;

  explicit AdminEnv(std::function<void(cbadmin::AdminOptions&)> tweak = {}) {
    opt.port = 0;
    opt.bind = "127.0.0.1";
    opt.serverId = kServer;
    opt.hubUrl = "http://hub.test";
    opt.admins = {kAdmin};
    opt.logsDir = logs.p;
    opt.threads = 4;
    if (tweak) tweak(opt);
    srv = std::make_unique<cbadmin::AdminServer>(opt, fakeBackend(fake, keys));
    srv->start();
    port = srv->boundPort();
    host = "127.0.0.1:" + std::to_string(port);
  }
  ~AdminEnv() { srv->stop(); }

  std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }

  Resp send(const std::string& method, const std::string& path, const httplib::Headers& extra,
            const std::string& body = "", const std::string& ctype = "application/json") {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    httplib::Headers h = extra;
    if (h.find("Host") == h.end()) h.emplace("Host", host);
    httplib::Result r = method == "GET" ? cli.Get(path, h) : cli.Post(path, h, body, ctype);
    Resp out;
    if (!r) return out;
    out.status = r->status;
    out.body = r->body;
    out.headers = r->headers;
    return out;
  }
  Resp get(const std::string& path, const std::string& cookie = "") {
    httplib::Headers h;
    if (!cookie.empty()) h.emplace("Cookie", cookie);
    return send("GET", path, h);
  }
  // Mutação "de navegador": Origin do painel + X-CB-Admin.
  Resp post(const std::string& path, const json& body, const std::string& cookie = "") {
    httplib::Headers h{{"Origin", origin()}, {"X-CB-Admin", "1"}};
    if (!cookie.empty()) h.emplace("Cookie", cookie);
    return send("POST", path, h, body.dump());
  }

  std::string ticket(const std::string& did, const std::string& aud = std::string(kServer) + "#admin",
                     const std::string& name = "Admin") {
    return signer.signDid("sub-" + did, did, "jti-" + std::to_string(++jti), fake.unixTime.load(), aud, "dev", name);
  }
  // GET /auth/start → state do cookie.
  std::string startState() {
    auto r = get("/auth/start");
    REQUIRE(r.status == 302);
    auto s = cookieValue(r, "cbadmin_state");
    REQUIRE(!s.empty());
    return s;
  }
  Resp loginWith(const std::string& tk, const std::string& state, const std::string& cookieState) {
    httplib::Headers h{{"Origin", origin()}, {"X-CB-Admin", "1"}};
    if (!cookieState.empty()) h.emplace("Cookie", "cbadmin_state=" + cookieState);
    return send("POST", "/auth/login", h, json{{"ticket", tk}, {"state", state}}.dump());
  }
  // Login completo; devolve "cbadmin=<sessão>" para o cabeçalho Cookie.
  std::string login(const std::string& did = kAdmin) {
    auto st = startState();
    auto r = loginWith(ticket(did), st, st);
    INFO(r.body);
    REQUIRE(r.status == 200);
    auto v = cookieValue(r, "cbadmin");
    REQUIRE(!v.empty());
    return "cbadmin=" + v;
  }
};

void requireSecurityHeaders(const Resp& r) {
  INFO("status " << r.status);
  const auto csp = r.header("Content-Security-Policy");
  REQUIRE_THAT(csp, ContainsSubstring("default-src 'self'"));
  REQUIRE_THAT(csp, ContainsSubstring("frame-ancestors 'none'"));
  REQUIRE_THAT(csp, ContainsSubstring("object-src 'none'"));
  REQUIRE_THAT(csp, ContainsSubstring("base-uri 'none'"));
  REQUIRE_THAT(csp, ContainsSubstring("form-action 'self'"));
  REQUIRE(csp.find("unsafe-inline") == std::string::npos);
  REQUIRE(r.header("X-Frame-Options") == "DENY");
  REQUIRE(r.header("Referrer-Policy") == "no-referrer");
  REQUIRE(r.header("Cache-Control") == "no-store");
  REQUIRE(r.header("X-Content-Type-Options") == "nosniff");
}

cbscript::PlayerView pv(uint16_t id, const std::string& name, const std::string& did, uint32_t session) {
  cbscript::PlayerView v;
  v.id = id;
  v.name = name;
  v.did = did;
  v.sub = "sub-" + name;
  v.session = session;
  return v;
}

}  // namespace

// ---------------------------------------------------------------- login

TEST_CASE("admin: /auth/start sets a strict state cookie and redirects to the Hub with serverId and state") {
  AdminEnv env;
  auto r = env.get("/auth/start");
  REQUIRE(r.status == 302);
  const auto st = cookieValue(r, "cbadmin_state");
  REQUIRE(st.size() >= 43);
  REQUIRE(st.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos);
  REQUIRE(r.header("Location") == "http://hub.test/v1/auth/server-admin?serverId=srv_test&state=" + st);
  const auto line = setCookieLine(r, "cbadmin_state");
  REQUIRE_THAT(line, ContainsSubstring("HttpOnly"));
  REQUIRE_THAT(line, ContainsSubstring("SameSite=Strict"));
  REQUIRE_THAT(line, ContainsSubstring("Path=/auth"));
  REQUIRE_THAT(line, ContainsSubstring("Max-Age=600"));
  REQUIRE(line.find("Secure") == std::string::npos);  // publicUrl ausente/http
  REQUIRE(env.get("/auth/start").header("Location") != r.header("Location"));  // state novo a cada início
}

TEST_CASE("admin: login with a valid admin ticket creates a session") {
  AdminEnv env;
  auto st = env.startState();
  auto r = env.loginWith(env.ticket(kAdmin), st, st);
  INFO(r.body);
  REQUIRE(r.status == 200);
  REQUIRE(r.j()["did"] == kAdmin);
  const auto line = setCookieLine(r, "cbadmin");
  REQUIRE_THAT(line, ContainsSubstring("HttpOnly"));
  REQUIRE_THAT(line, ContainsSubstring("SameSite=Strict"));
  REQUIRE_THAT(line, ContainsSubstring("Path=/"));
  // state consumido: o cookie é apagado
  REQUIRE_THAT(setCookieLine(r, "cbadmin_state"), ContainsSubstring("Max-Age=0"));
  const std::string cookie = "cbadmin=" + cookieValue(r, "cbadmin");
  auto me = env.get("/api/me", cookie);
  REQUIRE(me.status == 200);
  REQUIRE(me.j()["did"] == kAdmin);
  REQUIRE(me.j()["name"] == "Admin");
  REQUIRE(env.fake.hasAudit("admin_login", "\"ok\""));
  REQUIRE(env.get("/api/me").status == 401);
  REQUIRE(env.get("/api/me", "cbadmin=AAAA").status == 401);
}

TEST_CASE("admin: a game ticket (audience without #admin) is refused") {
  AdminEnv env;
  auto st = env.startState();
  auto r = env.loginWith(env.ticket(kAdmin, kServer), st, st);
  REQUIRE(r.status == 401);
  REQUIRE(cookieValue(r, "cbadmin").empty());
  // nem o de outro servidor
  st = env.startState();
  REQUIRE(env.loginWith(env.ticket(kAdmin, "srv_outro#admin"), st, st).status == 401);
  REQUIRE(env.fake.hasAudit("admin_login", "bad_ticket"));
}

TEST_CASE("admin: a valid ticket of a did outside admins is refused") {
  AdminEnv env;
  auto st = env.startState();
  auto r = env.loginWith(env.ticket(kOther), st, st);
  REQUIRE(r.status == 403);
  REQUIRE(cookieValue(r, "cbadmin").empty());
  REQUIRE(env.fake.hasAudit("not_admin", kOther));
}

TEST_CASE("admin: a repeated jti is refused") {
  AdminEnv env;
  const auto tk = env.ticket(kAdmin);
  auto st = env.startState();
  REQUIRE(env.loginWith(tk, st, st).status == 200);
  st = env.startState();
  auto r = env.loginWith(tk, st, st);
  REQUIRE(r.status == 401);
  REQUIRE_THAT(r.body, ContainsSubstring("usado"));
}

TEST_CASE("admin: state must match the cookie and is single use") {
  AdminEnv env;
  auto st = env.startState();
  auto other = env.startState();
  REQUIRE(env.loginWith(env.ticket(kAdmin), st, "").status == 403);     // sem cookie
  REQUIRE(env.loginWith(env.ticket(kAdmin), st, other).status == 403);  // cookie de outro início
  REQUIRE(env.loginWith(env.ticket(kAdmin), "", st).status == 403);     // sem state no corpo
  const std::string forged = (st[0] == 'A' ? "B" : "A") + st.substr(1);  // nunca emitido
  REQUIRE(env.loginWith(env.ticket(kAdmin), forged, forged).status == 403);
  REQUIRE(env.loginWith(env.ticket(kAdmin), st, st).status == 200);
  REQUIRE(env.loginWith(env.ticket(kAdmin), st, st).status == 403);  // uso único
  // vencido (10 min)
  auto old = env.startState();
  env.fake.mono = env.fake.mono.load() + 601;
  REQUIRE(env.loginWith(env.ticket(kAdmin), old, old).status == 403);
}

TEST_CASE("admin: dev token login only when configured, compared in full") {
  const std::string tok(40, 'k');
  AdminEnv env([&](cbadmin::AdminOptions& o) {
    o.hubUrl.clear();
    o.admins.clear();
    o.devToken = tok;
  });
  REQUIRE(env.get("/auth/start").status == 409);  // sem Hub
  auto mode = env.get("/auth/mode");
  REQUIRE(mode.status == 200);
  REQUIRE(mode.j()["dev"] == true);
  REQUIRE(mode.j()["hub"] == false);
  REQUIRE(env.post("/auth/login", {{"devToken", tok.substr(0, 39) + "x"}}).status == 401);
  REQUIRE(env.post("/auth/login", {{"devToken", tok.substr(0, 39)}}).status == 401);
  REQUIRE(env.post("/auth/login", {{"devToken", 5}}).status == 400);
  auto r = env.post("/auth/login", {{"devToken", tok}});
  REQUIRE(r.status == 200);
  REQUIRE(env.get("/api/me", "cbadmin=" + cookieValue(r, "cbadmin")).j()["did"] == "dev");

  AdminEnv noDev;  // sem devToken configurado
  REQUIRE(noDev.post("/auth/login", {{"devToken", tok}}).status == 401);
}

TEST_CASE("admin: https publicUrl gives __Host- session cookie and Secure state cookie") {
  AdminEnv env([](cbadmin::AdminOptions& o) { o.publicUrl = "https://painel.example.com"; });
  env.host = "painel.example.com";
  auto s = env.get("/auth/start");
  REQUIRE(s.status == 302);
  REQUIRE_THAT(setCookieLine(s, "__Secure-cbadmin_state"), ContainsSubstring("Secure"));
  const auto st = cookieValue(s, "__Secure-cbadmin_state");
  REQUIRE(!st.empty());
  httplib::Headers h{{"Origin", "https://painel.example.com"}, {"X-CB-Admin", "1"},
                     {"Cookie", "__Secure-cbadmin_state=" + st}};
  auto r = env.send("POST", "/auth/login", h, json{{"ticket", env.ticket(kAdmin)}, {"state", st}}.dump());
  INFO(r.body);
  REQUIRE(r.status == 200);
  const auto line = setCookieLine(r, "__Host-cbadmin");
  REQUIRE_THAT(line, ContainsSubstring("Secure"));
  REQUIRE_THAT(line, ContainsSubstring("Path=/"));
  REQUIRE(line.find("Domain") == std::string::npos);
  // /auth/start por outro host permitido (loopback) volta para a publicUrl, onde o cookie de state vale
  env.host = "127.0.0.1:" + std::to_string(env.port);
  auto lo = env.get("/auth/start");
  REQUIRE(lo.status == 302);
  REQUIRE(lo.header("Location") == "https://painel.example.com/auth/start");
}

TEST_CASE("admin: login attempts are rate limited per IP") {
  AdminEnv env([](cbadmin::AdminOptions& o) { o.loginPerIpPerMin = 3; });
  for (int i = 0; i < 3; ++i) REQUIRE(env.post("/auth/login", {{"ticket", "x"}, {"state", "y"}}).status == 403);
  REQUIRE(env.post("/auth/login", {{"ticket", "x"}, {"state", "y"}}).status == 429);
  REQUIRE(env.get("/auth/start").status == 429);
  env.fake.mono = env.fake.mono.load() + 61;  // janela passou
  REQUIRE(env.get("/auth/start").status == 302);
}

// ---------------------------------------------------------------- sessão

TEST_CASE("admin: session expires after 1 h idle and 8 h absolute; logout invalidates it on the server") {
  AdminEnv env;
  auto c = env.login();
  env.fake.mono = env.fake.mono.load() + 3599;
  REQUIRE(env.get("/api/me", c).status == 200);
  env.fake.mono = env.fake.mono.load() + 3601;
  REQUIRE(env.get("/api/me", c).status == 401);

  auto c2 = env.login();
  for (int i = 0; i < 16; ++i) {  // ativo a cada 30 min até 8 h
    env.fake.mono = env.fake.mono.load() + 1800;
    REQUIRE(env.get("/api/me", c2).status == 200);
  }
  env.fake.mono = env.fake.mono.load() + 1;
  REQUIRE(env.get("/api/me", c2).status == 401);

  auto c3 = env.login();
  auto lo = env.post("/auth/logout", json::object(), c3);
  REQUIRE(lo.status == 200);
  REQUIRE_THAT(setCookieLine(lo, "cbadmin"), ContainsSubstring("Max-Age=0"));
  REQUIRE(env.get("/api/me", c3).status == 401);  // o cookie antigo não vale mais
  REQUIRE(env.fake.hasAudit("admin_logout", kAdmin));
}

TEST_CASE("admin: each login gets a new session id and sessions per did are capped") {
  AdminEnv env([](cbadmin::AdminOptions& o) { o.maxSessionsPerDid = 2; });
  auto a = env.login();
  auto b = env.login();
  REQUIRE(a != b);
  auto c = env.login();
  REQUIRE(env.get("/api/me", a).status == 401);  // a mais antiga saiu
  REQUIRE(env.get("/api/me", b).status == 200);
  REQUIRE(env.get("/api/me", c).status == 200);
  REQUIRE(env.srv->sessionCount() == 2);
}

// ---------------------------------------------------------------- CSRF / Host / cabeçalhos

TEST_CASE("admin: a mutation without Origin, with a foreign or null Origin, or without X-CB-Admin gets 403") {
  AdminEnv env;
  auto c = env.login();
  {
    std::lock_guard<std::mutex> lk(env.fake.mu);
    env.fake.players->push_back(pv(1, "ana", "did-ana", 7));
  }
  const std::string body = json{{"reason", "x"}}.dump();
  auto noOrigin = env.send("POST", "/api/players/1/kick", {{"Cookie", c}, {"X-CB-Admin", "1"}}, body);
  REQUIRE(noOrigin.status == 403);
  REQUIRE(env.send("POST", "/api/players/1/kick", {{"Cookie", c}, {"X-CB-Admin", "1"}, {"Origin", "null"}}, body)
              .status == 403);
  REQUIRE(env.send("POST", "/api/players/1/kick",
                   {{"Cookie", c}, {"X-CB-Admin", "1"}, {"Origin", "http://evil.example"}}, body)
              .status == 403);
  REQUIRE(env.send("POST", "/api/players/1/kick", {{"Cookie", c}, {"Origin", env.origin()}}, body).status == 403);
  REQUIRE(env.send("POST", "/api/players/1/kick",
                   {{"Cookie", c}, {"X-CB-Admin", "1"}, {"Origin", env.origin()}}, body, "text/plain")
              .status == 415);
  REQUIRE(env.send("POST", "/api/players/1/kick",
                   {{"Cookie", c}, {"X-CB-Admin", "1"}, {"Origin", env.origin()}}, std::string(70 * 1024, ' '))
              .status == 413);
  REQUIRE(env.send("POST", "/auth/login", {{"X-CB-Admin", "1"}}, "{}").status == 403);
  REQUIRE(env.send("POST", "/auth/logout", {{"Cookie", c}, {"X-CB-Admin", "1"}}, "{}").status == 403);
  {
    std::lock_guard<std::mutex> lk(env.fake.mu);
    REQUIRE(env.fake.sim.empty());
  }
  REQUIRE(env.get("/api/me", c).status == 200);  // a sessão continua
}

TEST_CASE("admin: a Host outside the allowlist gets 421 on every route (DNS rebinding)") {
  AdminEnv env;
  auto c = env.login();
  for (const char* h : {"evil.example", "evil.example:80", "127.0.0.1", "127.0.0.2:1", ""}) {
    INFO(h);
    REQUIRE(env.send("GET", "/", {{"Host", h}}).status == 421);
    REQUIRE(env.send("GET", "/app.js", {{"Host", h}}).status == 421);
    REQUIRE(env.send("GET", "/api/players", {{"Host", h}, {"Cookie", c}}).status == 421);
    REQUIRE(env.send("GET", "/auth/start", {{"Host", h}}).status == 421);
    REQUIRE(env.send("POST", "/auth/login", {{"Host", h}, {"Origin", env.origin()}, {"X-CB-Admin", "1"}}, "{}")
                .status == 421);
  }
  const std::string p = std::to_string(env.port);
  REQUIRE(env.send("GET", "/", {{"Host", "localhost:" + p}}).status == 200);
  REQUIRE(env.send("GET", "/", {{"Host", "[::1]:" + p}}).status == 200);
  REQUIRE(env.send("GET", "/", {{"Host", "LOCALHOST:" + p}}).status == 200);
  // X-Forwarded-Host não muda nada
  REQUIRE(env.send("GET", "/", {{"Host", "evil.example"}, {"X-Forwarded-Host", "127.0.0.1:" + p}}).status == 421);

  // Bind não-loopback: só o host da publicUrl.
  AdminEnv pub([](cbadmin::AdminOptions& o) {
    o.publicUrl = "https://painel.example.com:8443";
    o.bind = "0.0.0.0";
  });
  REQUIRE(pub.send("GET", "/", {{"Host", "painel.example.com:8443"}}).status == 200);
  REQUIRE(pub.send("GET", "/", {{"Host", "painel.example.com"}}).status == 421);
}

TEST_CASE("admin: security headers on every response") {
  AdminEnv env;
  auto c = env.login();
  requireSecurityHeaders(env.get("/"));
  requireSecurityHeaders(env.get("/app.js"));
  requireSecurityHeaders(env.get("/app.css"));
  requireSecurityHeaders(env.get("/auth/callback"));
  requireSecurityHeaders(env.get("/auth/start"));
  requireSecurityHeaders(env.get("/api/players"));          // 401
  requireSecurityHeaders(env.get("/nada"));                 // 404
  requireSecurityHeaders(env.get("/api/players", c));       // 200
  requireSecurityHeaders(env.post("/auth/login", json::object()));  // 400/403
  requireSecurityHeaders(env.send("GET", "/", {{"Host", "evil.example"}}));  // 421
}

TEST_CASE("admin: the embedded app has no inline script or style and never uses innerHTML") {
  AdminEnv env;
  auto idx = env.get("/");
  REQUIRE(idx.status == 200);
  REQUIRE_THAT(idx.header("Content-Type"), StartsWith("text/html"));
  REQUIRE_THAT(idx.body, ContainsSubstring("<script src=\"/app.js\""));
  REQUIRE_THAT(idx.body, ContainsSubstring("href=\"/app.css\""));
  REQUIRE(idx.body.find("<script>") == std::string::npos);
  REQUIRE(idx.body.find(" style=") == std::string::npos);
  REQUIRE_FALSE(std::regex_search(idx.body, std::regex(R"(\son[a-z]+\s*=)", std::regex::icase)));  // onclick=…
  REQUIRE(env.get("/auth/callback").body == idx.body);
  auto js = env.get("/app.js");
  REQUIRE(js.status == 200);
  REQUIRE_THAT(js.header("Content-Type"), StartsWith("text/javascript"));
  REQUIRE(js.body.find("innerHTML") == std::string::npos);
  REQUIRE(js.body.find("outerHTML") == std::string::npos);
  REQUIRE(js.body.find("insertAdjacentHTML") == std::string::npos);
  REQUIRE(js.body.find("eval(") == std::string::npos);
  REQUIRE_THAT(js.body, ContainsSubstring("X-CB-Admin"));
  REQUIRE_THAT(env.get("/app.css").header("Content-Type"), StartsWith("text/css"));
}

// ---------------------------------------------------------------- API (backend falso)

TEST_CASE("admin: players, kick, ban and teleport go to the queues and are audited") {
  AdminEnv env;
  auto c = env.login();
  {
    std::lock_guard<std::mutex> lk(env.fake.mu);
    env.fake.players->push_back(pv(1, "<img src=x onerror=alert(1)>", "did-ana", 7));
    env.fake.players->push_back(pv(2, "bia", "did-bia", 8));
  }
  auto list = env.get("/api/players", c);
  REQUIRE(list.status == 200);
  REQUIRE(list.j().size() == 2);
  REQUIRE(list.j()[0]["name"] == "<img src=x onerror=alert(1)>");  // JSON cru; a UI usa textContent
  REQUIRE(list.j()[1]["did"] == "did-bia");

  REQUIRE(env.post("/api/players/1/kick", {{"reason", "tchau"}}, c).status == 202);
  REQUIRE(env.post("/api/players/9/kick", {{"reason", "tchau"}}, c).status == 404);
  REQUIRE(env.post("/api/players/2/ban", {{"reason", "cheat"}, {"durationS", 3600}}, c).status == 202);
  REQUIRE(env.post("/api/players/2/ban", {{"reason", "cheat"}, {"durationS", -1}}, c).status == 400);
  REQUIRE(env.post("/api/players/1/teleport", {{"x", 100}, {"y", -5}, {"z", 7}}, c).status == 202);
  REQUIRE(env.post("/api/players/1/teleport", {{"x", 1.5}, {"y", 0}, {"z", 0}}, c).status == 400);
  REQUIRE(env.post("/api/players/1/teleport", {{"x", 5000000000LL}, {"y", 0}, {"z", 0}}, c).status == 400);
  REQUIRE(env.post("/api/players/1/kick", {{"reason", std::string(300, 'a')}}, c).status == 400);

  std::lock_guard<std::mutex> lk(env.fake.mu);
  REQUIRE(env.fake.sim.size() == 2);
  const auto* k = std::get_if<cbscript::CmdKick>(&env.fake.sim[0]);
  REQUIRE(k);
  REQUIRE(k->id == 1);
  REQUIRE(k->session == 7);
  REQUIRE(k->reason == "tchau");
  const auto* t = std::get_if<cbscript::CmdSetPosition>(&env.fake.sim[1]);
  REQUIRE(t);
  REQUIRE((t->id == 1 && t->x == 100 && t->y == -5 && t->z == 7 && t->session == 7));
  REQUIRE(env.fake.bans.size() == 1);
  REQUIRE(env.fake.bans[0].did == "did-bia");
  REQUIRE(env.fake.bans[0].dur == 3600);
  REQUIRE(env.fake.bans[0].by == std::string("admin:") + kAdmin);
  bool kickAudit = false, banAudit = false, tpAudit = false;
  for (auto& l : env.fake.audit) {
    auto j = json::parse(l);
    REQUIRE(j.contains("ts"));
    if (j.value("action", "") == "kick" && j["did"] == kAdmin) kickAudit = true;
    if (j.value("action", "") == "ban" && j["did"] == kAdmin) banAudit = true;
    if (j.value("action", "") == "teleport" && j["did"] == kAdmin) tpAudit = true;
  }
  REQUIRE(kickAudit);
  REQUIRE(banAudit);
  REQUIRE(tpAudit);
}

TEST_CASE("admin: resources, metrics and resource actions") {
  AdminEnv env;
  auto c = env.login();
  auto rs = env.get("/api/resources", c);
  REQUIRE(rs.status == 200);
  REQUIRE(rs.j()[0]["name"] == "echo");
  REQUIRE(rs.j()[0]["state"] == "running");
  REQUIRE(env.post("/api/resources/echo/restart", json::object(), c).j()["ok"] == true);
  REQUIRE(env.post("/api/resources/nada/stop", json::object(), c).j()["ok"] == false);
  REQUIRE(env.post("/api/resources/echo/explode", json::object(), c).status == 404);
  REQUIRE(env.post("/api/resources/..%2Fx/start", json::object(), c).status >= 400);
  REQUIRE(env.get("/api/metrics", c).j()["tickP99Ms"] == 1.5);
  REQUIRE(env.fake.hasAudit("\"resource\"", "restart"));
}

TEST_CASE("admin: console runs internal and script commands as the admin, bounded and rate limited") {
  AdminEnv env;
  auto c = env.login();
  {
    std::lock_guard<std::mutex> lk(env.fake.mu);
    env.fake.players->push_back(pv(1, "ana", "did-ana", 7));
  }
  auto pl = env.post("/api/console", {{"line", "players"}}, c);
  REQUIRE(pl.status == 200);
  REQUIRE_THAT(pl.j()["reply"].get<std::string>(), ContainsSubstring("ana"));
  REQUIRE(env.post("/api/console", {{"line", "kick 1 sai"}}, c).status == 200);
  auto sc = env.post("/api/console", {{"line", "/hello mundo"}}, c);
  REQUIRE(sc.status == 200);
  REQUIRE(sc.j()["reply"] == std::string("oi de ") + kAdmin);
  {
    std::lock_guard<std::mutex> lk(env.fake.mu);
    REQUIRE(env.fake.cmds.size() == 1);
    REQUIRE(!env.fake.cmds[0].from.has_value());
    REQUIRE(env.fake.cmds[0].adminDid == kAdmin);
    REQUIRE(env.fake.cmds[0].line == "/hello mundo");
    REQUIRE(env.fake.sim.size() == 1);
    REQUIRE(std::get<cbscript::CmdKick>(env.fake.sim[0]).reason == "sai");
  }
  REQUIRE(env.fake.hasAudit("\"console\"", "/hello mundo"));
  REQUIRE(env.fake.hasAudit("\"console\"", std::string("oi de ")));
  // limite de taxa (rajada 5): já foram 3
  REQUIRE(env.post("/api/console", {{"line", "status"}}, c).status == 200);
  REQUIRE(env.post("/api/console", {{"line", "status"}}, c).status == 200);
  REQUIRE(env.post("/api/console", {{"line", "status"}}, c).status == 429);
  env.fake.mono = env.fake.mono.load() + 10;
  REQUIRE(env.post("/api/console", {{"line", std::string(513, 'a')}}, c).status == 400);
  REQUIRE(env.post("/api/console", {{"line", "a\nb"}}, c).status == 400);
  REQUIRE(env.post("/api/console", {{"line", ""}}, c).status == 400);
}

TEST_CASE("admin: logs tail only fixed files, bounded, never lines with the DB password") {
  AdminEnv env([](cbadmin::AdminOptions& o) { o.dbPassword = "s3nh4-secreta"; });
  put(env.logs.p / "audit-2026-11-09.jsonl", "{\"old\":1}\n");
  std::string today;
  for (int i = 0; i < 600; ++i) today += "{\"n\":" + std::to_string(i) + "}\n";
  today += "{\"leak\":\"s3nh4-secreta\"}\n{\"n\":\"last\"}\n";
  put(env.logs.p / "audit-2026-11-10.jsonl", today);
  put(env.logs.p / "anticheat-2026-11-10.jsonl", std::string(3 << 20, 'x') + "\n{\"ac\":1}\n");
  put(env.logs.p / "secret.txt", "nunca\n");
  auto c = env.login();

  auto a = env.get("/api/logs?kind=audit&tail=3", c);
  REQUIRE(a.status == 200);
  auto lines = a.j()["lines"];
  REQUIRE(lines.size() == 2);  // a linha com a senha some
  REQUIRE(lines[0] == "{\"n\":599}");
  REQUIRE(lines[1] == "{\"n\":\"last\"}");
  REQUIRE(a.j()["file"] == "audit-2026-11-10.jsonl");
  REQUIRE(a.body.find("s3nh4") == std::string::npos);

  auto big = env.get("/api/logs?kind=audit&tail=100000", c);
  REQUIRE(big.j()["lines"].size() <= 500);
  auto ac = env.get("/api/logs?kind=anticheat&tail=500", c);
  REQUIRE(ac.status == 200);
  REQUIRE(ac.body.size() <= (size_t{1} << 20) + 4096);
  REQUIRE(ac.j()["lines"].back() == "{\"ac\":1}");
  REQUIRE(env.get("/api/logs?kind=../secret.txt", c).status == 400);
  REQUIRE(env.get("/api/logs?kind=server", c).status == 400);
  REQUIRE(env.get("/api/logs?kind=audit&tail=abc", c).status == 400);
}

// ---------------------------------------------------------------- integração com World + host + bridge

namespace {

using Clock = std::chrono::steady_clock;

struct Live {
  Tmp dir;
  testing_world::Harness h;
  std::unique_ptr<cbscript::ScriptHost> host;
  std::unique_ptr<cbserver::ScriptBridge> bridge;
  std::shared_ptr<cbscript::BanStore> bans = std::make_shared<cbscript::BanStore>(fs::path());
  testing_ticket::DevSigner signer;
  cbserver::StaticKeys keys{signer.keys()};
  std::mutex mu;
  std::vector<std::string> audit, logs;
  std::unique_ptr<cbadmin::AdminServer> srv;
  int port = 0;

  Live() {
    fs::copy(fs::path(CBMP_EXAMPLE_RESOURCES) / "echo", dir.p / "echo", fs::copy_options::recursive);
    put(dir.p / "probe" / "resource.json", R"({"name":"probe","version":"1.0.0","server":["server/main.lua"]})");
    put(dir.p / "probe" / "server" / "main.lua",
        "cb.on('resourceStart', function() cb.log.info('probe start') end)\n"
        "cb.commands.register('quem', function(src, args) return 'src=' .. tostring(src) end, {admin = true})\n");
    cbscript::HostOptions ho;
    ho.idleWait = std::chrono::milliseconds(2);
    ho.log = [this](cbscript::LogLevel, const std::string& m) {
      std::lock_guard<std::mutex> lk(mu);
      logs.push_back(m);
    };
    cbscript::ApiOptions api;
    api.bans = bans;
    api.audit = [this](const std::string& l) {
      std::lock_guard<std::mutex> lk(mu);
      audit.push_back(l);
    };
    cbscript::installApi(ho, api);
    host = std::make_unique<cbscript::ScriptHost>(dir.p, cbscript::Limits{}, [] {
      return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    }, ho);
    host->start();
    bridge = std::make_unique<cbserver::ScriptBridge>(h.world, *host);
    cbadmin::AdminOptions o;
    o.port = 0;
    o.serverId = kServer;
    o.hubUrl = "http://hub.test";
    o.admins = {kAdmin};
    o.logsDir = dir.p;
    auto be = cbadmin::makeBackend(*host, *bridge, bans, keys, [] { return json{{"ok", true}}; },
                                   [this](const std::string& l) {
                                     std::lock_guard<std::mutex> lk(mu);
                                     audit.push_back(l);
                                   },
                                   [] { return int64_t{0}; });
    be.unixNow = [] {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
    };
    srv = std::make_unique<cbadmin::AdminServer>(o, std::move(be));
    srv->start();
    port = srv->boundPort();
  }
  ~Live() {
    srv->stop();
    srv.reset();
    bridge.reset();
    host.reset();
  }
  void tick() {
    bridge->applyCommands(h.now + 0.05);
    h.step();
    bridge->publishPlayers();
  }
  template <class P>
  bool ticksUntil(P pred) {
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < end) {
      tick();
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }
  bool has(const std::vector<std::string>& v, const std::string& a, const std::string& b = "") {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& l : v)
      if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) return true;
    return false;
  }
  size_t count(const std::vector<std::string>& v, const std::string& a) {
    std::lock_guard<std::mutex> lk(mu);
    size_t n = 0;
    for (auto& l : v) n += l.find(a) != std::string::npos;
    return n;
  }
  std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }
  httplib::Result call(const std::string& method, const std::string& path, const std::string& cookie,
                       const json& body = json::object()) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(10);
    httplib::Headers hd{{"Origin", origin()}, {"X-CB-Admin", "1"}};
    if (!cookie.empty()) hd.emplace("Cookie", cookie);
    if (method == "GET") return cli.Get(path, hd);
    return cli.Post(path, hd, body.dump(), "application/json");
  }
  std::string login() {
    auto s = call("GET", "/auth/start", "");
    REQUIRE(s);
    std::string st;
    auto range = s->headers.equal_range("Set-Cookie");
    for (auto it = range.first; it != range.second; ++it)
      if (it->second.rfind("cbadmin_state=", 0) == 0) st = it->second.substr(14, it->second.find(';') - 14);
    REQUIRE(!st.empty());
    const int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto tk = signer.signDid("sub-a", kAdmin, "jti-live", now, std::string(kServer) + "#admin");
    auto r = call("POST", "/auth/login", "cbadmin_state=" + st, json{{"ticket", tk}, {"state", st}});
    REQUIRE(r);
    REQUIRE(r->status == 200);
    std::string sess;
    range = r->headers.equal_range("Set-Cookie");
    for (auto it = range.first; it != range.second; ++it)
      if (it->second.rfind("cbadmin=", 0) == 0) sess = it->second.substr(0, it->second.find(';'));
    REQUIRE(!sess.empty());
    return sess;
  }
};

}  // namespace

TEST_CASE("admin: kick via the API becomes a KickOrder in the World and an audit line") {
  Live env;
  auto c = env.login();
  auto a = env.h.add("ana", {100, 0, 0});
  REQUIRE(env.ticksUntil([&] { return env.host->players()->size() == 1; }));
  auto list = env.call("GET", "/api/players", c);
  REQUIRE(list->status == 200);
  REQUIRE(json::parse(list->body)[0]["name"] == "ana");

  auto r = env.call("POST", "/api/players/" + std::to_string(a) + "/kick", c, {{"reason", "fora"}});
  REQUIRE(r->status == 202);
  REQUIRE(env.ticksUntil([&] { return !env.h.kicks.empty(); }));
  REQUIRE(env.h.kicks[0].who == a);
  REQUIRE(env.h.kicks[0].reason == "fora");
  REQUIRE(env.has(env.audit, "\"kick\"", kAdmin));
}

TEST_CASE("admin: ban via the API writes the ban and kicks by did") {
  Live env;
  auto c = env.login();
  auto a = env.h.add("ana", {100, 0, 0});
  REQUIRE(env.ticksUntil([&] { return env.host->players()->size() == 1; }));
  auto r = env.call("POST", "/api/players/" + std::to_string(a) + "/ban", c, {{"reason", "cheat"}});
  REQUIRE(r->status == 202);
  REQUIRE(env.ticksUntil([&] { return !env.h.kicks.empty(); }));
  const auto end = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < end && !env.bans->check("did-ana", 0)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto b = env.bans->check("did-ana", 0);
  REQUIRE(b);
  REQUIRE(b->by == std::string("admin:") + kAdmin);
}

TEST_CASE("admin: resource restart via the API is reflected in the status") {
  Live env;
  auto c = env.login();
  auto status = [&](const std::string& name) {
    auto r = env.call("GET", "/api/resources", c);
    for (auto& x : json::parse(r->body))
      if (x["name"] == name) return x["state"].get<std::string>();
    return std::string("?");
  };
  REQUIRE(status("probe") == "running");
  REQUIRE(env.count(env.logs, "probe start") == 1);
  auto st = env.call("POST", "/api/resources/probe/stop", c);
  REQUIRE(st->status == 200);
  REQUIRE(json::parse(st->body)["ok"] == true);
  REQUIRE(status("probe") == "stopped");
  REQUIRE(json::parse(env.call("POST", "/api/resources/probe/start", c)->body)["ok"] == true);
  REQUIRE(status("probe") == "running");
  REQUIRE(json::parse(env.call("POST", "/api/resources/probe/restart", c)->body)["ok"] == true);
  REQUIRE(status("probe") == "running");
  REQUIRE(env.count(env.logs, "probe start") == 3);
  REQUIRE(env.has(env.audit, "\"restart\"", kAdmin));
}

TEST_CASE("admin: console reaches an admin=true command registered by a resource") {
  Live env;
  auto c = env.login();
  auto r = env.call("POST", "/api/console", c, {{"line", "quem"}});
  REQUIRE(r->status == 200);
  REQUIRE(json::parse(r->body)["reply"] == "src=0");
  REQUIRE(env.has(env.audit, "\"command\"", kAdmin));  // auditoria da API cb.* com o did do admin
  auto rr = env.call("POST", "/api/console", c, {{"line", "restart probe"}});
  REQUIRE_THAT(json::parse(rr->body)["reply"].get<std::string>(), ContainsSubstring("probe"));
}
