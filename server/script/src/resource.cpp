#include "cbscript/resource.hpp"

#include <cbmp/safe_path.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

// Sandbox (decisões; ver também .superpowers/sdd/sub3/task-2-report.md):
// - O ambiente é montado por lista branca numa tabela nova e depois instalado no _G (o _G original é esvaziado e
//   recebe só a lista branca, para que sol2 e o registro LUA_RIDX_GLOBALS continuem apontando para a mesma tabela).
//   Ficam: assert error getmetatable ipairs next pairs pcall rawequal rawget rawlen rawset select tonumber
//   tostring type xpcall _VERSION, string (sem dump), table, math, utf8, coroutine, os.{time,clock,date};
//   load/collectgarbage/setmetatable/print/require embrulhados. Sem io, debug, package, dofile, loadfile, warn,
//   string.dump. As bibliotecas originais só continuam no registro (inalcançável sem debug).
// - load: sempre modo "t"; ambiente = o 4º argumento se não for nil, senão o sandbox (desvio aceito da spec).
// - Metatable das strings: __index = string do sandbox e __metatable = false (getmetatable("") devolve false).
// - setmetatable recusa metatables com __gc: finalizadores rodam fora da vigia (em qualquer alocação/no close).
// - collectgarbage: só "count" e "step".
// - print: vai para o callback `warn` como "[<resource>] print: ..."; linha cortada em 64 KiB; no máximo 200 linhas
//   por segundo por resource, o excesso é contado e resumido uma vez por janela.
// - Memória: alocador por estado conta bytes; acima do teto devolve NULL (Lua lança erro de memória após tentar
//   coleta de emergência). O teto só vale enquanto código do script roda (dentro de lua_resume); o código do host
//   (montar estado, traceback, empilhar argumentos) não é limitado, para nunca cair no panic fora de proteção.
// - Vigia (sem hooks do Lua): Lua com patches (vcpkg-overlays/lua). A thread do relógio (uma por processo) só
//   grava uma flag no global_State (lua_cbmp_setinterrupt) quando o prazo de abortAfter vence; nunca toca outras
//   estruturas do Lua. A VM confere a flag em chamadas, retornos e saltos (todo laço Lua salta) e, nos laços C longos
//   da biblioteca (match de padrões a cada 4096 passos; insert/remove/move por iteração; cada comparação do sort),
//   via lua_cbmp_poll. Com a flag ligada, a VM chama interruptCheck na thread do Lua, que lança o erro de tempo.
//   A flag fica ligada até a chamada externa terminar, então pcall/xpcall/coroutine.resume/__close não engolem o
//   aborto: a primeira instrução Lua depois de qualquer captura lança de novo. A flag é do estado (global_State),
//   então vale para todas as corrotinas sem rastrear qual thread está rodando.
//   Aviso (warnAfter) é registrado ao fim da chamada, com a duração.
//   Limite que sobra: uma única operação C sem laço vigiado (string.rep gigante, comparação/concatenação de strings
//   enormes, GC de emergência perto do teto) só é interrompida quando termina. O teto de memória limita o tamanho;
//   o ScriptHost precisa de supervisor (ver report).

// Funções chamadas a partir de quadros que o Lua atravessa com longjmp (callback de interrupção, funções C do
// sandbox) ficam fora de linha: assim o quadro atravessado não tem objetos C++ com destrutor. No MSVC, um longjmp do
// lua.dll (C) atravessando um quadro C++ com estado de EH derrubou o processo (0xC0000409/0xC0000374).
#if defined(_MSC_VER)
#define CBSCRIPT_NOINLINE __declspec(noinline)
#else
#define CBSCRIPT_NOINLINE __attribute__((noinline))
#endif

namespace fs = std::filesystem;

// Contrato do patch C2 (vcpkg-overlays/lua/cbmp-c2-vm-interrupt.patch): o extra space de cada thread guarda um
// ponteiro para esta struct (layout C idêntico) ou NULL; `check` roda na thread do Lua com a flag ligada.
struct cbmp_interrupt {
  void (*check)(lua_State* L);
};

