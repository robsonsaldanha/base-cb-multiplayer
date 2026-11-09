#pragma once
#include <filesystem>

// Grava cb-mp-AAAAMMDD-HHMMSS.dmp em `dir` se o processo crashar.
void InstallCrashDump(const std::filesystem::path& dir);
