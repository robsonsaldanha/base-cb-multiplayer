#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/ConnectFile.hpp>
#include <cbmp/messages.hpp>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>

using namespace cbmp::client;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kNow = 1'794'438'789;
constexpr std::string_view kLaunchId = "0123456789abcdef0123456789abcdef";

std::span<const uint8_t> Bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string Pointer(std::string_view runDirJson, int64_t writtenAt, std::string_view launchId = kLaunchId,
                    std::string_view v = "1") {
    return std::format(R"({{ "v": {}, "runDir": "{}", "launchId": "{}", "writtenAt": {}, "launcher": "0.2.0" }})", v,
                       runDirJson, launchId, writtenAt);
}

// base64url sem padding (só para montar tickets nos testes).
std::string B64Url(std::string_view in) {
    static constexpr char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += kAlpha[(n >> 18) & 63];
        out += kAlpha[(n >> 12) & 63];
        out += kAlpha[(n >> 6) & 63];
        out += kAlpha[n & 63];
    }
    if (in.size() - i == 1) {
        const uint32_t n = uint8_t(in[i]) << 16;
        out += kAlpha[(n >> 18) & 63];
        out += kAlpha[(n >> 12) & 63];
    } else if (in.size() - i == 2) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += kAlpha[(n >> 18) & 63];
        out += kAlpha[(n >> 12) & 63];
        out += kAlpha[(n >> 6) & 63];
    }
    return out;
}

// Ticket no formato do Hub: base64url(JSON compacto) "." base64url(assinatura).
std::string Ticket(int64_t exp) {
    const auto claims = std::format(
        R"({{"v":1,"kid":"k1","sub":"u1","did":"123","name":"Fulano","aud":"dev-local","iat":{},"exp":{},"jti":"j1"}})",
        exp - 300, exp);
    return B64Url(claims) + "." + B64Url(std::string(64, '\x5a'));
}

struct ConnectFields {
    std::string host = "127.0.0.1";
    std::string port = "27015";
    std::string ticket = Ticket(kNow + 300);
    std::string serverId = "dev-local";
    std::string protocol = "1";
    std::string launchIdLine = std::format(R"(,
  "launchId": "{}")", kLaunchId);
};

// Igual ao que o launcher grava (serde_json pretty, camelCase), com o launchId da Task 9.
std::string Connect(const ConnectFields& f = {}) {
    return std::format(R"({{
  "host": "{}",
  "port": {},
  "ticket": "{}",
  "serverId": "{}",
  "contentDir": "C:\\Users\\x\\AppData\\Local\\CBMultiplayer\\content\\dev-local",
  "protocol": {},
  "contentReadOnly": true{}
}})",
                       f.host, f.port, f.ticket, f.serverId, f.protocol, f.launchIdLine);
}

ConnectParse ParseC(const std::string& text, int64_t now = kNow) { return ParseConnect(Bytes(text), kLaunchId, now); }

fs::path FreshDir() {
    static std::atomic<int> n{0};
    std::random_device rd;
    const auto p = fs::temp_directory_path() / std::format("cbmp-client-test-{}-{}", rd(), n++);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void Touch(const fs::path& p) { std::ofstream(p, std::ios::binary) << "{}"; }
}  // namespace

// ---------------------------------------------------------------------------------------------------- ponteiro

TEST_CASE("client: ParsePointer aceita ponteiro valido") {
    std::string why;
    const auto p = ParsePointer(Bytes(Pointer(R"(C:\\Users\\x\\AppData\\Local\\CBMultiplayer-Teste\\run)", kNow - 5)),
                                kNow, &why);
    INFO(why);
    REQUIRE(p.has_value());
    CHECK(p->runDir == R"(C:\Users\x\AppData\Local\CBMultiplayer-Teste\run)");
    CHECK(p->launchId == kLaunchId);
    CHECK(p->writtenAt == kNow - 5);
    CHECK(p->launcher == "0.2.0");
}

TEST_CASE("client: ParsePointer ignora campos a mais e aceita runDir UTF-8") {
    const std::string text =
        R"({"v":1,"runDir":"D:\\Jogos\\Usuário\\run","launchId":"0123456789abcdef0123456789abcdef","writtenAt":)" +
        std::to_string(kNow) + R"(,"launcher":"0.2.0","extra":[1,2,3]})";
    const auto p = ParsePointer(Bytes(text), kNow);
    REQUIRE(p.has_value());
    CHECK(p->runDir == "D:\\Jogos\\Usu\xc3\xa1rio\\run");
}

