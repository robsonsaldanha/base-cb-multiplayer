// Session: correções da revisão da Task 5 (I1 troca de mundo, I2 teleporte adiado, I3 teto do log, Minors).
#include <catch2/catch_test_macros.hpp>

#include "SessionFixture.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace cbmp::client;
using namespace cbmp::client::test;

namespace {
cbmp::EntityEnter Enter(uint16_t id, int32_t x, int32_t y, int32_t z = 500) {
    cbmp::EntityEnter e{};
    e.id = id;
    e.s = St(x, y, z);
    e.name = "Jogador" + std::to_string(id);
    return e;
}
cbmp::Correction Corr(uint32_t seq, int32_t x, int32_t y, int32_t z) {
    cbmp::Correction c{};
    c.seq = seq;
    c.pos = {x, y, z};
    return c;
}
// Online/Playing com o boneco do jogador 3 criado.
void WithPuppet(SessionFixture& fx) {
    fx.GoOnline();
    fx.transport.Push(Enter(3, 11000, 20000));
    uint32_t tick = 1;
    for (int i = 0; i < 10; ++i) {
        fx.transport.Push(Snap(tick++, {{3, St(11000, 20000, 500)}}));
        fx.RunFor(0.05);
    }
    REQUIRE(fx.game.puppets.size() == 1);
}
void ChangeWorld(SessionFixture& fx) {
    fx.game.world.worldId = 2;
    fx.game.world.worldName = "Hogwarts";
}
}  // namespace

// ---------------------------------------------------------------- I1

TEST_CASE("client: Session releases puppets before server messages on the tick the world changes") {
    SessionFixture fx;
    WithPuppet(fx);

    SECTION("EntityLeave no mesmo tick: nenhum DestroyPuppet") {
        ChangeWorld(fx);
        cbmp::EntityLeave leave{};
        leave.id = 3;
        fx.transport.Push(leave);
        fx.Step();
        CHECK(fx.S().Phase() == OnlinePhase::AwaitWorld);
        CHECK(fx.game.destroyCalls == 0);
        CHECK(fx.game.forgetAllCalls == 1);
        CHECK(fx.LogHas("EntityLeave id=3"));
    }
    SECTION("link fechado no mesmo tick: nenhum DestroyPuppet") {
        ChangeWorld(fx);
        fx.transport.state = LinkState::Closed;
        fx.transport.end = LinkEnd{5001, "timeout"};
        fx.Step();
        REQUIRE(fx.S().State() == SessionState::Disconnected);
        CHECK(fx.game.destroyCalls == 0);
        CHECK(fx.game.forgetAllCalls == 1);
        CHECK(fx.S().Puppets().alive == 0);
    }
    SECTION("Correction curta no mesmo tick: sem TeleportLocal") {
        ChangeWorld(fx);
        fx.transport.Push(Corr(1, 11500, 20000, 500));
        fx.Step();
        CHECK(fx.S().Phase() == OnlinePhase::AwaitWorld);
        CHECK(fx.game.teleports.empty());
    }
    SECTION("desconexão por mensagens inválidas no mesmo tick: nenhum DestroyPuppet") {
        ChangeWorld(fx);
        for (int i = 0; i < 25; ++i) fx.transport.PushRaw({0xFF, 0xFF, 0xFF});
        fx.Step();
        REQUIRE(fx.S().State() == SessionState::Disconnected);
        CHECK(fx.S().DisconnectReason() == "servidor_invalido");
        CHECK(fx.game.destroyCalls == 0);
        CHECK(fx.game.forgetAllCalls == 1);
    }
}

// ---------------------------------------------------------------- I2

