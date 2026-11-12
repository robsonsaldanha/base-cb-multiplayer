#pragma once
// Transporte real do cliente: uma conexão GameNetworkingSockets por IP, sem callback global (o estado é lido por
// GetConnectionInfo a cada State()). Usado pelo mod e pelo cbmp-headless; nos testes da Session vale o FakeTransport.
#include <cbmp/client/Transport.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace cbmp::client {
struct GnsOptions {
    int fakeLossPercent = 0;  // só headless/testes: perda falsa do GNS (envio e recepção), 0..100
};

bool GnsInit(std::string* err = nullptr);  // GameNetworkingSockets_Init; idempotente
void GnsShutdown();                        // GameNetworkingSockets_Kill; idempotente

class GnsTransport final : public ITransport {
public:
    explicit GnsTransport(GnsOptions opt = {});
    ~GnsTransport() override;
    GnsTransport(const GnsTransport&) = delete;
    GnsTransport& operator=(const GnsTransport&) = delete;

    // "ip:porta" (IPv6 entre colchetes) + ConnectByIPAddress. false se o GNS não foi iniciado, se o endereço não é
    // um IP literal ou se já há uma conexão.
    bool Connect(const std::string& ipLiteral, uint16_t port) override;
    bool Send(std::span<const uint8_t> bytes, bool reliable) override;
    // RunCallbacks, puxa ReceiveMessagesOnConnection para a fila própria (para de puxar quando ela passa de
    // kMaxPendingMessages + 1) e entrega até `max`.
    size_t Receive(std::vector<std::vector<uint8_t>>& out, size_t max) override;
    size_t Pending() const override;
    // Connecting/FindingRoute → Connecting; Connected → Connected; ClosedByPeer/ProblemDetectedLocally → drena a fila
    // mais uma vez, guarda o motivo, fecha e fica Closed (o que já foi puxado continua disponível para Receive).
    LinkState State() override;
    LinkEnd End() const override;
    void Close() override;  // CloseConnection(conn, 0, "fim", false)

private:
    void Pull();
    void Finish(int code, std::string debug);

    GnsOptions m_opt;
    uint32_t m_conn = 0;  // HSteamNetConnection (0 = k_HSteamNetConnection_Invalid)
    LinkState m_state = LinkState::Idle;
    LinkEnd m_end;
    std::deque<std::vector<uint8_t>> m_queue;
};
}  // namespace cbmp::client
