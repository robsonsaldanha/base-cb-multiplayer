#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "test_util.hpp"


TEST_CASE("sandbox: forbidden functions are absent or fail") {
  Fixture f;
  const char* forbidden[] = {
      "return io == nil",
      "return os.execute == nil",
      "return os.exit == nil",
      "return os.getenv == nil",
      "return os.remove == nil and os.rename == nil and os.tmpname == nil",
      "return debug == nil",
      "return package == nil",
      "return dofile == nil",
      "return loadfile == nil",
      "return string.dump == nil",
      "return warn == nil",
      "return not pcall(function() return debug.getinfo(1) end)",
      "return not pcall(function() return package.loadlib('x', 'y') end)",
      "return not pcall(function() return io.open('x') end)",
  };
  for (const char* code : forbidden) {
    INFO(code);
    REQUIRE(f.isTrue(code));
  }
}

TEST_CASE("sandbox: load accepts only text chunks and uses the sandbox env") {
  Fixture f;
  REQUIRE(f.isTrue("local fn, e = load('\\27Lua\\x54\\0'); return fn == nil and type(e) == 'string'"));
  REQUIRE(f.isTrue("local fn = load(string.char(27) .. 'LuaT', 'x', 'b'); return fn == nil"));
  REQUIRE(f.isTrue("local fn = load('return io'); return fn ~= nil and fn() == nil"));
  REQUIRE(f.isTrue("local fn = load('return x', 'c', 't', {x = 7}); return fn() == 7"));
  REQUIRE(f.isTrue("local fn = load('return os', 'c', 't', nil); return fn() == os"));
  // leitor em função também não consegue entregar bytecode
  REQUIRE(f.isTrue("local s = {string.char(27), 'LuaT'}; local i = 0; "
                   "local fn = load(function() i = i + 1; return s[i] end); return fn == nil"));
}

TEST_CASE("sandbox: string metatable is protected and does not leak removed functions") {
  Fixture f;
  REQUIRE(f.isTrue("return getmetatable('') == false"));
  REQUIRE(f.isTrue("return ('x').dump == nil"));
  REQUIRE(f.isTrue("return ('abc'):upper() == 'ABC'"));
  REQUIRE(f.isTrue("return not pcall(setmetatable, '', {})"));
}

TEST_CASE("sandbox: allowed libraries work") {
  Fixture f;
  REQUIRE(f.isTrue("return type(os.time()) == 'number' and type(os.clock()) == 'number'"));
  REQUIRE(f.isTrue("return type(os.date('%Y-%m-%d')) == 'string' and os.date('*t').year >= 2024"));
  REQUIRE(f.isTrue("return math.max(1, 3) == 3 and table.concat({1, 2}, ',') == '1,2'"));
  REQUIRE(f.isTrue("return utf8.len('\xC3\xA1') == 1 and string.format('%d', 5) == '5'"));
  REQUIRE(f.isTrue("return coroutine.wrap(function() coroutine.yield(4) end)() == 4"));
  REQUIRE(f.isTrue("return type(collectgarbage('count')) == 'number'"));
  REQUIRE(f.isTrue("return not pcall(collectgarbage, 'stop')"));
  REQUIRE(f.isTrue("return not pcall(collectgarbage, 'incremental')"));
  REQUIRE(f.isTrue("return _G == _ENV and _G.pairs == pairs"));
  REQUIRE(f.isTrue("local ok, e = xpcall(error, function(m) return 'h:' .. m end, 'x'); return not ok and e == 'h:x'"));
  REQUIRE(f.isTrue("return getmetatable(setmetatable({}, {__index = {a = 1}})).__index.a == 1"));
  REQUIRE(f.isTrue("return not pcall(setmetatable, {}, {__gc = function() end})"));
}

TEST_CASE("sandbox: print goes to the warn callback with the resource name") {
  Fixture f;
  auto res = f.run("print('ola', 1, nil)");
  REQUIRE(res.ok);
  REQUIRE(f.warns.size() == 1);
  REQUIRE(contains(f.warns[0], "teste"));
  REQUIRE(contains(f.warns[0], "ola\t1\tnil"));
}

