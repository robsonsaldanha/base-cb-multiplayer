#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "cbscript/json_lua.hpp"
#include "test_util.hpp"

using cbscript::JsonOptions;
using cbscript::jsonToLua;
using cbscript::luaToJson;

namespace {

struct Lua {
  sol::state s;
  Lua() { s.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::coroutine); }
  lua_State* L() { return s.lua_state(); }
};

// JSON → Lua → JSON. ok=false com o erro da etapa que falhou.
struct Rt {
  bool ok = false;
  std::string out, err;
};
Rt roundTrip(lua_State* L, const std::string& in, const JsonOptions& o = {}) {
  Rt r;
  int top = lua_gettop(L);
  if (!jsonToLua(L, in, r.err, o)) {
    r.err = "decode: " + r.err;
    return r;
  }
  r.ok = luaToJson(L, -1, r.out, r.err, o);
  lua_settop(L, top);
  return r;
}

// Avalia `code` (que retorna 1 valor) e serializa.
Rt encode(Lua& lua, const std::string& code, const JsonOptions& o = {}) {
  Rt r;
  sol::protected_function fn = lua.s.load(code);
  sol::protected_function_result res = fn();
  REQUIRE(res.valid());
  sol::object v = res;
  v.push(lua.L());
  r.ok = luaToJson(lua.L(), -1, r.out, r.err, o);
  lua_pop(lua.L(), 1);
  return r;
}

std::string nested(int depth) {
  std::string s;
  for (int i = 0; i < depth; ++i) s += "[";
  for (int i = 0; i < depth; ++i) s += "]";
  return s;
}

}  // namespace

TEST_CASE("json_lua: nested tables and arrays round trip") {
  Lua lua;
  const std::string in =
      R"({"a":1,"b":[1,2,{"c":"x","d":[true,false]}],"e":{"f":{"g":"h"}},"s":"","n":-7,"neg":-2.5})";
  Rt r = roundTrip(lua.L(), in);
  INFO(r.err);
  REQUIRE(r.ok);
  REQUIRE(nlohmann::json::parse(r.out) == nlohmann::json::parse(in));
  // a tabela decodificada é navegável pelo script
  REQUIRE(jsonToLua(lua.L(), in, r.err));
  lua.s["t"] = sol::stack::pop<sol::object>(lua.L());
  REQUIRE(lua.s.script("return t.b[3].d[1] == true and #t.b == 3 and t.e.f.g == 'h' and t.neg == -2.5")
              .get<bool>());
}

TEST_CASE("json_lua: integers and floats are preserved") {
  Lua lua;
  Rt r = roundTrip(lua.L(), R"([1,1.0,-0.5,1e300,9007199254740993,-9223372036854775808,0,2.5e-8])");
  INFO(r.err);
  REQUIRE(r.ok);
  auto j = nlohmann::json::parse(r.out);
  REQUIRE(j[0].is_number_integer());
  REQUIRE(j[1].is_number_float());
  REQUIRE(j[1].get<double>() == 1.0);
  REQUIRE(j[2].get<double>() == -0.5);
  REQUIRE(j[3].is_number_float());
  REQUIRE(j[3].get<double>() == 1e300);
  REQUIRE(j[4].is_number_integer());
  REQUIRE(j[4].get<int64_t>() == 9007199254740993LL);
  REQUIRE(j[5].get<int64_t>() == INT64_MIN);
  REQUIRE(j[6].is_number_integer());
  REQUIRE(j[7].get<double>() == 2.5e-8);
  // tipos no lado Lua
  std::string err;
  REQUIRE(jsonToLua(lua.L(), "[3, 3.0, 18446744073709551615]", err));
  lua.s["t"] = sol::stack::pop<sol::object>(lua.L());
  REQUIRE(lua.s.script("return math.type(t[1]) == 'integer' and math.type(t[2]) == 'float' "
                       "and math.type(t[3]) == 'float'")
              .get<bool>());
  // Lua → JSON: float inteiro sai com ".0"
  REQUIRE(encode(lua, "return {2.0, 2, -0.0}").out == "[2.0,2,-0.0]");
}