TEST_CASE("client: ParsePointer recusa") {
    const std::string goodDir = R"(C:\\a\\run)";
    std::string why;
    SECTION("maior que 4 KiB") {
        std::string text = Pointer(goodDir, kNow);
        text.insert(1, R"("pad":")" + std::string(4096, 'x') + R"(",)");
        CHECK_FALSE(ParsePointer(Bytes(text), kNow, &why).has_value());
    }
    SECTION("nao e JSON") { CHECK_FALSE(ParsePointer(Bytes("{ v: 1"), kNow, &why).has_value()); }
    SECTION("nao e objeto") { CHECK_FALSE(ParsePointer(Bytes("[1,2]"), kNow, &why).has_value()); }
    SECTION("v diferente de 1") {
        CHECK_FALSE(ParsePointer(Bytes(Pointer(goodDir, kNow, kLaunchId, "2")), kNow, &why).has_value());
    }
    SECTION("v ausente") {
        const std::string text = R"({"runDir":"C:\\a\\run","launchId":"0123456789abcdef0123456789abcdef","writtenAt":)" +
                                 std::to_string(kNow) + "}";
        CHECK_FALSE(ParsePointer(Bytes(text), kNow, &why).has_value());
    }
    SECTION("runDir UNC") { CHECK_FALSE(ParsePointer(Bytes(Pointer(R"(\\\\srv\\x\\run)", kNow)), kNow, &why).has_value()); }
    SECTION("runDir com ..") { CHECK_FALSE(ParsePointer(Bytes(Pointer(R"(C:\\a\\..\\run)", kNow)), kNow, &why).has_value()); }
    SECTION("runDir terminando em rum") { CHECK_FALSE(ParsePointer(Bytes(Pointer(R"(C:\\a\\rum)", kNow)), kNow, &why).has_value()); }
    SECTION("runDir relativo") { CHECK_FALSE(ParsePointer(Bytes(Pointer(R"(a\\run)", kNow)), kNow, &why).has_value()); }
    SECTION("launchId invalido") {
        CHECK_FALSE(ParsePointer(Bytes(Pointer(goodDir, kNow, "0123456789ABCDEF0123456789ABCDEF")), kNow, &why).has_value());
    }
    SECTION("writtenAt nao inteiro") {
        const std::string text = R"({"v":1,"runDir":"C:\\a\\run","launchId":"0123456789abcdef0123456789abcdef","writtenAt":)" +
                                 std::to_string(kNow) + ".5}";
        CHECK_FALSE(ParsePointer(Bytes(text), kNow, &why).has_value());
    }
    SECTION("writtenAt 601 s atras") { CHECK_FALSE(ParsePointer(Bytes(Pointer(goodDir, kNow - 601)), kNow, &why).has_value()); }
    SECTION("writtenAt 121 s a frente") { CHECK_FALSE(ParsePointer(Bytes(Pointer(goodDir, kNow + 121)), kNow, &why).has_value()); }
    CHECK_FALSE(why.empty());
}

TEST_CASE("client: ParsePointer aceita exatamente 600 s atras e 120 s a frente") {
    const std::string goodDir = R"(C:\\a\\run)";
    CHECK(ParsePointer(Bytes(Pointer(goodDir, kNow - 600)), kNow).has_value());
    CHECK(ParsePointer(Bytes(Pointer(goodDir, kNow + 120)), kNow).has_value());
}

TEST_CASE("client: IsValidRunDir") {
    CHECK(IsValidRunDir(R"(C:\Users\x\AppData\Local\CBMultiplayer\run)"));
    CHECK(IsValidRunDir(R"(d:\run)"));
    CHECK_FALSE(IsValidRunDir(R"(\\srv\x\run)"));
    CHECK_FALSE(IsValidRunDir(R"(\\?\C:\a\run)"));
    CHECK_FALSE(IsValidRunDir(R"(C:\a\..\run)"));
    CHECK_FALSE(IsValidRunDir(R"(C:\a\.\run)"));
    CHECK_FALSE(IsValidRunDir(R"(C:\a/..\run)"));
    CHECK_FALSE(IsValidRunDir(R"(C:\a\rum)"));
    CHECK_FALSE(IsValidRunDir(R"(C:\a\run\)"));
    CHECK_FALSE(IsValidRunDir(R"(a\run)"));
    CHECK_FALSE(IsValidRunDir(R"(C:run)"));
    CHECK_FALSE(IsValidRunDir(R"(\run)"));
    CHECK_FALSE(IsValidRunDir(R"(1:\run)"));
    CHECK_FALSE(IsValidRunDir(R"(C:\a\run:x)"));
    CHECK_FALSE(IsValidRunDir(""));
    CHECK_FALSE(IsValidRunDir(std::string_view("C:\\a\0b\\run", 10)));
}

