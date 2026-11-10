add_library(cbscript STATIC src/resource.cpp src/json_lua.cpp src/host.cpp src/api.cpp src/bans.cpp src/db.cpp src/db_mysql.cpp src/http_out.cpp)
target_include_directories(cbscript PUBLIC include ${LUA_INCLUDE_DIR})
find_package(httplib CONFIG REQUIRED)
target_link_libraries(cbscript PUBLIC sol2::sol2 ${CBMP_LUA_TARGET} ${CBMP_MARIADB_TARGET} cbmp_proto spdlog::spdlog
  httplib::httplib)
# Lua compilado como C++: cabeçalhos sem extern "C". O sol2 com SOL_USING_CXX_LUA desliga o catch (...) dos seus
# trampolins (pegaria as exceções que o Lua usa para erro e yield); SOL_EXCEPTIONS_SAFE_PROPAGATION fica desligado:
# exceções C++ (std::exception) de funções do sol2 continuam virando erro do Lua em vez de cruzar a VM.
target_compile_definitions(cbscript PUBLIC SOL_USING_CXX_LUA=1)

add_executable(cbscript_tests tests/test_lua_smoke.cpp tests/test_resource.cpp tests/test_escapes.cpp
  tests/bench_resource.cpp tests/test_json_lua.cpp tests/test_host.cpp tests/test_cpu.cpp tests/test_api.cpp tests/test_db.cpp tests/test_bans.cpp tests/test_db_mysql.cpp
  tests/test_http_out.cpp tests/test_docs.cpp)
target_link_libraries(cbscript_tests PRIVATE cbscript Catch2::Catch2WithMain)
cbmp_lua_stack(cbscript_tests)
# Exemplos do guia de resources (docs/RESOURCES.md) conferidos por tests/test_docs.cpp.
target_compile_definitions(cbscript_tests PRIVATE CBMP_RESOURCES_DOC="${CMAKE_SOURCE_DIR}/docs/RESOURCES.md")
cbmp_copy_mariadb_plugins(cbscript_tests)
# Prazo por teste: casos de fuga da vigia já travaram; regressão deve falhar, não pendurar o CI.
catch_discover_tests(cbscript_tests PROPERTIES TIMEOUT 60)
