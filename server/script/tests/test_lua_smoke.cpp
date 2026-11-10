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
