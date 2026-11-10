#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cbserver/auth.hpp"
#include "ticket_helper.hpp"

using cbserver::Authenticator;
using Catch::Matchers::ContainsSubstring;
using testing_ticket::DevSigner;

namespace {
struct FakeKeys : cbserver::KeyProvider {
  cbmp::PublicKeys k;
  int refreshes = 0;
  cbmp::PublicKeys keys() override { return k; }
  void requestRefresh() override { ++refreshes; }
};
constexpr int64_t kNow = 1'900'000'000;
}  // namespace

TEST_CASE("auth valid ticket is accepted and returns the payload") {
  DevSigner dev;
  FakeKeys keys;
  keys.k = dev.keys();
  Authenticator auth(keys, "srv_dev", [] { return kNow; });
  auto r = auth.check(dev.sign("u1", "j1", kNow));
  REQUIRE(r);
  REQUIRE(r->sub == "u1");
  REQUIRE(r->did == "did-u1");
  REQUIRE(r->aud == "srv_dev");
  REQUIRE(keys.refreshes == 0);
}

TEST_CASE("auth repeated jti is refused") {
  DevSigner dev;
  FakeKeys keys;
  keys.k = dev.keys();
  Authenticator auth(keys, "srv_dev", [] { return kNow; });
  const auto t = dev.sign("u1", "j1", kNow);
  REQUIRE(auth.check(t));
  auto again = auth.check(t);
  REQUIRE_FALSE(again);
  REQUIRE_THAT(again.error(), ContainsSubstring("já usado"));
  REQUIRE(auth.check(dev.sign("u1", "j2", kNow)));  // outro jti passa
}

TEST_CASE("auth unknown kid is refused and asks the provider to refresh") {
  DevSigner dev;
  FakeKeys keys;
  keys.k = dev.keys("outra");
  Authenticator auth(keys, "srv_dev", [] { return kNow; });
  auto r = auth.check(dev.sign("u1", "j1", kNow));
  REQUIRE_FALSE(r);
  REQUIRE_THAT(r.error(), ContainsSubstring("unknown_kid"));
  REQUIRE(keys.refreshes == 1);
}

TEST_CASE("auth wrong audience is refused without consuming the jti") {
  DevSigner dev;
  FakeKeys keys;
  keys.k = dev.keys();
  Authenticator auth(keys, "srv_dev", [] { return kNow; });
  auto r = auth.check(dev.sign("u1", "j1", kNow, "srv_outro"));
  REQUIRE_FALSE(r);
  REQUIRE_THAT(r.error(), ContainsSubstring("wrong_audience"));
  REQUIRE(keys.refreshes == 0);
  REQUIRE(auth.check(dev.sign("u1", "j1", kNow)));
}

TEST_CASE("auth expired ticket is refused") {
  DevSigner dev;
  FakeKeys keys;
  keys.k = dev.keys();
  Authenticator auth(keys, "srv_dev", [] { return kNow + 1000; });
  auto r = auth.check(dev.sign("u1", "j1", kNow));
  REQUIRE_FALSE(r);
  REQUIRE_THAT(r.error(), ContainsSubstring("expired"));
}

TEST_CASE("auth StaticKeys from devPublicKey uses kid dev") {
  DevSigner dev;
  cbserver::StaticKeys keys(cbserver::StaticKeys::fromDevKey(dev.publicKeyB64()));
  REQUIRE(keys.keys().size() == 1);
  REQUIRE(keys.keys().at("dev") == dev.pk);
  keys.requestRefresh();  // não faz nada
  Authenticator auth(keys, "srv_dev", [] { return kNow; });
  REQUIRE(auth.check(dev.sign("u1", "j1", kNow)));
  REQUIRE_THROWS(cbserver::StaticKeys::fromDevKey("curta"));
}

TEST_CASE("auth admin ticket is refused by game verification and game ticket by admin audience") {
  DevSigner dev;
  FakeKeys keys;
  keys.k = dev.keys();
  Authenticator game(keys, "srv_dev", [] { return kNow; });
  auto r = game.check(dev.sign("u1", "ja", kNow, "srv_dev#admin"));
  REQUIRE_FALSE(r);
  REQUIRE_THAT(r.error(), ContainsSubstring("wrong_audience"));
  // O inverso: ticket de jogo nao vale para a audiencia do painel admin.
  auto a = cbmp::verifyTicket(dev.sign("u1", "jg", kNow), dev.keys(), "srv_dev#admin", kNow);
  REQUIRE_FALSE(a);
  REQUIRE(a.error() == cbmp::TicketError::WrongAudience);
  auto ok = cbmp::verifyTicket(dev.sign("u1", "jb", kNow, "srv_dev#admin"), dev.keys(), "srv_dev#admin", kNow);
  REQUIRE(ok);
  REQUIRE(ok->aud == "srv_dev#admin");
}
