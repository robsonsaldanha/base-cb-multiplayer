// HttpOutPool e validação de URL/endereços (ver http_out.hpp).
#include "cbscript/http_out.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <httplib.h>  // antes de qualquer <windows.h>: traz winsock2/ws2tcpip (Windows) ou arpa/inet/netdb (POSIX)
#include <spdlog/spdlog.h>

namespace cbscript {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

constexpr size_t kMaxUrl = 2048;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

std::string fmtBytes(size_t n) {
  if (n >= (size_t{1} << 20) && n % (size_t{1} << 20) == 0) return std::to_string(n >> 20) + " MiB";
  if (n >= 1024 && n % 1024 == 0) return std::to_string(n >> 10) + " KiB";
  return std::to_string(n) + " bytes";
}

// Nome de host já em minúsculas. Recusa IP (inclusive formas numéricas que o resolvedor aceitaria).
bool validHostname(std::string_view h, std::string& err) {
  if (h.empty()) {
    err = "falta o host";
    return false;
  }
  if (h.size() > 253) {
    err = "host longo demais";
    return false;
  }
  if (h.find_first_of("[]:") != std::string_view::npos) {
    err = "IP literal não é permitido (use o nome do host)";
    return false;
  }
  for (char c : h) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      err = "host com caracteres não ASCII (use punycode: xn--...)";
      return false;
    }
    if (c == '*') {
      err = "curinga não é permitido no host";
      return false;
    }
    if (!((c >= 'a' && c <= 'z') || isDigit(c) || c == '.' || c == '-')) {
      err = "host inválido (só letras minúsculas, dígitos, '.' e '-')";
      return false;
    }
  }
  size_t start = 0;
  std::string_view last;
  while (start <= h.size()) {
    size_t dot = h.find('.', start);
    if (dot == std::string_view::npos) dot = h.size();
    const std::string_view label = h.substr(start, dot - start);
    if (label.empty() || label.size() > 63) {
      err = "host inválido (rótulo vazio ou com mais de 63 caracteres)";
      return false;
    }
    if (label.front() == '-' || label.back() == '-') {
      err = "host inválido (rótulo começa ou termina com '-')";
      return false;
    }
    last = label;
    start = dot + 1;
    if (dot == h.size()) break;
  }
  // Último rótulo numérico (ou 0x...) = o resolvedor trataria como IPv4 ("127.1", "2130706433", "0x7f.1").
  const bool allDigits = std::all_of(last.begin(), last.end(), isDigit);
  const bool hex = last.size() > 2 && last[0] == '0' && last[1] == 'x' &&
                   std::all_of(last.begin() + 2, last.end(), isHex);
  if (allDigits || hex) {
    err = "IP literal não é permitido (use o nome do host)";
    return false;
  }
  return true;
}

bool parsePort(std::string_view s, int& out) {
  if (s.empty() || s.size() > 5) return false;
  int v = 0;
  for (char c : s) {
    if (!isDigit(c)) return false;
    v = v * 10 + (c - '0');
  }
  if (v < 1 || v > 65535) return false;
  out = v;
  return true;
}

int defaultPort(std::string_view scheme) { return scheme == "http" ? 80 : 443; }

// ---- endereços ----

