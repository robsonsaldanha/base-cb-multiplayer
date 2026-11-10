#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "cbscript/host.hpp"
#include "test_util.hpp"

using namespace cbscript;
using State = ResourceStatus::State;

namespace {

struct FakeClock {
  std::atomic<double> t{1000.0};
  std::atomic<bool> closed{false};  // depois de ~ScriptHost: ninguém deveria mais ler o relógio
  mutable std::atomic<int> lateReads{0};
  double get() const {
    if (closed.load()) lateReads.fetch_add(1);
    return t.load();
  }
  void advance(double s) { t.store(t.load() + s); }
};

template <class P>
bool waitFor(P pred, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
  auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

// block(): função C que prende a thread de scripts dentro de uma chamada ao Lua (simula uma operação C única que não
// volta) até release().
struct Blocker {
  std::mutex mu;
  std::condition_variable cv;
  bool released = false;
  std::atomic<int> entered{0};
  void release() {
    std::lock_guard<std::mutex> lk(mu);
    released = true;
    cv.notify_all();
  }
};

CBSCRIPT_TEST_NOINLINE void doBlock(Blocker* b) {
  b->entered.fetch_add(1);
  std::unique_lock<std::mutex> lk(b->mu);
  b->cv.wait(lk, [b] { return b->released; });
}
int luaBlock(lua_State* L) {
  doBlock(static_cast<Blocker*>(lua_touserdata(L, lua_upvalueindex(1))));
  return 0;
}
// once(): true só na primeira chamada (por ambiente de teste).
int luaOnce(lua_State* L) {
  auto* n = static_cast<std::atomic<int>*>(lua_touserdata(L, lua_upvalueindex(1)));
  lua_pushboolean(L, n->fetch_add(1) == 0);
  return 1;
}
CBSCRIPT_TEST_NOINLINE void doEmit(ScriptContext* ctx, const char* name) { ctx->emit(CmdEmit{-1, name, "{}", 0, 0}); }
int luaEmit(lua_State* L) {
  auto* ctx = static_cast<ScriptContext*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* s = luaL_checkstring(L, 1);
  doEmit(ctx, s);
  return 0;
}

void setGlobalC(lua_State* L, const char* name, lua_CFunction f, void* up) {
  lua_pushglobaltable(L);
  lua_pushstring(L, name);
  lua_pushlightuserdata(L, up);
  lua_pushcclosure(L, f, 1);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

const char* levelName(LogLevel l) {
  switch (l) {
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Critical: return "critical";
  }
  return "?";
}

// Ambiente de teste: pasta de resources, relógio falso, ganchos que registram tudo e um despachante mínimo
// (o que a Task 4 fará com cb.*): evento de cliente "x" chama exports.x de cada resource rodando.
struct HostEnv {
  TmpDir dir;
  std::shared_ptr<FakeClock> clockp = std::make_shared<FakeClock>();
  std::shared_ptr<Blocker> blockerp = std::make_shared<Blocker>();
  FakeClock& clock = *clockp;
  Blocker& blocker = *blockerp;
  std::atomic<int> onceCount{0};
  mutable std::mutex mu;
  std::vector<std::pair<LogLevel, std::string>> logs;
  std::vector<std::string> started, stopping, seen;
  HostOptions opt;
  Limits lim = shortLimits();
  std::unique_ptr<ScriptHost> host;
  std::vector<std::string> cmds;  // nomes dos CmdEmit drenados

  HostEnv() {
    opt.supervisorPeriod = std::chrono::milliseconds(5);
    opt.idleWait = std::chrono::milliseconds(2);
    opt.log = [this](LogLevel l, const std::string& m) {
      std::lock_guard<std::mutex> lk(mu);
      logs.emplace_back(l, m);
    };
    opt.onSetup = [this, b = blockerp](ScriptContext& ctx, HostedResource& r) {
      lua_State* L = r.lua().lua().lua_state();
      setGlobalC(L, "block", luaBlock, b.get());
      setGlobalC(L, "emit", luaEmit, &ctx);
      setGlobalC(L, "once", luaOnce, &onceCount);
    };
    opt.onStarted = [this](ScriptContext&, HostedResource& r) {
      std::lock_guard<std::mutex> lk(mu);
      started.push_back(r.name());
    };
    opt.onStopping = [this](ScriptContext&, HostedResource& r) {
      std::lock_guard<std::mutex> lk(mu);
      stopping.push_back(r.name());
    };
    opt.onEvent = [this](ScriptContext& ctx, SimToScript& ev) {
      std::visit(
          [&](auto& e) {
            using T = std::decay_t<decltype(e)>;
            std::string tag;
            if constexpr (std::is_same_v<T, EvPlayerJoin>) tag = "join:" + std::to_string(e.p.id);
            if constexpr (std::is_same_v<T, EvPlayerLeave>) tag = "leave:" + std::to_string(e.id);
            if constexpr (std::is_same_v<T, EvClientEvent>) tag = "ev:" + e.name;
            if constexpr (std::is_same_v<T, EvCommand>) tag = "cmd:" + e.line;
            {
              std::lock_guard<std::mutex> lk(mu);
              seen.push_back(tag);
            }
            if constexpr (std::is_same_v<T, EvCommand>) {
              if (e.reply) e.reply("ok " + e.line);
            }
            if constexpr (std::is_same_v<T, EvClientEvent>) {
              if (e.name == "__players") {
                ctx.emit(CmdEmit{-1, "players", std::to_string(ctx.players()->size()), 0, 0});
                return;
              }
              for (HostedResource* h : ctx.running()) {
                sol::object ex = h->lua().exports();
                if (ex.get_type() != sol::type::table) continue;
                sol::table t = ex;
                sol::object fn = t.raw_get<sol::object>(e.name);
                if (fn.get_type() == sol::type::function) ctx.invoke(*h, fn);
              }
            }
          },
          ev);
    };
  }
  ~HostEnv() {
    blocker.release();
    destroy();
  }
  void addRes(const std::string& name, const std::vector<std::string>& deps, const std::string& code) {
    nlohmann::json j = {{"name", name}, {"version", "1.0.0"}, {"server", {"server/main.lua"}}, {"dependencies", deps}};
    put(dir.p / name / "resource.json", j.dump());
    put(dir.p / name / "server" / "main.lua", code);
  }
  void make() {
    host = std::make_unique<ScriptHost>(dir.p, lim, [c = clockp] { return c->get(); }, opt);
  }
  void boot() {
    make();
    guarded("start", [this] {
      host->start();
      return 0;
    });
  }
  void destroy() {
    if (host)
      guarded("destroy", [this] {
        host.reset();
        return 0;
      });
  }
  bool call(std::future<bool> f) {
    return guarded("control", [&f] { return f.get(); });
  }
  ResourceStatus st(const std::string& name) const {
    for (auto& s : host->status())
      if (s.name == name) return s;
    return ResourceStatus{"<ausente>", State::Failed, "não listado", 0, 0};
  }
  bool is(const std::string& name, State s) const { return st(name).state == s; }
  bool hasLog(LogLevel l, const std::string& sub) const {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [lv, m] : logs)
      if (lv == l && contains(m, sub)) return true;
    return false;
  }
  std::string allLogs() const {
    std::lock_guard<std::mutex> lk(mu);
    std::string s;
    for (auto& [lv, m] : logs) s += std::string(levelName(lv)) + ": " + m + "\n";
    return s;
  }
  std::vector<std::string> get(const std::vector<std::string>& v) const {
    std::lock_guard<std::mutex> lk(mu);
    return v;
  }
  void drain() {
    std::vector<ScriptToSim> out;
    host->drainCommands(out);
    for (auto& c : out)
      if (auto* e = std::get_if<CmdEmit>(&c)) cmds.push_back(e->name + (e->json == "{}" ? "" : "=" + e->json));
  }
  bool gotCmd(const std::string& n) {
    drain();
    return std::find(cmds.begin(), cmds.end(), n) != cmds.end();
  }
  bool ev(const std::string& name) { return host->post(EvClientEvent{1, name, "{}", 0}); }
};

using Strs = std::vector<std::string>;

}  // namespace

TEST_CASE("host: resources start in topological order") {
  HostEnv env;
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {"c"}, "return {}");
  env.addRes("c", {}, "return {}");
  env.addRes("d", {}, "return {}");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.get(env.started) == Strs{"c", "b", "a", "d"});
  auto all = env.host->status();
  REQUIRE(all.size() == 4);
  for (auto& s : all) {
    INFO(s.name << ": " << s.lastError);
    REQUIRE(s.state == State::Running);
    REQUIRE(s.memBytes > 0);
  }
  REQUIRE(all[0].name == "a");  // status ordenado por nome
}

