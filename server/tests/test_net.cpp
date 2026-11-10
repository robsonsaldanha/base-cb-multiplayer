// Loopback real com GameNetworkingSockets: servidor e clientes no mesmo processo, porta efêmera.
#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include "cbserver/auth.hpp"
#include "cbserver/metrics.hpp"
#include "cbserver/net.hpp"
#include "cbserver/pool.hpp"
#include "cbserver/snapshots.hpp"
#include "cbserver/world.hpp"
#include "ticket_helper.hpp"

using cbserver::Inbound;
using cbserver::NetServer;
using cbserver::ServerConfig;
using testing_ticket::DevSigner;

namespace {

using Clock = std::chrono::steady_clock;

int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct TestClient {
  HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
  std::vector<cbmp::Message> got;

  explicit TestClient(uint16_t port) {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.SetIPv4(0x7F000001, port);
    conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 0, nullptr);
    REQUIRE(conn != k_HSteamNetConnection_Invalid);
  }
  ~TestClient() { SteamNetworkingSockets()->CloseConnection(conn, 0, nullptr, false); }
  TestClient(const TestClient&) = delete;
  TestClient& operator=(const TestClient&) = delete;

  ESteamNetworkingConnectionState state() const {
    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info)) return k_ESteamNetworkingConnectionState_None;
    return info.m_eState;
  }
  int endReason() const {
    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info)) return -1;
    return info.m_eEndReason;
  }
  bool connected() const { return state() == k_ESteamNetworkingConnectionState_Connected; }
  bool closed() const {
    auto s = state();
    return s == k_ESteamNetworkingConnectionState_ClosedByPeer ||
           s == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || s == k_ESteamNetworkingConnectionState_None;
  }
  void sendRaw(const std::vector<uint8_t>& b, bool reliable = true) {
    SteamNetworkingSockets()->SendMessageToConnection(
        conn, b.data(), static_cast<uint32_t>(b.size()),
        reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
  }
  void send(const cbmp::Message& m) { sendRaw(cbmp::encode(m), cbmp::isReliable(cbmp::typeOf(m))); }
  void poll() {
    SteamNetworkingMessage_t* msgs[32];
    for (;;) {
      const int n = SteamNetworkingSockets()->ReceiveMessagesOnConnection(conn, msgs, 32);
      if (n <= 0) return;
      for (int i = 0; i < n; ++i) {
        const auto* p = static_cast<const uint8_t*>(msgs[i]->m_pData);
        got.push_back(cbmp::decode(std::span<const uint8_t>(p, static_cast<size_t>(msgs[i]->m_cbSize))));
        msgs[i]->Release();
      }
    }
  }
  template <class T>
  const T* first() const {
    for (const auto& m : got)
      if (const T* p = std::get_if<T>(&m)) return p;
    return nullptr;
  }
};

cbserver::NetOptions loopback(cbserver::NetOptions o = {}) {
  o.bindAddress = "127.0.0.1";
  return o;
}

struct Fixture {
  explicit Fixture(ServerConfig c = {}, cbserver::NetOptions opt = loopback())
      : cfg(std::move(c)),
        keys(dev.keys()),
        auth(keys, "srv_dev", [] { return unixNow(); }),
        pool(1),
        builder(pool),
        world(cfg),
        net(cfg, auth, world, metrics, opt) {
    std::string err;
    const bool ok = net.start(0, err);
    INFO(err);
    REQUIRE(ok);
    port = net.boundPort();
    REQUIRE(port != 0);
  }
  ~Fixture() { net.stop(); }

  DevSigner dev;
  ServerConfig cfg;
  cbserver::StaticKeys keys;
  cbserver::Authenticator auth;
  cbserver::Metrics metrics;
  cbserver::TaskPool pool;
  cbserver::SnapshotBuilder builder;
  cbserver::World world;
  NetServer net;
  uint16_t port = 0;
  std::vector<Inbound> in;
  Clock::time_point t0 = Clock::now();

  double now() const { return std::chrono::duration<double>(Clock::now() - t0).count(); }
  void pump() { net.pump(now(), in); }

