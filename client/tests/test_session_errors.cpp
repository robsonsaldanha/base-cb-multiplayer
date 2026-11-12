// Session: uma linha por situação da tabela "Erros e falhas" da spec do sub5a.
#include <catch2/catch_test_macros.hpp>

#include "SessionFixture.hpp"

#include <string>
#include <string_view>

using namespace cbmp::client;
using namespace cbmp::client::test;

namespace {
// Conferências comuns a toda desconexão, inclusive o status mostrado uma única vez dentro do mundo.
void CheckDisconnected(SessionFixture& fx, std::string_view reason, std::string_view screen) {
    REQUIRE(fx.S().State() == SessionState::Disconnected);
    CHECK(fx.S().DisconnectReason() == reason);
    CHECK(fx.LogHas("desconectado: " + std::string(reason)));
    CHECK_FALSE(fx.S().HoldsTicket());
    CHECK(fx.transport.closeCalls >= 1);
    CHECK(fx.S().Puppets().alive == 0);
    CHECK(fx.game.puppets.empty());
    CHECK(DisconnectStatusText(reason) == screen);
    if (!fx.secretCalls.empty()) CHECK(fx.secretCalls.back() == "clear");

    if (!fx.game.local) fx.EnterWorld();
    fx.RunFor(3);
    CHECK(fx.StatusCount(screen) == 1);
    fx.RunFor(3);
    CHECK(fx.StatusCount(screen) == 1);
    for (const auto& l : fx.logs) CHECK(l.find(kTicketMarker) == std::string::npos);
}

constexpr std::string_view kExpired = "CB Multiplayer: desconectado — entrada expirou; feche o jogo e clique Jogar";
constexpr std::string_view kNoAnswer = "CB Multiplayer: desconectado — servidor não respondeu";
constexpr std::string_view kIncompatible = "CB Multiplayer: desconectado — servidor incompatível";

// Sucessor no menu: resgata aos 5 s.
SessionFixture& Successor(SessionFixture& fx) {
    fx.gate.epoch = fx.gate.latest = 2;
    return fx;
}
}  // namespace

TEST_CASE("client: Session error sem_arquivo when connect.json is missing") {
    SessionFixture fx;
    Successor(fx).RunFor(6);
    CHECK(fx.fs.claimCalls == 1);
    CHECK(fx.transport.connectCalls == 0);
    CheckDisconnected(fx, "sem_arquivo", kExpired);
}

TEST_CASE("client: Session error sem_arquivo after 5 s of sharing violation") {
    SessionFixture fx;
    for (int i = 0; i < 1000; ++i) fx.fs.results.push_back(Busy());
    Successor(fx).RunFor(5.5);
    CHECK(fx.S().State() == SessionState::Armed);
    CHECK(fx.fs.claimCalls > 20);
    fx.RunFor(5.5);
    CHECK(fx.transport.connectCalls == 0);
    CheckDisconnected(fx, "sem_arquivo", kExpired);
}

TEST_CASE("client: Session connects after 2 s of sharing violation") {
    SessionFixture fx;
    for (int i = 0; i < 125; ++i) fx.fs.results.push_back(Busy());  // ~2 s de ticks de 16 ms
    fx.fs.results.push_back(Ok(ValidConnect()));
    Successor(fx).RunFor(8);
    CHECK(fx.fs.claimCalls == 126);
    CHECK(fx.S().State() == SessionState::Connecting);
    CHECK(fx.transport.connectCalls == 1);
}

TEST_CASE("client: Session error arquivo_invalido for another launchId") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect("ffffffffffffffffffffffffffffffff")));
    Successor(fx).RunFor(6);
    CHECK(fx.transport.connectCalls == 0);
    for (const auto& c : fx.secretCalls) CHECK(c == "clear");  // o ticket nunca virou segredo ativo
    CheckDisconnected(fx, "arquivo_invalido", "CB Multiplayer: desconectado — arquivo de entrada inválido");
}

TEST_CASE("client: Session error arquivo_invalido for a rejected file") {
    SessionFixture fx;
    fx.fs.results.push_back(ClaimResult{ClaimRead::Rejected, {}});
    Successor(fx).RunFor(6);
    CHECK(fx.transport.connectCalls == 0);
    CheckDisconnected(fx, "arquivo_invalido", "CB Multiplayer: desconectado — arquivo de entrada inválido");
}

TEST_CASE("client: Session error protocolo_diferente never touches the network") {
    SessionFixture fx;
    ConnectOpts o;
    o.protocol = 2;
    fx.fs.results.push_back(Ok(ConnectBytes(o)));
    Successor(fx).RunFor(6);
    CHECK(fx.transport.connectCalls == 0);
    CheckDisconnected(fx, "protocolo_diferente",
                      "CB Multiplayer: desconectado — versão do mod diferente da do servidor");
}

