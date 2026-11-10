#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cbserver/config.hpp"

using cbserver::ServerConfig;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

static json base() {
  std::ifstream f(CBSERVER_CFG_EXAMPLE);
  return json::parse(f);
}

static void requireRejects(const json& j, const char* field) {
  try {
    (void)ServerConfig::fromJson(j);
    FAIL("deveria recusar " << field);
  } catch (const std::runtime_error& e) {
    REQUIRE_THAT(std::string(e.what()), ContainsSubstring(field));
  }
}

TEST_CASE("config example loads with defaults") {
  auto c = ServerConfig::load(CBSERVER_CFG_EXAMPLE);
  REQUIRE(c.maxPlayers == 512);
  REQUIRE(c.port == 30500);
  REQUIRE(c.tickHz == 20);
  REQUIRE(c.maxVisible == 128);
  REQUIRE(c.speedBroom == 4000);
  REQUIRE(c.tolerance == 0.2);
  REQUIRE(!c.hub.has_value());
  REQUIRE(!c.devPublicKey.has_value());
  REQUIRE(c.resourcesDir == "resources");
}

TEST_CASE("config empty object yields defaults") {
  auto c = ServerConfig::fromJson(json::object());
  REQUIRE(c.port == 30500);
  REQUIRE(c.eventsBurst == 40);
}

TEST_CASE("config rejects invalid values naming the field") {
  auto j = base(); j["maxPlayers"] = 0; requireRejects(j, "maxPlayers");
  j = base(); j["maxPlayers"] = 4096; requireRejects(j, "maxPlayers");
  j = base(); j["port"] = 70000; requireRejects(j, "port");
  j = base(); j["sim"]["tickHz"] = 61; requireRejects(j, "tickHz");
  j = base(); j["sim"]["streamRadiusM"] = 10; requireRejects(j, "streamRadiusM");
  j = base(); j["sim"]["maxVisible"] = 4; requireRejects(j, "maxVisible");
  j = base(); j["movement"]["tolerance"] = 1.5; requireRejects(j, "tolerance");
  j = base(); j["movement"]["maxSpeedCmS"]["run"] = -1; requireRejects(j, "run");
  j = base(); j["port"] = "abc"; requireRejects(j, "port");
}

TEST_CASE("config hub validation") {
  auto j = base();
  j["hub"] = {{"url", "https://hub.exemplo"}, {"serverId", "srv_abc"}};
  requireRejects(j, "key");
  j["hub"] = {{"url", "https://hub.exemplo"}, {"serverId", "abc"}, {"key", "sk_x"}};
  requireRejects(j, "serverId");
  j["hub"] = {{"url", "ftp://x"}, {"serverId", "srv_abc"}, {"key", "sk_x"}};
  requireRejects(j, "url");
  j["hub"] = {{"url", "https://hub.exemplo"}, {"serverId", "srv_abc"}, {"key", "xx"}};
  requireRejects(j, "key");
  j["hub"] = {{"url", "https://hub.exemplo"}, {"serverId", "srv_abc"}, {"key", "sk_x"}};
  auto c = ServerConfig::fromJson(j);
  REQUIRE(c.hub.has_value());
  REQUIRE(c.hub->serverId == "srv_abc");
}

TEST_CASE("config devPublicKey validation") {
  auto j = base();
  j["auth"]["devPublicKey"] = "nao-e-chave";
  requireRejects(j, "devPublicKey");
  j["auth"]["devPublicKey"] = std::string(43, 'A');  // 32 bytes, canonico
  REQUIRE(ServerConfig::fromJson(j).devPublicKey.has_value());
  j["auth"]["devPublicKey"] = std::string(42, 'A');  // 31 bytes
  requireRejects(j, "devPublicKey");
}

TEST_CASE("config load of missing file throws") {
  REQUIRE_THROWS_AS(ServerConfig::load("nao/existe.cfg"), std::runtime_error);
}

