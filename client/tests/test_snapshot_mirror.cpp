#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/SnapshotMirror.hpp>

#include <cstdint>
#include <utility>
#include <vector>

using namespace cbmp::client;

namespace {
cbmp::PlayerState St(int32_t x, int32_t y = 0, int32_t z = 0, cbmp::MoveState m = cbmp::MoveState::Walk) {
    cbmp::PlayerState s{};
    s.pos = {x, y, z};
    s.state = m;
    return s;
}

// Entidade completa (fora da baseline).
cbmp::SnapshotEntity Full(uint16_t id, const cbmp::PlayerState& s) {
    cbmp::SnapshotEntity e{};
    REQUIRE(cbmp::makeSnapshotEntity(id, nullptr, s, e));
    return e;
}

// Só a posição, em delta sobre a baseline.
cbmp::SnapshotEntity Delta(uint16_t id, int32_t dx) {
    cbmp::SnapshotEntity e{};
    e.id = id;
    e.mask = cbmp::kMaskPos;
    e.pos = {dx, 0, 0};
    return e;
}

cbmp::Snapshot Snap(uint32_t tick, uint32_t baseline, uint8_t index, uint8_t count, std::vector<cbmp::SnapshotEntity> ents) {
    cbmp::Snapshot s{};
    s.tick = tick;
    s.baselineTick = baseline;
    s.partIndex = index;
    s.partCount = count;
    s.ents = std::move(ents);
    return s;
}

cbmp::PlayerState Known(const cbmp::PlayerState& s) { return cbmp::applySnapshotEntity(cbmp::PlayerState{}, Full(1, s)); }

const MirrorEntity* Get(const TickRecord* r, uint16_t id) {
    for (const auto& e : r->ents)
        if (e.id == id) return &e;
    return nullptr;
}
}  // namespace

TEST_CASE("client: espelho aplica tick de 1 parte sem baseline") {
    SnapshotMirror m;
    auto r = m.OnSnapshot(Snap(10, 0, 0, 1, {Full(2, St(100)), Full(5, St(200))}));
    REQUIRE(r.has_value());
    REQUIRE(r->record != nullptr);
    REQUIRE(r->record->tick == 10);
    REQUIRE(r->record->ents.size() == 2);
    REQUIRE(r->record->ents[0].id == 2);
    REQUIRE(r->record->ents[0].s == Known(St(100)));
    REQUIRE(r->record->ents[1].id == 5);
    REQUIRE(r->changed == std::vector<uint16_t>{2, 5});
    REQUIRE(m.LastAppliedTick() == 10);
    REQUIRE(m.Find(10) == r->record);
    REQUIRE(m.Find(9) == nullptr);
    REQUIRE(m.Dropped() == 0);
}

