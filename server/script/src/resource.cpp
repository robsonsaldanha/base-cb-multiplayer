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
//   tostring type _VERSION, string (sem dump), table (move/insert/remove em Lua, sort sempre com comparador Lua),
//   math, utf8, coroutine (resume/wrap/close próprios), os.{time,clock,date}; load/collectgarbage/setmetatable/
//   xpcall/print/require embrulhados. Sem io, debug, package, dofile, loadfile, warn, string.dump. As bibliotecas
//   originais só continuam no registro (inalcançável sem debug).
// - load: sempre modo "t"; ambiente = o 4º argumento se não for nil, senão o sandbox (desvio aceito da spec).
// - Metatable das strings: __index = string do sandbox e __metatable = false (getmetatable("") devolve false).
// - setmetatable recusa metatables com __gc: finalizadores rodam fora da vigia (em qualquer alocação/no close).
// - collectgarbage: só "count" e "step".
// - print: vai para o callback `warn` como "[<resource>] print: ..."; linha cortada em 64 KiB; no máximo 200 linhas
//   por segundo por resource, o excesso é contado e resumido uma vez por janela.
// - Memória: alocador por estado conta bytes; acima do teto devolve NULL (Lua lança erro de memória após tentar
//   coleta de emergência). O teto só vale enquanto código do script roda (dentro de lua_resume); o código do host
//   (montar estado, traceback, empilhar argumentos) não é limitado, para nunca cair no panic fora de proteção.
// - Vigia:
//   * Hook de chamada (LUA_MASKCALL) permanente em toda thread (lua_newthread herda o hook): confere o prazo a cada
//     chamada de função, Lua ou C, comparando com um relógio grosso atômico (sem ler o relógio do sistema).
//     Não há hook de contagem permanente: no Lua 5.4 qualquer hook de contagem põe o interpretador em modo "trap"
//     em toda instrução (~1,8x mais lento em código normal; medido).
//   * Escalada assíncrona: uma thread por processo (relógio) vê o prazo de aviso/aborto vencido e chama
//     lua_sethook(contagem 1) na thread Lua em execução (`running`, protegida por runMu). Isso pega laços sem
//     chamadas e operadores caros (s < t, s == t, s .. s com strings enormes) na instrução seguinte. `running` é
//     mantido pelo host (resumeThread) e pelos coroutine.resume/wrap/close do sandbox (C, adaptados do lcorolib),
//     que publicam a corrotina antes de lua_resume/lua_closethread e devolvem a thread chamadora logo depois — sem
//     código Lua no meio, então o ponteiro nunca fica velho, e a thread publicada está sempre viva (está na pilha
//     de quem a retomou). Enquanto vencido, o relógio reaplica a escalada a cada tick (cobre troca de thread e corrida
//     com o próprio hook restaurando a máscara).
//   * Aviso único por chamada após warnAfter; após abortAfter o hook lança erro e a thread passa a disparar o hook a
//     cada instrução enquanto a chamada não termina, de modo que pcall/xpcall/coroutine.resume não engolem o aborto.
//     O erro nasce dentro do hook (o Lua desliga hooks ali), então o xpcall do sandbox pula o handler do script
//     durante o aborto. Lua com patches (vcpkg-overlays/lua): (C1) hooks religados antes de rodar __close de thread
//     fechada; (I1) match() de padrões chama kInterrupt a cada 4096 passos.
//   * Limite que sobra: uma única operação C sem callback (string.rep gigante, comparação de strings enormes, GC de
//     emergência perto do teto) só é interrompida quando termina. O teto de memória limita o tamanho; o ScriptHost
//     precisa de supervisor (ver report).

// Funções chamadas a partir de quadros que o Lua atravessa com longjmp (hook, funções C do sandbox) ficam fora de
// linha: assim o quadro atravessado não tem objetos C++ com destrutor. No MSVC, um longjmp do lua.dll (C)
// atravessando um quadro C++ com estado de EH derrubou o processo (0xC0000409/0xC0000374) — visto no require.
#if defined(_MSC_VER)
#define CBSCRIPT_NOINLINE __declspec(noinline)
#else
#define CBSCRIPT_NOINLINE __attribute__((noinline))
#endif

