// WinConnectFs contra o sistema de arquivos real (pasta temporária).
#include <catch2/catch_test_macros.hpp>
#include <WinConnectFs.hpp>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace cbmp::client;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> n{0};
        path = fs::temp_directory_path() / std::format("cbmp-wincfs-{}-{}", GetCurrentProcessId(), n++);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void WriteFile(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

std::vector<uint8_t> AsBytes(const std::string& s) { return {s.begin(), s.end()}; }
}  // namespace

TEST_CASE("client: WinConnectFs resgata arquivo comum e apaga") {
    TempDir dir;
    const auto file = dir.path / "connect.json";
    const std::string content = R"({"host":"127.0.0.1","port":27015})";
    WriteFile(file, content);

    WinConnectFs cfs(file);
    CHECK(cfs.Exists());
    const auto r = cfs.ClaimAndDelete();
    CHECK(r.status == ClaimRead::Ok);
    CHECK(r.bytes == AsBytes(content));
    CHECK_FALSE(fs::exists(file));
    CHECK_FALSE(cfs.Exists());
}

TEST_CASE("client: WinConnectFs arquivo vazio e Ok sem bytes") {
    TempDir dir;
    const auto file = dir.path / "connect.json";
    WriteFile(file, "");
    WinConnectFs cfs(file);
    const auto r = cfs.ClaimAndDelete();
    CHECK(r.status == ClaimRead::Ok);
    CHECK(r.bytes.empty());
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("client: WinConnectFs ausente e Missing") {
    TempDir dir;
    WinConnectFs cfs(dir.path / "connect.json");
    CHECK_FALSE(cfs.Exists());
    CHECK(cfs.ClaimAndDelete().status == ClaimRead::Missing);
    WinConnectFs noDir(dir.path / "nao-existe" / "connect.json");
    CHECK(noDir.ClaimAndDelete().status == ClaimRead::Missing);
}

TEST_CASE("client: WinConnectFs aberto por outro handle e Busy e fica") {
    TempDir dir;
    const auto file = dir.path / "connect.json";
    WriteFile(file, "{}");
    const HANDLE other = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(other != INVALID_HANDLE_VALUE);
    WinConnectFs cfs(file);
    const auto r = cfs.ClaimAndDelete();
    CloseHandle(other);
    CHECK(r.status == ClaimRead::Busy);
    CHECK(r.bytes.empty());
    CHECK(fs::exists(file));
    // Livre de novo: resgata.
    CHECK(cfs.ClaimAndDelete().status == ClaimRead::Ok);
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("client: WinConnectFs maior que 16 KiB e Rejected e apagado") {
    TempDir dir;
    const auto file = dir.path / "connect.json";
    WriteFile(file, std::string(17 * 1024, 'x'));
    WinConnectFs cfs(file);
    const auto r = cfs.ClaimAndDelete();
    CHECK(r.status == ClaimRead::Rejected);
    CHECK(r.bytes.empty());
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("client: WinConnectFs exatamente 16 KiB e Ok") {
    TempDir dir;
    const auto file = dir.path / "connect.json";
    WriteFile(file, std::string(16 * 1024, 'y'));
    WinConnectFs cfs(file);
    const auto r = cfs.ClaimAndDelete();
    CHECK(r.status == ClaimRead::Ok);
    CHECK(r.bytes.size() == 16 * 1024);
}

TEST_CASE("client: WinConnectFs pasta no lugar do arquivo nao e apagada") {
    TempDir dir;
    const auto folder = dir.path / "connect.json";
    fs::create_directories(folder);
    WriteFile(folder / "dentro.txt", "x");
    WinConnectFs cfs(folder);
    CHECK_FALSE(cfs.Exists());
    const auto r = cfs.ClaimAndDelete();
    CHECK((r.status == ClaimRead::Rejected || r.status == ClaimRead::Missing));
    CHECK(r.bytes.empty());
    CHECK(fs::is_directory(folder));
    CHECK(fs::exists(folder / "dentro.txt"));
}