TEST_CASE("require loads server/<path>.lua once and caches it") {
  Fixture f;
  put(f.t.p / "server/util/mod.lua", "LOADS = (LOADS or 0) + 1\nreturn { v = 42, io = io, name = ... }\n");
  put(f.t.p / "server/plain.lua", "X = 1\n");
  REQUIRE(f.isTrue("local a = require('util.mod'); local b = require('util.mod'); "
                   "return a == b and a.v == 42 and LOADS == 1 and a.io == nil and a.name == 'util.mod'"));
  REQUIRE(f.isTrue("return require('plain') == true and X == 1"));
}

TEST_CASE("require refuses unsafe names and missing files") {
  Fixture f;
  put(f.t.p / "x.lua", "return 1");
  put(f.t.p / "server/ok.lua", "return 1");
  const char* bad[] = {"../x",   "/x",      "C:/x",      "C:\\x",      "..x",     ".x",  "x.",
                       "a..b",   "",        "server.ok", "nao_existe", "ok.lua",  "ok/", "ok x"};
  for (const char* name : bad) {
    INFO(name);
    sol::protected_function req = f.r->lua()["require"];
    REQUIRE(req.valid());
    auto res = f.r->callWith(req, std::string(name));
    REQUIRE_FALSE(res.ok);
    REQUIRE(contains(res.error, "require"));
  }
  REQUIRE(f.isTrue("return not pcall(require, 1)"));
  REQUIRE(f.isTrue("return require('ok') == 1"));
}

TEST_CASE("require detects circular modules") {
  Fixture f;
  put(f.t.p / "server/a.lua", "return require('b')");
  put(f.t.p / "server/b.lua", "return require('a')");
  auto res = f.run("return require('a')");
  REQUIRE_FALSE(res.ok);
  REQUIRE(contains(res.error, "circular"));
}

TEST_CASE("memory limit raises a memory error and the state stays usable") {
  Fixture f(shortLimits(size_t{16} << 20));
  auto before = f.r->memoryUsed();
  REQUIRE(before > 0);
  REQUIRE(before < (size_t{16} << 20));

  // blocos de 256 KiB sem lixo intermediário (perto do teto, lixo faria o GC de emergência rodar sem parar).
  auto res = f.run("local s = string.rep('x', 256 * 1024) local t = {} for i = 1, 1e6 do t[i] = s .. i end");
  REQUIRE_FALSE(res.ok);
  REQUIRE_FALSE(res.timedOut);
  REQUIRE(contains(res.error, "mem\xC3\xB3ria"));
  REQUIRE(contains(res.error, "teste"));

  auto big = f.run("return #string.rep('x', 64 * 1024 * 1024)");
  REQUIRE_FALSE(big.ok);
  REQUIRE(contains(big.error, "mem\xC3\xB3ria"));

  REQUIRE(f.isTrue("collectgarbage('step'); return 1 + 1 == 2"));
  REQUIRE(f.r->memoryUsed() <= (size_t{16} << 20));
  // pcall dentro do script também recebe o erro de memória e continua.
  REQUIRE(f.isTrue("local ok = pcall(string.rep, 'x', 64 * 1024 * 1024); return ok == false"));
}

TEST_CASE("watchdog warns and then aborts an infinite loop") {
  Fixture f;
  auto t0 = std::chrono::steady_clock::now();
  auto res = f.run("while true do end");
  auto dt = std::chrono::steady_clock::now() - t0;
  REQUIRE_FALSE(res.ok);
  REQUIRE(res.timedOut);
  REQUIRE(dt >= 200ms);
  REQUIRE(dt < 10s);  // folga para máquina carregada (ctest -j); o travamento real é pego pelo TIMEOUT do ctest
  REQUIRE(contains(res.error, "teste"));
  REQUIRE(contains(res.error, "stack traceback"));
  REQUIRE(f.warns.size() == 1);
  REQUIRE(contains(f.warns[0], "teste"));
  REQUIRE(contains(f.warns[0], "abortada"));

  // depois do aborto o estado continua usável e nenhum aborto "velho" vaza para a próxima chamada. Sob carga a
  // chamada curta pode passar de warnAfter (20 ms) por preempção; um aviso "levou X ms" é aceitável, aborto não.
  f.warns.clear();
  auto again = f.run("local s = 0 for i = 1, 1000 do s = s + i end return s == 500500");
  INFO(again.error);
  REQUIRE(again.ok);
  REQUIRE_FALSE(again.timedOut);
  REQUIRE(again.value.as<bool>());
  REQUIRE(f.warns.size() <= 1);
  for (auto& w : f.warns) REQUIRE_FALSE(contains(w, "abortada"));
}