TEST_CASE("client: Session re-measures a deferred Correction and never teleports more than 20 m") {
    SessionFixture fx;
    fx.GoOnline();  // pawn em (10000, 20000, 500)
    fx.RunFor(0.5);
    fx.transport.Push(Corr(1, 11500, 20000, 500));
    fx.Step();
    REQUIRE(fx.game.teleports.size() == 1);  // pawn agora em 11500
    fx.RunFor(0.5);
    fx.transport.Push(Corr(2, 13000, 20000, 500));  // 15 m: adiada pelo limite de 1/s
    fx.Step();
    REQUIRE(fx.game.teleports.size() == 1);

    SECTION("o jogador se afastou (vassoura): sem teleporte, próximo Input com Apparate") {
        fx.transport.sent.clear();
        for (int i = 0; i < 50; ++i) {  // 0,8 s a 4000 cm/s em -X: termina a ~4700 cm do alvo
            fx.game.local->pos.x -= 4000 * 0.016;
            fx.Step();
        }
        CHECK(fx.game.teleports.size() == 1);
        CHECK(fx.LogHas("ressincroniza com Apparate"));
        bool apparate = false;
        for (const auto& in : fx.SentOf<cbmp::Input>()) apparate = apparate || in.s.state == cbmp::MoveState::Apparate;
        CHECK(apparate);
    }
    SECTION("o jogador já chegou perto do alvo: descartada") {
        fx.game.local->pos.x = 12950;
        fx.RunFor(1);
        CHECK(fx.game.teleports.size() == 1);
    }
    SECTION("ainda dentro de 20 m: teleporta") {
        fx.game.local->pos.x = 12000;
        fx.RunFor(1);
        REQUIRE(fx.game.teleports.size() == 2);
        CHECK(fx.game.teleports[1].x == 13000);
    }
}

// ---------------------------------------------------------------- I3

TEST_CASE("client: Session caps log lines driven by a Correction flood") {
    SessionFixture fx;
    fx.GoOnline();
    fx.RunFor(0.5);
    const size_t before = fx.logs.size();
    uint32_t seq = 1;
    const double end = fx.now + 1.0;
    while (fx.now < end) {
        for (int i = 0; i < 512; ++i) fx.transport.Push(Corr(seq++, 10050, 20000, 500));
        fx.Step();
    }
    fx.RunFor(1.5);  // fecha a janela: sai o resumo
    REQUIRE(fx.S().State() == SessionState::Online);
    CHECK(fx.S().Net().corrections == seq - 1);
    CHECK(fx.LogCount("Correction seq=") <= 2 * limits::kServerLogLinesPerSec);
    CHECK(fx.LogHas("linhas suprimidas (Correction)"));
    CHECK(fx.logs.size() - before < 40);
}

TEST_CASE("client: Session caps log lines driven by an EntityEnter flood") {
    SessionFixture fx;
    fx.GoOnline();
    const size_t before = fx.logs.size();
    const double end = fx.now + 0.5;
    while (fx.now < end) {
        for (int i = 0; i < 512; ++i) fx.transport.Push(Enter(3, 11000, 20000));
        fx.Step();
    }
    fx.RunFor(1.5);
    CHECK(fx.LogCount("EntityEnter id=3") <= limits::kServerLogLinesPerSec);
    CHECK(fx.LogHas("linhas suprimidas (EntityEnter)"));
    CHECK(fx.logs.size() - before < 40);
}

TEST_CASE("client: Session caps puppet log lines from an Enter/Leave churn but keeps state lines") {
    SessionFixture fx;
    fx.GoOnline();
    const size_t before = fx.logs.size();
    uint32_t tick = 1;
    for (int i = 0; i < 60; ++i) {  // ~1 s: entra, aparece num snapshot, sai, em todo tick
        fx.transport.Push(Enter(3, 11000, 20000));
        fx.transport.Push(Snap(tick++, {{3, St(11000, 20000, 500)}}));
        fx.Step();
        cbmp::EntityLeave leave{};
        leave.id = 3;
        fx.transport.Push(leave);
        fx.Step();
    }
    fx.RunFor(1.5);
    CHECK(fx.LogCount("boneco id=3") <= 3 * limits::kServerLogLinesPerSec);
    CHECK(fx.logs.size() - before < 80);
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{5001, "timeout"};
    fx.Step();
    CHECK(fx.LogHas("desconectado: conexao_perdida"));  // transições nunca são suprimidas
}

// ---------------------------------------------------------------- Minors

TEST_CASE("client: Session error sem_resposta when the Hello cannot be sent") {
    SessionFixture fx;
    fx.gate.epoch = fx.gate.latest = 2;
    fx.fs.results.push_back(Ok(ValidConnect()));
    REQUIRE(fx.RunUntil([&] { return fx.S().State() == SessionState::Connecting; }, 10));
    fx.transport.sendOk = false;
    fx.transport.state = LinkState::Connected;
    fx.Step();
    REQUIRE(fx.S().State() == SessionState::Disconnected);
    CHECK(fx.S().DisconnectReason() == "sem_resposta");
    CHECK_FALSE(fx.S().HoldsTicket());
    CHECK(fx.secretCalls.back() == "clear");
}

