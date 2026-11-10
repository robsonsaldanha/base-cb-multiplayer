#include "cbserver/config.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <vector>
#include <regex>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "cbmp/base64url.hpp"

namespace cbserver {
namespace {

using nlohmann::json;

[[noreturn]] void fail(const std::string& field, const std::string& why) {
  throw std::runtime_error("Configuração inválida: campo '" + field + "' " + why);
}

const json* sub(const json& j, const char* key, const std::string& path) {
  if (!j.is_object()) fail(path, "deve ser um objeto");
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return nullptr;
  return &*it;
}

void getInt(const json& obj, const char* key, const std::string& path, int lo, int hi, int& out) {
  const json* v = sub(obj, key, path);
  if (!v) return;
  if (!v->is_number_integer()) fail(path + key, "deve ser um número inteiro");
  auto n = v->get<int64_t>();
  if (n < lo || n > hi)
    fail(path + key, "deve estar entre " + std::to_string(lo) + " e " + std::to_string(hi));
  out = static_cast<int>(n);
}

void getNum(const json& obj, const char* key, const std::string& path, double lo, double hi, double& out) {
  const json* v = sub(obj, key, path);
  if (!v) return;
  if (!v->is_number()) fail(path + key, "deve ser um número");
  double n = v->get<double>();
  if (!(n >= lo && n <= hi)) fail(path + key, "fora do intervalo permitido");
  out = n;
}

void getStr(const json& obj, const char* key, const std::string& path, std::string& out) {
  const json* v = sub(obj, key, path);
  if (!v) return;
  if (!v->is_string()) fail(path + key, "deve ser texto");
  out = v->get<std::string>();
}

bool allOf(const std::string& s, const char* set) { return !s.empty() && s.find_first_not_of(set) == std::string::npos; }


std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t b = 0;
  for (;;) {
    const auto e = s.find(sep, b);
    out.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
    if (e == std::string::npos) break;
    b = e + 1;
  }
  return out;
}

// IPv4 decimal estrito: 4 octetos, sem zero à esquerda, <= 255.
bool strictIpv4(const std::string& host) {
  const auto parts = split(host, '.');
  if (parts.size() != 4) return false;
  for (const auto& p : parts) {
    if (p.empty() || p.size() > 3 || !allOf(p, "0123456789") || (p.size() > 1 && p[0] == '0')) return false;
    if (std::stoi(p) > 255) return false;
  }
  return true;
}

bool isLoopbackHost(const std::string& host) {
  return host == "localhost" || host == "[::1]" || (strictIpv4(host) && host.rfind("127.", 0) == 0);
}

// Conteúdo entre colchetes -> IPv6 canônico (RFC 5952 / WHATWG); vazio se inválido.
std::string canonicalIpv6(const std::string& inner) {
  if (!allOf(inner, "0123456789abcdef:")) return {};
  const auto first = inner.find("::");
  if (first != std::string::npos && first != inner.rfind("::")) return {};
  auto groupsOf = [](const std::string& s) { return s.empty() ? std::vector<std::string>{} : split(s, ':'); };
  std::vector<std::string> groups;
  if (first != std::string::npos) {
    auto head = groupsOf(inner.substr(0, first));
    auto tail = groupsOf(inner.substr(first + 2));
    if (head.size() + tail.size() > 7) return {};
    groups = head;
    groups.resize(8 - tail.size(), "0");
    groups.insert(groups.end(), tail.begin(), tail.end());
  } else {
    groups = split(inner, ':');
    if (groups.size() != 8) return {};
  }
  int n[8];
  for (size_t i = 0; i < 8; ++i) {
    if (groups[i].empty() || groups[i].size() > 4 || !allOf(groups[i], "0123456789abcdef")) return {};
    n[i] = std::stoi(groups[i], nullptr, 16);
  }
  int bestAt = -1, bestLen = 0;
  for (int i = 0; i < 8;) {
    if (n[i] != 0) {
      ++i;
      continue;
    }
    int j = i;
    while (j < 8 && n[j] == 0) ++j;
    if (j - i > bestLen) {
      bestAt = i;
      bestLen = j - i;
    }
    i = j;
  }
  auto hex = [&](int from, int to) {
    std::string out;
    char buf[8];
    for (int i = from; i < to; ++i) {
      std::snprintf(buf, sizeof buf, "%x", static_cast<unsigned>(n[i]));
      if (i > from) out += ':';
      out += buf;
    }
    return out;
  };
  if (bestLen < 2) return hex(0, 8);
  return hex(0, bestAt) + "::" + hex(bestAt + bestLen, 8);
}

// Nome DNS ASCII estrito: rótulos 1..63 de [a-z0-9-] sem hífen nas pontas, sem "xn--" (sem IDN), <= 253.
// Último rótulo numérico (ou 0x...) = IPv4 para o parser de URL: só a forma decimal estrita.
bool strictHostName(const std::string& host) {
  if (host.empty() || host.size() > 253) return false;
  const auto labels = split(host, '.');
  const std::string& last = labels.back();
  const bool numericLast = allOf(last, "0123456789") ||
                           (last.rfind("0x", 0) == 0 && (last.size() == 2 || allOf(last.substr(2), "0123456789abcdef")));
  if (numericLast) return strictIpv4(host);
  for (const auto& l : labels) {
    if (l.empty() || l.size() > 63 || !allOf(l, "abcdefghijklmnopqrstuvwxyz0123456789-") || l.front() == '-' ||
        l.back() == '-' || l.rfind("xn--", 0) == 0)
      return false;
  }
  return true;
}

}  // namespace

