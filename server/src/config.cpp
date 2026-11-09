#include "cbserver/config.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>

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
  getStr(j, "name", "", c.name);
  getStr(j, "description", "", c.description);
  getStr(j, "locale", "", c.locale);
  getStr(j, "resourcesDir", "", c.resourcesDir);
  getStr(j, "logsDir", "", c.logsDir);
  if (const json* t = sub(j, "tags", "")) {
    if (!t->is_array()) fail("tags", "deve ser uma lista de textos");
    c.tags.clear();
    for (auto& e : *t) {
      if (!e.is_string()) fail("tags", "deve ser uma lista de textos");
      c.tags.push_back(e.get<std::string>());
    }
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

  if (const json* n = sub(j, "net", "")) {
    getInt(*n, "pendingPerIp", "net.", 1, 1024, c.pendingPerIp);
    getInt(*n, "pendingGlobal", "net.", 1, 65536, c.pendingGlobal);
    getInt(*n, "maxOutKbpsPerClient", "net.", 1, 1000000, c.maxOutKbpsPerClient);
  }
  if (const json* s = sub(j, "sim", "")) {
    getInt(*s, "tickHz", "sim.", 1, 60, c.tickHz);
    getNum(*s, "streamRadiusM", "sim.", 50, 2000, c.streamRadiusM);
    getInt(*s, "maxVisible", "sim.", 8, 256, c.maxVisible);
  }
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