TEST_CASE("host: cycles, missing dependencies and bad manifests fail only the affected resources") {
  HostEnv env;
  env.addRes("x", {"y"}, "return {}");
  env.addRes("y", {"x"}, "return {}");
  env.addRes("z", {"missing"}, "return {}");
  env.addRes("w", {"z"}, "return {}");
  env.addRes("ok", {}, "return {}");
  env.addRes("boom", {}, "error('falhou no main')");
  env.addRes("needsboom", {"boom"}, "return {}");
  put(env.dir.p / "bad" / "resource.json", "{");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.get(env.started) == Strs{"ok"});
  REQUIRE(env.is("ok", State::Running));
  for (const char* n : {"x", "y"}) {
    INFO(n << ": " << env.st(n).lastError);
    REQUIRE(env.is(n, State::Failed));
    REQUIRE(contains(env.st(n).lastError, "ciclo"));
  }
  REQUIRE(env.is("z", State::Failed));
  REQUIRE(contains(env.st("z").lastError, "ausente"));
  REQUIRE(contains(env.st("z").lastError, "missing"));
  REQUIRE(env.is("w", State::Failed));
  REQUIRE(contains(env.st("w").lastError, "'z'"));
  REQUIRE(env.is("boom", State::Failed));
  REQUIRE(contains(env.st("boom").lastError, "falhou no main"));
  REQUIRE(env.is("needsboom", State::Failed));
  REQUIRE(contains(env.st("needsboom").lastError, "'boom'"));
  REQUIRE(env.is("bad", State::Failed));
  REQUIRE(contains(env.st("bad").lastError, "resource.json"));
  REQUIRE(env.hasLog(LogLevel::Error, "falhou no main"));
}