TEST_CASE("json_lua: unicode strings round trip and invalid UTF-8 is rejected") {
  Lua lua;
  const std::string in = "{\"k\\u00e9\":\"ol\xC3\xA1 \xF0\x9F\x98\x80 \\u2603 \\ud83d\\ude00\",\"z\":\"a\\u0000b\\n\\\"\\\\\"}";
  Rt r = roundTrip(lua.L(), in);
  INFO(r.err);
  REQUIRE(r.ok);
  REQUIRE(nlohmann::json::parse(r.out) == nlohmann::json::parse(in));
  std::string err;
  REQUIRE(jsonToLua(lua.L(), in, err));
  lua.s["t"] = sol::stack::pop<sol::object>(lua.L());
  REQUIRE(lua.s.script("return t['k\\u{e9}'] ~= nil and #t.z == 6 and t.z:byte(2) == 0").get<bool>());
  // controle e aspas escapados na saída
  REQUIRE(encode(lua, "return 'a\\1\"\\\\\\n'").out == "\"a\\u0001\\\"\\\\\\n\"");
  // UTF-8 inválido do lado Lua: sequência truncada, overlong, surrogate, acima de U+10FFFF
  for (const char* bad : {"return '\\xC3'", "return '\\xC0\\xAF'", "return '\\xED\\xA0\\x80'",
                          "return '\\xF4\\x90\\x80\\x80'", "return {['\\xFF'] = 1}"}) {
    INFO(bad);
    Rt e = encode(lua, bad);
    REQUIRE_FALSE(e.ok);
    REQUIRE(contains(e.err, "UTF-8"));
  }
  // UTF-8 inválido no JSON de entrada
  REQUIRE_FALSE(jsonToLua(lua.L(), "\"\xC3\"", err));
  REQUIRE_FALSE(err.empty());
}

TEST_CASE("json_lua: depth limit is 32 in both directions") {
  Lua lua;
  Rt ok = roundTrip(lua.L(), nested(32));
  INFO(ok.err);
  REQUIRE(ok.ok);
  REQUIRE(ok.out == nested(32));
  std::string err;
  int top = lua_gettop(lua.L());
  REQUIRE_FALSE(jsonToLua(lua.L(), nested(33), err));
  REQUIRE(lua_gettop(lua.L()) == top);
  REQUIRE(contains(err, "profundidade"));
  Rt e = encode(lua, "local t = {} ; local c = t ; for i = 1, 32 do c[1] = {} ; c = c[1] end ; return t");
  REQUIRE_FALSE(e.ok);
  REQUIRE(contains(e.err, "profundidade"));
  REQUIRE(encode(lua, "local t = {} ; local c = t ; for i = 1, 31 do c[1] = {} ; c = c[1] end ; return t").ok);
  // ciclo é recusado (não laço infinito)
  Rt cyc = encode(lua, "local t = {} ; t.self = t ; return t");
  REQUIRE_FALSE(cyc.ok);
  REQUIRE(contains(cyc.err, "ciclo"));
}

TEST_CASE("json_lua: payloads over 4096 bytes are rejected in both directions") {
  Lua lua;
  std::string atLimit = "\"" + std::string(4094, 'a') + "\"";
  REQUIRE(atLimit.size() == 4096);
  Rt ok = roundTrip(lua.L(), atLimit);
  INFO(ok.err);
  REQUIRE(ok.ok);
  REQUIRE(ok.out == atLimit);
  std::string err;
  REQUIRE_FALSE(jsonToLua(lua.L(), "\"" + std::string(4095, 'a') + "\"", err));
  REQUIRE(contains(err, "4096"));
  Rt big = encode(lua, "return string.rep('a', 4095)");
  REQUIRE_FALSE(big.ok);
  REQUIRE(contains(big.err, "4096"));
  REQUIRE(encode(lua, "return string.rep('a', 4094)").ok);
  // tabela enorme é recusada sem varrer tudo
  Rt huge = encode(lua, "local t = {} ; for i = 1, 200000 do t[i] = i end ; return t");
  REQUIRE_FALSE(huge.ok);
  REQUIRE(contains(huge.err, "4096"));
  // limite configurável
  JsonOptions small;
  small.maxBytes = 8;
  REQUIRE_FALSE(roundTrip(lua.L(), "[1,2,3,4,5]", small).ok);
}

TEST_CASE("json_lua: table shapes, scalars and unsupported values") {
  Lua lua;
  REQUIRE(encode(lua, "return {}").out == "[]");
  REQUIRE(encode(lua, "return {1, 'a', true}").out == "[1,\"a\",true]");
  REQUIRE(encode(lua, "return nil").out == "null");
  REQUIRE(encode(lua, "return false").out == "false");
  REQUIRE(nlohmann::json::parse(encode(lua, "return {[1] = 'a', [3] = 'c'}").out) ==
          nlohmann::json::parse(R"({"1":"a","3":"c"})"));
  REQUIRE(nlohmann::json::parse(encode(lua, "return {x = 1, [2] = 'b'}").out) ==
          nlohmann::json::parse(R"({"x":1,"2":"b"})"));
  for (const char* bad : {"return {[1.5] = 1}", "return {[true] = 1}", "return {[{}] = 1}", "return print",
                          "return {f = print}", "return coroutine.create(print)", "return 0/0", "return 1/0",
                          "return {-math.huge}"}) {
    INFO(bad);
    REQUIRE_FALSE(encode(lua, bad).ok);
  }
  // metamétodos não são consultados (acesso cru)
  REQUIRE(encode(lua, "return setmetatable({}, {__index = function() error('x') end, __pairs = error, "
                      "__len = function() return 5 end})")
              .out == "[]");
  // escalares no topo
  std::string err;
  for (const char* in : {"true", "null", "\"x\"", "12", "1.5"}) {
    INFO(in);
    Rt r = roundTrip(lua.L(), in);
    REQUIRE(r.ok);
    REQUIRE(r.out == in);
  }
  // JSON malformado
  for (const char* bad : {"", "{", "[1,]", "{'a':1}", "nan", "[1] x"}) {
    INFO(bad);
    int top = lua_gettop(lua.L());
    REQUIRE_FALSE(jsonToLua(lua.L(), bad, err));
    REQUIRE(lua_gettop(lua.L()) == top);
  }
}