namespace cbscript {

namespace {

constexpr uintmax_t kMaxScriptBytes = uintmax_t{16} << 20;
constexpr size_t kMaxNameLen = 64;
constexpr size_t kMaxPrintBytes = size_t{64} << 10;
constexpr int kMaxPrintPerSec = 200;
char kLoadingSentinel;  // endereço usado como light userdata no cache do require

std::string u8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

fs::path fromU8(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

int64_t realMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

struct Resource::Shared {
  // alocador
  size_t used = 0;
  size_t limit = 0;
  bool enforce = false;
  // vigia (thread de script)
  std::chrono::milliseconds warnAfter{50}, abortAfter{5000};
  std::chrono::steady_clock::time_point start{};
  int depth = 0;
  bool aborting = false;
  // vigia (compartilhado com a thread do relógio; campos não atômicos só sob runMu)
  lua_State* mainL = nullptr;  // imutável depois do construtor
  std::mutex runMu;
  bool active = false;  // chamada externa em andamento
  int64_t abortAtMs = 0;
  std::atomic<bool> abortDue{false};
  // print
  std::chrono::steady_clock::time_point printWindow{};
  int printCount = 0;
  uint64_t printDropped = 0;
  // saída
  std::string name;
  std::function<void(std::string)> warn;

  CBSCRIPT_NOINLINE void emit(std::string msg) noexcept {
    if (!warn) return;
    try {
      warn(std::move(msg));
    } catch (...) {
    }
  }
  CBSCRIPT_NOINLINE void warnSlow(long long ms, bool aborted) noexcept {
    try {
      emit("[" + name + "] vigia: execução " + (aborted ? "abortada após " : "levou ") + std::to_string(ms) +
           " ms (aviso a partir de " + std::to_string(warnAfter.count()) + " ms, aborto em " +
           std::to_string(abortAfter.count()) + " ms)");
    } catch (...) {
    }
  }
  // Fecha a janela de 1 s de print se ela acabou, resumindo o que foi descartado.
  CBSCRIPT_NOINLINE void rollPrintWindow(std::chrono::steady_clock::time_point now) noexcept {
    if (now - printWindow < std::chrono::seconds(1)) return;
    if (printDropped > 0) {
      try {
        emit("[" + name + "] print: " + std::to_string(printDropped) + " linhas descartadas (limite " +
             std::to_string(kMaxPrintPerSec) + " por segundo)");
      } catch (...) {
      }
    }
    printWindow = now;
    printCount = 0;
    printDropped = 0;
  }
  CBSCRIPT_NOINLINE void printLine(const char* p, size_t n) noexcept {
    rollPrintWindow(std::chrono::steady_clock::now());
    if (printCount >= kMaxPrintPerSec) {
      ++printDropped;
      return;
    }
    ++printCount;
    try {
      if (n > kMaxPrintBytes)
        emit("[" + name + "] print: " + escapeLogText(std::string_view(p, kMaxPrintBytes)) + " ...(truncado, " +
             std::to_string(n) + " bytes)");
      else
        emit("[" + name + "] print: " + escapeLogText(std::string_view(p, n)));
    } catch (...) {
    }
  }
};

std::string escapeLogText(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
      out += "\\x";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else {
      out += ch;
    }
  }
  return out;
}

namespace {

using Shared = Resource::Shared;

struct EnforceOff {
  Shared& s;
  bool prev;
  explicit EnforceOff(Shared& sh) : s(sh), prev(sh.enforce) { s.enforce = false; }
  ~EnforceOff() { s.enforce = prev; }
  EnforceOff(const EnforceOff&) = delete;
  EnforceOff& operator=(const EnforceOff&) = delete;
};

void* luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  auto* s = static_cast<Shared*>(ud);
  if (ptr == nullptr) osize = 0;  // osize codifica o tipo do objeto quando ptr é NULL
  if (nsize == 0) {
    std::free(ptr);
    s->used -= osize;
    return nullptr;
  }
  if (s->enforce && nsize > osize && (s->used >= s->limit || nsize - osize > s->limit - s->used)) return nullptr;
  void* p = std::realloc(ptr, nsize);
  if (p == nullptr) return nullptr;
  s->used = s->used - osize + nsize;
  return p;
}

Shared* sharedOf(lua_State* L) {
  void* ud = nullptr;
  lua_getallocf(L, &ud);
  return static_cast<Shared*>(ud);
}

// Chamado pelo Lua com patch, na thread do Lua, quando a flag de interrupção está ligada. Enquanto a chamada estiver
// vencida, lança o erro (longjmp; quadro sem objetos C++). Flag velha (fora de chamada vencida): desliga.
void interruptCheck(lua_State* L) {
  Shared* s = sharedOf(L);
  if (s->depth > 0 && (s->aborting || s->abortDue.load(std::memory_order_acquire))) {
    s->aborting = true;
    luaL_error(L, "tempo esgotado: execução passou de %d ms (abortada pela vigia)",
               static_cast<int>(s->abortAfter.count()));
    return;
  }
  lua_cbmp_setinterrupt(L, 0);
}

const cbmp_interrupt kInterrupt{&interruptCheck};

// Resources vivos, percorridos pela thread do relógio. Estado propositalmente "vazado" (nunca destruído): a thread é
// desanexada e não pode depender da ordem de destruição de estáticos na saída do processo.
std::mutex& registryMu() {
  static auto* m = new std::mutex;
  return *m;
}
std::vector<Shared*>& registry() {
  static auto* v = new std::vector<Shared*>;
  return *v;
}

// Uma thread por processo (~1 ms): para cada chamada vencida, marca abortDue e liga a flag do estado. Só isso:
// lua_cbmp_setinterrupt lê o l_G (imutável) e grava um inteiro; nenhuma outra estrutura do Lua é tocada.
void clockTick() {
  const int64_t now = realMs();
  std::lock_guard<std::mutex> lk(registryMu());
  for (Shared* s : registry()) {
    std::lock_guard<std::mutex> rl(s->runMu);
    if (!s->active || now < s->abortAtMs) continue;
    s->abortDue.store(true, std::memory_order_release);
    lua_cbmp_setinterrupt(s->mainL, 1);
  }
}

void startClock() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      for (;;) {
        clockTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }).detach();
  });
}