// MESMO algoritmo de normalizeAdminOrigin em packages/contracts/src/schemas.ts (vetores compartilhados em
// packages/contracts/test-vectors/admin-origin.json).
std::optional<std::string> normalizeAdminOrigin(std::string_view rawView) {
  if (rawView.empty() || rawView.size() > 256) return std::nullopt;
  std::string raw(rawView);
  for (char& ch : raw) {
    const auto u = static_cast<unsigned char>(ch);
    if (u <= 0x20 || u >= 0x7f) return std::nullopt;  // espaços, controles, não-ASCII
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  // Sem @, barra invertida, %, ? e #, e no máximo uma "/" final.
  static const std::regex re(R"(^(https?)://([a-z0-9.\-\[\]:]+)/?$)");
  std::smatch m;
  if (!std::regex_match(raw, m, re)) return std::nullopt;
  const std::string scheme = m[1].str();
  const std::string authority = m[2].str();
  std::string host;
  std::optional<std::string> port;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string::npos) return std::nullopt;
    const std::string v6 = canonicalIpv6(authority.substr(1, close - 1));
    if (v6.empty()) return std::nullopt;
    host = "[" + v6 + "]";
    const std::string rest = authority.substr(close + 1);
    if (!rest.empty()) {
      if (rest[0] != ':') return std::nullopt;
      port = rest.substr(1);
    }
  } else {
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
    if (!strictHostName(host)) return std::nullopt;
    if (colon != std::string::npos) port = authority.substr(colon + 1);
  }
  std::string portOut;
  if (port) {
    if (port->empty() || port->size() > 5 || !allOf(*port, "0123456789")) return std::nullopt;
    const int p = std::stoi(*port);
    if (p < 1 || p > 65535) return std::nullopt;
    if (!((scheme == "https" && p == 443) || (scheme == "http" && p == 80))) portOut = ":" + std::to_string(p);
  }
  if (scheme == "http" && !isLoopbackHost(host)) return std::nullopt;
  return scheme + "://" + host + portOut;
}

namespace {

// IPv4 decimal estrito (4 octetos 0..255, sem zeros à esquerda).
bool parseIpv4(std::string_view s, std::array<int, 4>& o) {
  size_t i = 0;
  for (int k = 0; k < 4; ++k) {
    if (k > 0) {
      if (i >= s.size() || s[i] != '.') return false;
      ++i;
    }
    size_t b = i;
    int v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - b < 3) v = v * 10 + (s[i++] - '0');
    if (i == b || v > 255 || (i - b > 1 && s[b] == '0')) return false;
    o[static_cast<size_t>(k)] = v;
  }
  return i == s.size();
}

