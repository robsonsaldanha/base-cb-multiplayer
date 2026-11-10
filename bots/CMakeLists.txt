find_package(unofficial-sodium CONFIG REQUIRED)

add_executable(cb-bots src/main.cpp src/bot.cpp)
target_link_libraries(cb-bots PRIVATE cbmp_proto unofficial-sodium::sodium
  ${CBMP_GNS_TARGET})
if(WIN32)
  target_link_libraries(cb-bots PRIVATE winmm)
endif()

add_executable(cbbots_tests tests/test_gaps.cpp tests/test_echo.cpp tests/test_ticket.cpp src/bot.cpp)
target_include_directories(cbbots_tests PRIVATE src)
target_link_libraries(cbbots_tests PRIVATE cbmp_proto unofficial-sodium::sodium ${CBMP_GNS_TARGET}
  Catch2::Catch2WithMain)
if(WIN32)
  target_link_libraries(cbbots_tests PRIVATE winmm)
endif()
catch_discover_tests(cbbots_tests)