TEST_CASE("config validates hub-compatible name description tags locale") {
  auto j = base(); j["name"] = "ab"; requireRejects(j, "name");
  j = base(); j["name"] = "  ab  "; requireRejects(j, "name");
  j = base(); j["name"] = std::string(65, 'x'); requireRejects(j, "name");
  j = base(); j["description"] = std::string(501, 'x'); requireRejects(j, "description");
  j = base(); j["tags"] = json::array({"ok", "Mai"}); requireRejects(j, "tags");
  j = base(); j["tags"] = json::array({std::string(25, 'a')}); requireRejects(j, "tags");
  j = base(); j["tags"] = json::array({"a","b","c","d","e","f","g","h","i","j","k"}); requireRejects(j, "tags");
  j = base(); j["locale"] = "pt_BR"; requireRejects(j, "locale");
  j = base(); j["locale"] = "en"; REQUIRE(ServerConfig::fromJson(j).locale == "en");
}

TEST_CASE("config sim.spawn defaults to origin and accepts origin explicitly") {
  REQUIRE(ServerConfig::fromJson(base()).spawnMode == cbserver::SpawnMode::Origin);
  auto j = base();
  j["sim"]["spawn"] = {{"mode", "origin"}};
  REQUIRE(ServerConfig::fromJson(j).spawnMode == cbserver::SpawnMode::Origin);
  j["sim"]["spawn"] = {{"mode", "aleatorio"}};
  requireRejects(j, "sim.spawn");
  j["sim"]["spawn"] = "origin";
  requireRejects(j, "sim.spawn");
}

TEST_CASE("config devTrustFirstInput only in dev mode without hub") {
  auto j = base();
  j["sim"]["spawn"] = {{"mode", "devTrustFirstInput"}};
  requireRejects(j, "sim.spawn");  // sem devPublicKey
  j["auth"]["devPublicKey"] = std::string(43, 'A');
  REQUIRE(ServerConfig::fromJson(j).spawnMode == cbserver::SpawnMode::DevTrustFirstInput);
  j["auth"]["devPublicKey"] = nullptr;
  j["hub"] = {{"url", "https://hub.exemplo"}, {"serverId", "srv_abc"}, {"key", "sk_x"}};
  requireRejects(j, "sim.spawn");  // com hub (e sem chave dev)
}

TEST_CASE("config hub and devPublicKey are mutually exclusive") {
  auto j = base();
  j["hub"] = {{"url", "https://hub.exemplo"}, {"serverId", "srv_abc"}, {"key", "sk_x"}};
  j["auth"]["devPublicKey"] = std::string(43, 'A');
  requireRejects(j, "hub");
  requireRejects(j, "auth.devPublicKey");
  j["auth"]["devPublicKey"] = nullptr;
  REQUIRE(ServerConfig::fromJson(j).hub.has_value());
}

TEST_CASE("config scripting section has defaults and validates its limits") {
  auto d = ServerConfig::fromJson(json::object());
  REQUIRE(d.scriptMemoryLimitMB == 256);
  REQUIRE(d.scriptWarnMs == 50);
  REQUIRE(d.scriptAbortMs == 5000);
  REQUIRE(d.bansFile == "bans.json");
  auto ex = ServerConfig::load(CBSERVER_CFG_EXAMPLE);
  REQUIRE(ex.scriptMemoryLimitMB == 256);

  auto j = base();
  j["scripting"] = {{"memoryLimitMB", 64}, {"warnMs", 20}, {"abortMs", 1000}, {"bansFile", "dados/bans.json"}};
  auto c = ServerConfig::fromJson(j);
  REQUIRE(c.scriptMemoryLimitMB == 64);
  REQUIRE(c.scriptWarnMs == 20);
  REQUIRE(c.scriptAbortMs == 1000);
  REQUIRE(c.bansFile == "dados/bans.json");
  REQUIRE(d.scriptEventOutKBps == 8);
  REQUIRE(d.scriptEventOutBurstKB == 32);
  auto out = base();
  out["scripting"] = {{"eventOutKBps", 32}, {"eventOutBurstKB", 256}};
  auto o = ServerConfig::fromJson(out);
  REQUIRE(o.scriptEventOutKBps == 32);
  REQUIRE(o.scriptEventOutBurstKB == 256);
  out["scripting"] = {{"eventOutBurstKB", 1}};
  requireRejects(out, "scripting.eventOutBurstKB");

  auto bad = base();
  bad["scripting"] = {{"memoryLimitMB", 0}};
  requireRejects(bad, "scripting.memoryLimitMB");
  bad["scripting"] = {{"warnMs", 0}};
  requireRejects(bad, "scripting.warnMs");
  bad["scripting"] = {{"warnMs", 500}, {"abortMs", 100}};
  requireRejects(bad, "scripting.abortMs");
  bad["scripting"] = 5;
  requireRejects(bad, "scripting");
}

