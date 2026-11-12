#include <cbmp/client/ConnectFile.hpp>
#include <cbmp/client/Limits.hpp>
#include <cbmp/messages.hpp>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

namespace cbmp::client {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {
constexpr size_t kMaxTicketBytes = 2048;
constexpr size_t kMaxServerIdBytes = 64;
constexpr std::string_view kPointerFileName = "cbmp-run.json";

bool IsSep(char c) { return c == '\\' || c == '/'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Inteiro JSON que cabe em int64 (sem sinal acima de INT64_MAX ou ponto flutuante → nullopt).
std::optional<int64_t> AsInt64(const json& v) {
    if (v.is_number_unsigned()) {
        const auto u = v.get<uint64_t>();
        if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
        return static_cast<int64_t>(u);
    }
    if (v.is_number_integer()) return v.get<int64_t>();
    return std::nullopt;
}

// Inteiro JSON não negativo.
std::optional<uint64_t> AsUInt64(const json& v) {
    if (v.is_number_unsigned()) return v.get<uint64_t>();
    return std::nullopt;
}

// b − a quando b ≥ a, sem estouro (a diferença de dois int64 sempre cabe em uint64).
uint64_t Distance(int64_t a, int64_t b) { return static_cast<uint64_t>(b) - static_cast<uint64_t>(a); }

const std::string* StringField(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return nullptr;
    return &it->get_ref<const std::string&>();
}

bool IsIPv4(std::string_view s) {
    int parts = 0;
    size_t i = 0;
    while (true) {
        size_t n = 0;
        unsigned value = 0;
        while (i + n < s.size() && IsDigit(s[i + n])) {
            if (n == 3) return false;
            value = value * 10 + static_cast<unsigned>(s[i + n] - '0');
            ++n;
        }
        if (n == 0 || value > 255 || (n > 1 && s[i] == '0')) return false;
        ++parts;
        i += n;
        if (i == s.size()) return parts == 4;
        if (s[i] != '.' || parts == 4) return false;
        ++i;
    }
}

// Grupos de 1..4 hex separados por ':' (texto vazio = zero grupos). O último pode ser IPv4 (conta 2) se permitido.
bool CountGroups(std::string_view s, bool allowV4Last, int& count) {
    count = 0;
    if (s.empty()) return true;
    size_t start = 0;
    while (true) {
        const size_t colon = s.find(':', start);
        const bool last = colon == std::string_view::npos;
        const std::string_view g = last ? s.substr(start) : s.substr(start, colon - start);
        if (last && allowV4Last && g.find('.') != std::string_view::npos) {
            if (!IsIPv4(g)) return false;
            count += 2;
            return true;
        }
        if (g.empty() || g.size() > 4) return false;
        for (const char c : g) {
            if (!IsHex(c)) return false;
        }
        ++count;
        if (last) return true;
        start = colon + 1;
    }
}

bool IsIPv6(std::string_view s) {
    if (s.find(':') == std::string_view::npos) return false;
    const size_t dbl = s.find("::");
    if (dbl == std::string_view::npos) {
        int n = 0;
        return CountGroups(s, true, n) && n == 8;
    }
    if (s.find("::", dbl + 1) != std::string_view::npos) return false;  // dois "::" ou ":::"
    int head = 0;
    int tail = 0;
    if (!CountGroups(s.substr(0, dbl), false, head) || !CountGroups(s.substr(dbl + 2), true, tail)) return false;
    return head + tail <= 7;
}

std::optional<std::string> DecodeBase64Url(std::string_view in) {
    if (in.size() % 4 == 1) return std::nullopt;
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        uint32_t v = 0;
        if (c >= 'A' && c <= 'Z') v = static_cast<uint32_t>(c - 'A');
        else if (c >= 'a' && c <= 'z') v = static_cast<uint32_t>(c - 'a' + 26);
        else if (IsDigit(c)) v = static_cast<uint32_t>(c - '0' + 52);
        else if (c == '-') v = 62;
        else if (c == '_') v = 63;
        else return std::nullopt;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------- ponteiro

bool IsValidRunDir(std::string_view s) {
    if (s.size() < 3 || !IsAsciiLetter(s[0]) || s[1] != ':' || !IsSep(s[2])) return false;
    std::string_view seg;
    size_t start = 3;
    while (true) {
        size_t end = start;
        while (end < s.size() && !IsSep(s[end])) ++end;
        seg = s.substr(start, end - start);
        if (seg.empty() || seg.back() == '.' || seg.back() == ' ') return false;  // também "." e ".."
        for (const char c : seg) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F) return false;
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') return false;
        }
        if (end == s.size()) break;
        start = end + 1;
    }
    return seg == "run";
}

bool IsLaunchId(std::string_view s) {
    if (s.size() != 32) return false;
    for (const char c : s) {
        if (!IsDigit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

bool IsIpLiteral(std::string_view host) {
    if (!host.empty() && host.front() == '[') {
        if (host.size() < 2 || host.back() != ']') return false;
        return IsIPv6(host.substr(1, host.size() - 2));
    }
    return IsIPv4(host) || IsIPv6(host);
}

std::optional<RunPointer> ParsePointer(std::span<const uint8_t> bytes, int64_t nowUnix, std::string* why) {
    const auto fail = [why](const char* reason) -> std::optional<RunPointer> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (bytes.size() > limits::kMaxPointerBytes) return fail("tamanho");
    const auto j = json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return fail("json");
    if (!j.is_object()) return fail("objeto");

    const auto v = j.find("v");
    if (v == j.end() || AsInt64(*v) != 1) return fail("v");
    const auto* runDir = StringField(j, "runDir");
    if (!runDir || !IsValidRunDir(*runDir)) return fail("runDir");
    const auto* launchId = StringField(j, "launchId");
    if (!launchId || !IsLaunchId(*launchId)) return fail("launchId");
    const auto w = j.find("writtenAt");
    const auto writtenAt = w == j.end() ? std::nullopt : AsInt64(*w);
    if (!writtenAt) return fail("writtenAt");
    if (*writtenAt <= nowUnix) {
        if (Distance(*writtenAt, nowUnix) > static_cast<uint64_t>(limits::kPointerMaxAgeSec)) return fail("velho");
    } else if (Distance(nowUnix, *writtenAt) > static_cast<uint64_t>(limits::kPointerMaxFutureSec)) {
        return fail("futuro");
    }

    RunPointer p;
    p.runDir = *runDir;
    p.launchId = *launchId;
    p.writtenAt = *writtenAt;
    if (const auto* launcher = StringField(j, "launcher")) p.launcher = *launcher;
    return p;
}

std::optional<fs::path> FindPointerFile(const fs::path& dllDir) {
    fs::path dir = dllDir.lexically_normal();
    if (!dir.empty() && !dir.has_filename() && dir != dir.root_path()) dir = dir.parent_path();  // "a\b\" → "a\b"
    for (int level = 0; level <= limits::kPointerSearchLevels; ++level) {
        std::error_code ec;
        auto candidate = dir / kPointerFileName;
        if (fs::is_regular_file(candidate, ec)) return candidate;
        auto parent = dir.parent_path();  // raiz: parent_path() devolve ela mesma
        if (parent.empty() || parent == dir) break;
        dir = std::move(parent);
    }
    return std::nullopt;
}

std::string ConnectPathFor(const RunPointer& p) { return p.runDir + "\\connect.json"; }

// ---------------------------------------------------------------------------------------------------- connect.json

ConnectParse ParseConnect(std::span<const uint8_t> bytes, std::string_view expectedLaunchId, int64_t nowUnix) {
    ConnectParse r;
    ConnectInfo info;
    const auto fail = [&r, &info](ConnectError error, const char* reason) {
        SecureZero(info.ticket);
        r.info.reset();
        r.error = error;
        r.why = reason;
        return r;
    };
    if (bytes.size() > limits::kMaxConnectBytes) return fail(ConnectError::Invalid, "tamanho");
    auto j = json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    // A cópia do ticket dentro do objeto JSON também é zerada, em qualquer saída.
    struct WipeTicket {
        json& j;
        ~WipeTicket() {
            if (!j.is_object()) return;
            const auto it = j.find("ticket");
            if (it != j.end() && it->is_string()) SecureZero(it->get_ref<std::string&>());
        }
    } wipe{j};
    if (j.is_discarded()) return fail(ConnectError::Invalid, "json");
    if (!j.is_object()) return fail(ConnectError::Invalid, "objeto");

    const auto* host = StringField(j, "host");
    if (!host || !IsIpLiteral(*host)) return fail(ConnectError::Invalid, "host");
    info.host = *host;

    const auto p = j.find("port");
    const auto port = p == j.end() ? std::nullopt : AsUInt64(*p);
    if (!port || *port < 1 || *port > 65535) return fail(ConnectError::Invalid, "port");
    info.port = static_cast<uint16_t>(*port);

    const auto* ticket = StringField(j, "ticket");
    if (!ticket || ticket->empty() || ticket->size() > kMaxTicketBytes) return fail(ConnectError::Invalid, "ticket");
    info.ticket = *ticket;

    const auto* serverId = StringField(j, "serverId");
    if (!serverId || serverId->empty() || serverId->size() > kMaxServerIdBytes) {
        return fail(ConnectError::Invalid, "serverId");
    }
    info.serverId = *serverId;

    const auto* launchId = StringField(j, "launchId");
    if (!launchId || *launchId != expectedLaunchId) return fail(ConnectError::Invalid, "launchId");
    info.launchId = *launchId;

    const auto pr = j.find("protocol");
    const auto protocol = pr == j.end() ? std::nullopt : AsUInt64(*pr);
    if (!protocol || *protocol > std::numeric_limits<uint32_t>::max()) return fail(ConnectError::Invalid, "protocol");
    info.protocol = static_cast<uint32_t>(*protocol);
    if (info.protocol != cbmp::kProtocolVersion) return fail(ConnectError::ProtocolMismatch, "protocol");

    if (const auto exp = TicketExp(info.ticket)) {
        const bool expired = *exp < nowUnix ||
                             Distance(nowUnix, *exp) < static_cast<uint64_t>(limits::kTicketMinRemainingSec);
        if (expired) return fail(ConnectError::TicketExpired, "exp");
    }

    r.info = std::move(info);
    return r;
}

std::optional<int64_t> TicketExp(std::string_view ticket) {
    const size_t dot = ticket.find('.');
    if (dot == std::string_view::npos || dot == 0) return std::nullopt;
    const auto claims = DecodeBase64Url(ticket.substr(0, dot));
    if (!claims) return std::nullopt;
    const auto j = json::parse(*claims, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;  // também is_discarded()
    const auto it = j.find("exp");
    if (it == j.end()) return std::nullopt;
    return AsInt64(*it);
}

void SecureZero(std::string& s) {
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
}  // namespace cbmp::client
