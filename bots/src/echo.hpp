#pragma once
// Payload do evento 'echo' dos bots: JSON {"t":<µs desde o início do bot>}. O resource Lua `echo` devolve o mesmo
// JSON; inteiro (e não double) para a ida e volta pelo Lua ser exata.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cbbots {

inline std::vector<uint8_t> encodeEcho(double tSec) {
  const auto us = static_cast<long long>(std::llround(std::max(0.0, tSec) * 1e6));
  const std::string s = "{\"t\":" + std::to_string(us) + "}";
  return {s.begin(), s.end()};
}

inline std::optional<double> decodeEcho(const std::vector<uint8_t>& p) {
  const std::string s(p.begin(), p.end());
  constexpr const char* kPrefix = "{\"t\":";
  if (s.size() < 7 || s.compare(0, 5, kPrefix) != 0 || s.back() != '}') return std::nullopt;
  long long us = 0;
  size_t i = 5;
  if (i >= s.size() - 1) return std::nullopt;
  for (; i < s.size() - 1; ++i) {
    if (s[i] < '0' || s[i] > '9' || us > 1'000'000'000'000'000LL) return std::nullopt;
    us = us * 10 + (s[i] - '0');
  }
  return static_cast<double>(us) / 1e6;
}

}  // namespace cbbots
