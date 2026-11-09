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
// Para cada id visível cujo tier "toca" no tick (tick % tier == id % tier): máscara = campos diferentes
// da baseline (kMaskAll se ausente); inclui se máscara != 0. O registro deste tick = baseline (restrita
// aos visíveis) mesclada com o que foi enviado. Sempre envia um Snapshot (mesmo vazio).
class SnapshotBuilder {
 public:
  explicit SnapshotBuilder(TaskPool& pool) : pool_(pool) {}
  void build(uint32_t tick, std::vector<PlayerSlot*>& slots,
             const std::function<const cbmp::PlayerState&(EntityId)>& stateOf, std::vector<Outgoing>& out);

  static uint8_t diffMask(const cbmp::PlayerState& a, const cbmp::PlayerState& b);

 private:
  TaskPool& pool_;
  std::vector<cbmp::Snapshot> results_;
};

}  // namespace cbserver