TEST_CASE("json_lua: null sentinel maps both ways") {
  Lua lua;
  lua_newtable(lua.L());
  lua_pushvalue(lua.L(), -1);
  lua.s["NULL"] = sol::stack::pop<sol::object>(lua.L());
  JsonOptions o;
  o.nullRef = luaL_ref(lua.L(), LUA_REGISTRYINDEX);
  Rt r = roundTrip(lua.L(), "[1,null,3,{\"a\":null}]", o);
  INFO(r.err);
  REQUIRE(r.ok);
  REQUIRE(r.out == "[1,null,3,{\"a\":null}]");
  std::string err;
  REQUIRE(jsonToLua(lua.L(), "[null]", err, o));
  lua.s["t"] = sol::stack::pop<sol::object>(lua.L());
  REQUIRE(lua.s.script("return t[1] == NULL and #t == 1").get<bool>());
  // sem sentinela: null some da tabela
  REQUIRE(jsonToLua(lua.L(), "{\"a\":null,\"b\":1}", err));
  lua.s["u"] = sol::stack::pop<sol::object>(lua.L());
  REQUIRE(lua.s.script("return u.a == nil and u.b == 1").get<bool>());
}

TEST_CASE("json_lua: a lightuserdata null sentinel maps both ways") {
  Lua lua;
  static char tag = 0;
  lua_pushlightuserdata(lua.L(), &tag);
  JsonOptions o;
  o.nullRef = luaL_ref(lua.L(), LUA_REGISTRYINDEX);
  Rt r = roundTrip(lua.L(), "[1,null,{\"a\":null},null]", o);
  INFO(r.err);
  REQUIRE(r.ok);
  REQUIRE(r.out == "[1,null,{\"a\":null},null]");
  // Outro lightuserdata não é o sentinela.
  static char other = 0;
  lua_pushlightuserdata(lua.L(), &other);
  std::string out, err;
  REQUIRE_FALSE(luaToJson(lua.L(), -1, out, err, o));
  lua_pushlightuserdata(lua.L(), &tag);
  REQUIRE(luaToJson(lua.L(), -1, out, err, o));
  REQUIRE(out == "null");
  lua_pop(lua.L(), 2);
}

namespace {
char g_decodeErr[256];
CBSCRIPT_TEST_NOINLINE int decodeInto(lua_State* L, const char* s, size_t n) {
  std::string err;
  if (jsonToLua(L, std::string_view(s, n), err)) return 1;
  std::snprintf(g_decodeErr, sizeof g_decodeErr, "%s", err.c_str());
  return 0;
}
// Função C exposta ao script: decode(s) → valor ou (nil, erro). Sem objetos C++ neste quadro.
int luaDecode(lua_State* L) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, 1, &n);
  if (decodeInto(L, s, n) == 1) return 1;
  lua_pushnil(L);
  lua_pushstring(L, g_decodeErr);
  return 2;
}
}  // namespace

TEST_CASE("json_lua: decode inside a script call under the memory cap fails cleanly") {
  Fixture f(shortLimits(size_t{4} << 20));
  lua_State* L = f.r->lua().lua_state();
  lua_pushglobaltable(L);
  lua_pushstring(L, "decode");
  lua_pushcfunction(L, luaDecode);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  REQUIRE(f.isTrue("local t = decode('{\"a\":[1,2,{\"b\":\"c\"}]}') ; return t.a[3].b == 'c'"));
  // enche o estado até o teto e tenta decodificar algo com muitas tabelas
  std::string payload = "[";
  for (int i = 0; i < 1364; ++i) payload += (i ? ",[]" : "[]");  // 1364 tabelas, ~90 KiB no Lua
  payload += "]";
  REQUIRE(payload.size() <= 4096);
  auto res = guarded("decode at cap", [&] {
    return runRaw(*f.r, "local hold = {} ; while true do local ok = pcall(function() hold[#hold + 1] = "
                        "string.rep('x', 1024) end) ; if not ok then break end end ; "
                        "local v, e = decode('" + payload + "') ; hold = nil ; return v == nil and e or 'decoded'");
  });
  INFO(res.res.error);
  REQUIRE(res.res.ok);
  REQUIRE(res.res.value.is<std::string>());
  INFO(res.res.value.as<std::string>());
  REQUIRE(contains(res.res.value.as<std::string>(), "mem"));
  // o estado continua utilizável
  REQUIRE(f.isTrue("collectgarbage('step', 0) ; return decode('[1]')[1] == 1"));
}

