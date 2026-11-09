#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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
  REQUIRE(s1[0].ents[0].s.pos.x == 1000);

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
    REQUIRE(s3[0].ents[0].s.pos.x == 1040);
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
