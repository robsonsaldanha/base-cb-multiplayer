#include "TestFramework.hpp"
#include <cbmp/Log.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace cbmp;

static std::tm SampleTime() {
    std::tm t{};
    t.tm_year = 2026 - 1900; t.tm_mon = 10; t.tm_mday = 9;
    t.tm_hour = 14; t.tm_min = 3; t.tm_sec = 7;
    return t;
}

TEST(format_line_info) {
    CHECK_EQ(FormatLine(Level::Info, "ola", SampleTime(), 5),
             std::string("[2026-11-09 14:03:07.005] INFO  ola"));
}

TEST(format_line_error) {
    CHECK_EQ(FormatLine(Level::Error, "deu ruim", SampleTime(), 123),
             std::string("[2026-11-09 14:03:07.123] ERRO  deu ruim"));
}

TEST(file_logger_escreve_linhas) {
    auto path = std::filesystem::temp_directory_path() / "cbmp_test_log.txt";
    std::filesystem::remove(path);
    {
        FileLogger log;
        CHECK(log.Open(path));
        log.Write(Level::Warn, "linha um");
        log.Write(Level::Info, "linha dois");
    }
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    std::string content = ss.str();
    CHECK(content.find("AVISO linha um\n") != std::string::npos);
    CHECK(content.find("INFO  linha dois\n") != std::string::npos);
}
