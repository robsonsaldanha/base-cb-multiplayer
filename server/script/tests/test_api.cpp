// API cb.* contra um ScriptHost real (relógio falso) e a fila de comandos (simulação falsa).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "cbscript/api.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/host.hpp"
#include "test_util.hpp"

using namespace cbscript;
using State = ResourceStatus::State;

namespace {

struct Clock {
  std::atomic<double> t{1000.0};
  double get() const { return t.load(); }
  void advance(double s) { t.store(t.load() + s); }
};

template <class P>
bool waitUntil(P pred, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
  auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

struct ApiEnv {
  TmpDir dir;
  std::shared_ptr<Clock> clock = std::make_shared<Clock>();
  mutable std::mutex mu;
  std::vector<std::pair<LogLevel, std::string>> logs;
  std::vector<std::string> audits;
  std::vector<ScriptToSim> cmds;
  std::vector<std::string> replies;
  std::atomic<int64_t> unix{1'700'000'000};
  HostOptions opt;
  ApiOptions api;
  std::shared_ptr<BanStore> bans;
  std::unique_ptr<ScriptHost> host;

  ApiEnv() {
    opt.supervisorPeriod = std::chrono::milliseconds(5);
    opt.idleWait = std::chrono::milliseconds(2);
    opt.log = [this](LogLevel l, const std::string& m) {
      std::lock_guard<std::mutex> lk(mu);
      logs.emplace_back(l, m);
    };
    bans = std::make_shared<BanStore>(dir.p / "bans.json");
    api.bans = bans;
    api.audit = [this](const std::string& line) {
      std::lock_guard<std::mutex> lk(mu);
      audits.push_back(line);
    };
    api.unixNow = [this] { return unix.load(); };
  }
  ~ApiEnv() {
    if (host)
      guarded("destroy", [this] {
        host.reset();
        return 0;
      });
  }
  void addRes(const std::string& name, const std::string& code, const std::vector<std::string>& deps = {},
              nlohmann::json config = nlohmann::json::object()) {
    nlohmann::json j = {{"name", name},     {"version", "1.0.0"}, {"server", {"server/main.lua"}},
                        {"dependencies", deps}, {"config", config}};
    put(dir.p / "res" / name / "resource.json", j.dump());
    put(dir.p / "res" / name / "server" / "main.lua", code);
  }
  void boot() {
    installApi(opt, api);
    host = std::make_unique<ScriptHost>(dir.p / "res", shortLimits(), [c = clock] { return c->get(); }, opt);
    guarded("start", [this] {
      host->start();
      return 0;
    });
  }
  bool call(std::future<bool> f) {
    return guarded("control", [&f] { return f.get(); });
  }
  bool hasLog(const std::string& sub, LogLevel l = LogLevel::Info) const {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [lv, m] : logs)
      if (lv == l && contains(m, sub)) return true;
    return false;
  }
  bool waitLog(const std::string& sub, LogLevel l = LogLevel::Info) const {
    return waitUntil([&] { return hasLog(sub, l); });
  }
  int countLog(const std::string& sub) const {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (auto& [lv, m] : logs) n += contains(m, sub) ? 1 : 0;
    return n;
  }
  // Posição (ordem) da primeira linha de log com `sub`; -1 se não houver.
  int logIndex(const std::string& sub) const {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < logs.size(); ++i)
      if (contains(logs[i].second, sub)) return static_cast<int>(i);
    return -1;
  }
  std::string allLogs() const {
    std::lock_guard<std::mutex> lk(mu);
    std::string s;
    for (auto& [lv, m] : logs) s += std::to_string(static_cast<int>(lv)) + ": " + m + "\n";
    return s;
  }
  std::vector<std::string> auditLines() const {
    std::lock_guard<std::mutex> lk(mu);
    return audits;
  }
  // Barreira: duas tarefas seguidas na thread de scripts garantem que tudo o que foi postado antes já foi
  // processado (tarefas rodam antes dos eventos da mesma volta do laço).
  void sync() {
    for (int i = 0; i < 2; ++i) {
      auto p = std::make_shared<std::promise<void>>();
      auto f = p->get_future();
      host->runOnScriptThread([p](ScriptContext&) { p->set_value(); });
      guarded("sync", [&f] {
        f.get();
        return 0;
      });
    }
  }
  void drain() {
    std::vector<ScriptToSim> out;
    host->drainCommands(out);
    for (auto& c : out) cmds.push_back(std::move(c));
  }
  template <class T, class P>
  const T* waitCmd(P pred) {
    const T* found = nullptr;
    waitUntil([&] {
      drain();
      for (auto& c : cmds)
        if (const T* x = std::get_if<T>(&c); x && pred(*x)) {
          found = x;
          return true;
        }
      return false;
    });
    return found;
  }
  void join(uint16_t id, const std::string& name, int32_t x = 0, int32_t y = 0, int32_t z = 0) {
    REQUIRE(host->post(EvPlayerJoin{PlayerView{id, name, "d" + std::to_string(id), "s" + std::to_string(id), x, y, z, 1}}));
  }
  void event(uint16_t id, const std::string& name, const std::string& json = "{}") {
    REQUIRE(host->post(EvClientEvent{id, name, json}));
  }
  void command(std::optional<uint16_t> from, const std::string& line, bool withReply = true) {
    EvCommand c;
    c.from = from;
    c.adminDid = from ? "" : "admin-did";
    c.line = line;
    if (withReply)
      c.reply = [this](std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        replies.push_back(std::move(s));
      };
    REQUIRE(host->post(std::move(c)));
  }
  bool hasReply(const std::string& s) const {
    std::lock_guard<std::mutex> lk(mu);
    return std::find(replies.begin(), replies.end(), s) != replies.end();
  }
  State state(const std::string& name) const {
    for (auto& s : host->status())
      if (s.name == name) return s.state;
    return State::Failed;
  }
};

}  // namespace

