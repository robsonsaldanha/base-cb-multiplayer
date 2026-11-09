#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <cbmp/messages.hpp>

#include "cbserver/auth.hpp"
#include "cbserver/config.hpp"
#include "cbserver/metrics.hpp"
#include "cbserver/world.hpp"

struct SteamNetConnectionStatusChangedCallback_t;

namespace cbserver {

// Mensagem de um jogador autenticado (Input, SnapshotAck ou Event).
struct Inbound {
  EntityId from = 0;
  cbmp::Message msg;
};

struct NetOptions {
  double helloTimeoutSec = 5.0;  // prazo para o Hello após a conexão
  size_t maxHelloBytes = 4096;
  std::string bindAddress;           // vazio = todas as interfaces; testes usam "127.0.0.1"
  size_t maxMessagesPerPump = 4096;  // o que sobrar fica para o próximo pump
  int sendBufferBytes = 0;           // 0 = padrão do GNS; testes reduzem para forçar LimitExceeded
};

// Chave do limite de pendentes por IP: IPv4 (inclusive mapeado) inteiro; IPv6 agrupado pelo prefixo /64.
std::string pendingIpKey(const std::array<uint8_t, 16>& ipv6);

// Encerramento de um jogador autenticado pela rede (DecodeError / tipo proibido): id, sub, did, motivo.
using ViolationSink =
    std::function<void(EntityId, const std::string& sub, const std::string& did, const std::string& reason)>;

// Inicializa o GameNetworkingSockets uma vez por processo (chamadas seguintes devolvem o mesmo resultado).
bool initNetworking(std::string& err);

// Rede do jogo sobre GameNetworkingSockets (UDP). Uso por uma única thread (a da simulação): todas as chamadas,
// inclusive os callbacks de status (disparados dentro de pump), acontecem nela.
//
// Regras de conexão: aceita só com pendentes por IP < pendingPerIp e global < pendingGlobal; o registro pendente
// guarda só {ip, t0}. A primeira mensagem tem de ser um Hello ≤ maxHelloBytes em ≤ helloTimeoutSec, senão fecha.
// Protocolo diferente → Reject{"protocolo incompatível"}; ticket inválido → Reject{razão}; `sub` já conectado →
// a sessão anterior recebe Kick{"conectado em outro lugar"}; mundo cheio → Reject{"servidor cheio"}; senão Welcome.
// Mensagem de quem não autenticou, tipo que só o servidor envia ou DecodeError (contado em Metrics) → fecha.
// Limites de recepção do GNS no listen socket (herdados pelas conexões): mensagem ≤ 8 KiB, buffer 64 KiB /
// 256 mensagens, 32 segmentos por pacote — mensagens maiores derrubam a conexão no próprio GNS.
// Welcome.serverTimeMs = ms monotônicos desde o início do servidor (o `now` do loop), não horário Unix.
// Envio confiável recusado por LimitExceeded → kick "conexão lenta demais"; falhas de envio contam em
// Metrics::sendFailures. Em stop(), o Kick{"servidor encerrando"} é melhor esforço (pode não chegar).
class NetServer {
 public:
  NetServer(const ServerConfig& cfg, Authenticator& auth, World& world, Metrics& metrics, NetOptions opt = {});
  ~NetServer();
  NetServer(const NetServer&) = delete;
  NetServer& operator=(const NetServer&) = delete;

  // port 0 = porta livre sorteada na faixa efêmera (ver boundPort()); o GNS não aceita porta 0.
  bool start(uint16_t port, std::string& err);
  void stop();
  uint16_t boundPort() const { return boundPort_; }

  // Roda os callbacks do GNS, recebe as mensagens e aplica as regras acima. Nunca bloqueia.
  void pump(double now, std::vector<Inbound>& in);
  // Confiável se cbmp::isReliable(tipo), senão UnreliableNoDelay; conta bytes em Metrics.
  void send(const std::vector<Outgoing>& out);
  // Envia Kick, fecha com linger e remove o jogador do mundo.
  void kick(EntityId id, const std::string& reason);
  void setViolationSink(ViolationSink sink) { violations_ = std::move(sink); }

  size_t pendingCount() const { return pending_.size(); }
  size_t sessionCount() const { return sessions_.size(); }

 private:
  struct Pending {
    std::string ip;
    double t0 = 0;
  };
  struct Session {
    EntityId id = 0;
    std::string sub, did;
  };

  static void statusThunk(SteamNetConnectionStatusChangedCallback_t* info);
  void onStatus(SteamNetConnectionStatusChangedCallback_t* info);
  void onMessage(uint32_t conn, const uint8_t* data, size_t size, std::vector<Inbound>& in);
  void onHello(uint32_t conn, const cbmp::Hello& h);
  // Bytes enviados, -2 = LimitExceeded, -1 = outra falha (ambas contam em Metrics::sendFailures).
  int sendTo(uint32_t conn, const cbmp::Message& m);
  void violation(const Session& s, const std::string& reason);
  void closeConn(uint32_t conn, const std::string& reason, bool linger);
  void dropPending(uint32_t conn);
  void reject(uint32_t conn, const std::string& reason);
  // Encerra a sessão: Kick opcional, fecha e remove o jogador do mundo.
  void endSession(uint32_t conn, const std::string& reason, bool sendKick);
  void updateGauges();
  // Banda "no fio" estimada pelo GNS (informativa), amostrada a cada 1 s no pump.
  void sampleWire();

  const ServerConfig& cfg_;
  Authenticator& auth_;
  World& world_;
  Metrics& metrics_;
  NetOptions opt_;
  uint32_t listen_ = 0, poll_ = 0;
  uint16_t boundPort_ = 0;
  double now_ = 0;
  double lastWireSample_ = -1e9;
  std::unordered_map<uint32_t, Pending> pending_;
  std::unordered_map<std::string, int> pendingByIp_;
  std::unordered_map<uint32_t, Session> sessions_;
  std::unordered_map<EntityId, uint32_t> connOf_;
  std::unordered_map<std::string, uint32_t> connBySub_;
  ViolationSink violations_;
  std::vector<EntityId> slow_;  // reaproveitado em send()
};

}  // namespace cbserver
