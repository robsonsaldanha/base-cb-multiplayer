// Regressões da revisão de segurança (fix round 1): fugas da vigia, require por link/junção, limites menores.
// Todo caso que já travou roda em guarded() (prazo rígido de 10 s): uma regressão falha em vez de pendurar o CI.
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <vector>

#include "test_util.hpp"

namespace {

// Aborto dentro do limite com folga de 2x (+ granularidade do relógio da vigia).
constexpr long long kMaxAbortMs = 2 * 200 + 50;

struct Case {
  const char* name;
  const char* code;
};

void expectAbort(const Case& c, Limits lim = shortLimits()) {
  Fixture f(lim);
  Timed t = guarded(c.name, [&] { return runRaw(*f.r, c.code); });
  std::printf("TIMING %-28s %5lld ms timedOut=%d\n", c.name, t.ms, t.res.timedOut ? 1 : 0);
  INFO(c.name << ": " << t.res.error);
  REQUIRE_FALSE(t.res.ok);
  REQUIRE(t.res.timedOut);
  REQUIRE(t.ms <= kMaxAbortMs);
  // o estado continua usável
  Timed after = guarded(c.name, [&] { return runRaw(*f.r, "return 'vivo'"); });
  INFO(after.res.error);
  REQUIRE(after.res.ok);
}

}  // namespace

TEST_CASE("escape C1: __close after an abort runs under the watchdog") {
  const Case cases[] = {
      {"c1_wrap_close_loop",
       "local co = coroutine.wrap(function() "
       "  local x <close> = setmetatable({}, {__close = function() while true do end end}) "
       "  while true do end "
       "end) return tostring(pcall(co))"},
      {"c1_create_resume_close",
       "local co = coroutine.create(function() "
       "  local x <close> = setmetatable({}, {__close = function() while true do end end}) "
       "  while true do end "
       "end) coroutine.resume(co) return tostring(coroutine.close(co))"},
      {"c1_close_suspended_loop",
       "local co = coroutine.create(function() "
       "  local x <close> = setmetatable({}, {__close = function() local i = 0 while true do i = i + 1 end end}) "
       "  coroutine.yield() "
       "end) coroutine.resume(co) return tostring(coroutine.close(co))"},
  };
  for (const auto& c : cases) expectAbort(c);
}

TEST_CASE("escape C1: closing a stored aborted coroutine in a later call") {
  Fixture f;
  Timed t1 = guarded("c1_store", [&] {
    return runRaw(*f.r,
                  "G_co = coroutine.create(function() "
                  "  local x <close> = setmetatable({}, {__close = function() while true do end end}) "
                  "  while true do end "
                  "end) coroutine.resume(G_co)");
  });
  std::printf("TIMING %-28s %5lld ms timedOut=%d\n", "c1_store", t1.ms, t1.res.timedOut ? 1 : 0);
  REQUIRE(t1.res.timedOut);
  Timed t2 = guarded("c1_close_later", [&] { return runRaw(*f.r, "return tostring(coroutine.close(G_co))"); });
  std::printf("TIMING %-28s %5lld ms timedOut=%d\n", "c1_close_later", t2.ms, t2.res.timedOut ? 1 : 0);
  INFO(t2.res.error);
  REQUIRE(t2.res.timedOut);
  REQUIRE(t2.ms <= kMaxAbortMs);
}

TEST_CASE("escape C2: loops of long C calls are aborted at the call boundary") {
  Limits big = shortLimits(size_t{256} << 20);
  const Case cases[] = {
      {"c2_rep_loop", "local x for i = 1, 1e9 do x = string.rep('a', 1 << 24) end"},
      {"c2_concat_loop", "local s = string.rep('a', 1 << 24) local t = {s, s} for i = 1, 1e9 do local x = table.concat(t) end"},
      {"c2_find_plain_loop", "local s = string.rep('a', 1 << 25) for i = 1, 1e9 do local x = s:find('b', 1, true) end"},
      {"c2_gc_step_loop",
       "local t = {} for i = 1, 2e5 do t[i] = {i} end while true do collectgarbage('step', 1 << 30) end"},
  };
  for (const auto& c : cases) expectAbort(c, big);
}

TEST_CASE("escape I1: catastrophic patterns are interrupted inside C") {
  const Case cases[] = {
      {"i1_find_backtrack", "return tostring(string.find(string.rep('a', 3000), string.rep('a-', 20) .. 'b'))"},
      {"i1_gmatch_backtrack",
       "local s = ('a'):rep(1e6) local n = 0 for w in s:gmatch('a-a-a-a-a-a-b') do n = n + 1 end return n"},
      {"i1_gsub_backtrack", "return (string.gsub(string.rep('a', 3000), string.rep('a-', 20) .. 'b', 'x'))"},
  };
  for (const auto& c : cases) expectAbort(c);
}

TEST_CASE("escape I1: table.move with a huge range is aborted") {
  expectAbort({"i1_table_move", "table.move({}, 1, 1e14, 2) return 'done'"});
}

