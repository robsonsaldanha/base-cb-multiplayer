// Teste espelho fim a fim: um mini-servidor monta os snapshots como server/src/snapshots.cpp (baseline = registro do
// último tick confirmado; makeSnapshotEntity contra o estado conhecido; registro = baseline restrita aos visíveis +
// aplicado; forget na saída) e o cliente os recebe pelo SnapshotMirror com perda, embaralhamento e duplicação.
// Para todo tick que o cliente confirma, o registro dele tem de ser igual ao do servidor.
#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/SnapshotMirror.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <vector>

using namespace cbmp::client;

namespace {
constexpr int kEntities = 40;
constexpr uint32_t kTicks = 600;
constexpr double kLoss = 0.30;
// No fio a idade da baseline é ≤ 31 e o anel do cliente guarda os últimos 32 ticks aplicados, então com o servidor
// real a baseline nunca falta. Para exercitar o descarte por baseline ausente, o servidor do teste aceita baselines
// mais velhas (sem passar pelo codec) e perde todos os acks em dois apagões longos.
constexpr uint32_t kTestMaxAge = 100;
bool AckBlackout(uint32_t tick) { return (tick >= 100 && tick < 180) || (tick >= 400 && tick < 480); }

using Record = std::vector<MirrorEntity>;

struct MiniServer {
    std::map<uint16_t, cbmp::PlayerState> world;  // visíveis para o cliente, por id
    std::map<uint32_t, Record> sent;              // registro por tick
    uint32_t lastAck = 0;

    void Forget(uint16_t id) {
        for (auto& [tick, rec] : sent) std::erase_if(rec, [id](const MirrorEntity& e) { return e.id == id; });
    }

    cbmp::Snapshot Build(uint32_t tick) {
        const Record* base = nullptr;
        uint32_t baseTick = 0;
        if (lastAck != 0 && tick - lastAck <= kTestMaxAge) {
            if (auto it = sent.find(lastAck); it != sent.end()) {
                base = &it->second;
                baseTick = lastAck;
            }
        }
        cbmp::Snapshot snap{};
        snap.tick = tick;
        snap.baselineTick = baseTick;
        Record rec;
        size_t j = 0;
        const size_t nb = base ? base->size() : 0;
        for (const auto& [id, cur] : world) {
            while (j < nb && (*base)[j].id < id) ++j;
            const cbmp::PlayerState* known = (j < nb && (*base)[j].id == id) ? &(*base)[j].s : nullptr;
            const uint32_t tier = (id % 4 == 0) ? 4 : 1;  // "entidades longe": menos atualizações
            cbmp::SnapshotEntity e{};
            if (tick % tier == id % tier && cbmp::makeSnapshotEntity(id, known, cur, e)) {
                snap.ents.push_back(e);
                rec.push_back(MirrorEntity{id, cbmp::applySnapshotEntity(known ? *known : cbmp::PlayerState{}, e)});
            } else if (known) {
                rec.push_back(MirrorEntity{id, *known});
            }
        }
        sent[tick] = std::move(rec);
        return snap;
    }
};

cbmp::PlayerState RandomState(std::mt19937& rng) {
    std::uniform_int_distribution<int32_t> pos(-500000, 500000);
    cbmp::PlayerState s{};
    s.pos = {pos(rng), pos(rng), pos(rng) / 100};
    s.yaw = static_cast<uint16_t>(rng());
    s.pitch = static_cast<uint8_t>(rng());
    s.state = cbmp::MoveState::Walk;
    return s;
}

void Walk(std::mt19937& rng, cbmp::PlayerState& s) {
    std::uniform_real_distribution<double> u(0, 1);
    std::uniform_int_distribution<int32_t> step(-300, 300);
    std::uniform_int_distribution<int> vel(-1500, 1500);
    if (u(rng) < 0.4) return;  // parado: nada muda, o servidor não manda
    s.pos.x += step(rng);
    s.pos.y += step(rng);
    s.pos.z += step(rng) / 6;
    if (u(rng) < 0.01) s.pos.x += 1'000'000;  // salto longo: eixo absoluto
    if (u(rng) < 0.5) s.yaw = static_cast<uint16_t>(s.yaw + step(rng) * 20);
    if (u(rng) < 0.2) s.pitch = static_cast<uint8_t>(rng());
    s.vel = {static_cast<int16_t>(vel(rng)), static_cast<int16_t>(vel(rng)), static_cast<int16_t>(vel(rng) / 10)};
    if (u(rng) < 0.05) s.state = static_cast<cbmp::MoveState>(rng() % (cbmp::kMaxMoveState + 1));
}
}  // namespace