TEST_CASE("api: cb.on receives playerJoin, playerLeave, resourceStart and resourceStop") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.on('resourceStart', function() cb.log.info('start') end)
    cb.on('resourceStop', function() cb.log.info('stop') end)
    cb.on('playerJoin', function(p)
      cb.log.info(('join %d %s %s %s %d %d %d %d'):format(p.id, p.name, p.did, p.sub, p.x, p.y, p.z, p.state))
    end)
    cb.on('playerLeave', function(p, why) cb.log.info(('leave %d %s %s %s'):format(p.id, p.name, p.did, why)) end)
    local ok, e = pcall(cb.on, 'xyz', function() end)
    cb.log.info('bad on ' .. tostring(ok) .. ' ' .. e)
    ok, e = pcall(cb.on, 'playerJoin', 42)
    cb.log.info('bad fn ' .. e)
  )");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.state("a") == State::Running);
  REQUIRE(env.waitLog("[a] start"));
  REQUIRE(env.hasLog("bad on false"));
  REQUIRE(env.hasLog("cb.on: evento desconhecido 'xyz'"));
  REQUIRE(env.hasLog("bad fn cb.on: função esperada"));
  env.join(7, "ana", 1, 2, 3);
  REQUIRE(env.waitLog("join 7 ana d7 s7 1 2 3 1"));
  REQUIRE(env.host->post(EvPlayerLeave{7, "d7", "saiu"}));
  REQUIRE(env.waitLog("leave 7 ana d7 saiu"));
  REQUIRE(env.call(env.host->stopResource("a")));
  REQUIRE(env.hasLog("[a] stop"));
}