TEST_CASE("host: stop stops dependents first and restart brings them back") {
  HostEnv env;
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {}, "return {}");
  env.addRes("solo", {}, "return {}");
  env.boot();
  REQUIRE(env.get(env.started) == Strs{"b", "a", "solo"});

  REQUIRE(env.call(env.host->stopResource("b")));
  REQUIRE(env.get(env.stopping) == Strs{"a", "b"});
  REQUIRE(env.is("a", State::Stopped));
  REQUIRE(env.is("b", State::Stopped));
  REQUIRE(env.is("solo", State::Running));

  REQUIRE(env.call(env.host->startResource("b")));
  REQUIRE(env.is("b", State::Running));
  REQUIRE(env.is("a", State::Stopped));  // start não religa dependentes parados à mão
  REQUIRE(env.call(env.host->startResource("a")));
  REQUIRE(env.is("a", State::Running));

  REQUIRE(env.call(env.host->restartResource("b")));
  REQUIRE(env.get(env.stopping) == Strs{"a", "b", "a", "b"});
  REQUIRE(env.get(env.started) == Strs{"b", "a", "solo", "b", "a", "b", "a"});
  REQUIRE(env.is("a", State::Running));
  REQUIRE(env.is("b", State::Running));

  // start de um dependente sobe a dependência parada
  REQUIRE(env.call(env.host->stopResource("b")));
  REQUIRE(env.call(env.host->startResource("a")));
  REQUIRE(env.is("a", State::Running));
  REQUIRE(env.is("b", State::Running));

  // nomes desconhecidos ou inseguros
  REQUIRE_FALSE(env.call(env.host->stopResource("nope")));
  REQUIRE_FALSE(env.call(env.host->startResource("nope")));
  REQUIRE_FALSE(env.call(env.host->restartResource("nope")));
  REQUIRE_FALSE(env.call(env.host->startResource("../a")));
  REQUIRE_FALSE(env.call(env.host->startResource("")));

  // resource novo na pasta sobe a quente
  env.addRes("late", {"solo"}, "return {}");
  REQUIRE(env.call(env.host->startResource("late")));
  REQUIRE(env.is("late", State::Running));

  // desligar o host para tudo, dependentes antes
  std::size_t before = env.get(env.stopping).size();
  env.destroy();
  Strs s = env.get(env.stopping);
  Strs tail(s.begin() + static_cast<std::ptrdiff_t>(before), s.end());
  REQUIRE(tail.size() == 4);
  auto pos = [&](const char* n) { return std::find(tail.begin(), tail.end(), n) - tail.begin(); };
  REQUIRE(pos("a") < pos("b"));
  REQUIRE(pos("late") < pos("solo"));
}

TEST_CASE("host: an error in a callback is logged and the resource keeps running") {
  HostEnv env;
  env.lim.memoryBytes = size_t{4} << 20;  // erro de memória rápido, bem antes do aborto por tempo
  env.addRes("errs", {}, "return { boom = function() error('kaboom') end, ping = function() emit('pong') end }");
  env.boot();
  REQUIRE(env.ev("boom"));
  REQUIRE(env.ev("ping"));
  REQUIRE(waitFor([&] { return env.gotCmd("pong"); }));
  INFO(env.allLogs());
  REQUIRE(env.is("errs", State::Running));
  REQUIRE(env.hasLog(LogLevel::Error, "kaboom"));
  REQUIRE(env.hasLog(LogLevel::Error, "[errs]"));
  REQUIRE(env.hasLog(LogLevel::Error, "stack traceback"));
  // erro de memória num callback também não derruba
  env.addRes("mem", {}, "return { eat = function() local t = {} ; for i = 1, 1e9 do t[i] = string.rep('x', 1e5) .. i end "
                        "end, ping = function() emit('mem-ok') end }");
  REQUIRE(env.call(env.host->startResource("mem")));
  REQUIRE(env.ev("eat"));
  REQUIRE(env.ev("ping"));
  bool memOk = waitFor([&] { return env.gotCmd("mem-ok"); });
  INFO(env.allLogs());
  REQUIRE(memOk);
  REQUIRE(env.is("mem", State::Running));
}

TEST_CASE("host: timeouts restart with back-off and fail after 3 in 60 s") {
  HostEnv env;
  env.addRes("loop", {}, "return { hang = function() while true do end end }");
  env.addRes("dep", {"loop"}, "return {}");
  env.boot();
  REQUIRE(env.get(env.started) == Strs{"loop", "dep"});
  auto restarting = [&] {
    return env.is("loop", State::Stopped) && contains(env.st("loop").lastError, "rein");
  };
  auto bothUp = [&] { return env.is("loop", State::Running) && env.is("dep", State::Running); };

  // 1º aborto: reinício em 1 s (relógio injetado)
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor(restarting));
  INFO(env.allLogs());
  REQUIRE(env.hasLog(LogLevel::Error, "tempo esgotado"));
  REQUIRE(env.is("dep", State::Stopped));
  REQUIRE(env.get(env.stopping) == Strs{"dep", "loop"});
  env.clock.advance(0.5);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(restarting());
  env.clock.advance(0.6);
  REQUIRE(waitFor(bothUp));
  REQUIRE(env.get(env.started) == Strs{"loop", "dep", "loop", "dep"});

  // 2º aborto: reinício em 2 s
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor(restarting));
  env.clock.advance(1.5);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(restarting());
  env.clock.advance(0.6);
  REQUIRE(waitFor(bothUp));

  // 3º aborto dentro de 60 s: Failed, sem novas tentativas
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("loop", State::Failed); }));
  REQUIRE(contains(env.st("loop").lastError, "3"));
  REQUIRE(env.is("dep", State::Stopped));
  REQUIRE(contains(env.st("dep").lastError, "loop"));
  env.clock.advance(120);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  REQUIRE(env.is("loop", State::Failed));
  REQUIRE(env.get(env.started).size() == 6);
  REQUIRE(env.ev("hang"));  // a fila aceita; o resource parado só não roda
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(env.get(env.started).size() == 6);
}

