#include <cbmp/client/Log.hpp>
#include <cbmp/client/Limits.hpp>
#include <cbmp/log_text.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <system_error>
#include <vector>

namespace cbmp::client {
namespace {
constexpr std::string_view kOmitted = "linha omitida (continha segredo)";

const char* LevelTag(Level l) {
    switch (l) {
        case Level::Info: return "INFO ";
        case Level::Warn: return "AVISO";
        case Level::Error: return "ERRO ";
    }
    return "?????";
}

std::tm LocalTime(std::time_t tt) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    return local;
}

// Avança sobre `n` dígitos (n = 0: um ou mais) a partir de `i`; false se não houver.
bool Digits(std::string_view s, size_t& i, size_t n) {
    const size_t start = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && (n == 0 || i - start < n)) ++i;
    return n == 0 ? i > start : i - start == n;
}

// Só nomes no formato de LogFileName: cb-mp-<8 dígitos>-<6 dígitos>-<pid>.log.
bool IsLogName(const std::filesystem::path& p) {
    // Nomes de LogFileName são ASCII; u8string evita conversão pela página de código no Windows.
    const auto u8 = p.filename().u8string();
    const std::string_view name(reinterpret_cast<const char*>(u8.data()), u8.size());
    constexpr std::string_view kPrefix = "cb-mp-", kExt = ".log";
    if (!name.starts_with(kPrefix) || !name.ends_with(kExt)) return false;
    const std::string_view mid = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kExt.size());
    size_t i = 0;
    return Digits(mid, i, 8) && i < mid.size() && mid[i++] == '-' && Digits(mid, i, 6) && i < mid.size() &&
           mid[i++] == '-' && Digits(mid, i, 0) && i == mid.size();
}
}  // namespace

std::string FormatLine(Level level, std::string_view msg, const std::tm& w, int millis) {
    return std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] {} {}",
                       w.tm_year + 1900, w.tm_mon + 1, w.tm_mday, w.tm_hour, w.tm_min, w.tm_sec,
                       millis, LevelTag(level), msg);
}

std::string LogFileName(const std::tm& w, uint32_t pid) {
    return std::format("cb-mp-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.log",
                       w.tm_year + 1900, w.tm_mon + 1, w.tm_mday, w.tm_hour, w.tm_min, w.tm_sec, pid);
}

void PruneLogs(const std::filesystem::path& dir, size_t keep) {
    std::error_code ec;
    std::vector<std::filesystem::path> logs;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && IsLogName(it->path())) logs.push_back(it->path());
    }
    if (logs.size() <= keep) return;
    std::sort(logs.begin(), logs.end(),
              [](const auto& a, const auto& b) { return a.filename().u8string() < b.filename().u8string(); });
    for (size_t i = 0; i + keep < logs.size(); ++i) {
        std::error_code rec;
        std::filesystem::remove(logs[i], rec);
    }
}

FileLogger::~FileLogger() {
    WipeSecretLocked();  // ninguém mais usa o objeto
}

bool FileLogger::Open(const std::filesystem::path& path) {
    std::lock_guard lock(m_mutex);
    // Um segundo Open (ou um erro de escrita anterior) não pode deixar o stream em falha com IsOpen() verdadeiro.
    if (m_out.is_open()) m_out.close();
    m_out.clear();
    m_out.open(path, std::ios::out | std::ios::app | std::ios::binary);
    return m_out.is_open();
}

bool FileLogger::IsOpen() const {
    std::lock_guard lock(m_mutex);
    return m_out.is_open();
}

void FileLogger::Write(Level level, std::string_view msg) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::tm local = LocalTime(system_clock::to_time_t(now));
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::lock_guard lock(m_mutex);
    if (!m_out.is_open()) return;
    const bool hasSecret = !m_secret.empty() && msg.find(m_secret) != std::string_view::npos;
    const std::string text = hasSecret ? std::string(kOmitted) : cbmp::escapeLogText(msg);
    m_out << FormatLine(level, text, local, ms) << '\n';
    m_out.flush();
}

void FileLogger::SetSecret(std::string_view secret) {
    // Curto demais: a chamada é ignorada e o segredo atual continua valendo (desligar o filtro é só ClearSecret).
    if (secret.size() < limits::kMinSecretBytes) return;
    std::lock_guard lock(m_mutex);
    WipeSecretLocked();
    m_secret.assign(secret);
}

void FileLogger::ClearSecret() {
    std::lock_guard lock(m_mutex);
    WipeSecretLocked();
}

void FileLogger::WipeSecretLocked() {
    // volatile: a escrita de zeros não pode ser descartada pelo compilador antes de o buffer ser liberado.
    volatile char* p = m_secret.data();
    for (size_t i = 0; i < m_secret.size(); ++i) p[i] = 0;
    m_secret.clear();
}

FileLogger& Log() {
    static FileLogger instance;
    return instance;
}
}
