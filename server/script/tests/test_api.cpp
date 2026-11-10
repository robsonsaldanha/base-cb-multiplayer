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
#include "api_env.hpp"
#include "test_util.hpp"

using namespace cbscript;
using State = ResourceStatus::State;


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
  env.leave(7);
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
  env.drain();
  for (auto& c : env.cmds)  // id não conectado: emit devolve false e nada vai para a simulação
    if (auto* em = std::get_if<CmdEmit>(&c)) REQUIRE(em->target != 999);
  REQUIRE(env.state("a") == State::Running);

  // Depois de sair, eventos atrasados do jogador não chegam mais aos handlers.
  env.leave(3);
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
      PlayerView{1, "ana", "d1", "s1", 10, 20, 30, 2, 1001}, PlayerView{2, "bia", "d2", "s2", 0, 0, 0, 3, 1002}}));
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
  REQUIRE(k->session == 1002);
  auto* tp = env.waitCmd<CmdSetPosition>([](const CmdSetPosition&) { return true; });
  REQUIRE(tp);
  REQUIRE(tp->id == 1);
  REQUIRE((tp->x == 100 && tp->y == -200 && tp->z == 300));
  REQUIRE(tp->session == 1001);

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

  auto* k = env.waitCmd<CmdKickDid>([](const CmdKickDid&) { return true; });  // resolvido na simulação
  REQUIRE(k);
  REQUIRE(k->did == "d2");
  REQUIRE(contains(k->reason, "cheat"));

  const int64_t now = env.unixTime.load();
  auto b = env.bans->check("d2", now);
  REQUIRE(b);
  REQUIRE(b->reason == "cheat");
  REQUIRE(b->by == "resource:a");
  REQUIRE(b->expiresAt == now + 60);
  REQUIRE_FALSE(env.bans->check("d2", now + 61));
  REQUIRE_FALSE(env.bans->check("d9", now));

  BanStore reloaded(env.dir.p / "bans.json", [&env] { return env.unixTime.load(); });
  REQUIRE(reloaded.check("d2", now));
  REQUIRE_FALSE(reloaded.check("d9", now));

  auto audits = env.auditLines();
  bool sawBan = false;
  for (auto& a : audits) sawBan = sawBan || nlohmann::json::parse(a)["type"] == "ban";
  REQUIRE(sawBan);
}