TEST_CASE("host: timeouts outside the window do not accumulate; manual start resets the history") {
  HostEnv env;
  env.addRes("loop", {}, "return { hang = function() while true do end end }");
  env.boot();
  for (int i = 0; i < 4; ++i) {
    REQUIRE(env.ev("hang"));
    REQUIRE(waitFor([&] { return env.is("loop", State::Stopped); }));
    env.clock.advance(61);  // cada aborto cai fora da janela do anterior
    REQUIRE(waitFor([&] { return env.is("loop", State::Running); }));
  }
  // manual: stop cancela o reinício pendente
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("loop", State::Stopped); }));
  REQUIRE(env.call(env.host->stopResource("loop")));
  env.clock.advance(10);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(env.is("loop", State::Stopped));
  REQUIRE(env.call(env.host->startResource("loop")));
  REQUIRE(env.is("loop", State::Running));
}

TEST_CASE("host: event queue is bounded, drops client events and never Join/Leave") {
  REQUIRE(HostOptions{}.eventQueueCapacity == 65536);
  HostEnv env;
  env.opt.eventQueueCapacity = 4;
  env.addRes("q", {}, "return { stuck = function() block() end }");
  env.boot();
  REQUIRE(env.ev("stuck"));
  REQUIRE(waitFor([&] { return env.blocker.entered.load() == 1; }));
  // o evento "stuck" ainda está sendo processado e conta na capacidade (4): cabem mais 3 eventos de cliente
  struct Out {
    bool a[3];
    bool fourth, join, leave, cmd;
  };
  Out o = guarded("post while script thread is busy", [&] {
    Out r{};
    for (int i = 0; i < 3; ++i) r.a[i] = env.ev("a" + std::to_string(i));
    r.fourth = env.ev("a3");
    r.join = env.host->post(EvPlayerJoin{PlayerView{7, "p", "d", "s", 0, 0, 0, 0, 0}});
    r.leave = env.host->post(EvPlayerLeave{7, "d", "quit", 0});
    r.cmd = env.host->post(EvCommand{std::nullopt, "", "x", nullptr});
    return r;
  });
  for (bool b : o.a) REQUIRE(b);
  REQUIRE_FALSE(o.fourth);
  REQUIRE(o.join);
  REQUIRE(o.leave);
  REQUIRE(o.cmd);  // comandos (admin, raros) não são descartados na capacidade, só no teto rígido
  auto m = env.host->metrics();
  REQUIRE(m.eventsDropped == 1);
  REQUIRE(m.eventsBackstopDropped == 0);
  REQUIRE(m.eventQueueDepth == 7);  // 6 na fila + o que está em processamento
  env.blocker.release();
  REQUIRE(waitFor([&] { return env.get(env.seen).size() == 7; }));
  REQUIRE(env.get(env.seen) == Strs{"ev:stuck", "ev:a0", "ev:a1", "ev:a2", "join:7", "leave:7", "cmd:x"});
  REQUIRE(env.host->metrics().eventQueueDepth == 0);
}

TEST_CASE("host: players snapshot, tasks, command replies and the command queue limit") {
  HostEnv env;
  env.opt.commandQueueCapacity = 3;
  env.addRes("p", {}, "return {}");
  env.boot();
  auto snap = std::make_shared<std::vector<PlayerView>>(3);
  env.host->publishPlayers(snap);
  REQUIRE(env.ev("__players"));
  REQUIRE(waitFor([&] { return env.gotCmd("players=3"); }));

  env.host->runOnScriptThread([](ScriptContext& ctx) { ctx.emit(CmdKick{5, "x", 0}); });
  std::vector<ScriptToSim> out;
  REQUIRE(waitFor([&] {
    env.host->drainCommands(out);
    return !out.empty();
  }));
  REQUIRE(std::get<CmdKick>(out[0]).id == 5);

  auto reply = std::make_shared<std::promise<std::string>>();
  auto fut = reply->get_future();
  REQUIRE(env.host->post(EvCommand{std::nullopt, "did1", "hello", [reply](std::string s) { reply->set_value(s); }}));
  REQUIRE(guarded("reply", [&] { return fut.get(); }) == "ok hello");

  // fila scripts → simulação limitada
  env.host->runOnScriptThread([](ScriptContext& ctx) {
    for (int i = 0; i < 5; ++i) ctx.emit(CmdEmit{i, "e", "{}", 0, 0});
    // Fila de emits cheia: kick/teleporte vão pelo canal prioritário e não são descartados.
    ctx.emit(CmdKick{9, "k", 0});
    ctx.emit(CmdSetPosition{9, 1, 2, 3, 0});
  });
  REQUIRE(waitFor([&] { return env.host->metrics().commandsDropped == 2; }));
  out.clear();
  REQUIRE(waitFor([&] {
    env.host->drainPriority(out);
    return out.size() >= 2;
  }));
  REQUIRE(out.size() == 2);
  REQUIRE(std::holds_alternative<CmdKick>(out[0]));
  REQUIRE(std::holds_alternative<CmdSetPosition>(out[1]));
  out.clear();
  env.host->drainCommands(out);
  REQUIRE(out.size() == 3);
  REQUIRE(env.host->metrics().priorityDropped == 0);
}

