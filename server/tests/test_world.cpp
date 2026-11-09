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

TEST_CASE("world lag spike after one teleport is a single incident without kick") {
  Harness h;
  auto a = h.add("ana");
  h.world.onInput(a, input(1, {0, 0, 0}), 0.05);  // fecha a janela do spawn
  h.step();
  h.out.clear();
  // 1 teleporte + 5 inputs ainda baseados na posição errada (RTT 250 ms)
  for (uint32_t i = 2; i <= 7; ++i)
    h.world.onInput(a, input(i, {100'000 + 20 * static_cast<int32_t>(i), 0, 0}), 0.05 + 0.05 * i);
  REQUIRE(h.world.find(a)->state.pos.x == 0);
  REQUIRE(h.world.find(a)->lastInputSeq == 7);
  // cliente aplicou a Correction e volta perto da posição corrigida
  h.world.onInput(a, input(8, {10, 0, 0}), 0.45);
  REQUIRE(h.world.find(a)->state.pos.x == 10);
  h.step();
  auto c = h.msgs<Correction>(a);
  REQUIRE(c.size() == 1);
  REQUIRE(c[0].seq == 2);
  REQUIRE(c[0].pos.x == 0);
  REQUIRE(h.world.find(a)->correctionTimes.size() == 1);
  REQUIRE(h.kicks.empty());
}

TEST_CASE("world correction window ends after 1 s and counts a new incident") {
  Harness h;
  auto a = h.add("ana");
  h.step(0);  // descarta a Correction do spawn
  h.world.onInput(a, input(1, {0, 0, 0}), 0.0625);
  h.world.onInput(a, input(2, {100'000, 0, 0}), 0.125);
  h.world.onInput(a, input(3, {100'000, 0, 0}), 1.0);    // ainda na janela: ignorado
  h.world.onInput(a, input(4, {100'000, 0, 0}), 1.125);  // janela venceu: novo incidente
  h.step();
  REQUIRE(h.msgs<Correction>(a).size() == 2);
  REQUIRE(h.world.find(a)->correctionTimes.size() == 2);
}

TEST_CASE("world continuous cheater is kicked after 11 incidents in 10 s") {
  Harness h;
  auto a = h.add("ana");
  h.step(0);  // descarta a Correction do spawn
  h.world.onInput(a, input(1, {0, 0, 0}), 0.0);
  // 16 inputs/s (passo exato em binário) sempre teleportados: um incidente por segundo
  uint32_t seq = 2;
  double t = 0.0625;
  for (; t < 10.0; t += 0.0625) h.world.onInput(a, input(seq++, {100'000, 0, 0}), t);
  h.step();
  REQUIRE(h.msgs<Correction>(a).size() == 10);
  REQUIRE(h.kicks.empty());
  for (; t <= 10.0625; t += 0.0625) h.world.onInput(a, input(seq++, {100'000, 0, 0}), t);
  h.step();
  REQUIRE(h.msgs<Correction>(a).size() == 1);
  REQUIRE(h.kicks.size() == 1);
  REQUIRE(h.kicks[0].who == a);
  REQUIRE(h.kicks[0].reason == "movimento inválido");
  h.world.onInput(a, input(seq++, {100'000, 0, 0}), 12.0);
  h.step();
  REQUIRE(h.kicks.empty());  // ordem emitida uma única vez
}

TEST_CASE("world isolated teleports every 1.125 s count one incident each") {
  Harness h;
  auto a = h.add("ana");
  h.step(0);  // descarta a Correction do spawn
  uint32_t seq = 1;
  h.world.onInput(a, input(seq++, {0, 0, 0}), 0.0);
  for (int k = 1; k <= 11; ++k) {
    const double t = 1.125 * k;
    h.world.onInput(a, input(seq++, {100'000, 0, 0}), t);
    h.world.onInput(a, input(seq++, {0, 0, 0}), t + 0.5);  // volta ao corrigido: fecha a janela
  }
  h.step(13.0);
  REQUIRE(h.msgs<Correction>(a).size() == 11);
  // > 10 em 10 s exige período < 1 s; com 1,125 s cabem no máximo 9 na janela
  REQUIRE(h.kicks.empty());
}

TEST_CASE("world spawn sets state and queues Correction with seq 0") {
  Harness h;
  cbmp::PlayerState sp;
  sp.pos = {5000, -300, 120};
  sp.vel = {10, 0, 0};
  auto id = h.world.addPlayer("s", "d", "nome", 0.0, sp);
  REQUIRE(id.has_value());
  REQUIRE(h.world.find(*id)->state == sp);
  h.step();
  auto c = h.msgs<Correction>(*id);
  REQUIRE(c.size() == 1);
  REQUIRE(c[0].seq == 0);
  REQUIRE(c[0].pos == sp.pos);
  REQUIRE(c[0].vel == sp.vel);
  // inputs atrasados da posição antiga (origem) são ignorados sem contar incidente
  h.world.onInput(*id, input(1, {0, 0, 0}), 0.06);
  REQUIRE(h.world.find(*id)->state.pos == sp.pos);
  REQUIRE(h.world.find(*id)->correctionTimes.empty());
}

TEST_CASE("world setState queues Correction and opens a window") {
  Harness h;
  auto a = h.add("ana");
  h.world.onInput(a, input(3, {0, 0, 0}), 0.05);
  h.step();
  cbmp::PlayerState t;
  t.pos = {90'000, 0, 0};
  h.world.setState(a, t, h.now);
  h.world.onInput(a, input(4, {20, 0, 0}), h.now + 0.05);  // ainda da posição antiga
  h.step();
  auto c = h.msgs<Correction>(a);
  REQUIRE(c.size() == 1);
  REQUIRE(c[0].seq == 3);
  REQUIRE(c[0].pos.x == 90'000);
  REQUIRE(h.world.find(a)->state.pos.x == 90'000);
  REQUIRE(h.world.find(a)->correctionTimes.empty());
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

TEST_CASE("world correction window closes when a broom flies away at legal speed") {
  Harness h;
  auto a = h.add("ana");
  h.step(0);  // descarta a Correction do spawn
  h.world.onInput(a, input(1, {0, 0, 0}, cbmp::MoveState::Broom), 0.05);  // fecha a janela do spawn
  h.world.onInput(a, input(2, {100'000, 0, 0}, cbmp::MoveState::Broom), 0.1);  // teleporte → Correction
  REQUIRE(h.world.find(a)->correcting);
  // longe demais mesmo para a vassoura: continua ignorado, sem novo incidente
  h.world.onInput(a, input(3, {5'000, 0, 0}, cbmp::MoveState::Broom), 0.4);
  REQUIRE(h.world.find(a)->state.pos.x == 0);
  REQUIRE(h.world.find(a)->correcting);
  // 12 m em 0,3 s: dentro de slack + 4000 × 0,3 × 1,2 = 14,9 m → fecha a janela e valida normalmente
  h.world.onInput(a, input(4, {1'200, 0, 0}, cbmp::MoveState::Broom), 0.4);
  REQUIRE_FALSE(h.world.find(a)->correcting);
  REQUIRE(h.world.find(a)->state.pos.x == 1'200);
  REQUIRE(h.world.find(a)->correctionTimes.size() == 1);
}

TEST_CASE("world origin spawn mode places new players at the origin") {
  Harness h;
  REQUIRE(h.cfg.spawnMode == cbserver::SpawnMode::Origin);
  auto id = h.world.addPlayer("s", "d", "n", 0.0);
  h.step();
  auto c = h.msgs<Correction>(*id);
  REQUIRE(c.size() == 1);
  REQUIRE(c[0].seq == 0);
  REQUIRE(c[0].pos == cbmp::Vec3i{0, 0, 0});
}

TEST_CASE("world devTrustFirstInput spawns at the first input position") {
  ServerConfig cfg;
  cfg.spawnMode = cbserver::SpawnMode::DevTrustFirstInput;
  Harness h(cfg);
  auto a = h.add("ana");
  auto b = h.add("bia");
  h.step();
  // pendentes: nenhuma Correction, ninguém se vê, mas recebem snapshot (vazio)
  REQUIRE(h.msgs<Correction>(a).empty());
  REQUIRE(h.msgs<EntityEnter>(a).empty());
  REQUIRE(h.msgs<EntityEnter>(b).empty());
  REQUIRE(h.snaps(a).size() == 1);
  REQUIRE(h.world.find(a)->awaitingSpawn);
  // primeiro input: posição aceita sem validação de movimento
  h.world.onInput(b, input(1, {500'000, 0, 0}, cbmp::MoveState::Walk), h.now);
  REQUIRE_FALSE(h.world.find(b)->awaitingSpawn);
  REQUIRE(h.world.find(b)->state.pos.x == 500'000);
  REQUIRE(h.world.find(b)->lastInputSeq == 1);
  h.step();
  REQUIRE(h.msgs<Correction>(b).empty());
  REQUIRE(h.msgs<EntityEnter>(a).empty());  // a ainda pendente: não vê nem é visto
  REQUIRE(h.msgs<EntityEnter>(b).empty());
  h.world.onInput(a, input(1, {499'000, 0, 0}), h.now);
  h.step();
  auto ea = h.msgs<EntityEnter>(a);
  REQUIRE(ea.size() == 1);
  REQUIRE(ea[0].id == b);
  REQUIRE(ea[0].s.pos.x == 500'000);
  REQUIRE(h.msgs<EntityEnter>(b).size() == 1);
  // depois do spawn, inputs seguintes são validados normalmente
  h.world.onInput(b, input(2, {900'000, 0, 0}, cbmp::MoveState::Walk), h.now + 0.05);
  REQUIRE(h.world.find(b)->state.pos.x == 500'000);
  REQUIRE(h.world.find(b)->correctionTimes.size() == 1);
  h.world.removePlayer(a);  // remover pendente/spawnado não quebra
}
