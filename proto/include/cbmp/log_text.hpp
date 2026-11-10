#pragma once
// Texto vindo de fora (scripts, clientes, arquivos) para uma linha de log: nada que quebre a linha ou engane quem lê.
#include <string>
#include <string_view>

namespace cbmp {

// Escapa: \n → "\n", \r → "\r", demais controles C0 (exceto tab) e DEL → "\xHH"; controles C1 (U+0080–U+009F),
// separadores U+2028/U+2029, marcas e controles bidi (U+200E/U+200F, U+061C, U+202A–U+202E, U+2066–U+2069) e
// U+FEFF → "\u{XXXX}". Bytes UTF-8 inválidos
// passam como estão (o sink do log decide).
inline std::string escapeLogText(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size());
  auto u = [&](unsigned cp) {
    out += "\\u{";
    for (int sh = 12; sh >= 0; sh -= 4) out += kHex[(cp >> sh) & 15];
    out += '}';
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    const auto c1 = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
    const auto c2 = i + 2 < s.size() ? static_cast<unsigned char>(s[i + 2]) : 0;
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
      out += "\\x";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else if (c == 0xC2 && c1 >= 0x80 && c1 <= 0x9F) {  // C1
      u(c1);
      i += 1;
    } else if (c == 0xE2 && c1 == 0x80 && c2 >= 0xA8 && c2 <= 0xAE) {  // U+2028..U+202E
      u(0x2000u + (c2 - 0x80u));
      i += 2;
    } else if (c == 0xE2 && c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9) {  // U+2066..U+2069
      u(0x2040u + (c2 - 0x80u));
      i += 2;
    } else if (c == 0xE2 && c1 == 0x80 && (c2 == 0x8E || c2 == 0x8F)) {  // U+200E LRM, U+200F RLM
      u(0x2000u + (c2 - 0x80u));
      i += 2;
    } else if (c == 0xD8 && c1 == 0x9C) {  // U+061C ALM
      u(0x061Cu);
      i += 1;
    } else if (c == 0xEF && c1 == 0xBB && c2 == 0xBF) {  // U+FEFF (BOM / ZWNBSP)
      u(0xFEFFu);
      i += 2;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

}  // namespace cbmp
