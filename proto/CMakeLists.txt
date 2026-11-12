find_package(unofficial-sodium CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Codec (bitstream + messages), sem libsodium nem nlohmann_json: usado também pelo cliente (client/core).
add_library(cbmp_codec STATIC src/bitstream.cpp src/messages.cpp)
target_include_directories(cbmp_codec PUBLIC include)

add_library(cbmp_proto STATIC)
target_include_directories(cbmp_proto PUBLIC include)
target_sources(cbmp_proto PRIVATE src/base64url.cpp src/ticket.cpp)
target_link_libraries(cbmp_proto PUBLIC cbmp_codec unofficial-sodium::sodium nlohmann_json::nlohmann_json)

add_executable(cbmp_proto_tests tests/test_bitstream.cpp tests/test_messages.cpp tests/test_snapshot.cpp tests/test_base64url.cpp tests/test_ticket.cpp)
target_link_libraries(cbmp_proto_tests PRIVATE cbmp_proto Catch2::Catch2WithMain)
target_compile_definitions(cbmp_proto_tests PRIVATE CBMP_VECTORS="${CMAKE_SOURCE_DIR}/packages/contracts/test-vectors/tickets.json")
catch_discover_tests(cbmp_proto_tests)

# Fuzz: proto_fuzz_replay roda em toda plataforma (sementes + mutações determinísticas, no ctest);
# proto_fuzz (libFuzzer) só com CBMP_BUILD_FUZZ=ON e clang (preset linux-fuzz).
# Os dois alvos compilam só o codec (bitstream + messages): o ticket usa <expected>, que o clang 18 com a libstdc++
# do Ubuntu 24.04 não oferece, e o fuzz não precisa dele.
set(CBMP_CODEC_SRC src/bitstream.cpp src/messages.cpp)
add_executable(proto_fuzz_replay fuzz/proto_fuzz.cpp fuzz/fuzz_replay.cpp ${CBMP_CODEC_SRC})
target_include_directories(proto_fuzz_replay PRIVATE include)
add_test(NAME proto_fuzz_replay COMMAND proto_fuzz_replay)

if(CBMP_BUILD_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
    message(FATAL_ERROR "CBMP_BUILD_FUZZ exige clang (libFuzzer); use o preset linux-fuzz")
  endif()
  # Codec compilado de novo, instrumentado (cbmp_proto continua normal para os demais alvos).
  set(CBMP_SAN -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_executable(proto_fuzz fuzz/proto_fuzz.cpp ${CBMP_CODEC_SRC})
  target_include_directories(proto_fuzz PRIVATE include)
  target_compile_options(proto_fuzz PRIVATE ${CBMP_SAN})
  target_link_options(proto_fuzz PRIVATE ${CBMP_SAN})
endif()
