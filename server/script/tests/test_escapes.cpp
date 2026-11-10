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

// ---- Fix round 2 ----

TEST_CASE("escape R2: table.insert/remove with a huge __len are aborted") {
  const Case cases[] = {
      {"r2_tinsert_len",
       "local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end}) "
       "table.insert(t, 1, 'x') return 'done'"},
      {"r2_tremove_len",
       "local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end}) "
       "table.remove(t, 1) return 'done'"},
  };
  for (const auto& c : cases) expectAbort(c);
}

TEST_CASE("escape R2: table.sort without comparator over huge strings is aborted") {
  expectAbort({"r2_sort_strcmp",
               "local big = ('a'):rep(1 << 22) local t = {} for i = 1, 3000 do t[i] = big end "
               "table.sort(t) return 'done'"},
              shortLimits(size_t{256} << 20));
}

TEST_CASE("escape R2: loops of one expensive operator are aborted") {
  Limits big = shortLimits(size_t{256} << 20);
  const Case cases[] = {
      {"r2_lt_loop", "local s = ('a'):rep(1 << 25) local t = ('a'):rep(1 << 25) while true do local _ = s < t end"},
      {"r2_eq_loop", "local s = ('a'):rep(1 << 25) local t = ('a'):rep(1 << 25) while true do local _ = s == t end"},
      {"r2_concat_op_loop", "local s = ('a'):rep(1 << 24) while true do local _ = s .. s end"},
      {"r2_empty_loop", "while true do end"},
      {"r2_loop_after_child_abort",
       "local co = coroutine.create(function() while true do end end) coroutine.resume(co) while true do end"},
      {"r2_loop_in_wrapped_child", "coroutine.wrap(function() local s = ('a'):rep(1 << 25) "
                                   "local t = ('a'):rep(1 << 25) while true do local _ = s < t end end)()"},
  };
  for (const auto& c : cases) expectAbort(c, big);
}

TEST_CASE("table.insert and table.remove keep standard semantics") {
  Fixture f;
  REQUIRE(f.isTrue("local t = {1, 2} table.insert(t, 3) return #t == 3 and t[3] == 3"));
  REQUIRE(f.isTrue("local t = {1, 2} table.insert(t, 1, 0) return t[1] == 0 and t[2] == 1 and t[3] == 2"));
  REQUIRE(f.isTrue("local t = {1, 2} table.insert(t, 3, 9) return t[3] == 9"));
  REQUIRE(f.isTrue("local ok, e = pcall(table.insert, {1}, 5, 9) "
                   "return not ok and e:find('position out of bounds', 1, true) ~= nil"));
  REQUIRE(f.isTrue("local ok, e = pcall(table.insert, {}, 1, 2, 3) "
                   "return not ok and e:find(\"wrong number of arguments to 'insert'\", 1, true) ~= nil"));
  REQUIRE(f.isTrue("local ok, e = pcall(table.insert, nil, 1) "
                   "return not ok and e:find('(table expected, got nil)', 1, true) ~= nil"));
  REQUIRE(f.isTrue("local t = {1, 2, 3} return table.remove(t) == 3 and #t == 2"));
  REQUIRE(f.isTrue("local t = {1, 2, 3} return table.remove(t, 1) == 1 and t[1] == 2 and t[2] == 3 and t[3] == nil"));
  REQUIRE(f.isTrue("local t = {} return table.remove(t) == nil and table.remove(t, 0) == nil"));
  REQUIRE(f.isTrue("local t = {1} return table.remove(t, 2) == nil and t[1] == 1"));
  REQUIRE(f.isTrue("local ok, e = pcall(table.remove, {1}, 5) "
                   "return not ok and e:find('position out of bounds', 1, true) ~= nil"));
  REQUIRE(f.isTrue("local log = {} local t = setmetatable({}, {__len = function() return 2 end, "
                   "__index = function(_, k) return k * 10 end, "
                   "__newindex = function(t, k, v) log[#log + 1] = k rawset(t, k, v) end}) "
                   "table.insert(t, 1, 'x') return table.concat(log, ',') == '3,2,1'"));
}

