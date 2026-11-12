// TryArm (inércia sem o launcher) e ReadSmallFile contra o sistema de arquivos real (pasta temporária com o layout do
// jogo: Win64\cbmp\cbmp-run.json e o main.dll em Win64\cbmp\atual\ue4ss\Mods\cb-mp\dlls\).
#include <catch2/catch_test_macros.hpp>
#include <Arming.hpp>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace cbmp::client;
namespace fs = std::filesystem;

namespace {
constexpr const char* kLaunchId = "0123456789abcdef0123456789abcdef";

std::string ToUtf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// Barra invertida e aspas escapadas para JSON.
std::string JsonEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '\\' || c == '"') r.push_back('\\');
        r.push_back(c);
    }
    return r;
}

void WriteFile(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct GameLayout {
    fs::path root, win64, container, dllDir, runDir;
    GameLayout() {
        static std::atomic<int> n{0};
        root = fs::temp_directory_path() / std::format("cbmp-arming-{}-{}", GetCurrentProcessId(), n++);
        fs::remove_all(root);
        win64 = root / "Phoenix" / "Binaries" / "Win64";
        container = win64 / "cbmp";
        dllDir = container / "atual" / "ue4ss" / "Mods" / "cb-mp" / "dlls";
        runDir = root / "dados" / "run";
        fs::create_directories(dllDir);
        fs::create_directories(runDir);
    }
    ~GameLayout() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string Pointer(int64_t writtenAt, const std::string& extra = "") const {
        return std::format(R"({{"v":1,"runDir":"{}","launchId":"{}","writtenAt":{},"launcher":"0.2.0"{}}})",
                           JsonEscape(ToUtf8(runDir)), kLaunchId, writtenAt, extra);
    }
    void WritePointer(const std::string& json) const { WriteFile(container / "cbmp-run.json", json); }
    void WriteConnect() const { WriteFile(runDir / "connect.json", R"({"host":"127.0.0.1"})"); }

    std::set<std::string> Snapshot() const {
        std::set<std::string> all;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            all.insert(ToUtf8(e.path()) + "|" + std::to_string(e.is_regular_file() ? fs::file_size(e.path()) : 0));
        }
        return all;
    }
};
}  // namespace

TEST_CASE("client: TryArm arma com ponteiro recente e connect.json presente") {
    GameLayout g;
    g.WritePointer(g.Pointer(Now()));
    g.WriteConnect();
    const auto before = g.Snapshot();

    std::string why;
    const auto armed = TryArm(g.dllDir, Now(), &why);
    INFO(why);
    REQUIRE(armed.has_value());
    CHECK(fs::equivalent(armed->container, g.container));
    CHECK(fs::equivalent(armed->connectJson, g.runDir / "connect.json"));
    CHECK(armed->pointer.launchId == kLaunchId);
    CHECK(armed->pointer.launcher == "0.2.0");
    CHECK(g.Snapshot() == before);  // só leitura: nada criado, nada apagado
}

TEST_CASE("client: TryArm fica inerte sem ponteiro") {
    GameLayout g;
    g.WriteConnect();
    const auto before = g.Snapshot();
    std::string why;
    CHECK_FALSE(TryArm(g.dllDir, Now(), &why).has_value());
    CHECK(why == "sem cbmp-run.json");
    CHECK(g.Snapshot() == before);
}

TEST_CASE("client: TryArm fica inerte sem connect.json ou com connect.json pasta") {
    GameLayout g;
    g.WritePointer(g.Pointer(Now()));
    std::string why;
    CHECK_FALSE(TryArm(g.dllDir, Now(), &why).has_value());
    CHECK(why == "connect.json ausente");

    fs::create_directory(g.runDir / "connect.json");
    CHECK_FALSE(TryArm(g.dllDir, Now(), &why).has_value());
    CHECK(why == "connect.json ausente");
}

TEST_CASE("client: TryArm fica inerte com ponteiro velho, do futuro ou invalido") {
    GameLayout g;
    g.WriteConnect();
    const int64_t now = Now();
    std::string why;

    g.WritePointer(g.Pointer(now - 601));
    CHECK_FALSE(TryArm(g.dllDir, now, &why).has_value());
    CHECK(why.starts_with("cbmp-run.json inválido"));

    g.WritePointer(g.Pointer(now + 121));
    CHECK_FALSE(TryArm(g.dllDir, now, &why).has_value());

    g.WritePointer(g.Pointer(now - 600));  // limite exato ainda vale
    CHECK(TryArm(g.dllDir, now, &why).has_value());

    g.WritePointer("{nao e json");
    CHECK_FALSE(TryArm(g.dllDir, now, &why).has_value());
}

TEST_CASE("client: TryArm recusa ponteiro maior que 4 KiB") {
    GameLayout g;
    g.WriteConnect();
    const int64_t now = Now();
    const std::string base = g.Pointer(now);
    std::string why;

    // JSON válido com espaços até exatamente 4096 bytes: arma. Um byte a mais: inerte.
    std::string exact = base;
    exact.insert(exact.size() - 1, std::string(4096 - base.size(), ' '));
    REQUIRE(exact.size() == 4096);
    g.WritePointer(exact);
    CHECK(TryArm(g.dllDir, now, &why).has_value());

    std::string over = base;
    over.insert(over.size() - 1, std::string(4097 - base.size(), ' '));
    g.WritePointer(over);
    CHECK_FALSE(TryArm(g.dllDir, now, &why).has_value());
    CHECK(why == "cbmp-run.json ilegível ou grande demais");
}

TEST_CASE("client: TryArm acha o ponteiro no limite de 5 pastas, nao em 6") {
    GameLayout g;
    g.WritePointer(g.Pointer(Now()));
    g.WriteConnect();
    // dlls → cb-mp → Mods → ue4ss → atual → cbmp: o ponteiro está 5 pastas acima de dlls.
    CHECK(TryArm(g.dllDir, Now()).has_value());
    const auto deeper = g.dllDir / "sub";
    fs::create_directory(deeper);
    CHECK_FALSE(TryArm(deeper, Now()).has_value());
    CHECK_FALSE(TryArm(fs::path{}, Now()).has_value());
}

TEST_CASE("client: ReadSmallFile le ate o limite e recusa o resto") {
    GameLayout g;
    const auto file = g.root / "f.bin";
    WriteFile(file, std::string(10, 'x'));
    const auto ten = ReadSmallFile(file, 10);
    REQUIRE(ten.has_value());
    CHECK(ten->size() == 10);
    CHECK_FALSE(ReadSmallFile(file, 9).has_value());
    CHECK_FALSE(ReadSmallFile(g.root / "nao-existe.bin", 10).has_value());
    CHECK_FALSE(ReadSmallFile(g.root, 10).has_value());  // pasta

    WriteFile(file, "");
    const auto empty = ReadSmallFile(file, 10);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}
