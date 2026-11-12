#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace cbmp::client {
enum class Level { Info, Warn, Error };
using LogFn = std::function<void(Level, std::string_view)>;

// "[2026-11-09 14:03:07.005] INFO  msg"  (INFO / AVISO / ERRO, alinhado em 5 chars + espaço)
std::string FormatLine(Level level, std::string_view msg, const std::tm& when, int millis);

// Um arquivo por processo: "cb-mp-20261111-201450-1234.log" (data/hora local + pid).
std::string LogFileName(const std::tm& when, uint32_t pid);

// Mantém em `dir` só os `keep` arquivos cb-mp-*.log mais novos (pelo nome); outros arquivos ficam. Erros de arquivo
// (em uso, sem permissão, pasta ausente) são ignorados.
void PruneLogs(const std::filesystem::path& dir, size_t keep);

class FileLogger {
public:
    FileLogger() = default;
    FileLogger(const FileLogger&) = delete;
    FileLogger& operator=(const FileLogger&) = delete;
    ~FileLogger();

    bool Open(const std::filesystem::path& path);  // append, UTF-8
    bool IsOpen() const;
    // Thread-safe, flush a cada linha. O texto passa por cbmp::escapeLogText (uma mensagem = uma linha); uma mensagem
    // que contém o segredo vira "linha omitida (continha segredo)".
    void Write(Level level, std::string_view msg);
    void SetSecret(std::string_view secret);  // < 16 bytes: ignorado
    void ClearSecret();                       // sobrescreve a cópia com zeros

private:
    void WipeSecretLocked();

    mutable std::mutex m_mutex;
    std::ofstream m_out;
    std::string m_secret;
};

FileLogger& Log();  // instância global do mod
}
