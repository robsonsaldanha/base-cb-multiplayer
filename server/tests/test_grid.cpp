#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cbserver/grid.hpp"

using cbserver::EntityId;
using cbserver::Grid;

namespace {
bool has(const std::vector<EntityId>& v, EntityId id) { return std::find(v.begin(), v.end(), id) != v.end(); }
}  // namespace

TEST_CASE("grid upsert move and remove") {
  Grid g(6400);
  g.upsert(1, {100, 100, 0});
  g.upsert(2, {200, 200, 0});
  REQUIRE(g.size() == 2);
  std::vector<EntityId> out;
  g.query({0, 0, 0}, 1000, out);
  REQUIRE(has(out, 1));
  REQUIRE(has(out, 2));

  g.upsert(1, {1'000'000, 0, 0});  // muda de célula
  REQUIRE(g.size() == 2);
  g.query({0, 0, 0}, 1000, out);
  REQUIRE_FALSE(has(out, 1));
  REQUIRE(has(out, 2));
  g.query({1'000'000, 0, 0}, 1000, out);
  REQUIRE(has(out, 1));
  REQUIRE(std::count(out.begin(), out.end(), EntityId{1}) == 1);

  g.remove(2);
  g.remove(2);  // idempotente
  REQUIRE(g.size() == 1);
  g.query({0, 0, 0}, 1000, out);
  REQUIRE(out.empty());
}

TEST_CASE("grid query returns adjacent cells but not far cells") {
  Grid g(6400);
  g.upsert(1, {6500, 0, 0});      // célula (1,0), vizinha
  g.upsert(2, {5000, -100, 0});   // célula (0,-1), vizinha
  g.upsert(3, {64000, 0, 0});     // célula (10,0), longe
  g.upsert(4, {0, -64000, 0});    // célula (0,-10), longe
  std::vector<EntityId> out;
  g.query({6000, 100, 0}, 1000, out);  // quadrado [5000,7000]x[-900,1100] → células x 0..1, y -1..0
  REQUIRE(has(out, 1));
  REQUIRE(has(out, 2));
  REQUIRE_FALSE(has(out, 3));
  REQUIRE_FALSE(has(out, 4));
}

TEST_CASE("grid entity exactly on cell border belongs to upper cell") {
  Grid g(6400);
  g.upsert(1, {6400, 0, 0});  // primeira posição da célula 1
  std::vector<EntityId> out;
  g.query({100, 100, 0}, 0, out);  // só célula 0
  REQUIRE_FALSE(has(out, 1));
  g.query({12799, 100, 0}, 0, out);  // só célula 1
  REQUIRE(has(out, 1));
  g.query({6399, 100, 0}, 1, out);  // células 0 e 1
  REQUIRE(has(out, 1));

  g.upsert(2, {-6400, 0, 0});  // célula -1 (divisão com piso)
  g.query({0, 100, 0}, 0, out);
  REQUIRE_FALSE(has(out, 2));
  g.query({-6401, 100, 0}, 0, out);  // célula -2
  REQUIRE_FALSE(has(out, 2));
  g.query({-1, 100, 0}, 0, out);  // célula -1
  REQUIRE(has(out, 2));
}