// IPv6 literal (sem colchetes/zona): até 8 grupos hex de 1..4 dígitos, no máximo um "::". Devolve os 8 grupos.
bool parseIpv6(std::string_view s, std::array<uint32_t, 8>& g) {
  if (s.empty() || s.size() > 45) return false;
  const size_t dbl = s.find("::");
  if (dbl != std::string_view::npos && s.find("::", dbl + 1) != std::string_view::npos) return false;
  auto groups = [](std::string_view part, std::vector<uint32_t>& out) {
    if (part.empty()) return true;
    size_t b = 0;
    while (true) {
      size_t e = part.find(':', b);
      std::string_view h = part.substr(b, e == std::string_view::npos ? std::string_view::npos : e - b);
      if (h.empty() || h.size() > 4) return false;
      uint32_t v = 0;
      for (char c : h) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v * 16 + static_cast<uint32_t>(d);
      }
      out.push_back(v);
      if (e == std::string_view::npos) return true;
      b = e + 1;
    }
  };
  std::vector<uint32_t> head, tail;
  if (dbl == std::string_view::npos) {
    if (!groups(s, head) || head.size() != 8) return false;
  } else {
    if (!groups(s.substr(0, dbl), head) || !groups(s.substr(dbl + 2), tail) || head.size() + tail.size() > 7)
      return false;
  }
  g.fill(0);
  for (size_t i = 0; i < head.size(); ++i) g[i] = head[i];
  for (size_t i = 0; i < tail.size(); ++i) g[8 - tail.size() + i] = tail[i];
  return true;
}

bool validBindIp(std::string_view s) {
  std::array<int, 4> o{};
  std::array<uint32_t, 8> g{};
  return parseIpv4(s, o) || parseIpv6(s, g);
}

}  // namespace

bool isLoopbackBind(std::string_view ip) {
  std::array<int, 4> o{};
  if (parseIpv4(ip, o)) return o[0] == 127;
  std::array<uint32_t, 8> g{};
  if (!parseIpv6(ip, g)) return false;
  for (size_t i = 0; i < 7; ++i)
    if (g[i] != 0) return false;
  return g[7] == 1;
}

bool isWildcardBind(std::string_view ip) {
  std::array<int, 4> o{};
  if (parseIpv4(ip, o)) return o == std::array<int, 4>{0, 0, 0, 0};
  std::array<uint32_t, 8> g{};
  if (!parseIpv6(ip, g)) return false;
  for (auto v : g)
    if (v != 0) return false;
  return true;
}

