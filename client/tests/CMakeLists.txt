add_executable(cbmp_client_tests
    test_transform.cpp
    test_log.cpp
    test_store.cpp
    test_mod_config.cpp
    test_limits.cpp
    test_guard.cpp
    test_connect_file.cpp
    test_claim_policy.cpp
    test_net_client.cpp
    test_snapshot_mirror.cpp
    test_mirror_e2e.cpp
    test_remote_store.cpp
    test_world_tracker.cpp
    test_local_sampler.cpp
    test_send_scheduler.cpp
    test_interp.cpp
    test_puppet_director.cpp
    test_session.cpp
    test_session_errors.cpp
    test_session_fixes.cpp
)
target_link_libraries(cbmp_client_tests PRIVATE cbmp_client_core Catch2::Catch2WithMain)
# Pasta do núcleo para o teste de guarda (test_guard.cpp), em caminho absoluto (UTF-8 com /utf-8 no MSVC).
cmake_path(SET _cbmp_client_core_dir NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../core")
target_compile_definitions(cbmp_client_tests PRIVATE CBMP_CLIENT_CORE_DIR="${_cbmp_client_core_dir}")
catch_discover_tests(cbmp_client_tests)
