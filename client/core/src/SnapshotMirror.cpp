#include <cbmp/client/SnapshotMirror.hpp>

#include <algorithm>

namespace cbmp::client {
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
    // 5. Entidades recebidas ordenadas por id uma vez (estável: entre partes, a última vence) e sem repetição. Mais de
    //    255 ids distintos descarta o tick antes de qualquer merge. Custo: O(n log n) com n ≤ 8 × 255.
    std::vector<const cbmp::SnapshotEntity*> recv;
    for (size_t p = 0; p < m_asmCount; ++p) {
        for (const auto& e : m_asmParts[p]) {
            recv.push_back(&e);
            ++m_mergeWork;
        }
    }
    std::stable_sort(recv.begin(), recv.end(), [this](const cbmp::SnapshotEntity* a, const cbmp::SnapshotEntity* b) {
        ++m_mergeWork;
        return a->id < b->id;
    });
    size_t distinct = 0;
    for (size_t i = 0; i < recv.size(); ++i) {
        if (i + 1 < recv.size() && recv[i + 1]->id == recv[i]->id) continue;  // fica a última ocorrência
        recv[distinct++] = recv[i];
    }
    recv.resize(distinct);
    if (recv.size() > limits::kMaxEntities) {
        Drop();
        CloseAssembly();
        return std::nullopt;
    }

    // 6. Registro = merge linear da baseline com as recebidas, cada uma aplicada sobre o estado dela na baseline (ou
    //    PlayerState{}). Passar de 255 descarta na hora.
    static const std::vector<MirrorEntity> kEmpty;
    const std::vector<MirrorEntity>& old = base ? base->ents : kEmpty;
    TickRecord rec;
    rec.tick = m_asmTick;
    rec.ents.reserve(std::min(old.size() + recv.size(), limits::kMaxEntities));
    std::vector<uint16_t> changed;
    changed.reserve(recv.size());
    size_t i = 0, j = 0;
    while (i < old.size() || j < recv.size()) {
        if (rec.ents.size() >= limits::kMaxEntities) {
            Drop();
            CloseAssembly();
            return std::nullopt;
        }
        ++m_mergeWork;
        if (j == recv.size() || (i < old.size() && old[i].id < recv[j]->id)) {
            rec.ents.push_back(old[i++]);
            continue;
        }
        const cbmp::SnapshotEntity& e = *recv[j++];
        const bool known = i < old.size() && old[i].id == e.id;
        rec.ents.push_back(MirrorEntity{e.id, cbmp::applySnapshotEntity(known ? old[i].s : cbmp::PlayerState{}, e)});
        if (known) ++i;
        changed.push_back(e.id);
    }
    CloseAssembly();

    // 7. Grava no anel (o mais velho sai).
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

uint64_t SnapshotMirror::MergeWork() const { return m_mergeWork; }
}  // namespace cbmp::client