std::string v4Refusal(uint32_t a, bool allowLoopback) {
  auto in = [a](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
  if (in(0x7F000000u, 8)) return allowLoopback ? std::string() : "loopback (127.0.0.0/8)";
  if (in(0x00000000u, 8)) return "rede 'este host' (0.0.0.0/8)";
  if (in(0x0A000000u, 8) || in(0xAC100000u, 12) || in(0xC0A80000u, 16)) return "rede privada (RFC 1918)";
  if (in(0x64400000u, 10)) return "CGNAT (100.64.0.0/10)";
  if (in(0xA9FE0000u, 16)) return "link-local (169.254.0.0/16)";
  if (in(0xC0000000u, 24)) return "reservado IETF (192.0.0.0/24)";
  if (in(0xC0000200u, 24) || in(0xC6336400u, 24) || in(0xCB007100u, 24)) return "faixa de documentação";
  if (in(0xC6120000u, 15)) return "faixa de benchmark (198.18.0.0/15)";
  if (in(0xC0586300u, 24)) return "relay 6to4 (192.88.99.0/24)";
  if (in(0xE0000000u, 4)) return "multicast";
  if (a == 0xFFFFFFFFu) return "broadcast";
  if (in(0xF0000000u, 4)) return "reservado (240.0.0.0/4)";
  return {};
}

uint32_t v4At(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

std::string v6Refusal(const uint8_t b[16], bool allowLoopback) {
  auto zero = [b](int from, int to) {
    for (int i = from; i < to; ++i)
      if (b[i] != 0) return false;
    return true;
  };
  auto embedded = [&](uint32_t v4, const char* what) {
    const std::string r = v4Refusal(v4, allowLoopback);
    return r.empty() ? r : std::string(what) + " com IPv4 proibido: " + r;
  };
  if (zero(0, 16)) return "endereço não especificado (::)";
  if (zero(0, 15) && b[15] == 1) return allowLoopback ? std::string() : "loopback (::1)";
  if (zero(0, 10) && b[10] == 0xFF && b[11] == 0xFF) return embedded(v4At(b + 12), "IPv4 mapeado");
  if (zero(0, 12)) return embedded(v4At(b + 12), "IPv4 compatível");
  if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B) {
    if (zero(4, 12)) return embedded(v4At(b + 12), "NAT64");
    if (b[4] == 0x00 && b[5] == 0x01) return "NAT64 local (64:ff9b:1::/48)";
  }
  if (b[0] == 0x20 && b[1] == 0x01) {
    if (b[2] == 0x00 && b[3] == 0x00) {  // Teredo: servidor nos bits 32-63, cliente (invertido) nos 96-127
      const uint32_t server = v4At(b + 4);
      const uint32_t client = ~v4At(b + 12);
      std::string r = embedded(server, "Teredo (servidor)");
      if (r.empty()) r = embedded(client, "Teredo (cliente)");
      return r;
    }
    if (b[2] == 0x0D && b[3] == 0xB8) return "faixa de documentação (2001:db8::/32)";
    if (b[2] == 0x00 && b[3] == 0x02 && b[4] == 0x00 && b[5] == 0x00) return "faixa de benchmark (2001:2::/48)";
    if (b[2] == 0x00 && (b[3] & 0xF0) == 0x10) return "ORCHID (2001:10::/28)";
    if (b[2] == 0x00 && (b[3] & 0xF0) == 0x20) return "ORCHIDv2 (2001:20::/28)";
  }
  if (b[0] == 0x20 && b[1] == 0x02) return embedded(v4At(b + 2), "6to4");
  if ((b[0] & 0xFE) == 0xFC) return "endereço local único (fc00::/7)";
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return "link-local (fe80::/10)";
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) return "site-local (fec0::/10)";
  if (b[0] == 0xFF) return "multicast (ff00::/8)";
  if ((b[0] & 0xE0) != 0x20) return "fora do espaço global (2000::/3)";
  return {};
}

}  // namespace

// ------------------------------------------------------------------------------------------------- utilitários

std::string HttpUrl::forLog() const {
  std::string s = scheme + "://" + host;
  if (explicitPort) s += ":" + std::to_string(port);
  const auto q = target.find('?');
  s += target.substr(0, q);
  return s;
}

std::string httpUrlForLog(std::string_view url) {
  const auto cut = url.find_first_of("?#");
  return std::string(url.substr(0, cut));
}

bool parseHttpUrl(std::string_view url, HttpUrl& out, std::string& err, bool allowHttp) {
  if (url.size() > kMaxUrl) {
    err = "URL longa demais (máximo " + std::to_string(kMaxUrl) + " bytes)";
    return false;
  }
  for (char c : url) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7F || c == '\\') {
      err = "caractere inválido na URL (espaço, controle ou '\\'; use %XX)";
      return false;
    }
    if (u >= 0x80) {
      err = "URL com caracteres não ASCII (use punycode no host e %XX no caminho)";
      return false;
    }
  }
  const auto sep = url.find("://");
  if (sep == std::string_view::npos) {
    err = "URL sem esquema (use https://host/...)";
    return false;
  }
  std::string scheme;
  for (char c : url.substr(0, sep)) scheme += lower(c);
  if (!(scheme == "https" || (allowHttp && scheme == "http"))) {
    err = "só https é permitido";
    return false;
  }
  std::string_view rest = url.substr(sep + 3);
  const auto authEnd = rest.find_first_of("/?#");
  std::string_view auth = rest.substr(0, authEnd);
  std::string_view tail = authEnd == std::string_view::npos ? std::string_view{} : rest.substr(authEnd);
  if (auth.find('@') != std::string_view::npos) {
    err = "usuário/senha na URL não é permitido (use cabeçalhos)";
    return false;
  }
  if (!auth.empty() && auth.front() == '[') {
    err = "IP literal não é permitido (use o nome do host)";
    return false;
  }
  HttpUrl u;
  u.scheme = scheme;
  u.port = defaultPort(scheme);
  if (const auto colon = auth.rfind(':'); colon != std::string_view::npos) {
    if (!parsePort(auth.substr(colon + 1), u.port)) {
      err = "porta inválida na URL";
      return false;
    }
    u.explicitPort = true;
    auth = auth.substr(0, colon);
  }
  for (char c : auth) u.host += lower(c);
  if (!validHostname(u.host, err)) return false;
  if (const auto hash = tail.find('#'); hash != std::string_view::npos) tail = tail.substr(0, hash);
  if (tail.empty())
    u.target = "/";
  else if (tail.front() == '?')
    u.target = "/" + std::string(tail);
  else
    u.target = std::string(tail);
  out = std::move(u);
  return true;
}

