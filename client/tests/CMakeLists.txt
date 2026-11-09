add_executable(cbmp_tests
    test_main.cpp
    test_transform.cpp
    test_log.cpp
    test_store.cpp
    test_spike_controller.cpp
)
target_link_libraries(cbmp_tests PRIVATE cbmp_core)
if(MSVC)
    target_compile_options(cbmp_tests PRIVATE /utf-8)
endif()
add_test(NAME cbmp_tests COMMAND cbmp_tests)
