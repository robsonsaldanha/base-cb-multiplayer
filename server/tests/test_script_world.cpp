// Integração World (simulação) ↔ ScriptHost + API cb.* pela ScriptBridge, com o resource de exemplo `echo` em Lua.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
#include <string>
#include <thread>
#include <vector>

#include "cbscript/api.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/host.hpp"
#include "cbserver/script_bridge.hpp"
#include "world_harness.hpp"

namespace fs = std::filesystem;
using cbmp::Event;
using testing_world::Harness;

namespace {

using Clock = std::chrono::steady_clock;

// Prazo rígido (o teste não pode pendurar o CI): roda f numa thread; se não voltar a tempo, o processo sai.
template <class F>
auto hard(const char* what, F f, std::chrono::seconds deadline = std::chrono::seconds(10)) -> decltype(f()) {
  using R = decltype(f());
  auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
  auto fut = task->get_future();
  std::thread([task] { (*task)(); }).detach();
  if (fut.wait_for(deadline) != std::future_status::ready) {
    std::fprintf(stderr, "FALHA: travou por mais de %lld s: %s\n", static_cast<long long>(deadline.count()), what);
    std::fflush(stderr);
    std::quick_exit(3);
  }
  return fut.get();
}

struct Tmp {
  fs::path p;
  Tmp() {
    static std::atomic<unsigned> n{0};
    std::random_device rd;
    p = fs::temp_directory_path() / ("cbbridge_" + std::to_string(rd()) + "_" + std::to_string(n++));
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

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string text(const std::vector<uint8_t>& b) { return {b.begin(), b.end()}; }

struct Env {
  Tmp dir;
  Harness h;
  std::mutex mu;
  std::vector<std::string> logs;
  std::unique_ptr<cbscript::ScriptHost> host;
  std::unique_ptr<cbserver::ScriptBridge> bridge;

  explicit Env(const std::string& probe, std::chrono::milliseconds abortAfter = std::chrono::milliseconds(300),
               std::shared_ptr<cbscript::BanStore> bans = nullptr) {
    fs::copy(fs::path(CBMP_EXAMPLE_RESOURCES) / "echo", dir.p / "echo", fs::copy_options::recursive);
    put(dir.p / "probe" / "resource.json",
        R"({"name":"probe","version":"1.0.0","server":["server/main.lua"]})");
    put(dir.p / "probe" / "server" / "main.lua", probe);
    cbscript::HostOptions opt;
    opt.idleWait = std::chrono::milliseconds(2);
    opt.supervisorPeriod = std::chrono::milliseconds(5);
    opt.log = [this](cbscript::LogLevel, const std::string& m) {
      std::lock_guard<std::mutex> lk(mu);
      logs.push_back(m);
    };
    cbscript::ApiOptions api;
    api.bans = std::move(bans);
    cbscript::installApi(opt, api);
    cbscript::Limits lim;
    lim.warnAfter = std::chrono::milliseconds(50);
    lim.abortAfter = abortAfter;
    host = std::make_unique<cbscript::ScriptHost>(dir.p, lim, [] {
      return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    }, opt);
    hard("start", [this] {
      host->start();
      return 0;
    });
    bridge = std::make_unique<cbserver::ScriptBridge>(h.world, *host);
  }
  ~Env() {
    bridge.reset();
    hard("destroy", [this] {
      host.reset();
      return 0;
    });
  }
  bool hasLog(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& l : logs)
      if (l.find(s) != std::string::npos) return true;
    return false;
  }
  std::string allLogs() {
    std::lock_guard<std::mutex> lk(mu);
    std::string r;
    for (auto& l : logs) r += l + "\n";
    return r;
  }
  // Um "tick" do servidor como no main: comandos dos scripts → tick → foto.
  void tick() {
    bridge->applyCommands(h.now + 0.05);
    h.step();
    bridge->publishPlayers();
  }
  // Roda ticks até pred (com o resultado do tick em h.out/h.kicks) ou 5 s.
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
};

const char* kProbe = R"(
  cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id .. ' ' .. p.name .. ' ' .. p.did) end)
  cb.on('playerLeave', function(p, why) cb.log.info('leave ' .. p.id .. ' ' .. why) end)
  cb.events.on('tp', function(p, d) cb.players.setPosition(p.id, d.x, d.y, d.z) end)
  cb.events.on('kickme', function(p) cb.players.kick(p.id, 'pedido') end)
  cb.events.on('who', function(p)
    local me = cb.players.get(p.id)
    cb.events.emit(p.id, 'who', {#cb.players.list(), me and me.x or -1})
  end)
  cb.events.on('all', function(p) cb.events.emit(-1, 'all', {from = p.id}) end)
  cb.events.on('spin', function() local t = os.clock() while os.clock() - t < 0.25 do end cb.log.info('spun') end)
  cb.commands.register('hello', function(src, args) return 'oi ' .. src end)
)";

}  // namespace