bool parseHttpAllowEntry(std::string_view entry, std::string& normalized, std::string& err) {
  if (entry.empty()) {
    err = "entrada vazia";
    return false;
  }
  for (char c : entry)
    if (c >= 'A' && c <= 'Z') {
      err = "use só minúsculas";
      return false;
    }
  if (entry.find("://") != std::string_view::npos || entry.find_first_of("/?#@") != std::string_view::npos) {
    err = "só o host (sem esquema, caminho ou usuário)";
    return false;
  }
  std::string_view host = entry;
  std::string port;
  if (entry.front() == '[' || std::count(entry.begin(), entry.end(), ':') > 1) {
    err = "IP literal não é permitido (use o nome do host)";
    return false;
  }
  if (const auto colon = entry.rfind(':'); colon != std::string_view::npos) {
    int p = 0;
    if (!parsePort(entry.substr(colon + 1), p)) {
      err = "porta inválida";
      return false;
    }
    port = std::to_string(p);
    host = entry.substr(0, colon);
  }
  if (!validHostname(host, err)) return false;
  normalized = std::string(host) + (port.empty() ? "" : ":" + port);
  return true;
}

bool httpAllowed(const std::vector<std::string>& allow, const HttpUrl& u) {
  const std::string withPort = u.host + ":" + std::to_string(u.port);
  for (const auto& e : allow) {
    if (e.find(':') != std::string::npos) {
      if (e == withPort) return true;
    } else if (e == u.host && u.port == defaultPort(u.scheme)) {
      return true;
    }
  }
  return false;
}

bool resolveHttpRedirect(const HttpUrl& base, std::string_view location, HttpUrl& out, std::string& err,
                         bool allowHttp) {
  if (location.empty()) {
    err = "redirecionamento sem Location";
    return false;
  }
  std::string abs;
  const std::string origin =
      base.scheme + "://" + base.host + (base.explicitPort ? ":" + std::to_string(base.port) : std::string());
  if (location.size() >= 2 && location[0] == '/' && location[1] == '/') {
    abs = base.scheme + ":" + std::string(location);
  } else if (location.find("://") != std::string_view::npos &&
             location.find("://") < location.find_first_of("/?#")) {
    abs = std::string(location);
  } else if (location.front() == '/') {
    abs = origin + std::string(location);
  } else {
    const auto colon = location.find(':');
    if (colon != std::string_view::npos && colon < location.find_first_of("/?#")) {
      err = "redirecionamento com esquema inválido";
      return false;
    }
    std::string path = base.target.substr(0, base.target.find('?'));
    if (location.front() == '?') {
      abs = origin + path + std::string(location);
    } else {
      path = path.substr(0, path.rfind('/') + 1);
      abs = origin + path + std::string(location);
    }
  }
  HttpUrl u;
  if (!parseHttpUrl(abs, u, err, allowHttp || base.scheme == "http")) return false;
  if (base.scheme == "https" && u.scheme != "https") {
    err = "redirecionamento de https para http não é permitido";
    return false;
  }
  if (!allowHttp && u.scheme != "https") {
    err = "só https é permitido";
    return false;
  }
  out = std::move(u);
  return true;
}

std::string httpAddressRefusal(std::string_view ip, bool allowLoopback) {
  if (ip.empty() || ip.size() > 64) return "endereço inválido";
  const std::string s(ip);
  uint8_t buf[16] = {};
  if (inet_pton(AF_INET, s.c_str(), buf) == 1) return v4Refusal(v4At(buf), allowLoopback);
  if (inet_pton(AF_INET6, s.c_str(), buf) == 1) return v6Refusal(buf, allowLoopback);
  return "endereço inválido";
}

// ------------------------------------------------------------------------------------------------------- pool

namespace detail {

struct HttpJob {
  uint64_t id = 0;
  HttpOutRequest req;
  HttpUrl url;
  HttpOutPool::Done done;
  Clock::time_point queuedUntil;
  size_t reqBytes = 0;
};

struct HttpOwner {
  std::deque<HttpJob> q;
  size_t pending = 0;  // na fila + em execução
  size_t bytes = 0;    // corpos de pedido + respostas recebidas
  bool inRr = false;
};

struct HttpSlot {
  uint64_t id = 0, owner = 0;
  std::shared_ptr<httplib::Client> cli;
  Clock::time_point deadline{};
  bool cancelled = false;  // cancel(): não chama done
  bool stopping = false;   // watchdog ou encerramento fecharam o socket
  bool timedOut = false;
  bool active = false;
  size_t charged = 0;  // bytes de resposta cobrados do dono neste pedido
  bool exited = false;
};

struct HttpPoolState {
  HttpOutOptions opt;
  bool test = false;

