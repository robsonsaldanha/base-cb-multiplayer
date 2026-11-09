#include <sodium.h>

#include <catch2/catch_test_macros.hpp>
#include <fstream>

#include "cbserver/manifest.hpp"

namespace fs = std::filesystem;
using cbserver::ResourceStore;

static void put(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}
static std::string sha(const std::string& s) {
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(s.data()), s.size());
  char hex[crypto_hash_sha256_BYTES * 2 + 1];
  sodium_bin2hex(hex, sizeof hex, h, sizeof h);
  return hex;
}

struct TmpDir {
  fs::path p = fs::temp_directory_path() / ("cbsrv_" + std::to_string(randombytes_random()));
  TmpDir() { fs::create_directories(p); }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

static void makeRes(const fs::path& root) {
  put(root / "res1/resource.json", R"({"name":"res1","version":"1.2.3"})");
  put(root / "res1/client/main.lua", "print('c')");
  put(root / "res1/ui/index.html", "<html></html>");
  put(root / "res1/server/main.lua", "print('s')");
  put(root / "res1/files/a.png", "PNGDATA");
}

TEST_CASE("manifest rejects native files anywhere") {
  TmpDir t;
  makeRes(t.p);
  put(t.p / "res1/client/x.exe", "MZ");
  REQUIRE_THROWS_AS(ResourceStore::scan(t.p), std::runtime_error);
}

TEST_CASE("manifest rejects native file inside server dir") {
  TmpDir t;
  makeRes(t.p);
  put(t.p / "res1/server/evil.dll", "MZ");
  REQUIRE_THROWS_AS(ResourceStore::scan(t.p), std::runtime_error);
}

TEST_CASE("manifest rejects invalid resource json") {
  TmpDir t;
  makeRes(t.p);
  put(t.p / "res1/resource.json", "{ nao json");
  REQUIRE_THROWS_AS(ResourceStore::scan(t.p), std::runtime_error);
  put(t.p / "res1/resource.json", R"({"name":"res1"})");
  REQUIRE_THROWS_AS(ResourceStore::scan(t.p), std::runtime_error);
}

TEST_CASE("manifest lists client ui files with correct hashes") {
  TmpDir t;
  makeRes(t.p);
  auto store = ResourceStore::scan(t.p);
  auto j = store.manifestJson("srv_abc", 1);
  REQUIRE(j["serverId"] == "srv_abc");
  REQUIRE(j["protocol"] == 1);
  REQUIRE(j["resources"].size() == 1);
  auto& r = j["resources"][0];
  REQUIRE(r["name"] == "res1");
  REQUIRE(r["version"] == "1.2.3");
  REQUIRE(r["files"].size() == 3);
  uint64_t total = 0;
  bool sawLua = false;
  for (auto& f : r["files"]) {
    std::string path = f["path"];
    REQUIRE(path.rfind("server/", 0) != 0);
    total += f["size"].get<uint64_t>();
    if (path == "client/main.lua") {
      sawLua = true;
      REQUIRE(f["sha256"] == sha("print('c')"));
      REQUIRE(f["size"] == 10);
    }
    auto found = store.fileBySha(f["sha256"]);
    REQUIRE(found.has_value());
    REQUIRE(fs::exists(*found));
  }
  REQUIRE(sawLua);
  REQUIRE(j["totalSize"] == total);
  REQUIRE(!store.fileBySha(std::string(64, '0')).has_value());
}

TEST_CASE("manifest ignores non whitelisted extension in files dir") {
  TmpDir t;
  makeRes(t.p);
  put(t.p / "res1/files/notes.txt", "x");
  put(t.p / "res1/client/app.js", "x");  // .js so em ui/
  auto j = ResourceStore::scan(t.p).manifestJson("srv_a", 1);
  REQUIRE(j["resources"][0]["files"].size() == 3);
}

TEST_CASE("manifest of missing dir is empty") {
  auto j = ResourceStore::scan(fs::temp_directory_path() / "cbsrv_nao_existe_xyz").manifestJson("srv_a", 1);
  REQUIRE(j["resources"].empty());
  REQUIRE(j["totalSize"] == 0);
}
