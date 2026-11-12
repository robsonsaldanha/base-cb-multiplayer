// GnsTransport contra um servidor GNS no mesmo processo (loopback): conexão, envio, recepção e o fim com motivo, com
// as mensagens que chegaram antes do fechamento ainda disponíveis (Kick → 1004 depende disso).
#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/GnsTransport.hpp>
#include <cbmp/client/Limits.hpp>

#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

using namespace cbmp::client;

namespace {
// Servidor mínimo: aceita a primeira conexão que chegar no socket de escuta (callback global só neste teste).
struct LoopServer {
    static inline LoopServer* self = nullptr;
    ISteamNetworkingSockets* gns = SteamNetworkingSockets();
    HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
    HSteamNetConnection peer = k_HSteamNetConnection_Invalid;
    uint16_t port = 0;

    static void OnStatus(SteamNetConnectionStatusChangedCallback_t* info) {
        if (!self) return;
        if (info->m_info.m_hListenSocket == self->listen &&
            info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
            self->peer == k_HSteamNetConnection_Invalid) {
            if (self->gns->AcceptConnection(info->m_hConn) == k_EResultOK) self->peer = info->m_hConn;
        }
    }

    LoopServer() {
        self = this;
        std::mt19937 rng{std::random_device{}()};
        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&LoopServer::OnStatus);
        for (int attempt = 0; attempt < 50 && listen == k_HSteamListenSocket_Invalid; ++attempt) {
            SteamNetworkingIPAddr addr;
            addr.Clear();
            addr.SetIPv4(0x7F000001, static_cast<uint16_t>(std::uniform_int_distribution<int>(41000, 60999)(rng)));
            listen = gns->CreateListenSocketIP(addr, 0, nullptr);
            if (listen != k_HSteamListenSocket_Invalid) port = addr.m_port;
        }
    }
    ~LoopServer() {
        if (peer != k_HSteamNetConnection_Invalid) gns->CloseConnection(peer, 0, nullptr, false);
        if (listen != k_HSteamListenSocket_Invalid) gns->CloseListenSocket(listen);
        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(nullptr);
        self = nullptr;
    }
    void Pump() { gns->RunCallbacks(); }
    std::vector<std::string> Receive() {
        std::vector<std::string> out;
        if (peer == k_HSteamNetConnection_Invalid) return out;
        SteamNetworkingMessage_t* msgs[16];
        int n;
        while ((n = gns->ReceiveMessagesOnConnection(peer, msgs, 16)) > 0) {
            for (int i = 0; i < n; ++i) {
                out.emplace_back(static_cast<const char*>(msgs[i]->m_pData), static_cast<size_t>(msgs[i]->m_cbSize));
                msgs[i]->Release();
            }
        }
        return out;
    }
    void Send(const std::string& s) {
        gns->SendMessageToConnection(peer, s.data(), static_cast<uint32_t>(s.size()), k_nSteamNetworkingSend_Reliable,
                                     nullptr);
    }
};

// GNS iniciado no teste e encerrado no fim (antes do fim do processo: sem o Kill, o LeakSanitizer acusa as conexões
// e sockets internos do GNS). Declarar antes de transportes e servidores, para ser destruído por último.
struct GnsScope {
    GnsScope() { REQUIRE(GnsInit()); }
    ~GnsScope() { GnsShutdown(); }
};

template <class F>
bool WaitFor(F cond, double seconds = 10) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::span<const uint8_t> Bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
}  // namespace

TEST_CASE("client: GnsTransport init and shutdown are idempotent") {
    std::string err;
    REQUIRE(GnsInit(&err));
    REQUIRE(GnsInit(&err));
    GnsShutdown();
    GnsShutdown();
    REQUIRE(GnsInit(&err));  // de novo depois do Kill
    GnsShutdown();
}

