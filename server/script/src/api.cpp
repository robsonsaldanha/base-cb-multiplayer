// API cb.* (ver api.hpp).
//
// Regra dos bindings (ver resource.cpp, Task 2): erros do Lua (luaL_error e erro de memória no teto do estado)
// fazem longjmp. Por isso as funções lua_CFunction abaixo ("quadros puros") só têm variáveis triviais (inteiros,
// ponteiros, buffers de char); todo trabalho com objetos C++ fica em ajudantes CBAPI_NOINLINE que não chamam
// nenhuma função do Lua capaz de lançar erro e devolvem um código + mensagem num buffer de char do chamador.
//
// Referências: os bindings guardam funções com luaL_ref (inteiro, no registro compartilhado por todas as threads do
// estado). O host só monta sol::reference a partir delas fora de qualquer chamada ao Lua (despacho de eventos,
// timers, eventos locais), quando o estado principal está ocioso e sem teto de memória.
#include "cbscript/api.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "cbscript/bans.hpp"
#include "cbscript/json_lua.hpp"

#if defined(_MSC_VER)
#define CBAPI_NOINLINE __declspec(noinline)
#else
#define CBAPI_NOINLINE __attribute__((noinline))
#endif

namespace cbscript {
namespace {

constexpr size_t kMaxEventName = 64;    // = cbmp::kMaxEventName
constexpr size_t kMaxPayload = 4096;    // = cbmp::kMaxEventPayload
constexpr size_t kMaxReason = 200;      // motivo de kick/ban (Kick/Reject aceitam até 255 bytes)
constexpr size_t kMaxLogLine = 64 * 1024;
constexpr int kLocalRounds = 64;        // rodadas de eventos locais por gancho (o resto fica para o próximo)
constexpr const char* kOnNames[] = {"playerJoin", "playerLeave", "resourceStart", "resourceStop"};
enum OnKind { OnJoin = 0, OnLeave, OnStart, OnStop, OnCount };

char gNullTag = 0;  // endereço do sentinela cb.null (lightuserdata)

struct Err {
  char msg[512];
};

void setErr(Err& e, const std::string& s) {
  const size_t n = std::min(s.size(), sizeof e.msg - 1);
  std::memcpy(e.msg, s.data(), n);
  e.msg[n] = '\0';
}

// Corta em `max` bytes sem quebrar um caractere UTF-8.
std::string clampUtf8(std::string_view s, size_t max) {
  if (s.size() <= max) return std::string(s);
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return std::string(s.substr(0, n));
}

// Referência no registro (do estado principal). Destruída pelo host (fora de chamadas ao Lua); dentro de um
// binding, solte com release(L) antes.
struct Ref {
  lua_State* L = nullptr;  // estado principal
  int ref = LUA_NOREF;
  Ref() = default;
  Ref(lua_State* main, int r) : L(main), ref(r) {}
  Ref(Ref&& o) noexcept : L(o.L), ref(std::exchange(o.ref, LUA_NOREF)) {}
  Ref& operator=(Ref&& o) noexcept {
    if (this != &o) {
      reset();
      L = o.L;
      ref = std::exchange(o.ref, LUA_NOREF);
    }
    return *this;
  }
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  ~Ref() { reset(); }
  void reset() {
    if (L && ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
    ref = LUA_NOREF;
  }
  // Para uso dentro de um binding: solta pela thread em execução (o registro é o mesmo).
  void release(lua_State* running) {
    if (ref != LUA_NOREF) luaL_unref(running, LUA_REGISTRYINDEX, ref);
    ref = LUA_NOREF;
  }
  // Só no host, com o estado principal ocioso.
  sol::reference get() const { return sol::reference(L, sol::ref_index(ref)); }
};

struct Conn {
  std::string name, did, sub;
  uint32_t session = 0;
};

// Limite de linhas por janela de 1 s (mesma regra do print: 200/s + linha de resumo).
struct RateWindow {
  std::chrono::steady_clock::time_point start{};
  int lines = 0;
  uint64_t dropped = 0;
  // true = pode registrar. `summary` recebe o número descartado na janela anterior (0 = nada a resumir).
  bool take(uint64_t& summary, int perSec = 200) {
    const auto now = std::chrono::steady_clock::now();
    summary = 0;
    if (now - start >= std::chrono::seconds(1)) {
      summary = dropped;
      start = now;
      lines = 0;
      dropped = 0;
    }
    if (lines >= perSec) {
      ++dropped;
      return false;
    }
    ++lines;
    return true;
  }
};

struct Shared;

struct Timer {
  double due = 0;
  double interval = 0;  // 0 = uma vez
  Ref fn;
};

struct ApiRes {
  std::shared_ptr<Shared> sh;
  ScriptContext* ctx = nullptr;
  HostedResource* h = nullptr;
  lua_State* L = nullptr;  // estado principal
  int nullRef = LUA_NOREF;
  uint32_t source = 0;  // origem dos emits deste resource (justiça entre resources nas filas)
  int depth = 0;  // chamadas da API em andamento neste resource (cb.resource recusa reentrância)
  bool live = false;
  std::array<std::vector<Ref>, OnCount> on;
  std::map<std::string, std::vector<Ref>, std::less<>> net, local;
  std::map<uint64_t, Timer> timers;
  std::set<std::pair<double, uint64_t>> due;
  uint64_t nextTimer = 1;
  std::string scratch;    // JSON temporário dos bindings
  PlayersSnapshot snap;   // foto em uso por players.list/get
  std::vector<const PlayerView*> view;  // entradas da foto cuja sessão bate com a dos conectados
  RateWindow logRate;
  RateWindow auditRate;  // só cb.audit

  JsonOptions json(size_t maxBytes = kMaxPayload) const {
    JsonOptions o;
    o.maxBytes = maxBytes;
    o.nullRef = nullRef;
    return o;
  }
  ~ApiRes();
};

struct Command {
  ApiRes* owner = nullptr;
  Ref fn;
  bool admin = false;
};

struct LocalEvent {
  std::string name, json, from;
};

struct Shared {
  ApiOptions o;
  std::unordered_map<uint16_t, Conn> players;  // conectados (Join/Leave, na ordem da fila)
  std::vector<ApiRes*> live;                   // resources rodando, em ordem de subida
  std::map<std::string, Command, std::less<>> commands;
  std::deque<LocalEvent> localQ;
  bool draining = false;
  double lastJsonWarn = -1e9;
  std::vector<uint64_t> dueScratch;
  uint32_t nextSource = 1;

