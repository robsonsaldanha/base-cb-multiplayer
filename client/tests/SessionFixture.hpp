#pragma once
// Montagem dos testes da Session: jogo, transporte, portão e arquivo falsos, relógio manual e captura do log.
#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/Session.hpp>
#include <cbmp/messages.hpp>

#include "FakeGame.hpp"
#include "FakeGate.hpp"
#include "FakeTransport.hpp"

#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cbmp::client::test {
inline constexpr std::string_view kLaunchId = "0123456789abcdef0123456789abcdef";
inline constexpr std::string_view kTicketMarker = "TICKETSECRETO";
inline constexpr int64_t kUnixBase = 1'794'438'789;

// Transporte falso que também pode lançar em State() (exceção genérica no tick).
struct SessionTransport : FakeTransport {
    bool throwOnState = false;
    LinkState State() override {
        if (throwOnState) throw std::runtime_error("State falhou (teste)");
        return FakeTransport::State();
    }
};

// base64url sem padding.
inline std::string B64Url(std::string_view in) {
    static constexpr char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const uint32_t n = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
        out += kAlpha[(n >> 18) & 63];
        out += kAlpha[(n >> 12) & 63];
        out += kAlpha[(n >> 6) & 63];
        out += kAlpha[n & 63];
    }
    if (in.size() - i == 1) {
        const uint32_t n = uint32_t(uint8_t(in[i])) << 16;
        out += kAlpha[(n >> 18) & 63];
        out += kAlpha[(n >> 12) & 63];
    } else if (in.size() - i == 2) {
        const uint32_t n = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8);
        out += kAlpha[(n >> 18) & 63];
        out += kAlpha[(n >> 12) & 63];
        out += kAlpha[(n >> 6) & 63];
    }
    return out;
}

// Ticket no formato do Hub; a "assinatura" carrega o marcador TICKETSECRETO.
inline std::string Ticket(int64_t exp) {
    const auto claims = std::format(
        R"({{"v":1,"kid":"k1","sub":"u1","did":"123","name":"Fulano","aud":"dev-local","iat":{},"exp":{},"jti":"j1"}})",
        exp - 300, exp);
    return B64Url(claims) + "." + std::string(kTicketMarker) + B64Url(std::string(48, '\x5a'));
}

struct ConnectOpts {
    std::string launchId = std::string(kLaunchId);
    int64_t exp = kUnixBase + 300;
    uint32_t protocol = 1;
    std::string host = "127.0.0.1";
    uint16_t port = 27015;
};

// connect.json como o launcher grava (pretty, camelCase, com launchId).
inline std::vector<uint8_t> ConnectBytes(const ConnectOpts& o) {
    const auto s = std::format(R"({{
  "host": "{}",
  "port": {},
  "ticket": "{}",
  "serverId": "dev-local",
  "contentDir": "C:\\cb\\content",
  "protocol": {},
  "contentReadOnly": true,
  "launchId": "{}"
}})",
                               o.host, o.port, Ticket(o.exp), o.protocol, o.launchId);
    return {s.begin(), s.end()};
}

inline std::vector<uint8_t> ValidConnect(std::string_view launchId = kLaunchId) {
    ConnectOpts o;
    o.launchId = std::string(launchId);
    return ConnectBytes(o);
}

inline ClaimResult Ok(std::vector<uint8_t> bytes) { return ClaimResult{ClaimRead::Ok, std::move(bytes)}; }
inline ClaimResult Busy() { return ClaimResult{ClaimRead::Busy, {}}; }

inline cbmp::PlayerState St(int32_t x, int32_t y, int32_t z = 0) {
    cbmp::PlayerState s{};
    s.pos = {x, y, z};
    return s;
}

// Snapshot completo de um tick (posições absolutas, sem baseline, uma parte).
inline cbmp::Snapshot Snap(uint32_t tick, const std::vector<std::pair<uint16_t, cbmp::PlayerState>>& ents) {
    cbmp::Snapshot s{};
    s.tick = tick;
    s.partIndex = 0;
    s.partCount = 1;
    for (const auto& [id, st] : ents) {
        cbmp::SnapshotEntity e{};
        REQUIRE(cbmp::makeSnapshotEntity(id, nullptr, st, e));
        s.ents.push_back(e);
    }
    return s;
}

