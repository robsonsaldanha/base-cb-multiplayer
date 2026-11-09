#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include <cbmp/base64url.hpp>
using namespace cbmp;

TEST_CASE("base64url canonico") {
  CHECK(base64urlDecodeCanonical("AQID") == std::vector<uint8_t>{1, 2, 3});
  CHECK_FALSE(base64urlDecodeCanonical("AQI="));   // padding
  CHECK_FALSE(base64urlDecodeCanonical("AQ+D"));   // charset
  CHECK_FALSE(base64urlDecodeCanonical("AQIDA"));  // len%4==1
  CHECK_FALSE(base64urlDecodeCanonical("AR"));     // bits sobrando não-zero (canônico seria "AQ")
  CHECK(base64urlEncode(std::vector<uint8_t>{1, 2, 3}) == "AQID");
}

TEST_CASE("base64url ida e volta e bordas") {
  CHECK(base64urlDecodeCanonical("") == std::vector<uint8_t>{});
  CHECK(base64urlEncode(std::vector<uint8_t>{}).empty());
  CHECK(base64urlDecodeCanonical("AQ") == std::vector<uint8_t>{1});
  CHECK(base64urlDecodeCanonical("AQI") == std::vector<uint8_t>{1, 2});
  CHECK_FALSE(base64urlDecodeCanonical("AQJ"));    // bits sobrando não-zero
  CHECK_FALSE(base64urlDecodeCanonical("A"));      // len%4==1
  CHECK_FALSE(base64urlDecodeCanonical("AQ.D"));
  CHECK_FALSE(base64urlDecodeCanonical("AQ D"));
  CHECK_FALSE(base64urlDecodeCanonical(std::string_view("AQ\0D", 4)));
  CHECK(base64urlEncode(std::vector<uint8_t>{0xfb, 0xff}) == "-_8");
  CHECK(base64urlDecodeCanonical("-_8") == std::vector<uint8_t>{0xfb, 0xff});
  std::vector<uint8_t> all(256);
  for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint8_t>(i);
  for (size_t n = 0; n <= all.size(); ++n) {
    std::vector<uint8_t> v(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n));
    const auto enc = base64urlEncode(v);
    CHECK(enc.size() == (n * 4 + 2) / 3);
    CHECK(base64urlDecodeCanonical(enc) == v);
  }
}
