#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

#include <sodium.h>

#include <cbmp/base64url.hpp>
#include <cbmp/ticket.hpp>

#include "bot.hpp"

TEST_CASE("bots tickets carry the requested kid and verify with that key") {
  REQUIRE(sodium_init() >= 0);
  std::array<uint8_t, 32> seed{};
  for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i + 1);
  std::array<uint8_t, crypto_sign_PUBLICKEYBYTES> pk{};
  std::array<uint8_t, crypto_sign_SECRETKEYBYTES> sk{};
  crypto_sign_seed_keypair(pk.data(), sk.data(), seed.data());
  const int64_t now = 1800000000;

  const auto dev = cbbots::makeTicket(seed, 3, "srv_x", now, "j1");
  auto r = cbmp::verifyTicket(dev, {{"dev", pk}}, "srv_x", now);
  REQUIRE(r);
  REQUIRE(r->kid == "dev");
  REQUIRE(r->did == "90000000000000003");

  const auto hub = cbbots::makeTicket(seed, 3, "srv_x", now, "j2", "k20261110000000");
  REQUIRE(!cbmp::verifyTicket(hub, {{"dev", pk}}, "srv_x", now));
  auto h = cbmp::verifyTicket(hub, {{"k20261110000000", pk}}, "srv_x", now);
  REQUIRE(h);
  REQUIRE(h->kid == "k20261110000000");
}