TEST_CASE("watchdog cannot be defeated by pcall or coroutines") {
  const char* loops[] = {
      "while true do pcall(function() while true do end end) end",
      "while true do xpcall(function() while true do end end, function() while true do end end) end",
      "coroutine.wrap(function() while true do end end)()",
      "while true do coroutine.resume(coroutine.create(function() while true do end end)) end",
      "local co = coroutine.wrap(function() while true do coroutine.yield() end end) while true do co() end",
      "while true do pcall(function() local x <close> = setmetatable({}, {__close = function() "
      "while true do end end}) while true do end end) end",
      "local t = {} for i = 1, 100 do t[i] = i end "
      "while true do pcall(table.sort, t, function(a, b) while true do end end) end",
  };
  for (const char* code : loops) {
    INFO(code);
    Fixture f;
    auto t0 = std::chrono::steady_clock::now();
    auto res = f.run(code);
    REQUIRE(std::chrono::steady_clock::now() - t0 < 3s);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.timedOut);
    REQUIRE(f.isTrue("return true"));
  }
}

TEST_CASE("slow but finite call only warns") {
  Fixture f;
  auto res = f.run("local t = os.clock() while os.clock() - t < 0.06 do end return 1");
  REQUIRE(res.ok);
  REQUIRE_FALSE(res.timedOut);
  REQUIRE(f.warns.size() == 1);
}

TEST_CASE("call yields and resumes as a coroutine") {
  Fixture f;
  sol::protected_function fn = f.r->lua().load("return function(a) local b = coroutine.yield(a + 1) return b * 2 end",
                                               "=t", sol::load_mode::text)
                                   .get<sol::protected_function>()
                                   .call()
                                   .get<sol::protected_function>();
  auto r1 = f.r->callWith(fn, 1);
  REQUIRE(r1.ok);
  REQUIRE(r1.yielded);
  REQUIRE(r1.value.as<int>() == 2);
  std::vector<sol::object> args{sol::make_object(f.r->lua(), 10)};
  auto r2 = f.r->resume(r1.thread, args);
  REQUIRE(r2.ok);
  REQUIRE_FALSE(r2.yielded);
  REQUIRE(r2.value.as<int>() == 20);
}

TEST_CASE("runMain runs server scripts in order and keeps exports") {
  TmpDir t;
  put(t.p / "server/a.lua", "ORDER = 'a'\n");
  put(t.p / "server/main.lua", "ORDER = ORDER .. 'b'\nreturn { order = ORDER }\n");
  ResourceManifest m;
  m.name = "meu-res";
  m.serverScripts = {"server/a.lua", "server/main.lua"};
  Resource r(m, t.p, shortLimits(), {});
  std::string err;
  REQUIRE(r.runMain(err));
  INFO(err);
  sol::table ex = r.exports();
  REQUIRE(ex["order"].get<std::string>() == "ab");
}

TEST_CASE("runMain error carries resource name and traceback") {
  TmpDir t;
  put(t.p / "server/main.lua", "local function falha() error('boom') end\nfalha()\n");
  ResourceManifest m;
  m.name = "meu-res";
  m.serverScripts = {"server/main.lua"};
  Resource r(m, t.p, shortLimits(), {});
  std::string err;
  REQUIRE_FALSE(r.runMain(err));
  REQUIRE(contains(err, "meu-res"));
  REQUIRE(contains(err, "boom"));
  REQUIRE(contains(err, "stack traceback"));
  REQUIRE(contains(err, "main.lua:1"));
}