int luaPrintSink(lua_State* L) {
  auto* s = static_cast<Shared*>(lua_touserdata(L, lua_upvalueindex(1)));
  size_t n = 0;
  const char* p = luaL_checklstring(L, 1, &n);
  s->printLine(p, n);
  return 0;
}

// Roda com o _G original (bibliotecas completas) e o transforma no sandbox. Só usa locais: o _G é esvaziado.
constexpr const char* kSandboxBoot = R"lua(
local G, printSink, sandboxRequire, foldCase = ...
local pairs, ipairs, next, type, error = pairs, ipairs, next, type, error
local select, rawget, tostring, concat, lower = select, rawget, tostring, table.concat, string.lower
local rawload, rawcg, rawsetmt, getmt = load, collectgarbage, setmetatable, getmetatable

local function copy(t, keys)
  local r = {}
  if keys then
    for _, k in ipairs(keys) do r[k] = t[k] end
  else
    for k, v in pairs(t) do r[k] = v end
  end
  return r
end

local baseNames = {"assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
  "rawlen", "rawset", "select", "tonumber", "tostring", "type", "xpcall", "_VERSION"}
local env = copy(G, baseNames)
env.string = copy(G.string)
env.string.dump = nil
env.table = copy(G.table)
env.math = copy(G.math)
env.utf8 = copy(G.utf8)
env.coroutine = copy(G.coroutine)
env.os = copy(G.os, {"clock", "date", "time"})

env.load = function(chunk, chunkname, _, e)
  if e == nil then e = G end
  return rawload(chunk, chunkname, "t", e)
end

local gcAllowed = {count = true, step = true}
env.collectgarbage = function(opt, ...)
  if not gcAllowed[opt] then
    error("collectgarbage: opção não permitida no sandbox: " .. tostring(opt) .. " (use 'count' ou 'step')", 2)
  end
  return rawcg(opt, ...)
end

env.setmetatable = function(t, mt)
  if type(mt) == "table" and rawget(mt, "__gc") ~= nil then
    error("setmetatable: __gc não é permitido no sandbox", 2)
  end
  return rawsetmt(t, mt)
end

env.print = function(...)
  local n = select("#", ...)
  local t = {...}
  for i = 1, n do t[i] = tostring(t[i]) end
  printSink(concat(t, "\t", 1, n))
end

-- Chave do cache: o nome (minúsculo onde o sistema de arquivos não diferencia maiúsculas).
env.require = function(name)
  if foldCase and type(name) == "string" then return sandboxRequire(name, lower(name)) end
  return sandboxRequire(name, name)
end

local smt = getmt("")
smt.__index = env.string
smt.__metatable = false

-- Retrato fixo das funções globais para o registro (_LOADED._G): luaL_traceback/luaL_argerror percorrem _LOADED
-- para dar nome às funções; o _G real é do script (tamanho controlado por ele) e não pode ficar lá.
local snapshot = copy(env, baseNames)
for _, k in ipairs({"load", "collectgarbage", "setmetatable", "print", "require"}) do snapshot[k] = env[k] end

