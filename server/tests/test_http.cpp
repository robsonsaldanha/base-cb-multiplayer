#include <httplib.h>
#include <sodium.h>

#include <catch2/catch_test_macros.hpp>
#include <fstream>

#include "cbserver/http.hpp"

static const int kSodiumInit = sodium_init();
namespace fs = std::filesystem;
using namespace cbserver;

TEST_CASE("http serves info manifest files and metrics") {
  fs::path dir = fs::temp_directory_path() / ("cbhttp_" + std::to_string(randombytes_random()));
  fs::create_directories(dir / "r/client");
  std::ofstream(dir / "r/resource.json") << R"({"name":"r","version":"1"})";
  std::ofstream(dir / "r/client/main.lua", std::ios::binary) << "hello";

  auto store = ResourceStore::scan(dir);
  auto cfg = ServerConfig::fromJson(nlohmann::json::object());
  Metrics m;
  HttpServer srv(cfg, store, m, [] {
    return nlohmann::json{{"serverId", "srv_t"}, {"name", "T"},         {"players", 0},
                          {"maxPlayers", 512},   {"protocol", 1},       {"version", "0.1.0"}};
  });
  srv.start("127.0.0.1", 0);
  REQUIRE(srv.boundPort() > 0);
  httplib::Client cli("127.0.0.1", srv.boundPort());

  auto info = cli.Get("/info");
  REQUIRE(info);
  REQUIRE(info->status == 200);
  REQUIRE(info->get_header_value("Content-Type").find("application/json") == 0);
  REQUIRE(nlohmann::json::parse(info->body)["serverId"] == "srv_t");

  auto man = cli.Get("/cb/manifest");
  REQUIRE(man);
  REQUIRE(man->status == 200);
  auto mj = nlohmann::json::parse(man->body);
  std::string sha = mj["resources"][0]["files"][0]["sha256"];

  auto f = cli.Get("/cb/files/" + sha);
  REQUIRE(f);
  REQUIRE(f->status == 200);
  REQUIRE(f->body == "hello");
  REQUIRE(f->get_header_value("Cache-Control") == "public, max-age=31536000, immutable");

  auto bad = cli.Get("/cb/files/zzz");
  REQUIRE(bad);
  REQUIRE(bad->status == 404);
  auto none = cli.Get("/cb/files/" + std::string(64, '0'));
  REQUIRE(none);
  REQUIRE(none->status == 404);

  auto met = cli.Get("/metrics");
  REQUIRE(met);
  REQUIRE(met->status == 200);
  REQUIRE(nlohmann::json::parse(met->body).contains("tickP99Ms"));

  auto nada = cli.Get("/nada");
  REQUIRE(nada);
  REQUIRE(nada->status == 404);
  REQUIRE(nlohmann::json::parse(nada->body)["error"] == "not_found");

  srv.stop();
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("http serves large file in chunks with identical bytes") {
  fs::path dir = fs::temp_directory_path() / ("cbhttpbig_" + std::to_string(randombytes_random()));
  fs::create_directories(dir / "r/files");
  std::ofstream(dir / "r/resource.json") << R"({"name":"r","version":"1"})";
  std::string big(3 * 1024 * 1024 + 123, '\0');
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 31 + (i >> 8)) & 0xff);
  { std::ofstream o(dir / "r/files/big.png", std::ios::binary); o.write(big.data(), static_cast<std::streamsize>(big.size())); }
  auto store = ResourceStore::scan(dir);
  auto cfg = ServerConfig::fromJson(nlohmann::json::object());
  Metrics m;
  HttpServer srv(cfg, store, m, [] { return nlohmann::json{{"serverId", "srv_t"}}; });
  srv.start("127.0.0.1", 0);
  httplib::Client cli("127.0.0.1", srv.boundPort());
  std::string sha = store.manifestJson("srv_t", 1)["resources"][0]["files"][0]["sha256"];
  auto r = cli.Get("/cb/files/" + sha);
  REQUIRE(r);
  REQUIRE(r->status == 200);
  REQUIRE(r->body.size() == big.size());
  REQUIRE(r->body == big);
  // hex maiusculo tambem acha o arquivo
  std::string up = sha;
  for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  auto r2 = cli.Get("/cb/files/" + up);
  REQUIRE(r2);
  REQUIRE(r2->status == 200);
  srv.stop();
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("metricsAllowed rules") {
  using std::nullopt;
  const std::optional<std::string> tok = "s3cret";
  REQUIRE(metricsAllowed("127.0.0.1", nullopt, nullopt));
  REQUIRE(metricsAllowed("::1", nullopt, nullopt));
  REQUIRE(metricsAllowed("::ffff:127.0.0.1", nullopt, nullopt));
  REQUIRE(!metricsAllowed("10.0.0.1", nullopt, nullopt));
  REQUIRE(metricsAllowed("10.0.0.1", std::string("Bearer s3cret"), tok));
  REQUIRE(!metricsAllowed("10.0.0.1", std::string("Bearer wrong"), tok));
  REQUIRE(!metricsAllowed("10.0.0.1", std::string("Bearer s3cre"), tok));
  REQUIRE(!metricsAllowed("10.0.0.1", nullopt, tok));
  REQUIRE(!metricsAllowed("10.0.0.1", std::string("Bearer s3cret"), nullopt));
}

TEST_CASE("http startPublic answers /info on 127.0.0.1 and ::1") {
  fs::path dir = fs::temp_directory_path() / ("cbhttp6_" + std::to_string(randombytes_random()));
  fs::create_directories(dir);
  auto store = ResourceStore::scan(dir);
  auto cfg = ServerConfig::fromJson(nlohmann::json::object());
  Metrics m;
  HttpServer srv(cfg, store, m, [] { return nlohmann::json{{"serverId", "srv_t"}}; });
  const std::string mode = srv.startPublic(0);
  REQUIRE(srv.boundPort() > 0);
  httplib::Client v4("127.0.0.1", srv.boundPort());
  auto r4 = v4.Get("/info");
  REQUIRE(r4);
  REQUIRE(r4->status == 200);
  if (mode == "ipv4") {
    srv.stop();
    SKIP("sem IPv6 loopback neste sistema (bind dual-stack indisponivel)");
  }
  REQUIRE(mode == "dual-stack");
  httplib::Client v6("::1", srv.boundPort());
  auto r6 = v6.Get("/info");
  REQUIRE(r6);
  REQUIRE(r6->status == 200);
  REQUIRE(nlohmann::json::parse(r6->body)["serverId"] == "srv_t");
  srv.stop();
  std::error_code ec;
  fs::remove_all(dir, ec);
}
