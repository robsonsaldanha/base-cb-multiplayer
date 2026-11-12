#include "Arming.hpp"
#include "WinConnectFs.hpp"
#include <cbmp/client/Limits.hpp>
#include <windows.h>
#include <string_view>

namespace cbmp::client {
namespace {
std::filesystem::path FromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

void Why(std::string* why, const char* text) {
    if (why) *why = text;
}
}  // namespace

std::optional<std::vector<uint8_t>> ReadSmallFile(const std::filesystem::path& file, size_t maxBytes) {
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;  // ausente, sem permissão ou pasta (sem BACKUP_SEMANTICS)
    std::optional<std::vector<uint8_t>> result;
    LARGE_INTEGER size{};
    if (GetFileType(h) == FILE_TYPE_DISK && GetFileSizeEx(h, &size) && size.QuadPart >= 0 &&
        static_cast<unsigned long long>(size.QuadPart) <= maxBytes) {
        // Lê até maxBytes + 1: se o arquivo cresceu entre o tamanho e a leitura, recusa.
        std::vector<uint8_t> buf(maxBytes + 1);
        size_t total = 0;
        bool ok = true;
        while (total < buf.size()) {
            DWORD got = 0;
            if (!ReadFile(h, buf.data() + total, static_cast<DWORD>(buf.size() - total), &got, nullptr)) {
                ok = false;
                break;
            }
            if (got == 0) break;
            total += got;
        }
        if (ok && total <= maxBytes) {
            buf.resize(total);
            result = std::move(buf);
        }
    }
    CloseHandle(h);
    return result;
}

std::optional<Armed> TryArm(const std::filesystem::path& dllDir, int64_t nowUnix, std::string* why) {
    if (dllDir.empty()) {
        Why(why, "pasta do main.dll desconhecida");
        return std::nullopt;
    }
    const auto pointerFile = FindPointerFile(dllDir);
    if (!pointerFile) {
        Why(why, "sem cbmp-run.json");
        return std::nullopt;
    }
    const auto bytes = ReadSmallFile(*pointerFile, limits::kMaxPointerBytes);
    if (!bytes) {
        Why(why, "cbmp-run.json ilegível ou grande demais");
        return std::nullopt;
    }
    std::string parseWhy;
    auto pointer = ParsePointer(*bytes, nowUnix, &parseWhy);
    if (!pointer) {
        if (why) *why = "cbmp-run.json inválido: " + parseWhy;
        return std::nullopt;
    }
    auto connectJson = FromUtf8(ConnectPathFor(*pointer));
    if (!WinConnectFs(connectJson).Exists()) {  // GetFileAttributesW: só consulta, não abre
        Why(why, "connect.json ausente");
        return std::nullopt;
    }
    return Armed{pointerFile->parent_path(), std::move(*pointer), std::move(connectJson)};
}
}  // namespace cbmp::client
