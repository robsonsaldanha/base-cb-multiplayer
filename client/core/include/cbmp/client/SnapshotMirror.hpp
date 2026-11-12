#pragma once
// Espelho do histórico de snapshots do servidor (server/src/snapshots.cpp): monta as partes de um tick, reconstrói o
// estado sobre a baseline e guarda os últimos 32 ticks aplicados. O registro de um tick é exatamente o que o servidor
// guarda para ele, e por isso pode servir de baseline aos ticks seguintes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <cbmp/client/Limits.hpp>
#include <cbmp/messages.hpp>

namespace cbmp::client {
struct MirrorEntity {
    uint16_t id = 0;
    cbmp::PlayerState s;
    bool operator==(const MirrorEntity&) const = default;
};
struct TickRecord {
    uint32_t tick = 0;  // 0 = vazio
    std::vector<MirrorEntity> ents;  // ids crescentes
};
struct AppliedTick {
    const TickRecord* record = nullptr;  // válido até 32 ticks aplicados depois (ou Reset)
    std::vector<uint16_t> changed;       // ids recebidos neste tick (crescentes)
};

class SnapshotMirror {
public:
    // Valor = tick completo e aplicado: confirme com SnapshotAck. nullopt = falta parte ou descartado.
    std::optional<AppliedTick> OnSnapshot(const cbmp::Snapshot& part);
    void Forget(uint16_t id);  // EntityLeave: apaga o id de todos os registros e da montagem em curso
    void Reset();
    uint32_t LastAppliedTick() const;
    const TickRecord* Find(uint32_t tick) const;
    uint64_t Dropped() const;  // partes/ticks descartados
    // Passos da montagem de registros (entidades recolhidas + comparações da ordenação + passos do merge), acumulado.
    // Diagnóstico: os testes conferem que o trabalho por tick é limitado pela entrada (≤ 8 × 255 recebidas).
    uint64_t MergeWork() const;

private:
    void Drop();
    void CloseAssembly();

    std::array<TickRecord, limits::kHistoryTicks> m_ring{};
    size_t m_next = 0;  // próxima posição do anel (a mais velha quando cheio)
    uint32_t m_lastApplied = 0;
    uint64_t m_dropped = 0;
    uint64_t m_mergeWork = 0;

    // Montagem do tick em curso.
    uint32_t m_asmTick = 0;
    uint32_t m_asmBaseline = 0;
    uint8_t m_asmCount = 0;
    uint8_t m_asmMask = 0;
    bool m_asmClosed = true;  // aplicado ou descartado: mais partes do mesmo tick não contam
    std::array<std::vector<cbmp::SnapshotEntity>, limits::kMaxParts> m_asmParts{};
};
}  // namespace cbmp::client
