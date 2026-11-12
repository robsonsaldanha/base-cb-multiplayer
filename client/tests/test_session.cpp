// Session: caminho feliz (resgate, handshake, Input, snapshots, bonecos, correções, status, contadores).
#include <catch2/catch_test_macros.hpp>

#include "SessionFixture.hpp"

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
}  // namespace

// ---------------------------------------------------------------- resgate (dois processos)

TEST_CASE("client: Session epoch 1 in menu waits 45 s, then claims and sends Hello on connect") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect()));
    fx.RunFor(44.9);
    CHECK(fx.fs.claimCalls == 0);
    CHECK(fx.gate.ownCalls == 0);
    CHECK(fx.S().State() == SessionState::Armed);

    fx.RunFor(0.2);
    CHECK(fx.fs.claimCalls == 1);
    CHECK(fx.S().State() == SessionState::Connecting);
    CHECK(fx.LogHas("dono epoca=1"));
    CHECK(fx.transport.connectCalls == 1);
    CHECK(fx.transport.lastHost == "127.0.0.1");
    CHECK(fx.transport.lastPort == 27015);
    CHECK(fx.S().HoldsTicket());
    REQUIRE(fx.secretCalls.size() == 1);
    CHECK(fx.secretCalls[0].starts_with("set:"));
    CHECK(fx.secretCalls[0].find(kTicketMarker) != std::string::npos);

    fx.transport.state = LinkState::Connected;
    fx.Step();
    CHECK(fx.S().State() == SessionState::Handshake);
    REQUIRE(fx.transport.sent.size() == 1);
    CHECK(fx.transport.sent[0].reliable);
    const auto hellos = fx.SentOf<cbmp::Hello>();
    REQUIRE(hellos.size() == 1);
    CHECK(hellos[0].protocol == cbmp::kProtocolVersion);
    CHECK(hellos[0].buildHash[0] == 0xAB);
    CHECK(hellos[0].ticket.find(kTicketMarker) != std::string::npos);
    CHECK_FALSE(fx.S().HoldsTicket());
    REQUIRE(fx.secretCalls.size() == 2);
    CHECK(fx.secretCalls[1] == "clear");
    CHECK(fx.LogHas("Hello enviado"));
}

TEST_CASE("client: Session epoch 2 claims after 5 s") {
    SessionFixture fx;
    fx.gate.epoch = fx.gate.latest = 2;
    fx.fs.results.push_back(Ok(ValidConnect()));
    fx.RunFor(4.9);
    CHECK(fx.fs.claimCalls == 0);
    fx.RunFor(0.2);
    CHECK(fx.fs.claimCalls == 1);
    CHECK(fx.S().State() == SessionState::Connecting);
    CHECK(fx.LogHas("dono epoca=2"));
}

TEST_CASE("client: Session claims as soon as the world is InWorld") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect()));
    fx.EnterWorld();
    fx.RunFor(1.9);
    CHECK(fx.fs.claimCalls == 0);  // InWorld exige 2 s estáveis
    fx.RunFor(0.2);
    CHECK(fx.fs.claimCalls == 1);
    CHECK(fx.S().State() == SessionState::Connecting);
    CHECK(fx.LogHas("mundo: Overland → Overland InWorld"));
}

TEST_CASE("client: Session superseded by a newer process never claims") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect()));
    fx.gate.latest = 2;
    fx.Step();
    CHECK(fx.S().State() == SessionState::Superseded);
    CHECK(fx.LogHas("substituido epoca=1→2"));
    fx.EnterWorld();
    fx.RunFor(60);
    CHECK(fx.S().State() == SessionState::Superseded);
    CHECK(fx.fs.claimCalls == 0);
    CHECK(fx.gate.ownCalls == 0);
    CHECK(fx.transport.connectCalls == 0);
    CHECK(fx.game.statuses.empty());
}

TEST_CASE("client: Session superseded when another process owns the mutex") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect()));
    fx.gate.epoch = fx.gate.latest = 2;
    fx.gate.ownOk = false;
    fx.RunFor(6);
    CHECK(fx.S().State() == SessionState::Superseded);
    CHECK(fx.LogHas("substituido: outro processo é o dono"));
    CHECK(fx.gate.ownCalls == 1);
    CHECK(fx.fs.claimCalls == 0);
    fx.RunFor(10);
    CHECK(fx.gate.ownCalls == 1);
}