TEST_CASE("client: IsLaunchId") {
    CHECK(IsLaunchId("0123456789abcdef0123456789abcdef"));
    CHECK_FALSE(IsLaunchId("0123456789abcdef0123456789abcde"));
    CHECK_FALSE(IsLaunchId("0123456789abcdef0123456789abcdef0"));
    CHECK_FALSE(IsLaunchId("0123456789ABCDEF0123456789abcdef"));
    CHECK_FALSE(IsLaunchId("0123456789abcdeg0123456789abcdef"));
    CHECK_FALSE(IsLaunchId(""));
}

TEST_CASE("client: FindPointerFile sobe ate 5 pastas") {
    const auto root = FreshDir();
    const auto win64 = root / "Win64";
    const auto cbmp = win64 / "cbmp";
    const auto dlls = cbmp / "atual" / "ue4ss" / "Mods" / "cb-mp" / "dlls";
    fs::create_directories(dlls);

    SECTION("acha em cbmp (5 acima de dlls)") {
        Touch(cbmp / "cbmp-run.json");
        const auto found = FindPointerFile(dlls);
        REQUIRE(found.has_value());
        CHECK(fs::equivalent(*found, cbmp / "cbmp-run.json"));
    }
    SECTION("6 niveis acima nao acha") {
        Touch(win64 / "cbmp-run.json");
        CHECK_FALSE(FindPointerFile(dlls).has_value());
    }
    SECTION("na propria pasta") {
        Touch(dlls / "cbmp-run.json");
        const auto found = FindPointerFile(dlls);
        REQUIRE(found.has_value());
        CHECK(fs::equivalent(*found, dlls / "cbmp-run.json"));
    }
    SECTION("sem arquivo") { CHECK_FALSE(FindPointerFile(dlls).has_value()); }
    SECTION("pasta com o nome do arquivo nao conta") {
        fs::create_directories(cbmp / "cbmp-run.json");
        CHECK_FALSE(FindPointerFile(dlls).has_value());
    }
    fs::remove_all(root);
}

TEST_CASE("client: ConnectPathFor") {
    RunPointer p;
    p.runDir = R"(C:\Users\x\AppData\Local\CBMultiplayer\run)";
    CHECK(ConnectPathFor(p) == R"(C:\Users\x\AppData\Local\CBMultiplayer\run\connect.json)");
}

// ---------------------------------------------------------------------------------------------------- connect.json

TEST_CASE("client: ParseConnect aceita o arquivo do launcher") {
    const auto r = ParseC(Connect());
    INFO(r.why);
    REQUIRE(r.error == ConnectError::None);
    REQUIRE(r.info.has_value());
    CHECK(r.info->host == "127.0.0.1");
    CHECK(r.info->port == 27015);
    CHECK(r.info->ticket == Ticket(kNow + 300));
    CHECK(r.info->serverId == "dev-local");
    CHECK(r.info->protocol == cbmp::kProtocolVersion);
    CHECK(r.info->launchId == kLaunchId);
}

TEST_CASE("client: ParseConnect aceita IPv6") {
    ConnectFields f;
    f.host = "::1";
    CHECK(ParseC(Connect(f)).error == ConnectError::None);
    f.host = "[2001:db8::1]";
    CHECK(ParseC(Connect(f)).error == ConnectError::None);
}

TEST_CASE("client: ParseConnect recusa") {
    ConnectFields f;
    ConnectError expected = ConnectError::Invalid;
    std::string text;
    SECTION("maior que 16 KiB") {
        text = Connect(f);
        text.insert(1, R"("pad":")" + std::string(16 * 1024, 'x') + R"(",)");
    }
    SECTION("JSON invalido") { text = Connect(f).substr(0, 40); }
    SECTION("nao e objeto") { text = "[]"; }
    SECTION("host nome") {
        f.host = "exemplo.com";
        text = Connect(f);
    }
    SECTION("host 1.2.3") {
        f.host = "1.2.3";
        text = Connect(f);
    }
    SECTION("porta 0") {
        f.port = "0";
        text = Connect(f);
    }
    SECTION("porta 65536") {
        f.port = "65536";
        text = Connect(f);
    }
    SECTION("porta texto") {
        f.port = R"("27015")";
        text = Connect(f);
    }
    SECTION("ticket vazio") {
        f.ticket = "";
        text = Connect(f);
    }
    SECTION("ticket maior que 2048") {
        f.ticket = std::string(2049, 'a');
        text = Connect(f);
    }
    SECTION("serverId vazio") {
        f.serverId = "";
        text = Connect(f);
    }
    SECTION("serverId maior que 64") {
        f.serverId = std::string(65, 's');
        text = Connect(f);
    }
    SECTION("launchId ausente") {
        f.launchIdLine = "";
        text = Connect(f);
    }
    SECTION("launchId diferente") {
        f.launchIdLine = R"(,
  "launchId": "ffffffffffffffffffffffffffffffff")";
        text = Connect(f);
    }
    SECTION("protocolo 2") {
        f.protocol = "2";
        text = Connect(f);
        expected = ConnectError::ProtocolMismatch;
    }
    SECTION("ticket com exp em 9 s") {
        f.ticket = Ticket(kNow + 9);
        text = Connect(f);
        expected = ConnectError::TicketExpired;
    }
    SECTION("ticket ja vencido") {
        f.ticket = Ticket(kNow - 100);
        text = Connect(f);
        expected = ConnectError::TicketExpired;
    }
    const auto r = ParseC(text);
    CHECK(r.error == expected);
    CHECK_FALSE(r.info.has_value());
    CHECK_FALSE(r.why.empty());
}

