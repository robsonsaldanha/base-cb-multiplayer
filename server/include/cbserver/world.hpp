#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <cbmp/messages.hpp>

#include "cbserver/config.hpp"
#include "cbserver/grid.hpp"
#include "cbserver/movement.hpp"
#include "cbserver/rate.hpp"
#include "cbserver/visibility.hpp"

namespace cbserver {

inline constexpr size_t kSnapshotRing = 32;

// Estado de uma entidade como o cliente o conhece num tick (após aplicar o snapshot daquele tick).
struct SentEntity {
  EntityId id = 0;
  cbmp::PlayerState s;
};
// Registro do anel: vetor ordenado por id, reaproveitado (clear mantém a capacidade).
struct SentRecord {
  uint32_t tick = 0;  // 0 = vazio
  std::vector<SentEntity> ents;
};

struct PlayerSlot {
  EntityId id = 0;
  std::string sub, did, name;
  cbmp::PlayerState state;
  uint32_t lastInputSeq = 0;
  double lastInputTime = 0;  // instante do último input aceito (ou do último setState)
  std::deque<double> correctionTimes;  // um registro por incidente (não por Correction repetida)
  bool correcting = false;             // janela após Correction: inputs longe da posição corrigida são ignorados
  double correctionSince = 0;
  int ignoredWindows = 0;  // janelas de correção seguidas que venceram com o cliente ainda longe (≥ 5 → kick)
  VisibleSet visible;
  std::unordered_map<std::string, TokenBucket> eventBuckets;
  std::deque<double> eventViolations;
  // Baselines de snapshot: anel de 32 ticks → estados que o cliente terá após aquele tick.
  std::array<SentRecord, kSnapshotRing> sent;
  uint32_t lastAckTick = 0;
  double lastAckTime = 0;
  bool baselineFresh = false;  // calculado pelo World antes de montar snapshots (ack ≤ 1 s)
  bool kicked = false;         // KickOrder já emitida; ignora novos inputs/eventos
  bool awaitingSpawn = false;  // SpawnMode::DevTrustFirstInput: fora da grade até o primeiro Input
};

struct Outgoing {
  EntityId to = 0;
  cbmp::Message msg;
};
struct KickOrder {
  EntityId who = 0;
  std::string reason;
};

class SnapshotBuilder;

// Estado autoritativo do mundo, sem rede. Uso por uma única thread (a da simulação).
class World {
 public:
  explicit World(const ServerConfig& cfg);
  ~World();

  // Ocupa o menor id livre; aplica `spawn` e enfileira Correction{seq 0, pos, vel} para o próprio jogador.
  // Em SpawnMode::DevTrustFirstInput ignora `spawn`: o jogador fica pendente (invisível, sem Correction).
  std::optional<EntityId> addPlayer(std::string sub, std::string did, std::string name, double now,
                                    const cbmp::PlayerState& spawn = cbmp::PlayerState{});
  // Remove o jogador; os que o viam recebem EntityLeave no próximo tick.
  void removePlayer(EntityId id);
  // Posicionamento autoritativo (spawn/teleporte do servidor): aplica o estado e envia Correction.
  void setState(EntityId id, const cbmp::PlayerState& s, double now);

  // Ignora seq ≤ último. Com Correction pendente (janela de 1 s), ignora inputs longe da posição corrigida;
  // fecha a janela um input a ≤ slack + maxSpeed(estado) × (now − correctionSince) × (1+tol) dela (o cliente
  // pode ter andado legalmente desde a correção; alcance limitado a 1 s). Cada Correction é um incidente; > 10 em
  // 10 s → kick. Janela que vence com o input ainda longe = janela ignorada; 5 seguidas → kick (alcance zera).
  // Com SpawnMode::DevTrustFirstInput, o primeiro input define o spawn sem validação.
  void onInput(EntityId id, const cbmp::Input& in, double now);
  bool onEvent(EntityId id, const cbmp::Event& ev, double now);  // false = descartado
  void onSnapshotAck(EntityId id, uint32_t tick, double now);

  // Um tick: mensagens pendentes, visibilidade + Enter/Leave, snapshots (via SnapshotBuilder), kicks.
  // O chamador envia `out`, envia Kick aos de `kicks` e então chama removePlayer neles.
  void tick(double now, SnapshotBuilder& builder, std::vector<Outgoing>& out, std::vector<KickOrder>& kicks);

  uint32_t currentTick() const { return tick_; }
  size_t playerCount() const { return active_.size(); }
  const PlayerSlot* find(EntityId id) const;

  static constexpr size_t kMaxEventNamesPerPlayer = 64;

 private:
  PlayerSlot* slot(EntityId id);
  void orderKick(PlayerSlot& s, const char* reason);
  void forget(PlayerSlot& viewer, EntityId gone);  // tira `gone` das baselines do viewer
  void sendCorrection(PlayerSlot& p, uint32_t seq, double now);  // enfileira e abre a janela
  double maxSpeedOf(cbmp::MoveState a, cbmp::MoveState b) const;  // maior limite entre os dois estados

  ServerConfig cfg_;
  MovementLimits limits_;
  VisibilityParams vis_;
  double minDt_;
  Grid grid_;
  std::vector<std::unique_ptr<PlayerSlot>> slots_;  // índice = id (0 não usado)
  std::vector<EntityId> active_;                    // ids ativos, ordenados
  std::vector<PlayerSlot*> activeSlots_;            // reaproveitado a cada tick
  std::vector<Outgoing> pending_;
  std::vector<KickOrder> pendingKicks_;
  std::vector<EntityId> scratch_;
  uint32_t tick_ = 0;
};

}  // namespace cbserver