TEST_CASE("client: Session error ticket_expirado when the ticket expires in 5 s") {
    SessionFixture fx;
    ConnectOpts o;
    o.exp = kUnixBase + 5 + 5;  // resgate aos 5 s: faltam 5 s
    fx.fs.results.push_back(Ok(ConnectBytes(o)));
    Successor(fx).RunFor(6);
    CHECK(fx.transport.connectCalls == 0);
    CheckDisconnected(fx, "ticket_expirado", kExpired);
}

TEST_CASE("client: Session error sem_resposta when Connect fails") {
    SessionFixture fx;
    fx.transport.connectOk = false;
    fx.fs.results.push_back(Ok(ValidConnect()));
    Successor(fx).RunFor(6);
    CHECK(fx.transport.connectCalls == 1);
    CheckDisconnected(fx, "sem_resposta", kNoAnswer);
}

TEST_CASE("client: Session error sem_resposta after 10 s in Connecting") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect()));
    Successor(fx).RunUntil([&] { return fx.S().State() == SessionState::Connecting; });
    fx.RunFor(9.9);
    CHECK(fx.S().State() == SessionState::Connecting);
    CHECK(fx.S().HoldsTicket());
    fx.RunFor(0.2);
    CHECK(fx.transport.SentMessages().empty());
    CheckDisconnected(fx, "sem_resposta", kNoAnswer);
}

TEST_CASE("client: Session error sem_resposta when the link closes while connecting") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect()));
    Successor(fx).RunUntil([&] { return fx.S().State() == SessionState::Connecting; });
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{4002, "sem rota"};
    fx.Step();
    CheckDisconnected(fx, "sem_resposta", kNoAnswer);
}

TEST_CASE("client: Session error recusado with the Reject text") {
    SessionFixture fx;
    Successor(fx).GoHandshake();
    cbmp::Reject r{};
    r.reason = "banido";
    fx.transport.Push(r);
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{1001, "debug do servidor"};
    fx.Step();
    CheckDisconnected(fx, "recusado: banido", "CB Multiplayer: desconectado — recusado: banido");
}

TEST_CASE("client: Session error recusado without text uses the debug text or sem motivo") {
    SECTION("debug") {
        SessionFixture fx;
        Successor(fx).GoHandshake();
        fx.transport.state = LinkState::Closed;
        fx.transport.end = LinkEnd{1003, "protocolo"};
        fx.Step();
        CheckDisconnected(fx, "recusado: protocolo", "CB Multiplayer: desconectado — recusado: protocolo");
    }
    SECTION("sem motivo") {
        SessionFixture fx;
        Successor(fx).GoHandshake();
        fx.transport.state = LinkState::Closed;
        fx.transport.end = LinkEnd{1002, ""};
        fx.Step();
        CheckDisconnected(fx, "recusado: sem motivo", "CB Multiplayer: desconectado — recusado: sem motivo");
    }
}

TEST_CASE("client: Session error sem_welcome after 10 s without Welcome") {
    SessionFixture fx;
    Successor(fx).GoHandshake();
    fx.RunFor(9.9);
    CHECK(fx.S().State() == SessionState::Handshake);
    fx.RunFor(0.2);
    CheckDisconnected(fx, "sem_welcome", kNoAnswer);
}

TEST_CASE("client: Session error servidor_incompativel for tickRateHz out of 5..60") {
    for (const uint8_t hz : {uint8_t{4}, uint8_t{61}, uint8_t{0}, uint8_t{255}}) {
        SessionFixture fx;
        Successor(fx).GoHandshake();
        fx.PushWelcome(7, hz);
        fx.Step();
        CheckDisconnected(fx, "servidor_incompativel", kIncompatible);
    }
    for (const uint8_t hz : {uint8_t{5}, uint8_t{60}}) {
        SessionFixture fx;
        Successor(fx).GoHandshake();
        fx.PushWelcome(7, hz);
        fx.Step();
        CHECK(fx.S().State() == SessionState::Online);
    }
}

TEST_CASE("client: Session error expulso with the Kick text") {
    SessionFixture fx;
    fx.GoOnline();
    cbmp::Kick k{};
    k.reason = "conectado em outro lugar";
    fx.transport.Push(k);
    fx.Step();
    CHECK(fx.S().State() == SessionState::Online);  // espera o transporte fechar
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{1004, "kick"};
    fx.Step();
    CheckDisconnected(fx, "expulso: conectado em outro lugar",
                      "CB Multiplayer: desconectado — conectado em outro lugar");
}