TEST_CASE("host: supervisor halts the host when a C call never returns") {
  HostEnv env;
  env.addRes("s1", {}, "return { stuck = function() block() end }");
  env.addRes("s2", {}, "return {}");
  env.boot();
  REQUIRE(env.ev("stuck"));
  REQUIRE(waitFor([&] { return env.blocker.entered.load() == 1 && env.host->metrics().inLuaCall; }));

  // abaixo de abortAfter (0,2 s) + 10 s: só o aviso de chamada presa
  env.clock.advance(10.0);
  REQUIRE(waitFor([&] { return env.hasLog(LogLevel::Warn, "sem progresso"); }));
  INFO(env.allLogs());
  REQUIRE(env.hasLog(LogLevel::Warn, "[s1]"));
  REQUIRE_FALSE(env.host->metrics().halted);

  env.clock.advance(0.5);
  REQUIRE(waitFor([&] { return env.hasLog(LogLevel::Critical, "[s1]"); }));
  REQUIRE(waitFor([&] { return env.host->metrics().halted; }));  // CRÍTICO é logado antes de `halted`
  REQUIRE(contains(env.host->metrics().haltReason, "s1"));
  for (auto& s : env.host->status()) {
    INFO(s.name);
    REQUIRE(s.state == State::Failed);
    REQUIRE(!s.lastError.empty());
  }

  // a simulação segue: nada bloqueia, eventos são descartados e contados, controle recusa na hora
  struct Out {
    bool ev, join;
    size_t cmds;
    bool start, restart;
  };
  Out o = guarded("simulation calls while halted", [&] {
    Out r{};
    r.ev = env.ev("x");
    r.join = env.host->post(EvPlayerJoin{});
    env.host->publishPlayers(std::make_shared<std::vector<PlayerView>>(2));
    std::vector<ScriptToSim> out;
    env.host->drainCommands(out);
    r.cmds = out.size();
    (void)env.host->status();
    r.start = env.host->startResource("s2").get();
    r.restart = env.host->restartResource("s1").get();
    return r;
  });
  REQUIRE_FALSE(o.ev);
  REQUIRE_FALSE(o.join);
  REQUIRE_FALSE(o.start);
  REQUIRE_FALSE(o.restart);
  REQUIRE(env.host->metrics().eventsDropped >= 2);

  // a chamada volta: o host continua parado
  env.blocker.release();
  REQUIRE(waitFor([&] { return !env.host->metrics().inLuaCall; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(env.host->metrics().halted);
  REQUIRE(env.is("s1", State::Failed));
  REQUIRE(env.is("s2", State::Failed));
  REQUIRE_FALSE(env.ev("y"));
  REQUIRE_FALSE(env.call(env.host->startResource("s2")));
  REQUIRE(env.get(env.started).size() == 2);
}

TEST_CASE("host: hard backstop at 4x capacity drops even Join/Leave with a critical log") {
  HostEnv env;
  env.opt.eventQueueCapacity = 2;  // teto rígido = 8
  env.addRes("q", {}, "return { stuck = function() block() end }");
  env.boot();
  REQUIRE(env.ev("stuck"));
  REQUIRE(waitFor([&] { return env.blocker.entered.load() == 1; }));
  struct Out {
    int accepted = 0;
    bool overJoin = true, overCmd = true;
  };
  Out o = guarded("posts up to the backstop", [&] {
    Out r;
    // "stuck" em processamento conta: 1 evento de cliente cabe na capacidade (2) e 6 Join até o teto (8)
    r.accepted += env.ev("e") ? 1 : 0;
    r.accepted += env.ev("e2") ? 1 : 0;  // capacidade de cliente cheia: descartado
    for (int i = 0; i < 6; ++i) r.accepted += env.host->post(EvPlayerJoin{}) ? 1 : 0;
    r.overJoin = env.host->post(EvPlayerLeave{});
    r.overCmd = env.host->post(EvCommand{std::nullopt, "", "c", nullptr});
    return r;
  });
  REQUIRE(o.accepted == 7);
  REQUIRE_FALSE(o.overJoin);
  REQUIRE_FALSE(o.overCmd);
  auto m = env.host->metrics();
  REQUIRE(m.eventsBackstopDropped == 2);
  REQUIRE(m.eventsDropped == 3);
  REQUIRE(m.eventQueueDepth == 8);
  int criticals = 0;
  {
    std::lock_guard<std::mutex> lk(env.mu);
    for (auto& [lv, msg] : env.logs)
      if (lv == LogLevel::Critical && contains(msg, "teto")) ++criticals;
  }
  REQUIRE(criticals == 1);  // um log por episódio
  env.blocker.release();
  REQUIRE(waitFor([&] { return env.host->metrics().eventQueueDepth == 0 && env.get(env.seen).size() == 8; }));
}

TEST_CASE("host: dependency chain waits for a dependency that timed out in its main script at boot") {
  HostEnv env;  // revisão ATK1
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {"c"}, "return {}");
  env.addRes("c", {}, "if once() then while true do end end return {}");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.is("c", State::Stopped));
  REQUIRE(contains(env.st("c").lastError, "rein"));
  for (const char* n : {"a", "b"}) {
    INFO(n << ": " << env.st(n).lastError);
    REQUIRE(env.is(n, State::Stopped));
    REQUIRE(contains(env.st(n).lastError, "aguardando"));
  }
  env.clock.advance(1.5);
  REQUIRE(waitFor([&] { return env.is("a", State::Running) && env.is("b", State::Running) && env.is("c", State::Running); }));
  REQUIRE(env.get(env.started) == Strs{"c", "b", "a"});
}

TEST_CASE("host: manual start of a transitive dependent while the dependency restart is pending") {
  HostEnv env;  // revisão ATK2
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {"c"}, "return {}");
  env.addRes("c", {}, "return { hang = function() while true do end end }");
  env.boot();
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("c", State::Stopped) && env.is("a", State::Stopped); }));
  REQUIRE_FALSE(env.call(env.host->startResource("a")));  // fica aguardando, não Failed
  INFO(env.allLogs());
  REQUIRE(env.is("a", State::Stopped));
  REQUIRE(contains(env.st("a").lastError, "aguardando"));
  REQUIRE(env.is("b", State::Stopped));
  REQUIRE(contains(env.st("b").lastError, "aguardando"));
  env.clock.advance(1.5);
  REQUIRE(waitFor([&] { return env.is("a", State::Running) && env.is("b", State::Running) && env.is("c", State::Running); }));
  // falha definitiva da dependência propaga "falhou" pela cadeia (o 1º aborto já contou; mais 2 = 3 em 60 s)
  for (int i = 0; i < 2; ++i) {
    REQUIRE(waitFor([&] { return env.is("c", State::Running); }));
    REQUIRE(env.ev("hang"));
    REQUIRE(waitFor([&] { return !env.is("c", State::Running); }));
    env.clock.advance(5);
  }
  REQUIRE(waitFor([&] { return env.is("c", State::Failed); }));
  for (const char* n : {"a", "b"}) {
    INFO(n << ": " << env.st(n).lastError);
    REQUIRE(env.is(n, State::Stopped));
    REQUIRE(contains(env.st(n).lastError, "falhou"));
  }
}