  mutable std::mutex mu;
  std::condition_variable cv, wdCv, exitCv;
  std::unordered_map<uint64_t, HttpOwner> owners;
  std::deque<uint64_t> rr;
  uint64_t lastServed = 0;
  size_t queued = 0;
  std::vector<HttpSlot> slots;
  std::vector<std::thread> threads;
  std::thread watchdog;
  bool wdExited = false;
  bool stop = false;
  uint64_t nextId = 1;
  size_t exited = 0;

  std::atomic<uint64_t> requests{0}, errors{0}, timeouts{0}, refused{0};
  std::atomic<int> busy{0};
  std::atomic<bool> quiet{false};

  void log(bool error, const std::string& m) const {
    if (quiet.load()) return;
    try {
      if (opt.log)
        opt.log(error, m);
      else if (error)
        spdlog::error("{}", m);
      else
        spdlog::warn("{}", m);
    } catch (...) {
    }
  }

  // Sob mu.
  void release(uint64_t owner, size_t bytes) {
    auto it = owners.find(owner);
    if (it == owners.end()) return;
    if (it->second.pending > 0) --it->second.pending;
    it->second.bytes -= std::min(it->second.bytes, bytes);
    if (it->second.pending == 0 && it->second.q.empty()) owners.erase(it);
  }

  // Sob mu: próximo pedido em rodízio entre os donos.
  bool pick(HttpJob& out) {
    if (rr.empty()) return false;
    if (rr.size() > 1 && rr.front() == lastServed) {
      rr.push_back(rr.front());
      rr.pop_front();
    }
    const uint64_t owner = rr.front();
    rr.pop_front();
    auto& o = owners[owner];
    out = std::move(o.q.front());
    o.q.pop_front();
    --queued;
    if (o.q.empty())
      o.inRr = false;
    else
      rr.push_back(owner);
    lastServed = owner;
    return true;
  }

  static void finish(HttpJob& j, HttpOutResult r) {
    if (!j.done) return;
    try {
      j.done(std::move(r));
    } catch (...) {
    }
  }

  // Sob mu: cobra `n` bytes de resposta do dono (registrados na vaga `w`). false = passaria do teto.
  bool charge(size_t w, uint64_t owner, size_t n) {
    auto it = owners.find(owner);
    if (it == owners.end()) return false;
    if (it->second.bytes + n > opt.maxBytesPerOwner) return false;
    it->second.bytes += n;
    slots[w].charged += n;
    return true;
  }
  // Sob mu: devolve ao dono o que a vaga `w` cobrou até aqui.
  void uncharge(size_t w, uint64_t owner) {
    if (auto it = owners.find(owner); it != owners.end())
      it->second.bytes -= std::min(it->second.bytes, slots[w].charged);
    slots[w].charged = 0;
  }

  std::vector<std::string> resolve(const std::string& host, std::string& err) const {
    std::vector<std::string> out;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
      err = "não foi possível resolver " + host;
      return out;
    }
    for (addrinfo* p = res; p; p = p->ai_next) {
      char buf[INET6_ADDRSTRLEN] = {};
      const void* src = nullptr;
      if (p->ai_family == AF_INET)
        src = &reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr;
      else if (p->ai_family == AF_INET6)
        src = &reinterpret_cast<const sockaddr_in6*>(p->ai_addr)->sin6_addr;
      if (!src || !inet_ntop(p->ai_family, src, buf, sizeof buf)) continue;
      std::string ip(buf);
      if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(std::move(ip));
    }
    freeaddrinfo(res);
    if (out.empty()) err = "não foi possível resolver " + host;
    // IPv4 primeiro: rota IPv6 quebrada (comum em VPS) ou porta fechada no Windows (~2 s por tentativa) não
    // consomem o prazo antes de tentar o IPv4. A ordem relativa de cada família é a do resolvedor.
    std::stable_partition(out.begin(), out.end(), [](const std::string& ip) { return ip.find(':') == std::string::npos; });
    return out;
  }

  struct Attempt {
    bool sent = false;  // houve resposta HTTP
    bool connectFailed = false;
    HttpOutResult res;
  };

