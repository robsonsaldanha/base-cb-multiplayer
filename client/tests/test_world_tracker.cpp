#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/WorldTracker.hpp>

#include <optional>
#include <string>
#include <utility>

using namespace cbmp::client;

namespace {
WorldSample World(uint64_t worldId, std::string name, uint64_t pawnId, bool hasPawn = true, bool hint = false) {
    WorldSample w{};
    w.worldId = worldId;
    w.worldName = std::move(name);
    w.pawnId = pawnId;
    w.hasPawn = hasPawn;
    w.frontendHint = hint;
    return w;
}

std::optional<LocalState> At(double x, double y, double z) {
    LocalState s{};
    s.pos = {x, y, z};
    s.mode = MoveMode::Ground;
    return s;
}
}  // namespace

TEST_CASE("client: IsMenuWorld reconhece so o RootLevel") {
    REQUIRE(IsMenuWorld("RootLevel"));
    REQUIRE_FALSE(IsMenuWorld("Overland"));
    REQUIRE_FALSE(IsMenuWorld(""));
}

TEST_CASE("client: WorldTracker sem pawn fica em Loading") {
    WorldTracker wt;
    REQUIRE(wt.Update(World(1, "Overland", 0, false), std::nullopt, 0.0) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 0, false), std::nullopt, 10.0) == WorldPhase::Loading);
    // pawn presente mas sem LocalState utilizável
    REQUIRE(wt.Update(World(1, "Overland", 10), std::nullopt, 20.0) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 10), std::nullopt, 30.0) == WorldPhase::Loading);
    REQUIRE(wt.Phase() == WorldPhase::Loading);
    REQUIRE(wt.WorldName() == "Overland");
}

TEST_CASE("client: WorldTracker RootLevel e frontendHint sao Frontend") {
    WorldTracker wt;
    REQUIRE(wt.Update(World(1, "RootLevel", 10), At(5000, 0, 0), 0.0) == WorldPhase::Frontend);
    REQUIRE(wt.Update(World(1, "RootLevel", 10), At(5000, 0, 0), 5.0) == WorldPhase::Frontend);
    REQUIRE(wt.WorldName() == "RootLevel");

    WorldTracker hint;
    REQUIRE(hint.Update(World(2, "Overland", 10, true, true), At(5000, 0, 0), 0.0) == WorldPhase::Frontend);
    REQUIRE(hint.Update(World(2, "Overland", 10, true, true), At(5000, 0, 0), 5.0) == WorldPhase::Frontend);
    REQUIRE(hint.Phase() == WorldPhase::Frontend);
}

TEST_CASE("client: WorldTracker pawn perto da origem fica em Loading") {
    WorldTracker wt;
    REQUIRE(wt.Update(World(1, "Overland", 10), At(0, 0, 90), 0.0) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 10), At(0, 0, 90), 5.0) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 10), At(70, 70, -5000), 6.0) == WorldPhase::Loading);  // hypot 98,99
    REQUIRE(wt.Update(World(1, "Overland", 10), At(80, 80, 0), 7.0) == WorldPhase::InWorld);      // hypot 113
}

TEST_CASE("client: WorldTracker InWorld so depois de 2 s estavel") {
    WorldTracker wt;
    REQUIRE(wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 10.0) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 11.9) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 12.0) == WorldPhase::InWorld);
    REQUIRE(wt.Phase() == WorldPhase::InWorld);
    REQUIRE(wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 30.0) == WorldPhase::InWorld);

    // primeira amostra não é mudança
    const auto c = wt.TakeChange();
    REQUIRE_FALSE(c.worldChanged);
    REQUIRE_FALSE(c.pawnChanged);
}

TEST_CASE("client: WorldTracker troca de pawn volta a Loading e marca pawnChanged") {
    WorldTracker wt;
    wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 0.0);
    REQUIRE(wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 2.0) == WorldPhase::InWorld);
    REQUIRE(wt.Update(World(1, "Overland", 11), At(5000, 0, 0), 3.0) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 11), At(5000, 0, 0), 4.9) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(1, "Overland", 11), At(5000, 0, 0), 5.0) == WorldPhase::InWorld);
    const auto c = wt.TakeChange();
    REQUIRE(c.pawnChanged);
    REQUIRE_FALSE(c.worldChanged);
    // TakeChange zera
    const auto z = wt.TakeChange();
    REQUIRE_FALSE(z.pawnChanged);
    REQUIRE_FALSE(z.worldChanged);
}

TEST_CASE("client: WorldTracker troca de mundo marca worldChanged e reinicia os 2 s") {
    WorldTracker wt;
    wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 0.0);
    REQUIRE(wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 2.0) == WorldPhase::InWorld);
    REQUIRE(wt.Update(World(2, "Hogwarts", 10), At(5000, 0, 0), 3.0) == WorldPhase::Loading);
    REQUIRE(wt.WorldName() == "Hogwarts");
    const auto c = wt.TakeChange();
    REQUIRE(c.worldChanged);
    REQUIRE_FALSE(c.pawnChanged);
    REQUIRE(wt.Update(World(2, "Hogwarts", 10), At(5000, 0, 0), 4.0) == WorldPhase::Loading);
    REQUIRE(wt.Update(World(2, "Hogwarts", 10), At(5000, 0, 0), 5.0) == WorldPhase::InWorld);
    REQUIRE_FALSE(wt.TakeChange().worldChanged);

    // mundo e pawn trocados juntos: conta como troca de mundo
    wt.Update(World(3, "Overland", 12), At(5000, 0, 0), 6.0);
    const auto d = wt.TakeChange();
    REQUIRE(d.worldChanged);
}

TEST_CASE("client: WorldTracker acumula mudancas ate o TakeChange") {
    WorldTracker wt;
    wt.Update(World(1, "Overland", 10), At(5000, 0, 0), 0.0);
    wt.Update(World(2, "Overland", 10), At(5000, 0, 0), 1.0);  // mundo
    wt.Update(World(2, "Overland", 11), At(5000, 0, 0), 2.0);  // pawn
    const auto c = wt.TakeChange();
    REQUIRE(c.worldChanged);
    REQUIRE(c.pawnChanged);
}
