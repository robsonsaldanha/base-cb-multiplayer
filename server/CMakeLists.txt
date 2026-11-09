find_package(spdlog CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)

add_library(cbserver_core STATIC src/config.cpp src/metrics.cpp src/manifest.cpp src/http.cpp
  src/grid.cpp src/visibility.cpp src/movement.cpp src/world.cpp src/snapshots.cpp src/pool.cpp
  src/auth.cpp src/net.cpp)
target_include_directories(cbserver_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(cbserver_core PUBLIC cbmp_proto spdlog::spdlog httplib::httplib Threads::Threads
  GameNetworkingSockets::GameNetworkingSockets)
if(WIN32)
  target_link_libraries(cbserver_core PUBLIC psapi)
endif()

add_executable(cb-server src/main.cpp)
target_link_libraries(cb-server PRIVATE cbserver_core)

add_executable(cbserver_tests tests/test_config.cpp tests/test_metrics.cpp tests/test_manifest.cpp tests/test_http.cpp
  tests/test_grid.cpp tests/test_visibility.cpp tests/test_rate.cpp tests/test_movement.cpp tests/test_world.cpp
  tests/test_snapshots.cpp tests/test_auth.cpp tests/test_net.cpp)
target_link_libraries(cbserver_tests PRIVATE cbserver_core Catch2::Catch2WithMain)
target_compile_definitions(cbserver_tests PRIVATE CBSERVER_CFG_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/server.cfg.example")
catch_discover_tests(cbserver_tests)