for k in next, G do G[k] = nil end
for k, v in next, env do G[k] = v end
G._G = G
return snapshot
)lua";

// "util.mod" → "server/util/mod.lua"; vazio se o nome não for seguro (charset, ponto no início/fim, "..").
std::string moduleRel(std::string_view name) {
  if (name.empty() || name.size() > 128 || name.front() == '.' || name.back() == '.' ||
      name.find("..") != std::string_view::npos)
    return {};
  std::string p;
  for (char c : name) {
    if (c == '.')
      p += '/';
    else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
      p += c;
    else
      return {};
  }
  std::string rel = "server/" + p + ".lua";
  return cbmp::safeRelPath(rel) ? rel : std::string{};
}

bool isScriptPath(const std::string& rel) {
  return rel.size() > 11 && rel.starts_with("server/") && rel.ends_with(".lua") && cbmp::safeRelPath(rel);
}

bool isLinkLike(const fs::file_status& st) {
  if (fs::is_symlink(st)) return true;
#if defined(_MSC_VER)
  if (st.type() == fs::file_type::junction) return true;  // extensão do STL da Microsoft
#endif
  return false;
}

bool isUnder(const fs::path& base, const fs::path& p) {
  auto [b, q] = std::mismatch(base.begin(), base.end(), p.begin(), p.end());
  (void)q;
  return b == base.end();
}

}  // namespace

// Parte C++ do require, isolada num quadro próprio: aqui só há operações protegidas (luaL_loadbufferx) ou sem
// limite de memória, então nenhum longjmp do Lua atravessa objetos C++ vivos. Empilha o chunk ou a mensagem de erro.
CBSCRIPT_NOINLINE int prepareModule(Resource* self, lua_State* L) {
  EnforceOff off(*self->sh_);
  size_t n = 0;
  const char* name = lua_tolstring(L, 1, &n);
  std::string rel = moduleRel(std::string_view(name, n));
  if (rel.empty()) {
    std::string msg = "require: nome de módulo inválido '" + std::string(name, n) +
                      "' (use só A-Z a-z 0-9 _ e '.' como separador de pasta)";
    lua_pushlstring(L, msg.data(), msg.size());
    return LUA_ERRRUN;
  }
  std::string err;
  int st = self->loadFile(L, rel, err);
  if (st != LUA_OK) {
    std::string msg = "require: módulo '" + std::string(name, n) + "': " + err;
    lua_pushlstring(L, msg.data(), msg.size());
  }
  return st;
}

// require(nome, chave): upvalue 1 = Resource*, upvalue 2 = cache (chave → valor do módulo | sentinela "carregando").
// A chave vem do embrulho em Lua (nome em minúsculas no Windows).
int luaRequire(lua_State* L) {
  auto* self = static_cast<Resource*>(lua_touserdata(L, lua_upvalueindex(1)));
  if (lua_type(L, 1) != LUA_TSTRING) return luaL_error(L, "require: o nome do módulo deve ser texto");
  lua_settop(L, 2);
  if (lua_type(L, 2) != LUA_TSTRING) {
    lua_pushvalue(L, 1);
    lua_replace(L, 2);
  }
  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(2));
  if (!lua_isnil(L, -1)) {
    if (lua_touserdata(L, -1) == &kLoadingSentinel)
      return luaL_error(L, "require: dependência circular no módulo '%s'", lua_tostring(L, 1));
    return 1;
  }
  lua_pop(L, 1);

  int st = prepareModule(self, L);
  if (st != LUA_OK) return lua_error(L);

  lua_pushvalue(L, 2);
  lua_pushlightuserdata(L, &kLoadingSentinel);
  lua_rawset(L, lua_upvalueindex(2));
  lua_pushvalue(L, 1);  // o módulo recebe o próprio nome em `...`
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    lua_pushvalue(L, 2);
    lua_pushnil(L);
    lua_rawset(L, lua_upvalueindex(2));
    return lua_error(L);
  }
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
  }
  lua_pushvalue(L, 2);
  lua_pushvalue(L, -2);
  lua_rawset(L, lua_upvalueindex(2));
  return 1;
}

