#include <cbmp/base64url.hpp>

#include <array>

namespace cbmp {
namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr std::array<int8_t, 256> makeReverse() {
  std::array<int8_t, 256> r{};
  for (auto& x : r) x = -1;
  for (int i = 0; i < 64; ++i) r[static_cast<uint8_t>(kAlphabet[i])] = static_cast<int8_t>(i);
  return r;
}
constexpr auto kReverse = makeReverse();
}  // namespace

std::string base64urlEncode(std::span<const uint8_t> data) {
  std::string out;
  out.reserve((data.size() * 4 + 2) / 3);
  size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    const uint32_t n = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) | data[i + 2];
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }
  const size_t rest = data.size() - i;
  if (rest == 1) {
    const uint32_t n = uint32_t{data[i]} << 16;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
  } else if (rest == 2) {
    const uint32_t n = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
  }
  return out;
}

std::optional<std::vector<uint8_t>> base64urlDecodeCanonical(std::string_view text) {
  if (text.size() % 4 == 1) return std::nullopt;
  std::vector<uint8_t> out;
  out.reserve(text.size() * 3 / 4);
  uint32_t acc = 0;
  unsigned bits = 0;
  for (const char ch : text) {
    const int8_t v = kReverse[static_cast<uint8_t>(ch)];
    if (v < 0) return std::nullopt;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>(acc >> bits));
      acc &= (1u << bits) - 1;
    }
  }
  // Bits sobrando (2 ou 4) precisam ser zero; é o mesmo que exigir encode(decode(x)) == x.
  if (acc != 0 || base64urlEncode(out) != text) return std::nullopt;
  return out;
}
}  // namespace cbmp