TEST_CASE("table.sort keeps standard behaviour") {
  Fixture f;
  REQUIRE(f.isTrue("local t = {3, 1, 2} table.sort(t) return t[1] == 1 and t[2] == 2 and t[3] == 3"));
  REQUIRE(f.isTrue("local t = {3, 1, 2} table.sort(t, function(a, b) return a > b end) return t[1] == 3"));
  REQUIRE(f.isTrue("local t = {'b', 'a'} table.sort(t) return t[1] == 'a'"));
  REQUIRE(f.isTrue("return not pcall(table.sort, {1, 'x', 2})"));
  REQUIRE(f.isTrue("return not pcall(table.sort, {1, 2}, 5)"));
  REQUIRE(f.isTrue("local t = {} for i = 1, 100 do t[i] = i % 7 end "
                   "local ok, e = pcall(table.sort, t, function(a, b) return true end) "
                   "return not ok and e:find('invalid order function', 1, true) ~= nil"));
}

TEST_CASE("table.move type-checks its table arguments like C") {
  Fixture f;
  REQUIRE(f.isTrue("local ok, e = pcall(table.move, 1, 1, 2, 1) "
                   "return not ok and e:find('#1 to', 1, true) and e:find('(table expected, got number)', 1, true) ~= nil"));
  REQUIRE(f.isTrue("local ok, e = pcall(table.move, 1, 5, 1, 1) "
                   "return not ok and e:find('#1 to', 1, true) and e:find('(table expected, got number)', 1, true) ~= nil"));
  REQUIRE(f.isTrue("local ok, e = pcall(table.move, {}, 1, 2, 1, 'x') "
                   "return not ok and e:find('#5 to', 1, true) and e:find('(table expected, got string)', 1, true) ~= nil"));
  REQUIRE(f.isTrue("return table.move({1, 2}, '1', '2', 3)[4] == 2"));
}

TEST_CASE("coroutine library keeps standard behaviour") {
  Fixture f;
  REQUIRE(f.isTrue("local co = coroutine.create(function(a) local b = coroutine.yield(a + 1) return b * 2 end) "
                   "local ok1, v1 = coroutine.resume(co, 1) local ok2, v2 = coroutine.resume(co, 10) "
                   "return ok1 and v1 == 2 and ok2 and v2 == 20 and coroutine.status(co) == 'dead'"));
  REQUIRE(f.isTrue("local ok, e = coroutine.resume(coroutine.create(function() error('x') end)) "
                   "return not ok and e:find('x') ~= nil"));
  REQUIRE(f.isTrue("local co = coroutine.create(function() end) coroutine.resume(co) "
                   "local ok, e = coroutine.resume(co) return not ok and e == 'cannot resume dead coroutine'"));
  REQUIRE(f.isTrue("return not pcall(coroutine.resume, 1)"));
  REQUIRE(f.isTrue("local g = coroutine.wrap(function() coroutine.yield(1) coroutine.yield(2) end) "
                   "return g() == 1 and g() == 2"));
  REQUIRE(f.isTrue("local ok, e = pcall(coroutine.wrap(function() error('boom') end)) "
                   "return not ok and e:find('boom') ~= nil"));
  REQUIRE(f.isTrue("local closed = false local co = coroutine.create(function() "
                   "local x <close> = setmetatable({}, {__close = function() closed = true end}) coroutine.yield() end) "
                   "coroutine.resume(co) return coroutine.close(co) == true and closed"));
  REQUIRE(f.isTrue("return not pcall(coroutine.close, coroutine.running())"));
  REQUIRE(f.isTrue("local A local B = coroutine.create(function() return pcall(coroutine.close, A) end) "
                   "A = coroutine.create(function() return coroutine.resume(B) end) "
                   "local ok1, ok2, pok, perr = coroutine.resume(A) "
                   "return ok1 and ok2 and not pok and perr:find('normal') ~= nil"));
}

// ---- Fix round 3: formas 70-75 do revisor (troca de thread em volta de operador caro) ----

