#include <cbmp/Log.hpp>
#include <chrono>
#include <format>

namespace cbmp {
static const char* LevelTag(Level l) {
    switch (l) {
        case Level::Info: return "INFO ";
        case Level::Warn: return "AVISO";
        case Level::Error: return "ERRO ";
    }
    return "?????";
}

std::string FormatLine(Level level, std::string_view msg, const std::tm& w, int millis) {
    return std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] {} {}",
                       w.tm_year + 1900, w.tm_mon + 1, w.tm_mday, w.tm_hour, w.tm_min, w.tm_sec,
                       millis, LevelTag(level), msg);
}

bool FileLogger::Open(const std::filesystem::path& path) {
    std::lock_guard lock(m_mutex);
    m_out.open(path, std::ios::out | std::ios::app | std::ios::binary);
    return m_out.is_open();
}

void FileLogger::Write(Level level, std::string_view msg) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &tt);
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::lock_guard lock(m_mutex);
    if (!m_out.is_open()) return;
    m_out << FormatLine(level, msg, local, ms) << '\n';
    m_out.flush();
}

FileLogger& Log() {
    static FileLogger instance;
    return instance;
}
}