// ---------------------------------------------------------------- AwaitWorld e Input

TEST_CASE("client: Session in AwaitWorld sends no Input but acks snapshots and records EntityEnter") {
    SessionFixture fx;
    fx.GoAwaitWorld();
    CHECK(fx.S().PlayerId() == 7);
    CHECK(fx.LogHas("Welcome playerId=7"));
    fx.transport.sent.clear();

    fx.transport.Push(Enter(3, 12000, 20000));
    uint32_t tick = 1;
    for (int i = 0; i < 100; ++i) {  // 5 s
        fx.transport.Push(Snap(tick++, {{3, St(12000, 20000, 500)}}));
        fx.RunFor(0.05);
    }
    CHECK(fx.CountSent<cbmp::Input>() == 0);
    CHECK(fx.CountSent<cbmp::SnapshotAck>() >= 99);
    CHECK(fx.LogHas("EntityEnter id=3"));
    CHECK(fx.game.puppets.empty());
    CHECK(fx.game.spawnCalls == 0);

    fx.transport.sent.clear();
    fx.EnterWorld();
    REQUIRE(fx.RunUntil([&] { return fx.CountSent<cbmp::Input>() > 0; }, 5));
    CHECK(fx.S().Phase() == OnlinePhase::Playing);
    const auto inputs = fx.SentOf<cbmp::Input>();
    REQUIRE(inputs.size() == 1);
    CHECK(inputs[0].seq == 1);
    CHECK(inputs[0].s.state == cbmp::MoveState::Apparate);
    CHECK(inputs[0].s.pos == cbmp::Vec3i{10000, 20000, 500});
    CHECK(fx.LogHas("Input Apparate"));
}

TEST_CASE("client: Session in Playing sends 20 Input per second with increasing seq and no burst") {
    SessionFixture fx;
    fx.GoOnline();
    fx.RunFor(0.5);
    fx.transport.sent.clear();
    fx.RunFor(1.0);
    const auto inputs = fx.SentOf<cbmp::Input>();
    CHECK(inputs.size() >= 19);
    CHECK(inputs.size() <= 21);
    for (size_t i = 1; i < inputs.size(); ++i) CHECK(inputs[i].seq == inputs[i - 1].seq + 1);
    for (const auto& in : inputs) CHECK(in.s.state != cbmp::MoveState::Apparate);
    for (const auto& s : fx.transport.sent)
        if (std::holds_alternative<cbmp::Input>(cbmp::decode(s.bytes))) CHECK_FALSE(s.reliable);

    // Tick atrasado em 1 s: um único Input, sem rajada.
    fx.transport.sent.clear();
    fx.Step(1.0);
    CHECK(fx.CountSent<cbmp::Input>() == 1);
    fx.Step(0.016);
    fx.Step(0.016);
    CHECK(fx.CountSent<cbmp::Input>() == 1);
}

TEST_CASE("client: Session uses the Welcome tick rate for Input") {
    SessionFixture fx;
    fx.GoOnline(30);
    fx.RunFor(0.5);
    fx.transport.sent.clear();
    fx.RunFor(1.0);
    const auto n = fx.CountSent<cbmp::Input>();
    CHECK(n >= 28);
    CHECK(n <= 32);
}

// ---------------------------------------------------------------- bonecos

TEST_CASE("client: Session creates, moves and destroys the puppet of another player") {
    SessionFixture fx;
    fx.GoOnline();
    fx.transport.Push(Enter(3, 11000, 20000));
    uint32_t tick = 1;
    for (int i = 0; i < 20; ++i) {
        fx.transport.Push(Snap(tick++, {{3, St(11000 + i * 10, 20000, 500)}}));
        fx.RunFor(0.05);
    }
    CHECK(fx.game.puppets.size() == 1);
    CHECK(fx.S().Puppets().alive == 1);
    CHECK(fx.LogHas("boneco id=3 criado"));
    const int moves = fx.game.moveCalls;
    fx.RunFor(0.16);  // 10 ticks
    CHECK(fx.game.moveCalls >= moves + 9);
    REQUIRE(fx.S().Mirror().Find(tick - 1) != nullptr);
    CHECK(fx.S().Mirror().Find(tick - 1)->ents.size() == 1);

    cbmp::EntityLeave leave{};
    leave.id = 3;
    fx.transport.Push(leave);
    fx.Step();
    CHECK(fx.game.puppets.empty());
    CHECK(fx.S().Puppets().alive == 0);
    CHECK(fx.LogHas("EntityLeave id=3"));
    CHECK(fx.LogHas("boneco id=3 destruído"));
    const auto* rec = fx.S().Mirror().Find(tick - 1);
    REQUIRE(rec != nullptr);
    CHECK(rec->ents.empty());
}

