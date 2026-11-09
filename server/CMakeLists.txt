find_package(spdlog CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

add_library(cbserver_core STATIC src/config.cpp src/metrics.cpp src/manifest.cpp src/http.cpp)
target_include_directories(cbserver_core PUBLIC include)
target_link_libraries(cbserver_core PUBLIC cbmp_proto spdlog::spdlog httplib::httplib)
if(WIN32)
  target_link_libraries(cbserver_core PUBLIC psapi)
endif()

add_executable(cb-server src/main.cpp)
target_link_libraries(cb-server PRIVATE cbserver_core)

add_executable(cbserver_tests tests/test_config.cpp tests/test_metrics.cpp tests/test_manifest.cpp tests/test_http.cpp)
target_link_libraries(cbserver_tests PRIVATE cbserver_core Catch2::Catch2WithMain)
target_compile_definitions(cbserver_tests PRIVATE CBSERVER_CFG_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/server.cfg.example")
catch_discover_tests(cbserver_tests)