TEST_CASE("host: control futures resolve false when the supervisor halts, even the stuck one") {
  HostEnv env;  // revisão ATK3
  env.addRes("s1", {}, "return {}");
  env.addRes("s2", {}, "return {}");
  env.boot();
  put(env.dir.p / "s1" / "server" / "main.lua", "block() return {}");
  auto stuck = env.host->restartResource("s1");
  REQUIRE(waitFor([&] { return env.blocker.entered.load() == 1; }));
  auto queued = env.host->startResource("s2");  // fica na fila atrás da tarefa presa
  env.clock.advance(11);
  REQUIRE(waitFor([&] { return env.host->metrics().halted; }));
  REQUIRE(stuck.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  REQUIRE(queued.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  REQUIRE_FALSE(stuck.get());
  REQUIRE_FALSE(queued.get());
  env.blocker.release();
  REQUIRE(waitFor([&] { return !env.host->metrics().inLuaCall; }));
  REQUIRE(env.host->metrics().halted);
}

TEST_CASE("host: a stuck call that returns after the host is destroyed touches nothing") {
  // Revisão ATK4 (use-after-free depois do detach). Tudo que a thread desanexada ainda pode alcançar (relógio,
  // Blocker, marcas lidas pelos ganchos) é dono de shared_ptr capturado pelos fechos do host, então sobrevive ao
  // HostEnv; a thread estacionada é confirmada pelo contador antes de qualquer verificação e antes do HostEnv morrer.
  struct Late {
    std::atomic<bool> destroyed{false};
    std::atomic<int> logs{0}, hooks{0};
  };
  auto late = std::make_shared<Late>();
  const size_t parkedBefore = ScriptHost::parkedThreadCount();
  HostEnv env;
  auto prevLog = env.opt.log;
  env.opt.log = [late, prevLog](LogLevel l, const std::string& m) {
    if (late->destroyed.load()) {
      late->logs.fetch_add(1);
      return;  // nunca chama o HostEnv depois de destruído
    }
    prevLog(l, m);
  };
  auto prevEvent = env.opt.onEvent;
  env.opt.onEvent = [late, prevEvent](ScriptContext& ctx, SimToScript& ev) {
    if (late->destroyed.load()) {
      late->hooks.fetch_add(1);
      return;
    }
    prevEvent(ctx, ev);
    if (late->destroyed.load()) late->hooks.fetch_add(1);  // o despacho continuou depois do detach
  };
  auto prevStop = env.opt.onStopping;
  env.opt.onStopping = [late, prevStop](ScriptContext& ctx, HostedResource& r) {
    if (late->destroyed.load()) {
      late->hooks.fetch_add(1);
      return;
    }
    prevStop(ctx, r);
  };
  // depois de voltar do bloqueio o script ainda tenta imprimir (log) e o despachante tenta outro resource
  env.addRes("s1", {}, "return { stuck = function() block() print('depois') end }");
  env.addRes("s2", {}, "return { stuck = function() print('s2') end }");
  env.boot();
  REQUIRE(env.ev("stuck"));
  REQUIRE(waitFor([&] { return env.blocker.entered.load() == 1; }));
  env.clock.advance(11);
  REQUIRE(waitFor([&] { return env.host->metrics().halted; }));
  env.destroy();  // desanexa a thread presa
  late->destroyed = true;
  env.clock.closed = true;
  env.blocker.release();
  const bool parked = waitFor([&] { return ScriptHost::parkedThreadCount() == parkedBefore + 1; });
  if (!parked) {
    // A thread não estacionou: ela ainda pode tocar no HostEnv. Não deixa o teste seguir (evita UAF no relatório).
    std::fprintf(stderr, "FALHA: a thread de scripts desanexada não estacionou\n");
    std::fflush(stderr);
    std::quick_exit(3);
  }
  REQUIRE(late->logs.load() == 0);
  REQUIRE(late->hooks.load() == 0);
  REQUIRE(env.clock.lateReads.load() == 0);
}

TEST_CASE("host: admin stop of a waiting dependent is honored when the dependency returns") {
  HostEnv env;  // revisão ATK8
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {"c"}, "return {}");
  env.addRes("c", {}, "return { hang = function() while true do end end }");
  env.boot();
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("c", State::Stopped) && env.is("a", State::Stopped); }));
  REQUIRE(env.call(env.host->stopResource("b")));
  REQUIRE(env.st("b").lastError == "parado pelo admin");
  REQUIRE(contains(env.st("a").lastError, "parado junto com 'b'"));
  env.clock.advance(1.5);
  REQUIRE(waitFor([&] { return env.is("c", State::Running); }));
  // c voltou; b (parado pelo admin) e a (dependente dele) não voltam sozinhos
  REQUIRE(env.call(env.host->startResource("c")));  // barreira: o laço já passou pelo bringBack de c
  INFO(env.allLogs());
  REQUIRE(env.is("b", State::Stopped));
  REQUIRE(env.is("a", State::Stopped));
  REQUIRE(env.st("b").lastError == "parado pelo admin");
  REQUIRE(env.get(env.started) == Strs{"c", "b", "a", "c"});
}

