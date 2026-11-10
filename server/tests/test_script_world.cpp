// Integração World (simulação) ↔ ScriptHost + API cb.* pela ScriptBridge, com o resource de exemplo `echo` em Lua.
#include <catch2/catch_test_macros.hpp>

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

  explicit Env(const std::string& probe) {
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
    cbscript::installApi(opt, {});
    cbscript::Limits lim;
    lim.warnAfter = std::chrono::milliseconds(50);
    lim.abortAfter = std::chrono::milliseconds(300);
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
  Env env(kProbe);
  auto a = env.h.add("ana");
  REQUIRE(env.ticksUntil([&] { return env.hasLog("join 1 ana"); }));
  REQUIRE(env.h.world.onEvent(a, Event{"spin", bytes("{}")}, env.h.now));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));  // a thread de scripts está presa no laço de 250 ms
  const auto t0 = Clock::now();
  for (int i = 0; i < 50; ++i) {
    env.h.world.onEvent(a, Event{"x", bytes("{}")}, env.h.now);
    env.tick();
  }
  const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  INFO("50 ticks com a thread de scripts ocupada: " << ms << " ms");
  REQUIRE(ms < 150);
  REQUIRE(env.ticksUntil([&] { return env.hasLog("spun"); }));
}