TEST_CASE("client: GnsTransport refuses bad addresses and sends nothing before connecting") {
    GnsScope gnsScope;
    GnsTransport t;
    CHECK(t.State() == LinkState::Idle);
    const std::string hello = "x";
    CHECK_FALSE(t.Send(Bytes(hello), true));
    CHECK_FALSE(t.Connect("servidor.exemplo", 27015));
    CHECK_FALSE(t.Connect("127.0.0.1", 0));
    CHECK(t.State() == LinkState::Idle);
    std::vector<std::vector<uint8_t>> out;
    CHECK(t.Receive(out, 10) == 0);
    CHECK(t.Pending() == 0);
    t.Close();
    CHECK(t.State() == LinkState::Idle);
}

TEST_CASE("client: GnsTransport connects, exchanges messages and keeps messages queued before the peer closed") {
    GnsScope gnsScope;
    LoopServer srv;
    REQUIRE(srv.port != 0);
    GnsTransport t;
    REQUIRE(t.Connect("127.0.0.1", srv.port));
    CHECK_FALSE(t.Connect("127.0.0.1", srv.port));  // uma conexão por transporte
    std::vector<std::vector<uint8_t>> out;
    REQUIRE(WaitFor([&] {
        srv.Pump();
        t.Receive(out, 16);
        return t.State() == LinkState::Connected && srv.peer != k_HSteamNetConnection_Invalid;
    }));

    // Cliente → servidor, confiável e não confiável.
    CHECK(t.Send(Bytes("confiavel"), true));
    CHECK(t.Send(Bytes("rapido"), false));
    std::vector<std::string> got;
    REQUIRE(WaitFor([&] {
        srv.Pump();
        for (auto& s : srv.Receive()) got.push_back(s);
        return got.size() >= 2;
    }));
    CHECK(got[0] == "confiavel");
    CHECK(got[1] == "rapido");

    // Servidor → cliente, com Receive limitado por max.
    for (int i = 0; i < 5; ++i) srv.Send("m" + std::to_string(i));
    out.clear();
    REQUIRE(WaitFor([&] {
        srv.Pump();
        t.Receive(out, 2);
        return out.size() >= 5;
    }));
    CHECK(std::string(out[0].begin(), out[0].end()) == "m0");
    CHECK(std::string(out[4].begin(), out[4].end()) == "m4");

    // Três mensagens e o fechamento com motivo da aplicação: State() drena antes de fechar.
    srv.Send("a");
    srv.Send("b");
    srv.Send("kick");
    srv.gns->CloseConnection(srv.peer, limits::kEndKicked, "expulso pelo teste", true);
    srv.peer = k_HSteamNetConnection_Invalid;
    REQUIRE(WaitFor([&] { return t.State() == LinkState::Closed; }));
    CHECK(t.End().code == limits::kEndKicked);
    CHECK(t.End().debug == "expulso pelo teste");
    CHECK(t.Pending() == 3);
    out.clear();
    CHECK(t.Receive(out, 512) == 3);
    CHECK(std::string(out[2].begin(), out[2].end()) == "kick");
    CHECK_FALSE(t.Send(Bytes("depois"), true));
    t.Close();
    CHECK(t.State() == LinkState::Closed);
}

TEST_CASE("client: GnsTransport closes cleanly while still connecting") {
    GnsScope gnsScope;
    GnsTransport t;
    REQUIRE(t.Connect("127.0.0.1", 9));  // porta sem ninguém
    CHECK(t.State() != LinkState::Connected);
    t.Close();
    CHECK(t.State() == LinkState::Closed);
    CHECK_FALSE(t.Send(Bytes("x"), true));

    // IPv6 sem colchetes também é aceito (o transporte põe os colchetes); a máquina pode não ter IPv6, então só
    // confere que não lança e fecha limpo.
    GnsTransport t6;
    if (t6.Connect("::1", 9)) {
        CHECK(t6.State() != LinkState::Connected);
        t6.Close();
        CHECK(t6.State() == LinkState::Closed);
    }
}