TEST_CASE("client: teste espelho com perda, embaralhamento, baseline e saidas") {
    std::mt19937 rng{1234};
    std::uniform_real_distribution<double> u(0, 1);
    MiniServer server;
    SnapshotMirror mirror;
    for (uint16_t id = 1; id <= kEntities; ++id) server.world[id] = RandomState(rng);
    uint16_t nextId = kEntities + 1;

    std::set<uint32_t> confirmed;
    int baselineDrops = 0, comparedNow = 0, encodedParts = 0, rawParts = 0, leaves = 0;
    std::vector<cbmp::Snapshot> parts;

    for (uint32_t tick = 1; tick <= kTicks; ++tick) {
        for (auto& [id, s] : server.world) Walk(rng, s);
        if (tick % 50 == 0) {
            // sai uma entidade (o servidor faz o forget e manda EntityLeave, confiável) e entra outra; a cada 100 ticks
            // o id que saiu volta logo (reaproveitado), testando que nada dele sobrou no histórico do cliente.
            auto it = server.world.begin();
            std::advance(it, static_cast<long>(rng() % server.world.size()));
            const uint16_t gone = it->first;
            server.world.erase(it);
            server.Forget(gone);
            mirror.Forget(gone);
            ++leaves;
            const uint16_t fresh = (tick % 100 == 0) ? gone : nextId++;
            server.world[fresh] = RandomState(rng);
        }

        const cbmp::Snapshot snap = server.Build(tick);
        cbmp::splitSnapshot(snap, 400, parts);  // força várias partes
        if (u(rng) < 0.1) parts.push_back(parts[rng() % parts.size()]);  // duplicada
        std::shuffle(parts.begin(), parts.end(), rng);

        std::set<uint8_t> delivered;
        bool applied = false;
        for (auto part : parts) {
            if (u(rng) < kLoss) continue;
            delivered.insert(part.partIndex);
            const bool wireOk = part.baselineTick == 0 || part.tick - part.baselineTick <= cbmp::kMaxBaselineAge;
            if (wireOk) {
                part = std::get<cbmp::Snapshot>(cbmp::decode(cbmp::encode(cbmp::Message{part})));
                ++encodedParts;
            } else {
                ++rawParts;
            }
            auto r = mirror.OnSnapshot(part);
            if (!r) continue;
            REQUIRE_FALSE(applied);
            applied = true;
            confirmed.insert(tick);
            REQUIRE(r->record == mirror.Find(tick));
            REQUIRE(r->record->tick == tick);
            REQUIRE(r->record->ents == server.sent.at(tick));
            ++comparedNow;
            std::vector<uint16_t> ids;
            for (const auto& e : snap.ents) ids.push_back(e.id);
            REQUIRE(r->changed == ids);
            if (u(rng) >= kLoss && !AckBlackout(tick) && tick > server.lastAck) server.lastAck = tick;
        }
        if (!applied && delivered.size() == parts.front().partCount) ++baselineDrops;  // todas chegaram, sem ack
        REQUIRE(mirror.LastAppliedTick() == (confirmed.empty() ? 0u : *confirmed.rbegin()));
    }

    // Depois de todas as saídas, o histórico que sobrou ainda bate com o do servidor.
    int stillInRing = 0;
    for (uint32_t tick : confirmed) {
        if (const TickRecord* r = mirror.Find(tick)) {
            REQUIRE(r->ents == server.sent.at(tick));
            ++stillInRing;
        }
    }

    CAPTURE(confirmed.size(), baselineDrops, encodedParts, rawParts, stillInRing, mirror.Dropped());
    REQUIRE(comparedNow == static_cast<int>(confirmed.size()));
    REQUIRE(confirmed.size() >= 100);
    REQUIRE(baselineDrops >= 1);
    REQUIRE(stillInRing == static_cast<int>(limits::kHistoryTicks));
    REQUIRE(leaves == static_cast<int>(kTicks / 50));
    REQUIRE(encodedParts > 0);
    REQUIRE(mirror.Dropped() > 0);
}
