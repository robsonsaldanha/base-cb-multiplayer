#include "WinConnectFs.hpp"
#include <cbmp/client/Limits.hpp>
#include <windows.h>
#include <aclapi.h>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cbmp::client {
namespace {
struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct LocalFreer {
    void operator()(void* p) const { LocalFree(p); }
};

// SID de uma classe do token (TokenUser ou TokenOwner) é igual a `sid`?
bool TokenSidEquals(HANDLE token, TOKEN_INFORMATION_CLASS cls, PSID sid) {
    DWORD len = 0;
    GetTokenInformation(token, cls, nullptr, 0, &len);
    if (len == 0) return false;
    std::vector<uint8_t> buf(len);
    if (!GetTokenInformation(token, cls, buf.data(), len, &len)) return false;
    PSID mine = cls == TokenUser ? reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid
                                 : reinterpret_cast<TOKEN_OWNER*>(buf.data())->Owner;
    return mine && IsValidSid(mine) && EqualSid(mine, sid);
}

// Dono do arquivo = usuário do token do processo. Também vale o dono padrão do token (TokenOwner): num processo de
// administrador elevado os arquivos que ele cria ficam com o grupo Administradores como dono.
bool OwnerIsCurrentUser(HANDLE file) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS) {
        return false;
    }
    const std::unique_ptr<void, LocalFreer> sdGuard(sd);
    if (!owner || !IsValidSid(owner)) return false;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
    const UniqueHandle token(rawToken);
    return TokenSidEquals(rawToken, TokenUser, owner) || TokenSidEquals(rawToken, TokenOwner, owner);
}

void Wipe(std::vector<uint8_t>& bytes) {
    volatile uint8_t* p = bytes.data();
    for (size_t i = 0; i < bytes.size(); ++i) p[i] = 0;
    bytes.clear();
}
}  // namespace

WinConnectFs::WinConnectFs(std::filesystem::path connectJson) : path_(std::move(connectJson)) {}

bool WinConnectFs::Exists() {
    const DWORD attrs = GetFileAttributesW(path_.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

ClaimResult WinConnectFs::ClaimAndDelete() {
    // Sem compartilhamento: se outro processo está com o arquivo aberto, violação (Busy). DELETE_ON_CLOSE: daqui em
    // diante o arquivo some ao fechar, aceito ou recusado. OPEN_REPARSE_POINT: um link é aberto (e apagado) como link.
    HANDLE raw = CreateFileW(path_.c_str(), GENERIC_READ | DELETE, 0 /*sem compartilhar*/, nullptr, OPEN_EXISTING,
                             FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return {ClaimRead::Missing, {}};
        if (err == ERROR_SHARING_VIOLATION) return {ClaimRead::Busy, {}};
        return {ClaimRead::Rejected, {}};  // pasta (acesso negado), somente leitura, exclusão pendente...
    }
    const UniqueHandle file(raw);

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(raw, &info)) return {ClaimRead::Rejected, {}};
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return {ClaimRead::Rejected, {}};
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > limits::kMaxConnectBytes) return {ClaimRead::Rejected, {}};
    if (!OwnerIsCurrentUser(raw)) return {ClaimRead::Rejected, {}};

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    size_t total = 0;
    while (total < bytes.size()) {
        DWORD got = 0;
        if (!ReadFile(raw, bytes.data() + total, static_cast<DWORD>(bytes.size() - total), &got, nullptr)) {
            Wipe(bytes);
            return {ClaimRead::Rejected, {}};
        }
        if (got == 0) break;
        total += got;
    }
    bytes.resize(total);
    return {ClaimRead::Ok, std::move(bytes)};
}
}  // namespace cbmp::client
