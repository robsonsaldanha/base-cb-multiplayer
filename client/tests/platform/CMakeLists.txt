# Testes das portas Win32 (WinConnectFs, WinProcessGate, Arming) contra o sistema real. Só Windows.
add_executable(cbmp_client_platform_tests
    test_win_connect_fs.cpp
    test_win_process_gate.cpp
    test_arming.cpp
)
target_link_libraries(cbmp_client_platform_tests PRIVATE cbmp_client_platform Catch2::Catch2WithMain)
target_compile_definitions(cbmp_client_platform_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
catch_discover_tests(cbmp_client_platform_tests)
