#include "cbserver/snapshots.hpp"

#include <utility>

namespace cbserver {

uint8_t SnapshotBuilder::diffMask(const cbmp::PlayerState& a, const cbmp::PlayerState& b) {
  uint8_t m = 0;
  if (a.pos != b.pos) m |= cbmp::kMaskPos;
  if (a.yaw != b.yaw || a.pitch != b.pitch) m |= cbmp::kMaskRot;
  if (a.vel != b.vel) m |= cbmp::kMaskVel;
  if (a.state != b.state) m |= cbmp::kMaskState;
  return m;
}

namespace {

void buildOne(uint32_t tick, PlayerSlot& s, const std::function<const cbmp::PlayerState&(EntityId)>& stateOf,
              cbmp::Snapshot& snap) {
  const SentRecord* base = nullptr;
  if (s.baselineFresh && s.lastAckTick != 0 && s.lastAckTick < tick && tick - s.lastAckTick < kSnapshotRing) {
    const SentRecord& r = s.sent[s.lastAckTick % kSnapshotRing];
    if (r.tick == s.lastAckTick) base = &r;
  }
  SentRecord& rec = s.sent[tick % kSnapshotRing];  // != base: tick - lastAckTick < 32
  rec.tick = tick;
  rec.ents.clear();

  snap.tick = tick;
  snap.ackInputSeq = s.lastInputSeq;
  snap.baselineTick = base ? s.lastAckTick : 0;
  snap.ents.clear();

  size_t j = 0;
  const size_t nb = base ? base->ents.size() : 0;
  for (EntityId id : s.visible.ids) {
    while (j < nb && base->ents[j].id < id) ++j;
    const cbmp::PlayerState* known = (j < nb && base->ents[j].id == id) ? &base->ents[j].s : nullptr;
    const cbmp::PlayerState& cur = stateOf(id);
    const auto tier = static_cast<uint32_t>(tierFor(distSq(s.state.pos, cur.pos)));
    if (tick % tier == id % tier) {
      const uint8_t mask = known ? SnapshotBuilder::diffMask(*known, cur) : cbmp::kMaskAll;
      if (mask != 0) snap.ents.push_back(cbmp::SnapshotEntity{id, mask, cur});
      rec.ents.push_back(SentEntity{id, cur});
    } else if (known) {
      rec.ents.push_back(SentEntity{id, *known});
    }
  }
}

}  // namespace

void SnapshotBuilder::build(uint32_t tick, std::vector<PlayerSlot*>& slots,
                            const std::function<const cbmp::PlayerState&(EntityId)>& stateOf,
                            std::vector<Outgoing>& out) {
  if (results_.size() < slots.size()) results_.resize(slots.size());
  pool_.parallelFor(slots.size(), [&](size_t begin, size_t end, unsigned) {
    for (size_t i = begin; i < end; ++i) buildOne(tick, *slots[i], stateOf, results_[i]);
  });
  out.reserve(out.size() + slots.size());
  // Copia (não move) para manter a capacidade dos vetores de resultado entre ticks.
  for (size_t i = 0; i < slots.size(); ++i) out.push_back(Outgoing{slots[i]->id, results_[i]});
}

}  // namespace cbserver
