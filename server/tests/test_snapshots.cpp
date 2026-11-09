#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <cbmp/quantize.hpp>

#include "world_harness.hpp"

using cbmp::Snapshot;
using cbserver::ServerConfig;
using testing_world::Harness;
using testing_world::input;

TEST_CASE("snapshot is sent every tick with ackInputSeq even when empty") {
  Harness h;
  auto a = h.add("ana");
  h.step();
  h.world.onInput(a, input(7, {10, 0, 0}), h.now);
  for (int i = 0; i < 3; ++i) {
    h.step();
    auto s = h.msgs<Snapshot>(a);
    REQUIRE(s.size() == 1);
    REQUIRE(s[0].tick == h.world.currentTick());
    REQUIRE(s[0].ackInputSeq == 7);
    REQUIRE(s[0].ents.empty());
  }
}

TEST_CASE("snapshot delta against acked baseline") {
  Harness h;
  auto a = h.add("ana");
  auto b = h.add("bia", {1000, 0, 0});
  h.step();  // tick 1: sem baseline → estado completo
  auto s1 = h.msgs<Snapshot>(a);
  REQUIRE(s1.size() == 1);
  REQUIRE(s1[0].baselineTick == 0);
  REQUIRE(s1[0].ents.size() == 1);
  REQUIRE(s1[0].ents[0].id == b);
  REQUIRE(s1[0].ents[0].mask == cbmp::kMaskAll);
  REQUIRE(s1[0].ents[0].posAbs == 7);
  REQUIRE(s1[0].ents[0].pos.x == 1000);
  // registro do servidor = estado que o cliente calcula
  const auto known1 = cbmp::applySnapshotEntity(cbmp::PlayerState{}, s1[0].ents[0]);
  REQUIRE(h.world.find(a)->sent[1].tick == 1);
  REQUIRE(h.world.find(a)->sent[1].ents.size() == 1);
  REQUIRE(h.world.find(a)->sent[1].ents[0].s == known1);

  h.world.onSnapshotAck(a, 1, h.now);
  h.step();  // tick 2: baseline 1, B parado → não aparece
  auto s2 = h.msgs<Snapshot>(a);
  REQUIRE(s2[0].baselineTick == 1);
  REQUIRE(s2[0].ents.empty());

  SECTION("only position changed gives kMaskPos") {
    h.world.onInput(b, input(1, {1040, 0, 0}), h.now);
    REQUIRE(h.world.find(b)->state.pos.x == 1040);
    h.step();  // tick 3: baseline ainda 1
    auto s3 = h.msgs<Snapshot>(a);
    REQUIRE(s3[0].baselineTick == 1);
    REQUIRE(s3[0].ents.size() == 1);
    REQUIRE(s3[0].ents[0].mask == cbmp::kMaskPos);
    REQUIRE(s3[0].ents[0].posAbs == 0);
    REQUIRE(s3[0].ents[0].pos.x == 40);
    REQUIRE(cbmp::applySnapshotEntity(known1, s3[0].ents[0]).pos.x == 1040);
  }

  SECTION("no ack for more than 1 s falls back to full state") {
    h.step(1.1);
    auto s = h.msgs<Snapshot>(a);
    REQUIRE(s[0].baselineTick == 0);
    REQUIRE(s[0].ents.size() == 1);
    REQUIRE(s[0].ents[0].mask == cbmp::kMaskAll);
  }

  SECTION("ack never regresses and unknown ticks are ignored") {
    h.world.onSnapshotAck(a, 2, h.now);
    h.world.onSnapshotAck(a, 1, h.now);   // mais antigo: ignorado
    h.world.onSnapshotAck(a, 99, h.now);  // nunca enviado: ignorado
    REQUIRE(h.world.find(a)->lastAckTick == 2);
  }

  SECTION("ack after dropped snapshots uses the acked tick as baseline") {
    h.step();  // 3
    h.step();  // 4
    h.step();  // 5 (cliente perdeu 3 e 4)
    h.world.onSnapshotAck(a, 5, h.now);
    h.step();  // 6
    auto s = h.msgs<Snapshot>(a);
    REQUIRE(s[0].baselineTick == 5);
    REQUIRE(s[0].ents.empty());
  }

  SECTION("ack older than the ring is ignored") {
    for (int i = 0; i < 40; ++i) h.step(0.01);
    h.world.onSnapshotAck(a, 2, h.now);
    REQUIRE(h.world.find(a)->lastAckTick == 1);
    h.world.onSnapshotAck(a, h.world.currentTick() - 31, h.now);
    REQUIRE(h.world.find(a)->lastAckTick == h.world.currentTick() - 31);
  }
}