TEST_CASE("api: client events are decoded, emitted back as JSON and limited to connected players") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('echo', function(p, data) cb.events.emit(p.id, 'echo', data) end)
    cb.events.on('nulls', function(p, data)
      cb.log.info('null? ' .. tostring(data[2] == cb.null))
      cb.events.emit(-1, 'n', {1, cb.null, 3})
    end)
    cb.events.on('big', function(p)
      local ok, e = pcall(cb.events.emit, p.id, 'x', string.rep('a', 5000))
      cb.log.info('big ' .. tostring(ok) .. ' ' .. e)
      ok, e = pcall(cb.events.emit, p.id, 'x', function() end)
      cb.log.info('fnpayload ' .. e)
      ok, e = pcall(cb.events.emit, 'x', 'x', {})
      cb.log.info('badtarget ' .. e)
    end)
    cb.events.on('ghost', function() cb.log.info('ghost ran') end)
    cb.events.on('to999', function() cb.events.emit(999, 'x', {}) cb.log.info('emit999 ok') end)
    local ok, e = pcall(cb.events.on, 'cb:command', function() end)
    cb.log.info('reserved-on ' .. e)
    ok, e = pcall(cb.events.emit, 1, 'cb:reply', {})
    cb.log.info('reserved-emit ' .. e)
    ok, e = pcall(cb.events.on, string.rep('n', 65), function() end)
    cb.log.info('longname ' .. e)
  )");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.hasLog("reserved-on cb.events.on: nomes 'cb:' são reservados"));
  REQUIRE(env.hasLog("reserved-emit cb.events.emit: nomes 'cb:' são reservados"));
  REQUIRE(env.hasLog("longname cb.events.on: nome inválido"));

  env.join(3, "ana");
  env.event(3, "echo", R"([12,"x",1.5,{"k":true}])");
  auto* e = env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.name == "echo"; });
  REQUIRE(e);
  REQUIRE(e->target == 3);
  REQUIRE(e->json == R"([12,"x",1.5,{"k":true}])");

  env.event(3, "nulls", "[1,null,3]");
  auto* n = env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.name == "n"; });
  INFO(env.allLogs());
  REQUIRE(n);
  REQUIRE(n->target == -1);
  REQUIRE(n->json == "[1,null,3]");
  REQUIRE(env.hasLog("null? true"));

  env.event(3, "big");
  REQUIRE(env.waitLog("big false cb.events.emit: payload"));
  REQUIRE(env.hasLog("4096"));
  REQUIRE(env.hasLog("fnpayload cb.events.emit:"));
  REQUIRE(env.hasLog("badtarget cb.events.emit: alvo inválido"));

  env.event(9, "ghost");  // jogador 9 nunca entrou
  env.event(3, "echo", "{malformed");
  env.event(3, "to999");
  REQUIRE(env.waitLog("emit999 ok"));
  env.sync();
  REQUIRE_FALSE(env.hasLog("ghost ran"));
  REQUIRE(env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.target == 999; }));  // ignorado pela simulação
  REQUIRE(env.state("a") == State::Running);

  // Depois de sair, eventos atrasados do jogador não chegam mais aos handlers.
  REQUIRE(env.host->post(EvPlayerLeave{3, "d3", "saiu"}));
  env.event(3, "to999");
  env.sync();
  REQUIRE(env.countLog("emit999 ok") == 1);
}

TEST_CASE("api: players come from the snapshot; kick and setPosition become commands with audit") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('go', function(p)
      local l = cb.players.list()
      cb.log.info('n=' .. #l .. ' first=' .. l[1].name .. ' x=' .. l[1].x .. ' z=' .. l[1].z)
      local g = cb.players.get(2)
      cb.log.info('get2=' .. g.name .. ' state=' .. g.state .. ' did=' .. g.did)
      cb.log.info('get9=' .. tostring(cb.players.get(9)))
      cb.log.info('kick=' .. tostring(cb.players.kick(2, 'tchau')))
      cb.log.info('kick9=' .. tostring(cb.players.kick(9, 'x')))
      cb.log.info('tp=' .. tostring(cb.players.setPosition(1, 100, -200, 300)))
      local ok, e = pcall(cb.players.kick, 'x')
      cb.log.info('badkick ' .. e)
      ok, e = pcall(cb.players.setPosition, 1, 1.5, 0, 0)
      cb.log.info('badtp ' .. e)
      ok, e = pcall(cb.players.get, -1)
      cb.log.info('badget ' .. e)
    end)
  )");
  env.boot();
  INFO(env.allLogs());
  env.join(1, "ana", 10, 20, 30);
  env.join(2, "bia");
  env.host->publishPlayers(std::make_shared<const std::vector<PlayerView>>(std::vector<PlayerView>{
      PlayerView{1, "ana", "d1", "s1", 10, 20, 30, 2}, PlayerView{2, "bia", "d2", "s2", 0, 0, 0, 3}}));
  env.event(1, "go");
  REQUIRE(env.waitLog("n=2 first=ana x=10 z=30"));
  REQUIRE(env.hasLog("get2=bia state=3 did=d2"));
  REQUIRE(env.hasLog("get9=nil"));
  REQUIRE(env.hasLog("kick=true"));
  REQUIRE(env.hasLog("kick9=false"));
  REQUIRE(env.hasLog("tp=true"));
  REQUIRE(env.hasLog("badkick cb.players.kick: id inválido"));
  REQUIRE(env.hasLog("badtp cb.players.setPosition: coordenadas inteiras"));
  REQUIRE(env.hasLog("badget cb.players.get: id inválido"));

  auto* k = env.waitCmd<CmdKick>([](const CmdKick&) { return true; });
  REQUIRE(k);
  REQUIRE(k->id == 2);
  REQUIRE(k->reason == "tchau");
  REQUIRE(k->did == "d2");
  auto* tp = env.waitCmd<CmdSetPosition>([](const CmdSetPosition&) { return true; });
  REQUIRE(tp);
  REQUIRE(tp->id == 1);
  REQUIRE((tp->x == 100 && tp->y == -200 && tp->z == 300));
  REQUIRE(tp->did == "d1");

  auto audits = env.auditLines();
  REQUIRE(audits.size() == 1);
  auto j = nlohmann::json::parse(audits[0]);
  REQUIRE(j["type"] == "kick");
  REQUIRE(j["resource"] == "a");
  REQUIRE(j["did"] == "d2");
  REQUIRE(j["id"] == 2);
  REQUIRE(j["reason"] == "tchau");
  REQUIRE(j["ts"] == 1'700'000'000);
}