struct SessionFixture {
    FakeGame game;
    SessionTransport transport;
    FakeGate gate;
    FakeConnectFs fs;
    double now = 0;
    int64_t unixBase = kUnixBase;  // relógio Unix manual (somado ao `now`)
    std::vector<std::string> logs;
    std::vector<std::string> secretCalls;  // "set:<segredo>" / "clear", em ordem
    std::unique_ptr<Session> session;

    explicit SessionFixture(bool statusOnScreen = true, int maxPuppets = 32) {
        GoMenu();
        SessionConfig cfg;
        cfg.launchId = std::string(kLaunchId);
        cfg.buildHash.fill(0xAB);
        cfg.statusOnScreen = statusOnScreen;
        cfg.maxPuppets = maxPuppets;
        session = std::make_unique<Session>(
            SessionDeps{game, transport, gate, fs,
                        [this](Level, std::string_view m) { logs.emplace_back(m); },
                        [this] { return unixBase + static_cast<int64_t>(now); },
                        [this](std::string_view s) { secretCalls.push_back("set:" + std::string(s)); },
                        [this] { secretCalls.emplace_back("clear"); }},
            cfg);
    }

    Session& S() { return *session; }

    void Step(double dt = 0.016) {
        now += dt;
        session->Tick(now);
    }
    void RunFor(double seconds, double step = 0.016) {
        const double end = now + seconds;
        while (now + step <= end + 1e-9) Step(step);
    }
    // Roda até a condição valer (no máximo `limit` s). Devolve se valeu.
    template <class F>
    bool RunUntil(F cond, double limit = 120, double step = 0.016) {
        const double end = now + limit;
        while (!cond()) {
            if (now > end) return false;
            Step(step);
        }
        return true;
    }

    // Jogador dentro do mundo, longe da origem.
    void EnterWorld(double x = 10000, double y = 20000, double z = 500) {
        game.world = WorldSample{1, "Overland", 10, true, false};
        game.local = LocalState{Vec3{x, y, z}, 90.0, 0.0, Vec3{}, MoveMode::Ground};
    }
    void GoMenu() {
        game.world = WorldSample{100, "RootLevel", 99, true, false};  // mundo e pawn do menu
        game.local.reset();
    }

    // Até Handshake: resgate (connect.json válido), Connecting, Connected.
    void GoHandshake() {
        if (fs.results.empty()) fs.results.push_back(Ok(ValidConnect()));
        REQUIRE(RunUntil([&] { return S().State() != SessionState::Armed; }));
        REQUIRE(S().State() == SessionState::Connecting);
        transport.state = LinkState::Connected;
        Step();
        REQUIRE(S().State() == SessionState::Handshake);
    }
    void PushWelcome(uint16_t playerId = 7, uint8_t hz = 20) {
        cbmp::Welcome w{};
        w.playerId = playerId;
        w.tickRateHz = hz;
        transport.Push(w);
    }
    // Até Online/AwaitWorld (sem entrar no mundo).
    void GoAwaitWorld(uint8_t hz = 20) {
        gate.epoch = gate.latest = 2;  // sucessor: resgata aos 5 s, mesmo no menu
        GoHandshake();
        PushWelcome(7, hz);
        Step();
        REQUIRE(S().State() == SessionState::Online);
        REQUIRE(S().Phase() == OnlinePhase::AwaitWorld);
    }
    // Até Online/Playing com Welcome{playerId 7, hz}.
    void GoOnline(uint8_t hz = 20) {
        EnterWorld();
        GoHandshake();
        PushWelcome(7, hz);
        REQUIRE(RunUntil([&] { return S().State() == SessionState::Online && S().Phase() == OnlinePhase::Playing; }, 10));
    }

    int LogCount(std::string_view s) const {
        int n = 0;
        for (const auto& l : logs) n += l.find(s) != std::string::npos ? 1 : 0;
        return n;
    }
    bool LogHas(std::string_view s) const { return LogCount(s) > 0; }

    template <class T>
    std::vector<T> SentOf() const {
        std::vector<T> out;
        for (const auto& m : transport.SentMessages())
            if (const auto* x = std::get_if<T>(&m)) out.push_back(*x);
        return out;
    }
    template <class T>
    size_t CountSent() const {
        return SentOf<T>().size();
    }
    int StatusCount(std::string_view text) const {
        int n = 0;
        for (const auto& s : game.statuses) n += s == text ? 1 : 0;
        return n;
    }
};
}  // namespace cbmp::client::test
