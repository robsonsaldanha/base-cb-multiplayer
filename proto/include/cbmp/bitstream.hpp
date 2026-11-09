#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cbmp {
struct DecodeError : std::runtime_error { using std::runtime_error::runtime_error; };

// Bitstream LSB-first: o primeiro bit escrito é o bit 0 do byte 0.
class BitWriter {
public:
  void writeBits(uint64_t value, unsigned bits);
  void writeU8(uint8_t v) { writeBits(v, 8); }
  void writeU16(uint16_t v) { writeBits(v, 16); }
  void writeU32(uint32_t v) { writeBits(v, 32); }
  void writeU64(uint64_t v) { writeBits(v, 64); }
  void writeI16(int16_t v) { writeBits(static_cast<uint16_t>(v), 16); }
  void writeI32(int32_t v) { writeBits(static_cast<uint32_t>(v), 32); }
  void writeBool(bool b) { writeBits(b ? 1 : 0, 1); }
  void writeString(std::string_view s, unsigned lenBits, size_t maxLen);
  void writeBlob(std::span<const uint8_t> b, unsigned lenBits, size_t maxLen);
  std::vector<uint8_t> finish();
  size_t bitSize() const { return bits_; }
private:
  std::vector<uint8_t> buf_;
  size_t bits_ = 0;
};

class BitReader {
public:
  explicit BitReader(std::span<const uint8_t> data) : data_(data) {}
  uint64_t readBits(unsigned bits);
  uint8_t readU8() { return static_cast<uint8_t>(readBits(8)); }
  uint16_t readU16() { return static_cast<uint16_t>(readBits(16)); }
  uint32_t readU32() { return static_cast<uint32_t>(readBits(32)); }
  uint64_t readU64() { return readBits(64); }
  int16_t readI16() { return static_cast<int16_t>(readU16()); }
  int32_t readI32() { return static_cast<int32_t>(readU32()); }
  bool readBool() { return readBits(1) != 0; }
  std::string readString(unsigned lenBits, size_t maxLen);
  std::vector<uint8_t> readBlob(unsigned lenBits, size_t maxLen);
  size_t remainingBits() const { return data_.size() * 8 - pos_; }
  // Só bits de preenchimento (zero) até o fim do byte atual; nada depois.
  void expectEnd() const;
private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};
}  // namespace cbmp