TEST_CASE("api: ban persists to bans.json, kicks the connected player and unban removes it") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('ban', function(p)
      cb.log.info('ban=' .. tostring(cb.players.ban('d2', 'cheat', 60)))
      cb.log.info('perm=' .. tostring(cb.players.ban('d9', 'x')))
      cb.log.info('unban=' .. tostring(cb.players.unban('d9')))
      cb.log.info('unban2=' .. tostring(cb.players.unban('d9')))
      local ok, e = pcall(cb.players.ban, '', 'x')
      cb.log.info('badban ' .. e)
      ok, e = pcall(cb.players.ban, 'd5', 'x', -3)
      cb.log.info('baddur ' .. e)
    end)
  )");
  env.boot();
  INFO(env.allLogs());
  env.join(1, "ana");
  env.join(2, "bia");
  env.event(1, "ban");
  REQUIRE(env.waitLog("unban2=false"));
  REQUIRE(env.hasLog("ban=true"));
  REQUIRE(env.hasLog("perm=true"));
  REQUIRE(env.hasLog("unban=true"));
  REQUIRE(env.hasLog("badban cb.players.ban: did inválido"));
  REQUIRE(env.hasLog("baddur cb.players.ban: duração inválida"));

  auto* k = env.waitCmd<CmdKick>([](const CmdKick&) { return true; });
  REQUIRE(k);
  REQUIRE(k->id == 2);
  REQUIRE(k->did == "d2");
  REQUIRE(contains(k->reason, "cheat"));

  const int64_t now = env.unix.load();
  auto b = env.bans->check("d2", now);
  REQUIRE(b);
  REQUIRE(b->reason == "cheat");
  REQUIRE(b->by == "resource:a");
  REQUIRE(b->expiresAt == now + 60);
  REQUIRE_FALSE(env.bans->check("d2", now + 61));
  REQUIRE_FALSE(env.bans->check("d9", now));

  BanStore reloaded(env.dir.p / "bans.json");
  REQUIRE(reloaded.check("d2", now));
  REQUIRE_FALSE(reloaded.check("d9", now));

  auto audits = env.auditLines();
  bool sawBan = false;
  for (auto& a : audits) sawBan = sawBan || nlohmann::json::parse(a)["type"] == "ban";
  REQUIRE(sawBan);
}

TEST_CASE("bans: file round trip, expiry and malformed files") {
  TmpDir t;
  {
    BanStore s(t.p / "bans.json");
    std::string err;
    REQUIRE(s.add(Ban{"a", "r1", "admin:x", 100, 0}, err));
    REQUIRE(s.add(Ban{"b", "r2", "resource:y", 100, 200}, err));
    REQUIRE(s.check("a", 1'000'000));
    REQUIRE(s.check("b", 199));
    REQUIRE_FALSE(s.check("b", 200));
    bool removed = false;
    REQUIRE(s.remove("a", removed, err));
    REQUIRE(removed);
    REQUIRE(s.remove("a", removed, err));
    REQUIRE_FALSE(removed);
  }
  BanStore again(t.p / "bans.json");
  REQUIRE_FALSE(again.check("a", 150));
  auto b = again.check("b", 150);
  REQUIRE(b);
  REQUIRE(b->reason == "r2");
  REQUIRE(b->by == "resource:y");
  REQUIRE(again.list().size() == 1);

  put(t.p / "bad.json", "{nope");
  REQUIRE_THROWS(BanStore(t.p / "bad.json"));
  put(t.p / "bad2.json", R"([{"did": 5}])");
  REQUIRE_THROWS(BanStore(t.p / "bad2.json"));
  BanStore mem{std::filesystem::path{}};
  std::string err;
  REQUIRE(mem.add(Ban{"z", "r", "x", 1, 0}, err));
  REQUIRE(mem.check("z", 2));
}