TEST_CASE("config db section is optional, has defaults and never echoes the password in errors") {
  REQUIRE_FALSE(ServerConfig::fromJson(json::object()).db.has_value());
  REQUIRE_FALSE(ServerConfig::load(CBSERVER_CFG_EXAMPLE).db.has_value());  // exemplo: "db": null
  auto j = base();
  j["db"] = {{"host", "db.local"}, {"user", "cb"}, {"password", "s3cr3t-xyz"}, {"database", "castelo"}};
  auto c = ServerConfig::fromJson(j);
  REQUIRE(c.db.has_value());
  REQUIRE(c.db->host == "db.local");
  REQUIRE(c.db->port == 3306);
  REQUIRE(c.db->user == "cb");
  REQUIRE(c.db->password == "s3cr3t-xyz");
  REQUIRE(c.db->database == "castelo");
  REQUIRE(c.db->pool == 4);
  REQUIRE(c.db->timeoutMs == 5000);
  REQUIRE(c.db->tls == "auto");
  j["db"]["port"] = 3307;
  j["db"]["pool"] = 8;
  j["db"]["timeoutMs"] = 2500;
  j["db"]["tls"] = "verify";
  c = ServerConfig::fromJson(j);
  REQUIRE(c.db->port == 3307);
  REQUIRE(c.db->pool == 8);
  REQUIRE(c.db->timeoutMs == 2500);
  REQUIRE(c.db->tls == "verify");

  auto rejects = [&](const char* key, json value, const char* field) {
    auto b = j;
    if (value.is_null())
      b["db"].erase(key);
    else
      b["db"][key] = value;
    try {
      (void)ServerConfig::fromJson(b);
      FAIL("deveria recusar " << field);
    } catch (const std::runtime_error& e) {
      const std::string m = e.what();
      REQUIRE_THAT(m, ContainsSubstring(field));
      REQUIRE(m.find("s3cr3t-xyz") == std::string::npos);
    }
  };
  rejects("host", nullptr, "db.host");
  rejects("host", "", "db.host");
  rejects("user", nullptr, "db.user");
  rejects("database", nullptr, "db.database");
  rejects("pool", 0, "db.pool");
  rejects("pool", 65, "db.pool");
  rejects("timeoutMs", 50, "db.timeoutMs");
  rejects("port", 0, "db.port");
  rejects("tls", "talvez", "db.tls");
  rejects("password", 5, "db.password");
  auto bad = base();
  bad["db"] = "mysql://x";
  requireRejects(bad, "db");
}