TEST_CASE("client: Session never creates a puppet for its own playerId") {
    SessionFixture fx;
    fx.GoOnline();
    fx.transport.Push(Enter(7, 10000, 20000));
    uint32_t tick = 1;
    for (int i = 0; i < 20; ++i) {
        fx.transport.Push(Snap(tick++, {{7, St(10000, 20000, 500)}}));
        fx.RunFor(0.05);
    }
    CHECK(fx.game.spawnCalls == 0);
    CHECK(fx.game.puppets.empty());
}

TEST_CASE("client: Session releases puppets when leaving InWorld and recreates them on return") {
    SessionFixture fx;
    fx.GoOnline();
    fx.transport.Push(Enter(3, 11000, 20000));
    uint32_t tick = 1;
    for (int i = 0; i < 10; ++i) {
        fx.transport.Push(Snap(tick++, {{3, St(11000, 20000, 500)}}));
        fx.RunFor(0.05);
    }
    REQUIRE(fx.game.puppets.size() == 1);

    SECTION("pawn trocado: DestroyPuppet, sem Input até voltar") {
        fx.game.world.pawnId = 11;
        fx.Step();
        CHECK(fx.S().Phase() == OnlinePhase::AwaitWorld);
        CHECK(fx.game.destroyCalls == 1);
        CHECK(fx.game.forgetAllCalls == 0);
        CHECK(fx.game.puppets.empty());
        fx.transport.sent.clear();
        fx.RunFor(1.5);
        CHECK(fx.CountSent<cbmp::Input>() == 0);

        const int spawns = fx.game.spawnCalls;
        REQUIRE(fx.RunUntil([&] { return fx.CountSent<cbmp::Input>() > 0; }, 3));
        CHECK(fx.S().Phase() == OnlinePhase::Playing);
        const auto inputs = fx.SentOf<cbmp::Input>();
        CHECK(inputs[0].s.state == cbmp::MoveState::Apparate);
        CHECK(fx.game.spawnCalls == spawns + 1);
        CHECK(fx.game.puppets.size() == 1);
    }
    SECTION("mundo trocado: ForgetAllPuppets") {
        fx.game.world.worldId = 2;
        fx.game.world.worldName = "Hogwarts";
        fx.Step();
        CHECK(fx.S().Phase() == OnlinePhase::AwaitWorld);
        CHECK(fx.game.forgetAllCalls == 1);
        CHECK(fx.game.destroyCalls == 0);
        CHECK(fx.game.puppets.empty());
        CHECK(fx.LogHas("mundo: Overland InWorld → Hogwarts"));
    }
}

// ---------------------------------------------------------------- correções

