#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>
#include <cbmp/bitstream.hpp>
#include <cbmp/quantize.hpp>
using namespace cbmp;

TEST_CASE("bits ida e volta com larguras variadas") {
  BitWriter w;
  w.writeBits(0b101, 3); w.writeBool(true); w.writeU16(0xBEEF); w.writeI32(-123456); w.writeBits(0x1F, 5); w.writeU64(0x0123456789ABCDEFull);
  auto bytes = w.finish();
  BitReader r(bytes);
  CHECK(r.readBits(3) == 0b101); CHECK(r.readBool()); CHECK(r.readU16() == 0xBEEF);
  CHECK(r.readI32() == -123456); CHECK(r.readBits(5) == 0x1F); CHECK(r.readU64() == 0x0123456789ABCDEFull);
}
TEST_CASE("LSB-first: primeiro bit escrito e o bit 0 do byte 0") {
  BitWriter w; w.writeBits(1, 1); w.writeBits(0, 7);
  auto b = w.finish(); REQUIRE(b.size() == 1); CHECK(b[0] == 0x01);
}
TEST_CASE("leitura alem do fim lanca DecodeError") {
  std::vector<uint8_t> one{0xFF}; BitReader r(one);
  r.readBits(8); CHECK_THROWS_AS(r.readBits(1), DecodeError);
}
TEST_CASE("string respeita maxLen na escrita e na leitura") {
  BitWriter w; w.writeString("abc", 8, 255); auto b = w.finish();
  BitReader r(b); CHECK(r.readString(8, 255) == "abc");
  BitWriter w2; CHECK_THROWS(w2.writeString(std::string(300, 'x'), 8, 255));
  BitWriter w3; w3.writeBits(10, 8); w3.writeBits(0, 40); w3.writeBits(0, 40); auto b3 = w3.finish();
  BitReader r3(b3); CHECK_THROWS_AS(r3.readString(8, 5), DecodeError);
}
TEST_CASE("expectEnd tolera so bits de preenchimento zero") {
  BitWriter w; w.writeBits(3, 2); auto b = w.finish();
  BitReader r(b); r.readBits(2); CHECK_NOTHROW(r.expectEnd());
  std::vector<uint8_t> extra{0x03, 0x01}; BitReader r2(extra); r2.readBits(2); CHECK_THROWS_AS(r2.expectEnd(), DecodeError);
}
TEST_CASE("quantizacao de yaw/pitch") {
  CHECK(quantizeYaw(0.f) == 0); CHECK(quantizeYaw(360.f) == 0); CHECK(quantizeYaw(-90.f) == quantizeYaw(270.f));
  CHECK(std::abs(dequantizeYaw(quantizeYaw(123.4f)) - 123.4f) < 0.01f);
  CHECK(quantizePitch(-90.f) == 0); CHECK(quantizePitch(90.f) == 255);
  CHECK(std::abs(dequantizePitch(quantizePitch(10.f)) - 10.f) < 0.8f);
  CHECK(quantizePitch(200.f) == 255);
}
