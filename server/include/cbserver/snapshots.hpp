#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include <cbmp/messages.hpp>

#include "cbserver/pool.hpp"
#include "cbserver/world.hpp"

namespace cbserver {

// Monta um Snapshot por slot, em paralelo. Cada worker escreve só no próprio slot (anel `sent`) e na
// posição do slot em um vetor de resultados reaproveitado; o agregado em `out` segue a ordem de `slots`,
// logo é idêntico para qualquer número de workers.
//
// Regras por slot: baseline = sent[lastAckTick] se baselineFresh e ainda no anel; senão vazia (baselineTick 0).
// Para cada id visível cujo tier "toca" no tick (tick % tier == id % tier): cbmp::makeSnapshotEntity contra o
// estado conhecido pelo cliente (completo se ausente); inclui se houver mudança. O registro deste tick =
// baseline (restrita aos visíveis) + cbmp::applySnapshotEntity do que foi enviado — o mesmo cálculo do cliente.
// Sempre envia um Snapshot (mesmo vazio); acima de kMaxSnapshotBytes, divide em partes do mesmo tick.
class SnapshotBuilder {
 public:
  explicit SnapshotBuilder(TaskPool& pool) : pool_(pool) {}
  void build(uint32_t tick, std::vector<PlayerSlot*>& slots,
             const std::function<const cbmp::PlayerState&(EntityId)>& stateOf, std::vector<Outgoing>& out);


 private:
  TaskPool& pool_;
  std::vector<cbmp::Snapshot> results_;
  std::vector<std::vector<cbmp::Snapshot>> parts_;  // só quando o snapshot passa de kMaxSnapshotBytes
};

}  // namespace cbserver