Resource::Resource(ResourceManifest m, fs::path dir, Limits lim, std::function<void(std::string)> warn)
    : manifest_(std::move(m)),
      dir_(std::move(dir)),
      sh_(std::make_unique<Shared>()),
      lua_(sol::default_at_panic, &luaAlloc, sh_.get()) {
  startClock();
  sh_->limit = lim.memoryBytes;  // só vale com enforce=true (código do script); a montagem abaixo não é limitada
  sh_->warnAfter = lim.warnAfter;
  sh_->abortAfter = lim.abortAfter;
  sh_->name = manifest_.name;
  sh_->warn = std::move(warn);

  lua_State* L = lua_.lua_state();
  sh_->mainL = L;
  // Antes de qualquer lua_newthread: as threads copiam o extra space da thread principal.
  *static_cast<const cbmp_interrupt**>(lua_getextraspace(L)) = &kInterrupt;

  lua_.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::utf8,
                      sol::lib::coroutine, sol::lib::os);
  if (luaL_loadbufferx(L, kSandboxBoot, std::strlen(kSandboxBoot), "=cbscript_sandbox", "t") != LUA_OK)
    throw std::runtime_error(std::string("cbscript: sandbox não compila: ") + lua_tostring(L, -1));
  lua_pushglobaltable(L);
  lua_pushlightuserdata(L, sh_.get());
  lua_pushcclosure(L, luaPrintSink, 1);
  lua_pushlightuserdata(L, this);
  lua_newtable(L);  // cache do require
  lua_pushcclosure(L, luaRequire, 2);
#if defined(_WIN32)
  lua_pushboolean(L, 1);
#else
  lua_pushboolean(L, 0);
#endif
  if (lua_pcall(L, 4, 1, 0) != LUA_OK) {
    std::string e = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error("cbscript: falha ao montar o sandbox: " + e);
  }
  // _LOADED só com tabelas que o script não alcança (as bibliotecas originais e o retrato acima): sem o _G real,
  // nenhum lua_next do host (findfield em tracebacks e erros de argumento) depende de dados do script.
  // O sol2 também registra o _G como _LOADED.base: qualquer entrada que aponte para o _G real sai.
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_pushglobaltable(L);
  lua_pushnil(L);
  while (lua_next(L, -3) != 0) {  // [snap, loaded, G, k, v]
    if (lua_rawequal(L, -1, -3)) {
      lua_pushvalue(L, -2);
      lua_pushnil(L);
      lua_rawset(L, -6);  // loaded[k] = nil (permitido durante a travessia)
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);          // [snap, loaded]
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, LUA_GNAME);
  lua_pop(L, 2);
  lua_gc(L, LUA_GCCOLLECT);
  std::lock_guard<std::mutex> lk(registryMu());
  registry().push_back(sh_.get());
}

Resource::~Resource() {
  std::lock_guard<std::mutex> lk(registryMu());
  auto& r = registry();
  r.erase(std::remove(r.begin(), r.end(), sh_.get()), r.end());
}

sol::state& Resource::lua() { return lua_; }

size_t Resource::memoryUsed() const { return sh_->used; }

int Resource::loadFile(lua_State* L, const std::string& rel, std::string& err) {
  if (!cbmp::safeRelPath(rel)) {
    err = "caminho não permitido: " + rel;
    return LUA_ERRFILE;
  }
  // Cada componente abaixo da pasta do resource: existe e não é link simbólico/junção.
  std::error_code ec;
  fs::path p = dir_;
  fs::file_status st;
  for (const auto& comp : fromU8(rel)) {
    p /= comp;
    st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
      err = "arquivo não encontrado: " + rel;
      return LUA_ERRFILE;
    }
    if (isLinkLike(st)) {
      err = "links simbólicos/junções não são permitidos: " + rel;
      return LUA_ERRFILE;
    }
  }
  if (!fs::is_regular_file(st)) {
    err = "não é um arquivo: " + rel;
    return LUA_ERRFILE;
  }
  // E o caminho real fica dentro de <dir>/server (a própria pasta do resource pode ser um link: ok).
  std::error_code ec1, ec2;
  fs::path base = fs::canonical(dir_ / "server", ec1);
  fs::path real = fs::weakly_canonical(p, ec2);
  if (ec1 || ec2 || !isUnder(base, real)) {
    err = "caminho fora da pasta server/ do resource: " + rel;
    return LUA_ERRFILE;
  }
  auto size = fs::file_size(p, ec);
  if (ec || size > kMaxScriptBytes) {
    err = "arquivo grande demais ou ilegível: " + rel;
    return LUA_ERRFILE;
  }
  std::string code(static_cast<size_t>(size), char{0});
  std::ifstream f(p, std::ios::binary);
  if (!f || (size > 0 && !f.read(code.data(), static_cast<std::streamsize>(size)))) {
    err = "não foi possível ler: " + rel;
    return LUA_ERRFILE;
  }
  std::string chunk = "@" + manifest_.name + "/" + rel;
  int r = luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t");
  if (r != LUA_OK) {
    const char* m = lua_tostring(L, -1);
    err = m ? m : "erro ao compilar";
    lua_pop(L, 1);
  }
  return r;
}

