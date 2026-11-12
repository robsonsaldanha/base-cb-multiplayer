#include <cbmp/client/GameGlue.hpp>

namespace cbmp::client {
MoveMode MoveModeFromMovementMode(uint8_t movementMode) {
    switch (movementMode) {
        case 1:
        case 2: return MoveMode::Ground;
        case 3: return MoveMode::Falling;
        case 4: return MoveMode::Swimming;
        case 5: return MoveMode::Flying;
        default: return MoveMode::Other;
    }
}

uint64_t ObjectKey(int32_t index, int32_t serial) {
    if (serial == 0 || index < 0) return 0;
    return (static_cast<uint64_t>(static_cast<uint32_t>(serial)) << 32) | static_cast<uint32_t>(index);
}

std::u16string Utf8ToUtf16(std::string_view in) {
    constexpr char16_t kBad = u'\uFFFD';
    std::u16string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const auto b0 = static_cast<uint8_t>(in[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<char16_t>(b0));
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        uint32_t min = 0;
        if ((b0 & 0xE0) == 0xC0) {
            len = 2, cp = b0 & 0x1F, min = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3, cp = b0 & 0x0F, min = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4, cp = b0 & 0x07, min = 0x10000;
        } else {
            out.push_back(kBad);  // continuação solta ou byte que não começa sequência
            ++i;
            continue;
        }
        size_t n = 1;
        for (; n < len && i + n < in.size(); ++n) {
            const auto b = static_cast<uint8_t>(in[i + n]);
            if ((b & 0xC0) != 0x80) break;
            cp = (cp << 6) | (b & 0x3F);
        }
        if (n < len) {  // cortada: troca o que veio e segue do primeiro byte que não é continuação
            out.push_back(kBad);
            i += n;
            continue;
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            for (size_t k = 0; k < len; ++k) out.push_back(kBad);  // um U+FFFD por byte
            i += len;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return out;
}
}  // namespace cbmp::client
