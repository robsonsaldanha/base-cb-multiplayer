#include "cbserver/snapshots.hpp"

#include <utility>

namespace cbserver {

namespace {

void buildOne(uint32_t tick, PlayerSlot& s, const std::function<const cbmp::PlayerState&(EntityId)>& stateOf,
              cbmp::Snapshot& snap) {
  const SentRecord* base = nullptr;
  if (s.baselineFresh && s.lastAckTick != 0 && s.lastAckTick < tick && tick - s.lastAckTick < kSnapshotRing) {
    const SentRecord& r = s.sent[s.lastAckTick % kSnapshotRing];
    if (r.tick == s.lastAckTick) base = &r;
  }
  SentRecord& rec = s.sent[tick % kSnapshotRing];  // != base: tick - lastAckTick < 32
  rec.tick = 0;  // só passa a valer quando completo (exceção no meio não deixa registro falso)
  rec.ents.clear();

  snap.tick = tick;
  snap.ackInputSeq = s.lastInputSeq;
  snap.baselineTick = base ? s.lastAckTick : 0;
  snap.partIndex = 0;
  snap.partCount = 1;
  snap.ents.clear();

  size_t j = 0;
  const size_t nb = base ? base->ents.size() : 0;
  cbmp::SnapshotEntity e;
  for (EntityId id : s.visible.ids) {
    while (j < nb && base->ents[j].id < id) ++j;
    const cbmp::PlayerState* known = (j < nb && base->ents[j].id == id) ? &base->ents[j].s : nullptr;
    const cbmp::PlayerState& cur = stateOf(id);
    const auto tier = static_cast<uint32_t>(tierFor(distSq(s.state.pos, cur.pos)));
    if (tick % tier == id % tier && cbmp::makeSnapshotEntity(id, known, cur, e)) {
      snap.ents.push_back(e);
      // O registro guarda exatamente o que o cliente terá (valores quantizados), nunca `cur` cru.
      rec.ents.push_back(SentEntity{id, cbmp::applySnapshotEntity(known ? *known : cbmp::PlayerState{}, e)});
    } else if (known) {
      rec.ents.push_back(SentEntity{id, *known});
    }
  }
  rec.tick = tick;
}

}  // namespace

void SnapshotBuilder::build(uint32_t tick, std::vector<PlayerSlot*>& slots,
                            const std::function<const cbmp::PlayerState&(EntityId)>& stateOf,
                            std::vector<Outgoing>& out) {
  if (results_.size() < slots.size()) {
    results_.resize(slots.size());
    parts_.resize(slots.size());
  }
  pool_.parallelFor(slots.size(), [&](size_t begin, size_t end, unsigned) {
    for (size_t i = begin; i < end; ++i) {
      buildOne(tick, *slots[i], stateOf, results_[i]);
      if (cbmp::encodedSize(results_[i]) > cbmp::kMaxSnapshotBytes)
        cbmp::splitSnapshot(results_[i], cbmp::kMaxSnapshotBytes, parts_[i]);
      else
        parts_[i].clear();
    }
  });
  out.reserve(out.size() + slots.size());
  // Copia (não move) para manter a capacidade dos vetores de resultado entre ticks.
  for (size_t i = 0; i < slots.size(); ++i) {
    if (parts_[i].empty()) {
      out.push_back(Outgoing{slots[i]->id, results_[i]});
    } else {
      for (auto& p : parts_[i]) out.push_back(Outgoing{slots[i]->id, std::move(p)});
      parts_[i].clear();
    }
  }
}

}  // namespace cbserver