TEST_CASE("bans: file round trip, expiry and malformed files") {
  TmpDir t;
  auto clock = [] { return int64_t{150}; };
  {
    BanStore s(t.p / "bans.json", clock);
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
  BanStore again(t.p / "bans.json", clock);
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

// ============================================== regressões dos ataques do revisor (Task 4, fix round 1)

TEST_CASE("api: pairs over cb.config does not leak the original table") {
  ApiEnv env;
  env.addRes("a", R"(
    local _, orig = pairs(cb.config)
    local ok = pcall(function() orig.x = 'hacked' end)
    cb.log.info('cfg ' .. tostring(ok) .. ' ' .. tostring(cb.config.x))
  )", {}, {{"x", "orig"}});
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.waitLog("cfg false orig"));
}

TEST_CASE("api: hostile arguments only raise Lua errors") {
  ApiEnv env;
  env.addRes("b", R"( return { v = 1 } )");
  env.addRes("a", R"(
    local mt = {__index=function() error('idx') end, __len=function() error('len') end,
                __tostring=function() error('ts') end, __eq=function() return true end,
                __pairs=function() error('pairs') end, __name='Hostil'}
    local hostile = setmetatable({}, mt)
    local function try(name, f, ...)
      local ok, e = pcall(f, ...)
      cb.log.info('T ' .. name .. ' ' .. tostring(ok) .. ' ' .. tostring(e))
    end
    try('emit-nan', cb.events.emit, 0/0, 'x', {})
    try('emit-huge', cb.events.emit, 2^63, 'x', {})
    try('emit-minint', cb.events.emit, math.mininteger, 'x', {})
    try('emit-hostile', cb.events.emit, -1, 'x', hostile)
    try('emit-hostile-name', cb.events.emit, -1, hostile, {})
    try('emit-nul', cb.events.emit, -1, 'a\0b', {})
    try('emit-nested-hostile', cb.events.emit, -1, 'x', {a = {b = hostile}})
    try('kick-nan', cb.players.kick, 0/0)
    try('kick-reason-tbl', cb.players.kick, 1, hostile)
    try('setpos-inf', cb.players.setPosition, 1, math.huge, 0, 0)
    try('setpos-big', cb.players.setPosition, 1, 2^31, 0, 0)
    try('setpos-str', cb.players.setPosition, 1, '1', 0, 0)
    try('after-inf', cb.timers.after, math.huge, function() end)
    try('every-nan', cb.timers.every, 0/0, function() end)
    try('every-neg0', cb.timers.every, -0.0, function() end)
    try('cancel-huge', cb.timers.cancel, math.maxinteger)
    try('cancel-neg', cb.timers.cancel, -1)
    try('ban-hostile', cb.players.ban, hostile)
    try('ban-dur-huge', cb.players.ban, 'd', 'r', math.maxinteger)
    try('ban-dur-float', cb.players.ban, 'd', 'r', 1.5)
    try('audit-hostile', cb.audit, 't', hostile)
    try('audit-id-hostile', cb.audit, 't', setmetatable({id = 1}, mt))
    try('log-hostile', cb.log.info, hostile)
    try('log-tostring-nonstr', cb.log.info, setmetatable({}, {__tostring = function() return {} end}))
    try('log-name', cb.log.info, setmetatable({}, {__name = 'zz'}))
    try('register-hostile-opts', cb.commands.register, 'c1', function() end, hostile)
    try('resource-hostile', cb.resource, hostile)
    try('resource-nul', cb.resource, 'b\0x')
    try('config-newindex', function() cb.config.x = 1 end)
    try('get-nan', cb.players.get, 0/0)
    try('on-nul', cb.on, 'playerJoin\0zzz', function() end)
    try('null-index', function() return cb.null.x end)
    local deep = {} local c = deep for i = 1, 100 do c[1] = {} c = c[1] end
    try('emitLocal-deep', cb.events.emitLocal, 'x', deep)
    local rec; rec = setmetatable({}, {__tostring = function() cb.log.info(rec) return 'x' end})
    try('log-recursive', cb.log.info, rec)
    local yielder = setmetatable({}, {__tostring = function() coroutine.yield() return 'y' end})
    try('log-yield', cb.log.info, yielder)
    cb.timers.after(0, function()
      local p = cb.resource('b')
      try('proxy-hostile-key', function() return p[hostile] end)
      try('proxy-set', function() p.z = 1 end)
      try('proxy-v', function() return p.v end)
      cb.log.info('ALIVE')
    end)
  )", {"b"});
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.waitLog("ALIVE"));
  CHECK(env.state("a") == State::Running);
}

TEST_CASE("api: bindings at the memory cap only raise Lua errors") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.timers.after(0, function()
      local hog = {}
      pcall(function() while true do hog[#hog + 1] = string.rep('x', 65536) .. #hog end end)
      local fails = 0
      for i = 1, 150 do  -- 150: cb.log.info abaixo do limite de 200 linhas/s (o MEMOK tem de passar)
        if not pcall(cb.players.list) then fails = fails + 1 end
        if not pcall(cb.events.emit, -1, 'x', {1, 2, 3}) then fails = fails + 1 end
        if not pcall(cb.log.info, 'x', i) then fails = fails + 1 end
        if not pcall(cb.timers.after, 1000000, function() end) then fails = fails + 1 end
        if not pcall(cb.events.on, 'n' .. i, function() end) then fails = fails + 1 end
        if not pcall(cb.audit, 't', {a = i}) then fails = fails + 1 end
        if not pcall(cb.events.emitLocal, 'l', {i}) then fails = fails + 1 end
        if not pcall(cb.commands.register, 'c' .. i, function() end) then fails = fails + 1 end
        if not pcall(function() return cb.config.k end) then fails = fails + 1 end
      end
      hog = nil
      for i = 1, 50 do collectgarbage('step') end
      cb.log.info('MEMOK ' .. fails)
    end)
  )", {}, {{"k", "v"}});
  // Teto pequeno e prazo folgado: encher o teto com o GC de emergência leva mais que os 200 ms padrão dos testes.
  env.lim = shortLimits(size_t{16} << 20);
  env.lim.abortAfter = std::chrono::milliseconds(5000);
  env.boot();
  env.join(1, "ana");
  const bool ok = env.waitLog("MEMOK", LogLevel::Info);
  INFO(env.allLogs());
  REQUIRE(ok);
  REQUIRE(env.state("a") == State::Running);
}

