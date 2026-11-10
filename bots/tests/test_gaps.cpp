#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "gaps.hpp"

using Catch::Approx;
using cbbots::GapStats;

TEST_CASE("bots tail gap covers a bot that never got a snapshot") {
  // Welcome em 1 s, nenhum snapshot, sessão até 31 s → 30 s sem snapshot
  REQUIRE(GapStats::tailGap(1.0, -1.0, 31.0) == Approx(30.0));
}

TEST_CASE("bots tail gap covers a bot starved after its last snapshot") {
  GapStats g;
  for (int i = 1; i <= 100; ++i) g.add(0.05);
  g.add(GapStats::tailGap(1.0, 6.0, 31.0));  // último snapshot em 6 s, fim em 31 s
  REQUIRE(g.maxSec() == Approx(25.0));
  REQUIRE(g.count() == 101);
  REQUIRE(g.p99Ms() == Approx(50.0));
}

TEST_CASE("bots gap p99 from histogram") {
  GapStats g;
  for (int i = 0; i < 99; ++i) g.add(0.05);
  g.add(2.0);
  REQUIRE(g.p99Ms() == Approx(50.0));
  g.add(2.0);
  REQUIRE(g.p99Ms() == Approx(2000.0));
  g.add(99.0);  // satura no último balde
  REQUIRE(g.maxSec() == Approx(99.0));
}