TEST_CASE("client: Session error conexao_perdida when the link ends with another code online") {
    SessionFixture fx;
    fx.GoOnline();
    cbmp::EntityEnter e{};
    e.id = 3;
    e.s = St(11000, 20000, 500);
    fx.transport.Push(e);
    fx.RunFor(0.5);
    REQUIRE(fx.game.puppets.size() == 1);
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{5001, "timeout"};
    fx.Step();
    CheckDisconnected(fx, "conexao_perdida", "CB Multiplayer: desconectado — conexão perdida");
}

TEST_CASE("client: Session error servidor_invalido after 21 invalid messages") {
    SessionFixture fx;
    fx.GoOnline();
    for (int i = 0; i < 21; ++i) fx.transport.PushRaw({0xFF, 0x01, 0x02});
    fx.Step();
    CHECK(fx.LogHas("recebidas="));
    CheckDisconnected(fx, "servidor_invalido", kIncompatible);
}

TEST_CASE("client: Session error servidor_invalido when 5000 messages are pending") {
    SessionFixture fx;
    fx.GoOnline();
    for (int i = 0; i < 5000; ++i) fx.transport.Push(cbmp::Event{"x", {}});
    fx.Step();
    CheckDisconnected(fx, "servidor_invalido", kIncompatible);
}

TEST_CASE("client: Session contains game errors from SampleWorld") {
    SessionFixture fx;
    fx.GoOnline();
    fx.game.throwOnSample = true;
    for (int i = 0; i < 100; ++i) fx.Step();
    CHECK(fx.S().State() == SessionState::Online);
    CHECK(fx.S().Phase() == OnlinePhase::AwaitWorld);
    fx.game.throwOnSample = false;
    fx.RunFor(0.5);
    CHECK(fx.S().Phase() == OnlinePhase::Playing);
    fx.RunFor(10);
    CHECK(fx.LogHas("erros_jogo=200"));
    CHECK(fx.S().State() == SessionState::Online);
}

TEST_CASE("client: Session error erro_interno after 100 exceptions in 60 s") {
    SessionFixture fx;
    fx.fs.results.push_back(Ok(ValidConnect()));
    Successor(fx).RunUntil([&] { return fx.S().State() == SessionState::Connecting; });
    fx.transport.throwOnState = true;
    for (int i = 0; i < 99; ++i) fx.Step();
    CHECK(fx.S().State() == SessionState::Connecting);
    CHECK(fx.LogCount("exceção no tick") == 1);
    fx.Step();
    fx.transport.throwOnState = false;
    CheckDisconnected(fx, "erro_interno", "CB Multiplayer: desconectado — erro interno");
}

TEST_CASE("client: Session summarizes sparse exceptions every 10 s and stays alive") {
    SessionFixture fx;
    fx.GoOnline();
    for (int i = 0; i < 50; ++i) {  // 50 exceções em 25 s
        fx.transport.throwOnState = true;
        fx.Step();
        fx.transport.throwOnState = false;
        fx.RunFor(0.5);
    }
    CHECK(fx.S().State() == SessionState::Online);
    CHECK(fx.LogCount("exceção no tick") == 1);
    CHECK(fx.LogCount("exceções no tick") >= 2);
}

TEST_CASE("client: Session shows a disconnection that happened in the menu once on entering the world") {
    SessionFixture fx;
    Successor(fx).RunFor(6);
    REQUIRE(fx.S().State() == SessionState::Disconnected);
    fx.RunFor(30);
    CHECK(fx.game.statuses.empty());
    fx.EnterWorld();
    fx.RunFor(1.9);
    CHECK(fx.game.statuses.empty());
    fx.RunFor(0.3);
    REQUIRE(fx.game.statuses.size() == 1);
    CHECK(fx.game.statuses[0] == kExpired);
    fx.GoMenu();
    fx.RunFor(1);
    fx.EnterWorld();
    fx.RunFor(5);
    CHECK(fx.game.statuses.size() == 1);
}

TEST_CASE("client: Session never writes the ticket to the log") {
    SessionFixture fx;
    fx.GoOnline();
    fx.transport.state = LinkState::Closed;
    fx.transport.end = LinkEnd{5001, "timeout"};
    fx.Step();
    REQUIRE(fx.S().State() == SessionState::Disconnected);
    REQUIRE(fx.secretCalls.size() >= 2);
    CHECK(fx.secretCalls.front().find(kTicketMarker) != std::string::npos);
    for (const auto& l : fx.logs) CHECK(l.find(kTicketMarker) == std::string::npos);

    for (const auto& bytes : {ValidConnect("ffffffffffffffffffffffffffffffff"), [] {
                                  ConnectOpts o;
                                  o.protocol = 2;
                                  return ConnectBytes(o);
                              }()}) {
        SessionFixture f2;
        f2.fs.results.push_back(Ok(bytes));
        Successor(f2).RunFor(6);
        REQUIRE(f2.S().State() == SessionState::Disconnected);
        for (const auto& l : f2.logs) CHECK(l.find(kTicketMarker) == std::string::npos);
    }
}