// Comportamento atual (documentado para a Task 9): a chamada a cb.resource é síncrona, então o prazo da vigia do
// chamador continua correndo enquanto o alvo roda. Um alvo preso é abortado, mas o chamador também passa do prazo e
// leva uma falta (os dois reiniciam com back-off). O host não para e os dois voltam a rodar.
TEST_CASE("api: a cb.resource target that spins forever is aborted; caller and target restart") {
  ApiEnv env;
  env.addRes("b", R"( return { spin = function() while true do end end } )");
  env.addRes("a", R"(
    cb.on('resourceStart', function()
      cb.timers.after(0, function()
        local ok, e = pcall(function() return cb.resource('b').spin() end)
        cb.log.info('SPIN ' .. tostring(ok) .. ' ' .. tostring(e))
      end)
    end)
  )", {"b"});
  env.boot();
  const bool both = waitUntil([&] {
    return env.hasLog("[b] abortado por tempo", LogLevel::Warn) || env.countLog("abortado por tempo") >= 2;
  });
  INFO(env.allLogs());
  REQUIRE(both);
  REQUIRE(env.hasLog("[b] tempo esgotado", LogLevel::Error));
  REQUIRE_FALSE(env.host->metrics().halted);
  env.clock->advance(5.0);  // back-off
  REQUIRE(waitUntil([&] { return env.state("a") == State::Running && env.state("b") == State::Running; }));
}

TEST_CASE("api: cb.log and print are rate limited and cannot forge log lines") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.log.info('line1\n[2026-11-10 00:00:00.000] [info] [admin] forged\r\0x')
    print('p1\np2')
    for i = 1, 20000 do cb.log.info('flood') end
    for i = 1, 20000 do print('pflood') end
    cb.timers.after(1100, function() cb.log.info('after window') end)
  )");
  env.boot();
  INFO("cb.log lines = " << env.countLog("] flood"));
  REQUIRE(env.countLog("] flood") <= 200);
  REQUIRE(env.countLog("pflood") <= 200);
  REQUIRE_FALSE(env.hasLog("\n[2026-11-10"));
  REQUIRE(env.hasLog("line1\\n[2026-11-10 00:00:00.000] [info] [admin] forged\\r\\x00x"));
  REQUIRE(env.countLog("p1\\np2") == 1);
  // Janela seguinte: resumo do que foi descartado e a linha nova.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  env.clock->advance(1.2);
  REQUIRE(env.waitLog("after window"));
  REQUIRE(env.hasLog("linhas de cb.log descartadas", LogLevel::Warn));
}

TEST_CASE("api: callback error logs and audit lines are rate limited per second") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('boom', function() error('kaboom') end)
    cb.events.on('audit', function() for i = 1, 1000 do cb.audit('flood', {i = i}) end cb.log.info('audited') end)
  )");
  env.api.stats = std::make_shared<ApiStats>();
  env.boot();
  env.join(3, "ana");
  for (int i = 0; i < 500; ++i) env.event(3, "boom");
  env.event(3, "audit");
  REQUIRE(env.waitLog("audited"));
  env.sync();
  INFO("kaboom lines = " << env.countLog("kaboom"));
  REQUIRE(env.countLog("kaboom") <= 200);
  REQUIRE(env.auditLines().size() <= 200);
  REQUIRE(env.api.stats->auditDropped.load() >= 800);
}