TEST_CASE("api: commands from console and chat, admin flag and replies") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.commands.register('hello', function(src, args, raw)
      return 'oi ' .. src .. ' ' .. #args .. ' ' .. (args[1] or '-') .. ' [' .. raw .. ']'
    end)
    cb.commands.register('secret', function(src) return 'segredo' end, {admin = true})
    cb.commands.register('quiet', function() cb.log.info('quiet ran') end)
    cb.commands.register('fail', function() error('falhou feio') end)
    local ok, e = pcall(cb.commands.register, 'bad name', function() end)
    cb.log.info('badcmd ' .. e)
  )");
  env.addRes("b", R"(
    local ok, e = pcall(cb.commands.register, 'hello', function() end)
    cb.log.info('dup ' .. e)
  )", {"a"});
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.hasLog("badcmd cb.commands.register: nome inválido"));
  REQUIRE(env.hasLog("dup cb.commands.register: comando 'hello' já registrado por 'a'"));

  env.command(std::nullopt, "hello a  b");
  REQUIRE(waitUntil([&] { return env.hasReply("oi 0 2 a [hello a  b]"); }));
  env.command(std::nullopt, "secret");
  REQUIRE(waitUntil([&] { return env.hasReply("segredo"); }));
  env.command(std::nullopt, "nope x");
  REQUIRE(waitUntil([&] { return env.hasReply("comando desconhecido: nope"); }));
  env.command(std::nullopt, "quiet");
  REQUIRE(env.waitLog("quiet ran"));
  env.command(std::nullopt, "fail");
  REQUIRE(waitUntil([&] { return env.hasReply("erro ao executar 'fail' (ver log)"); }));
  REQUIRE(env.hasLog("falhou feio", LogLevel::Error));

  // Chat: resposta volta como evento cb:reply para o jogador; admin=true recusado sem isAdmin.
  env.join(3, "ana");
  env.command(3, "/hello x", false);
  auto* r = env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.name == "cb:reply" && contains(c.json, "oi 3"); });
  REQUIRE(r);
  REQUIRE(r->target == 3);
  REQUIRE(r->json == R"("oi 3 1 x [hello x]")");
  env.command(3, "/secret", false);
  REQUIRE(env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.name == "cb:reply" && contains(c.json, "permiss"); }));
  env.command(4, "/hello", false);  // jogador 4 não está conectado: ignorado
  env.sync();
  env.drain();
  for (auto& c : env.cmds)
    if (auto* e = std::get_if<CmdEmit>(&c)) REQUIRE(e->target != 4);

  // Admin pelo painel fica auditado com o did.
  bool audited = false;
  for (auto& a : env.auditLines()) {
    auto j = nlohmann::json::parse(a);
    audited = audited || (j["type"] == "command" && j["by"] == "admin-did" && j["line"] == "secret");
  }
  REQUIRE(audited);

  // Parar o resource tira os comandos dele.
  REQUIRE(env.call(env.host->stopResource("a")));
  env.command(std::nullopt, "hello");
  REQUIRE(waitUntil([&] { return env.hasReply("comando desconhecido: hello"); }));
}

TEST_CASE("api: chat admin commands are allowed for admins") {
  ApiEnv env;
  env.api.isAdmin = [](const std::string& did) { return did == "d3"; };
  env.addRes("a", "cb.commands.register('secret', function(src) return 'segredo ' .. src end, {admin = true})");
  env.boot();
  env.join(3, "ana");
  env.join(4, "bia");
  env.command(3, "secret", false);
  env.command(4, "secret", false);
  REQUIRE(env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.target == 3 && c.json == R"("segredo 3")"; }));
  REQUIRE(env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.target == 4 && contains(c.json, "permiss"); }));
}

