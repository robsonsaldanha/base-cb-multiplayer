#include "cbadmin/admin_server.hpp"

#include <httplib.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

#include <spdlog/spdlog.h>

#include <cbmp/base64url.hpp>
#include <cbmp/safe_path.hpp>
#include <cbmp/ticket.hpp>

#include "cbadmin_web.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/host.hpp"
#include "cbserver/auth.hpp"
#include "cbserver/config.hpp"
#include "cbserver/script_bridge.hpp"

namespace cbadmin {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr size_t kMaxBody = 64 * 1024;
constexpr size_t kMaxLoginBody = 8 * 1024;  // ticket ≤ 4 KiB + state
constexpr size_t kMaxReason = 200;
constexpr size_t kMaxConsoleLine = 512;
constexpr size_t kMaxLogTail = 500;
constexpr size_t kMaxLogBytes = size_t{1} << 20;
constexpr int64_t kMaxBanSeconds = int64_t{10} * 365 * 24 * 3600;

const char* kCsp =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; "
    "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";

std::string dumpSafe(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

void sendJson(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(dumpSafe(j), "application/json; charset=utf-8");
}
void sendError(httplib::Response& res, int status, const std::string& msg) { sendJson(res, {{"error", msg}}, status); }

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string randomToken() {
  std::vector<uint8_t> b(32);
  randombytes_buf(b.data(), b.size());
  return cbmp::base64urlEncode(b);
}

std::array<unsigned char, crypto_hash_sha256_BYTES> sha256(std::string_view s) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> h{};
  crypto_hash_sha256(h.data(), reinterpret_cast<const unsigned char*>(s.data()), s.size());
  return h;
}
std::string hashKey(std::string_view s) {
  const auto h = sha256(s);
  return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}
// Tempo constante, inclusive para tamanhos diferentes (compara os hashes).
bool ctEqual(std::string_view a, std::string_view b) {
  const auto ha = sha256(a), hb = sha256(b);
  const bool same = sodium_memcmp(ha.data(), hb.data(), ha.size()) == 0;
  return same && a.size() == b.size();
}

// Chave de limite por IP: IPv6 agrupado por /64; IPv4 (inclusive mapeado em IPv6) inteiro.
std::string ipKey(const std::string& addr) {
  in6_addr a6{};
  if (inet_pton(AF_INET6, addr.c_str(), &a6) == 1) {
    const auto* b = reinterpret_cast<const unsigned char*>(&a6);
    static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::equal(b, b + 12, kMapped)) return "4:" + std::to_string(b[12]) + "." + std::to_string(b[13]) + "." +
                                                std::to_string(b[14]) + "." + std::to_string(b[15]);
    return "6:" + std::string(reinterpret_cast<const char*>(b), 8);
  }
  return "4:" + addr;
}

// IP literal (v4 ou v6; v4 mapeado em v6 vira v4) → 16 bytes + família.
struct IpAddr {
  bool v6 = false;
  std::array<unsigned char, 16> b{};
};
std::optional<IpAddr> parseIp(const std::string& s) {
  IpAddr a;
  in_addr a4{};
  if (inet_pton(AF_INET, s.c_str(), &a4) == 1) {
    std::memcpy(a.b.data(), &a4, 4);
    return a;
  }
  in6_addr a6{};
  if (inet_pton(AF_INET6, s.c_str(), &a6) != 1) return std::nullopt;
  const auto* p = reinterpret_cast<const unsigned char*>(&a6);
  static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  if (std::equal(p, p + 12, kMapped)) {
    std::memcpy(a.b.data(), p + 12, 4);
    return a;
  }
  a.v6 = true;
  std::memcpy(a.b.data(), p, 16);
  return a;
}

// Rede de proxy confiável: IP ou CIDR (validado na config; aqui inválido = ignorado).
struct IpNet {
  IpAddr base;
  int bits = 0;
  bool contains(const IpAddr& a) const {
    if (a.v6 != base.v6) return false;
    int left = bits;
    for (size_t i = 0; left > 0; ++i, left -= 8) {
      const unsigned char mask = left >= 8 ? 0xff : static_cast<unsigned char>(0xff << (8 - left));
      if ((a.b[i] & mask) != (base.b[i] & mask)) return false;
    }
    return true;
  }
};
std::optional<IpNet> parseNet(const std::string& s) {
  const size_t slash = s.find('/');
  auto ip = parseIp(s.substr(0, slash));
  if (!ip) return std::nullopt;
  const int max = ip->v6 ? 128 : 32;
  int bits = max;
  if (slash != std::string::npos) {
    const std::string b = s.substr(slash + 1);
    if (b.empty() || b.size() > 3 || b.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    bits = std::stoi(b);
    if (bits > max) return std::nullopt;
  }
  return IpNet{*ip, bits};
}

struct Bucket {
  double tokens = 0, last = 0;
  // Recarrega e tenta gastar 1 ficha.
  bool take(double now, double rate, double cap) {
    tokens = std::min(cap, tokens + std::max(0.0, now - last) * rate);
    last = now;
    if (tokens < 1) return false;
    tokens -= 1;
    return true;
  }
  bool full(double now, double rate, double cap) const { return tokens + std::max(0.0, now - last) * rate >= cap; }
  bool has(double now, double rate, double cap) const {
    return std::min(cap, tokens + std::max(0.0, now - last) * rate) >= 1;
  }
};

// Baldes por chave com teto de chaves (cheio: limpa os ociosos; se continuar cheio, nullptr = recusar).
struct KeyedBuckets {
  std::unordered_map<std::string, Bucket> m;
  Bucket* get(const std::string& key, double now, double rate, double cap, size_t maxKeys) {
    auto it = m.find(key);
    if (it != m.end()) return &it->second;
    if (m.size() >= maxKeys) {
      for (auto i = m.begin(); i != m.end();) i = i->second.full(now, rate, cap) ? m.erase(i) : std::next(i);
      if (m.size() >= maxKeys) return nullptr;
    }
    return &m.emplace(key, Bucket{cap, now}).first->second;
  }
};

// Inteiro JSON dentro de [lo, hi] (sem deixar um sem-sinal enorme dar a volta para negativo).
bool intIn(const nlohmann::json& v, int64_t lo, int64_t hi, int64_t& out) {
  if (!v.is_number_integer()) return false;
  if (v.is_number_unsigned()) {
    const auto u = v.get<uint64_t>();
    if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    out = static_cast<int64_t>(u);
  } else {
    out = v.get<int64_t>();
  }
  return out >= lo && out <= hi;
}

struct Session {
  std::string did, name;
  double created = 0, lastSeen = 0;
  uint64_t seq = 0;  // ordem de criação (desempate quando o relógio não andou)
  Bucket api, console;
};

struct SessionView {
  std::string did, name;
};

std::optional<std::string> cookieOf(const httplib::Request& req, std::string_view name) {
  const size_t n = req.get_header_value_count("Cookie");
  for (size_t k = 0; k < n; ++k) {
    // get_header_value devolve std::string por valor: o texto precisa viver enquanto a view existir.
    const std::string hdr = req.get_header_value("Cookie", "", k);
    std::string_view all = hdr;
    while (!all.empty()) {
      size_t semi = all.find(';');
      std::string_view part = all.substr(0, semi);
      while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
      while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
      const size_t eq = part.find('=');
      if (eq != std::string_view::npos && part.substr(0, eq) == name) return std::string(part.substr(eq + 1));
      if (semi == std::string_view::npos) break;
      all.remove_prefix(semi + 1);
    }
  }
  return std::nullopt;
}

const char* stateName(cbscript::ResourceStatus::State s) {
  switch (s) {
    case cbscript::ResourceStatus::State::Running:
      return "running";
    case cbscript::ResourceStatus::State::Failed:
      return "failed";
    default:
      return "stopped";
  }
}

std::vector<std::string> tokens(std::string_view line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    if (j > i) out.emplace_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

// Resto da linha depois dos `n` primeiros tokens.
std::string restAfter(std::string_view line, size_t n) {
  size_t i = 0;
  for (size_t k = 0; k < n; ++k) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
  }
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  std::string r(line.substr(i));
  while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) r.pop_back();
  return r;
}

