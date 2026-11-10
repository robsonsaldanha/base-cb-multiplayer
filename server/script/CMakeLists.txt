add_library(cbscript STATIC src/resource.cpp)
target_include_directories(cbscript PUBLIC include ${LUA_INCLUDE_DIR})
target_link_libraries(cbscript PUBLIC sol2::sol2 ${CBMP_LUA_TARGET} ${CBMP_MARIADB_TARGET} cbmp_proto spdlog::spdlog)

add_executable(cbscript_tests tests/test_lua_smoke.cpp tests/test_resource.cpp)
target_link_libraries(cbscript_tests PRIVATE cbscript Catch2::Catch2WithMain)
catch_discover_tests(cbscript_tests)
