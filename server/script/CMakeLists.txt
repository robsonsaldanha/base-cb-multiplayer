add_library(cbscript STATIC src/resource.cpp src/json_lua.cpp src/host.cpp src/api.cpp src/bans.cpp src/db.cpp src/db_mysql.cpp)
target_include_directories(cbscript PUBLIC include ${LUA_INCLUDE_DIR})
target_link_libraries(cbscript PUBLIC sol2::sol2 ${CBMP_LUA_TARGET} ${CBMP_MARIADB_TARGET} cbmp_proto spdlog::spdlog)

add_executable(cbscript_tests tests/test_lua_smoke.cpp tests/test_resource.cpp tests/test_escapes.cpp
  tests/bench_resource.cpp tests/test_json_lua.cpp tests/test_host.cpp tests/test_cpu.cpp tests/test_api.cpp tests/test_db.cpp tests/test_bans.cpp tests/test_db_mysql.cpp)
target_link_libraries(cbscript_tests PRIVATE cbscript Catch2::Catch2WithMain)
cbmp_copy_mariadb_plugins(cbscript_tests)
# Prazo por teste: casos de fuga da vigia já travaram; regressão deve falhar, não pendurar o CI.
catch_discover_tests(cbscript_tests PROPERTIES TIMEOUT 60)