TEST_CASE("table.move keeps standard semantics") {
  Fixture f;
  REQUIRE(f.isTrue("local t = table.move({1, 2, 3}, 1, 3, 2) return t[1] == 1 and t[2] == 1 and t[3] == 2 and t[4] == 3"));
  REQUIRE(f.isTrue("local t = table.move({1, 2, 3}, 2, 3, 1) return t[1] == 2 and t[2] == 3 and t[3] == 3"));
  REQUIRE(f.isTrue("local d = table.move({1, 2}, 1, 2, 5, {}) return d[5] == 1 and d[6] == 2 and d[1] == nil"));
  REQUIRE(f.isTrue("local t = {1} return table.move(t, 3, 2, 1) == t"));
  REQUIRE(f.isTrue("return not pcall(table.move, {}, 'x', 2, 1)"));
  REQUIRE(f.isTrue("return not pcall(table.move, {}, 1.5, 2, 1)"));
  REQUIRE(f.isTrue("return not pcall(table.move, {}, -1, math.maxinteger, 1)"));
}

TEST_CASE("collectgarbage allows only count and step") {
  Fixture f;
  REQUIRE(f.isTrue("return type(collectgarbage('count')) == 'number'"));
  REQUIRE(f.isTrue("return type(collectgarbage('step')) == 'boolean'"));
  REQUIRE(f.isTrue("return not pcall(collectgarbage)"));
  REQUIRE(f.isTrue("return not pcall(collectgarbage, 'collect')"));
  REQUIRE(f.isTrue("return not pcall(collectgarbage, 'isrunning')"));
}

TEST_CASE("print is size and rate limited") {
  Fixture f;
  auto t = guarded("print_flood", [&] { return runRaw(*f.r, "for i = 1, 1e4 do print(string.rep('x', 1000)) end"); });
  REQUIRE(t.res.ok);
  REQUIRE(f.warns.size() <= 205);
  REQUIRE(f.warns.size() >= 200);

  Fixture g;
  auto big = g.run("print(string.rep('y', 200 * 1024))");
  REQUIRE(big.ok);
  REQUIRE(g.warns.size() == 1);
  REQUIRE(g.warns[0].size() < 66 * 1024);
  REQUIRE(contains(g.warns[0], "truncad"));
}

TEST_CASE("print drop summary is emitted once the second ends") {
  Fixture f;
  auto t = guarded("print_summary", [&] {
    return runRaw(*f.r,
                  "for i = 1, 300 do print('a') end "
                  "local t0 = os.clock() while os.clock() - t0 < 0.15 do end");
  });
  REQUIRE(t.res.timedOut == false);
  // segunda janela: um print novo faz sair o resumo dos descartados
  std::this_thread::sleep_for(1100ms);
  REQUIRE(f.run("print('b')").ok);
  bool summary = false;
  for (auto& w : f.warns)
    if (contains(w, "100") && contains(w, "descartad")) summary = true;
  REQUIRE(summary);
}

TEST_CASE("require refuses empty segments even when the file exists") {
  Fixture f;
  put(f.t.p / "server/x/.lua", "return 1");
  put(f.t.p / "server/x.lua", "return 2");
  REQUIRE(f.isTrue("return not pcall(require, 'x.')"));
  REQUIRE(f.isTrue("return not pcall(require, '.x')"));
  REQUIRE(f.isTrue("return require('x') == 2"));
}

#ifdef _WIN32
TEST_CASE("require cache is case-insensitive on Windows") {
  Fixture f;
  put(f.t.p / "server/util/mod.lua", "N = (N or 0) + 1 return {}");
  REQUIRE(f.isTrue("return require('util.mod') == require('Util.Mod') and N == 1"));
}
#endif

namespace {
// Junção (Windows, sem admin) ou link simbólico de diretório. false se não der para criar.
bool makeDirLink(const fs::path& target, const fs::path& link) {
  std::error_code ec;
  fs::create_directory_symlink(target, link, ec);
  if (!ec) return true;
#ifdef _WIN32
  std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >NUL 2>&1";
  return std::system(cmd.c_str()) == 0 && fs::exists(link);
#else
  return false;
#endif
}
}  // namespace

TEST_CASE("require and server scripts refuse links and junctions") {
  Fixture f;
  TmpDir outside;
  put(outside.p / "secret.lua", "return 'segredo'");
  put(f.t.p / "server/ok.lua", "return 'ok'");
  fs::create_directories(f.t.p / "server");
  if (!makeDirLink(outside.p, f.t.p / "server" / "sub")) SKIP("não foi possível criar junção/link de diretório");
  REQUIRE(f.isTrue("return require('ok') == 'ok'"));
  auto res = f.run("return require('sub.secret')");
  REQUIRE_FALSE(res.ok);
  REQUIRE(contains(res.error, "require"));

  ResourceManifest m;
  m.name = "linkado";
  m.serverScripts = {"server/sub/secret.lua"};
  Resource r(m, f.t.p, shortLimits(), {});
  std::string err;
  REQUIRE_FALSE(r.runMain(err));
  REQUIRE(contains(err, "secret.lua"));
  std::error_code ec;
  fs::remove(f.t.p / "server" / "sub", ec);  // remove o link, não o alvo
}

TEST_CASE("resource dir itself reached through a junction still works") {
  TmpDir real;
  TmpDir holder;
  put(real.p / "server/main.lua", "return {v = 1}");
  fs::path via = holder.p / "res";
  if (!makeDirLink(real.p, via)) SKIP("não foi possível criar junção/link de diretório");
  ResourceManifest m;
  m.name = "via";
  m.serverScripts = {"server/main.lua"};
  {
    Resource r(m, via, shortLimits(), {});
    std::string err;
    INFO(err);
    REQUIRE(r.runMain(err));
  }
  std::error_code ec;
  fs::remove(via, ec);
}