TEST_CASE("client: Session handles Correction by distance") {
    SessionFixture fx;
    fx.GoOnline();  // pawn em (10000, 20000, 500)
    fx.RunFor(0.5);

    SECTION("a 50 cm: nada") {
        fx.transport.Push(Corr(1, 10050, 20000, 500));
        fx.RunFor(1);
        CHECK(fx.game.teleports.empty());
        CHECK(fx.LogHas("Correction seq=1"));
    }
    SECTION("a 1500 cm: um TeleportLocal") {
        fx.transport.Push(Corr(1, 11500, 20000, 500));
        fx.Step();
        REQUIRE(fx.game.teleports.size() == 1);
        CHECK(fx.game.teleports[0].x == 11500);
        fx.RunFor(2);
        CHECK(fx.game.teleports.size() == 1);
    }
    SECTION("duas em 0,5 s: a segunda espera 1 s") {
        fx.transport.Push(Corr(1, 11500, 20000, 500));
        fx.Step();
        REQUIRE(fx.game.teleports.size() == 1);
        const double first = fx.now;
        fx.RunFor(0.5);
        fx.transport.Push(Corr(2, 13000, 20000, 500));
        fx.Step();
        CHECK(fx.game.teleports.size() == 1);
        fx.RunUntil([&] { return fx.now >= first + 0.97; }, 2);
        CHECK(fx.game.teleports.size() == 1);
        fx.RunFor(0.06);
        REQUIRE(fx.game.teleports.size() == 2);
        CHECK(fx.game.teleports[1].x == 13000);
    }
    SECTION("a 5000 cm: sem teleporte, próximo Input com Apparate") {
        fx.transport.sent.clear();
        fx.transport.Push(Corr(1, 15000, 20000, 500));
        fx.RunFor(0.2);
        CHECK(fx.game.teleports.empty());
        const auto inputs = fx.SentOf<cbmp::Input>();
        REQUIRE_FALSE(inputs.empty());
        CHECK(inputs[0].s.state == cbmp::MoveState::Apparate);
        CHECK_FALSE(fx.LogHas("posicao_recusada"));
    }
    SECTION("segunda longe com o mesmo seq: posicao_recusada uma vez") {
        const std::string text = "CB Multiplayer: o servidor não aceitou sua posição";
        fx.transport.Push(Corr(4, 15000, 20000, 500));
        fx.RunFor(0.5);
        CHECK(fx.StatusCount(text) == 0);
        fx.transport.Push(Corr(4, 15000, 20000, 500));
        fx.RunFor(0.5);
        CHECK(fx.LogCount("posicao_recusada") == 1);
        CHECK(fx.StatusCount(text) == 1);
        fx.transport.Push(Corr(4, 15000, 20000, 500));
        fx.RunFor(0.5);
        fx.transport.Push(Corr(5, 15000, 20000, 500));
        fx.transport.Push(Corr(5, 15000, 20000, 500));
        fx.RunFor(0.5);
        CHECK(fx.LogCount("posicao_recusada") == 1);
        CHECK(fx.StatusCount(text) == 1);
        CHECK(fx.S().State() == SessionState::Online);
        CHECK(fx.game.teleports.empty());
    }
}

TEST_CASE("client: Session never teleports in AwaitWorld") {
    SessionFixture fx;
    fx.GoAwaitWorld();
    fx.game.local = LocalState{Vec3{10000, 20000, 500}, 0.0, 0.0, Vec3{}, MoveMode::Ground};  // pawn, mas no menu
    fx.transport.Push(Corr(0, 11500, 20000, 500));
    fx.RunFor(2);
    CHECK(fx.game.teleports.empty());
    CHECK(fx.LogHas("Correction seq=0"));
}

// ---------------------------------------------------------------- status, contadores, encerramento

TEST_CASE("client: Session shows the connected status once on entering Playing") {
    SessionFixture fx;
    fx.GoOnline();
    fx.RunFor(3);
    REQUIRE(fx.game.statuses.size() == 1);
    CHECK(fx.game.statuses[0] == "CB Multiplayer: conectado (0 por perto)");
}

TEST_CASE("client: Session shows no status when statusOnScreen is false") {
    SessionFixture fx(false);
    fx.GoOnline();
    fx.RunFor(3);
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{5001, "timeout"};
    fx.RunFor(1);
    CHECK(fx.S().State() == SessionState::Disconnected);
    CHECK(fx.game.statuses.empty());
}

TEST_CASE("client: Session turns the status off after ShowStatus fails") {
    SessionFixture fx;
    fx.game.statusOk = false;
    fx.GoOnline();
    fx.RunFor(1);
    CHECK(fx.game.statuses.size() == 1);
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{5001, "timeout"};
    fx.RunFor(1);
    CHECK(fx.S().State() == SessionState::Disconnected);
    CHECK(fx.game.statuses.size() == 1);
    CHECK(fx.LogCount("status na tela desligado") == 1);
}

TEST_CASE("client: Session logs the counters every 10 s") {
    SessionFixture fx;
    fx.GoOnline();
    fx.RunFor(25);
    CHECK(fx.LogCount("contadores snapshots=") == 2);
    CHECK(fx.LogHas("acks="));
    CHECK(fx.LogHas("correcoes="));
    CHECK(fx.LogHas("bonecos="));
    CHECK(fx.LogHas("erros_jogo=0"));
}

TEST_CASE("client: Session Shutdown logs and closes without waiting") {
    SessionFixture fx;
    fx.GoOnline();
    fx.S().Shutdown();
    CHECK(fx.LogHas("encerrando"));
    CHECK(fx.transport.closeCalls == 1);
    CHECK_FALSE(fx.S().HoldsTicket());
    fx.RunFor(1);
    CHECK(fx.transport.closeCalls == 1);
}
