#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace cbmp::client {
// SHA-256 do arquivo inteiro (BCrypt). false se não conseguir ler (out fica zerado).
bool Sha256Bytes(const std::filesystem::path& file, std::array<uint8_t, 32>& out);

// SHA-256 em hex minúsculo; "erro" se não conseguir ler.
std::string Sha256Hex(const std::filesystem::path& file);
}  // namespace cbmp::client