  Attempt attempt(size_t w, const HttpJob& job, const HttpUrl& cur, const std::string& ip, bool moreAddrs,
                  HttpMethod method, const std::string& body, const std::string& contentType,
                  const HttpHeaders& headers, Clock::time_point deadline) {
    Attempt at;
    const auto left = std::chrono::duration_cast<milliseconds>(deadline - Clock::now());
    if (left.count() <= 0) {
      at.res = HttpOutResult::fail("tempo esgotado (" + std::to_string(opt.timeout.count()) + " ms) em " +
                                       cur.forLog(),
                                   true);
      return at;
    }
    auto cli = std::make_shared<httplib::Client>(cur.scheme + "://" + cur.host + ":" + std::to_string(cur.port));
    cli->set_direct_hostname_addr_map({{cur.host, ip}});
    cli->set_follow_location(false);
    cli->set_keep_alive(false);
    cli->set_decompress(false);
    cli->set_compress(false);
    cli->set_path_encode(false);
    cli->set_payload_max_length(opt.maxResponseBody);
    // Com outros endereços na reserva, um endereço que não responde não leva o prazo inteiro.
    cli->set_connection_timeout(moreAddrs ? std::min(left, milliseconds(3000)) : left);
    cli->set_read_timeout(left);
    cli->set_write_timeout(left);
    cli->set_max_timeout(left);
    if (cur.scheme == "https") {
      cli->enable_server_certificate_verification(true);
      cli->enable_server_hostname_verification(true);
      if (!opt.caFile.empty()) cli->set_ca_cert_path(opt.caFile);
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      if (slots[w].cancelled || slots[w].stopping) {
        if (slots[w].timedOut)
          at.res = HttpOutResult::fail("tempo esgotado (" + std::to_string(opt.timeout.count()) + " ms) em " +
                                           cur.forLog(),
                                       true);
        else
          at.res = HttpOutResult::fail(stop ? "HTTP encerrado" : "pedido cancelado");
        return at;
      }
      slots[w].cli = cli;
    }
    httplib::Request rq;
    rq.method = method == HttpMethod::Post ? "POST" : "GET";
    rq.path = cur.target;
    for (const auto& [k, v] : headers) rq.headers.emplace(k, v);
    rq.headers.emplace("Accept-Encoding", "identity");
    if (!rq.has_header("User-Agent")) rq.headers.emplace("User-Agent", "cb-server");
    if (method == HttpMethod::Post) {
      rq.body = body;
      rq.headers.emplace("Content-Type", contentType.empty() ? "application/octet-stream" : contentType);
    }
    std::string got;
    bool tooBig = false, overBudget = false;
    rq.content_receiver = [&](const char* data, size_t n, size_t, size_t) {
      if (got.size() + n > opt.maxResponseBody) {
        tooBig = true;
        return false;
      }
      {
        std::lock_guard<std::mutex> lk(mu);
        if (!charge(w, job.req.owner, n)) {
          overBudget = true;
          return false;
        }
      }
      got.append(data, n);
      return true;
    };
    httplib::Response rs;
    httplib::Error e = httplib::Error::Success;
    bool ok = false;
    try {
      ok = cli->send(rq, rs, e);
    } catch (const std::exception& ex) {
      e = httplib::Error::Unknown;
      (void)ex;
    } catch (...) {
      e = httplib::Error::Unknown;
    }
    bool timedOutFlag = false, stopping = false, shutdown = false;
    {
      std::lock_guard<std::mutex> lk(mu);
      slots[w].cli.reset();
      timedOutFlag = slots[w].timedOut;
      stopping = slots[w].stopping;
      shutdown = stop;
    }
    const std::string where = cur.forLog();
    if (ok && !tooBig && !overBudget) {
      size_t hbytes = 0;
      for (const auto& [k, v] : rs.headers) hbytes += k.size() + v.size() + 4;
      if (hbytes > opt.maxResponseHeaderBytes) {
        at.res = HttpOutResult::fail("cabeçalhos da resposta grandes demais (máximo " +
                                     fmtBytes(opt.maxResponseHeaderBytes) + ") em " + where);
        return at;
      }
      at.sent = true;
      at.res.ok = true;
      at.res.status = rs.status;
      at.res.body = std::move(got);
      for (const auto& [k, v] : rs.headers) {
        std::string name;
        name.reserve(k.size());
        for (char c : k) name += lower(c);
        auto it = std::find_if(at.res.headers.begin(), at.res.headers.end(),
                               [&](const auto& p) { return p.first == name; });
        if (it == at.res.headers.end())
          at.res.headers.emplace_back(std::move(name), v);
        else
          it->second += ", " + v;
      }
      return at;
    }
    const bool late = Clock::now() >= deadline;
    if (tooBig || e == httplib::Error::ExceedMaxPayloadSize) {
      at.res = HttpOutResult::fail("resposta grande demais (máximo " + fmtBytes(opt.maxResponseBody) + ") em " + where);
    } else if (overBudget) {
      at.res = HttpOutResult::fail("bytes demais em andamento neste resource (máximo " +
                                   fmtBytes(opt.maxBytesPerOwner) + " de corpos)");
    } else if (timedOutFlag || (late && !stopping) || e == httplib::Error::Timeout ||
               e == httplib::Error::ConnectionTimeout) {
      at.res = HttpOutResult::fail("tempo esgotado (" + std::to_string(opt.timeout.count()) + " ms) em " + where, true);
    } else if (stopping) {
      at.res = HttpOutResult::fail(shutdown ? "HTTP encerrado" : "pedido cancelado");
    } else if (e == httplib::Error::SSLServerVerification) {
      at.res = HttpOutResult::fail("certificado TLS inválido ou não confiável em " + where);
    } else if (e == httplib::Error::SSLServerHostnameVerification) {
      at.res = HttpOutResult::fail("certificado TLS não vale para " + cur.host);
    } else if (e == httplib::Error::SSLConnection || e == httplib::Error::SSLLoadingCerts) {
      at.res = HttpOutResult::fail("falha no TLS (" + httplib::to_string(e) + ") em " + where);
    } else if (e == httplib::Error::Connection) {
      at.connectFailed = true;
      at.res = HttpOutResult::fail("falha ao conectar em " + where);
    } else {
      at.res = HttpOutResult::fail("erro HTTP (" + httplib::to_string(e) + ") em " + where);
    }
    return at;
  }