bool parseId(std::string_view s, uint16_t& id) {
  if (s.empty() || s.size() > 5 || s.find_first_not_of("0123456789") != std::string_view::npos) return false;
  const int v = std::stoi(std::string(s));
  if (v < 1 || v > 65535) return false;
  id = static_cast<uint16_t>(v);
  return true;
}

std::string clip(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  size_t k = n;
  while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) --k;
  return s.substr(0, k) + "…";
}

}  // namespace

// =====================================================================================================================

struct AdminServer::Impl {
  AdminOptions opt;
  AdminBackend be;
  httplib::Server svr;
  std::thread th;
  int port = 0;
  bool https = false;
  std::string publicHost;  // host[:porta] da publicUrl
  std::string sessionCookie, stateCookie;
  std::set<std::string> hosts, origins;

  mutable std::mutex mu;
  std::unordered_map<std::string, Session> sessions;  // chave = sha256 do id
  std::unordered_map<std::string, double> states;      // sha256 do state → vence em
  std::unordered_map<std::string, int64_t> jtis;       // jti → vence em (Unix)
  KeyedBuckets loginFailByIp, startByIp;  // falhas de login / inícios, por IP do cliente
  Bucket loginFailGlobal;
  bool loginGlobalInit = false;
  uint64_t sessionSeq = 0;
  // Logins recusados por limite: nenhuma linha por requisição; um resumo por janela (sai na virada e no stop).
  static constexpr double kRefusedWindowSec = 60;
  static constexpr size_t kRefusedMaxIps = 16;
  double refusedSince = 0;
  uint64_t refusedCount = 0, refusedOtherIps = 0;
  std::map<std::string, uint64_t> refusedByIp;
  std::vector<IpNet> proxies;          // admin.trustedProxies
  std::vector<std::string> pwNeedles;  // senha do banco (crua e escapada em JSON) para o filtro de logs

