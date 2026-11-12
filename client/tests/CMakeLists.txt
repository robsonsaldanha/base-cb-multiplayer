add_executable(cbmp_client_tests
    test_transform.cpp
    test_log.cpp
    test_store.cpp
    test_mod_config.cpp
    test_limits.cpp
    test_guard.cpp
)
target_link_libraries(cbmp_client_tests PRIVATE cbmp_client_core Catch2::Catch2WithMain)
# Pasta do núcleo para o teste de guarda (test_guard.cpp), em caminho absoluto.
cmake_path(ABSOLUTE_PATH CMAKE_CURRENT_SOURCE_DIR OUTPUT_VARIABLE _cbmp_client_dir)
cmake_path(GET _cbmp_client_dir PARENT_PATH _cbmp_client_dir)
target_compile_definitions(cbmp_client_tests PRIVATE CBMP_CLIENT_CORE_DIR="${_cbmp_client_dir}/core")
catch_discover_tests(cbmp_client_tests)