bool Resource::runMain(std::string& err, bool* timedOut) {
  if (timedOut != nullptr) *timedOut = false;
  lua_State* L = lua_.lua_state();
  sol::object last;
  for (const auto& rel : manifest_.serverScripts) {
    std::string e;
    int st;
    {
      EnforceOff off(*sh_);
      st = loadFile(L, rel, e);
    }
    if (st != LUA_OK) {
      err = "[" + manifest_.name + "] erro ao carregar " + rel + ": " + e;
      return false;
    }
    sol::protected_function fn(L, -1);
    lua_pop(L, 1);
    CallResult r = call(fn);
    if (!r.ok) {
      err = r.error;
      if (timedOut != nullptr) *timedOut = r.timedOut;
      return false;
    }
    if (r.yielded) {
      err = "[" + manifest_.name + "] " + rel + ": o script principal não pode ceder (coroutine.yield) no nível de cima";
      return false;
    }
    last = r.value;
  }
  exports_ = last;
  return true;
}

Resource::CallResult Resource::call(const sol::reference& fn, std::span<const sol::object> args) {
  lua_State* L = lua_.lua_state();
  CallResult r;
  lua_State* co = nullptr;
  sol::thread th;
  {
    EnforceOff off(*sh_);
    co = lua_newthread(L);
    th = sol::thread(L, -1);
    lua_pop(L, 1);
    if (!lua_checkstack(co, static_cast<int>(args.size()) + 1)) {
      r.error = "[" + manifest_.name + "] argumentos demais";
      return r;
    }
    fn.push(co);
    for (const auto& a : args) a.push(co);
  }
  return resumeThread(co, static_cast<int>(args.size()), std::move(th));
}

Resource::CallResult Resource::resume(const sol::thread& co, std::span<const sol::object> args) {
  CallResult r;
  lua_State* c = co.valid() ? co.thread_state() : nullptr;
  if (c == nullptr || lua_status(c) != LUA_YIELD) {
    r.error = "[" + manifest_.name + "] a corrotina não está suspensa";
    return r;
  }
  {
    EnforceOff off(*sh_);
    lua_settop(c, 0);
    if (!lua_checkstack(c, static_cast<int>(args.size()))) {
      r.error = "[" + manifest_.name + "] argumentos demais";
      return r;
    }
    for (const auto& a : args) a.push(c);
  }
  return resumeThread(c, static_cast<int>(args.size()), co);
}

Resource::CallResult Resource::resumeThread(lua_State* co, int nargs, sol::thread th) {
  Shared& s = *sh_;
  lua_State* L = lua_.lua_state();
  const bool outer = s.depth == 0;
  if (outer) {
    std::lock_guard<std::mutex> lk(s.runMu);
    s.start = std::chrono::steady_clock::now();
    s.abortAtMs = realMs() + s.abortAfter.count();
    s.aborting = false;
    s.abortDue.store(false, std::memory_order_relaxed);
    lua_cbmp_setinterrupt(L, 0);
    s.active = true;
  }
  ++s.depth;
  const bool prevEnforce = s.enforce;
  s.enforce = true;
  int nres = 0;
  int st = lua_resume(co, L, nargs, &nres);
  s.enforce = prevEnforce;
  --s.depth;
  if (outer) {
    std::lock_guard<std::mutex> lk(s.runMu);
    s.active = false;
    lua_cbmp_setinterrupt(L, 0);
  }

  EnforceOff off(s);
  CallResult r;
  r.timedOut = s.aborting;
  if ((st == LUA_OK || st == LUA_YIELD) && !s.aborting) {
    r.ok = true;
    r.yielded = st == LUA_YIELD;
    if (nres > 0) {
      lua_pushvalue(co, -nres);
      lua_xmove(co, L, 1);
      r.value = sol::object(L, -1);
      lua_pop(L, 1);
    }
    lua_pop(co, nres);
    if (r.yielded) r.thread = std::move(th);
  } else {
    std::string msg;
    if (s.aborting) {
      msg = "tempo esgotado: execução passou de " + std::to_string(s.abortAfter.count()) +
            " ms e foi abortada pela vigia";
    } else {
      int t = lua_type(co, -1);
      if (t == LUA_TSTRING || t == LUA_TNUMBER)
        msg = lua_tostring(co, -1);
      else
        msg = std::string("erro não textual (") + lua_typename(co, t) + ")";
      if (st == LUA_ERRMEM || msg.find("not enough memory") != std::string::npos)
        msg = "memória esgotada (limite " + std::to_string(s.limit >> 20) + " MiB): " + msg;
    }
    msg = "[" + manifest_.name + "] " + msg;
    luaL_traceback(L, co, msg.c_str(), 0);
    r.error = lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  if (outer) {
    const auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - s.start);
    if (ms >= s.warnAfter) s.warnSlow(static_cast<long long>(ms.count()), s.aborting);
    s.rollPrintWindow(now);
    s.aborting = false;
  }
  return r;
}