  Impl(AdminOptions o, AdminBackend b) : opt(std::move(o)), be(std::move(b)) {
    if (sodium_init() < 0) throw std::runtime_error("painel admin: falha ao iniciar libsodium");
    https = opt.publicUrl && opt.publicUrl->rfind("https://", 0) == 0;
    if (opt.publicUrl) publicHost = opt.publicUrl->substr(opt.publicUrl->find("://") + 3);
    sessionCookie = https ? "__Host-cbadmin" : "cbadmin";
    stateCookie = https ? "__Secure-cbadmin_state" : "cbadmin_state";
    while (!opt.hubUrl.empty() && opt.hubUrl.back() == '/') opt.hubUrl.pop_back();
    for (const auto& t : opt.trustedProxies) {
      if (auto n = parseNet(t)) {
        proxies.push_back(*n);
        if (n->bits < (n->base.v6 ? 32 : 8))
          spdlog::warn("painel admin: trustedProxies '{}' é larga demais (quase certamente um engano): qualquer "
                       "conexão dessa faixa decide o IP do cliente pelo X-Forwarded-For",
                       t);
      } else {
        spdlog::warn("painel admin: proxy confiável inválido ignorado: '{}'", t);
      }
    }
    if (opt.dbPassword.size() >= 4) {
      pwNeedles.push_back(opt.dbPassword);
      std::string esc = json(opt.dbPassword).dump(-1, ' ', false, json::error_handler_t::replace);
      esc = esc.substr(1, esc.size() - 2);
      if (esc != opt.dbPassword) pwNeedles.push_back(esc);
    } else if (!opt.dbPassword.empty()) {
      spdlog::warn("painel admin: senha do banco com menos de 4 caracteres; o filtro de logs do painel fica "
                   "desligado (use uma senha forte)");
    }
    if (!be.unixNow)
      be.unixNow = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
      };
    if (!be.monoNow)
      be.monoNow = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
      };
    routes();
  }

  // ------------------------------------------------------------------ utilidades

  double mono() const { return be.monoNow(); }

  // IP do cliente: o do par TCP, ou, se o par for um proxy confiável (admin.trustedProxies), o primeiro endereço não
  // confiável do X-Forwarded-For lido da direita para a esquerda. Entrada inválida → o par.
  std::string clientIp(const httplib::Request& req) const {
    if (proxies.empty()) return req.remote_addr;
    auto trusted = [this](const std::string& ip) {
      auto a = parseIp(ip);
      if (!a) return false;
      for (const auto& n : proxies)
        if (n.contains(*a)) return true;
      return false;
    };
    if (!trusted(req.remote_addr)) return req.remote_addr;
    std::string all;
    const size_t n = req.get_header_value_count("X-Forwarded-For");
    for (size_t k = 0; k < n; ++k) {
      if (!all.empty()) all += ',';
      all += req.get_header_value("X-Forwarded-For", "", k);
    }
    if (all.size() > 4096) return req.remote_addr;
    std::vector<std::string> hops;
    for (size_t b = 0; b <= all.size();) {
      size_t e = all.find(',', b);
      if (e == std::string::npos) e = all.size();
      std::string h = all.substr(b, e - b);
      while (!h.empty() && (h.front() == ' ' || h.front() == '\t')) h.erase(h.begin());
      while (!h.empty() && (h.back() == ' ' || h.back() == '\t')) h.pop_back();
      hops.push_back(std::move(h));
      b = e + 1;
    }
    for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
      const std::string h = hopAddr(*it);
      if (!parseIp(h)) return req.remote_addr;
      if (!trusted(h)) return h;
    }
    return req.remote_addr;
  }

  // Salto do X-Forwarded-For sem porta ("1.2.3.4:5678", "[2001:db8::1]:443") e sem zona ("fe80::1%eth0").
  static std::string hopAddr(std::string h) {
    if (!h.empty() && h.front() == '[') {
      const size_t close = h.find(']');
      h = close == std::string::npos ? std::string() : h.substr(1, close - 1);
    } else if (std::count(h.begin(), h.end(), ':') == 1) {
      h = h.substr(0, h.find(':'));
    }
    if (const size_t pct = h.find('%'); pct != std::string::npos) h.resize(pct);
    return h;
  }

  // ---- logins recusados por limite (agregados)
  void noteRefused(const std::string& ip) {
    std::optional<json> sum;
    {
      std::lock_guard<std::mutex> lk(mu);
      sum = takeRefusedLocked(mono(), false);
      if (refusedCount == 0) refusedSince = mono();
      ++refusedCount;
      if (auto it = refusedByIp.find(ip); it != refusedByIp.end())
        ++it->second;
      else if (refusedByIp.size() < kRefusedMaxIps)
        refusedByIp.emplace(ip, 1);
      else
        ++refusedOtherIps;
    }
    if (sum) writeAudit(std::move(*sum));
  }
  std::optional<json> takeRefusedLocked(double now, bool force) {
    if (refusedCount == 0 || (!force && now - refusedSince < kRefusedWindowSec)) return std::nullopt;
    json ips = json::object();
    for (const auto& [ip, n] : refusedByIp) ips[ip] = n;
    json j{{"type", "admin_login"}, {"result", "rate_limited"}, {"count", refusedCount},
           {"windowS", static_cast<int>(kRefusedWindowSec)}, {"ips", std::move(ips)}};
    if (refusedOtherIps) j["otherIps"] = refusedOtherIps;
    refusedCount = refusedOtherIps = 0;
    refusedByIp.clear();
    return j;
  }
  void flushRefused(bool force) {
    std::optional<json> sum;
    {
      std::lock_guard<std::mutex> lk(mu);
      sum = takeRefusedLocked(mono(), force);
    }
    if (sum) writeAudit(std::move(*sum));
  }
  void writeAudit(json j) {
    j["ts"] = be.unixNow();
    if (be.audit) be.audit(dumpSafe(j));
  }

  void audit(json j, const httplib::Request& req) {
    flushRefused(false);  // resumo de uma janela já vencida sai antes, na ordem
    j["ip"] = clientIp(req);
    writeAudit(std::move(j));
  }
  void auditAction(const SessionView& s, const httplib::Request& req, const std::string& action, json extra) {
    json j{{"type", "admin"}, {"action", action}, {"did", s.did}};
    for (auto it = extra.begin(); it != extra.end(); ++it) j[it.key()] = it.value();
    audit(std::move(j), req);
  }

  std::string cookieAttrs(const char* path, int maxAge) const {
    std::string a = std::string("; Path=") + path + "; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(maxAge);
    if (https) a += "; Secure";
    return a;
  }

  // Login: só falhas gastam o orçamento (por IP do cliente, /64 no IPv6, e global). Sucessos nunca gastam, e um par
  // ticket + state válido entra mesmo com o orçamento esgotado (tickets não se forjam e o state é de uso único: o que
  // se pode adivinhar é só o devToken, que fica bloqueado sem orçamento).
  double perIpRate() const { return std::max(1, opt.loginPerIpPerMin) / 60.0; }
  double globalRate() const { return std::max(1, opt.loginGlobalPerMin) / 60.0; }
  void initGlobalLocked(double now) {
    if (!loginGlobalInit) {
      loginFailGlobal = {static_cast<double>(std::max(1, opt.loginGlobalPerMin)), now};
      loginGlobalInit = true;
    }
  }
  // Devolve uma falha cobrada antes da hora (devToken certo).
  void refundFailure(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu);
    const double cap = std::max(1, opt.loginPerIpPerMin);
    if (auto it = loginFailByIp.m.find(ipKey(ip)); it != loginFailByIp.m.end())
      it->second.tokens = std::min(cap, it->second.tokens + 1);
    loginFailGlobal.tokens = std::min<double>(std::max(1, opt.loginGlobalPerMin), loginFailGlobal.tokens + 1);
  }
  // Gasta uma falha; false = sem orçamento (responder 429).
  bool chargeFailure(const std::string& ip) {
    const double now = mono();
    std::lock_guard<std::mutex> lk(mu);
    initGlobalLocked(now);
    const double cap = std::max(1, opt.loginPerIpPerMin);
    Bucket* b = loginFailByIp.get(ipKey(ip), now, perIpRate(), cap, opt.maxRateKeys);
    if (!b || !b->take(now, perIpRate(), cap)) return false;
    return loginFailGlobal.take(now, globalRate(), std::max(1, opt.loginGlobalPerMin));
  }
  // /auth/start: barato, limite próprio só por IP (o teto de states pendentes segura a memória).
  bool startAllowed(const std::string& ip) {
    const double now = mono();
    const double cap = std::max(1, opt.startPerIpPerMin);
    std::lock_guard<std::mutex> lk(mu);
    Bucket* b = startByIp.get(ipKey(ip), now, cap / 60, cap, opt.maxRateKeys);
    return b && b->take(now, cap / 60, cap);
  }

  // Mutação, só cabeçalhos (barato, antes de autenticar e de ler o JSON): Origin do painel, X-CB-Admin: 1,
  // Content-Type JSON e tamanho. false = resposta já escrita.
  bool guardHeaders(const httplib::Request& req, httplib::Response& res, size_t maxBody = kMaxBody) {
    const std::string origin = req.get_header_value("Origin");
    if (origin.empty() || !origins.count(lower(origin))) {
      sendError(res, 403, "origem não permitida");
      return false;
    }
    if (req.get_header_value("X-CB-Admin") != "1") {
      sendError(res, 403, "cabeçalho X-CB-Admin ausente");
      return false;
    }
    const std::string ct = lower(req.get_header_value("Content-Type"));
    if (!(ct == "application/json" || ct.rfind("application/json;", 0) == 0)) {
      sendError(res, 415, "use Content-Type: application/json");
      return false;
    }
    if (req.body.size() > maxBody) {
      sendError(res, 413, "corpo grande demais");
      return false;
    }
    return true;
  }
  static std::optional<json> parseBody(const httplib::Request& req) {
    auto j = json::parse(req.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
  }
  // Mutação autenticada: cabeçalhos → sessão → corpo.
  std::optional<std::pair<SessionView, json>> mutation(const httplib::Request& req, httplib::Response& res,
                                                       bool console = false) {
    if (!guardHeaders(req, res)) return std::nullopt;
    auto s = auth(req, res, console);
    if (!s) return std::nullopt;
    auto j = parseBody(req);
    if (!j) {
      sendError(res, 400, "corpo deve ser um objeto JSON");
      return std::nullopt;
    }
    return std::make_pair(std::move(*s), std::move(*j));
  }

  void purgeSessionsLocked(double now) {
    for (auto it = sessions.begin(); it != sessions.end();)
      it = (now - it->second.lastSeen > opt.sessionIdleSec || now - it->second.created > opt.sessionAbsoluteSec)
               ? sessions.erase(it)
               : std::next(it);
  }

  // Sessão válida (renova o "visto por último") + limite da API. Resposta escrita em caso de falha.
  std::optional<SessionView> auth(const httplib::Request& req, httplib::Response& res, bool console = false) {
    auto id = cookieOf(req, sessionCookie);
    const double now = mono();
    std::lock_guard<std::mutex> lk(mu);
    auto it = id && !id->empty() && id->size() <= 128 ? sessions.find(hashKey(*id)) : sessions.end();
    if (it == sessions.end()) {
      sendError(res, 401, "não autenticado");
      return std::nullopt;
    }
    Session& s = it->second;
    if (now - s.lastSeen > opt.sessionIdleSec || now - s.created > opt.sessionAbsoluteSec) {
      sessions.erase(it);
      sendError(res, 401, "sessão expirada");
      return std::nullopt;
    }
    s.lastSeen = now;
    if (!s.api.take(now, opt.apiPerSec, opt.apiBurst) ||
        (console && !s.console.take(now, opt.consolePerSec, opt.consoleBurst))) {
      sendError(res, 429, "muitas requisições");
      return std::nullopt;
    }
    return SessionView{s.did, s.name};
  }

  void createSession(httplib::Response& res, const std::string& did, const std::string& name) {
    const std::string id = randomToken();
    const double now = mono();
    {
      std::lock_guard<std::mutex> lk(mu);
      purgeSessionsLocked(now);
      // No máximo maxSessionsPerDid por did: sai a mais antiga.
      for (;;) {
        size_t n = 0;
        auto oldest = sessions.end();
        for (auto it = sessions.begin(); it != sessions.end(); ++it)
          if (it->second.did == did) {
            ++n;
            if (oldest == sessions.end() || it->second.seq < oldest->second.seq) oldest = it;
          }
        if (n < std::max<size_t>(1, opt.maxSessionsPerDid)) break;
        sessions.erase(oldest);
      }
      if (sessions.size() >= std::max<size_t>(1, opt.maxSessions)) {
        auto oldest = std::min_element(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
          return a.second.lastSeen < b.second.lastSeen ||
                 (a.second.lastSeen == b.second.lastSeen && a.second.seq < b.second.seq);
        });
        sessions.erase(oldest);
      }
      Session s;
      s.did = did;
      s.name = name;
      s.created = s.lastSeen = now;
      s.seq = ++sessionSeq;
      s.api = {opt.apiBurst, now};
      s.console = {opt.consoleBurst, now};
      sessions[hashKey(id)] = std::move(s);
    }
    res.set_header("Set-Cookie", sessionCookie + "=" + id +
                                     cookieAttrs("/", static_cast<int>(opt.sessionAbsoluteSec)));
  }

  cbscript::PlayersSnapshot players() const {
    if (!be.players) return std::make_shared<const std::vector<cbscript::PlayerView>>();
    auto p = be.players();
    return p ? p : std::make_shared<const std::vector<cbscript::PlayerView>>();
  }
  std::optional<cbscript::PlayerView> player(uint16_t id) const {
    auto snap = players();
    for (const auto& p : *snap)
      if (p.id == id) return p;
    return std::nullopt;
  }

  // ------------------------------------------------------------------ login

  void handleStart(const httplib::Request& req, httplib::Response& res) {
    if (!startAllowed(clientIp(req))) return sendError(res, 429, "muitos inícios de login; espere um minuto");
    if (opt.hubUrl.empty()) return sendError(res, 409, "login via Hub indisponível: servidor sem 'hub' (modo dev)");
    if (opt.publicUrl && lower(req.get_header_value("Host")) != publicHost) {
      // O Hub volta para a publicUrl: o cookie de state precisa nascer nela.
      res.status = 302;
      res.set_header("Location", *opt.publicUrl + "/auth/start");
      return;
    }
    const std::string st = randomToken();
    const double now = mono();
    {
      std::lock_guard<std::mutex> lk(mu);
      for (auto it = states.begin(); it != states.end();) it = it->second < now ? states.erase(it) : std::next(it);
      if (states.size() >= std::max<size_t>(1, opt.maxPendingStates)) {
        auto oldest = std::min_element(states.begin(), states.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
        states.erase(oldest);
      }
      states[hashKey(st)] = now + opt.stateTtlSec;
    }
    res.set_header("Set-Cookie", stateCookie + "=" + st + cookieAttrs("/auth", static_cast<int>(opt.stateTtlSec)));
    res.status = 302;
    res.set_header("Location", opt.hubUrl + "/v1/auth/server-admin?serverId=" + opt.serverId + "&state=" + st);
  }

  void handleLogin(const httplib::Request& req, httplib::Response& res) {
    // Recusas do guard (cabeçalhos/tamanho) não gastam orçamento: não provam nada e não chegam a validar nada.
    if (!guardHeaders(req, res, kMaxLoginBody)) return;
    const std::string ip = clientIp(req);
    auto fail = [&](int status, const std::string& why, const std::string& msg, const std::string& did = "") {
      json a{{"type", "admin_login"}, {"result", why}};
      if (!did.empty()) a["did"] = did;
      if (!chargeFailure(ip)) {
        // Sem orçamento: nada por requisição (senão qualquer um enche o disco); só o resumo da janela.
        noteRefused(ip);
        return sendError(res, 429, "muitas tentativas de login falhas; espere um minuto");
      }
      audit(std::move(a), req);
      sendError(res, status, msg);
    };
    auto body = parseBody(req);
    if (!body) return fail(400, "bad_request", "corpo deve ser um objeto JSON");

    if (body->contains("devToken")) {
      // Único segredo adivinhável: a tentativa é cobrada ANTES de comparar (atômico; sem corrida entre checar e
      // cobrar) e devolvida se o token estiver certo. Sem orçamento, nem compara.
      if (!chargeFailure(ip)) {
        noteRefused(ip);
        return sendError(res, 429, "muitas tentativas de login falhas; espere um minuto");
      }
      const auto& t = (*body)["devToken"];
      auto charged = [&](int status, const std::string& why, const std::string& msg) {
        audit({{"type", "admin_login"}, {"result", why}}, req);
        sendError(res, status, msg);
      };
      if (!t.is_string()) return charged(400, "bad_request", "devToken deve ser texto");
      if (!opt.devToken || !ctEqual(t.get_ref<const std::string&>(), *opt.devToken))
        return charged(401, "bad_dev_token", "token inválido");
      refundFailure(ip);
      createSession(res, "dev", "dev");
      audit({{"type", "admin_login"}, {"result", "ok"}, {"did", "dev"}, {"method", "devToken"}}, req);
      return sendJson(res, {{"did", "dev"}, {"name", "dev"}});
    }

    // Ticket só no modo Hub (em dev qualquer um com a seed dev cunharia tickets #admin).
    if (opt.hubUrl.empty()) return fail(409, "no_hub", "login por ticket exige o Hub (servidor em modo dev)");

    // state: cookie == corpo (tempo constante), emitido por /auth/start, vivo e de uso único. O cookie sai sempre.
    res.set_header("Set-Cookie", stateCookie + "=" + cookieAttrs("/auth", 0));
    const auto stv = body->value("state", json());
    const std::string st = stv.is_string() ? stv.get<std::string>() : std::string();
    const auto ck = cookieOf(req, stateCookie).value_or("");
    if (st.empty() || ck.empty() || st.size() > 128 || !ctEqual(st, ck))
      return fail(403, "bad_state", "login não iniciado neste navegador (state); comece de novo");
    if (!consumeState(st)) return fail(403, "bad_state", "login vencido ou já usado; comece de novo");

    const auto tkv = body->value("ticket", json());
    if (!tkv.is_string()) return fail(400, "bad_request", "ticket ausente");
    if (!be.keys) return fail(503, "no_keys", "chaves indisponíveis");
    const int64_t now = be.unixNow();
    auto r = cbmp::verifyTicket(tkv.get_ref<const std::string&>(), be.keys->keys(), opt.serverId + "#admin", now);
    if (!r) {
      if (r.error() == cbmp::TicketError::UnknownKid) be.keys->requestRefresh();
      return fail(401, "bad_ticket", "ticket inválido (" + std::string(cbmp::toString(r.error())) + ")");
    }
    switch (consumeJti(r->jti, r->exp, now)) {
      case JtiResult::Replay:
        return fail(401, "replay", "ticket já usado; entre de novo", r->did);
      case JtiResult::Full:
        return fail(503, "busy", "muitos logins recentes; tente em alguns minutos", r->did);
      case JtiResult::Ok:
        break;
    }
    if (std::find(opt.admins.begin(), opt.admins.end(), r->did) == opt.admins.end())
      return fail(403, "not_admin", "esta conta Discord não é admin deste servidor", r->did);
    createSession(res, r->did, r->name);
    audit({{"type", "admin_login"}, {"result", "ok"}, {"did", r->did}, {"name", r->name}}, req);
    sendJson(res, {{"did", r->did}, {"name", r->name}});
  }

  // State emitido por /auth/start, ainda vivo; sai do mapa em qualquer caso (uso único).
  bool consumeState(const std::string& st) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = states.find(hashKey(st));
    if (it == states.end()) return false;
    const bool alive = it->second >= mono();
    states.erase(it);
    return alive;
  }

  // jti de admin: guardado até exp + folga do relógio (≥ TTL do ticket). Teto de memória: cheio → recusa.
  enum class JtiResult { Ok, Replay, Full };
  JtiResult consumeJti(const std::string& jti, int64_t exp, int64_t now) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = jtis.begin(); it != jtis.end();) it = it->second < now ? jtis.erase(it) : std::next(it);
    if (jtis.count(jti)) return JtiResult::Replay;
    if (jtis.size() >= std::max<size_t>(1, opt.maxJti)) return JtiResult::Full;
    jtis.emplace(jti, exp + cbmp::kClockSkewSeconds);
    return JtiResult::Ok;
  }

  void handleLogout(const httplib::Request& req, httplib::Response& res) {
    if (!guardHeaders(req, res)) return;
    std::string did;
    if (auto id = cookieOf(req, sessionCookie); id && !id->empty() && id->size() <= 128) {
      std::lock_guard<std::mutex> lk(mu);
      auto it = sessions.find(hashKey(*id));
      if (it != sessions.end()) {
        did = it->second.did;
        sessions.erase(it);
      }
    }
    res.set_header("Set-Cookie", sessionCookie + "=" + cookieAttrs("/", 0));
    if (!did.empty()) audit({{"type", "admin_logout"}, {"did", did}}, req);
    sendJson(res, {{"ok", true}});
  }

  // ------------------------------------------------------------------ jogadores

  void handlePlayerAction(const httplib::Request& req, httplib::Response& res) {
    auto m = mutation(req, res);
    if (!m) return;
    const SessionView& s = m->first;
    const json& body = m->second;
    const std::string action = req.matches[2].str();
    // Toda recusa também vai para a auditoria.
    auto reject = [&](int status, const std::string& result, const std::string& msg, json extra = json::object()) {
      extra["result"] = result;
      auditAction(s, req, action, std::move(extra));
      sendError(res, status, msg);
    };
    uint16_t id = 0;
    std::optional<cbscript::PlayerView> p;
    if (parseId(req.matches[1].str(), id)) p = player(id);
    if (!p) return reject(404, "not_found", "jogador não encontrado", {{"target", {{"id", req.matches[1].str()}}}});
    const json target{{"id", p->id}, {"did", p->did}, {"name", p->name}};

    auto reasonOf = [&](const char* def) -> std::optional<std::string> {
      auto r = body.value("reason", json());
      if (r.is_null()) return std::string(def);
      if (!r.is_string() || r.get_ref<const std::string&>().size() > kMaxReason) return std::nullopt;
      auto v = r.get<std::string>();
      return v.empty() ? std::string(def) : v;
    };

    if (action == "kick") {
      auto reason = reasonOf("expulso por um admin");
      if (!reason) return reject(400, "invalid", "reason deve ser texto de até 200 bytes", {{"target", target}});
      if (!be.simCommand || !be.simCommand(cbscript::CmdKick{p->id, *reason, p->session}))
        return reject(503, "unavailable", "fila da simulação cheia", {{"target", target}});
      auditAction(s, req, "kick", {{"target", target}, {"reason", *reason}, {"result", "accepted"}});
      return sendJson(res, {{"accepted", true}}, 202);
    }
    if (action == "ban") {
      auto reason = reasonOf("banido por um admin");
      if (!reason) return reject(400, "invalid", "reason deve ser texto de até 200 bytes", {{"target", target}});
      int64_t dur = 0;
      if (auto d = body.value("durationS", json()); !d.is_null() && !intIn(d, 0, kMaxBanSeconds, dur))
        return reject(400, "invalid", "durationS deve ser inteiro de 0 (permanente) a 10 anos", {{"target", target}});
      if (p->did.empty() || p->did.size() > 64)
        return reject(400, "invalid", "jogador sem did válido", {{"target", target}});
      const json info{{"target", target}, {"reason", *reason}, {"durationS", dur}};
      auto out = runBan(p->did, *reason, dur, "admin:" + s.did);
      json a = info;
      a["result"] = banResult(out);
      auditAction(s, req, "ban", std::move(a));
      if (!out) return sendJson(res, {{"pending", true}}, 202);
      if (*out == BanOutcome::Failed)
        return sendError(res, 503, "ban não aplicado (host de scripts parado; ver log). O jogador foi expulso.");
      if (*out == BanOutcome::MemoryOnly)
        return sendJson(res, {{"ok", true},
                              {"persisted", false},
                              {"warning", "ban valendo, mas não gravado (falha no arquivo/banco; ver log): some no "
                                          "próximo restart"}});
      return sendJson(res, {{"ok", true}, {"persisted", true}});
    }
    // teleport
    int32_t xyz[3] = {};
    const char* keys[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
      int64_t v = 0;
      if (!intIn(body.value(keys[i], json()), std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(),
                 v))
        return reject(400, "invalid", "x, y e z devem ser inteiros (cm)", {{"target", target}});
      xyz[i] = static_cast<int32_t>(v);
    }
    if (!be.simCommand || !be.simCommand(cbscript::CmdSetPosition{p->id, xyz[0], xyz[1], xyz[2], p->session}))
      return reject(503, "unavailable", "fila da simulação cheia", {{"target", target}});
    auditAction(s, req, "teleport",
                {{"target", target}, {"pos", {xyz[0], xyz[1], xyz[2]}}, {"result", "accepted"}});
    sendJson(res, {{"accepted", true}}, 202);
  }

  // Ban: o backend grava pela thread de scripts e confirma pelo future. nullopt = ainda em andamento (202).
  std::optional<BanOutcome> runBan(const std::string& did, const std::string& reason, int64_t dur,
                                   const std::string& by) {
    if (!be.ban) return BanOutcome::Failed;
    auto f = be.ban(did, reason, dur, by);
    if (!f.valid()) return BanOutcome::Failed;
    if (f.wait_for(opt.actionWait) != std::future_status::ready) return std::nullopt;
    try {
      return f.get();
    } catch (const std::future_error&) {  // tarefa descartada (host parou): promessa quebrada
      return BanOutcome::Failed;
    }
  }
  static const char* banResult(const std::optional<BanOutcome>& o) {
    if (!o) return "pending";
    switch (*o) {
      case BanOutcome::Saved:
        return "ok";
      case BanOutcome::MemoryOnly:
        return "memory_only";
      default:
        return "failed";
    }
  }

  // ------------------------------------------------------------------ resources

  // Espera a operação até actionWait; nullopt = ainda em andamento.
  std::optional<bool> runOp(ResourceOp op, const std::string& name) {
    if (!be.resourceOp) return false;
    auto f = be.resourceOp(op, name);
    if (!f.valid()) return false;
    if (f.wait_for(opt.actionWait) != std::future_status::ready) return std::nullopt;
    return f.get();
  }

  void handleResourceAction(const httplib::Request& req, httplib::Response& res) {
    if (!guardHeaders(req, res)) return;
    auto s = auth(req, res);
    if (!s) return;
    const std::string name = req.matches[1].str(), op = req.matches[2].str();
    if (name.size() > 64 || !cbmp::safeSegment(name)) return sendError(res, 400, "nome de resource inválido");
    const ResourceOp o = op == "start" ? ResourceOp::Start : op == "stop" ? ResourceOp::Stop : ResourceOp::Restart;
    auto r = runOp(o, name);
    auditAction(*s, req, "resource",
                {{"op", op}, {"name", name}, {"result", !r ? "pending" : *r ? "ok" : "failed"}});
    if (!r) return sendJson(res, {{"pending", true}}, 202);
    sendJson(res, {{"ok", *r}});
  }

  // ------------------------------------------------------------------ console

  std::string consoleStatus() {
    std::string out;
    const auto snap = players();
    out += "jogadores: " + std::to_string(snap->size());
    if (be.metrics) {
      auto m = be.metrics();
      if (m.contains("tickP99Ms") && m["tickP99Ms"].is_number()) {
        char b[64];
        std::snprintf(b, sizeof b, "; tick p99 %.2f ms", m["tickP99Ms"].get<double>());
        out += b;
      }
    }
    if (be.resources) {
      out += "\nresources:";
      for (const auto& r : be.resources()) out += " " + r.name + "(" + stateName(r.state) + ")";
    }
    return out;
  }

  // nullopt = comando não é interno.
  std::optional<std::string> internalCommand(const SessionView& s, const httplib::Request& req,
                                             const std::vector<std::string>& tok, std::string_view line) {
    const std::string& c = tok[0];
    if (c == "help")
      return std::string("comandos: status, players, restart <resource>, kick <id> [motivo], ban <id> [motivo], ") +
             "help; e os comandos dos resources registrados com admin = true";
    if (c == "status") return consoleStatus();
    if (c == "players") {
      const auto snap = players();
      std::string out = std::to_string(snap->size()) + " jogador(es)";
      for (const auto& p : *snap) out += "\n" + std::to_string(p.id) + "  " + p.name + "  " + p.did;
      return out;
    }
    if (c == "restart") {
      if (tok.size() != 2) return std::string("uso: restart <resource>");
      if (tok[1].size() > 64 || !cbmp::safeSegment(tok[1])) return std::string("nome de resource inválido");
      auto r = runOp(ResourceOp::Restart, tok[1]);
      auditAction(s, req, "resource",
                  {{"op", "restart"}, {"name", tok[1]}, {"result", !r ? "pending" : *r ? "ok" : "failed"}});
      if (!r) return "resource '" + tok[1] + "': reinício em andamento";
      return "resource '" + tok[1] + "': " + (*r ? "reiniciado" : "falhou (ver status/log)");
    }
    if (c == "kick" || c == "ban") {
      uint16_t id = 0;
      if (tok.size() < 2 || !parseId(tok[1], id)) return "uso: " + c + " <id> [motivo]";
      auto p = player(id);
      if (!p) return "jogador " + tok[1] + " não encontrado";
      std::string reason = restAfter(line, 2);
      if (reason.size() > kMaxReason) return std::string("motivo grande demais (até 200 bytes)");
      const json target{{"id", p->id}, {"did", p->did}, {"name", p->name}};
      if (c == "kick") {
        if (reason.empty()) reason = "expulso por um admin";
        if (!be.simCommand || !be.simCommand(cbscript::CmdKick{p->id, reason, p->session}))
          return std::string("fila da simulação cheia");
        auditAction(s, req, "kick", {{"target", target}, {"reason", reason}, {"result", "accepted"}});
        return "expulsando " + p->name + " (" + std::to_string(p->id) + ")";
      }
      if (reason.empty()) reason = "banido por um admin";
      if (p->did.empty() || p->did.size() > 64) return std::string("jogador sem did válido");
      auto out = runBan(p->did, reason, 0, "admin:" + s.did);
      auditAction(s, req, "ban",
                  {{"target", target}, {"reason", reason}, {"durationS", 0}, {"result", banResult(out)}});
      if (!out) return "ban de " + p->name + " em andamento";
      if (*out == BanOutcome::Failed) return std::string("ban não aplicado (host de scripts parado; ver log)");
      if (*out == BanOutcome::MemoryOnly)
        return "banido: " + p->name + " (" + p->did + "), mas NÃO gravado (some no restart; ver log)";
      return "banido: " + p->name + " (" + p->did + ")";
    }
    return std::nullopt;
  }

  void handleConsole(const httplib::Request& req, httplib::Response& res) {
    auto m = mutation(req, res, true);
    if (!m) return;
    auto s = std::optional<SessionView>(std::move(m->first));
    auto lv = m->second.value("line", json());
    if (!lv.is_string()) return sendError(res, 400, "line deve ser texto");
    const std::string line = lv.get<std::string>();
    if (line.size() > kMaxConsoleLine) return sendError(res, 400, "linha grande demais (até 512 bytes)");
    for (char ch : line)
      if ((static_cast<unsigned char>(ch) < 0x20 && ch != '\t') || ch == 0x7f)
        return sendError(res, 400, "linha com caracteres de controle");
    std::string_view cmdLine = line;
    while (!cmdLine.empty() && (cmdLine.front() == ' ' || cmdLine.front() == '\t')) cmdLine.remove_prefix(1);
    if (!cmdLine.empty() && cmdLine.front() == '/') cmdLine.remove_prefix(1);
    const auto tok = tokens(cmdLine);
    if (tok.empty()) return sendError(res, 400, "linha vazia");

    if (auto out = internalCommand(*s, req, tok, cmdLine)) {
      auditAction(*s, req, "console", {{"line", line}, {"result", clip(*out, 500)}});
      return sendJson(res, {{"reply", *out}, {"pending", false}});
    }
    // Comando de resource: vai à thread de scripts como console (sem jogador), com o did do admin.
    struct Waiter {
      std::mutex m;
      std::condition_variable cv;
      std::optional<std::string> text;
    };
    auto w = std::make_shared<Waiter>();
    cbscript::EvCommand ev;
    ev.adminDid = s->did;
    ev.adminOnly = true;  // o console do painel só roda comandos de admin
    ev.line = line;
    ev.reply = [w](std::string t) {
      std::lock_guard<std::mutex> lk(w->m);
      if (!w->text) w->text = std::move(t);
      w->cv.notify_all();
    };
    if (!be.scriptCommand || !be.scriptCommand(std::move(ev))) {
      auditAction(*s, req, "console", {{"line", line}, {"result", "unavailable"}});
      return sendError(res, 503, "host de scripts indisponível");
    }
    std::optional<std::string> reply;
    {
      std::unique_lock<std::mutex> lk(w->m);
      w->cv.wait_for(lk, opt.consoleWait, [&] { return w->text.has_value(); });
      reply = w->text;
    }
    auditAction(*s, req, "console", {{"line", line}, {"result", reply ? clip(*reply, 500) : "pending"}});
    sendJson(res, {{"reply", reply ? json(*reply) : json()}, {"pending", !reply}});
  }

  // ------------------------------------------------------------------ logs

  void handleLogs(const httplib::Request& req, httplib::Response& res) {
    auto s = auth(req, res);
    if (!s) return;
    const std::string kind = req.get_param_value("kind");
    if (kind != "audit" && kind != "anticheat") return sendError(res, 400, "kind deve ser 'audit' ou 'anticheat'");
    size_t tail = 200;
    if (req.has_param("tail")) {
      const std::string t = req.get_param_value("tail");
      if (t.empty() || t.size() > 7 || t.find_first_not_of("0123456789") != std::string::npos)
        return sendError(res, 400, "tail deve ser um inteiro");
      tail = std::clamp<size_t>(static_cast<size_t>(std::stoul(t)), 1, kMaxLogTail);
    }
    // Só arquivos fixos: <kind>-AAAA-MM-DD.jsonl direto em logsDir (o mais recente pelo nome).
    std::string best;
    std::error_code ec;
    for (fs::directory_iterator it(opt.logsDir, ec), end; !ec && it != end; it.increment(ec)) {
      std::string n;
      try {
        const auto u8 = it->path().filename().u8string();
        n.assign(u8.begin(), u8.end());
      } catch (const std::exception&) {
        continue;  // nome não representável: não é um dos nossos
      }
      const std::string pre = kind + "-";
      if (n.size() != pre.size() + 16 || n.rfind(pre, 0) != 0 || n.substr(n.size() - 6) != ".jsonl") continue;
      const std::string d = n.substr(pre.size(), 10);
      bool okDate = true;
      for (size_t i = 0; i < d.size(); ++i)
        okDate = okDate && ((i == 4 || i == 7) ? d[i] == '-' : (d[i] >= '0' && d[i] <= '9'));
      std::error_code e2;
      if (!okDate || it->is_symlink(e2) || !it->is_regular_file(e2)) continue;
      if (n > best) best = n;
    }
    json lines = json::array();
    if (best.empty()) return sendJson(res, {{"file", nullptr}, {"lines", lines}});
    std::ifstream f(opt.logsDir / best, std::ios::binary);
    if (!f) return sendJson(res, {{"file", best}, {"lines", lines}});
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(std::max<std::streamoff>(0, f.tellg()));
    const size_t from = size > kMaxLogBytes ? size - kMaxLogBytes : 0;
    std::string buf(size - from, '\0');
    f.seekg(static_cast<std::streamoff>(from));
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<size_t>(f.gcount()));
    std::string_view v = buf;
    if (from > 0) {  // primeira linha cortada no meio
      const size_t nl = v.find('\n');
      v = nl == std::string_view::npos ? std::string_view() : v.substr(nl + 1);
    }
    std::vector<std::string_view> all;
    while (!v.empty()) {
      const size_t nl = v.find('\n');
      std::string_view l = v.substr(0, nl);
      if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
      if (!l.empty()) all.push_back(l);
      if (nl == std::string_view::npos) break;
      v.remove_prefix(nl + 1);
    }
    const size_t start = all.size() > tail ? all.size() - tail : 0;
    for (size_t i = start; i < all.size(); ++i) {
      bool leak = false;
      for (const auto& n : pwNeedles) leak = leak || all[i].find(n) != std::string_view::npos;
      if (leak) continue;
      lines.push_back(std::string(all[i]));
    }
    sendJson(res, {{"file", best}, {"lines", lines}});
  }

  // ------------------------------------------------------------------ rotas

  void serveFile(httplib::Response& res, std::string_view path) {
    for (const auto& f : web::kFiles)
      if (f.path == path) {
        res.status = 200;
        res.set_content(std::string(f.body()), std::string(f.mime));
        return;
      }
    sendError(res, 404, "não encontrado");
  }

  void routes() {
    svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
      // DNS rebinding: só os Hosts do painel, em toda rota. X-Forwarded-* nunca é usado.
      if (!hosts.count(lower(req.get_header_value("Host")))) {
        sendError(res, 421, "host não permitido");
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
      res.set_header("Content-Security-Policy", kCsp);
      res.set_header("X-Frame-Options", "DENY");
      res.set_header("Referrer-Policy", "no-referrer");
      res.set_header("Cache-Control", "no-store");
      res.set_header("X-Content-Type-Options", "nosniff");
      res.set_header("Cross-Origin-Opener-Policy", "same-origin");
      res.set_header("Cross-Origin-Resource-Policy", "same-origin");
    });
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
      if (res.body.empty()) sendError(res, res.status, res.status == 413 ? "corpo grande demais" : "não encontrado");
      return httplib::Server::HandlerResponse::Handled;
    });
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
      sendError(res, 500, "erro interno");
    });

    auto page = [this](const httplib::Request&, httplib::Response& res) { serveFile(res, "/index.html"); };
    svr.Get("/", page);
    svr.Get("/index.html", page);
    svr.Get("/auth/callback", page);
    svr.Get("/app.js", [this](const httplib::Request&, httplib::Response& res) { serveFile(res, "/app.js"); });
    svr.Get("/app.css", [this](const httplib::Request&, httplib::Response& res) { serveFile(res, "/app.css"); });

    svr.Get("/auth/mode", [this](const httplib::Request&, httplib::Response& res) {
      sendJson(res, {{"hub", !opt.hubUrl.empty() && opt.publicUrl.has_value()},
                     {"hubConfigured", !opt.hubUrl.empty()},
                     {"publicUrl", opt.publicUrl ? json(*opt.publicUrl) : json()},
                     {"dev", opt.devToken.has_value()},
                     {"serverId", opt.serverId}});
    });
    svr.Get("/auth/start", [this](const httplib::Request& req, httplib::Response& res) { handleStart(req, res); });
    svr.Post("/auth/login", [this](const httplib::Request& req, httplib::Response& res) { handleLogin(req, res); });
    svr.Post("/auth/logout", [this](const httplib::Request& req, httplib::Response& res) { handleLogout(req, res); });

    svr.Get("/api/me", [this](const httplib::Request& req, httplib::Response& res) {
      auto s = auth(req, res);
      if (!s) return;
      sendJson(res, {{"did", s->did}, {"name", s->name}, {"serverId", opt.serverId}});
    });
    svr.Get("/api/players", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth(req, res)) return;
      json out = json::array();
      for (const auto& p : *players())
        out.push_back({{"id", p.id}, {"name", p.name}, {"did", p.did}, {"x", p.x}, {"y", p.y}, {"z", p.z},
                       {"state", p.state}, {"session", p.session}});
      sendJson(res, out);
    });
    svr.Post(R"(/api/players/([0-9]{1,5})/(kick|ban|teleport))",
             [this](const httplib::Request& req, httplib::Response& res) { handlePlayerAction(req, res); });
    svr.Get("/api/resources", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth(req, res)) return;
      json out = json::array();
      if (be.resources)
        for (const auto& r : be.resources())
          out.push_back({{"name", r.name}, {"state", stateName(r.state)}, {"lastError", r.lastError},
                         {"cpuMsPerSec", r.cpuMsPerSec}, {"memBytes", r.memBytes}});
      sendJson(res, out);
    });
    svr.Post(R"(/api/resources/([^/]{1,64})/(start|stop|restart))",
             [this](const httplib::Request& req, httplib::Response& res) { handleResourceAction(req, res); });
    svr.Post("/api/console", [this](const httplib::Request& req, httplib::Response& res) { handleConsole(req, res); });
    svr.Get("/api/logs", [this](const httplib::Request& req, httplib::Response& res) { handleLogs(req, res); });
    svr.Get("/api/metrics", [this](const httplib::Request& req, httplib::Response& res) {
      if (!auth(req, res)) return;
      sendJson(res, be.metrics ? be.metrics() : json::object());
    });
  }

  void start() {
    if (th.joinable()) throw std::runtime_error("painel admin já iniciado");
    const size_t n = static_cast<size_t>(std::clamp(opt.threads, 1, 64));
    svr.new_task_queue = [n] { return new httplib::ThreadPool(n, n, 32); };
    svr.set_payload_max_length(kMaxBody);
    svr.set_read_timeout(10, 0);
    svr.set_write_timeout(10, 0);
    svr.set_keep_alive_timeout(2);  // conexões ociosas seguram um worker do httplib
    svr.set_keep_alive_max_count(50);
    port = opt.port == 0 ? svr.bind_to_any_port(opt.bind) : (svr.bind_to_port(opt.bind, opt.port) ? opt.port : -1);
    if (port <= 0)
      throw std::runtime_error("painel admin: não foi possível abrir " + opt.bind + ":" + std::to_string(opt.port));

    // Hosts/origens aceitos (antes de aceitar conexões).
    if (opt.publicUrl) {
      hosts.insert(publicHost);
      origins.insert(*opt.publicUrl);
    }
    if (cbserver::isLoopbackBind(opt.bind)) {
      const std::string p = std::to_string(port);
      const std::string bindHost = opt.bind.find(':') != std::string::npos ? "[" + lower(opt.bind) + "]" : opt.bind;
      for (const std::string& h : {bindHost + ":" + p, "127.0.0.1:" + p, "localhost:" + p, "[::1]:" + p}) {
        hosts.insert(h);
        origins.insert("http://" + h);
      }
    }
    if (!cbserver::isLoopbackBind(opt.bind) && !https)
      spdlog::warn("painel admin: escutando em {} (fora do loopback) sem admin.publicUrl https: use um proxy reverso "
                   "com TLS (ver server/README.md)",
                   opt.bind);
    if (https && cbserver::isLoopbackBind(opt.bind) && proxies.empty() && publicHost.rfind("127.", 0) != 0 &&
        publicHost.rfind("localhost", 0) != 0 && publicHost.rfind("[::1]", 0) != 0)
      spdlog::warn("painel admin: atrás de proxy (publicUrl {} com bind {}) sem admin.trustedProxies: todos os "
                   "acessos têm o IP do proxy e os limites por IP (início de login, falhas) valem para todo mundo",
                   *opt.publicUrl, opt.bind);
    if (hosts.empty())
      spdlog::warn("painel admin: nenhum Host aceito (bind fora do loopback sem admin.publicUrl): toda requisição "
                   "recebe 421");
    th = std::thread([this] { svr.listen_after_bind(); });
    svr.wait_until_ready();
  }

  void stop() {
    svr.stop();
    if (th.joinable()) th.join();
    flushRefused(true);
  }
};

