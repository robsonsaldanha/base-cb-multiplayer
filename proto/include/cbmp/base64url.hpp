#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cbmp {
// base64url sem padding (RFC 4648 §5).
std::string base64urlEncode(std::span<const uint8_t> data);

// Decodifica só a forma canônica: charset A-Z a-z 0-9 - _, sem padding, len%4 != 1 e bits
// sobrando zerados (reencodar devolve exatamente a entrada). Qualquer outra coisa → nullopt.
std::optional<std::vector<uint8_t>> base64urlDecodeCanonical(std::string_view text);
}  // namespace cbmp
