#include "cbserver/config.hpp"

#include <fstream>
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

}  // namespace

ServerConfig ServerConfig::fromJson(const json& j) {
  if (!j.is_object()) fail("(raiz)", "deve ser um objeto JSON");
  ServerConfig c;
  static const char* kKnown[] = {"name",   "description", "tags",     "locale",       "maxPlayers", "port",
                                 "hub",    "auth",        "net",      "sim",          "movement",   "events",
                                 "metrics", "resourcesDir", "logsDir", "scripting", "db"};
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
