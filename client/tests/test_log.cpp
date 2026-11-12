#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/Log.hpp>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace cbmp::client;
namespace fs = std::filesystem;

namespace {
std::tm SampleTime() {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;
    t.tm_mday = 9;
    t.tm_hour = 14;
    t.tm_min = 3;
    t.tm_sec = 7;
    return t;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("client: FormatLine INFO") {
    REQUIRE(FormatLine(Level::Info, "ola", SampleTime(), 5) == "[2026-11-09 14:03:07.005] INFO  ola");
}

TEST_CASE("client: FormatLine ERRO") {
    REQUIRE(FormatLine(Level::Error, "deu ruim", SampleTime(), 123) == "[2026-11-09 14:03:07.123] ERRO  deu ruim");
}

TEST_CASE("client: FileLogger escreve linhas") {
    const auto path = fs::temp_directory_path() / "cbmp_client_test_log.txt";
    fs::remove(path);
    {
        FileLogger log;
        REQUIRE(log.Open(path));
        log.Write(Level::Warn, "linha um");
        log.Write(Level::Info, "linha dois");
    }
    const std::string content = ReadAll(path);
    CHECK(content.find("AVISO linha um\n") != std::string::npos);
    CHECK(content.find("INFO  linha dois\n") != std::string::npos);
    fs::remove(path);
}

TEST_CASE("client: LogFileName usa data, hora e pid") {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;
    t.tm_mday = 11;
    t.tm_hour = 20;
    t.tm_min = 14;
    t.tm_sec = 50;
    REQUIRE(LogFileName(t, 1234) == "cb-mp-20261111-201450-1234.log");
}

TEST_CASE("client: PruneLogs mantem os mais novos pelo nome e ignora outros arquivos") {
    const auto dir = fs::temp_directory_path() / "cbmp_client_prune_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<std::string> names;
    for (int i = 0; i < 12; ++i) names.push_back(std::format("cb-mp-20261111-2014{:02}-1234.log", i));
    // Criados fora de ordem: a ordem vem do nome, não da data de modificação.
    for (int i = 11; i >= 0; --i) std::ofstream(dir / names[static_cast<size_t>(i)]) << "x";
    std::ofstream(dir / "outro.txt") << "x";

    PruneLogs(dir, 10);

    std::set<std::string> left;
    for (const auto& e : fs::directory_iterator(dir)) left.insert(e.path().filename().string());
    std::set<std::string> expected(names.begin() + 2, names.end());
    expected.insert("outro.txt");
    CHECK(left == expected);
    fs::remove_all(dir);
}

TEST_CASE("client: FileLogger omite linhas com o segredo") {
    const auto path = fs::temp_directory_path() / "cbmp_client_test_secret.txt";
    fs::remove(path);
    const std::string secret = "0123456789abcdefTICKET";
    const std::string line = "connect ticket=" + secret + " fim";
    {
        FileLogger log;
        REQUIRE(log.Open(path));
        REQUIRE(log.IsOpen());
        log.SetSecret(secret);
        log.Write(Level::Info, line);
        CHECK(ReadAll(path).find(secret) == std::string::npos);  // flush por linha: o arquivo já está gravado
        log.ClearSecret();
        log.Write(Level::Info, "depois " + line);
    }
    const std::string content = ReadAll(path);
    const auto firstEnd = content.find('\n');
    REQUIRE(firstEnd != std::string::npos);
    const std::string firstLine = content.substr(0, firstEnd);
    CHECK(firstLine.ends_with("INFO  linha omitida (continha segredo)"));
    CHECK(firstLine.find(secret) == std::string::npos);
    // Depois de ClearSecret a mesma linha é gravada.
    CHECK(content.substr(firstEnd + 1).ends_with("INFO  depois " + line + "\n"));
    fs::remove(path);
}

TEST_CASE("client: FileLogger ignora segredo curto") {
    const auto path = fs::temp_directory_path() / "cbmp_client_test_short_secret.txt";
    fs::remove(path);
    {
        FileLogger log;
        REQUIRE(log.Open(path));
        log.SetSecret("12345678");
        log.Write(Level::Info, "valor 12345678 aparece");
    }
    const std::string content = ReadAll(path);
    CHECK(content.find("INFO  valor 12345678 aparece\n") != std::string::npos);
    CHECK(content.find("omitida") == std::string::npos);
    fs::remove(path);
}

TEST_CASE("client: FileLogger fechado nao grava nada") {
    FileLogger log;
    CHECK_FALSE(log.IsOpen());
    log.Write(Level::Info, "sem arquivo");
}

TEST_CASE("client: FileLogger escapa quebras de linha") {
    const auto path = fs::temp_directory_path() / "cbmp_client_test_escape.txt";
    fs::remove(path);
    {
        FileLogger log;
        REQUIRE(log.Open(path));
        log.Write(Level::Info, "a\nb");
    }
    CHECK(ReadAll(path).ends_with("INFO  a\\nb\n"));
    fs::remove(path);
}