  HttpOutResult execute(size_t w, HttpJob& job) {
    const auto deadline = Clock::now() + opt.timeout;
    {
      std::lock_guard<std::mutex> lk(mu);
      slots[w].deadline = deadline;
    }
    HttpUrl cur = job.url;
    HttpMethod method = job.req.method;
    std::string body = job.req.body, contentType = job.req.contentType;
    HttpHeaders headers = job.req.headers;
    for (int hop = 0;; ++hop) {
      if (!httpAllowed(job.req.allow, cur)) {
        refused.fetch_add(1);
        return HttpOutResult::fail(cur.forLog() + ": host fora de permissions.http do resource");
      }
      std::string err;
      const auto addrs = resolve(cur.host, err);
      if (addrs.empty()) return HttpOutResult::fail(err);
      for (const auto& ip : addrs) {
        const std::string why = httpAddressRefusal(ip, test);
        if (!why.empty()) {
          refused.fetch_add(1);
          return HttpOutResult::fail(cur.host + " resolve para um endereço proibido: " + why);
        }
      }
      Attempt at;
      for (size_t i = 0; i < addrs.size(); ++i) {
        at = attempt(w, job, cur, addrs[i], i + 1 < addrs.size(), method, body, contentType, headers, deadline);
        if (!at.connectFailed) break;
        {
          std::lock_guard<std::mutex> lk(mu);
          if (slots[w].stopping || slots[w].cancelled) break;
        }
      }
      if (!at.res.ok) return std::move(at.res);
      const int st = at.res.status;
      const bool redirect = st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
      std::string location;
      for (const auto& [k, v] : at.res.headers)
        if (k == "location") location = v;
      if (!redirect || location.empty()) return std::move(at.res);
      {
        std::lock_guard<std::mutex> lk(mu);
        uncharge(w, job.req.owner);  // o corpo do salto é descartado
      }
      if (hop >= opt.maxRedirects)
        return HttpOutResult::fail("redirecionamentos demais (máximo " + std::to_string(opt.maxRedirects) + ") a partir de " +
                                   job.url.forLog());
      HttpUrl next;
      if (!resolveHttpRedirect(cur, location, next, err, test)) {
        refused.fetch_add(1);
        return HttpOutResult::fail("redirecionamento recusado a partir de " + cur.forLog() + ": " + err);
      }
      if (!httpAllowed(job.req.allow, next)) {
        refused.fetch_add(1);
        return HttpOutResult::fail("redirecionamento para " + next.forLog() + " fora de permissions.http do resource");
      }
      if (st == 303 || ((st == 301 || st == 302) && method == HttpMethod::Post)) {
        method = HttpMethod::Get;
        body.clear();
        contentType.clear();
      }
      if (next.host != cur.host || next.port != cur.port) headers.clear();  // credenciais não vão para outro host
      cur = std::move(next);
    }
  }