TEST_CASE("client: Session error sem_resposta when Connect throws") {
    SessionFixture fx;
    fx.gate.epoch = fx.gate.latest = 2;
    fx.fs.results.push_back(Ok(ValidConnect()));
    fx.transport.throwOnConnect = true;
    REQUIRE(fx.RunUntil([&] { return fx.S().State() != SessionState::Armed; }, 10));
    CHECK(fx.S().State() == SessionState::Disconnected);
    CHECK(fx.S().DisconnectReason() == "sem_resposta");
    CHECK(fx.fs.claimCalls == 1);
    CHECK_FALSE(fx.S().HoldsTicket());
}

TEST_CASE("client: Session phase goes back to AwaitWorld on disconnect") {
    SessionFixture fx;
    fx.GoOnline();
    REQUIRE(fx.S().Phase() == OnlinePhase::Playing);
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{5001, "timeout"};
    fx.Step();
    REQUIRE(fx.S().State() == SessionState::Disconnected);
    CHECK(fx.S().Phase() == OnlinePhase::AwaitWorld);
}

TEST_CASE("client: Session treats a non-finite local position as no pawn for Correction") {
    SessionFixture fx;
    fx.GoOnline();
    fx.game.local->pos.x = std::numeric_limits<double>::quiet_NaN();
    fx.transport.Push(Corr(1, 11500, 20000, 500));
    fx.Step();
    CHECK(fx.game.teleports.empty());
    CHECK(fx.LogHas("Correction seq=1 distancia=sem pawn"));
}

TEST_CASE("client: Session Shutdown before and during the handshake") {
    SessionFixture fx;
    SECTION("Armed: não fecha nada") {
        fx.RunFor(1);
        fx.S().Shutdown();
        CHECK(fx.LogHas("encerrando"));
        CHECK(fx.transport.closeCalls == 0);
    }
    SECTION("Connecting: fecha, zera o ticket e limpa o segredo") {
        fx.gate.epoch = fx.gate.latest = 2;
        fx.fs.results.push_back(Ok(ValidConnect()));
        REQUIRE(fx.RunUntil([&] { return fx.S().State() == SessionState::Connecting; }, 10));
        REQUIRE(fx.S().HoldsTicket());
        fx.S().Shutdown();
        CHECK(fx.transport.closeCalls == 1);
        CHECK_FALSE(fx.S().HoldsTicket());
        CHECK(fx.secretCalls.back() == "clear");
    }
    SECTION("Handshake: fecha") {
        fx.gate.epoch = fx.gate.latest = 2;
        fx.GoHandshake();
        fx.S().Shutdown();
        CHECK(fx.transport.closeCalls == 1);
        CHECK_FALSE(fx.S().HoldsTicket());
        fx.RunFor(1);
        CHECK(fx.transport.closeCalls == 1);
    }
}

TEST_CASE("client: Session sharing violation gives up exactly at 5 s") {
    SessionFixture fx;
    for (int i = 0; i < 1000; ++i) fx.fs.results.push_back(Busy());
    fx.gate.epoch = fx.gate.latest = 2;
    REQUIRE(fx.RunUntil([&] { return fx.fs.claimCalls >= 1; }, 10));
    fx.RunFor(4.9);
    CHECK(fx.S().State() == SessionState::Armed);
    fx.RunFor(0.2);
    CHECK(fx.S().State() == SessionState::Disconnected);
    CHECK(fx.S().DisconnectReason() == "sem_arquivo");
}

TEST_CASE("client: Session Correction in AwaitWorld makes the first Input an Apparate at the local position") {
    SessionFixture fx;
    fx.GoAwaitWorld();
    fx.transport.Push(Corr(0, 0, 0, 0));  // spawn do servidor na origem, ainda no menu
    fx.RunFor(0.5);
    fx.transport.sent.clear();
    fx.EnterWorld();
    REQUIRE(fx.RunUntil([&] { return fx.CountSent<cbmp::Input>() > 0; }, 5));
    const auto inputs = fx.SentOf<cbmp::Input>();
    CHECK(inputs[0].s.state == cbmp::MoveState::Apparate);
    CHECK(inputs[0].s.pos == cbmp::Vec3i{10000, 20000, 500});
}