TEST_CASE("api: timers fire with the injected clock, can be cancelled and disappear on stop") {
  ApiEnv env;
  env.api.maxTimersPerResource = 5;
  env.addRes("a", R"(
    local n = 0
    cb.timers.every(100, function() n = n + 1; cb.log.info('tick ' .. n) end)
    cb.timers.after(250, function() cb.log.info('after fired') end)
    local c = cb.timers.after(50, function() cb.log.info('cancelled ran') end)
    cb.log.info('cancel=' .. tostring(cb.timers.cancel(c)) .. ' again=' .. tostring(cb.timers.cancel(c)))
    local ok, e = pcall(cb.timers.every, 5, function() end)
    cb.log.info('min ' .. e)
    ok, e = pcall(cb.timers.after, 10, 'x')
    cb.log.info('badfn ' .. e)
  )");
  env.addRes("lots", R"(
    for i = 1, 5 do cb.timers.after(1000, function() end) end
    local ok, e = pcall(cb.timers.after, 1000, function() end)
    cb.log.info('limit ' .. e)
  )");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.hasLog("cancel=true again=false"));
  REQUIRE(env.hasLog("min cb.timers.every: intervalo mínimo de 10 ms"));
  REQUIRE(env.hasLog("badfn cb.timers.after: função esperada"));
  REQUIRE(env.hasLog("limit cb.timers.after: limite de 5 timers"));
  env.sync();
  REQUIRE(env.countLog("tick ") == 0);

  env.clock->advance(0.1);
  REQUIRE(env.waitLog("tick 1"));
  env.clock->advance(0.1);
  REQUIRE(env.waitLog("tick 2"));
  env.sync();
  REQUIRE_FALSE(env.hasLog("after fired"));
  env.clock->advance(0.06);
  REQUIRE(env.waitLog("after fired"));
  env.sync();
  REQUIRE(env.countLog("tick ") == 2);
  REQUIRE_FALSE(env.hasLog("cancelled ran"));

  REQUIRE(env.call(env.host->stopResource("a")));
  env.clock->advance(5.0);
  env.sync();
  env.sync();
  REQUIRE(env.countLog("tick ") == 2);
  REQUIRE(env.countLog("after fired") == 1);
}

TEST_CASE("api: local events reach every resource after the emitting callback returns") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.onLocal('ping', function(data, from) cb.log.info('a got ' .. data.n .. ' from ' .. from) end)
  )");
  env.addRes("b", R"(
    cb.events.onLocal('ping', function(data, from)
      cb.log.info('b got ' .. data.n .. ' from ' .. from)
      data.n = 99  -- cópia: não afeta os outros
    end)
    cb.events.on('go', function()
      local t = {n = 5}
      cb.events.emitLocal('ping', t)
      cb.log.info('emitted')
    end)
    local ok, e = pcall(cb.events.emitLocal, 'cb:x', {})
    cb.log.info('reserved ' .. e)
  )");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.hasLog("reserved cb.events.emitLocal: nomes 'cb:' são reservados"));
  env.join(1, "ana");
  env.event(1, "go");
  REQUIRE(env.waitLog("a got 5 from b"));
  REQUIRE(env.waitLog("b got 5 from b"));
  REQUIRE(env.logIndex("emitted") < env.logIndex("a got 5"));
  REQUIRE(env.logIndex("a got 5") < env.logIndex("b got 5"));  // ordem de subida
}

TEST_CASE("api: cb.resource calls exports of another resource with copied values") {
  ApiEnv env;
  env.addRes("lib", R"(
    return {
      add = function(a, b) return a + b end,
      touch = function(t) t.x = 1; return t end,
      version = '1.2',
      boom = function() error('lib quebrou') end,
    }
  )");
  env.addRes("app", R"(
    cb.on('resourceStart', function()
      local lib = cb.resource('lib')
      cb.log.info('add=' .. lib.add(2, 3))
      local t = {y = 2}
      local r = lib.touch(t)
      cb.log.info('copy ' .. tostring(t.x) .. ' ' .. r.x .. ' ' .. r.y)
      cb.log.info('ver=' .. lib.version .. ' missing=' .. tostring(lib.nothing))
      local ok, e = pcall(lib.boom)
      cb.log.info('boom ' .. tostring(ok) .. ' ' .. e)
      ok, e = pcall(cb.resource, 'nope')
      cb.log.info('nope ' .. e)
      ok, e = pcall(cb.resource, 'app')
      cb.log.info('self ' .. e)
      ok, e = pcall(lib.add, function() end)
      cb.log.info('badarg ' .. e)
    end)
  )", {"lib"});
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.waitLog("add=5"));
  REQUIRE(env.hasLog("copy nil 1 2"));
  REQUIRE(env.hasLog("ver=1.2 missing=nil"));
  REQUIRE(env.hasLog("boom false cb.resource: erro em 'lib.boom'"));
  REQUIRE(env.hasLog("lib quebrou", LogLevel::Error));
  REQUIRE(env.hasLog("nope cb.resource: resource 'nope' não está rodando"));
  REQUIRE(env.hasLog("self cb.resource: um resource não pode chamar a si mesmo"));
  REQUIRE(env.hasLog("badarg cb.resource:"));
  REQUIRE(env.state("lib") == State::Running);
  REQUIRE(env.state("app") == State::Running);
}

