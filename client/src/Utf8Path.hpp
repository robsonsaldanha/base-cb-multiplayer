#pragma once
#include <filesystem>
#include <string>

// Caminho -> UTF-8 para o log. path::string() usa a code page ANSI e estraga acentos (ex. C:\Jogos\Ação\).
inline std::string ToUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
