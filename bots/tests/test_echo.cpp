#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "echo.hpp"

using Catch::Approx;

TEST_CASE("bots echo payload is JSON with the send time in microseconds") {
  auto p = cbbots::encodeEcho(12.3456789);
  REQUIRE(std::string(p.begin(), p.end()) == R"({"t":12345679})");
  auto t = cbbots::decodeEcho(p);
  REQUIRE(t);
  REQUIRE(*t == Approx(12.345679));
}

TEST_CASE("bots echo decode rejects other payloads") {
  auto bytes = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
  REQUIRE_FALSE(cbbots::decodeEcho(bytes("")));
  REQUIRE_FALSE(cbbots::decodeEcho(bytes("12345678")));  // formato binário antigo
  REQUIRE_FALSE(cbbots::decodeEcho(bytes(R"({"t":-5})")));
  REQUIRE_FALSE(cbbots::decodeEcho(bytes(R"({"t":"x"})")));
  REQUIRE_FALSE(cbbots::decodeEcho(bytes(R"({"t":12)")));
  REQUIRE(cbbots::decodeEcho(bytes(R"({"t":0})")));
}
