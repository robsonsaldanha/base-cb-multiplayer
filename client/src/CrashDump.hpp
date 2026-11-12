#pragma once
#include <filesystem>

namespace cbmp::client {
// Grava cb-mp-AAAAMMDD-HHMMSS.dmp em `dir` se o processo crashar (MiniDumpNormal: pilhas e módulos, sem o heap, para
// o ticket não ir parar no dump).
void InstallCrashDump(const std::filesystem::path& dir);
}  // namespace cbmp::client
