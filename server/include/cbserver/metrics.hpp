#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace cbserver {

class Metrics {
 public:
  static constexpr size_t kWindow = 1200;  // 60 s a 20 Hz

  void recordTick(std::chrono::microseconds d);
  // Banda por jogador = bytes de payload das mensagens GNS enviadas a ele (sem cabeçalhos IP/UDP/GNS) desde o
  // primeiro envio da conexão ÷ tempo desde então (mínimo 1 s). avg/maxKbpsPerPlayer = média/máximo entre os
  // jogadores conectados. removePlayer encerra a conta da conexão (o id pode ser reutilizado).
  void addBytesOut(uint16_t playerId, size_t bytes);
  void removePlayer(uint16_t playerId);
  // Estimativa "no fio" (GNS m_flOutBytesPerSec, com cabeçalhos/retransmissões), só informativa.
  void setWireOut(double avgKbps, double maxKbps);
  nlohmann::json snapshot() const;
  // Banco (cb.db/bans), atualizado pelo loop principal 1×/s: {queries, errors, timeouts, qps, p50Ms, p99Ms, busy,
  // connected, size, queued, activeTx}. Nulo = sem banco (o campo "db" some do /metrics). Nunca inclui a senha.
  void setDb(nlohmann::json db);

  std::atomic<int64_t> players{0}, pending{0}, corrections{0}, kicks{0}, droppedEvents{0}, decodeErrors{0}, sendFailures{0};
  // Envio: mensagens descartadas por não caberem no protocolo (não derrubam o servidor).
  std::atomic<int64_t> encodeErrors{0};
  // Scripts (atualizados pelo loop principal): eventos não aceitos pela fila do host, comandos descartados pelo host
  // (fila cheia) ou recusados pela simulação (inválidos), eventos servidor → cliente acima da cota do jogador,
  // logs/auditoria descartados por limite de taxa, Join/Leave perdidos.
  std::atomic<int64_t> scriptEventsDropped{0}, scriptCommandsDropped{0}, scriptCommandsRejected{0},
      scriptEventsOutDropped{0}, scriptLogDropped{0}, scriptAuditDropped{0}, scriptJoinLeaveDropped{0};

 private:
  mutable std::mutex mu_;
  std::vector<int64_t> ticks_;  // anel, em microssegundos
  size_t head_ = 0;
  struct ConnBytes {
    uint64_t bytes = 0;
    std::chrono::steady_clock::time_point since;
  };
  std::unordered_map<uint16_t, ConnBytes> bytes_;
  double wireAvgKbps_ = 0, wireMaxKbps_ = 0;
  mutable std::mutex dbMu_;
  nlohmann::json db_;
  mutable double lastCpuSec_ = -1;
  mutable std::chrono::steady_clock::time_point lastCpuAt_{};
};

}  // namespace cbserver
