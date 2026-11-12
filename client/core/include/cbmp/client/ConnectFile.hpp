#pragma once
// Ponteiro do launcher (Win64\cbmp\cbmp-run.json) e connect.json: validação pura sobre bytes, mais a porta IConnectFs
// para "abrir exclusivo, conferir, ler e apagar" (implementação Win32 em client/src/platform, falsa nos testes).
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cbmp::client {
struct RunPointer {
    std::string runDir;  // UTF-8, como veio
    std::string launchId;
    int64_t writtenAt = 0;
    std::string launcher;
};

// ≤ 4 KiB, objeto JSON, v == 1, runDir válido, launchId válido, writtenAt inteiro com no máximo 600 s de idade e 120 s
// no futuro. Campos a mais são ignorados. Falha → nullopt e, se `why` não for nulo, um motivo curto.
std::optional<RunPointer> ParsePointer(std::span<const uint8_t> bytes, int64_t nowUnix, std::string* why = nullptr);

// "X:\...\run": letra de unidade + ":" + separador, sem "\\" inicial, sem segmento vazio, "." ou ".." (nem terminado em
// ponto ou espaço, que o Windows apaga), sem caracteres de controle nem `<>:"|?*` depois da unidade, último segmento
// "run". Aceita '\' e '/' como separadores.
bool IsValidRunDir(std::string_view utf8);

// 32 hex minúsculos.
bool IsLaunchId(std::string_view s);

// IPv4 a.b.c.d (decimal, 0..255, sem zero à esquerda) ou IPv6 (com ou sem colchetes, sem zona). Nunca nome.
bool IsIpLiteral(std::string_view host);

// Procura "cbmp-run.json" (arquivo comum) em dllDir e em até 5 pastas acima.
std::optional<std::filesystem::path> FindPointerFile(const std::filesystem::path& dllDir);

// runDir + "\connect.json" (UTF-8).
std::string ConnectPathFor(const RunPointer& p);

struct ConnectInfo {
    std::string host;
    uint16_t port = 0;
    std::string ticket;
    std::string serverId;
    uint32_t protocol = 0;
    std::string launchId;
};
enum class ConnectError { None, Invalid, ProtocolMismatch, TicketExpired };
struct ConnectParse {
    std::optional<ConnectInfo> info;
    ConnectError error = ConnectError::None;
    std::string why;
};

// connect.json do launcher. `contentDir` nunca é lido. Em erro, o ticket lido é zerado antes de devolver.
ConnectParse ParseConnect(std::span<const uint8_t> bytes, std::string_view expectedLaunchId, int64_t nowUnix);

// Claim "exp" do JSON antes do ponto (base64url), sem conferir a assinatura. nullopt se não der para ler.
std::optional<int64_t> TicketExp(std::string_view ticket);

// Sobrescreve com zeros (escrita volátil) e limpa.
void SecureZero(std::string& s);
void SecureZero(std::vector<uint8_t>& bytes);

enum class ClaimRead { Ok, Missing, Busy, Rejected };
struct ClaimResult {
    ClaimRead status = ClaimRead::Missing;
    std::vector<uint8_t> bytes;
};

class IConnectFs {
public:
    virtual ~IConnectFs() = default;
    virtual bool Exists() = 0;                 // só consulta
    // Abre exclusivo, confere, lê e apaga. Os bytes de um Ok trazem o ticket em claro: quem chama passa por
    // ParseConnect e depois zera o buffer com SecureZero.
    virtual ClaimResult ClaimAndDelete() = 0;
};
}  // namespace cbmp::client
