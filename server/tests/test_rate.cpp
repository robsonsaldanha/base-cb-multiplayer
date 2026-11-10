#include <catch2/catch_test_macros.hpp>

#include "cbserver/rate.hpp"

using cbserver::TokenBucket;

TEST_CASE("token bucket accepts burst of 40 and refuses the 41st") {
  TokenBucket b(20, 40);
  for (int i = 0; i < 40; ++i) REQUIRE(b.take(0.0));
  REQUIRE_FALSE(b.take(0.0));
}

TEST_CASE("token bucket recovers 10 tokens after half a second at 20 per second") {
  TokenBucket b(20, 40);
  for (int i = 0; i < 40; ++i) REQUIRE(b.take(0.0));
  REQUIRE_FALSE(b.take(0.0));
  for (int i = 0; i < 10; ++i) REQUIRE(b.take(0.5));
  REQUIRE_FALSE(b.take(0.5));
}

TEST_CASE("token bucket never exceeds burst after long idle") {
  TokenBucket b(20, 40);
  REQUIRE(b.take(0.0));
  for (int i = 0; i < 40; ++i) REQUIRE(b.take(100.0));
  REQUIRE_FALSE(b.take(100.0));
}