TEST_CASE("client: espelho monta 3 partes fora de ordem e so confirma na ultima") {
    SnapshotMirror m;
    REQUIRE_FALSE(m.OnSnapshot(Snap(7, 0, 2, 3, {Full(30, St(3))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(7, 0, 0, 3, {Full(10, St(1))})).has_value());
    REQUIRE(m.LastAppliedTick() == 0);
    auto r = m.OnSnapshot(Snap(7, 0, 1, 3, {Full(20, St(2))}));
    REQUIRE(r.has_value());
    REQUIRE(r->record->ents.size() == 3);
    REQUIRE(r->record->ents[0].id == 10);
    REQUIRE(r->record->ents[1].id == 20);
    REQUIRE(r->record->ents[2].id == 30);
    REQUIRE(r->changed == std::vector<uint16_t>{10, 20, 30});
    REQUIRE(m.LastAppliedTick() == 7);
}

TEST_CASE("client: espelho ignora parte repetida") {
    SnapshotMirror m;
    REQUIRE_FALSE(m.OnSnapshot(Snap(7, 0, 0, 2, {Full(10, St(1))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(7, 0, 0, 2, {Full(10, St(1))})).has_value());
    REQUIRE(m.Dropped() == 1);
    REQUIRE(m.OnSnapshot(Snap(7, 0, 1, 2, {Full(20, St(2))})).has_value());
    // depois de aplicado, o mesmo tick não volta a ser aplicado
    REQUIRE_FALSE(m.OnSnapshot(Snap(7, 0, 1, 2, {Full(20, St(2))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(7, 0, 0, 1, {Full(20, St(2))})).has_value());
    REQUIRE(m.Dropped() == 3);
}

TEST_CASE("client: espelho descarta parte de tick antigo no meio da montagem") {
    SnapshotMirror m;
    REQUIRE_FALSE(m.OnSnapshot(Snap(8, 0, 0, 2, {Full(10, St(1))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(7, 0, 1, 2, {Full(20, St(2))})).has_value());  // tick 7 < 8 em montagem
    REQUIRE(m.Dropped() == 1);
    auto r = m.OnSnapshot(Snap(8, 0, 1, 2, {Full(20, St(5))}));
    REQUIRE(r.has_value());
    REQUIRE(Get(r->record, 20)->s == Known(St(5)));
}

TEST_CASE("client: tick novo reinicia a montagem") {
    SnapshotMirror m;
    REQUIRE_FALSE(m.OnSnapshot(Snap(8, 0, 0, 2, {Full(10, St(1))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(9, 0, 1, 2, {Full(20, St(2))})).has_value());
    // a parte 0 do tick 8 não conta para o tick 9
    REQUIRE_FALSE(m.OnSnapshot(Snap(8, 0, 0, 2, {Full(10, St(1))})).has_value());
    auto r = m.OnSnapshot(Snap(9, 0, 0, 2, {Full(11, St(3))}));
    REQUIRE(r.has_value());
    REQUIRE(r->record->tick == 9);
    REQUIRE(r->record->ents.size() == 2);
    REQUIRE(r->record->ents[0].id == 11);
    REQUIRE(r->record->ents[1].id == 20);
    REQUIRE(m.Find(8) == nullptr);
}

TEST_CASE("client: delta aplicado sobre o registro da baseline") {
    SnapshotMirror m;
    REQUIRE(m.OnSnapshot(Snap(10, 0, 0, 1, {Full(1, St(1000)), Full(2, St(2000)), Full(3, St(3000))})).has_value());
    auto r = m.OnSnapshot(Snap(12, 10, 0, 1, {Delta(2, 50)}));
    REQUIRE(r.has_value());
    REQUIRE(r->changed == std::vector<uint16_t>{2});
    REQUIRE(r->record->ents.size() == 3);  // 1 e 3 vêm da baseline como estavam
    REQUIRE(Get(r->record, 1)->s == Known(St(1000)));
    REQUIRE(Get(r->record, 2)->s.pos.x == 2050);
    REQUIRE(Get(r->record, 2)->s.state == cbmp::MoveState::Walk);
    REQUIRE(Get(r->record, 3)->s == Known(St(3000)));
    // o registro do tick 10 continua como estava
    REQUIRE(Get(m.Find(10), 2)->s.pos.x == 2000);
}

TEST_CASE("client: baseline fora do anel descarta o tick sem ack") {
    SnapshotMirror m;
    REQUIRE(m.OnSnapshot(Snap(10, 0, 0, 1, {Full(1, St(1000))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(12, 11, 0, 1, {Delta(1, 5)})).has_value());  // 11 nunca aplicado
    REQUIRE(m.Dropped() == 1);
    REQUIRE(m.LastAppliedTick() == 10);
    REQUIRE(m.Find(12) == nullptr);
    // partes repetidas do tick descartado não o reabrem
    REQUIRE_FALSE(m.OnSnapshot(Snap(12, 11, 0, 1, {Delta(1, 5)})).has_value());
    REQUIRE(m.Find(12) == nullptr);
    REQUIRE(m.OnSnapshot(Snap(13, 10, 0, 1, {Delta(1, 5)})).has_value());
}

TEST_CASE("client: entidade fora da baseline e aplicada sobre PlayerState vazio") {
    SnapshotMirror m;
    REQUIRE(m.OnSnapshot(Snap(10, 0, 0, 1, {Full(1, St(1000))})).has_value());
    // id 4 não está na baseline: um delta é aplicado sobre PlayerState{} (como no servidor)
    auto r = m.OnSnapshot(Snap(11, 10, 0, 1, {Delta(4, 70)}));
    REQUIRE(r.has_value());
    cbmp::PlayerState expected{};
    expected.pos.x = 70;
    REQUIRE(Get(r->record, 4)->s == expected);
    REQUIRE(r->record->ents.size() == 2);
}

TEST_CASE("client: Forget apaga de todos os registros e a baseline nao ressuscita o id") {
    SnapshotMirror m;
    REQUIRE(m.OnSnapshot(Snap(10, 0, 0, 1, {Full(1, St(1000)), Full(2, St(2000))})).has_value());
    REQUIRE(m.OnSnapshot(Snap(11, 10, 0, 1, {Delta(1, 5)})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(12, 0, 0, 2, {Full(2, St(5))})).has_value());  // montagem em curso com o id 2
    m.Forget(2);
    REQUIRE(Get(m.Find(10), 2) == nullptr);
    REQUIRE(Get(m.Find(11), 2) == nullptr);
    REQUIRE(Get(m.Find(10), 1) != nullptr);
    auto r = m.OnSnapshot(Snap(13, 10, 0, 1, {Delta(1, 9)}));
    REQUIRE(r.has_value());
    REQUIRE(r->record->ents.size() == 1);
    REQUIRE(r->record->ents[0].id == 1);
}

TEST_CASE("client: Forget limpa a montagem em curso") {
    SnapshotMirror m;
    REQUIRE_FALSE(m.OnSnapshot(Snap(5, 0, 0, 2, {Full(2, St(2000))})).has_value());
    m.Forget(2);
    auto r = m.OnSnapshot(Snap(5, 0, 1, 2, {Full(3, St(3000))}));
    REQUIRE(r.has_value());
    REQUIRE(r->record->ents.size() == 1);
    REQUIRE(r->record->ents[0].id == 3);
}

TEST_CASE("client: anel guarda 32 ticks e o mais velho sai") {
    SnapshotMirror m;
    for (uint32_t t = 1; t <= 33; ++t) REQUIRE(m.OnSnapshot(Snap(t, 0, 0, 1, {Full(1, St(static_cast<int32_t>(t)))})).has_value());
    REQUIRE(m.Find(1) == nullptr);
    REQUIRE(m.Find(2) != nullptr);
    REQUIRE(m.Find(33) != nullptr);
    REQUIRE_FALSE(m.OnSnapshot(Snap(34, 1, 0, 1, {Delta(1, 1)})).has_value());  // baseline 1 saiu do anel
    REQUIRE(m.OnSnapshot(Snap(35, 2, 0, 1, {Delta(1, 1)})).has_value());
}

TEST_CASE("client: partCount inconsistente descarta a parte") {
    SnapshotMirror m;
    REQUIRE_FALSE(m.OnSnapshot(Snap(5, 0, 0, 2, {Full(1, St(1))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(5, 0, 1, 3, {Full(2, St(2))})).has_value());  // total diferente
    REQUIRE(m.Dropped() == 1);
    REQUIRE_FALSE(m.OnSnapshot(Snap(5, 0, 0, 9, {})).has_value());   // mais de 8 partes
    REQUIRE_FALSE(m.OnSnapshot(Snap(5, 0, 2, 2, {})).has_value());   // índice fora do total
    REQUIRE_FALSE(m.OnSnapshot(Snap(5, 0, 0, 0, {})).has_value());   // total zero
    REQUIRE(m.Dropped() == 4);
    REQUIRE(m.OnSnapshot(Snap(5, 0, 1, 2, {Full(2, St(2))})).has_value());
}

TEST_CASE("client: 256 entidades sao descartadas") {
    SnapshotMirror m;
    std::vector<cbmp::SnapshotEntity> ents;
    for (uint16_t id = 1; id <= 256; ++id) ents.push_back(Full(id, St(id)));
    REQUIRE_FALSE(m.OnSnapshot(Snap(5, 0, 0, 1, ents)).has_value());
    REQUIRE(m.Dropped() == 1);
    // 128 + 128 em duas partes: o resultado passaria de 255
    std::vector<cbmp::SnapshotEntity> a(ents.begin(), ents.begin() + 128), b(ents.begin() + 128, ents.end());
    REQUIRE_FALSE(m.OnSnapshot(Snap(6, 0, 0, 2, a)).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(6, 0, 1, 2, b)).has_value());
    REQUIRE(m.Find(6) == nullptr);
    REQUIRE(m.Dropped() == 2);
    // 255 cabem
    ents.pop_back();
    auto r = m.OnSnapshot(Snap(7, 0, 0, 1, ents));
    REQUIRE(r.has_value());
    REQUIRE(r->record->ents.size() == 255);
}

TEST_CASE("client: espelho descarta tick zero e Reset limpa tudo") {
    SnapshotMirror m;
    REQUIRE_FALSE(m.OnSnapshot(Snap(0, 0, 0, 1, {})).has_value());
    REQUIRE(m.OnSnapshot(Snap(3, 0, 0, 1, {Full(1, St(1))})).has_value());
    m.Reset();
    REQUIRE(m.LastAppliedTick() == 0);
    REQUIRE(m.Find(3) == nullptr);
    REQUIRE(m.Dropped() == 0);
    REQUIRE(m.OnSnapshot(Snap(2, 0, 0, 1, {})).has_value());
}

TEST_CASE("client: partes do mesmo tick com baselines diferentes") {
    SnapshotMirror m;
    REQUIRE(m.OnSnapshot(Snap(10, 0, 0, 1, {Full(1, St(1))})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(12, 10, 0, 2, {Delta(1, 1)})).has_value());
    REQUIRE_FALSE(m.OnSnapshot(Snap(12, 0, 1, 2, {Full(2, St(2))})).has_value());  // baseline diferente: descarta
    REQUIRE(m.Dropped() == 1);
    REQUIRE(m.OnSnapshot(Snap(12, 10, 1, 2, {Full(2, St(2))})).has_value());
}
