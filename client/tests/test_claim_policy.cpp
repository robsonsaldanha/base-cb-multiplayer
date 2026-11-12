#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/ClaimPolicy.hpp>
#include "FakeGate.hpp"

using namespace cbmp::client;

TEST_CASE("client: DecideClaim substituido vence tudo") {
    CHECK(DecideClaim(false, false, 0.0, 1) == ClaimDecision::Superseded);
    CHECK(DecideClaim(false, true, 100.0, 1) == ClaimDecision::Superseded);
    CHECK(DecideClaim(false, true, 100.0, 3) == ClaimDecision::Superseded);
}

TEST_CASE("client: DecideClaim InWorld resgata na hora") {
    CHECK(DecideClaim(true, true, 0.0, 1) == ClaimDecision::Claim);
    CHECK(DecideClaim(true, true, 0.0, 2) == ClaimDecision::Claim);
}

TEST_CASE("client: DecideClaim epoca 1 espera 45 s") {
    CHECK(DecideClaim(true, false, 0.0, 1) == ClaimDecision::Wait);
    CHECK(DecideClaim(true, false, 5.0, 1) == ClaimDecision::Wait);
    CHECK(DecideClaim(true, false, 44.9, 1) == ClaimDecision::Wait);
    CHECK(DecideClaim(true, false, 45.0, 1) == ClaimDecision::Claim);
    CHECK(DecideClaim(true, false, 300.0, 1) == ClaimDecision::Claim);
}

TEST_CASE("client: DecideClaim epoca maior que 1 espera 5 s") {
    CHECK(DecideClaim(true, false, 0.0, 2) == ClaimDecision::Wait);
    CHECK(DecideClaim(true, false, 4.9, 2) == ClaimDecision::Wait);
    CHECK(DecideClaim(true, false, 5.0, 2) == ClaimDecision::Claim);
    CHECK(DecideClaim(true, false, 4.9, 7) == ClaimDecision::Wait);
    CHECK(DecideClaim(true, false, 5.0, 7) == ClaimDecision::Claim);
}

TEST_CASE("client: FakeGate e FakeConnectFs") {
    FakeGate g;
    CHECK(g.Epoch() == 1);
    CHECK(g.IsCurrent());
    g.latest = 2;
    CHECK(g.Latest() == 2);
    CHECK_FALSE(g.IsCurrent());
    CHECK(g.TryOwn());
    g.ownOk = false;
    CHECK_FALSE(g.TryOwn());
    CHECK(g.ownCalls == 2);

    FakeConnectFs fs;
    CHECK(fs.Exists());
    fs.results.push_back({ClaimRead::Busy, {}});
    fs.results.push_back({ClaimRead::Ok, {1, 2, 3}});
    CHECK(fs.ClaimAndDelete().status == ClaimRead::Busy);
    const auto ok = fs.ClaimAndDelete();
    CHECK(ok.status == ClaimRead::Ok);
    CHECK(ok.bytes == std::vector<uint8_t>{1, 2, 3});
    CHECK(fs.ClaimAndDelete().status == ClaimRead::Missing);
    CHECK(fs.claimCalls == 3);
}