TEST_CASE("api: chat command replies always fit in an event and admin chat commands are audited") {
  ApiEnv env;
  env.api.isAdmin = [](const std::string& did) { return did == "d3"; };
  env.addRes("a", R"(
    cb.commands.register('big', function() return string.rep('x', 5000) end)
    cb.commands.register('esc', function() return string.rep('"', 3000) end)
    cb.commands.register('adm', function() return 'ok' end, {admin = true})
  )");
  env.boot();
  env.join(3, "ana");
  env.command(3, std::string(4094, 'z'), false);
  env.command(3, "big", false);
  env.command(3, "esc", false);
  env.command(3, "adm", false);
  auto* unknown = env.waitCmd<CmdEmit>([](const CmdEmit& c) { return contains(c.json, "desconhecido"); });
  auto* big = env.waitCmd<CmdEmit>([](const CmdEmit& c) { return contains(c.json, "xxxx"); });
  auto* esc = env.waitCmd<CmdEmit>([](const CmdEmit& c) { return contains(c.json, "\\\""); });
  REQUIRE(unknown);
  REQUIRE(big);
  REQUIRE(esc);
  REQUIRE(unknown->json.size() < 100);  // nome desconhecido ecoado com no máximo 32 bytes
  REQUIRE(big->json.size() <= 4096);
  REQUIRE(esc->json.size() <= 4096);
  REQUIRE(contains(big->json, "\u2026"));
  REQUIRE(nlohmann::json::parse(esc->json).is_string());
  REQUIRE(unknown->session == ApiEnv::sessionOf(3));
  REQUIRE(env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.json == R"("ok")"; }));
  bool audited = false;
  for (auto& a : env.auditLines()) {
    auto j = nlohmann::json::parse(a);
    audited = audited || (j["type"] == "command" && j["by"] == "d3" && j["line"] == "adm" && j["id"] == 3);
  }
  REQUIRE(audited);
}

// Regressão do CI Linux/ASan: o teste acima guardava ponteiros de waitCmd e, nas esperas seguintes, drain() acrescentava
// comandos; com `cmds` num vector a realocação liberava a memória apontada (heap-use-after-free). Aqui a realocação é
// forçada (centenas de comandos depois do primeiro ponteiro), e o endereço e o conteúdo precisam continuar os mesmos.
TEST_CASE("api test env: commands returned by waitCmd stay valid while more commands arrive") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('burst', function(p, data)
      for i = 1, data.n do cb.events.emit(p.id, data.tag .. i, {i = i}) end
      cb.log.info('burst ' .. data.tag)
    end)
  )");
  env.boot();
  env.join(3, "ana");
  env.event(3, "burst", R"({"n":1,"tag":"first"})");
  const CmdEmit* first = env.waitCmd<CmdEmit>([](const CmdEmit& c) { return c.name == "first1"; });
  REQUIRE(first);
  size_t idx = env.cmds.size();
  for (size_t i = 0; i < env.cmds.size(); ++i)
    if (std::get_if<CmdEmit>(&env.cmds[i]) == first) idx = i;
  REQUIRE(idx < env.cmds.size());
  const std::string firstJson = first->json;
  for (int k = 0; k < 4; ++k) {  // cada rodada: 200 comandos novos, drenados aos poucos pelas esperas
    const std::string tag = "r" + std::to_string(k) + "_";
    env.event(3, "burst", R"({"n":200,"tag":")" + tag + R"("})");
    REQUIRE(env.waitCmd<CmdEmit>([&](const CmdEmit& c) { return c.name == tag + "200"; }));
  }
  REQUIRE(env.cmds.size() >= idx + 801);
  REQUIRE(std::get_if<CmdEmit>(&env.cmds[idx]) == first);  // num vector, a realocação mudaria o endereço
  REQUIRE(first->name == "first1");
  REQUIRE(first->json == firstJson);
  REQUIRE(first->target == 3);
}

TEST_CASE("api: events, leaves and players from another session of the same id are ignored") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.on('playerLeave', function(p, why) cb.log.info('leave ' .. p.id .. ' ' .. why) end)
    cb.events.on('who', function(p)
      local me = cb.players.get(p.id)
      cb.log.info('who ' .. p.name .. ' x=' .. p.x .. ' get=' .. tostring(me and me.name) .. ' n=' .. #cb.players.list())
    end)
  )");
  env.boot();
  env.join(3, "ana");
  // Foto ainda da sessão velha (id 3 era de outra pessoa, sessão 77).
  env.host->publishPlayers(std::make_shared<const std::vector<PlayerView>>(
      std::vector<PlayerView>{PlayerView{3, "velho", "dv", "sv", 500, 0, 0, 1, 77}}));
  REQUIRE(env.host->post(EvClientEvent{3, "who", "{}", 77}));          // sessão velha: ignorado
  REQUIRE(env.host->post(EvPlayerLeave{3, "dv", "velho saiu", 77}));   // idem
  env.event(3, "who");
  REQUIRE(env.waitLog("who ana x=0 get=nil n=0"));
  REQUIRE_FALSE(env.hasLog("leave 3 velho saiu"));
  env.host->publishPlayers(std::make_shared<const std::vector<PlayerView>>(
      std::vector<PlayerView>{PlayerView{3, "ana", "d3", "s3", 40, 0, 0, 1, ApiEnv::sessionOf(3)}}));
  env.event(3, "who");
  REQUIRE(env.waitLog("who ana x=40 get=ana n=1"));
}

