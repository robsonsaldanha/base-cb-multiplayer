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
