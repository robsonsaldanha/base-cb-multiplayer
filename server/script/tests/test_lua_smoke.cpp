#include <catch2/catch_test_macros.hpp>
#include <sol/sol.hpp>
#include <mysql.h>
#include <string>

TEST_CASE("lua smoke: estado sandbox abre bibliotecas basicas") {
  sol::state lua;
  lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::utf8,
                     sol::lib::coroutine);
  std::string v = lua["_VERSION"];
  REQUIRE(v == "Lua 5.4");
  int r = lua.script("return 1+1");
  REQUIRE(r == 2);
}

TEST_CASE("libmariadb smoke: cliente linkado") {
  const char* info = mysql_get_client_info();
  REQUIRE(info != nullptr);
  REQUIRE(std::string(info).size() > 0);
}

namespace {
int gLuaCxxDtors = 0;
bool gLuaCxxCaught = false;
struct CountDtor {
  ~CountDtor() { ++gLuaCxxDtors; }
};
int raiseThroughCxxFrame(lua_State* L) {
  CountDtor guard;
  std::string alive = "objeto C++ vivo no quadro";  // destrutor não trivial atravessado pelo erro
  try {
    return luaL_error(L, "erro %s", alive.c_str());
  } catch (...) {
    gLuaCxxCaught = true;  // só um Lua compilado como C++ lança exceção aqui (C faria longjmp por cima)
    throw;
  }
}
}  // namespace

// I2 da revisão final do sub3: o Lua precisa ser compilado como C++ (LUAI_THROW = throw), senão lua_error faz
// longjmp por cima de quadros C++ (comportamento indefinido; derrubou o processo no MSVC).
TEST_CASE("lua smoke: erros do Lua desenrolam quadros C++ (Lua compilado como C++)") {
  sol::state lua;
  lua_State* L = lua.lua_state();
  gLuaCxxDtors = 0;
  gLuaCxxCaught = false;
  lua_pushcfunction(L, raiseThroughCxxFrame);
  REQUIRE(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  REQUIRE(std::string(lua_tostring(L, -1)) == "erro objeto C++ vivo no quadro");
  lua_pop(L, 1);
  CHECK(gLuaCxxCaught);
  CHECK(gLuaCxxDtors == 1);
}
