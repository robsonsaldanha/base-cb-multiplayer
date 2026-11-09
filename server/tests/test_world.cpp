#include <catch2/catch_test_macros.hpp>

#include "world_harness.hpp"

using cbmp::Correction;
using cbmp::EntityEnter;
using cbmp::EntityLeave;
using cbmp::Event;
using cbserver::ServerConfig;
using testing_world::Harness;
using testing_world::input;

TEST_CASE("world two players 10 m apart get EntityEnter of each other on first tick") {
  Harness h;
  auto a = h.add("ana", {0, 0, 0});
  auto b = h.add("bia", {1000, 0, 0});
  REQUIRE(h.world.playerCount() == 2);
  h.step();
  REQUIRE(h.world.currentTick() == 1);
  auto ea = h.msgs<EntityEnter>(a);
  REQUIRE(ea.size() == 1);
  REQUIRE(ea[0].id == b);
  REQUIRE(ea[0].name == "bia");
  REQUIRE(ea[0].s.pos.x == 1000);
  auto eb = h.msgs<EntityEnter>(b);
  REQUIRE(eb.size() == 1);
  REQUIRE(eb[0].id == a);
  // não repete no tick seguinte
  h.step();
  REQUIRE(h.msgs<EntityEnter>(a).empty());
}

TEST_CASE("world moving a player 400 m away emits EntityLeave") {
  Harness h;
  auto a = h.add("ana");
  auto b = h.add("bia", {1000, 0, 0});
  h.step();
  cbmp::PlayerState far;
  far.pos = {40'000, 0, 0};
  h.world.setState(b, far, h.now);
  h.step();
  auto la = h.msgs<EntityLeave>(a);
  REQUIRE(la.size() == 1);
  REQUIRE(la[0].id == b);
  REQUIRE(h.msgs<EntityLeave>(b).size() == 1);
  REQUIRE(h.entityIn(a, b) == nullptr);
}

TEST_CASE("world input with repeated or older seq is ignored") {
  Harness h;
  auto a = h.add("ana");
  h.step();
  h.world.onInput(a, input(5, {10, 0, 0}), h.now);
  REQUIRE(h.world.find(a)->state.pos.x == 10);
  REQUIRE(h.world.find(a)->lastInputSeq == 5);
  h.world.onInput(a, input(5, {20, 0, 0}), h.now + 0.05);
  h.world.onInput(a, input(4, {20, 0, 0}), h.now + 0.05);
  REQUIRE(h.world.find(a)->state.pos.x == 10);
  REQUIRE(h.world.find(a)->lastInputSeq == 5);
}

TEST_CASE("world teleport yields Correction and 11 in 10 s yield KickOrder") {
  Harness h;
  auto a = h.add("ana");
  h.step();
  h.out.clear();
  for (uint32_t i = 1; i <= 10; ++i) h.world.onInput(a, input(i, {100'000, 0, 0}), h.now + 0.05 * i);
  REQUIRE(h.world.find(a)->state.pos.x == 0);
  h.step(0.6);
  auto c = h.msgs<Correction>(a);
  REQUIRE(c.size() == 10);
  REQUIRE(c[0].seq == 1);
  REQUIRE(c[9].seq == 10);
  REQUIRE(c[9].pos.x == 0);
  REQUIRE(h.kicks.empty());

  h.world.onInput(a, input(11, {100'000, 0, 0}), h.now);
  h.step();
  REQUIRE(h.msgs<Correction>(a).size() == 1);
  REQUIRE(h.kicks.size() == 1);
  REQUIRE(h.kicks[0].who == a);
  REQUIRE(h.kicks[0].reason == "movimento inválido");
  h.step();
  REQUIRE(h.kicks.empty());  // ordem emitida uma única vez
}

TEST_CASE("world corrections older than 10 s do not count toward kick") {
  Harness h;
  auto a = h.add("ana");
  for (uint32_t i = 1; i <= 10; ++i) h.world.onInput(a, input(i, {100'000, 0, 0}), 0.1 * i);
  h.world.onInput(a, input(11, {100'000, 0, 0}), 11.0);
  h.step(11.0);
  REQUIRE(h.kicks.empty());
}

TEST_CASE("world event flood yields KickOrder") {
  ServerConfig cfg;
  cfg.eventsPerSecond = 20;
  cfg.eventsBurst = 40;
  Harness h(cfg);
  auto a = h.add("ana");
  Event e{"spam", {}};
  for (int i = 0; i < 40; ++i) REQUIRE(h.world.onEvent(a, e, 1.0));
  for (int i = 0; i < 120; ++i) REQUIRE_FALSE(h.world.onEvent(a, e, 1.0));
  h.step();
  REQUIRE(h.kicks.empty());
  REQUIRE_FALSE(h.world.onEvent(a, e, 1.0));  // 121ª violação
  h.step();
  REQUIRE(h.kicks.size() == 1);
  REQUIRE(h.kicks[0].who == a);
  REQUIRE(h.kicks[0].reason == "flood de eventos");
}

TEST_CASE("world addPlayer beyond maxPlayers returns nullopt and ids are reused") {
  ServerConfig cfg;
  cfg.maxPlayers = 2;
  Harness h(cfg);
  auto a = h.world.addPlayer("s1", "d1", "um", 0);
  auto b = h.world.addPlayer("s2", "d2", "dois", 0);
  REQUIRE(a == 1);
  REQUIRE(b == 2);
  REQUIRE_FALSE(h.world.addPlayer("s3", "d3", "tres", 0).has_value());
  h.world.removePlayer(1);
  REQUIRE(h.world.find(1) == nullptr);
  auto c = h.world.addPlayer("s3", "d3", "tres", 0);
  REQUIRE(c == 1);
  REQUIRE(h.world.find(1)->name == "tres");
  const auto& s = h.world.find(1)->state;
  REQUIRE(s.pos == cbmp::Vec3i{0, 0, 0});
  REQUIRE(s.vel == cbmp::Vec3s{});
  REQUIRE(s.state == cbmp::MoveState::Idle);
}

TEST_CASE("world echo event returns to sender only") {
  Harness h;
  auto a = h.add("ana");
  auto b = h.add("bia", {100, 0, 0});
  REQUIRE(h.world.onEvent(a, Event{"echo", {1, 2, 3}}, 0.0));
  REQUIRE(h.world.onEvent(a, Event{"outro", {9}}, 0.0));
  h.step();
  auto ea = h.msgs<Event>(a);
  REQUIRE(ea.size() == 1);
  REQUIRE(ea[0].name == "echo");
  REQUIRE(ea[0].payload == std::vector<uint8_t>{1, 2, 3});
  REQUIRE(h.msgs<Event>(b).empty());
}

TEST_CASE("world removed player id reused in same tick produces Leave then Enter") {
  Harness h;
  auto a = h.add("ana");
  auto b = h.add("bia", {100, 0, 0});
  h.step();
  h.world.removePlayer(b);
  auto c = h.add("caio", {200, 0, 0});
  REQUIRE(c == b);
  h.step();
  auto l = h.msgs<EntityLeave>(a);
  auto e = h.msgs<EntityEnter>(a);
  REQUIRE(l.size() == 1);
  REQUIRE(e.size() == 1);
  REQUIRE(e[0].name == "caio");
  // Leave chega antes do Enter
  size_t li = 0, ei = 0;
  for (size_t i = 0; i < h.out.size(); ++i) {
    if (h.out[i].to != a) continue;
    if (std::holds_alternative<EntityLeave>(h.out[i].msg)) li = i;
    if (std::holds_alternative<EntityEnter>(h.out[i].msg)) ei = i;
  }
  REQUIRE(li < ei);
}

TEST_CASE("world long names are truncated to protocol limit") {
  Harness h;
  auto id = h.world.addPlayer("s", "d", std::string(100, 'x'), 0);
  REQUIRE(h.world.find(*id)->name.size() == cbmp::kMaxName);
}
