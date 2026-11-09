find_package(unofficial-sodium CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(cbmp_proto STATIC)
target_include_directories(cbmp_proto PUBLIC include)
target_sources(cbmp_proto PRIVATE src/bitstream.cpp src/messages.cpp src/base64url.cpp src/ticket.cpp)
target_link_libraries(cbmp_proto PUBLIC unofficial-sodium::sodium nlohmann_json::nlohmann_json)

add_executable(cbmp_proto_tests tests/test_bitstream.cpp tests/test_messages.cpp tests/test_base64url.cpp tests/test_ticket.cpp)
target_link_libraries(cbmp_proto_tests PRIVATE cbmp_proto Catch2::Catch2WithMain)
target_compile_definitions(cbmp_proto_tests PRIVATE CBMP_VECTORS="${CMAKE_SOURCE_DIR}/packages/contracts/test-vectors/tickets.json")
catch_discover_tests(cbmp_proto_tests)