ResourceManifest loadManifest(const fs::path& dir) {
  const std::string where = u8(dir);
  auto fail = [&](const std::string& why) {
    return std::runtime_error("resource.json inválido em '" + where + "': " + why);
  };
  std::ifstream f(dir / "resource.json", std::ios::binary);
  if (!f) throw std::runtime_error("resource.json não encontrado em '" + where + "'");
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(f);
  } catch (const std::exception& e) {
    throw fail(std::string("JSON malformado: ") + e.what());
  }
  if (!j.is_object()) throw fail("o conteúdo deve ser um objeto JSON");

  auto validName = [](const std::string& n) { return n.size() <= kMaxNameLen && cbmp::safeSegment(n); };
  auto stringArray = [&](const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key)) return out;
    const auto& a = j[key];
    if (!a.is_array()) throw fail(std::string("'") + key + "' deve ser uma lista de textos");
    for (const auto& v : a) {
      if (!v.is_string()) throw fail(std::string("'") + key + "' deve ser uma lista de textos");
      out.push_back(v.get<std::string>());
    }
    return out;
  };

  ResourceManifest m;
  if (!j.contains("name") || !j["name"].is_string()) throw fail("falta 'name' (texto)");
  m.name = j["name"].get<std::string>();
  if (!validName(m.name))
    throw fail("nome de resource inválido '" + m.name + "' (use só A-Z a-z 0-9 . _ -, até 64 caracteres)");
  if (!j.contains("version") || !j["version"].is_string() || j["version"].get<std::string>().empty())
    throw fail("falta 'version' (texto)");
  m.version = j["version"].get<std::string>();

  m.serverScripts = stringArray("server");
  for (const auto& s : m.serverScripts)
    if (!isScriptPath(s)) throw fail("script de servidor inválido '" + s + "' (precisa ser server/....lua seguro)");
  m.dependencies = stringArray("dependencies");
  for (const auto& d : m.dependencies)
    if (!validName(d)) throw fail("dependência com nome inválido '" + d + "'");
  if (j.contains("config")) m.config = j["config"];

  if (j.contains("permissions")) {
    const auto& p = j["permissions"];
    if (!p.is_object()) throw fail("'permissions' deve ser um objeto");
    if (p.contains("db")) {
      if (!p["db"].is_boolean()) throw fail("'permissions.db' deve ser true/false");
      m.db = p["db"].get<bool>();
    }
    if (p.contains("http")) {
      if (!p["http"].is_array()) throw fail("'permissions.http' deve ser uma lista de domínios");
      for (const auto& h : p["http"]) {
        if (!h.is_string()) throw fail("'permissions.http' deve ser uma lista de domínios");
        std::string host = h.get<std::string>();
        bool ok = !host.empty() && host.size() <= 253 && host.front() != '.' && host.back() != '.' &&
                  host.front() != '-';
        for (char& c : host) {
          if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
          if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) ok = false;
        }
        if (!ok) throw fail("domínio inválido em 'permissions.http': '" + h.get<std::string>() + "' (só o host)");
        m.httpAllow.push_back(std::move(host));
      }
    }
  }
  return m;
}

}  // namespace cbscript