TEST_CASE("config admin publicUrl is optional and normalized to an origin") {
  auto none = ServerConfig::fromJson(json::object());
  REQUIRE(!none.admin.publicUrl.has_value());
  auto nullAdmin = ServerConfig::fromJson(json{{"admin", nullptr}});
  REQUIRE(!nullAdmin.admin.publicUrl.has_value());
  auto c = ServerConfig::fromJson(json{{"admin", {{"publicUrl", "https://Painel.Example.com:8443/"}}}});
  REQUIRE(c.admin.publicUrl == "https://painel.example.com:8443");
  REQUIRE(ServerConfig::fromJson(json{{"admin", {{"publicUrl", "https://painel.example.com:443"}}}}).admin.publicUrl ==
          "https://painel.example.com");
  REQUIRE(ServerConfig::fromJson(json{{"admin", {{"publicUrl", "http://127.0.0.1:30510"}}}}).admin.publicUrl ==
          "http://127.0.0.1:30510");
  REQUIRE(ServerConfig::fromJson(json{{"admin", {{"publicUrl", "http://[::1]:30510"}}}}).admin.publicUrl ==
          "http://[::1]:30510");
  REQUIRE(ServerConfig::fromJson(json{{"admin", {{"publicUrl", "http://localhost:30510"}}}}).admin.publicUrl ==
          "http://localhost:30510");
  REQUIRE(ServerConfig::fromJson(json{{"admin", {{"publicUrl", "https://[2001:db8::1]:8443"}}}}).admin.publicUrl ==
          "https://[2001:db8::1]:8443");
}

TEST_CASE("config admin publicUrl rejects anything but a safe origin") {
  for (const char* u :
       {"http://painel.example.com", "http://203.0.113.5:30510", "http://127.0.0.1.nip.io", "http://[::2]",
        "http://127.0.0.256", "https://painel.example.com/admin", "https://painel.example.com?x=1",
        "https://painel.example.com#x", "https://user:pw@painel.example.com", "https://painel.example.com\\@evil.com",
        " https://painel.example.com", "https://painel.example.com ", "https://painel.example.com\n",
        "https://painel.example.com//", "javascript:alert(1)", "ftp://painel.example.com", "https://", "painel.example.com",
        "https://painel.example.com:0", "https://painel.example.com:65536", "https://painel.example.com:", "https://a:b:c",
        "https://[::1", "https://painel.ex\xc3\xa1mple.com", ""}) {
    INFO(u);
    requireRejects(json{{"admin", {{"publicUrl", u}}}}, "admin.publicUrl");
  }
  requireRejects(json{{"admin", {{"publicUrl", "https://" + std::string(250, 'a') + ".com"}}}}, "admin.publicUrl");
  requireRejects(json{{"admin", {{"publicUrl", 42}}}}, "admin.publicUrl");
  requireRejects(json{{"admin", "https://painel.example.com"}}, "admin");
}

TEST_CASE("config admin origin matches the shared TS vectors") {
  std::ifstream f(CBMP_ADMIN_ORIGIN_VECTORS);
  REQUIRE(f);
  const auto v = json::parse(f);
  for (const auto& pair : v.at("accept")) {
    const auto in = pair.at(0).get<std::string>();
    INFO(in);
    const auto out = cbserver::normalizeAdminOrigin(in);
    REQUIRE(out.has_value());
    REQUIRE(*out == pair.at(1).get<std::string>());
  }
  for (const auto& in : v.at("reject")) {
    INFO(in.dump());
    REQUIRE_FALSE(cbserver::normalizeAdminOrigin(in.get<std::string>()).has_value());
  }
}