TEST_CASE("json_lua: shared tables are serialized in each place but scanned once; cycles are rejected") {
  Lua lua;
  // revisão ATK13: tabela compartilhada legítima
  REQUIRE(encode(lua, "local e = {} ; return {e, e}").out == "[[],[]]");
  REQUIRE(encode(lua, "local p = {x = 1} ; return {p, p}").out == R"([{"x":1},{"x":1}])");
  REQUIRE(nlohmann::json::parse(encode(lua, "local p = {x = 1} ; return {a = p, b = p}").out) ==
          nlohmann::json::parse(R"({"a":{"x":1},"b":{"x":1}})"));
  Rt cyc = encode(lua, "local a = {} ; local b = {a} ; a[1] = b ; return {a}");
  REQUIRE_FALSE(cyc.ok);
  REQUIRE(contains(cyc.err, "ciclo"));
  // o limite de profundidade vale no lugar onde a cópia memorizada entra
  const char* deepShared = "local d = {{}} ; local t = {d, {}} ; local c = t[2] ; for i = 1, %d do c[1] = {} ; "
                           "c = c[1] end ; c[1] = d ; return t";
  char buf[256];
  std::snprintf(buf, sizeof buf, deepShared, 28);
  REQUIRE(encode(lua, buf).ok);
  std::snprintf(buf, sizeof buf, deepShared, 29);
  Rt deep = encode(lua, buf);
  REQUIRE_FALSE(deep.ok);
  REQUIRE(contains(deep.err, "profundidade"));
  // Revisão I-1/ATK6: tabela com parte de array enorme e vazia, referenciada 1500 vezes. Antes: 4,7 s / 11,5 s numa
  // chamada C. Agora é varrida uma vez e o resto vem da memória; 1500 × "[]" passa de 4096 bytes.
  for (int n : {1 << 20, 1 << 22}) {
    INFO(n);
    lua.s["N"] = n;
    lua.s.script("E = {} ; for i = 1, N do E[i] = true end ; for i = 1, N do E[i] = nil end ; "
                 "L = {} ; for i = 1, 1500 do L[i] = E end ; S = {} ; for i = 1, 600 do S[i] = E end");
    struct R {
      bool ok, okSmall;
      std::string err, small;
      long long ms;
    };
    R r = guarded("luaToJson shared sparse table", [&] {
      lua_State* L = lua.L();
      R x{};
      auto t0 = std::chrono::steady_clock::now();
      lua_getglobal(L, "L");
      std::string out;
      x.ok = luaToJson(L, -1, out, x.err);
      lua_pop(L, 1);
      lua_getglobal(L, "S");
      x.okSmall = luaToJson(L, -1, x.small, out);
      lua_pop(L, 1);
      x.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      return x;
    });
    REQUIRE_FALSE(r.ok);
    REQUIRE(contains(r.err, "4096"));
    REQUIRE(r.okSmall);
    REQUIRE(r.small.size() == 1 + 600 * 3 - 1 + 1);
    REQUIRE(r.ms < 1000);
  }
}

TEST_CASE("json_lua: numbers out of double range and edge values") {
  Lua lua;
  std::string err;
  int top = lua_gettop(lua.L());
  REQUIRE_FALSE(jsonToLua(lua.L(), "1E400", err));
  REQUIRE(contains(err, "fora do intervalo"));
  REQUIRE_FALSE(jsonToLua(lua.L(), "[1,{\"a\":-1e999}]", err));
  REQUIRE(lua_gettop(lua.L()) == top);
  REQUIRE(roundTrip(lua.L(), "-0").out == "0");
  REQUIRE(roundTrip(lua.L(), "-0.0").out == "-0.0");
  REQUIRE(nlohmann::json::parse(roundTrip(lua.L(), "18446744073709551615").out).is_number_float());
  REQUIRE(nlohmann::json::parse(roundTrip(lua.L(), "-9223372036854775809").out).is_number_float());
  REQUIRE_FALSE(jsonToLua(lua.L(), R"("\ud800")", err));  // surrogate solto (escape JSON)
  // sem sentinela, null em array vira buraco: a volta é um objeto (documentado)
  REQUIRE(nlohmann::json::parse(roundTrip(lua.L(), "[1,null,3]").out) ==
          nlohmann::json::parse(R"({"1":1,"3":3})"));
  REQUIRE(roundTrip(lua.L(), "{}").out == "[]");
}
