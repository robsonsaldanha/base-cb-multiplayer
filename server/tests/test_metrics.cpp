#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "cbserver/metrics.hpp"

using cbserver::Metrics;
using Catch::Approx;
using namespace std::chrono_literals;

TEST_CASE("metrics tick percentiles over 1200 samples") {
  Metrics m;
  for (int i = 1; i <= 1200; ++i) m.recordTick(std::chrono::microseconds(i));
  auto s = m.snapshot();
  REQUIRE(s["tickP50Ms"].get<double>() == Approx(0.6).margin(0.01));
  REQUIRE(s["tickP99Ms"].get<double>() == Approx(1.188).margin(0.01));
  REQUIRE(s["tickMaxMs"].get<double>() == Approx(1.2).margin(0.001));
}

TEST_CASE("metrics window drops old samples") {
  Metrics m;
  m.recordTick(50000us);
  for (size_t i = 0; i < Metrics::kWindow; ++i) m.recordTick(100us);
  auto s = m.snapshot();
  REQUIRE(s["tickMaxMs"].get<double>() == Approx(0.1).margin(0.001));
}

TEST_CASE("metrics counters bandwidth and process stats") {
  Metrics m;
  m.players = 2;
  m.kicks = 1;
  m.corrections = 3;
  m.addBytesOut(1, 1000);
  m.addBytesOut(2, 3000);
  auto s = m.snapshot();
  for (auto k : {"tickP50Ms", "tickP99Ms", "tickMaxMs", "players", "pending", "corrections", "kicks",
                 "droppedEvents", "avgKbpsPerPlayer", "maxKbpsPerPlayer", "rssBytes", "cpuPercent"})
    REQUIRE(s.contains(k));
  REQUIRE(s["players"] == 2);
  REQUIRE(s["kicks"] == 1);
  REQUIRE(s["corrections"] == 3);
  REQUIRE(s["maxKbpsPerPlayer"].get<double>() >= s["avgKbpsPerPlayer"].get<double>());
  REQUIRE(s["maxKbpsPerPlayer"].get<double>() > 0);
  REQUIRE(s["rssBytes"].is_number_unsigned());
  REQUIRE(s["rssBytes"].get<double>() > 0);
}

TEST_CASE("metrics empty snapshot is zeros") {
  Metrics m;
  auto s = m.snapshot();
  REQUIRE(s["tickP99Ms"].get<double>() == 0);
}

TEST_CASE("metrics bandwidth is per connection and ends on removePlayer") {
  Metrics m;
  m.addBytesOut(1, 1000);  // < 1 s de conexão: divide por 1 s → 8 kbps
  m.addBytesOut(2, 3000);  // 24 kbps
  auto s = m.snapshot();
  REQUIRE(s["avgKbpsPerPlayer"].get<double>() == Approx(16.0).margin(0.01));
  REQUIRE(s["maxKbpsPerPlayer"].get<double>() == Approx(24.0).margin(0.01));
  m.removePlayer(2);
  s = m.snapshot();
  REQUIRE(s["maxKbpsPerPlayer"].get<double>() == Approx(8.0).margin(0.01));
  m.removePlayer(1);
  m.setWireOut(30.0, 40.0);
  s = m.snapshot();
  REQUIRE(s["avgKbpsPerPlayer"].get<double>() == 0);
  REQUIRE(s["wireAvgKbpsPerPlayer"].get<double>() == 30.0);
  REQUIRE(s["wireMaxKbpsPerPlayer"].get<double>() == 40.0);
}
