// Guarda: client/core é C++ puro. Nada de UE4SS, Unreal nem Windows.h (a camada do jogo fica em client/src/game/).
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct Hit {
    int line;
    std::string text;
};

std::string Lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// Linhas "#include" (com ou sem espaços depois do '#') que puxam algo proibido no núcleo.
std::vector<Hit> FindForbiddenIncludes(std::string_view text) {
    static constexpr std::string_view kForbidden[] = {"<Unreal/", "\"Unreal/", "<Mod/", "\"Mod/", "DynamicOutput",
                                                      "UE4SS", "<Helpers/", "\"Helpers/"};
    // Cabeçalhos Win32 que puxam Windows.h por trás (comparados em minúsculas).
    static constexpr std::string_view kWin32[] = {"windows.h", "winsock2.h", "windef.h", "winbase.h"};
    std::vector<Hit> hits;
    std::istringstream in{std::string(text)};
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] != '#') continue;
        const auto kw = line.find_first_not_of(" \t", first + 1);
        if (kw == std::string::npos || line.compare(kw, 7, "include") != 0) continue;
        const std::string_view rest = std::string_view(line).substr(kw + 7);
        const std::string lower = Lower(rest);
        bool bad = false;
        for (const auto w : kWin32) bad = bad || lower.find(w) != std::string::npos;
        for (const auto f : kForbidden) bad = bad || rest.find(f) != std::string_view::npos;
        if (bad) hits.push_back({n, line});
    }
    return hits;
}
}  // namespace

TEST_CASE("client: detector de include proibido") {
    const std::string sample =
        "#include <cbmp/client/Log.hpp>\n"
        "#include <windows.h>\n"
        "  #  include <WINDOWS.H>\n"
        "#include <Unreal/UObject.hpp>\n"
        "#include <Mod/CppUserModBase.hpp>\n"
        "#include <DynamicOutput/DynamicOutput.hpp>\n"
        "#include <UE4SSProgram.hpp>\n"
        "#include <Helpers/String.hpp>\n"
        "#include <WinSock2.h>\n"
        "// Windows.h e UE4SS citados em comentário não contam\n"
        "#include <string>\n";
    const auto hits = FindForbiddenIncludes(sample);
    REQUIRE(hits.size() == 8);
    CHECK(hits.front().line == 2);
    CHECK(hits.back().line == 9);
}

TEST_CASE("client: nucleo nao inclui UE4SS, Unreal nem Windows.h") {
    // O literal é UTF-8; construir de char8_t evita a página ANSI no Windows (pasta com acento).
    const fs::path root{std::u8string_view(reinterpret_cast<const char8_t*>(CBMP_CLIENT_CORE_DIR))};
    REQUIRE(fs::is_directory(root));
    int files = 0;
    std::string report;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension();
        if (ext != ".hpp" && ext != ".cpp" && ext != ".h" && ext != ".inl" && ext != ".ipp") continue;
        ++files;
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        for (const auto& h : FindForbiddenIncludes(ss.str())) {
            report += e.path().generic_string() + ":" + std::to_string(h.line) + ": " + h.text + "\n";
        }
    }
    INFO(report);
    CHECK(files > 0);
    CHECK(report.empty());
}
