#include <cbmp/bitstream.hpp>
#include <utility>

namespace cbmp {
void BitWriter::writeBits(uint64_t value, unsigned bits) {
  if (bits == 0 || bits > 64) throw std::invalid_argument("bits fora de 1..64");
  if (bits < 64) value &= (uint64_t{1} << bits) - 1;
  for (unsigned i = 0; i < bits; ++i, ++bits_) {
    if ((bits_ & 7) == 0) buf_.push_back(0);
    if ((value >> i) & 1) buf_.back() |= static_cast<uint8_t>(1u << (bits_ & 7));
  }
}
void BitWriter::writeString(std::string_view s, unsigned lenBits, size_t maxLen) {
  if (s.size() > maxLen || (lenBits < 64 && s.size() >= (size_t{1} << lenBits))) throw std::length_error("string grande demais");
  writeBits(s.size(), lenBits);
  for (unsigned char c : s) writeU8(c);
}
void BitWriter::writeBlob(std::span<const uint8_t> b, unsigned lenBits, size_t maxLen) {
  if (b.size() > maxLen || (lenBits < 64 && b.size() >= (size_t{1} << lenBits))) throw std::length_error("blob grande demais");
  writeBits(b.size(), lenBits);
  for (uint8_t c : b) writeU8(c);
}
std::vector<uint8_t> BitWriter::finish() { return std::move(buf_); }

uint64_t BitReader::readBits(unsigned bits) {
  if (bits == 0 || bits > 64) throw DecodeError("bits fora de 1..64");
  if (bits > remainingBits()) throw DecodeError("fim inesperado");
  uint64_t v = 0;
  for (unsigned i = 0; i < bits; ++i, ++pos_) {
    if ((data_[pos_ >> 3] >> (pos_ & 7)) & 1) v |= uint64_t{1} << i;
  }
  return v;
}
std::string BitReader::readString(unsigned lenBits, size_t maxLen) {
  const auto n = readBits(lenBits);
  if (n > maxLen) throw DecodeError("string grande demais");
  if (n * 8 > remainingBits()) throw DecodeError("fim inesperado");
  std::string s(static_cast<size_t>(n), '\0');
  for (auto& c : s) c = static_cast<char>(readU8());
  return s;
}
std::vector<uint8_t> BitReader::readBlob(unsigned lenBits, size_t maxLen) {
  const auto n = readBits(lenBits);
  if (n > maxLen) throw DecodeError("blob grande demais");
  if (n * 8 > remainingBits()) throw DecodeError("fim inesperado");
  std::vector<uint8_t> b(static_cast<size_t>(n));
  for (auto& c : b) c = readU8();
  return b;
}
void BitReader::expectEnd() const {
  const size_t endOfByte = (pos_ + 7) & ~size_t{7};
  if (endOfByte != data_.size() * 8) throw DecodeError("bytes sobrando");
  for (size_t p = pos_; p < endOfByte; ++p)
    if ((data_[p >> 3] >> (p & 7)) & 1) throw DecodeError("preenchimento não-zero");
}
}  // namespace cbmp
