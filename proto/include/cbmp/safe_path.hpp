#pragma once
#include <string_view>

// Regra única de "caminho seguro" de resources, usada pelo manifest do servidor (cbserver) e pelo runtime Lua
// (cbscript: nomes de resource, scripts do manifest e `require`).
namespace cbmp {

// [A-Za-z0-9._-]
constexpr bool isSafePathChar(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
         c == '-';
}

// Um segmento: não vazio, só [A-Za-z0-9._-], e diferente de "." e "..".
constexpr bool safeSegment(std::string_view s) noexcept {
  if (s.empty() || s == "." || s == "..") return false;
  for (char c : s)
    if (!isSafePathChar(c)) return false;
  return true;
}

// ^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$ sem segmentos "." ou "..": relativo, sem barra inicial/final, sem "//",
// sem "\\", sem ":" (letra de unidade).
constexpr bool safeRelPath(std::string_view rel) noexcept {
  if (rel.empty()) return false;
  size_t start = 0;
  while (true) {
    size_t end = rel.find('/', start);
    if (!safeSegment(rel.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)))
      return false;
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
}

}  // namespace cbmp
