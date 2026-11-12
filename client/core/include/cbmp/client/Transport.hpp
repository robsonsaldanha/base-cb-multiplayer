#pragma once
// Porta de transporte do cliente: a sessão fala com o servidor só por aqui (GNS no mod, FakeTransport nos testes).
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cbmp::client {
enum class LinkState { Idle, Connecting, Connected, Closed };

// Como a conexão terminou (válido em Closed): código de fim do GNS e o texto de depuração dele.
struct LinkEnd {
    int code = 0;
    std::string debug;
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool Connect(const std::string& ipLiteral, uint16_t port) = 0;              // false = falha imediata
    virtual bool Send(std::span<const uint8_t> bytes, bool reliable) = 0;
    // Acrescenta até `max` mensagens. O transporte real limita o tamanho de cada mensagem já na recepção (GNS:
    // k_ESteamNetworkingConfig_RecvMaxMessageSize = kMaxMessageBytes), para um lote de 512 não alocar centenas de MiB
    // antes de o NetClient descartar o que passa de 8 KiB.
    virtual size_t Receive(std::vector<std::vector<uint8_t>>& out, size_t max) = 0;
    virtual size_t Pending() const = 0;                                                  // ainda na fila local depois do Receive
    virtual LinkState State() = 0;
    virtual LinkEnd End() const = 0;                                                     // válido em Closed
    virtual void Close() = 0;                                                            // sem esperar
};
}  // namespace cbmp::client