TEST_CASE("api: a join that reaches the scripts after a ban is kicked and not dispatched") {
  ApiEnv env;
  env.addRes("a", "cb.on('playerJoin', function(p) cb.log.info('join ' .. p.id) end)");
  env.boot();
  std::string err;
  REQUIRE(env.bans->add(Ban{"d5", "trapaça", "admin:x", env.unixTime.load(), 0}, err));
  env.join(5, "eva");
  env.join(6, "ivo");
  REQUIRE(env.waitLog("join 6"));
  REQUIRE_FALSE(env.hasLog("join 5"));
  auto* k = env.waitCmd<CmdKick>([](const CmdKick& c) { return c.id == 5; });
  REQUIRE(k);
  REQUIRE(k->session == ApiEnv::sessionOf(5));
  REQUIRE(contains(k->reason, "trapaça"));
}

TEST_CASE("api: NUL is rejected in event, command, cb.on and resource names") {
  ApiEnv env;
  env.addRes("a", R"(
    local function e(f, ...) local ok, m = pcall(f, ...) return tostring(ok) end
    cb.log.info('nul ' .. e(cb.events.on, 'a\0b', print) .. e(cb.events.emit, -1, 'a\0b', {}) ..
      e(cb.events.onLocal, 'a\0b', print) .. e(cb.commands.register, 'a\0b', print) ..
      e(cb.on, 'playerJoin\0x', print) .. e(cb.resource, 'a\0b'))
  )");
  env.boot();
  INFO(env.allLogs());
  REQUIRE(env.hasLog("nul falsefalsefalsefalsefalsefalse"));
}

TEST_CASE("bans: expired bans are purged on load and save; out-of-range values are rejected") {
  TmpDir t;
  int64_t now = 1000;
  auto clk = [&now] { return now; };
  {
    BanStore s(t.p / "b.json", clk);
    std::string err;
    REQUIRE(s.add(Ban{"x", "r", "by", 900, 1500}, err));
    REQUIRE(s.add(Ban{"y", "r", "by", 900, 0}, err));
  }
  now = 2000;
  BanStore again(t.p / "b.json", clk);
  REQUIRE(again.list().size() == 1);
  std::string err;
  REQUIRE(again.add(Ban{"z", "r", "by", 2000, 0}, err));
  std::ifstream f(t.p / "b.json");
  std::stringstream ss;
  ss << f.rdbuf();
  REQUIRE_FALSE(contains(ss.str(), "\"x\""));
  REQUIRE(contains(ss.str(), "\"z\""));
  put(t.p / "neg.json", R"([{"did": "a", "expiresAt": -1}])");
  REQUIRE_THROWS(BanStore(t.p / "neg.json", clk));
  put(t.p / "huge.json", R"([{"did": "a", "createdAt": 1000000000000000}])");
  REQUIRE_THROWS(BanStore(t.p / "huge.json", clk));
}

// ============================================== regressões (Task 4, fix round 2)

TEST_CASE("api: cb.audit is limited per resource and engine audit lines are never limited") {
  ApiEnv env;
  env.api.stats = std::make_shared<ApiStats>();
  env.addRes("a", R"(
    cb.events.on('flood', function() for i = 1, 1000 do cb.audit('flood', {i = i}) end cb.log.info('flooded') end)
  )");
  env.addRes("b", R"(
    cb.events.on('both', function()
      cb.audit('b-own', {x = 1})
      for i = 1, 300 do cb.players.kick(3, 'k' .. i) end
      cb.log.info('kicked')
    end)
  )");
  env.boot();
  env.join(3, "ana");
  env.event(3, "flood");
  REQUIRE(env.waitLog("flooded"));
  env.event(3, "both");
  REQUIRE(env.waitLog("kicked"));
  int own = 0, kicks = 0, floods = 0;
  for (auto& l : env.auditLines()) {
    auto j = nlohmann::json::parse(l);
    own += j["type"] == "b-own" ? 1 : 0;
    kicks += j["type"] == "kick" ? 1 : 0;
    floods += j["type"] == "flood" ? 1 : 0;
  }
  REQUIRE(floods == 200);  // a: limitado
  REQUIRE(own == 1);       // b: não sofre com o limite de a
  REQUIRE(kicks == 300);   // linhas do motor: nunca limitadas
  REQUIRE(env.api.stats->auditDropped.load() == 800);
}