TEST_CASE("bridge: join reaches Lua and the Lua echo resource answers the client event") {
  Env env(kProbe);
  INFO(env.allLogs());
  auto a = env.h.add("ana", {100, 0, 0});
  auto b = env.h.add("bia", {200, 0, 0});
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1 ana did-ana") && env.hasLog("join 2 bia did-bia"); }));

  const std::string payload = R"({"t":123456789})";
  REQUIRE(env.h.world.onEvent(a, Event{"echo", bytes(payload)}, env.h.now));
  std::vector<Event> got;
  REQUIRE(env.ticksUntil([&] {
    auto ev = env.h.msgs<Event>(a);
    got.insert(got.end(), ev.begin(), ev.end());
    return !got.empty();
  }));
  REQUIRE(got.size() == 1);
  REQUIRE(got[0].name == "echo");
  REQUIRE(text(got[0].payload) == payload);
  env.tick();
  REQUIRE(env.h.msgs<Event>(b).empty());  // eco só para o remetente

  // Broadcast (-1) chega a todos.
  REQUIRE(env.h.world.onEvent(b, Event{"all", bytes("{}")}, env.h.now));
  REQUIRE(env.ticksUntil([&] { return !env.h.msgs<Event>(a).empty() && !env.h.msgs<Event>(b).empty(); }));
  REQUIRE(text(env.h.msgs<Event>(a)[0].payload) == R"({"from":2})");

  env.h.world.removePlayer(a, "saiu do jogo");
  REQUIRE(env.ticksUntil([&] { return env.hasLog("leave 1 saiu do jogo"); }));
}

TEST_CASE("bridge: snapshot, setPosition with loading grace and kick from Lua reach the World") {
  Env env(kProbe);
  INFO(env.allLogs());
  auto a = env.h.add("ana", {100, 0, 0});
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1 ana"); }));

  REQUIRE(env.h.world.onEvent(a, Event{"who", bytes("{}")}, env.h.now));
  REQUIRE(env.ticksUntil([&] { return !env.h.msgs<Event>(a).empty(); }));
  REQUIRE(text(env.h.msgs<Event>(a)[0].payload) == "[1,100]");

  REQUIRE(env.h.world.onEvent(a, Event{"tp", bytes(R"({"x":5000,"y":-7,"z":12})")}, env.h.now));
  REQUIRE(env.ticksUntil([&] { return !env.h.msgs<cbmp::Correction>(a).empty(); }));
  auto c = env.h.msgs<cbmp::Correction>(a)[0];
  REQUIRE(c.pos == cbmp::Vec3i{5000, -7, 12});
  const auto* slot = env.h.world.find(a);
  REQUIRE(slot->state.pos == cbmp::Vec3i{5000, -7, 12});
  REQUIRE(slot->correcting);
  REQUIRE(slot->serverCorrection);  // carência de carregamento: não conta para kick

  REQUIRE(env.h.world.onEvent(a, Event{"kickme", bytes("{}")}, env.h.now));
  REQUIRE(env.ticksUntil([&] { return !env.h.kicks.empty(); }));
  REQUIRE(env.h.kicks[0].who == a);
  REQUIRE(env.h.kicks[0].reason == "pedido");
}

TEST_CASE("bridge: kick and setPosition aimed at a reused id do not hit the new player") {
  Env env(kProbe);
  auto a = env.h.add("ana", {100, 0, 0});
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1 ana"); }));
  // O script decide expulsar 'ana' (id 1), mas antes de o comando chegar ela sai e 'caio' pega o id 1.
  REQUIRE(env.h.world.onEvent(a, Event{"kickme", bytes("{}")}, env.h.now));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  env.h.world.removePlayer(a);
  auto c = env.h.add("caio", {0, 0, 0});
  REQUIRE(c == a);
  for (int i = 0; i < 20; ++i) env.tick();
  REQUIRE(env.h.world.find(c) != nullptr);
  REQUIRE_FALSE(env.h.world.find(c)->kicked);
}

