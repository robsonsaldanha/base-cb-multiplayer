#pragma once
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace cbmp {
enum class Level { Info, Warn, Error };

// "[2026-11-09 14:03:07.005] INFO  msg"  (INFO / AVISO / ERRO, alinhado em 5 chars + espaço)
std::string FormatLine(Level level, std::string_view msg, const std::tm& when, int millis);

class FileLogger {
public:
    bool Open(const std::filesystem::path& path); // append, UTF-8
    void Write(Level level, std::string_view msg); // thread-safe, flush a cada linha
private:
    std::mutex m_mutex;
    std::ofstream m_out;
};

FileLogger& Log(); // instância global do mod
}