TEST_CASE("api: player tables expose the session and emit/kick/setPosition accept an optional session") {
  ApiEnv env;
  env.addRes("a", R"(
    cb.events.on('go', function(p)
      cb.log.info('sess ' .. p.session .. ' ' .. cb.players.get(p.id).session)
      cb.log.info('emit-old ' .. tostring(cb.events.emit(p.id, 'x', {}, p.session + 1)))
      cb.log.info('emit-cur ' .. tostring(cb.events.emit(p.id, 'x', {}, p.session)))
      cb.log.info('tp-old ' .. tostring(cb.players.setPosition(p.id, 1, 2, 3, 99)))
      cb.log.info('kick-old ' .. tostring(cb.players.kick(p.id, 'x', 99)))
      cb.log.info('kick-cur ' .. tostring(cb.players.kick(p.id, 'x', p.session)))
      local ok, e = pcall(cb.players.kick, p.id, 'x', -1)
      cb.log.info('bad ' .. e)
    end)
  )");
  env.boot();
  env.join(3, "ana");
  env.host->publishPlayers(std::make_shared<const std::vector<PlayerView>>(
      std::vector<PlayerView>{PlayerView{3, "ana", "d3", "s3", 0, 0, 0, 1, ApiEnv::sessionOf(3)}}));
  env.event(3, "go");
  REQUIRE(env.waitLog("bad "));
  INFO(env.allLogs());
  REQUIRE(env.hasLog("sess 1003 1003"));
  REQUIRE(env.hasLog("emit-old false"));
  REQUIRE(env.hasLog("emit-cur true"));
  REQUIRE(env.hasLog("tp-old false"));
  REQUIRE(env.hasLog("kick-old false"));
  REQUIRE(env.hasLog("kick-cur true"));
  REQUIRE(env.hasLog("cb.players.kick: sessão inválida"));
}

TEST_CASE("api: escapeLogText escapes C0/C1 controls, line separators and bidi overrides") {
  REQUIRE(cbscript::escapeLogText("a\nb\rc\x01\x7f\td") == "a\\nb\\rc\\x01\\x7f\td");
  REQUIRE(cbscript::escapeLogText("x\xC2\x85y") == "x\\u{0085}y");                    // NEL (C1)
  REQUIRE(cbscript::escapeLogText("\xE2\x80\xA8\xE2\x80\xA9") == "\\u{2028}\\u{2029}");  // separadores
  REQUIRE(cbscript::escapeLogText("\xE2\x80\xAE" "abc") == "\\u{202e}abc");              // RLO
  REQUIRE(cbscript::escapeLogText("\xE2\x81\xA6\xE2\x81\xA9") == "\\u{2066}\\u{2069}");  // isolates
  REQUIRE(cbscript::escapeLogText("ção €") == "ção €");                                  // UTF-8 comum intacto
}

TEST_CASE("api: callback error messages are escaped in the log") {
  ApiEnv env;
  env.addRes("a", R"(cb.events.on('boom', function() error('x\n[2026-11-10] [info] forjado') end))");
  env.boot();
  env.join(3, "ana");
  env.event(3, "boom");
  REQUIRE(env.waitLog("forjado", LogLevel::Error));
  REQUIRE_FALSE(env.hasLog("\n[2026-11-10]", LogLevel::Error));
  REQUIRE(env.hasLog("x\\n[2026-11-10] [info] forjado", LogLevel::Error));
}

TEST_CASE("api: escapeLogText also escapes LRM/RLM, ALM and BOM") {
  REQUIRE(cbscript::escapeLogText("\xE2\x80\x8E" "a" "\xE2\x80\x8F") == "\\u{200e}a\\u{200f}");
  REQUIRE(cbscript::escapeLogText("x\xD8\x9Cy") == "x\\u{061c}y");
  REQUIRE(cbscript::escapeLogText("\xEF\xBB\xBF" "z") == "\\u{feff}z");
  REQUIRE(cbscript::escapeLogText("\xD8\xA7") == "\xD8\xA7");  // letra árabe comum intacta
}