TEST_CASE("bridge: chat command arrives as cb:command and the reply comes back as cb:reply") {
  Env env(kProbe);
  INFO(env.allLogs());
  auto a = env.h.add("ana");
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1 ana"); }));
  REQUIRE(env.h.world.onEvent(a, Event{"cb:command", bytes(R"("/hello mundo")")}, env.h.now));
  REQUIRE(env.ticksUntil([&] { return !env.h.msgs<Event>(a).empty(); }));
  auto r = env.h.msgs<Event>(a)[0];
  REQUIRE(r.name == "cb:reply");
  REQUIRE(text(r.payload) == R"("oi 1")");
  // Outros nomes cb:* do cliente e cb:command malformado são descartados pela ponte.
  REQUIRE(env.h.world.onEvent(a, Event{"cb:reply", bytes("\"x\"")}, env.h.now));
  REQUIRE(env.h.world.onEvent(a, Event{"cb:command", bytes("{bad")}, env.h.now));
  for (int i = 0; i < 10; ++i) env.tick();
  REQUIRE(env.h.msgs<Event>(a).empty());
}

TEST_CASE("bridge: the simulation never waits for a busy script thread") {
  // Medida relativa (robusta sob ASan / 2 vCPU): a thread de scripts fica presa ~1,5 s; os 50 ticks têm de terminar
  // ANTES de ela voltar. Se a simulação esperasse pelo Lua, o "spun" já estaria no log ao fim dos ticks.
  Env env(R"(
    cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id) end)
    cb.events.on('spin', function() local t = os.clock() while os.clock() - t < 1.5 do end cb.log.info('spun') end)
  )", std::chrono::milliseconds(5000));
  auto a = env.h.add("ana");
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1"); }));
  REQUIRE(env.h.world.onEvent(a, Event{"spin", bytes("{}")}, env.h.now));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // a thread de scripts entrou no laço
  const auto t0 = Clock::now();
  for (int i = 0; i < 50; ++i) {
    env.h.world.onEvent(a, Event{"x", bytes("{}")}, env.h.now);
    env.tick();
  }
  const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  INFO("50 ticks com a thread de scripts ocupada: " << ms << " ms");
  REQUIRE_FALSE(env.hasLog("spun"));
  REQUIRE(ms < 1000);
  REQUIRE(env.ticksUntil([&] { return env.hasLog("spun"); }));
}

// ============================================== regressões dos ataques do revisor (Task 4, fix round 1)

TEST_CASE("bridge: replies to a 4096-byte cb:command and long command output fit in one encodable event") {
  Env env(R"(
    cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id) end)
    cb.commands.register('big', function() return string.rep('x', 5000) end)
  )");
  auto a = env.h.add("ana");
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1"); }));
  std::string payload = "\"" + std::string(4094, 'z') + "\"";
  REQUIRE(payload.size() == 4096);
  for (const std::string& line : {payload, std::string(R"("/big")")}) {
    REQUIRE(env.h.world.onEvent(a, Event{"cb:command", bytes(line)}, env.h.now));
    std::vector<Event> got;
    REQUIRE(env.ticksUntil([&] {
      auto ev = env.h.msgs<Event>(a);
      got.insert(got.end(), ev.begin(), ev.end());
      return !got.empty();
    }));
    INFO("reply payload bytes = " << got[0].payload.size());
    REQUIRE(got[0].name == "cb:reply");
    REQUIRE(got[0].payload.size() <= 4096);
    REQUIRE_NOTHROW(cbmp::encode(cbmp::Message{got[0]}));
  }
}

TEST_CASE("bridge: oversized CmdEmit from a script is dropped and counted, never sent") {
  Env env("cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id) end)");
  auto a = env.h.add("ana");
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1"); }));
  const uint32_t sess = env.h.world.find(a)->session;
  env.host->runOnScriptThread([sess](cbscript::ScriptContext& ctx) {
    ctx.emit(cbscript::CmdEmit{1, "ok", "{}", sess});
    ctx.emit(cbscript::CmdEmit{1, std::string(65, 'n'), "{}", sess});
    ctx.emit(cbscript::CmdEmit{-1, "big", std::string(4097, 'x'), 0});
  });
  REQUIRE(env.ticksUntil([&] { return !env.h.msgs<Event>(a).empty(); }));
  for (int i = 0; i < 5; ++i) env.tick();
  REQUIRE(env.bridge->commandsRejected() == 2);
  REQUIRE(env.h.msgs<Event>(a).empty());
}

