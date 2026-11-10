#include <httplib.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

#include <cbmp/base64url.hpp>

#include "cbserver/hub.hpp"

using namespace cbserver;
using namespace std::chrono_literals;

namespace {

std::string keyB64(uint8_t fill) {
  std::vector<uint8_t> k(32, fill);
  return cbmp::base64urlEncode(k);
}

template <class F>
bool waitFor(F f, std::chrono::milliseconds max = 5s) {
  const auto end = std::chrono::steady_clock::now() + max;
  while (std::chrono::steady_clock::now() < end) {
    if (f()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return f();
}

struct FakeHub {
  httplib::Server svr;
  std::thread th;
  int port = 0;
  std::atomic<int> keyGets{0}, beats{0};
  std::mutex m;
  nlohmann::json lastBody;
  std::string lastAuth;
  std::string keysBody;
  int beatStatus = 200;

  FakeHub() {
    keysBody = nlohmann::json::array({{{"kid", "k1"}, {"publicKey", keyB64(1)}},
                                      {{"kid", "k2"}, {"publicKey", keyB64(2)}},
                                      {{"kid", "bad"}, {"publicKey", "AAAA"}},         // curta
                                      {{"kid", "bad2"}, {"publicKey", keyB64(3) + "="}}})  // não canônica
                  .dump();
    svr.Get("/v1/keys/public", [this](const httplib::Request&, httplib::Response& r) {
      ++keyGets;
      std::lock_guard lk(m);
      r.set_content(keysBody, "application/json");
    });
    svr.Post("/v1/servers/heartbeat", [this](const httplib::Request& q, httplib::Response& r) {
      ++beats;
      std::lock_guard lk(m);
      lastAuth = q.get_header_value("Authorization");
      lastBody = nlohmann::json::parse(q.body, nullptr, false);
      r.status = beatStatus;
      r.set_content(R"({"ok":true,"listed":true,"nextHeartbeatSeconds":30})", "application/json");
    });
    port = svr.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { svr.listen_after_bind(); });
    svr.wait_until_ready();
  }
  ~FakeHub() {
    svr.stop();
    th.join();
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

}  // namespace

TEST_CASE("hubkeys loads valid keys skips invalid and throttles refresh") {
  FakeHub hub;
  std::atomic<double> now{1000};
  HubKeys keys(hub.url(), [&now] { return now.load(); });
  REQUIRE(hub.keyGets == 1);
  auto k = keys.keys();
  REQUIRE(k.size() == 2);
  REQUIRE(k.count("k1") == 1);
  REQUIRE(k.count("k2") == 1);
  REQUIRE(k.at("k1")[0] == 1);

  now = 1030;  // dentro de 60 s: nao busca
  keys.requestRefresh();
  std::this_thread::sleep_for(200ms);
  REQUIRE(hub.keyGets == 1);

  now = 1061;  // depois de 60 s: busca de novo
  keys.requestRefresh();
  REQUIRE(waitFor([&] { return hub.keyGets == 2; }));

  keys.requestRefresh();  // logo em seguida: limitado
  std::this_thread::sleep_for(200ms);
  REQUIRE(hub.keyGets == 2);
}

TEST_CASE("hubkeys retries in background when hub is down at start") {
  std::atomic<double> now{0};
  FakeHub hub;
  hub.keysBody = "oops";  // resposta invalida no inicio
  HubKeys keys(hub.url(), [&now] { return now.load(); }, 50ms);
  REQUIRE(keys.keys().empty());
  REQUIRE(waitFor([&] { return keys.fetchCount() >= 3; }));  // continua tentando, sem excecao
  {
    std::lock_guard lk(hub.m);
    hub.keysBody = nlohmann::json::array({{{"kid", "k"}, {"publicKey", keyB64(9)}}}).dump();
  }
  REQUIRE(waitFor([&] { return keys.keys().size() == 1; }));
  const int n = keys.fetchCount();
  std::this_thread::sleep_for(200ms);
  REQUIRE(keys.fetchCount() == n);  // com chaves carregadas, para de repetir
}

TEST_CASE("heartbeat sends expected body and bearer header") {
  FakeHub hub;
  HeartbeatInfo info;
  info.name = "  Castelo  ";
  info.description = "desc";
  info.maxPlayers = 100;
  info.gamePort = 30500;
  info.httpPort = 30501;
  info.version = "0.1.0";
  info.protocol = 1;
  info.tags = {"rp", "hp"};
  info.locale = "pt-BR";
  std::atomic<uint32_t> players{7};
  Heartbeat hb(hub.url(), "sk_secret", info, [&players] { return players.load(); },
               {std::chrono::milliseconds(60), std::chrono::milliseconds(240)});
  REQUIRE(waitFor([&] { return hub.beats >= 2; }));  // intervalo reduzido: manda varios
  hb.stop();
  std::lock_guard lk(hub.m);
  REQUIRE(hub.lastAuth == "Bearer sk_secret");
  REQUIRE(hub.lastBody["name"] == "Castelo");  // aparado
  REQUIRE(hub.lastBody["description"] == "desc");
  REQUIRE(hub.lastBody["players"] == 7);
  REQUIRE(hub.lastBody["maxPlayers"] == 100);
  REQUIRE(hub.lastBody["gamePort"] == 30500);
  REQUIRE(hub.lastBody["httpPort"] == 30501);
  REQUIRE(hub.lastBody["version"] == "0.1.0");
  REQUIRE(hub.lastBody["protocol"] == 1);
  REQUIRE(hub.lastBody["tags"] == nlohmann::json::array({"rp", "hp"}));
  REQUIRE(hub.lastBody["locale"] == "pt-BR");
  REQUIRE_FALSE(hub.lastBody.dump().find("sk_") != std::string::npos);
  REQUIRE_FALSE(hub.lastBody.contains("adminUrl"));  // sem admin.publicUrl: campo ausente
}

TEST_CASE("heartbeat sends adminUrl only when configured") {
  FakeHub hub;
  HeartbeatInfo info;
  info.name = "Servidor";
  info.version = "0.1.0";
  info.adminUrl = "https://painel.example.com";
  Heartbeat hb(hub.url(), "sk_x", info, [] { return 0u; }, {std::chrono::milliseconds(60), std::chrono::milliseconds(240)});
  REQUIRE(waitFor([&] { return hub.beats >= 1; }));
  hb.stop();
  std::lock_guard lk(hub.m);
  REQUIRE(hub.lastBody["adminUrl"] == "https://painel.example.com");
}

TEST_CASE("heartbeat backs off when hub is down and never throws") {
  FakeHub hub;
  hub.beatStatus = 503;  // hub fora do ar (conexao recusada em loopback demora no Windows; 503 e deterministico)
  HeartbeatInfo info;
  info.name = "Servidor";
  info.version = "0.1.0";
  Heartbeat hb(hub.url(), "sk_x", info, [] { return 0u; },
               {std::chrono::milliseconds(20), std::chrono::milliseconds(80)});
  REQUIRE(waitFor([&] { return hb.attempts() >= 4; }));
  REQUIRE(hb.lastDelay() == 80ms);  // 20 -> 40 -> 80 (maximo)
  hb.stop();
  hb.stop();  // idempotente
}

TEST_CASE("heartbeat backs off on rejected key and stops promptly") {
  FakeHub hub;
  hub.beatStatus = 401;
  HeartbeatInfo info;
  info.name = "Servidor";
  info.version = "0.1.0";
  Heartbeat hb(hub.url(), "sk_x", info, [] { return 0u; }, {std::chrono::seconds(30), std::chrono::seconds(120)});
  REQUIRE(waitFor([&] { return hub.beats >= 1; }));
  const auto t0 = std::chrono::steady_clock::now();
  hb.stop();  // acorda a espera de 30 s
  REQUIRE(std::chrono::steady_clock::now() - t0 < 2s);
}