TEST_CASE("snapshot far tier entity only every 4 ticks") {
  Harness h;
  auto a = h.add("ana");
  auto b = h.add("bia", {25'000, 0, 0});  // 250 m → Far
  std::vector<uint32_t> seen;
  for (int i = 0; i < 8; ++i) {
    h.step();
    if (h.entityIn(a, b)) seen.push_back(h.world.currentTick());
    REQUIRE(h.msgs<Snapshot>(a).size() == 1);
  }
  REQUIRE(seen.size() == 2);
  REQUIRE(seen[1] - seen[0] == 4);
  REQUIRE(seen[0] % 4 == static_cast<uint32_t>(b % 4));
}

TEST_CASE("snapshot mid tier entity every 2 ticks") {
  Harness h;
  auto a = h.add("ana");
  auto b = h.add("bia", {15'000, 0, 0});  // 150 m → Mid
  int count = 0;
  for (int i = 0; i < 8; ++i) {
    h.step();
    if (h.entityIn(a, b)) ++count;
  }
  REQUIRE(count == 4);
}

namespace {
std::vector<std::pair<cbserver::EntityId, std::vector<uint8_t>>> runScenario(unsigned threads) {
  Harness h(ServerConfig{}, threads);
  REQUIRE(h.pool.workers() == threads);
  uint32_t rng = 12345;
  auto next = [&rng] {
    rng = rng * 1103515245u + 12345u;
    return static_cast<int32_t>((rng >> 8) % 60000) - 30000;
  };
  std::vector<cbserver::EntityId> ids;
  for (int i = 0; i < 80; ++i) ids.push_back(h.add("p" + std::to_string(i), {next(), next(), 0}));
  std::vector<std::pair<cbserver::EntityId, std::vector<uint8_t>>> all;
  for (uint32_t t = 1; t <= 12; ++t) {
    for (size_t k = 0; k < ids.size(); k += 3) {
      const auto& st = h.world.find(ids[k])->state;
      h.world.onInput(ids[k], input(t, {st.pos.x + 30, st.pos.y, 0}), h.now + 0.05);
    }
    h.step();
    if (t % 2 == 0)
      for (size_t k = 0; k < ids.size(); k += 2) h.world.onSnapshotAck(ids[k], t - 1, h.now);
    std::vector<std::pair<cbserver::EntityId, std::vector<uint8_t>>> tickOut;
    for (const auto& o : h.out) tickOut.emplace_back(o.to, cbmp::encode(o.msg));
    std::stable_sort(tickOut.begin(), tickOut.end(),
                     [](const auto& x, const auto& y) { return x.first < y.first; });
    all.insert(all.end(), tickOut.begin(), tickOut.end());
  }
  return all;
}
}  // namespace

TEST_CASE("snapshot build with 1 worker equals build with 4 workers") {
  auto one = runScenario(1);
  auto four = runScenario(4);
  REQUIRE(one.size() > 80 * 12);
  REQUIRE(one == four);
}

TEST_CASE("task pool rethrows worker exception and stays usable") {
  cbserver::TaskPool pool(4);
  REQUIRE_THROWS_AS(pool.parallelFor(100,
                                     [](size_t b, size_t e, unsigned) {
                                       for (size_t i = b; i < e; ++i)
                                         if (i == 77) throw std::runtime_error("falha");
                                     }),
                    std::runtime_error);
  std::atomic<size_t> sum{0};
  pool.parallelFor(100, [&](size_t b, size_t e, unsigned) { sum += e - b; });
  REQUIRE(sum.load() == 100);
}

TEST_CASE("snapshot ring entry is not left claiming a tick after an exception") {
  cbserver::TaskPool pool(2);
  cbserver::SnapshotBuilder builder(pool);
  cbserver::PlayerSlot s;
  s.id = 1;
  s.visible.ids = {2};
  s.sent[5].tick = 5;  // registro antigo no mesmo índice que o tick 37
  s.sent[5].ents.push_back(cbserver::SentEntity{2, {}});
  std::vector<cbserver::PlayerSlot*> slots{&s};
  std::vector<cbserver::Outgoing> out;
  auto throwing = [](cbserver::EntityId) -> const cbmp::PlayerState& { throw std::runtime_error("estado"); };
  REQUIRE_THROWS(builder.build(37, slots, throwing, out));
  REQUIRE(s.sent[5].tick == 0);  // nem 37 (incompleto) nem 5 (já apagado)
  REQUIRE(out.empty());
}

TEST_CASE("snapshot with 128 full entities is split in parts under 1100 bytes") {
  Harness h;
  std::vector<cbserver::EntityId> ids;
  for (int i = 0; i < 129; ++i)
    ids.push_back(h.add("p" + std::to_string(i), {(i % 12) * 300, (i / 12) * 300, 0}));
  h.step();
  auto parts = h.snaps(ids[0]);
  REQUIRE(parts.size() >= 2);
  std::vector<uint16_t> got;
  for (size_t i = 0; i < parts.size(); ++i) {
    REQUIRE(cbmp::encode(cbmp::Message{parts[i]}).size() <= cbmp::kMaxSnapshotBytes);
    REQUIRE(parts[i].partIndex == i);
    REQUIRE(parts[i].partCount == parts.size());
    REQUIRE(parts[i].tick == 1);
    for (const auto& e : parts[i].ents) got.push_back(e.id);
  }
  REQUIRE(got.size() == 128);
  REQUIRE(std::is_sorted(got.begin(), got.end()));
}

TEST_CASE("task pool covers every index exactly once") {
  cbserver::TaskPool pool(4);
  for (size_t n : {0u, 1u, 3u, 17u, 1000u}) {
    std::vector<int> hits(n, 0);
    std::atomic<unsigned> maxWorker{0};
    pool.parallelFor(n, [&](size_t b, size_t e, unsigned w) {
      unsigned cur = maxWorker.load();
      while (w > cur && !maxWorker.compare_exchange_weak(cur, w)) {
      }
      for (size_t i = b; i < e; ++i) ++hits[i];
    });
    REQUIRE(maxWorker.load() < pool.workers());
    REQUIRE(std::all_of(hits.begin(), hits.end(), [](int x) { return x == 1; }));
  }
}

TEST_CASE("snapshot bandwidth in CI-like crowd stays under 50 kbps per player") {
  Harness h;
  constexpr int kPlayers = 256, kCluster = 77, kTicks = 120;
  uint32_t rng = 2024;
  auto rnd = [&rng] {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<double>(rng >> 8) / static_cast<double>(1u << 24);  // [0,1)
  };
  struct Mover { cbserver::EntityId id; double x, y, heading; uint32_t seq; };
  std::vector<Mover> m;
  for (int i = 0; i < kPlayers; ++i) {
    double x, y;
    if (i < kCluster) {  // círculo de 30 m
      const double r = 3000.0 * std::sqrt(rnd()), a = rnd() * 6.283185307179586;
      x = r * std::cos(a);
      y = r * std::sin(a);
    } else {  // uniforme em 2 km × 2 km
      x = (rnd() - 0.5) * 200'000.0;
      y = (rnd() - 0.5) * 200'000.0;
    }
    const auto id = h.add("p" + std::to_string(i), {static_cast<int32_t>(x), static_cast<int32_t>(y), 0});
    m.push_back({id, x, y, rnd() * 6.283185307179586, 0});
  }
  std::vector<uint64_t> bytes(kPlayers + 1, 0);
  size_t maxPart = 0;
  for (int t = 1; t <= kTicks; ++t) {
    for (auto& p : m) {  // anda a 400 cm/s mudando de direção todo tick
      p.heading += (rnd() - 0.5) * 0.6;
      const double vx = 400.0 * std::cos(p.heading), vy = 400.0 * std::sin(p.heading);
      p.x += vx * 0.05;
      p.y += vy * 0.05;
      cbmp::Input in;
      in.seq = ++p.seq;
      in.s.pos = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), 0};
      in.s.vel = {static_cast<int16_t>(vx), static_cast<int16_t>(vy), 0};
      in.s.yaw = cbmp::quantizeYaw(static_cast<float>(p.heading * 57.29577951308232));
      in.s.state = cbmp::MoveState::Walk;
      h.world.onInput(p.id, in, h.now + 0.05);
    }
    h.step();
    for (const auto& o : h.out)
      if (const auto* s = std::get_if<cbmp::Snapshot>(&o.msg)) {
        const size_t n = cbmp::encode(o.msg).size();
        bytes[o.to] += n;
        maxPart = std::max(maxPart, n);
        (void)s;
      }
    if (t > 3)  // acks chegam 3 ticks atrasados
      for (const auto& p : m) h.world.onSnapshotAck(p.id, static_cast<uint32_t>(t - 3), h.now);
  }
  const double secs = kTicks / 20.0;
  double total = 0, cluster = 0;
  for (int i = 0; i < kPlayers; ++i) {
    const double kbps = static_cast<double>(bytes[m[static_cast<size_t>(i)].id]) * 8.0 / 1000.0 / secs;
    total += kbps;
    if (i < kCluster) cluster += kbps;
  }
  const double avg = total / kPlayers, avgCluster = cluster / kCluster;
  std::printf("banda snapshots: media %.1f kbps/jogador, aglomerado %.1f kbps, maior parte %zu B\n", avg, avgCluster,
              maxPart);
  REQUIRE(maxPart <= cbmp::kMaxSnapshotBytes);
  REQUIRE(avg < 50.0);
}
