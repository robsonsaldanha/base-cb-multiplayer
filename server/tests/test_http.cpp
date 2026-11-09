#include <httplib.h>
#include <sodium.h>

#include <catch2/catch_test_macros.hpp>
#include <fstream>

#include "cbserver/http.hpp"

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
