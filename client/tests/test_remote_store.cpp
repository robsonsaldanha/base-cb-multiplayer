#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/RemoteStore.hpp>

#include <cstdint>
#include <string>
#include <utility>

using namespace cbmp::client;

namespace {
cbmp::EntityEnter Enter(uint16_t id, std::string name, int32_t x = 0) {
    cbmp::EntityEnter e{};
    e.id = id;
    e.name = std::move(name);
    e.s.pos.x = x;
    return e;
}
}  // namespace

TEST_CASE("client: RemoteStore enter, leave, known e update") {
    RemoteStore rs;
    REQUIRE_FALSE(rs.Known(4));
    REQUIRE(rs.Enter(Enter(4, "Hermione", 100), 1.5));
    REQUIRE(rs.Known(4));
    const auto& e = rs.All().at(4);
    REQUIRE(e.id == 4);
    REQUIRE(e.name == "Hermione");
    REQUIRE(e.last.pos.x == 100);
    REQUIRE(e.lastAt == 1.5);

    cbmp::PlayerState s{};
    s.pos.x = 250;
    rs.Update(4, s, 2.0);
    REQUIRE(rs.All().at(4).last.pos.x == 250);
    REQUIRE(rs.All().at(4).lastAt == 2.0);

    rs.Update(9, s, 2.0);  // desconhecido: ignora
    REQUIRE_FALSE(rs.Known(9));
    REQUIRE(rs.All().size() == 1);

    // reentrada do mesmo id substitui
    REQUIRE(rs.Enter(Enter(4, "Rony", 7), 3.0));
    REQUIRE(rs.All().size() == 1);
    REQUIRE(rs.All().at(4).name == "Rony");
    REQUIRE(rs.All().at(4).last.pos.x == 7);

    REQUIRE(rs.Leave(4));
    REQUIRE_FALSE(rs.Leave(4));
    REQUIRE_FALSE(rs.Known(4));
    REQUIRE(rs.All().empty());
}

TEST_CASE("client: RemoteStore recusa o 256o Enter") {
    RemoteStore rs;
    for (uint16_t id = 1; id <= 255; ++id) REQUIRE(rs.Enter(Enter(id, "x"), 0));
    REQUIRE_FALSE(rs.Enter(Enter(1000, "y"), 0));
    REQUIRE_FALSE(rs.Known(1000));
    REQUIRE(rs.Enter(Enter(10, "z"), 0));  // reentrada com o store cheio continua valendo
    REQUIRE(rs.All().at(10).name == "z");
    rs.Clear();
    REQUIRE(rs.All().empty());
    REQUIRE(rs.Enter(Enter(1000, "y"), 0));
}

TEST_CASE("client: RemoteStore escapa controles e corta o nome em 64 bytes") {
    RemoteStore rs;
    REQUIRE(rs.Enter(Enter(1, "Dra\nco\x1b"), 0));
    REQUIRE(rs.All().at(1).name == "Dra\\nco\\x1b");
    REQUIRE(rs.Enter(Enter(2, std::string(64, 'a')), 0));
    REQUIRE(rs.All().at(2).name.size() == 64);
    REQUIRE(rs.Enter(Enter(3, std::string(60, 'a') + "\n\n\n"), 0));
    REQUIRE(rs.All().at(3).name == std::string(60, 'a') + "\\n\\n");
}