TEST_CASE("escape R3: expensive operator after coroutine switches is aborted") {
  Limits big = shortLimits(size_t{256} << 20);
  const std::string pre = "local s = ('a'):rep(1 << 25) local t = ('a'):rep(1 << 25) ";
  const std::string codes[] = {
      // 70: A retoma B, B cede de volta, A entra no laço
      pre + "local B = coroutine.create(function() coroutine.yield() end) "
            "local A = coroutine.create(function() coroutine.resume(B) while true do local _ = s < t end end) "
            "coroutine.resume(A)",
      // 71: yield de dentro de metamétodo, laço no chamador
      pre + "local mt = {__index = function() coroutine.yield(1) return 1 end} "
            "local co = coroutine.wrap(function() local x = setmetatable({}, mt).foo return x end) "
            "co() while true do local _ = s == t end",
      // 72: erros saindo de resume/wrap/close antes do laço
      pre + "for i = 1, 5 do pcall(coroutine.wrap(function() error('x') end)) end "
            "local co = coroutine.create(function() local _ <close> = setmetatable({}, {__close = function() end}) "
            "error('y') end) coroutine.resume(co) coroutine.close(co) while true do local _ = s < t end",
      // 73: iterador gmatch numa corrotina, laço numa função carregada
      pre + "local it = coroutine.wrap(function() for w in ('abc'):gmatch('.') do coroutine.yield(w) end end) "
            "it() it() local f = load('local s, t = ... while true do local _ = s < t end') f(s, t)",
      // 74: resume de si mesma / wrap morta / close da corrente
      pre + "local co; co = coroutine.create(function() return coroutine.resume(co) end) "
            "coroutine.resume(co) coroutine.resume(co) local w = coroutine.wrap(function() end) w() pcall(w) "
            "pcall(coroutine.close, coroutine.running()) while true do local _ = s < t end",
      // 75: wrap aninhado 1000 níveis com o laço no fundo
      pre + "local function nest(n) if n == 0 then while true do local _ = s < t end end "
            "return coroutine.wrap(nest)(n - 1) end pcall(nest, 1000) while true do local _ = s < t end",
  };
  int k = 70;
  for (const auto& code : codes) {
    std::string name = "r3_shape_" + std::to_string(k++);
    expectAbort({name.c_str(), code.c_str()}, big);
  }
}

TEST_CASE("R3 semantics: native coroutine and table libraries (shape 76)") {
  Fixture f;
  auto res = f.run(
      "local r = {} "
      "r[#r+1] = select(2, pcall(coroutine.resume, 1)) "
      "r[#r+1] = tostring(coroutine.resume(coroutine.create(function(...) return select('#', ...) end), 1, nil, 3)) "
      "local co = coroutine.create(function() coroutine.yield(coroutine.running()) end) "
      "local _, inner = coroutine.resume(co) r[#r+1] = tostring(inner == co) .. coroutine.status(co) "
      "r[#r+1] = select(2, pcall(coroutine.close, coroutine.running())) "
      "r[#r+1] = select(2, pcall(coroutine.wrap(function() error('boom') end))) "
      "r[#r+1] = tostring(coroutine.close(coroutine.create(print))) "
      "local t = {3,1,2} table.sort(t) r[#r+1] = table.concat(t) "
      "r[#r+1] = select(2, pcall(table.sort, {1,'a'})) "
      "t = {1,2,3} table.insert(t, 2, 9) table.insert(t, 7) "
      "r[#r+1] = table.concat(t, ',') .. '/' .. table.remove(t, 1) .. '/' .. tostring(table.remove({})) "
      "r[#r+1] = select(2, pcall(table.insert, {}, 5, 1)) "
      "return table.concat(r, ' | ')");
  INFO(res.error);
  REQUIRE(res.ok);
  std::string v = res.value.as<std::string>();
  INFO(v);
  REQUIRE(contains(v, "thread expected"));
  REQUIRE(contains(v, "true | truesuspended"));
  REQUIRE(contains(v, "cannot close a running coroutine"));
  REQUIRE(contains(v, "boom"));
  REQUIRE(contains(v, "| true | 123 |"));
  REQUIRE(contains(v, "attempt to compare"));
  REQUIRE(contains(v, "1,9,2,3,7/1/nil"));
  REQUIRE(contains(v, "position out of bounds"));
}
