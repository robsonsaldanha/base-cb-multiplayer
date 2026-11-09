#include <algorithm>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cbserver/visibility.hpp"

using cbmp::Vec3i;
using cbserver::computeVisible;
using cbserver::EntityId;
using cbserver::Grid;
using cbserver::Tier;
using cbserver::tierFor;
using cbserver::VisibilityParams;
using cbserver::VisibleSet;

namespace {
struct Fixture {
  Grid grid;
  std::unordered_map<EntityId, Vec3i> pos;
  std::vector<EntityId> scratch;
  VisibilityParams params;
  void put(EntityId id, Vec3i p) {
    pos[id] = p;
    grid.upsert(id, p);
  }
  VisibleSet run(EntityId viewer, const VisibleSet& prev) {
    return computeVisible(viewer, pos.at(viewer), prev, grid,
                          [this](EntityId id) -> const Vec3i& { return pos.at(id); }, params, scratch);
  }
};
bool has(const VisibleSet& s, EntityId id) { return std::find(s.ids.begin(), s.ids.end(), id) != s.ids.end(); }
}  // namespace

TEST_CASE("visibility enters below 300 m and not at 310 m") {
  Fixture f;
  f.put(1, {0, 0, 0});
  f.put(2, {29'900, 0, 0});
  f.put(3, {0, 31'000, 0});
  auto v = f.run(1, {});
  REQUIRE(has(v, 2));
  REQUIRE_FALSE(has(v, 3));
}

TEST_CASE("visibility hysteresis keeps at 320 m and drops beyond 330 m") {
  Fixture f;
  f.put(1, {0, 0, 0});
  f.put(2, {32'000, 0, 0});
  f.put(3, {0, 34'000, 0});
  VisibleSet prev;
  prev.ids = {2, 3};
  auto v = f.run(1, prev);
  REQUIRE(has(v, 2));
  REQUIRE_FALSE(has(v, 3));
  // não estava visível: 320 m não basta para entrar
  auto fresh = f.run(1, {});
  REQUIRE_FALSE(has(fresh, 2));
}

TEST_CASE("visibility caps at 128 keeping the closest of 200 candidates") {
  Fixture f;
  f.put(1, {0, 0, 0});
  for (EntityId i = 0; i < 200; ++i) f.put(static_cast<EntityId>(10 + i), {100 * (i + 1), 0, 0});
  auto v = f.run(1, {});
  REQUIRE(v.ids.size() == 128);
  REQUIRE(std::is_sorted(v.ids.begin(), v.ids.end()));
  for (EntityId i = 0; i < 128; ++i) REQUIRE(has(v, static_cast<EntityId>(10 + i)));
  REQUIRE_FALSE(has(v, static_cast<EntityId>(10 + 128)));
}

TEST_CASE("visibility never includes the viewer itself") {
  Fixture f;
  f.put(1, {0, 0, 0});
  f.put(2, {10, 0, 0});
  VisibleSet prev;
  prev.ids = {1, 2};
  auto v = f.run(1, prev);
  REQUIRE_FALSE(has(v, 1));
  REQUIRE(has(v, 2));
}

TEST_CASE("visibility tier by distance") {
  auto sq = [](int64_t m) { return m * 100 * m * 100; };
  REQUIRE(tierFor(sq(99)) == Tier::Near);
  REQUIRE(tierFor(sq(150)) == Tier::Mid);
  REQUIRE(tierFor(sq(250)) == Tier::Far);
  REQUIRE(tierFor(sq(100)) == Tier::Mid);
  REQUIRE(tierFor(sq(200)) == Tier::Far);
}
