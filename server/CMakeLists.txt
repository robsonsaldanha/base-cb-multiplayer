find_package(spdlog CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

add_library(cbserver_core STATIC src/config.cpp src/metrics.cpp src/manifest.cpp src/http.cpp
  src/grid.cpp src/visibility.cpp src/movement.cpp src/world.cpp src/snapshots.cpp src/pool.cpp
  src/auth.cpp src/net.cpp src/hub.cpp)
target_include_directories(cbserver_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(cbserver_core PUBLIC cbmp_proto spdlog::spdlog httplib::httplib Threads::Threads
  ${CBMP_GNS_TARGET})
if(WIN32)
  target_link_libraries(cbserver_core PUBLIC psapi)
endif()

# Ponte World ↔ ScriptHost (separada do core para o core não depender do Lua).
add_library(cbserver_script STATIC src/script_bridge.cpp)
target_link_libraries(cbserver_script PUBLIC cbserver_core cbscript)

add_subdirectory(admin)

add_executable(cb-server src/main.cpp)
target_link_libraries(cb-server PRIVATE cbserver_core cbserver_script cbadmin)
cbmp_copy_mariadb_plugins(cb-server)
cbmp_lua_stack(cb-server)

add_executable(cbserver_tests tests/test_config.cpp tests/test_metrics.cpp tests/test_manifest.cpp tests/test_http.cpp
  tests/test_grid.cpp tests/test_visibility.cpp tests/test_rate.cpp tests/test_movement.cpp tests/test_world.cpp
  tests/test_snapshots.cpp tests/test_auth.cpp tests/test_net.cpp tests/test_hub.cpp tests/test_script_world.cpp)
target_link_libraries(cbserver_tests PRIVATE cbserver_core cbserver_script Catch2::Catch2WithMain)
cbmp_lua_stack(cbserver_tests)
target_compile_definitions(cbserver_tests PRIVATE CBSERVER_CFG_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/server.cfg.example"
  CBMP_EXAMPLE_RESOURCES="${CMAKE_SOURCE_DIR}/resources.example"
  CBMP_ADMIN_ORIGIN_VECTORS="${CMAKE_SOURCE_DIR}/packages/contracts/test-vectors/admin-origin.json")
# Prazo por teste: os de integração com a thread de scripts não podem pendurar o CI.
catch_discover_tests(cbserver_tests PROPERTIES TIMEOUT 60)
