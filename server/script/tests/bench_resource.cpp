// Micro-benchmark (oculto: rode com `cbscript_tests "[.bench]"`). Mede o custo da vigia em código Lua normal.
#include <catch2/catch_test_macros.hpp>

#include <cstdio>

#include "test_util.hpp"

namespace {

// Núcleo: chamadas de função, tabelas, strings.
constexpr const char* kCore = R"lua(
local function f(a, b) return a + b end
local s = 0
for i = 1, 1e7 do s = f(s, i) end
local t = {}
for i = 1, 1e6 do t[#t + 1] = i end
local parts = {}
for i = 1, 1e5 do parts[i] = tostring(i) end
local str = table.concat(parts, ",")
local n = 0
for w in str:gmatch("%d+") do n = n + 1 end
return s
)lua";

// Biblioteca table (no round 2 insert/remove eram Lua e o sort usava comparador Lua; no round 3, C nativo).
constexpr const char* kTable = R"lua(
local u = {}
for i = 1, 2e5 do table.insert(u, i) end
for i = 1, 2e5 do table.remove(u) end
for i = 1, 2e3 do table.insert(u, 1, i) end
local r = {}
for i = 1, 2e5 do r[i] = (i * 7919) % 200003 end
table.sort(r)
return #r
)lua";

void hookNoop(lua_State*, lua_Debug*) {}

long long runPlain(const char* code, int mask, int count) {
  sol::state lua;
  *static_cast<void**>(lua_getextraspace(lua.lua_state())) = nullptr;  // sem callback de interrupção (patch I1)
  lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);
  if (mask != 0) lua_sethook(lua.lua_state(), hookNoop, mask, count);
  auto t0 = std::chrono::steady_clock::now();
  auto r = lua.safe_script(code);
  REQUIRE(r.valid());
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

long long runResource(const char* code) {
  Limits lim = shortLimits(size_t{256} << 20);
  lim.warnAfter = 60s;
  lim.abortAfter = 120s;
  Fixture f(lim);
  Timed t = runRaw(*f.r, code);
  INFO(t.res.error);
  REQUIRE(t.res.ok);
  return t.ms;
}

}  // namespace

TEST_CASE("bench: watchdog overhead on normal code", "[.bench]") {
  const struct {
    const char* name;
    const char* code;
  } benches[] = {{"core", kCore}, {"table", kTable}};
  for (const auto& b : benches) {
    for (int rep = 0; rep < 3; ++rep) {
      long long none = runPlain(b.code, 0, 0);
      long long call = runPlain(b.code, LUA_MASKCALL, 0);
      long long count = runPlain(b.code, LUA_MASKCOUNT, 10000);
      long long countCall = runPlain(b.code, LUA_MASKCOUNT | LUA_MASKCALL, 10000);
      long long res = runResource(b.code);
      std::printf("BENCH %-5s plain=%lld callHook=%lld countHook=%lld count+call=%lld Resource=%lld (ms)\n", b.name,
                  none, call, count, countCall, res);
    }
  }
}