TEST_CASE("bridge: cb.events.emit aimed at a player that left does not reach the new player with the reused id") {
  Env env(R"(
    cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id .. ' ' .. p.did) end)
    cb.events.on('secret', function(p) cb.events.emit(p.id, 'secret', {owner = p.did}) cb.log.info('sent') end)
  )");
  auto a = env.h.add("ana");
  REQUIRE(a == 1);
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1 did-ana"); }));
  REQUIRE(env.h.world.onEvent(a, Event{"secret", bytes("{}")}, env.h.now));
  for (int i = 0; i < 2500 && !env.hasLog("sent"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(env.hasLog("sent"));
  env.h.world.removePlayer(a, "saiu");
  auto b = env.h.add("bia");
  REQUIRE(b == 1);
  std::vector<Event> got;
  for (int i = 0; i < 20; ++i) {
    env.tick();
    auto ev = env.h.msgs<Event>(b);
    got.insert(got.end(), ev.begin(), ev.end());
  }
  INFO(env.allLogs());
  REQUIRE(got.empty());  // o dado de ana não chega a bia
}

TEST_CASE("bridge: ban issued while the target's join is still queued kicks the target") {
  auto bans = std::make_shared<cbscript::BanStore>(fs::path{});
  Env env(R"(
    cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id) end)
    cb.events.on('banbia', function(p) cb.players.ban('did-bia', 'teste') cb.log.info('banned') end)
  )", std::chrono::milliseconds(300), bans);
  auto adm = env.h.add("adm");
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1"); }));
  // Mesmo tick: o evento que bane chega antes do Join de bia (a admissão de bia já tinha passado).
  REQUIRE(env.h.world.onEvent(adm, Event{"banbia", bytes("{}")}, env.h.now));
  auto b = env.h.add("bia");
  REQUIRE(env.ticksUntil([&] {
    for (auto& k : env.h.kicks)
      if (k.who == b) return true;
    return false;
  }));
  REQUIRE(env.hasLog("banned"));
  REQUIRE(bans->check("did-bia", 0).has_value());
  REQUIRE(env.h.world.find(b)->kicked);
}

TEST_CASE("bridge: a script burst of broadcasts is spread over ticks with bounded work and memory") {
  Env env(R"(
    cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id) end)
    cb.events.on('burst', function(p)
      local s = string.rep('a', 1000)
      local n = 0
      for i = 1, 65536 do if cb.events.emit(-1, 'b', s) then n = n + 1 end end
      cb.log.info('queued ' .. n)
    end)
  )", std::chrono::milliseconds(5000));
  std::vector<cbserver::EntityId> ids;
  for (int i = 0; i < 16; ++i) ids.push_back(env.h.add("p" + std::to_string(i)));
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 16"); }));
  REQUIRE(env.h.world.onEvent(ids[0], Event{"burst", bytes("{}")}, env.h.now));
  for (int i = 0; i < 5000 && !env.hasLog("queued"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(env.hasLog("queued"));
  // Fila do host limitada em bytes (16 MiB): só uma parte dos 65 536 × 1000 bytes foi aceita.
  const auto hm = env.host->metrics();
  REQUIRE(hm.commandsDropped > 40000);
  double worst = 0;
  size_t maxEvents = 0, totalEvents = 0;
  for (int t = 0; t < 40; ++t) {
    const auto t0 = Clock::now();
    env.tick();
    worst = std::max(worst, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    size_t evs = 0;
    for (auto& o : env.h.out) evs += std::holds_alternative<cbmp::Event>(o.message()) ? 1 : 0;
    maxEvents = std::max(maxEvents, evs);
    totalEvents += evs;
  }
  INFO(env.allLogs());
  WARN("rajada: pior tick " << worst << " ms, máx " << maxEvents << " eventos/tick, total " << totalEvents
                            << ", cota descartou " << env.h.world.eventsOutDropped());
  REQUIRE(worst < 25);
  REQUIRE(maxEvents <= cbserver::ScriptBridge::kMaxCommandsPerTick * 16);
  // Cota de 128 KiB de burst + 64 KiB/s por jogador: em 2 s de jogo, no máximo ~260 KiB (≈ 260 eventos) cada.
  REQUIRE(totalEvents <= 16 * 270);
  REQUIRE(env.h.world.eventsOutDropped() > 0);
  REQUIRE(env.bridge->backlog() <= cbserver::ScriptBridge::kMaxCommandsPerTick * 40);
}