TEST_CASE("host: admin stop keeps the reason of dependents that were already stopped") {
  HostEnv env;  // revisão round 3, M-1
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {}, "return {}");
  env.addRes("x", {"b"}, "return {}");
  env.boot();
  REQUIRE(env.call(env.host->stopResource("a")));
  REQUIRE(env.st("a").lastError == "parado pelo admin");
  REQUIRE(env.call(env.host->stopResource("b")));
  REQUIRE(env.st("b").lastError == "parado pelo admin");
  REQUIRE(env.st("a").lastError == "parado pelo admin");     // já estava parado: mantém o motivo
  REQUIRE(env.st("x").lastError == "parado junto com 'b'");  // rodava: ganha o motivo novo
}

TEST_CASE("host: admin stop of a dependency with a pending restart cancels it and tells the waiting dependents") {
  HostEnv env;  // revisão ATK11
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {"c"}, "return {}");
  env.addRes("c", {}, "return { hang = function() while true do end end }");
  env.boot();
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("c", State::Stopped) && env.is("a", State::Stopped); }));
  REQUIRE(env.call(env.host->stopResource("c")));
  REQUIRE(env.st("c").lastError == "parado pelo admin");
  for (const char* n : {"a", "b"}) {
    INFO(n << ": " << env.st(n).lastError);
    REQUIRE(env.is(n, State::Stopped));
    REQUIRE(contains(env.st(n).lastError, "a dependência 'c' foi parada"));
  }
  env.clock.advance(5);
  REQUIRE_FALSE(env.call(env.host->stopResource("nope")));  // barreira: o laço já passou por pollRestarts
  REQUIRE(env.is("c", State::Stopped));
  REQUIRE(env.is("b", State::Stopped));
  REQUIRE(env.is("a", State::Stopped));
  REQUIRE(env.get(env.started) == Strs{"c", "b", "a"});
}

TEST_CASE("host: restart of a waiting dependent, then the dependency fails for good") {
  HostEnv env;  // revisão ATK12
  env.addRes("a", {"b"}, "return {}");
  env.addRes("b", {"c"}, "return {}");
  env.addRes("c", {}, "return { hang = function() while true do end end }");
  env.boot();
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("c", State::Stopped); }));
  REQUIRE_FALSE(env.call(env.host->restartResource("a")));  // aguardando c
  REQUIRE(contains(env.st("a").lastError, "aguardando"));
  env.clock.advance(1.5);
  REQUIRE(waitFor([&] { return env.is("c", State::Running) && env.is("b", State::Running) && env.is("a", State::Running); }));
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("c", State::Stopped); }));
  env.clock.advance(2.5);
  REQUIRE(waitFor([&] { return env.is("a", State::Running); }));
  REQUIRE(env.ev("hang"));
  REQUIRE(waitFor([&] { return env.is("c", State::Failed); }));
  INFO(env.allLogs());
  for (const char* n : {"a", "b"}) {
    INFO(n << ": " << env.st(n).lastError);
    REQUIRE(env.is(n, State::Stopped));
    REQUIRE(contains(env.st(n).lastError, "falhou"));
  }
}

