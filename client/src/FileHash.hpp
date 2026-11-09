#pragma once
#include <filesystem>
#include <string>

// SHA-256 em hex minúsculo; "erro" se não conseguir ler.
std::string Sha256Hex(const std::filesystem::path& file);