TEST_CASE("config admin port, bind and admins have defaults and are validated") {
  auto c = ServerConfig::fromJson(json::object());
  REQUIRE(c.admin.port == 30510);
  REQUIRE(c.admin.bind == "127.0.0.1");
  REQUIRE(c.admin.admins.empty());
  REQUIRE(!c.admin.devToken);
  REQUIRE(!c.admin.enabled());
  REQUIRE(!ServerConfig::load(CBSERVER_CFG_EXAMPLE).admin.enabled());  // exemplo: painel desligado

  auto a = ServerConfig::fromJson(json{{"admin",
                                        {{"port", 31000},
                                         {"bind", "0.0.0.0"},
                                         {"admins", {"123456789012345678", "12345678901234567890", "123456789012345678"}}}}});
  REQUIRE(a.admin.port == 31000);
  REQUIRE(a.admin.bind == "0.0.0.0");
  REQUIRE(a.admin.admins == std::vector<std::string>{"123456789012345678", "12345678901234567890"});  // sem repetidos
  REQUIRE(a.admin.enabled());
  REQUIRE(ServerConfig::fromJson(json{{"admin", {{"bind", "::1"}}}}).admin.bind == "::1");

  requireRejects(json{{"admin", {{"port", 0}}}}, "admin.port");
  requireRejects(json{{"admin", {{"port", 70000}}}}, "admin.port");
  requireRejects(json{{"port", 30510}, {"admin", {{"port", 30510}}}}, "admin.port");  // mesma porta TCP do jogo
  for (const char* b : {"localhost", "painel.example.com", "", "1.2.3", "300.1.1.1", "::g", "127.0.0.1:30510"}) {
    INFO(b);
    requireRejects(json{{"admin", {{"bind", b}}}}, "admin.bind");
  }
  for (const char* d : {"1234567890123456", "123456789012345678901", "12345678901234567a", " 123456789012345678", ""}) {
    INFO(d);
    requireRejects(json{{"admin", {{"admins", {d}}}}}, "admin.admins");
  }
  requireRejects(json{{"admin", {{"admins", "123456789012345678"}}}}, "admin.admins");
  requireRejects(json{{"admin", {{"admins", {123456789012345678LL}}}}}, "admin.admins");
  json many = json::array();
  for (int i = 0; i < 101; ++i) many.push_back("1234567890123456" + std::to_string(10 + i));
  requireRejects(json{{"admin", {{"admins", many}}}}, "admin.admins");
}

TEST_CASE("config admin devToken only in dev mode and long enough") {
  json dev = base();
  dev["auth"]["devPublicKey"] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  dev["admin"] = {{"devToken", std::string(32, 'a')}};
  auto c = ServerConfig::fromJson(dev);
  REQUIRE(c.admin.devToken == std::string(32, 'a'));
  REQUIRE(c.admin.enabled());

  dev["admin"]["devToken"] = std::string(31, 'a');
  requireRejects(dev, "admin.devToken");
  dev["admin"]["devToken"] = std::string(31, 'a') + "\n";
  requireRejects(dev, "admin.devToken");
  dev["admin"]["devToken"] = 5;
  requireRejects(dev, "admin.devToken");

  json hub = base();
  hub["hub"] = {{"url", "https://hub.example.com"}, {"serverId", "srv_abcdefghijkl"}, {"key", "sk_x"}};
  hub["admin"] = {{"devToken", std::string(40, 'a')}};
  requireRejects(hub, "admin.devToken");  // modo Hub: erro de config
  json none = base();
  none["admin"] = {{"devToken", std::string(40, 'a')}};
  requireRejects(none, "admin.devToken");  // sem devPublicKey
}

TEST_CASE("config admin trustedProxies accepts IPs and CIDRs only") {
  REQUIRE(ServerConfig::fromJson(json::object()).admin.trustedProxies.empty());
  auto c = ServerConfig::fromJson(json{{"admin", {{"trustedProxies", {"127.0.0.1", "10.0.0.0/8", "::1", "fd00::/8"}}}}});
  REQUIRE(c.admin.trustedProxies == std::vector<std::string>{"127.0.0.1", "10.0.0.0/8", "::1", "fd00::/8"});
  for (const char* p : {"localhost", "10.0.0.0/33", "::1/129", "10.0.0.0/", "/8", "1.2.3.4/8/1", "*", ""}) {
    INFO(p);
    requireRejects(json{{"admin", {{"trustedProxies", {p}}}}}, "admin.trustedProxies");
  }
  requireRejects(json{{"admin", {{"trustedProxies", "127.0.0.1"}}}}, "admin.trustedProxies");
  json many = json::array();
  for (int i = 0; i < 33; ++i) many.push_back("10.0.0." + std::to_string(i));
  requireRejects(json{{"admin", {{"trustedProxies", many}}}}, "admin.trustedProxies");
}