  template <class Pred>
  bool until(Pred p, std::initializer_list<TestClient*> clients = {}, double timeoutSec = 5.0) {
    const auto end = Clock::now() + std::chrono::duration<double>(timeoutSec);
    while (Clock::now() < end) {
      pump();
      for (auto* c : clients) c->poll();
      if (p()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  cbmp::Hello hello(const std::string& sub, const std::string& jti) {
    cbmp::Hello h;
    h.ticket = dev.sign(sub, jti, unixNow());
    return h;
  }

  void connect(TestClient& c) { REQUIRE(until([&] { return c.connected(); }, {&c})); }

  void login(TestClient& c, const std::string& sub, const std::string& jti) {
    connect(c);
    c.send(hello(sub, jti));
    REQUIRE(until([&] { return c.first<cbmp::Welcome>() || c.first<cbmp::Reject>(); }, {&c}));
    REQUIRE(c.first<cbmp::Welcome>());
  }
};

}  // namespace

TEST_CASE("net handshake with valid ticket receives Welcome and inputs reach the world") {
  Fixture f;
  TestClient c(f.port);
  f.login(c, "u1", "j1");
  const auto* w = c.first<cbmp::Welcome>();
  REQUIRE(w->playerId == 1);
  REQUIRE(w->tickRateHz == 20);
  REQUIRE(f.world.playerCount() == 1);
  REQUIRE(f.world.find(1)->sub == "u1");
  REQUIRE(f.net.pendingCount() == 0);
  REQUIRE(f.net.sessionCount() == 1);
  REQUIRE(f.metrics.players.load() == 1);

  cbmp::Input in;
  in.seq = 1;
  c.send(in);
  f.in.clear();
  REQUIRE(f.until([&] { return !f.in.empty(); }));
  REQUIRE(f.in[0].from == 1);
  REQUIRE(std::get<cbmp::Input>(f.in[0].msg).seq == 1);

  // saída do mundo chega ao cliente: Correction de spawn (confiável) e Snapshot (não confiável)
  std::vector<cbserver::Outgoing> out;
  std::vector<cbserver::KickOrder> kicks;
  f.world.tick(f.now(), f.builder, out, kicks);
  f.net.send(out);
  REQUIRE(f.until([&] { return c.first<cbmp::Correction>() && c.first<cbmp::Snapshot>(); }, {&c}));
  REQUIRE(f.metrics.snapshot()["maxKbpsPerPlayer"].get<double>() > 0);  // bytes contados
}

TEST_CASE("net invalid ticket gets Reject and the connection closes") {
  Fixture f;
  TestClient c(f.port);
  f.connect(c);
  cbmp::Hello h;
  h.ticket = f.dev.sign("u1", "j1", unixNow(), "srv_outro");
  c.send(h);
  REQUIRE(f.until([&] { return c.first<cbmp::Reject>() != nullptr && c.closed(); }, {&c}));
  REQUIRE(c.first<cbmp::Reject>()->reason.find("wrong_audience") != std::string::npos);
  REQUIRE(f.world.playerCount() == 0);
  REQUIRE(f.net.pendingCount() == 0);
}

TEST_CASE("net protocol mismatch gets Reject") {
  Fixture f;
  TestClient c(f.port);
  f.connect(c);
  auto h = f.hello("u1", "j1");
  h.protocol = cbmp::kProtocolVersion + 1;
  c.send(h);
  REQUIRE(f.until([&] { return c.first<cbmp::Reject>() != nullptr && c.closed(); }, {&c}));
  REQUIRE(c.first<cbmp::Reject>()->reason == "protocolo incompatível");
}

TEST_CASE("net connection without Hello is closed after the hello timeout") {
  cbserver::NetOptions opt;
  opt.helloTimeoutSec = 0.2;
  Fixture f({}, loopback(opt));
  TestClient c(f.port);
  f.connect(c);
  REQUIRE(f.net.pendingCount() == 1);
  const auto t = Clock::now();
  REQUIRE(f.until([&] { return c.closed(); }, {&c}));
  REQUIRE(Clock::now() - t >= std::chrono::milliseconds(150));
  REQUIRE(f.net.pendingCount() == 0);
}

TEST_CASE("net fifth pending connection from the same IP is refused") {
  Fixture f;
  std::vector<std::unique_ptr<TestClient>> cs;
  for (int i = 0; i < 4; ++i) {
    cs.push_back(std::make_unique<TestClient>(f.port));
    f.connect(*cs.back());
  }
  REQUIRE(f.net.pendingCount() == 4);
  TestClient fifth(f.port);
  REQUIRE(f.until([&] { return fifth.closed(); }, {&fifth}));
  REQUIRE_FALSE(fifth.connected());
  REQUIRE(f.net.pendingCount() == 4);
  for (auto& c : cs) REQUIRE(c->connected());
  // um pendente autentica → libera vaga
  cs[0]->send(f.hello("u1", "j1"));
  REQUIRE(f.until([&] { return cs[0]->first<cbmp::Welcome>() != nullptr; }, {cs[0].get()}));
  TestClient sixth(f.port);
  f.connect(sixth);
  REQUIRE(f.net.pendingCount() == 4);
}

TEST_CASE("net global pending limit is enforced") {
  ServerConfig cfg;
  cfg.pendingGlobal = 2;
  Fixture f(cfg);
  TestClient a(f.port), b(f.port);
  f.connect(a);
  f.connect(b);
  TestClient c(f.port);
  REQUIRE(f.until([&] { return c.closed(); }, {&c}));
  REQUIRE(f.net.pendingCount() == 2);
}

TEST_CASE("net second login of the same sub kicks the first session") {
  Fixture f;
  TestClient first(f.port);
  f.login(first, "u1", "j1");
  TestClient second(f.port);
  f.login(second, "u1", "j2");
  REQUIRE(f.until([&] { return first.first<cbmp::Kick>() != nullptr && first.closed(); }, {&first, &second}));
  REQUIRE(first.first<cbmp::Kick>()->reason == "conectado em outro lugar");
  REQUIRE(second.connected());
  REQUIRE(f.world.playerCount() == 1);
  REQUIRE(f.net.sessionCount() == 1);
}

TEST_CASE("net full server rejects with servidor cheio") {
  ServerConfig cfg;
  cfg.maxPlayers = 1;
  Fixture f(cfg);
  TestClient a(f.port);
  f.login(a, "u1", "j1");
  TestClient b(f.port);
  f.connect(b);
  b.send(f.hello("u2", "j2"));
  REQUIRE(f.until([&] { return b.first<cbmp::Reject>() != nullptr && b.closed(); }, {&b}));
  REQUIRE(b.first<cbmp::Reject>()->reason == "servidor cheio");
}

TEST_CASE("net Input before Hello closes the connection") {
  Fixture f;
  TestClient c(f.port);
  f.connect(c);
  cbmp::Input in;
  in.seq = 1;
  c.sendRaw(cbmp::encode(in), true);
  REQUIRE(f.until([&] { return c.closed(); }, {&c}));
  REQUIRE(f.net.pendingCount() == 0);
  REQUIRE(f.world.playerCount() == 0);
}

TEST_CASE("net garbage message closes the connection and is counted") {
  Fixture f;
  TestClient c(f.port);
  f.connect(c);
  c.sendRaw({0xEE, 0x01, 0x02});
  REQUIRE(f.until([&] { return c.closed(); }, {&c}));
  REQUIRE(f.metrics.decodeErrors.load() == 1);

  SECTION("garbage after login also closes and removes the player") {
    TestClient d(f.port);
    f.login(d, "u1", "j1");
    d.sendRaw({0xEE});
    REQUIRE(f.until([&] { return d.closed(); }, {&d}));
    REQUIRE(f.metrics.decodeErrors.load() == 2);
    REQUIRE(f.world.playerCount() == 0);
    REQUIRE(f.net.sessionCount() == 0);
  }
}

TEST_CASE("net oversized Hello and server-only messages close the connection") {
  Fixture f;
  TestClient big(f.port);
  f.connect(big);
  std::vector<uint8_t> b(5000, 0);
  b[0] = static_cast<uint8_t>(cbmp::MsgType::Hello);
  big.sendRaw(b);
  REQUIRE(f.until([&] { return big.closed(); }, {&big}));

  TestClient c(f.port);
  f.login(c, "u1", "j1");
  c.send(cbmp::Kick{"x"});  // só o servidor envia Kick
  REQUIRE(f.until([&] { return c.closed(); }, {&c}));
  REQUIRE(f.world.playerCount() == 0);
}

TEST_CASE("net kick sends Kick, closes and removes the player") {
  Fixture f;
  TestClient c(f.port);
  f.login(c, "u1", "j1");
  f.net.kick(1, "movimento inválido");
  REQUIRE(f.until([&] { return c.first<cbmp::Kick>() != nullptr && c.closed(); }, {&c}));
  REQUIRE(c.first<cbmp::Kick>()->reason == "movimento inválido");
  REQUIRE(f.world.playerCount() == 0);
  REQUIRE(f.net.sessionCount() == 0);
}

TEST_CASE("net client disconnect removes the player") {
  Fixture f;
  auto c = std::make_unique<TestClient>(f.port);
  f.login(*c, "u1", "j1");
  c.reset();
  REQUIRE(f.until([&] { return f.world.playerCount() == 0; }));
  REQUIRE(f.net.sessionCount() == 0);
}

TEST_CASE("net start on port 0 picks a port outside the OS ephemeral range") {
  Fixture f;
  REQUIRE(f.port >= 20000);
  REQUIRE(f.port <= 32767);
}

TEST_CASE("net oversized message before auth is dropped by GNS receive limits") {
  Fixture f;
  TestClient c(f.port);
  f.connect(c);
  c.sendRaw(std::vector<uint8_t>(64 * 1024, static_cast<uint8_t>(cbmp::MsgType::Hello)));
  f.in.clear();
  // o GNS fecha a conexão sozinho; o callback de status chega ao servidor num pump seguinte
  REQUIRE(f.until([&] { return c.closed() && f.net.pendingCount() == 0; }, {&c}));
  // fechada pelo próprio GNS (limite de recepção), não pela checagem do Hello na aplicação
  const int reason = c.endReason();
  INFO("motivo " << reason);
  REQUIRE((reason < k_ESteamNetConnectionEnd_App_Min || reason > k_ESteamNetConnectionEnd_AppException_Max));
  REQUIRE(f.in.empty());
  REQUIRE(f.net.pendingCount() == 0);
  REQUIRE(f.metrics.decodeErrors.load() == 0);
}

TEST_CASE("net oversized message after auth closes and removes the player") {
  Fixture f;
  TestClient c(f.port);
  f.login(c, "u1", "j1");
  c.sendRaw(std::vector<uint8_t>(64 * 1024, static_cast<uint8_t>(cbmp::MsgType::Event)));
  f.in.clear();
  REQUIRE(f.until([&] { return c.closed() && f.world.playerCount() == 0; }, {&c}));
  const int reason = c.endReason();
  REQUIRE((reason < k_ESteamNetConnectionEnd_App_Min || reason > k_ESteamNetConnectionEnd_AppException_Max));
  REQUIRE(f.in.empty());
  REQUIRE(f.net.sessionCount() == 0);
}

TEST_CASE("net pending key groups IPv6 by /64 and keeps IPv4 whole") {
  std::array<uint8_t, 16> a{0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8};
  auto b = a;
  b[15] = 99;
  b[8] = 0xff;  // mesmo /64, outro host
  auto c = a;
  c[7] = 3;  // outro /64
  REQUIRE(cbserver::pendingIpKey(a) == cbserver::pendingIpKey(b));
  REQUIRE(cbserver::pendingIpKey(a) != cbserver::pendingIpKey(c));
  std::array<uint8_t, 16> v4a{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 0, 1};
  auto v4b = v4a;
  v4b[15] = 2;
  REQUIRE(cbserver::pendingIpKey(v4a) == "10.0.0.1");
  REQUIRE(cbserver::pendingIpKey(v4a) != cbserver::pendingIpKey(v4b));
}

TEST_CASE("net caps messages processed per pump and keeps the rest") {
  cbserver::NetOptions opt;
  opt.maxMessagesPerPump = 10;
  Fixture f({}, loopback(opt));
  TestClient c(f.port);
  f.login(c, "u1", "j1");
  for (uint32_t i = 1; i <= 25; ++i) {
    cbmp::Input in;
    in.seq = i;
    c.sendRaw(cbmp::encode(in), true);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // a thread de serviço do GNS recebe tudo
  f.in.clear();
  f.pump();
  REQUIRE(f.in.size() == 10);
  f.in.clear();
  f.pump();
  REQUIRE(f.in.size() == 10);
  f.in.clear();
  f.pump();
  REQUIRE(f.in.size() == 5);
  REQUIRE(std::get<cbmp::Input>(f.in.back().msg).seq == 25);
}

TEST_CASE("net reliable send over the buffer limit kicks the slow player") {
  cbserver::NetOptions opt;
  opt.sendBufferBytes = 16 * 1024;
  Fixture f({}, loopback(opt));
  TestClient c(f.port);
  f.login(c, "u1", "j1");
  std::vector<cbserver::Outgoing> out;
  for (int i = 0; i < 20; ++i) out.push_back({1, cbmp::Event{"x", std::vector<uint8_t>(4000, 7)}});
  f.net.send(out);
  REQUIRE(f.metrics.sendFailures.load() > 0);
  REQUIRE(f.world.playerCount() == 0);
  REQUIRE(f.net.sessionCount() == 0);
  REQUIRE(f.metrics.kicks.load() == 1);
  REQUIRE(f.until([&] { return c.closed(); }, {&c}));
}

TEST_CASE("net network-level closes of players are reported to the violation sink") {
  Fixture f;
  struct V {
    cbserver::EntityId id;
    std::string sub, did, reason;
  };
  std::vector<V> got;
  f.net.setViolationSink([&](cbserver::EntityId id, const std::string& sub, const std::string& did,
                             const std::string& reason) { got.push_back({id, sub, did, reason}); });
  TestClient a(f.port);
  f.login(a, "u1", "j1");
  a.sendRaw({0xEE});
  REQUIRE(f.until([&] { return a.closed(); }, {&a}));
  TestClient b(f.port);
  f.login(b, "u2", "j2");
  b.send(cbmp::Kick{"x"});
  REQUIRE(f.until([&] { return b.closed(); }, {&b}));
  REQUIRE(got.size() == 2);
  REQUIRE(got[0].sub == "u1");
  REQUIRE(got[0].did == "did-u1");
  REQUIRE(got[0].reason == "mensagem inválida");
  REQUIRE(got[1].sub == "u2");
  REQUIRE(got[1].reason == "mensagem não permitida");
}