namespace fs = std::filesystem;

// Contrato do patch I1 (vcpkg-overlays/lua/cbmp-i1-match-interrupt.patch): o extra space de cada thread guarda um
// ponteiro para esta struct (layout C idêntico) ou NULL.
struct cbmp_interrupt {
  void (*check)(lua_State* L);
};

namespace cbscript {

namespace {

constexpr int kBaseMask = LUA_MASKCALL;                   // normal: só chamadas, sem contagem
constexpr int kEscalatedMask = LUA_MASKCALL | LUA_MASKCOUNT;  // escalado: contagem 1 (toda instrução)
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

// Relógio grosso (~1 ms; no Windows, a granularidade do sleep) lido pelo hook sem chamar o relógio do sistema.
std::atomic<int64_t> g_coarseNow{0};

int64_t coarseNow() noexcept { return g_coarseNow.load(std::memory_order_relaxed); }

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
  bool warned = false;
  bool aborting = false;
  // vigia (compartilhado com a thread do relógio; campos não atômicos só sob runMu)
  std::mutex runMu;
  bool active = false;           // chamada externa em andamento
  lua_State* running = nullptr;  // thread Lua em execução (viva enquanto publicada)
  int64_t warnAtMs = 0, abortAtMs = 0;
  bool warnEscalated = false;
  std::atomic<bool> warnDue{false}, abortDue{false};
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
  CBSCRIPT_NOINLINE void warnRunning() noexcept {
    try {
      emit("[" + name + "] vigia: execução passou de " + std::to_string(warnAfter.count()) +
           " ms e continua rodando (aborto em " + std::to_string(abortAfter.count()) + " ms)");
    } catch (...) {
    }
  }
  CBSCRIPT_NOINLINE void warnSlow(long long ms) noexcept {
    try {
      emit("[" + name + "] vigia: execução levou " + std::to_string(ms) + " ms (aviso a partir de " +
           std::to_string(warnAfter.count()) + " ms)");
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
        emit("[" + name + "] print: " + std::string(p, kMaxPrintBytes) + " ...(truncado, " + std::to_string(n) +
             " bytes)");
      else
        emit("[" + name + "] print: " + std::string(p, n));
    } catch (...) {
    }
  }
};

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

void watchHook(lua_State* L, lua_Debug*);

bool overdue(const Shared* s) noexcept {
  return s->aborting || s->abortDue.load(std::memory_order_acquire) || coarseNow() >= s->abortAtMs;
}

// Marca o aborto, deixa a thread disparando o hook a cada instrução e lança o erro (longjmp).
void raiseTimeout(lua_State* L, Shared* s) {
  s->aborting = true;
  lua_sethook(L, watchHook, kEscalatedMask, 1);
  luaL_error(L, "tempo esgotado: execução passou de %d ms (abortada pela vigia)",
             static_cast<int>(s->abortAfter.count()));
}

void watchHook(lua_State* L, lua_Debug*) {
  Shared* s = sharedOf(L);
  if (s->depth > 0 && overdue(s)) {
    raiseTimeout(L, s);
    return;
  }
  // Escalada de aviso (ou resto de um aborto anterior): volta ao hook só de chamadas.
  if (lua_gethookmask(L) != kBaseMask) lua_sethook(L, watchHook, kBaseMask, 0);
  if (s->depth > 0 && !s->warned &&
      (s->warnDue.load(std::memory_order_acquire) || coarseNow() >= s->warnAtMs)) {
    s->warned = true;
    s->warnRunning();
  }
}

// Chamado pelo Lua com patch (I1) de dentro de loops C longos (match de padrões). Fora de hook: hooks seguem ligados.
void interruptCheck(lua_State* L) {
  Shared* s = sharedOf(L);
  if (s->depth > 0 && overdue(s)) raiseTimeout(L, s);
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

// Uma thread por processo: atualiza o relógio grosso e escala chamadas vencidas. lua_sethook pode ser chamado de
// fora da thread do Lua (ldebug.c: "can be called asynchronously"); `running` só é lido sob runMu.
void clockTick() {
  const int64_t now = realMs();
  g_coarseNow.store(now, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(registryMu());
  for (Shared* s : registry()) {
    std::lock_guard<std::mutex> rl(s->runMu);
    if (!s->active || s->running == nullptr) continue;
    if (now >= s->abortAtMs) {
      s->abortDue.store(true, std::memory_order_release);
      lua_sethook(s->running, watchHook, kEscalatedMask, 1);  // reaplicado a cada tick enquanto vencido
    } else if (!s->warnEscalated && now >= s->warnAtMs) {
      s->warnEscalated = true;
      s->warnDue.store(true, std::memory_order_release);
      lua_sethook(s->running, watchHook, kEscalatedMask, 1);
    }
  }
}

void startClock() {
  static std::once_flag once;
  std::call_once(once, [] {
    g_coarseNow.store(realMs(), std::memory_order_relaxed);
    std::thread([] {
      for (;;) {
        clockTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }).detach();
  });
}

CBSCRIPT_NOINLINE void setRunning(Shared* s, lua_State* t) noexcept {
  std::lock_guard<std::mutex> lk(s->runMu);
  s->running = t;
}

// --- coroutine.resume/wrap/close do sandbox: adaptados de lcorolib.c (Lua 5.4.7, licença MIT), publicando a thread
// em execução para a escalada da vigia. Só API do Lua e setRunning (fora de linha) nestes quadros.

lua_State* getco(lua_State* L) {
  lua_State* co = lua_tothread(L, 1);
  luaL_argexpected(L, co, 1, "thread");
  return co;
}

int auxresume(Shared* s, lua_State* L, lua_State* co, int narg) {
  if (!lua_checkstack(co, narg)) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;
  }
  lua_xmove(L, co, narg);
  setRunning(s, co);
  int nres = 0;
  int status = lua_resume(co, L, narg, &nres);
  setRunning(s, L);
  if (status == LUA_OK || status == LUA_YIELD) {
    if (!lua_checkstack(L, nres + 1)) {
      lua_pop(co, nres);
      lua_pushliteral(L, "too many results to resume");
      return -1;
    }
    lua_xmove(co, L, nres);
    return nres;
  }
  lua_xmove(co, L, 1);
  return -1;
}

int luaCoResume(lua_State* L) {
  auto* s = static_cast<Shared*>(lua_touserdata(L, lua_upvalueindex(1)));
  lua_State* co = getco(L);
  int r = auxresume(s, L, co, lua_gettop(L) - 1);
  if (r < 0) {
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;
  }
  lua_pushboolean(L, 1);
  lua_insert(L, -(r + 1));
  return r + 1;
}

int luaCoAuxWrap(lua_State* L) {
  lua_State* co = lua_tothread(L, lua_upvalueindex(1));
  auto* s = static_cast<Shared*>(lua_touserdata(L, lua_upvalueindex(2)));
  int r = auxresume(s, L, co, lua_gettop(L));
  if (r < 0) {
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD) {  // erro dentro da corrotina: fecha as variáveis to-be-closed
      setRunning(s, co);
      stat = lua_closethread(co, L);
      setRunning(s, L);
      lua_xmove(co, L, 1);
    }
    if (stat != LUA_ERRMEM && lua_type(L, -1) == LUA_TSTRING) {
      luaL_where(L, 1);
      lua_insert(L, -2);
      lua_concat(L, 2);
    }
    return lua_error(L);
  }
  return r;
}

int luaCoWrap(lua_State* L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_State* nl = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, nl, 1);
  lua_pushvalue(L, lua_upvalueindex(1));  // Shared*
  lua_pushcclosure(L, luaCoAuxWrap, 2);
  return 1;
}

int luaCoClose(lua_State* L) {
  auto* s = static_cast<Shared*>(lua_touserdata(L, lua_upvalueindex(1)));
  lua_State* co = getco(L);
  const char* bad = nullptr;
  if (co == L) {
    bad = "running";
  } else {
    switch (lua_status(co)) {
      case LUA_YIELD:
        break;
      case LUA_OK: {
        lua_Debug ar;
        if (lua_getstack(co, 0, &ar)) bad = "normal";  // tem quadros: está retomando outra
        break;
      }
      default:
        break;  // morta por erro
    }
  }
  if (bad != nullptr) return luaL_error(L, "cannot close a %s coroutine", bad);
  setRunning(s, co);
  int status = lua_closethread(co, L);
  setRunning(s, L);
  if (status == LUA_OK) {
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_pushboolean(L, 0);
  lua_xmove(co, L, 1);
  return 2;
}

int luaIsAborting(lua_State* L) {
  lua_pushboolean(L, static_cast<Shared*>(lua_touserdata(L, lua_upvalueindex(1)))->aborting ? 1 : 0);
  return 1;
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
local G, printSink, sandboxRequire, isAborting, foldCase, coResume, coWrap, coClose = ...
local pairs, ipairs, next, type, error, select, tonumber = pairs, ipairs, next, type, error, select, tonumber
local rawget, tostring, concat, lower = rawget, tostring, table.concat, string.lower
local rawload, rawcg, rawsetmt, getmt, rawxpcall = load, collectgarbage, setmetatable, getmetatable, xpcall
local tointeger, maxinteger, ult = math.tointeger, math.maxinteger, math.ult
local rawsort = table.sort

local function copy(t, keys)
  local r = {}
  if keys then
    for _, k in ipairs(keys) do r[k] = t[k] end
  else
    for k, v in pairs(t) do r[k] = v end
  end
  return r
end

local env = copy(G, {"assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
  "rawlen", "rawset", "select", "tonumber", "tostring", "type", "xpcall", "_VERSION"})
env.string = copy(G.string)
env.string.dump = nil
env.table = copy(G.table)
env.math = copy(G.math)
env.utf8 = copy(G.utf8)
env.coroutine = copy(G.coroutine)
env.coroutine.resume = coResume
env.coroutine.wrap = coWrap
env.coroutine.close = coClose
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

-- table.move/insert/remove em Lua e sort sempre com comparador em Lua: os laços do C (até 2^63 iterações com
-- __len gigante; comparações de strings enormes) não passam pela vigia. Mesmas mensagens do C, com a posição de
-- quem chamou; só tabelas de verdade são aceitas (sem o caso de userdata com metamétodos).
local function tabArg(v, n, fname)
  if type(v) ~= "table" then
    error("bad argument #" .. n .. " to '" .. fname .. "' (table expected, got " ..
      type(v) .. ")", 3)
  end
end
local function intArg(v, n, fname)
  local i = tointeger(v)
  if i == nil then
    if type(v) == "number" or (type(v) == "string" and tonumber(v) ~= nil) then
      error("bad argument #" .. n .. " to '" .. fname .. "' (number has no integer representation)", 3)
    end
    error("bad argument #" .. n .. " to '" .. fname .. "' (number expected, got " ..
      type(v) .. ")", 3)
  end
  return i
end
local function lenOf(t)
  local n = tointeger(#t)
  if n == nil then error("object length is not an integer", 3) end
  return n
end

env.table.move = function(a1, f, e, t, a2)
  tabArg(a1, 1, "move")
  if a2 == nil then a2 = a1 else tabArg(a2, 5, "move") end
  f, e, t = intArg(f, 2, "move"), intArg(e, 3, "move"), intArg(t, 4, "move")
  if e >= f then
    if not (f > 0 or e < maxinteger + f) then error("bad argument #3 to 'move' (too many elements to move)", 2) end
    if t > maxinteger - (e - f) then error("bad argument #4 to 'move' (destination wrap around)", 2) end
    if t > e or t <= f or a1 ~= a2 then
      for i = 0, e - f do a2[t + i] = a1[f + i] end
    else
      for i = e - f, 0, -1 do a2[t + i] = a1[f + i] end
    end
  end
  return a2
end

env.table.insert = function(t, ...)
  local nargs = select("#", ...)
  tabArg(t, 1, "insert")
  local e = lenOf(t) + 1  -- primeiro vazio
  if nargs == 1 then
    t[e] = ...
  elseif nargs == 2 then
    local pos, v = ...
    pos = intArg(pos, 2, "insert")
    if not ult(pos - 1, e) then error("bad argument #2 to 'insert' (position out of bounds)", 2) end
    for i = e, pos + 1, -1 do t[i] = t[i - 1] end
    t[pos] = v
  else
    error("wrong number of arguments to 'insert'", 2)
  end
end

env.table.remove = function(t, pos)
  tabArg(t, 1, "remove")
  local size = lenOf(t)
  if pos == nil then pos = size else pos = intArg(pos, 2, "remove") end
  if pos ~= size and not (pos - 1 == size or ult(pos - 1, size)) then
    error("bad argument #2 to 'remove' (position out of bounds)", 2)
  end
  local result = t[pos]
  while pos < size do
    t[pos] = t[pos + 1]
    pos = pos + 1
  end
  t[pos] = nil
  return result
end

local function defaultLess(a, b) return a < b end
env.table.sort = function(t, cmp)
  if cmp == nil then cmp = defaultLess end
  return rawsort(t, cmp)
end

env.print = function(...)
  local n = select("#", ...)
  local t = {...}
  for i = 1, n do t[i] = tostring(t[i]) end
  printSink(concat(t, "\t", 1, n))
end

-- O erro de aborto nasce dentro do hook, onde o Lua desliga os hooks; o handler do xpcall rodaria sem vigia.
-- Durante o aborto o handler do script é pulado.
env.xpcall = function(f, msgh, ...)
  return rawxpcall(f, function(e)
    if isAborting() then return e end
    return msgh(e)
  end, ...)
end

-- Chave do cache: o nome (minúsculo onde o sistema de arquivos não diferencia maiúsculas).
env.require = function(name)
  if foldCase and type(name) == "string" then return sandboxRequire(name, lower(name)) end
  return sandboxRequire(name, name)
end

local smt = getmt("")
smt.__index = env.string
smt.__metatable = false

for k in next, G do G[k] = nil end
for k, v in next, env do G[k] = v end
G._G = G
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
  lua_pushlightuserdata(L, sh_.get());
  lua_pushcclosure(L, luaIsAborting, 1);
#if defined(_WIN32)
  lua_pushboolean(L, 1);
#else
  lua_pushboolean(L, 0);
#endif
  lua_pushlightuserdata(L, sh_.get());
  lua_pushcclosure(L, luaCoResume, 1);
  lua_pushlightuserdata(L, sh_.get());
  lua_pushcclosure(L, luaCoWrap, 1);
  lua_pushlightuserdata(L, sh_.get());
  lua_pushcclosure(L, luaCoClose, 1);
  if (lua_pcall(L, 8, 0, 0) != LUA_OK) {
    std::string e = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error("cbscript: falha ao montar o sandbox: " + e);
  }
  lua_gc(L, LUA_GCCOLLECT);
  lua_sethook(L, watchHook, kBaseMask, 0);
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

bool Resource::runMain(std::string& err) {
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
  lua_State* prevRunning = nullptr;
  {
    std::lock_guard<std::mutex> lk(s.runMu);
    prevRunning = s.running;
    if (outer) {
      s.start = std::chrono::steady_clock::now();
      const int64_t startMs = realMs();
      s.warnAtMs = startMs + s.warnAfter.count();
      s.abortAtMs = startMs + s.abortAfter.count();
      s.warned = false;
      s.aborting = false;
      s.warnEscalated = false;
      s.warnDue.store(false, std::memory_order_relaxed);
      s.abortDue.store(false, std::memory_order_relaxed);
      s.active = true;
    }
    s.running = co;
  }
  ++s.depth;
  const bool prevEnforce = s.enforce;
  s.enforce = true;
  int nres = 0;
  int st = lua_resume(co, L, nargs, &nres);
  s.enforce = prevEnforce;
  --s.depth;
  {
    std::lock_guard<std::mutex> lk(s.runMu);
    s.running = prevRunning;
    if (outer) s.active = false;
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
    if (!s.warned && ms >= s.warnAfter) {
      s.warned = true;
      s.warnSlow(static_cast<long long>(ms.count()));
    }
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