TEST_CASE("runMain reports syntax errors and missing files") {
  TmpDir t;
  put(t.p / "server/main.lua", "isto nao e lua\n");
  ResourceManifest m;
  m.name = "meu-res";
  m.serverScripts = {"server/main.lua"};
  {
    Resource r(m, t.p, shortLimits(), {});
    std::string err;
    REQUIRE_FALSE(r.runMain(err));
    REQUIRE(contains(err, "meu-res"));
    REQUIRE(contains(err, "main.lua"));
  }
  m.serverScripts = {"server/falta.lua"};
  Resource r(m, t.p, shortLimits(), {});
  std::string err;
  REQUIRE_FALSE(r.runMain(err));
  REQUIRE(contains(err, "falta.lua"));
}

TEST_CASE("loadManifest reads a full resource.json") {
  TmpDir t;
  put(t.p / "resource.json", R"({"name":"echo","version":"1.0.0","server":["server/main.lua","server/x/y.lua"],
    "dependencies":["base-lib"],"config":{"k":[1,2]},"permissions":{"http":["api.example.com","hooks.example.com:8443"],"db":true}})");
  auto m = cbscript::loadManifest(t.p);
  REQUIRE(m.name == "echo");
  REQUIRE(m.version == "1.0.0");
  REQUIRE(m.serverScripts == std::vector<std::string>{"server/main.lua", "server/x/y.lua"});
  REQUIRE(m.dependencies == std::vector<std::string>{"base-lib"});
  REQUIRE(m.config["k"][1] == 2);
  REQUIRE(m.httpAllow == std::vector<std::string>{"api.example.com", "hooks.example.com:8443"});
  REQUIRE(m.db);

  put(t.p / "resource.json", R"({"name":"min","version":"0.1"})");
  auto m2 = cbscript::loadManifest(t.p);
  REQUIRE(m2.serverScripts.empty());
  REQUIRE_FALSE(m2.db);
  REQUIRE(m2.config.is_object());
}

TEST_CASE("loadManifest rejects invalid names and unsafe paths") {
  TmpDir t;
  const char* bad[] = {
      R"({"name":"a b","version":"1"})",
      R"({"name":"..","version":"1"})",
      R"({"name":"x/y","version":"1"})",
      R"({"name":"","version":"1"})",
      R"({"name":"ok"})",
      R"({"name":"ok","version":"1","server":["server/../../x.lua"]})",
      R"({"name":"ok","version":"1","server":["/etc/x.lua"]})",
      R"({"name":"ok","version":"1","server":["client/main.lua"]})",
      R"({"name":"ok","version":"1","server":["server/main.txt"]})",
      R"({"name":"ok","version":"1","server":"server/main.lua"})",
      R"({"name":"ok","version":"1","dependencies":["../x"]})",
      R"({"name":"ok","version":"1","permissions":{"db":"sim"}})",
      R"({"name":"ok","version":"1","permissions":{"http":["https://x.com"]}})",
      R"({"name":"ok","version":"1","permissions":{"http":["*.x.com"]}})",
      R"({"name":"ok","version":"1","permissions":{"http":["1.2.3.4"]}})",
      R"({"name":"ok","version":"1","permissions":{"http":["Api.Example.com"]}})",
      R"({"name":"ok","version":"1","permissions":{"http":["x.com:0"]}})",
      R"({"name":"ok","version":"1","permissions":{"http":["bücher.example"]}})",
      R"([1,2])",
      "nao e json",
  };
  for (const char* j : bad) {
    INFO(j);
    put(t.p / "resource.json", j);
    REQUIRE_THROWS_AS(cbscript::loadManifest(t.p), std::runtime_error);
  }
  fs::remove(t.p / "resource.json");
  REQUIRE_THROWS_AS(cbscript::loadManifest(t.p), std::runtime_error);
}