  int64_t unixNow() const {
    if (o.unixNow) return o.unixNow();
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
};

ApiRes::~ApiRes() {
  if (!sh) return;
  std::erase(sh->live, this);
  for (auto it = sh->commands.begin(); it != sh->commands.end();)
    it = it->second.owner == this ? sh->commands.erase(it) : std::next(it);
  // Refs (handlers, timers) se soltam sozinhas: o host zera `api` antes de fechar o estado.
}

ApiRes* self(lua_State* L) { return static_cast<ApiRes*>(lua_touserdata(L, lua_upvalueindex(1))); }

const Conn* connOf(const Shared& sh, uint16_t id) {
  auto it = sh.players.find(id);
  return it == sh.players.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------------------------- auditoria

// `limited` = linha pedida pelo script (cb.audit): 200/s por resource. Linhas do motor (kick, ban, unban, command)
// nunca são limitadas.
CBAPI_NOINLINE void auditLine(ApiRes& a, nlohmann::json line, bool limited = false) {
  if (a.ctx->abandoned() || !a.sh->o.audit) return;
  try {
    if (limited) {
      uint64_t summary = 0;
      const bool ok = a.auditRate.take(summary);
      if (summary > 0)
        a.sh->o.audit(nlohmann::json{{"type", "audit_dropped"}, {"ts", a.sh->unixNow()}, {"resource", a.h->name()},
                                     {"count", summary}}
                          .dump());
      if (!ok) {
        if (a.sh->o.stats) a.sh->o.stats->auditDropped.fetch_add(1);
        return;
      }
    }
    line["ts"] = a.sh->unixNow();
    line["resource"] = a.h->name();
    a.sh->o.audit(line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  } catch (...) {
  }
}

// ------------------------------------------------------------------------------------- checagens (quadro puro)

const char* checkName(lua_State* L, int idx, const char* fn, size_t* len) {
  if (lua_type(L, idx) != LUA_TSTRING) luaL_error(L, "%s: nome inválido (texto de 1 a 64 bytes)", fn);
  const char* s = lua_tolstring(L, idx, len);
  if (*len == 0 || *len > kMaxEventName || std::memchr(s, 0, *len) != nullptr)
    luaL_error(L, "%s: nome inválido (texto de 1 a 64 bytes, sem NUL)", fn);
  if (*len >= 3 && std::memcmp(s, "cb:", 3) == 0) luaL_error(L, "%s: nomes 'cb:' são reservados ao motor", fn);
  return s;
}

void checkFn(lua_State* L, int idx, const char* fn) {
  if (lua_type(L, idx) != LUA_TFUNCTION) luaL_error(L, "%s: função esperada", fn);
}

uint16_t checkId(lua_State* L, int idx, const char* fn) {
  int isnum = 0;
  lua_Integer v = lua_type(L, idx) == LUA_TNUMBER ? lua_tointegerx(L, idx, &isnum) : 0;
  if (!isnum || v < 1 || v > 65535) luaL_error(L, "%s: id inválido", fn);
  return static_cast<uint16_t>(v);
}

// Texto opcional (nil → `def`).
const char* optText(lua_State* L, int idx, const char* fn, const char* what, const char* def, size_t* len) {
  if (lua_isnoneornil(L, idx)) {
    *len = std::strlen(def);
    return def;
  }
  if (lua_type(L, idx) != LUA_TSTRING) luaL_error(L, "%s: %s deve ser texto", fn, what);
  return lua_tolstring(L, idx, len);
}

// Sessão opcional (nil → 0 = ocupante atual do id).
uint32_t optSession(lua_State* L, int idx, const char* fn) {
  if (lua_isnoneornil(L, idx)) return 0;
  int isnum = 0;
  lua_Integer v = lua_type(L, idx) == LUA_TNUMBER ? lua_tointegerx(L, idx, &isnum) : 0;
  if (!isnum || v < 1 || v > 0xFFFFFFFFll) luaL_error(L, "%s: sessão inválida", fn);
  return static_cast<uint32_t>(v);
}

int pushRef(lua_State* L, int idx) {
  lua_pushvalue(L, idx);
  return luaL_ref(L, LUA_REGISTRYINDEX);
}

// ------------------------------------------------------------------------------------------------- cb.on

CBAPI_NOINLINE bool addHandler(std::vector<Ref>& v, ApiRes* a, int ref) {
  try {
    v.emplace_back(a->L, ref);
    return true;
  } catch (...) {
    return false;
  }
}

CBAPI_NOINLINE bool addNamed(std::map<std::string, std::vector<Ref>, std::less<>>& m, ApiRes* a, const char* name,
                             size_t len, int ref) {
  try {
    m[std::string(name, len)].emplace_back(a->L, ref);
    return true;
  } catch (...) {
    return false;
  }
}

int l_on(lua_State* L) {
  ApiRes* a = self(L);
  if (lua_type(L, 1) != LUA_TSTRING) return luaL_error(L, "cb.on: nome do evento deve ser texto");
  size_t nlen = 0;
  const char* name = lua_tolstring(L, 1, &nlen);
  int kind = -1;
  for (int i = 0; i < OnCount; ++i)
    if (nlen == std::strlen(kOnNames[i]) && std::memcmp(name, kOnNames[i], nlen) == 0) kind = i;
  if (kind < 0 && std::memchr(name, 0, nlen) != nullptr) return luaL_error(L, "cb.on: nome do evento contém NUL");
  if (kind < 0)
    return luaL_error(L, "cb.on: evento desconhecido '%s' (use playerJoin, playerLeave, resourceStart ou resourceStop)",
                      name);
  checkFn(L, 2, "cb.on");
  int ref = pushRef(L, 2);
  if (!addHandler(a->on[static_cast<size_t>(kind)], a, ref)) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return luaL_error(L, "cb.on: sem memória");
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------- cb.events

int registerNamed(lua_State* L, bool local) {
  ApiRes* a = self(L);
  const char* fn = local ? "cb.events.onLocal" : "cb.events.on";
  size_t len = 0;
  const char* name = checkName(L, 1, fn, &len);
  checkFn(L, 2, fn);
  int ref = pushRef(L, 2);
  if (!addNamed(local ? a->local : a->net, a, name, len, ref)) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return luaL_error(L, "%s: sem memória", fn);
  }
  return 0;
}
int l_events_on(lua_State* L) { return registerNamed(L, false); }
int l_events_onLocal(lua_State* L) { return registerNamed(L, true); }

// 1 = enfileirado, 0 = fila cheia, -1 = erro.
CBAPI_NOINLINE int doEmit(ApiRes* a, lua_State* L, lua_Integer target, const char* name, size_t len,
                          uint32_t want, Err& e) {
  try {
    std::string err;
    if (!luaToJson(L, 3, a->scratch, err, a->json())) {
      setErr(e, "cb.events.emit: payload inválido: " + err);
      return -1;
    }
    uint32_t session = 0;
    if (target != -1) {
      const Conn* c = connOf(*a->sh, static_cast<uint16_t>(target));
      if (!c) return 0;  // jogador não conectado: ignorado (sem erro)
      if (want != 0 && want != c->session) return 0;  // outra sessão ocupa o id
      session = c->session;
    }
    return a->ctx->emit(CmdEmit{static_cast<int32_t>(target), std::string(name, len), a->scratch, session, a->source})
               ? 1
               : 0;
  } catch (...) {
    setErr(e, "cb.events.emit: sem memória");
    return -1;
  }
}

int l_events_emit(lua_State* L) {
  ApiRes* a = self(L);
  int isnum = 0;
  lua_Integer t = lua_type(L, 1) == LUA_TNUMBER ? lua_tointegerx(L, 1, &isnum) : 0;
  if (!isnum || !(t == -1 || (t >= 1 && t <= 65535)))
    return luaL_error(L, "cb.events.emit: alvo inválido (id do jogador ou -1 para todos)");
  size_t len = 0;
  const char* name = checkName(L, 2, "cb.events.emit", &len);
  lua_settop(L, 4);
  const uint32_t want = optSession(L, 4, "cb.events.emit");
  Err e;
  int rc = doEmit(a, L, t, name, len, want, e);
  if (rc < 0) return luaL_error(L, "%s", e.msg);
  lua_pushboolean(L, rc);
  return 1;
}

CBAPI_NOINLINE int doEmitLocal(ApiRes* a, lua_State* L, const char* name, size_t len, Err& e) {
  try {
    std::string err, out;
    if (!luaToJson(L, 2, out, err, a->json(a->sh->o.maxLocalBytes))) {
      setErr(e, "cb.events.emitLocal: dados inválidos: " + err);
      return -1;
    }
    if (a->sh->localQ.size() >= a->sh->o.maxLocalQueue) {
      setErr(e, "cb.events.emitLocal: fila de eventos locais cheia (" + std::to_string(a->sh->o.maxLocalQueue) + ")");
      return -1;
    }
    a->sh->localQ.push_back(LocalEvent{std::string(name, len), std::move(out), a->h->name()});
    return 0;
  } catch (...) {
    setErr(e, "cb.events.emitLocal: sem memória");
    return -1;
  }
}

int l_events_emitLocal(lua_State* L) {
  ApiRes* a = self(L);
  size_t len = 0;
  const char* name = checkName(L, 1, "cb.events.emitLocal", &len);
  lua_settop(L, 2);
  Err e;
  if (doEmitLocal(a, L, name, len, e) < 0) return luaL_error(L, "%s", e.msg);
  return 0;
}

// --------------------------------------------------------------------------------------------- cb.players

// Foto atual, só com as entradas cuja sessão é a do jogador conectado (sem dados velhos de um id reaproveitado).
CBAPI_NOINLINE void grabSnap(ApiRes* a) {
  a->snap = a->ctx->players();
  a->view.clear();
  try {
    for (const PlayerView& p : *a->snap) {
      const Conn* c = connOf(*a->sh, p.id);
      if (c && c->session == p.session) a->view.push_back(&p);
    }
  } catch (...) {
    a->view.clear();
  }
}

void pushPlayer(lua_State* L, const PlayerView& p) {
  lua_createtable(L, 0, 9);
  lua_pushinteger(L, p.id);
  lua_setfield(L, -2, "id");
  lua_pushlstring(L, p.name.data(), p.name.size());
  lua_setfield(L, -2, "name");
  lua_pushlstring(L, p.did.data(), p.did.size());
  lua_setfield(L, -2, "did");
  lua_pushlstring(L, p.sub.data(), p.sub.size());
  lua_setfield(L, -2, "sub");
  lua_pushinteger(L, p.x);
  lua_setfield(L, -2, "x");
  lua_pushinteger(L, p.y);
  lua_setfield(L, -2, "y");
  lua_pushinteger(L, p.z);
  lua_setfield(L, -2, "z");
  lua_pushinteger(L, p.state);
  lua_setfield(L, -2, "state");
  lua_pushinteger(L, static_cast<lua_Integer>(p.session));
  lua_setfield(L, -2, "session");
}

int l_players_list(lua_State* L) {
  ApiRes* a = self(L);
  grabSnap(a);
  const std::vector<const PlayerView*>* v = &a->view;
  lua_createtable(L, static_cast<int>(std::min<size_t>(v->size(), 65535)), 0);
  for (size_t i = 0; i < v->size(); ++i) {
    pushPlayer(L, *(*v)[i]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

int l_players_get(lua_State* L) {
  ApiRes* a = self(L);
  uint16_t id = checkId(L, 1, "cb.players.get");
  grabSnap(a);
  const std::vector<const PlayerView*>* v = &a->view;
  for (size_t i = 0; i < v->size(); ++i)
    if ((*v)[i]->id == id) {
      pushPlayer(L, *(*v)[i]);
      return 1;
    }
  lua_pushnil(L);
  return 1;
}

// 1 = feito, 0 = jogador não conectado.
CBAPI_NOINLINE int doKick(ApiRes* a, uint16_t id, const char* reason, size_t len, uint32_t want) {
  if (a->ctx->abandoned()) return 0;
  try {
    const Conn* c = connOf(*a->sh, id);
    if (!c || (want != 0 && want != c->session)) return 0;
    std::string why = clampUtf8(std::string_view(reason, len), kMaxReason);
    if (!a->ctx->emit(CmdKick{id, why, c->session})) {
      a->ctx->log(LogLevel::Critical, "[" + a->h->name() + "] cb.players.kick: comando não entregue à simulação");
      return 0;
    }
    auditLine(*a, {{"type", "kick"}, {"id", id}, {"did", c->did}, {"reason", why}});
    return 1;
  } catch (...) {
    return 0;
  }
}

int l_players_kick(lua_State* L) {
  ApiRes* a = self(L);
  uint16_t id = checkId(L, 1, "cb.players.kick");
  size_t len = 0;
  const char* reason = optText(L, 2, "cb.players.kick", "o motivo", "expulso", &len);
  const uint32_t want = optSession(L, 3, "cb.players.kick");
  lua_pushboolean(L, doKick(a, id, reason, len, want));
  return 1;
}

CBAPI_NOINLINE int doSetPosition(ApiRes* a, uint16_t id, lua_Integer x, lua_Integer y, lua_Integer z, uint32_t want) {
  try {
    const Conn* c = connOf(*a->sh, id);
    if (!c || (want != 0 && want != c->session)) return 0;
    if (!a->ctx->emit(CmdSetPosition{id, static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z),
                                     c->session})) {
      a->ctx->log(LogLevel::Critical, "[" + a->h->name() + "] cb.players.setPosition: comando não entregue à simulação");
      return 0;
    }
    return 1;
  } catch (...) {
    return 0;
  }
}

int l_players_setPosition(lua_State* L) {
  ApiRes* a = self(L);
  uint16_t id = checkId(L, 1, "cb.players.setPosition");
  lua_Integer c[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    int isnum = 0;
    c[i] = lua_type(L, 2 + i) == LUA_TNUMBER ? lua_tointegerx(L, 2 + i, &isnum) : 0;
    if (!isnum || c[i] < INT32_MIN || c[i] > INT32_MAX)
      return luaL_error(L, "cb.players.setPosition: coordenadas inteiras (cm, int32) esperadas");
  }
  const uint32_t want = optSession(L, 5, "cb.players.setPosition");
  lua_pushboolean(L, doSetPosition(a, id, c[0], c[1], c[2], want));
  return 1;
}

// 1 = ok; -1 = erro (mensagem em e).
CBAPI_NOINLINE int doBan(ApiRes* a, const char* did, size_t didLen, const char* reason, size_t reasonLen,
                         lua_Integer dur, Err& e) {
  if (a->ctx->abandoned()) {
    setErr(e, "cb.players.ban: host de scripts encerrado");
    return -1;
  }
  if (!a->sh->o.bans) {
    setErr(e, "cb.players.ban: bans não configurados neste servidor");
    return -1;
  }
  try {
    Ban b;
    b.did.assign(did, didLen);
    b.reason = clampUtf8(std::string_view(reason, reasonLen), kMaxReason);
    b.by = "resource:" + a->h->name();
    b.createdAt = a->sh->unixNow();
    b.expiresAt = dur > 0 ? b.createdAt + dur : 0;
    std::string err;
    if (!a->sh->o.bans->add(b, err))
      a->ctx->log(LogLevel::Error, "[" + a->h->name() + "] cb.players.ban: " + err + " (o ban vale até reiniciar)");
    const std::string why = clampUtf8("banido: " + b.reason, kMaxReason);
    // Resolvido na simulação: pega também quem entrou e ainda não chegou aos scripts.
    if (!a->ctx->emit(CmdKickDid{b.did, why}))
      a->ctx->log(LogLevel::Critical, "[" + a->h->name() + "] cb.players.ban: expulsão não entregue à simulação");
    auditLine(*a, {{"type", "ban"}, {"did", b.did}, {"reason", b.reason}, {"expiresAt", b.expiresAt}});
    return 1;
  } catch (...) {
    setErr(e, "cb.players.ban: sem memória");
    return -1;
  }
}

const char* checkDid(lua_State* L, int idx, const char* fn, size_t* len) {
  if (lua_type(L, idx) != LUA_TSTRING) luaL_error(L, "%s: did inválido", fn);
  const char* s = lua_tolstring(L, idx, len);
  if (*len == 0 || *len > 64) luaL_error(L, "%s: did inválido", fn);
  return s;
}

int l_players_ban(lua_State* L) {
  ApiRes* a = self(L);
  size_t dl = 0, rl = 0;
  const char* did = checkDid(L, 1, "cb.players.ban", &dl);
  const char* reason = optText(L, 2, "cb.players.ban", "o motivo", "banido", &rl);
  lua_Integer dur = 0;
  if (!lua_isnoneornil(L, 3)) {
    int isnum = 0;
    dur = lua_type(L, 3) == LUA_TNUMBER ? lua_tointegerx(L, 3, &isnum) : 0;
    if (!isnum || dur <= 0 || dur > (lua_Integer{1} << 40))
      return luaL_error(L, "cb.players.ban: duração inválida (segundos inteiros > 0, ou nil = permanente)");
  }
  Err e;
  if (doBan(a, did, dl, reason, rl, dur, e) < 0) return luaL_error(L, "%s", e.msg);
  lua_pushboolean(L, 1);
  return 1;
}

// 1 = removido, 0 = não havia, -1 = erro.
CBAPI_NOINLINE int doUnban(ApiRes* a, const char* did, size_t len, Err& e) {
  if (a->ctx->abandoned() || !a->sh->o.bans) {
    setErr(e, "cb.players.unban: bans não configurados neste servidor");
    return -1;
  }
  try {
    std::string d(did, len), err;
    bool removed = false;
    if (!a->sh->o.bans->remove(d, removed, err)) a->ctx->log(LogLevel::Error, "[" + a->h->name() + "] cb.players.unban: " + err);
    if (removed) auditLine(*a, {{"type", "unban"}, {"did", d}});
    return removed ? 1 : 0;
  } catch (...) {
    setErr(e, "cb.players.unban: sem memória");
    return -1;
  }
}

int l_players_unban(lua_State* L) {
  ApiRes* a = self(L);
  size_t len = 0;
  const char* did = checkDid(L, 1, "cb.players.unban", &len);
  Err e;
  int rc = doUnban(a, did, len, e);
  if (rc < 0) return luaL_error(L, "%s", e.msg);
  lua_pushboolean(L, rc);
  return 1;
}

// -------------------------------------------------------------------------------------------- cb.commands

bool validCommandName(const char* s, size_t n) {
  if (n == 0 || n > 32) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
          c == '.'))
      return false;
  }
  return true;
}

// 0 = ok, -1 = erro. Em erro o chamador solta a ref.
CBAPI_NOINLINE int addCommand(ApiRes* a, lua_State* running, const char* name, size_t len, int ref, bool admin,
                              Err& e) {
  try {
    auto it = a->sh->commands.find(std::string_view(name, len));
    if (it != a->sh->commands.end()) {
      if (it->second.owner != a) {
        setErr(e, "cb.commands.register: comando '" + std::string(name, len) + "' já registrado por '" +
                      it->second.owner->h->name() + "'");
        return -1;
      }
      it->second.fn.release(running);  // mesmo resource: substitui
      it->second.fn = Ref(a->L, ref);
      it->second.admin = admin;
      return 0;
    }
    a->sh->commands.emplace(std::string(name, len), Command{a, Ref(a->L, ref), admin});
    return 0;
  } catch (...) {
    setErr(e, "cb.commands.register: sem memória");
    return -1;
  }
}

int l_commands_register(lua_State* L) {
  ApiRes* a = self(L);
  size_t len = 0;
  const char* name = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &len) : nullptr;
  if (!name || !validCommandName(name, len))
    return luaL_error(L, "cb.commands.register: nome inválido (1 a 32 de A-Z a-z 0-9 _ - .)");
  checkFn(L, 2, "cb.commands.register");
  bool admin = false;
  if (!lua_isnoneornil(L, 3)) {
    if (lua_type(L, 3) != LUA_TTABLE) return luaL_error(L, "cb.commands.register: opções devem ser uma tabela");
    lua_getfield(L, 3, "admin");
    admin = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
  }
  int ref = pushRef(L, 2);
  Err e;
  if (addCommand(a, L, name, len, ref, admin, e) < 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return luaL_error(L, "%s", e.msg);
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------- cb.timers

// id > 0 = criado; 0 = limite; -1 = sem memória.
CBAPI_NOINLINE lua_Integer addTimer(ApiRes* a, double ms, bool repeat, int ref) {
  if (a->timers.size() >= a->sh->o.maxTimersPerResource) return 0;
  try {
    const uint64_t id = a->nextTimer++;
    const double sec = ms / 1000.0;
    Timer t;
    t.due = a->ctx->now() + sec;
    t.interval = repeat ? sec : 0;
    t.fn = Ref(a->L, ref);
    a->due.emplace(t.due, id);
    a->timers.emplace(id, std::move(t));
    return static_cast<lua_Integer>(id);
  } catch (...) {
    return -1;
  }
}

int newTimer(lua_State* L, bool repeat) {
  ApiRes* a = self(L);
  const char* fn = repeat ? "cb.timers.every" : "cb.timers.after";
  if (lua_type(L, 1) != LUA_TNUMBER) return luaL_error(L, "%s: tempo em ms esperado", fn);
  const double ms = lua_tonumber(L, 1);
  if (!(ms >= 0) || ms > 1e12) return luaL_error(L, "%s: tempo em ms inválido", fn);
  if (repeat && ms < a->sh->o.minEveryMs)
    return luaL_error(L, "%s: intervalo mínimo de %d ms", fn, static_cast<int>(a->sh->o.minEveryMs));
  checkFn(L, 2, fn);
  int ref = pushRef(L, 2);
  lua_Integer id = addTimer(a, ms, repeat, ref);
  if (id <= 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    if (id == 0)
      return luaL_error(L, "%s: limite de %d timers por resource", fn,
                        static_cast<int>(a->sh->o.maxTimersPerResource));
    return luaL_error(L, "%s: sem memória", fn);
  }
  lua_pushinteger(L, id);
  return 1;
}
int l_timers_after(lua_State* L) { return newTimer(L, false); }
int l_timers_every(lua_State* L) { return newTimer(L, true); }

CBAPI_NOINLINE bool cancelTimer(ApiRes* a, lua_State* running, lua_Integer id) {
  auto it = a->timers.find(static_cast<uint64_t>(id));
  if (it == a->timers.end()) return false;
  it->second.fn.release(running);
  a->due.erase({it->second.due, it->first});
  a->timers.erase(it);
  return true;
}

int l_timers_cancel(lua_State* L) {
  ApiRes* a = self(L);
  int isnum = 0;
  lua_Integer id = lua_type(L, 1) == LUA_TNUMBER ? lua_tointegerx(L, 1, &isnum) : 0;
  lua_pushboolean(L, isnum && id > 0 && cancelTimer(a, L, id));
  return 1;
}

// ------------------------------------------------------------------------------------------ cb.log / audit

CBAPI_NOINLINE void doLog(ApiRes* a, LogLevel lv, const char* s, size_t n) {
  try {
    uint64_t summary = 0;
    const bool ok = a->logRate.take(summary);
    if (summary > 0)
      a->ctx->log(LogLevel::Warn, "[" + a->h->name() + "] " + std::to_string(summary) +
                                      " linhas de cb.log descartadas (limite de 200 por segundo)");
    if (!ok) {
      if (a->sh->o.stats) a->sh->o.stats->logDropped.fetch_add(1);
      return;
    }
    std::string line = "[" + a->h->name() + "] ";
    if (n > kMaxLogLine) {
      line += escapeLogText(clampUtf8(std::string_view(s, n), kMaxLogLine));
      line += "...(truncado, " + std::to_string(n) + " bytes)";
    } else {
      line += escapeLogText(std::string_view(s, n));
    }
    a->ctx->log(lv, line);
  } catch (...) {
  }
}

int logAt(lua_State* L, LogLevel lv) {
  ApiRes* a = self(L);
  const int n = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; ++i) {
    if (i > 1) luaL_addchar(&b, ' ');
    luaL_tolstring(L, i, nullptr);
    luaL_addvalue(&b);
  }
  luaL_pushresult(&b);
  size_t len = 0;
  const char* s = lua_tolstring(L, -1, &len);
  doLog(a, lv, s, len);
  return 0;
}
int l_log_info(lua_State* L) { return logAt(L, LogLevel::Info); }
int l_log_warn(lua_State* L) { return logAt(L, LogLevel::Warn); }
int l_log_error(lua_State* L) { return logAt(L, LogLevel::Error); }

CBAPI_NOINLINE int doAudit(ApiRes* a, lua_State* L, const char* type, size_t len, lua_Integer pid, Err& e) {
  try {
    std::string data, err;
    if (!luaToJson(L, 2, data, err, a->json(a->sh->o.maxLocalBytes))) {
      setErr(e, "cb.audit: dados inválidos: " + err);
      return -1;
    }
    nlohmann::json line{{"type", std::string(type, len)}, {"data", nlohmann::json::parse(data)}};
    if (pid >= 1 && pid <= 65535)
      if (const Conn* c = connOf(*a->sh, static_cast<uint16_t>(pid))) {
        line["id"] = pid;
        line["did"] = c->did;
        line["sub"] = c->sub;
      }
    auditLine(*a, std::move(line), true);
    return 0;
  } catch (...) {
    setErr(e, "cb.audit: falha ao montar a linha");
    return -1;
  }
}

int l_audit(lua_State* L) {
  ApiRes* a = self(L);
  if (lua_type(L, 1) != LUA_TSTRING) return luaL_error(L, "cb.audit: tipo deve ser texto");
  size_t len = 0;
  const char* type = lua_tolstring(L, 1, &len);
  if (len == 0 || len > 64) return luaL_error(L, "cb.audit: tipo deve ter de 1 a 64 bytes");
  lua_settop(L, 2);
  lua_Integer pid = 0;
  if (lua_type(L, 2) == LUA_TTABLE) {
    lua_pushliteral(L, "id");
    lua_rawget(L, 2);
    if (lua_isinteger(L, -1)) pid = lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  Err e;
  if (doAudit(a, L, type, len, pid, e) < 0) return luaL_error(L, "%s", e.msg);
  return 0;
}

// ---------------------------------------------------------------------------------------------- cb.resource

// Chamada da API num resource, contando reentrância.
Resource::CallResult invokeOn(ApiRes& a, const sol::reference& fn, std::span<const sol::object> args) {
  ++a.depth;
  auto r = a.ctx->invoke(*a.h, fn, args);
  --a.depth;
  return r;
}

ApiRes* apiOf(HostedResource* h) { return h ? static_cast<ApiRes*>(h->api.get()) : nullptr; }

// Resource alvo de cb.resource, rodando e ocioso; nullptr + erro.
ApiRes* target(ApiRes* a, std::string_view name, Err& e) {
  HostedResource* h = a->ctx->find(name);
  ApiRes* b = apiOf(h);
  if (!h || !h->running() || !h->hasState() || !b) {
    setErr(e, "cb.resource: resource '" + std::string(name) + "' não está rodando");
    return nullptr;
  }
  if (b->depth > 0) {
    setErr(e, "cb.resource: chamada reentrante para '" + std::string(name) + "' (ele está no meio de uma chamada)");
    return nullptr;
  }
  return b;
}

// Empilha em bL o valor exports[key] (ou nil). false se exports não for tabela.
bool pushExport(ApiRes* b, const char* key, size_t klen) {
  lua_State* bL = b->L;
  if (!lua_checkstack(bL, 4)) return false;
  sol::object ex = b->h->lua().exports();
  ex.push(bL);
  if (lua_type(bL, -1) != LUA_TTABLE) {
    lua_pop(bL, 1);
    return false;
  }
  lua_pushlstring(bL, key, klen);
  lua_rawget(bL, -2);
  lua_remove(bL, -2);
  return true;
}

// __index do proxy. Retorna: 0 = nil, 1 = valor copiado empilhado em L, 2 = função (o chamador cria o proxy),
// -1 = erro.
CBAPI_NOINLINE int proxyIndex(ApiRes* a, lua_State* L, const char* res, size_t rlen, const char* key, size_t klen,
                              Err& e) {
  try {
    ApiRes* b = target(a, std::string_view(res, rlen), e);
    if (!b) return -1;
    if (!pushExport(b, key, klen)) {
      setErr(e, "cb.resource: '" + std::string(res, rlen) + "' não exporta uma tabela");
      return -1;
    }
    const int t = lua_type(b->L, -1);
    if (t == LUA_TNIL) {
      lua_pop(b->L, 1);
      return 0;
    }
    if (t == LUA_TFUNCTION) {
      lua_pop(b->L, 1);
      return 2;
    }
    std::string js, err;
    const bool ok = luaToJson(b->L, -1, js, err, b->json(a->sh->o.maxLocalBytes));
    lua_pop(b->L, 1);
    if (!ok || !jsonToLua(L, js, err, a->json(a->sh->o.maxLocalBytes))) {
      setErr(e, "cb.resource: valor '" + std::string(res, rlen) + "." + std::string(key, klen) + "' não copiável: " + err);
      return -1;
    }
    return 1;
  } catch (...) {
    setErr(e, "cb.resource: sem memória");
    return -1;
  }
}

// Chamada a exports[key] de outro resource. 0 = nil, 1 = valor empilhado em L, -1 = erro.
CBAPI_NOINLINE int proxyCall(ApiRes* a, lua_State* L, int nargs, const char* res, size_t rlen, const char* key,
                             size_t klen, Err& e) {
  try {
    const std::string what = std::string(res, rlen) + "." + std::string(key, klen);
    ApiRes* b = target(a, std::string_view(res, rlen), e);
    if (!b) return -1;
    std::vector<std::string> js(static_cast<size_t>(nargs));
    for (int i = 0; i < nargs; ++i) {
      std::string err;
      if (!luaToJson(L, i + 1, js[static_cast<size_t>(i)], err, a->json(a->sh->o.maxLocalBytes))) {
        setErr(e, "cb.resource: argumento " + std::to_string(i + 1) + " de '" + what + "' não copiável: " + err);
        return -1;
      }
    }
    lua_State* bL = b->L;
    if (!pushExport(b, key, klen)) {
      setErr(e, "cb.resource: '" + std::string(res, rlen) + "' não exporta uma tabela");
      return -1;
    }
    if (lua_type(bL, -1) != LUA_TFUNCTION) {
      lua_pop(bL, 1);
      setErr(e, "cb.resource: '" + what + "' não é uma função");
      return -1;
    }
    sol::reference fn(bL, -1);
    lua_pop(bL, 1);
    std::vector<sol::object> args;
    args.reserve(js.size());
    for (const auto& s : js) {
      std::string err;
      if (!jsonToLua(bL, s, err, b->json(a->sh->o.maxLocalBytes))) {
        setErr(e, "cb.resource: argumento de '" + what + "': " + err);
        return -1;
      }
      args.emplace_back(bL, -1);
      lua_pop(bL, 1);
    }
    auto r = invokeOn(*b, fn, args);
    if (!r.ok) {
      setErr(e, "cb.resource: erro em '" + what + "' (ver log)");
      return -1;
    }
    if (r.yielded) {
      setErr(e, "cb.resource: '" + what + "' cedeu (operação assíncrona não é suportada entre resources)");
      return -1;
    }
    if (!r.value.valid() || r.value.get_type() == sol::type::lua_nil || r.value.get_type() == sol::type::none)
      return 0;
    std::string out, err;
    r.value.push(bL);
    const bool ok = luaToJson(bL, -1, out, err, b->json(a->sh->o.maxLocalBytes));
    lua_pop(bL, 1);
    if (!ok || !jsonToLua(L, out, err, a->json(a->sh->o.maxLocalBytes))) {
      setErr(e, "cb.resource: retorno de '" + what + "' não copiável: " + err);
      return -1;
    }
    return 1;
  } catch (...) {
    setErr(e, "cb.resource: sem memória");
    return -1;
  }
}

// Upvalues: 1 = ApiRes, 2 = nome do resource, 3 = chave.
int l_proxy_call(lua_State* L) {
  ApiRes* a = self(L);
  size_t rlen = 0, klen = 0;
  const char* res = lua_tolstring(L, lua_upvalueindex(2), &rlen);
  const char* key = lua_tolstring(L, lua_upvalueindex(3), &klen);
  const int nargs = lua_gettop(L);
  if (!lua_checkstack(L, 4)) return luaL_error(L, "cb.resource: pilha esgotada");
  Err e;
  const int rc = proxyCall(a, L, nargs, res, rlen, key, klen, e);
  if (rc < 0) return luaL_error(L, "%s", e.msg);
  if (rc == 0) lua_pushnil(L);
  return 1;
}

// __index(proxy, key). Upvalues: 1 = ApiRes, 2 = nome do resource.
int l_proxy_index(lua_State* L) {
  ApiRes* a = self(L);
  if (lua_type(L, 2) != LUA_TSTRING) {
    lua_pushnil(L);
    return 1;
  }
  size_t rlen = 0, klen = 0;
  const char* res = lua_tolstring(L, lua_upvalueindex(2), &rlen);
  const char* key = lua_tolstring(L, 2, &klen);
  if (!lua_checkstack(L, 4)) return luaL_error(L, "cb.resource: pilha esgotada");
  Err e;
  const int rc = proxyIndex(a, L, res, rlen, key, klen, e);
  if (rc < 0) return luaL_error(L, "%s", e.msg);
  if (rc == 0) lua_pushnil(L);
  if (rc == 2) {
    lua_pushlightuserdata(L, a);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, l_proxy_call, 3);
  }
  return 1;
}

int l_readonly(lua_State* L) { return luaL_error(L, "%s é somente leitura", lua_tostring(L, lua_upvalueindex(1))); }

CBAPI_NOINLINE int checkTarget(ApiRes* a, const char* name, size_t len, Err& e) {
  try {
    if (std::string_view(name, len) == a->h->name()) {
      setErr(e, "cb.resource: um resource não pode chamar a si mesmo");
      return -1;
    }
    return target(a, std::string_view(name, len), e) ? 0 : -1;
  } catch (...) {
    setErr(e, "cb.resource: sem memória");
    return -1;
  }
}

int l_resource(lua_State* L) {
  ApiRes* a = self(L);
  if (lua_type(L, 1) != LUA_TSTRING) return luaL_error(L, "cb.resource: nome deve ser texto");
  size_t len = 0;
  const char* name = lua_tolstring(L, 1, &len);
  if (std::memchr(name, 0, len) != nullptr) return luaL_error(L, "cb.resource: nome contém NUL");
  Err e;
  if (checkTarget(a, name, len, e) < 0) return luaL_error(L, "%s", e.msg);
  lua_createtable(L, 0, 0);                 // proxy
  lua_createtable(L, 0, 3);                 // metatable
  lua_pushlightuserdata(L, a);
  lua_pushvalue(L, 1);
  lua_pushcclosure(L, l_proxy_index, 2);
  lua_setfield(L, -2, "__index");
  lua_pushliteral(L, "cb.resource");
  lua_pushcclosure(L, l_readonly, 1);
  lua_setfield(L, -2, "__newindex");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, -2);
  return 1;
}

// ------------------------------------------------------------------------------- tabelas somente leitura

// Metamétodos do proxy somente leitura: o original fica em __index da metatable.
void pushOriginal(lua_State* L, int proxy) {
  if (!lua_getmetatable(L, proxy)) {
    lua_pushnil(L);
    return;
  }
  lua_pushliteral(L, "__index");
  lua_rawget(L, -2);
  lua_remove(L, -2);
}
// next(proxy, k) sobre a tabela original (upvalue 1): o script nunca recebe a original.
int l_ro_next(lua_State* L) {
  lua_settop(L, 2);
  lua_pushvalue(L, 2);
  if (lua_next(L, lua_upvalueindex(1))) return 2;
  lua_pushnil(L);
  return 1;
}
int l_ro_pairs(lua_State* L) {
  pushOriginal(L, 1);
  if (lua_type(L, -1) != LUA_TTABLE) return luaL_error(L, "pairs: tabela somente leitura inválida");
  lua_pushcclosure(L, l_ro_next, 1);
  lua_pushvalue(L, 1);
  lua_pushnil(L);
  return 3;
}
int l_ro_len(lua_State* L) {
  pushOriginal(L, 1);
  lua_pushinteger(L, static_cast<lua_Integer>(lua_rawlen(L, -1)));
  return 1;
}

// Troca a tabela no topo por um proxy somente leitura (recursivo). Host (onSetup), estado ocioso.
void makeReadOnly(lua_State* L, const char* what, int depth) {
  const int t = lua_gettop(L);
  if (depth < 40) {
    lua_pushnil(L);
    while (lua_next(L, t)) {  // [t, k, v]
      if (lua_type(L, -1) == LUA_TTABLE) {
        makeReadOnly(L, what, depth + 1);  // [t, k, proxy]
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, t);  // t[k] = proxy (chave já existente: permitido durante a travessia)
      } else {
        lua_pop(L, 1);
      }
    }
  }
  lua_createtable(L, 0, 0);  // [t, proxy]
  lua_createtable(L, 0, 5);  // [t, proxy, mt]
  lua_pushvalue(L, t);
  lua_setfield(L, -2, "__index");
  lua_pushstring(L, what);
  lua_pushcclosure(L, l_readonly, 1);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, l_ro_pairs);
  lua_setfield(L, -2, "__pairs");
  lua_pushcfunction(L, l_ro_len);
  lua_setfield(L, -2, "__len");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, -2);  // [t, proxy]
  lua_remove(L, t);         // [proxy]
}

// ------------------------------------------------------------------------------------------- montagem (host)

struct Fn {
  const char* name;
  lua_CFunction f;
};

// Cria em L (topo) uma tabela com as funções, e registra cópia em _LOADED[modName] (nomes nos tracebacks).
void pushModule(lua_State* L, ApiRes* a, const char* modName, std::initializer_list<Fn> fns) {
  lua_createtable(L, 0, static_cast<int>(fns.size()));
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_createtable(L, 0, static_cast<int>(fns.size()));  // [mod, loaded, snap]
  for (const Fn& f : fns) {
    lua_pushlightuserdata(L, a);
    lua_pushcclosure(L, f.f, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, f.name);  // snap
    lua_setfield(L, -4, f.name);  // mod
  }
  lua_setfield(L, -2, modName);  // loaded[modName] = snap
  lua_pop(L, 1);
}

void buildApi(ApiRes* a) {
  lua_State* L = a->L;
  if (!lua_checkstack(L, 16)) throw std::runtime_error("cbscript: pilha do Lua esgotada ao montar cb.*");
  const int top = lua_gettop(L);
  // cb.null: lightuserdata (imutável, único, igual em todos os estados).
  lua_pushlightuserdata(L, &gNullTag);
  a->nullRef = luaL_ref(L, LUA_REGISTRYINDEX);

  pushModule(L, a, "cb", {{"on", l_on}, {"audit", l_audit}, {"resource", l_resource}});  // [cb]
  pushModule(L, a, "cb.events",
             {{"on", l_events_on}, {"emit", l_events_emit}, {"onLocal", l_events_onLocal},
              {"emitLocal", l_events_emitLocal}});
  lua_setfield(L, -2, "events");
  pushModule(L, a, "cb.players",
             {{"list", l_players_list}, {"get", l_players_get}, {"kick", l_players_kick}, {"ban", l_players_ban},
              {"unban", l_players_unban}, {"setPosition", l_players_setPosition}});
  lua_setfield(L, -2, "players");
  pushModule(L, a, "cb.commands", {{"register", l_commands_register}});
  lua_setfield(L, -2, "commands");
  pushModule(L, a, "cb.timers", {{"after", l_timers_after}, {"every", l_timers_every}, {"cancel", l_timers_cancel}});
  lua_setfield(L, -2, "timers");
  pushModule(L, a, "cb.log", {{"info", l_log_info}, {"warn", l_log_warn}, {"error", l_log_error}});
  lua_setfield(L, -2, "log");
  lua_pushlightuserdata(L, &gNullTag);
  lua_setfield(L, -2, "null");

  // cb.config: cópia somente leitura do "config" do manifest.
  std::string err;
  JsonOptions jo = a->json(size_t{16} << 20);
  jo.maxDepth = 32;
  const std::string cfg = a->h->manifest().config.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  if (!jsonToLua(L, cfg, err, jo)) throw std::runtime_error("cbscript: 'config' do resource.json: " + err);
  if (lua_type(L, -1) == LUA_TTABLE) makeReadOnly(L, "cb.config", 0);
  lua_setfield(L, -2, "config");

  lua_pushglobaltable(L);
  lua_pushliteral(L, "cb");
  lua_pushvalue(L, -3);
  lua_rawset(L, -3);
  lua_settop(L, top);
}

// ------------------------------------------------------------------------------------------- despacho (host)

// Tabela do jogador no estado principal de `a`.
sol::object playerObj(ApiRes& a, const PlayerView& p) {
  pushPlayer(a.L, p);
  sol::object o(a.L, -1);
  lua_pop(a.L, 1);
  return o;
}

PlayerView viewOf(const Shared& sh, ScriptContext& ctx, uint16_t id, const Conn* c, const std::string& didFallback) {
  PlayerView v;
  v.id = id;
  if (c) {
    v.name = c->name;
    v.did = c->did;
    v.sub = c->sub;
  } else {
    v.did = didFallback;
  }
  (void)sh;
  v.session = c ? c->session : 0;
  PlayersSnapshot snap = ctx.players();
  for (const auto& p : *snap)
    if (p.id == id && c && p.session == c->session) {  // posição só da mesma sessão (id pode ter sido reaproveitado)
      v.x = p.x;
      v.y = p.y;
      v.z = p.z;
      v.state = p.state;
      break;
    }
  return v;
}

void dispatchOn(Shared& sh, OnKind kind, const PlayerView* p, const std::string* reason) {
  for (size_t i = 0; i < sh.live.size(); ++i) {
    ApiRes& a = *sh.live[i];
    auto& hs = a.on[kind];
    if (hs.empty()) continue;
    std::vector<sol::object> args;
    if (p) args.push_back(playerObj(a, *p));
    if (reason) args.push_back(sol::make_object(a.h->lua().lua(), *reason));
    for (size_t k = 0; k < hs.size(); ++k) invokeOn(a, hs[k].get(), args);
  }
}

bool decodeFor(Shared& sh, ApiRes& a, std::string_view json, size_t maxBytes, sol::object& out, std::string& err) {
  if (!jsonToLua(a.L, json, err, a.json(maxBytes))) return false;
  out = sol::object(a.L, -1);
  lua_pop(a.L, 1);
  (void)sh;
  return true;
}

void warnJson(Shared& sh, ScriptContext& ctx, const std::string& msg) {
  const double now = ctx.now();
  if (now - sh.lastJsonWarn < 1.0) return;  // no máximo 1 aviso por segundo (cliente malicioso)
  sh.lastJsonWarn = now;
  ctx.log(LogLevel::Warn, msg);
}

void onClientEvent(Shared& sh, ScriptContext& ctx, EvClientEvent& ev) {
  const Conn* c = connOf(sh, ev.id);
  if (!c) return;  // jogador já saiu (ou nunca entrou)
  if (ev.session != 0 && ev.session != c->session) return;  // evento de uma sessão anterior com o mesmo id
  PlayerView pv;
  bool havePv = false;
  for (size_t i = 0; i < sh.live.size(); ++i) {
    ApiRes& a = *sh.live[i];
    auto it = a.net.find(std::string_view(ev.name));
    if (it == a.net.end() || it->second.empty()) continue;
    if (!havePv) {
      pv = viewOf(sh, ctx, ev.id, c, c->did);
      havePv = true;
    }
    sol::object data;
    std::string err;
    if (!decodeFor(sh, a, ev.json, kMaxPayload, data, err)) {
      warnJson(sh, ctx,
               "evento '" + escapeLogText(ev.name) + "' do jogador " + std::to_string(ev.id) + " descartado: " + err);
      return;
    }
    std::array<sol::object, 2> args{playerObj(a, pv), data};
    auto& hs = it->second;
    for (size_t k = 0; k < hs.size(); ++k) invokeOn(a, hs[k].get(), args);
  }
}

// Texto JSON (string) de no máximo kMaxPayload bytes: corta o texto (sem quebrar UTF-8) e acrescenta "…".
std::string replyJson(const std::string& text) {
  auto dump = [](const std::string& t) {
    return nlohmann::json(t).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  };
  std::string js = dump(text);
  if (js.size() <= kMaxPayload) return js;
  // Maior corte (em bytes do texto, sem quebrar UTF-8) cujo JSON com "\u2026" cabe: busca bin\u00e1ria. Escapes (\u0001 = 6
  // bytes) e bytes inv\u00e1lidos (U+FFFD = 3 bytes) fazem o JSON crescer mais que o texto; um corte proporcional
  // simples podia cair para s\u00f3 "\u2026".
  size_t lo = 1, hi = std::min(text.size(), kMaxPayload);
  std::string best = dump("\u2026");
  while (lo <= hi) {
    const size_t mid = lo + (hi - lo) / 2;
    std::string cand = dump(clampUtf8(text, mid) + "\u2026");
    if (cand.size() <= kMaxPayload) {
      best = std::move(cand);
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return best;
}

void reply(ScriptContext& ctx, EvCommand& ev, uint32_t session, const std::string& text) {
  if (ev.reply) {
    ev.reply(text);
  } else if (ev.from) {
    ctx.emit(CmdEmit{*ev.from, "cb:reply", replyJson(text), session, 0});
  }
}

void onCommand(Shared& sh, ScriptContext& ctx, EvCommand& ev) {
  const Conn* c = nullptr;
  uint32_t session = 0;
  if (ev.from) {
    c = connOf(sh, *ev.from);
    if (!c) return;  // jogador já saiu
    session = c->session;
  }
  std::string_view line = ev.line;
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
  if (!line.empty() && line.front() == '/') line.remove_prefix(1);
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n'))
    line.remove_suffix(1);
  std::vector<std::string> tok;
  for (size_t i = 0; i < line.size();) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    if (j > i) tok.emplace_back(line.substr(i, j - i));
    i = j;
  }
  if (tok.empty()) return;
  auto it = sh.commands.find(std::string_view(tok[0]));
  if (it == sh.commands.end()) {
    // Eco curto: a linha veio do cliente (até 4 KiB) e a resposta tem de caber num evento.
    std::string shown = clampUtf8(tok[0], 32);
    if (shown.size() < tok[0].size()) shown += "\u2026";
    reply(ctx, ev, session, "comando desconhecido: " + shown);
    return;
  }
  Command& cmd = it->second;
  if (ev.from && cmd.admin && !(sh.o.isAdmin && sh.o.isAdmin(c->did))) {
    reply(ctx, ev, session, "sem permissão para '/" + tok[0] + "'");
    return;
  }
  ApiRes& a = *cmd.owner;
  if (!ev.from)
    auditLine(a, {{"type", "command"}, {"by", ev.adminDid.empty() ? "console" : ev.adminDid}, {"line", std::string(line)}});
  else if (cmd.admin)  // comando de admin digitado no chat
    auditLine(a, {{"type", "command"}, {"by", c->did}, {"id", *ev.from}, {"line", std::string(line)}});
  lua_State* L = a.L;
  lua_createtable(L, static_cast<int>(tok.size() - 1), 0);
  for (size_t i = 1; i < tok.size(); ++i) {
    lua_pushlstring(L, tok[i].data(), tok[i].size());
    lua_rawseti(L, -2, static_cast<lua_Integer>(i));
  }
  sol::object argsT(L, -1);
  lua_pop(L, 1);
  std::array<sol::object, 3> args{sol::make_object(a.h->lua().lua(), ev.from ? *ev.from : 0), argsT,
                                  sol::make_object(a.h->lua().lua(), std::string(line))};
  sol::reference fn = cmd.fn.get();
  const std::string name = tok[0];
  auto r = invokeOn(a, fn, args);
  if (!r.ok) {
    reply(ctx, ev, session, "erro ao executar '" + name + "' (ver log)");
  } else if (!r.yielded && r.value.valid() && r.value.get_type() == sol::type::string) {
    reply(ctx, ev, session, r.value.as<std::string>());
  }
}

void drainLocal(Shared& sh) {
  if (sh.draining) return;
  sh.draining = true;
  for (int round = 0; round < kLocalRounds && !sh.localQ.empty(); ++round) {
    std::deque<LocalEvent> batch;
    batch.swap(sh.localQ);
    for (auto& ev : batch) {
      for (size_t i = 0; i < sh.live.size(); ++i) {
        ApiRes& a = *sh.live[i];
        auto it = a.local.find(std::string_view(ev.name));
        if (it == a.local.end() || it->second.empty()) continue;
        sol::object data;
        std::string err;
        if (!decodeFor(sh, a, ev.json, sh.o.maxLocalBytes, data, err)) continue;  // já validado na saída
        std::array<sol::object, 2> args{data, sol::make_object(a.h->lua().lua(), ev.from)};
        auto& hs = it->second;
        for (size_t k = 0; k < hs.size(); ++k) invokeOn(a, hs[k].get(), args);
      }
    }
  }
  sh.draining = false;
}

void fireTimers(Shared& sh, ScriptContext& ctx) {
  if (sh.live.empty()) return;
  const double now = ctx.now();
  for (size_t i = 0; i < sh.live.size(); ++i) {
    ApiRes& a = *sh.live[i];
    if (a.due.empty() || a.due.begin()->first > now) continue;
    sh.dueScratch.clear();
    for (auto it = a.due.begin(); it != a.due.end() && it->first <= now; ++it) sh.dueScratch.push_back(it->second);
    for (uint64_t id : sh.dueScratch) {
      auto t = a.timers.find(id);
      if (t == a.timers.end()) continue;  // cancelado por um timer anterior
      sol::reference fn = t->second.fn.get();
      a.due.erase({t->second.due, id});
      if (t->second.interval > 0) {
        double next = t->second.due + t->second.interval;
        if (next <= now) next = now + t->second.interval;  // atrasado: não tenta compensar
        t->second.due = next;
        a.due.emplace(next, id);
      } else {
        a.timers.erase(t);  // a ref do Timer some aqui; `fn` mantém a função viva para esta chamada
      }
      invokeOn(a, fn, {});
    }
  }
}

}  // namespace

void installApi(HostOptions& opt, ApiOptions api) {
  auto sh = std::make_shared<Shared>();
  sh->o = std::move(api);
  auto prevSetup = std::move(opt.onSetup);
  auto prevStarted = std::move(opt.onStarted);
  auto prevStopping = std::move(opt.onStopping);
  auto prevEvent = std::move(opt.onEvent);
  auto prevTick = std::move(opt.onTick);

  opt.onSetup = [sh, prevSetup](ScriptContext& ctx, HostedResource& h) {
    auto a = std::make_shared<ApiRes>();
    a->sh = sh;
    a->ctx = &ctx;
    a->h = &h;
    a->L = h.lua().lua().lua_state();
    a->source = sh->nextSource++;
    if (a->source == 0) a->source = sh->nextSource++;
    h.api = a;
    buildApi(a.get());
    if (prevSetup) prevSetup(ctx, h);
  };
  opt.onStarted = [sh, prevStarted](ScriptContext& ctx, HostedResource& h) {
    if (ApiRes* a = apiOf(&h)) {
      if (!a->live) {
        a->live = true;
        sh->live.push_back(a);
      }
      for (size_t k = 0; k < a->on[OnStart].size(); ++k) invokeOn(*a, a->on[OnStart][k].get(), {});
    }
    drainLocal(*sh);
    if (prevStarted) prevStarted(ctx, h);
  };
  opt.onStopping = [sh, prevStopping](ScriptContext& ctx, HostedResource& h) {
    if (ApiRes* a = apiOf(&h)) {
      for (size_t k = 0; k < a->on[OnStop].size(); ++k) invokeOn(*a, a->on[OnStop][k].get(), {});
      std::erase(sh->live, a);
      a->live = false;
      for (auto it = sh->commands.begin(); it != sh->commands.end();)
        it = it->second.owner == a ? sh->commands.erase(it) : std::next(it);
    }
    drainLocal(*sh);
    if (prevStopping) prevStopping(ctx, h);
  };
  opt.onEvent = [sh, prevEvent](ScriptContext& ctx, SimToScript& ev) {
    std::visit(
        [&](auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, EvPlayerJoin>) {
            // Ban gravado depois da admissão deste jogador (o Join ainda estava na fila): expulsa já.
            if (sh->o.bans) {
              if (auto ban = sh->o.bans->check(e.p.did, sh->unixNow())) {
                ctx.emit(CmdKick{e.p.id, clampUtf8("banido: " + ban->reason, kMaxReason), e.p.session});
                sh->players.erase(e.p.id);
                return;
              }
            }
            sh->players[e.p.id] = Conn{e.p.name, e.p.did, e.p.sub, e.p.session};
            dispatchOn(*sh, OnJoin, &e.p, nullptr);
          } else if constexpr (std::is_same_v<T, EvPlayerLeave>) {
            auto it = sh->players.find(e.id);
            if (it == sh->players.end() || (e.session != 0 && it->second.session != e.session)) return;
            PlayerView pv = viewOf(*sh, ctx, e.id, &it->second, e.did);
            sh->players.erase(it);
            dispatchOn(*sh, OnLeave, &pv, &e.reason);
          } else if constexpr (std::is_same_v<T, EvClientEvent>) {
            onClientEvent(*sh, ctx, e);
          } else if constexpr (std::is_same_v<T, EvCommand>) {
            onCommand(*sh, ctx, e);
          }
        },
        ev);
    drainLocal(*sh);
    if (prevEvent) prevEvent(ctx, ev);
  };
  opt.onTick = [sh, prevTick](ScriptContext& ctx) {
    fireTimers(*sh, ctx);
    drainLocal(*sh);
    if (prevTick) prevTick(ctx);
  };
}

}  // namespace cbscript