  void workerMain(size_t w) {
    for (;;) {
      HttpJob job;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return stop || queued > 0; });
        if (stop) break;
        pick(job);
        slots[w] = HttpSlot{};
        slots[w].id = job.id;
        slots[w].owner = job.req.owner;
        slots[w].active = true;
        slots[w].deadline = Clock::now() + opt.timeout;
      }
      busy.fetch_add(1);
      HttpOutResult r;
      if (Clock::now() > job.queuedUntil) {
        r = HttpOutResult::fail("tempo esgotado na fila do HTTP (" + std::to_string(opt.queueTimeout.count()) + " ms)",
                                true);
      } else {
        try {
          r = execute(w, job);
        } catch (...) {
          r = HttpOutResult::fail("erro interno do HTTP");
        }
      }
      busy.fetch_sub(1);
      bool cancelled = false;
      {
        std::lock_guard<std::mutex> lk(mu);
        cancelled = slots[w].cancelled;
        uncharge(w, job.req.owner);
        slots[w] = HttpSlot{};
        release(job.req.owner, job.reqBytes);
      }
      requests.fetch_add(1);
      if (!r.ok) errors.fetch_add(1);
      if (r.timedOut) timeouts.fetch_add(1);
      if (!cancelled && !quiet.load()) finish(job, std::move(r));
    }
    std::lock_guard<std::mutex> lk(mu);
    slots[w].exited = true;
    ++exited;
    exitCv.notify_all();
  }

  // Fecha os sockets de pedidos cancelados, vencidos ou do encerramento (fora de `mu`: stop() pode esperar o
  // mutex do socket enquanto a conexão está sendo aberta).
  void watchdogMain() {
    std::unique_lock<std::mutex> lk(mu);
    for (;;) {
      if (stop && exited == slots.size()) break;
      std::vector<std::shared_ptr<httplib::Client>> toStop;
      const auto now = Clock::now();
      for (auto& s : slots) {
        if (!s.active) continue;
        if (!s.timedOut && now > s.deadline + milliseconds(200)) s.timedOut = true;
        if (stop || s.cancelled || s.timedOut) {
          s.stopping = true;
          if (s.cli) toStop.push_back(s.cli);
        }
      }
      if (!toStop.empty()) {
        lk.unlock();
        for (auto& c : toStop) c->stop();
        toStop.clear();
        lk.lock();
      }
      wdCv.wait_for(lk, milliseconds(50));
    }
    wdExited = true;
    exitCv.notify_all();
  }
};

}  // namespace detail

HttpOutPool::HttpOutPool(HttpOutOptions opt) { init(std::move(opt), false); }
HttpOutPool::HttpOutPool(HttpOutOptions opt, InsecureLoopbackForTests) { init(std::move(opt), true); }

void HttpOutPool::init(HttpOutOptions opt, bool test) {
  st_ = std::make_shared<detail::HttpPoolState>();
  auto& s = *st_;
  opt.threads = std::clamp(opt.threads, 1, 64);
  opt.maxRedirects = std::clamp(opt.maxRedirects, 0, 10);
  if (opt.timeout.count() < 1) opt.timeout = milliseconds(1);
  s.opt = std::move(opt);
  s.test = test;
  s.slots.resize(static_cast<size_t>(s.opt.threads));
  auto self = st_;
  for (size_t i = 0; i < s.slots.size(); ++i) s.threads.emplace_back([self, i] { self->workerMain(i); });
  s.watchdog = std::thread([self] { self->watchdogMain(); });
}

HttpOutPool::~HttpOutPool() {
  auto& s = *st_;
  std::vector<detail::HttpJob> left;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    s.stop = true;
    for (auto& [id, o] : s.owners)
      for (auto& j : o.q) left.push_back(std::move(j));
    s.owners.clear();
    s.rr.clear();
    s.queued = 0;
    s.cv.notify_all();
    s.wdCv.notify_all();
  }
  for (auto& j : left) detail::HttpPoolState::finish(j, HttpOutResult::fail("HTTP encerrado"));
  {
    std::unique_lock<std::mutex> lk(s.mu);
    s.exitCv.wait_for(lk, s.opt.shutdownWait, [&] { return s.exited == s.slots.size() && s.wdExited; });
  }
  for (size_t i = 0; i < s.threads.size(); ++i) {
    bool done;
    {
      std::lock_guard<std::mutex> lk(s.mu);
      done = s.slots[i].exited;
    }
    if (done) {
      s.threads[i].join();
    } else {
      s.log(true, "http: thread " + std::to_string(i + 1) + " presa no encerramento (DNS?); thread desanexada");
      s.threads[i].detach();
    }
  }
  bool wd;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    wd = s.wdExited;
  }
  if (wd)
    s.watchdog.join();
  else
    s.watchdog.detach();
  s.quiet.store(true);
}