TEST_CASE("client: ParseConnect aceita exp em 10 s e ticket ilegivel") {
    ConnectFields f;
    f.ticket = Ticket(kNow + 10);
    CHECK(ParseC(Connect(f)).error == ConnectError::None);
    f.ticket = "nao-e-um-ticket";
    CHECK(ParseC(Connect(f)).error == ConnectError::None);
    f.ticket = "!!!.???";
    CHECK(ParseC(Connect(f)).error == ConnectError::None);
}

TEST_CASE("client: ParseConnect nunca le contentDir") {
    // contentDir com tipo qualquer (ou ausente) não muda nada.
    std::string text = Connect();
    const auto pos = text.find(R"("contentDir")");
    REQUIRE(pos != std::string::npos);
    const auto end = text.find('\n', pos);
    text.replace(pos, end - pos, R"("contentDir": 12345,)");
    CHECK(ParseC(text).error == ConnectError::None);
}

TEST_CASE("client: TicketExp") {
    CHECK(TicketExp(Ticket(kNow + 300)) == kNow + 300);
    CHECK_FALSE(TicketExp("").has_value());
    CHECK_FALSE(TicketExp("semponto").has_value());
    CHECK_FALSE(TicketExp("!!!.abc").has_value());
    CHECK_FALSE(TicketExp(B64Url("[1,2]") + ".x").has_value());
    CHECK_FALSE(TicketExp(B64Url(R"({"exp":"1"})") + ".x").has_value());
    CHECK_FALSE(TicketExp(B64Url(R"({"exp":1.5})") + ".x").has_value());
    CHECK(TicketExp(B64Url(R"({"exp":42})") + ".x") == 42);
}

TEST_CASE("client: IsIpLiteral") {
    CHECK(IsIpLiteral("127.0.0.1"));
    CHECK(IsIpLiteral("0.0.0.0"));
    CHECK(IsIpLiteral("255.255.255.255"));
    CHECK(IsIpLiteral("::1"));
    CHECK(IsIpLiteral("::"));
    CHECK(IsIpLiteral("[2001:db8::1]"));
    CHECK(IsIpLiteral("2001:0db8:0000:0000:0000:ff00:0042:8329"));
    CHECK(IsIpLiteral("::ffff:192.0.2.1"));
    CHECK_FALSE(IsIpLiteral("256.1.1.1"));
    CHECK_FALSE(IsIpLiteral("a.b"));
    CHECK_FALSE(IsIpLiteral("1.2.3"));
    CHECK_FALSE(IsIpLiteral("1.2.3.4.5"));
    CHECK_FALSE(IsIpLiteral("01.2.3.4"));
    CHECK_FALSE(IsIpLiteral("1.2.3.4 "));
    CHECK_FALSE(IsIpLiteral("exemplo.com"));
    CHECK_FALSE(IsIpLiteral("localhost"));
    CHECK_FALSE(IsIpLiteral(""));
    CHECK_FALSE(IsIpLiteral("[]"));
    CHECK_FALSE(IsIpLiteral("[127.0.0.1]"));
    CHECK_FALSE(IsIpLiteral("[::1"));
    CHECK_FALSE(IsIpLiteral("1::2::3"));
    CHECK_FALSE(IsIpLiteral("1:2:3:4:5:6:7:8:9"));
    CHECK_FALSE(IsIpLiteral("1:2:3:4:5:6:7"));
    CHECK_FALSE(IsIpLiteral("12345::1"));
    CHECK_FALSE(IsIpLiteral("fe80::1%eth0"));
}

TEST_CASE("client: SecureZero deixa a string vazia") {
    std::string s = "segredo-muito-longo-que-nao-cabe-no-sso-0123456789";
    SecureZero(s);
    CHECK(s.empty());
    std::string empty;
    SecureZero(empty);
    CHECK(empty.empty());
}
