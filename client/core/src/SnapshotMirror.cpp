#include <cbmp/client/SnapshotMirror.hpp>

#include <algorithm>

namespace cbmp::client {
namespace {
const MirrorEntity* FindEnt(const std::vector<MirrorEntity>& ents, uint16_t id) {
    auto it = std::lower_bound(ents.begin(), ents.end(), id, [](const MirrorEntity& e, uint16_t v) { return e.id < v; });
    return (it != ents.end() && it->id == id) ? &*it : nullptr;
}
}  // namespace

void SnapshotMirror::Drop() { ++m_dropped; }

void SnapshotMirror::CloseAssembly() {
    m_asmClosed = true;
    m_asmMask = 0;
    for (auto& p : m_asmParts) p.clear();
}

std::optional<AppliedTick> SnapshotMirror::OnSnapshot(const cbmp::Snapshot& part) {
    // 1. Forma (o decode já garante; aqui vale também para quem monta a struct à mão).
    if (part.partCount == 0 || part.partCount > limits::kMaxParts || part.partIndex >= part.partCount ||
        part.ents.size() > limits::kMaxEntities) {
        Drop();
        return std::nullopt;
    }
    // 2. Ordem dos ticks.
    if (part.tick < m_asmTick || part.tick == m_lastApplied) {
        Drop();
        return std::nullopt;
    }
    if (part.tick > m_asmTick) {
        CloseAssembly();
        m_asmTick = part.tick;
        m_asmCount = part.partCount;
        m_asmBaseline = part.baselineTick;
        m_asmClosed = false;
    }
    // Mesmo tick: total de partes e baseline têm de bater com a montagem; tick já aplicado ou descartado não reabre.
    if (m_asmClosed || part.partCount != m_asmCount || part.baselineTick != m_asmBaseline) {
        Drop();
        return std::nullopt;
    }
    // 3. Guarda a parte.
    const auto bit = static_cast<uint8_t>(1u << part.partIndex);
    if (m_asmMask & bit) {
        Drop();
        return std::nullopt;
    }
    m_asmMask = static_cast<uint8_t>(m_asmMask | bit);
    m_asmParts[part.partIndex] = part.ents;
    if (m_asmMask != static_cast<uint8_t>((1u << m_asmCount) - 1u)) return std::nullopt;

    // 4. Completo: a baseline tem de estar no anel.
    const TickRecord* base = nullptr;
    if (m_asmBaseline != 0) {
        base = Find(m_asmBaseline);
        if (!base) {
            Drop();
            CloseAssembly();
            return std::nullopt;
        }
    }
    // 5. Registro = baseline + cada entidade recebida aplicada sobre o estado dela na baseline (ou PlayerState{}).
    TickRecord rec;
    rec.tick = m_asmTick;
    if (base) rec.ents = base->ents;
    std::vector<uint16_t> changed;
    for (size_t p = 0; p < m_asmCount; ++p) {
        for (const auto& e : m_asmParts[p]) {
            const MirrorEntity* known = base ? FindEnt(base->ents, e.id) : nullptr;
            const cbmp::PlayerState s = cbmp::applySnapshotEntity(known ? known->s : cbmp::PlayerState{}, e);
            auto it = std::lower_bound(rec.ents.begin(), rec.ents.end(), e.id,
                                       [](const MirrorEntity& x, uint16_t v) { return x.id < v; });
            if (it != rec.ents.end() && it->id == e.id)
                it->s = s;
            else
                rec.ents.insert(it, MirrorEntity{e.id, s});
            changed.push_back(e.id);
        }
    }
    CloseAssembly();
    if (rec.ents.size() > limits::kMaxEntities) {
        Drop();
        return std::nullopt;
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());

    // 6. Grava no anel (o mais velho sai).
    TickRecord& slot = m_ring[m_next % m_ring.size()];
    slot = std::move(rec);
    ++m_next;
    m_lastApplied = slot.tick;
    return AppliedTick{&slot, std::move(changed)};
}

void SnapshotMirror::Forget(uint16_t id) {
    for (auto& r : m_ring) std::erase_if(r.ents, [id](const MirrorEntity& e) { return e.id == id; });
    for (auto& p : m_asmParts) std::erase_if(p, [id](const cbmp::SnapshotEntity& e) { return e.id == id; });
}

void SnapshotMirror::Reset() { *this = SnapshotMirror{}; }

uint32_t SnapshotMirror::LastAppliedTick() const { return m_lastApplied; }

const TickRecord* SnapshotMirror::Find(uint32_t tick) const {
    if (tick == 0) return nullptr;
    for (const auto& r : m_ring)
        if (r.tick == tick) return &r;
    return nullptr;
}

uint64_t SnapshotMirror::Dropped() const { return m_dropped; }
}  // namespace cbmp::client