TEST_CASE("api: cb.config is a read-only copy of the manifest config; cb.null and cb.log levels") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.log.info('cfg ' .. cb.config.name .. ' ' .. cb.config.list[2] .. ' ' .. #cb.config.list .. ' ' .. cb.config.nested.k)
    local ok, e = pcall(function() cb.config.name = 'x' end)
    cb.log.info('ro ' .. tostring(ok) .. ' ' .. e)
    ok, e = pcall(function() cb.config.nested.k = 'x' end)
    cb.log.info('ro2 ' .. tostring(ok) .. ' ' .. e)
    local keys = 0
    for k in pairs(cb.config) do keys = keys + 1 end
    cb.log.info('keys ' .. keys .. ' null ' .. type(cb.null) .. ' ' .. tostring(cb.null == cb.null))
    cb.log.warn('aviso', 1, true)
    cb.log.error('erro grave')
  )", {}, nlohmann::json{{"name", "abc"}, {"list", {1, 2, 3}}, {"nested", {{"k", "v"}}}});
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.state("a") == State::Running);
  REQUIRE(env.hasLog("[a] cfg abc 2 3 v"));
  REQUIRE(env.hasLog("ro false"));
  REQUIRE(env.hasLog("ro2 false"));
  REQUIRE(env.countLog("cb.config é somente leitura") == 2);
  REQUIRE(env.hasLog("keys 3 null userdata true"));
  REQUIRE(env.hasLog("[a] aviso 1 true", LogLevel::Warn));
  REQUIRE(env.hasLog("[a] erro grave", LogLevel::Error));
}

TEST_CASE("api: callback errors are logged with traceback and the resource keeps running") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('boom', function() error('kaboom') end)
    cb.events.on('ok', function() cb.log.info('still alive') end)
    cb.events.on('ok', function() cb.log.info('second handler') end)
    cb.events.on('audit', function(p)
      cb.audit('teste', {id = p.id, x = 1})
      cb.audit('semjogador', {foo = 'bar'})
      local ok, e = pcall(cb.audit, 'x', function() end)
      cb.log.info('badaudit ' .. e)
    end)
  )");
  env.boot();
  env.join(3, "ana");
  env.event(3, "boom");
  REQUIRE(env.waitLog("kaboom", LogLevel::Error));
  REQUIRE(env.hasLog("stack traceback", LogLevel::Error));
  env.event(3, "ok");
  REQUIRE(env.waitLog("still alive"));
  REQUIRE(env.waitLog("second handler"));
  env.event(3, "audit");
  REQUIRE(env.waitLog("badaudit cb.audit:"));
  INFO(env.allLogs());
  auto audits = env.auditLines();
  REQUIRE(audits.size() == 2);
  auto a = nlohmann::json::parse(audits[0]);
  REQUIRE(a["type"] == "teste");
  REQUIRE(a["resource"] == "a");
  REQUIRE(a["did"] == "d3");
  REQUIRE(a["data"]["x"] == 1);
  auto b = nlohmann::json::parse(audits[1]);
  REQUIRE(b["type"] == "semjogador");
  REQUIRE_FALSE(b.contains("did"));
  REQUIRE(env.state("a") == State::Running);
}

TEST_CASE("api: handlers and commands registered by a restarted resource are not duplicated") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('ping', function() cb.log.info('pong') end)
    cb.commands.register('c', function() return 'ok' end)
  )");
  env.boot();
  REQUIRE(env.call(env.host->restartResource("a")));
  env.join(1, "ana");
  env.event(1, "ping");
  REQUIRE(env.waitLog("pong"));
  env.command(std::nullopt, "c");
  REQUIRE(waitUntil([&] { return env.hasReply("ok"); }));
  env.sync();
  REQUIRE(env.countLog("pong") == 1);
}