uint64_t HttpOutPool::submit(HttpOutRequest r, Done done, std::string& err) {
  auto& s = *st_;
  const auto& o = s.opt;
  if (r.url.size() > o.maxUrl) {
    err = "URL longa demais (máximo " + std::to_string(o.maxUrl) + " bytes)";
    return 0;
  }
  HttpUrl u;
  if (!parseHttpUrl(r.url, u, err, s.test)) return 0;
  if (!httpAllowed(r.allow, u)) {
    s.refused.fetch_add(1);
    err = u.host + (u.explicitPort ? ":" + std::to_string(u.port) : std::string()) +
          " não está em permissions.http do resource";
    return 0;
  }
  if (r.method == HttpMethod::Get && !r.body.empty()) {
    err = "GET não leva corpo";
    return 0;
  }
  static constexpr std::string_view kReserved[] = {
      "host",       "content-length",   "transfer-encoding", "connection", "accept-encoding", "te",
      "upgrade",    "keep-alive",       "proxy-connection",  "trailer",    "expect",          "proxy-authorization"};
  size_t hbytes = 0;
  HttpHeaders hs;
  hs.reserve(r.headers.size());
  for (auto& [k, v] : r.headers) {
    bool okName = !k.empty() && k.size() <= 256;
    for (char c : k) {
      const bool tchar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) ||
                         std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
      if (!tchar || c == '\0') okName = false;
    }
    bool okValue = true;
    for (char c : v) {
      const auto uc = static_cast<unsigned char>(c);
      if ((uc < 0x20 && c != '\t') || uc == 0x7F) okValue = false;
    }
    if (!okName || !okValue) {
      err = "cabeçalho '" + (okName ? k : std::string("?")) + "' inválido (nome ou valor com caractere de controle)";
      return 0;
    }
    for (auto res : kReserved)
      if (ieq(k, res)) {
        err = "cabeçalho '" + k + "' não pode ser definido pelo script";
        return 0;
      }
    hbytes += k.size() + v.size() + 4;
    if (ieq(k, "content-type")) {
      r.contentType = v;
      continue;
    }
    hs.emplace_back(std::move(k), std::move(v));
  }
  hbytes += r.contentType.size();
  if (hbytes > o.maxRequestHeaderBytes) {
    err = "cabeçalhos grandes demais (máximo " + fmtBytes(o.maxRequestHeaderBytes) + ")";
    return 0;
  }
  for (char c : r.contentType) {
    const auto uc = static_cast<unsigned char>(c);
    if ((uc < 0x20 && c != '\t') || uc == 0x7F) {
      err = "Content-Type inválido";
      return 0;
    }
  }
  if (r.body.size() > o.maxRequestBody) {
    err = "corpo grande demais (máximo " + fmtBytes(o.maxRequestBody) + ")";
    return 0;
  }
  r.headers = std::move(hs);
  detail::HttpJob j;
  j.reqBytes = r.body.size();
  j.url = std::move(u);
  j.req = std::move(r);
  j.done = std::move(done);
  j.queuedUntil = Clock::now() + o.queueTimeout;
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.stop) {
    err = "HTTP encerrado";
    return 0;
  }
  auto& ow = s.owners[j.req.owner];
  if (ow.pending >= o.maxPendingPerOwner) {
    err = "pedidos HTTP pendentes demais neste resource (máximo " + std::to_string(o.maxPendingPerOwner) + ")";
    if (ow.pending == 0 && ow.q.empty()) s.owners.erase(j.req.owner);
    return 0;
  }
  if (ow.bytes + j.reqBytes > o.maxBytesPerOwner) {
    err = "bytes demais em andamento neste resource (máximo " + fmtBytes(o.maxBytesPerOwner) + " de corpos)";
    if (ow.pending == 0 && ow.q.empty()) s.owners.erase(j.req.owner);
    return 0;
  }
  j.id = s.nextId++;
  const uint64_t id = j.id;
  ++ow.pending;
  ow.bytes += j.reqBytes;
  const uint64_t owner = j.req.owner;
  ow.q.push_back(std::move(j));
  if (!ow.inRr) {
    ow.inRr = true;
    s.rr.push_back(owner);
  }
  ++s.queued;
  s.cv.notify_one();
  return id;
}

void HttpOutPool::cancel(uint64_t id) {
  auto& s = *st_;
  detail::HttpJob gone;  // destruído fora do lock
  std::lock_guard<std::mutex> lk(s.mu);
  for (auto& slot : s.slots)
    if (slot.active && slot.id == id) {
      slot.cancelled = true;  // o watchdog fecha o socket
      s.wdCv.notify_all();
      return;
    }
  for (auto it = s.owners.begin(); it != s.owners.end(); ++it) {
    auto& q = it->second.q;
    for (auto jt = q.begin(); jt != q.end(); ++jt) {
      if (jt->id != id) continue;
      gone = std::move(*jt);
      q.erase(jt);
      --s.queued;
      const uint64_t owner = it->first;
      if (q.empty() && it->second.inRr) {
        it->second.inRr = false;
        s.rr.erase(std::remove(s.rr.begin(), s.rr.end(), owner), s.rr.end());
      }
      s.release(owner, gone.reqBytes);
      return;
    }
  }
}

HttpOutMetrics HttpOutPool::metrics() const {
  const auto& s = *st_;
  HttpOutMetrics m;
  m.requests = s.requests.load();
  m.errors = s.errors.load();
  m.timeouts = s.timeouts.load();
  m.refused = s.refused.load();
  m.busy = s.busy.load();
  m.threads = s.opt.threads;
  std::lock_guard<std::mutex> lk(s.mu);
  m.queued = s.queued;
  return m;
}

const HttpOutOptions& HttpOutPool::options() const { return st_->opt; }
bool HttpOutPool::testMode() const { return st_->test; }

}  // namespace cbscript