TEST_CASE("host: capacity and backstop count the batch being processed") {
  HostEnv env;  // revisão ATK14
  env.opt.eventQueueCapacity = 4;  // teto rígido = 16
  env.addRes("q", {}, "return { stuck = function() block() end }");
  env.boot();
  REQUIRE(env.ev("stuck"));
  for (int i = 0; i < 15; ++i) REQUIRE(env.host->post(EvPlayerJoin{PlayerView{static_cast<uint16_t>(i), "p", "d", "s", 0, 0, 0, 0, 0}}));
  REQUIRE(waitFor([&] { return env.blocker.entered.load() == 1; }));
  // a thread de scripts já tirou os 16 da fila (lote em processamento), mas eles ainda contam
  int accepted = guarded("posts at the backstop", [&] {
    int acc = 0;
    for (int i = 0; i < 40; ++i)
      if (env.host->post(EvPlayerJoin{PlayerView{static_cast<uint16_t>(100 + i), "p", "d", "s", 0, 0, 0, 0, 0}})) ++acc;
    if (env.ev("late")) ++acc;
    return acc;
  });
  REQUIRE(accepted == 0);
  auto m = env.host->metrics();
  REQUIRE(m.eventQueueDepth == 16);
  REQUIRE(m.eventsBackstopDropped == 40);
  REQUIRE(m.eventsDropped == 41);
  env.blocker.release();
  REQUIRE(waitFor([&] { return env.host->metrics().eventQueueDepth == 0; }));
  REQUIRE(env.ev("again"));  // abaixo da capacidade de novo
}

TEST_CASE("host: the script to simulation command queue is bounded in bytes") {
  HostEnv env;
  env.opt.commandQueueBytes = 20000;
  env.addRes("a", {}, "return {}");
  env.boot();
  auto p = std::make_shared<std::promise<int>>();
  auto fut = p->get_future();
  env.host->runOnScriptThread([p](ScriptContext& ctx) {
    int ok = 0;
    for (int i = 0; i < 40; ++i) ok += ctx.emit(CmdEmit{-1, "e", std::string(1000, 'x'), 0, 0}) ? 1 : 0;
    p->set_value(ok);
  });
  const int accepted = guarded("emit", [&fut] { return fut.get(); });
  REQUIRE(accepted >= 10);
  REQUIRE(accepted < 20);
  REQUIRE(env.host->metrics().commandsDropped == static_cast<uint64_t>(40 - accepted));
  std::vector<ScriptToSim> out;
  env.host->drainCommands(out);
  REQUIRE(out.size() == static_cast<size_t>(accepted));
  // Drenou: cabe de novo.
  auto p2 = std::make_shared<std::promise<bool>>();
  auto f2 = p2->get_future();
  env.host->runOnScriptThread([p2](ScriptContext& ctx) { p2->set_value(ctx.emit(CmdEmit{-1, "e", std::string(1000, 'x'), 0, 0})); });
  REQUIRE(guarded("emit2", [&f2] { return f2.get(); }));
}

TEST_CASE("host: the priority channel coalesces teleports and kicks; emits are fair between sources") {
  HostEnv env;
  env.opt.commandQueueCapacity = 40;
  env.addRes("a", {}, "return {}");
  env.boot();
  auto p = std::make_shared<std::promise<int>>();
  auto fut = p->get_future();
  env.host->runOnScriptThread([p](ScriptContext& ctx) {
    for (int i = 0; i < 5000; ++i) ctx.emit(CmdSetPosition{1, i, 0, 0, 7});
    ctx.emit(CmdSetPosition{1, 1, 1, 1, 8});  // outra sessão: entrada própria
    ctx.emit(CmdKick{2, "primeiro", 9});
    ctx.emit(CmdKick{2, "segundo", 9});
    ctx.emit(CmdKickDid{"d", "a"});
    ctx.emit(CmdKickDid{"d", "b"});
    int ok = 0;
    for (int i = 0; i < 40; ++i) ok += ctx.emit(CmdEmit{-1, "f", "0", 0, 1}) ? 1 : 0;  // origem 1: 1/4 da fila
    ok += ctx.emit(CmdEmit{-1, "g", "0", 0, 2}) ? 100 : 0;                         // origem 2 ainda cabe
    p->set_value(ok);
  });
  const int ok = guarded("emit", [&fut] { return fut.get(); });
  REQUIRE(ok == 110);
  std::vector<ScriptToSim> out;
  env.host->drainPriority(out);
  REQUIRE(out.size() == 4);
  REQUIRE(std::get<CmdSetPosition>(out[0]).x == 4999);  // o último teleporte, na posição do primeiro
  REQUIRE(std::get<CmdSetPosition>(out[1]).session == 8);
  REQUIRE(std::get<CmdKick>(out[2]).reason == "primeiro");
  REQUIRE(std::get<CmdKickDid>(out[3]).reason == "a");
  REQUIRE(env.host->metrics().priorityDropped == 0);
  out.clear();
  env.host->drainCommands(out);
  REQUIRE(out.size() == 11);
}