// =====================================================================================================================

AdminServer::AdminServer(AdminOptions opt, AdminBackend be)
    : impl_(std::make_unique<Impl>(std::move(opt), std::move(be))) {}
AdminServer::~AdminServer() { stop(); }
void AdminServer::start() { impl_->start(); }
void AdminServer::stop() {
  if (impl_) impl_->stop();
}
int AdminServer::boundPort() const { return impl_->port; }
size_t AdminServer::sessionCount() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->sessions.size();
}

AdminBackend makeBackend(cbscript::ScriptHost& host, cbserver::ScriptBridge& bridge,
                         std::shared_ptr<cbscript::BanStore> bans, cbserver::KeyProvider& keys,
                         std::function<nlohmann::json()> metrics, std::function<void(const std::string&)> audit,
                         std::function<int64_t()> unixNow) {
  AdminBackend be;
  be.keys = &keys;
  be.unixNow = unixNow;
  be.players = [&host] { return host.players(); };
  be.simCommand = [&bridge](cbscript::ScriptToSim c) { return bridge.postAdmin(std::move(c)); };
  be.resources = [&host] { return host.status(); };
  be.resourceOp = [&host](ResourceOp op, std::string name) {
    switch (op) {
      case ResourceOp::Start:
        return host.startResource(std::move(name));
      case ResourceOp::Stop:
        return host.stopResource(std::move(name));
      default:
        return host.restartResource(std::move(name));
    }
  };
  be.scriptCommand = [&host](cbscript::EvCommand c) { return host.post(std::move(c)); };
  be.ban = [&host, &bridge, bans, unixNow](const std::string& did, const std::string& reason, int64_t dur,
                                           const std::string& by) -> std::future<BanOutcome> {
    auto done = std::make_shared<std::promise<BanOutcome>>();
    auto fut = done->get_future();
    if (!bans || host.metrics().halted) {
      done->set_value(BanOutcome::Failed);
      return fut;
    }
    cbscript::Ban b;
    b.did = did;
    b.reason = reason;
    b.by = by;
    b.createdAt = unixNow ? unixNow()
                          : std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    b.expiresAt = dur > 0 ? b.createdAt + dur : 0;
    // BanStore tem um escritor só: a thread de scripts. O resultado volta pelo future; se a tarefa for descartada
    // (host parou entre a checagem e a fila), a promessa é destruída sem valor e o painel lê isso como falha.
    host.runOnScriptThread([bans, b, done](cbscript::ScriptContext& ctx) mutable {
      std::string err;
      const bool ok = bans->add(std::move(b), err);
      if (!ok) ctx.log(cbscript::LogLevel::Error, "painel admin: ban não gravado no arquivo (vale em memória): " + err);
      done->set_value(ok ? BanOutcome::Saved : BanOutcome::MemoryOnly);
    });
    // Kick por did já, pelo canal prioritário da ponte (thread do painel; pega também quem ainda não chegou aos
    // scripts). A ponte vive mais que o painel (o painel para antes).
    std::string why = "banido: " + reason;
    if (why.size() > 250) why = clip(why, 240);
    bridge.postAdmin(cbscript::CmdKickDid{did, std::move(why)});
    return fut;
  };
  be.metrics = std::move(metrics);
  be.audit = std::move(audit);
  return be;
}

}  // namespace cbadmin