ServerConfig ServerConfig::fromJson(const json& j) {
  if (!j.is_object()) fail("(raiz)", "deve ser um objeto JSON");
  ServerConfig c;
  static const char* kKnown[] = {"name",   "description", "tags",     "locale",       "maxPlayers", "port",
                                 "hub",    "auth",        "net",      "sim",          "movement",   "events",
                                 "metrics", "resourcesDir", "logsDir", "scripting", "db",       "admin"};
  for (auto it = j.begin(); it != j.end(); ++it) {
    bool known = false;
    for (auto* k : kKnown) known = known || it.key() == k;
    if (!known) spdlog::warn("Configuração: chave desconhecida '{}' ignorada", it.key());
  }
  getStr(j, "name", "", c.name);
  {
    auto b = c.name.find_first_not_of(" \t\r\n");
    auto e = c.name.find_last_not_of(" \t\r\n");
    c.name = b == std::string::npos ? std::string() : c.name.substr(b, e - b + 1);
    if (c.name.size() < 3 || c.name.size() > 64) fail("name", "deve ter entre 3 e 64 caracteres (sem espaços nas pontas)");
  }
  getStr(j, "description", "", c.description);
  if (c.description.size() > 500) fail("description", "deve ter no máximo 500 caracteres");
  getStr(j, "locale", "", c.locale);
  if (!std::regex_match(c.locale, std::regex("^[a-z]{2}(-[A-Z]{2})?$"))) fail("locale", "deve ser como 'pt-BR' ou 'en'");
  getStr(j, "resourcesDir", "", c.resourcesDir);
  getStr(j, "logsDir", "", c.logsDir);
  if (const json* t = sub(j, "tags", "")) {
    if (!t->is_array()) fail("tags", "deve ser uma lista de textos");
    c.tags.clear();
    for (auto& e : *t) {
      if (!e.is_string()) fail("tags", "deve ser uma lista de textos");
      if (!std::regex_match(e.get<std::string>(), std::regex("^[a-z0-9-]{1,24}$")))
        fail("tags", "cada tag deve casar com [a-z0-9-] de 1 a 24 caracteres");
      c.tags.push_back(e.get<std::string>());
    }
    if (c.tags.size() > 10) fail("tags", "deve ter no máximo 10 itens");
  }
  getInt(j, "maxPlayers", "", 1, 2048, c.maxPlayers);
  getInt(j, "port", "", 1, 65535, c.port);

  if (const json* h = sub(j, "hub", "")) {
    if (!h->is_object()) fail("hub", "deve ser um objeto");
    HubConfig hub;
    auto need = [&](const char* k, std::string& out) {
      const json* v = sub(*h, k, "hub.");
      if (!v || !v->is_string()) fail(std::string("hub.") + k, "é obrigatório quando 'hub' é informado");
      out = v->get<std::string>();
    };
    need("url", hub.url);
    need("serverId", hub.serverId);
    need("key", hub.key);
    if (!std::regex_match(hub.serverId, std::regex("^srv_.+"))) fail("hub.serverId", "deve começar com 'srv_'");
    if (!std::regex_match(hub.key, std::regex("^sk_.+"))) fail("hub.key", "deve começar com 'sk_'");
    if (!std::regex_match(hub.url, std::regex("^https?://[^\\s/]+.*"))) fail("hub.url", "deve ser http(s)://...");
    c.hub = std::move(hub);
  }

  if (const json* a = sub(j, "auth", "")) {
    if (const json* k = sub(*a, "devPublicKey", "auth.")) {
      if (!k->is_string()) fail("auth.devPublicKey", "deve ser texto base64url");
      auto raw = cbmp::base64urlDecodeCanonical(k->get<std::string>());
      if (!raw || raw->size() != 32) fail("auth.devPublicKey", "deve ser base64url canônico de 32 bytes");
      c.devPublicKey = k->get<std::string>();
    }
  }

  if (c.hub && c.devPublicKey)
    fail("hub", "e 'auth.devPublicKey' são mutuamente exclusivos: use o Hub ou a chave dev, não os dois");

  if (const json* n = sub(j, "net", "")) {
    getInt(*n, "pendingPerIp", "net.", 1, 1024, c.pendingPerIp);
    getInt(*n, "pendingGlobal", "net.", 1, 65536, c.pendingGlobal);
    getInt(*n, "maxOutKbpsPerClient", "net.", 1, 1000000, c.maxOutKbpsPerClient);
  }
  if (const json* s = sub(j, "sim", "")) {
    getInt(*s, "tickHz", "sim.", 1, 60, c.tickHz);
    getNum(*s, "streamRadiusM", "sim.", 50, 2000, c.streamRadiusM);
    getInt(*s, "maxVisible", "sim.", 8, 256, c.maxVisible);
    if (const json* sp = sub(*s, "spawn", "sim.")) {
      if (!sp->is_object()) fail("sim.spawn", "deve ser um objeto como {\"mode\":\"origin\"}");
      std::string mode = "origin";
      getStr(*sp, "mode", "sim.spawn.", mode);
      if (mode == "origin")
        c.spawnMode = SpawnMode::Origin;
      else if (mode == "devTrustFirstInput")
        c.spawnMode = SpawnMode::DevTrustFirstInput;
      else
        fail("sim.spawn.mode", "deve ser 'origin' ou 'devTrustFirstInput'");
    }
  }
  if (c.spawnMode == SpawnMode::DevTrustFirstInput && (!c.devPublicKey || c.hub))
    fail("sim.spawn.mode", "'devTrustFirstInput' só é permitido em modo dev (auth.devPublicKey definido e hub null)");
  if (const json* m = sub(j, "movement", "")) {
    getNum(*m, "tolerance", "movement.", 0, 1, c.tolerance);
    if (const json* sp = sub(*m, "maxSpeedCmS", "movement.")) {
      const double inf = 1e9;
      getNum(*sp, "idle", "movement.maxSpeedCmS.", 0, inf, c.speedIdle);
      getNum(*sp, "walk", "movement.maxSpeedCmS.", 0, inf, c.speedWalk);
      getNum(*sp, "run", "movement.maxSpeedCmS.", 0, inf, c.speedRun);
      getNum(*sp, "broom", "movement.maxSpeedCmS.", 0, inf, c.speedBroom);
      getNum(*sp, "swim", "movement.maxSpeedCmS.", 0, inf, c.speedSwim);
      getNum(*sp, "fall", "movement.maxSpeedCmS.", 0, inf, c.speedFall);
      getNum(*sp, "apparate", "movement.maxSpeedCmS.", 0, inf, c.speedApparate);
    }
  }
  if (const json* e = sub(j, "events", "")) {
    getInt(*e, "perSecond", "events.", 1, 100000, c.eventsPerSecond);
    getInt(*e, "burst", "events.", 1, 100000, c.eventsBurst);
  }
  if (const json* sc = sub(j, "scripting", "")) {
    if (!sc->is_object()) fail("scripting", "deve ser um objeto");
    getInt(*sc, "memoryLimitMB", "scripting.", 1, 65536, c.scriptMemoryLimitMB);
    getInt(*sc, "warnMs", "scripting.", 1, 600000, c.scriptWarnMs);
    getInt(*sc, "abortMs", "scripting.", 1, 600000, c.scriptAbortMs);
    if (c.scriptAbortMs <= c.scriptWarnMs) fail("scripting.abortMs", "deve ser maior que 'scripting.warnMs'");
    getStr(*sc, "bansFile", "scripting.", c.bansFile);
    if (c.bansFile.empty()) fail("scripting.bansFile", "não pode ser vazio");
    getNum(*sc, "eventOutKBps", "scripting.", 1, 1e6, c.scriptEventOutKBps);
    getNum(*sc, "eventOutBurstKB", "scripting.", 8, 1e6, c.scriptEventOutBurstKB);
  }
  if (const json* d = sub(j, "db", "")) {
    if (!d->is_object()) fail("db", "deve ser um objeto {host, port, user, password, database, pool, timeoutMs, tls} ou null");
    static const char* kDbKeys[] = {"host", "port", "user", "password", "database", "pool", "timeoutMs", "tls"};
    for (auto it = d->begin(); it != d->end(); ++it) {
      bool known = false;
      for (auto* k : kDbKeys) known = known || it.key() == k;
      if (!known) spdlog::warn("Configuração: chave desconhecida 'db.{}' ignorada", it.key());
    }
    DbSettings db;
    getStr(*d, "host", "db.", db.host);
    getInt(*d, "port", "db.", 1, 65535, db.port);
    getStr(*d, "user", "db.", db.user);
    getStr(*d, "password", "db.", db.password);  // erros de tipo citam só o campo, nunca o valor
    getStr(*d, "database", "db.", db.database);
    getInt(*d, "pool", "db.", 1, 64, db.pool);
    getInt(*d, "timeoutMs", "db.", 100, 600000, db.timeoutMs);
    getStr(*d, "tls", "db.", db.tls);
    if (!d->contains("host") || db.host.empty() || db.host.size() > 255) fail("db.host", "é obrigatório (1 a 255 caracteres)");
    if (!d->contains("user") || db.user.empty() || db.user.size() > 80) fail("db.user", "é obrigatório (1 a 80 caracteres)");
    if (!d->contains("database") || db.database.empty() || db.database.size() > 64)
      fail("db.database", "é obrigatório (1 a 64 caracteres)");
    if (db.tls != "auto" && db.tls != "off" && db.tls != "required" && db.tls != "verify")
      fail("db.tls", "deve ser 'auto', 'off', 'required' ou 'verify'");
    c.db = std::move(db);
  }
  if (const json* a = sub(j, "admin", "")) {
    if (!a->is_object()) fail("admin", "deve ser um objeto como {\"publicUrl\": \"https://painel.exemplo.com\"} ou null");
    static const char* kAdminKeys[] = {"publicUrl", "port", "bind", "admins", "devToken"};
    for (auto it = a->begin(); it != a->end(); ++it) {
      bool known = false;
      for (auto* k : kAdminKeys) known = known || it.key() == k;
      if (!known) spdlog::warn("Configuração: chave desconhecida 'admin.{}' ignorada", it.key());
    }
    getInt(*a, "port", "admin.", 1, 65535, c.admin.port);
    if (c.admin.port == c.port) fail("admin.port", "deve ser diferente de 'port' (a porta TCP do jogo)");
    getStr(*a, "bind", "admin.", c.admin.bind);
    if (!validBindIp(c.admin.bind))
      fail("admin.bind", "deve ser um IP literal (ex. \"127.0.0.1\", \"0.0.0.0\", \"::1\"), sem porta");
    if (const json* l = sub(*a, "admins", "admin.")) {
      if (!l->is_array()) fail("admin.admins", "deve ser uma lista de ids Discord (texto com 17 a 20 dígitos)");
      if (l->size() > 100) fail("admin.admins", "deve ter no máximo 100 itens");
      for (const auto& e : *l) {
        if (!e.is_string()) fail("admin.admins", "deve ser uma lista de ids Discord (texto com 17 a 20 dígitos)");
        const auto& s = e.get_ref<const std::string&>();
        if (s.size() < 17 || s.size() > 20 || !allOf(s, "0123456789"))
          fail("admin.admins", "cada item deve ser um id Discord (17 a 20 dígitos)");
        if (std::find(c.admin.admins.begin(), c.admin.admins.end(), s) == c.admin.admins.end())
          c.admin.admins.push_back(s);
      }
    }
    if (const json* t = sub(*a, "devToken", "admin.")) {
      // Nunca repete o valor em mensagens.
      if (!t->is_string()) fail("admin.devToken", "deve ser texto");
      const auto& s = t->get_ref<const std::string&>();
      if (s.size() < 32 || s.size() > 256) fail("admin.devToken", "deve ter entre 32 e 256 caracteres");
      for (char ch : s)
        if (static_cast<unsigned char>(ch) < 0x21 || static_cast<unsigned char>(ch) > 0x7e)
          fail("admin.devToken", "só aceita ASCII visível (sem espaços)");
      if (!c.devPublicKey || c.hub)
        fail("admin.devToken", "só é permitido em modo dev (auth.devPublicKey definido e hub null); no modo Hub o "
                               "login é pelo Discord via Hub");
      c.admin.devToken = s;
    }
    if (const json* u = sub(*a, "publicUrl", "admin.")) {
      if (!u->is_string()) fail("admin.publicUrl", "deve ser texto (origem https://host[:porta])");
      auto o = normalizeAdminOrigin(u->get<std::string>());
      if (!o)
        fail("admin.publicUrl",
             "deve ser uma origem https://host[:porta] (http só em loopback), sem caminho, query, fragmento ou "
             "credenciais, até 256 caracteres");
      c.admin.publicUrl = std::move(*o);
    }
  }
  if (const json* m = sub(j, "metrics", "")) {
    if (const json* t = sub(*m, "token", "metrics.")) {
      if (!t->is_string()) fail("metrics.token", "deve ser texto");
      c.metricsToken = t->get<std::string>();
    }
  }
  return c;
}

ServerConfig ServerConfig::load(const std::filesystem::path& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Não foi possível abrir o arquivo de configuração: " + path.string());
  json j;
  try {
    j = json::parse(f);
  } catch (const json::exception& e) {
    throw std::runtime_error("Configuração inválida: JSON malformado em " + path.string() + ": " + e.what());
  }
  return fromJson(j);
}

}  // namespace cbserver
