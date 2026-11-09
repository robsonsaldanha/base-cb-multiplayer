# CB-MP Spike de Injeção — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mod C++ `cb-mp` para UE4SS que, dentro do Hogwarts Legacy (Steam/Epic), registra a posição do jogador 1x/s em `cb-mp.log` e cria/remove um clone do bruxo com F8, empacotado em `cb-mp-spike.zip` para teste em outro PC.

**Architecture:** Lógica pura (log, transform, detecção de loja, `SpikeController`) fica em `client/core`, sem UE4SS, testada com um `FakeGame`. O mod (`client/src`) implementa `IGame` sobre o UE4SS (`client/src/game/Ue4ssGame`) e só faz a ponte: tick do motor → `SpikeController::Tick`, F8 → fila → `SpikeController::OnToggleClone` no game thread. UE4SS é compilado do fonte num commit fixo e empacotado junto, para o ABI bater.

**Tech Stack:** C++23, MSVC 14.44+ (Visual Studio 2026 Community), CMake 4.2 (o que vem no VS), Rust stable (exigido pelo patternsleuth do UE4SS), RE-UE4SS commit `4fb9b615a24c724041ef23e2c4073f2610c4c6d0`, PowerShell 7 para scripts.

## Global Constraints

- Proibido copiar/portar qualquer código MafiaHub Framework / arcane-mp / HogwartsMP. Dumper-7 só como ferramenta, nunca código copiado.
- Alvo: Hogwarts Legacy UE 4.27, Steam e Epic. Executável em `<jogo>\Phoenix\Binaries\Win64\HogwartsLegacy.exe`.
- UE4SS fixado em `4fb9b615a24c724041ef23e2c4073f2610c4c6d0` (MIT). Mod e UE4SS compilados juntos, mesma config `Game__Shipping__Win64`.
- **Sem git no nosso projeto por enquanto** (decisão do Robson, 2026-11-09): nenhum passo faz `git add/commit`. Git é usado só para baixar o RE-UE4SS e seus submódulos em `client/deps/`. Cada task termina com um "Checkpoint" (build/teste verde) no lugar do commit.
- Unidades Unreal: centímetros. "2 m à frente" = 200 unidades.
- Log em UTF-8, arquivo `cb-mp.log` na pasta do mod (`...\ue4ss\Mods\cb-mp\`).
- Nunca usar "Hogwarts" no nome do produto. Nome do mod: `cb-mp`.

## Ajustes em relação à spec

1. `GetTransform` retorna `std::optional<Transform>` (falha de leitura não é exceção).
2. "Desativar após 30 s sem player" virou: aviso + diagnóstico a cada 30 s enquanto não acha o player (o menu principal pode durar minutos e não deve matar o mod); o mod se desativa após **5 erros** vindos da camada do jogo. Critério de sucesso inalterado.
3. Interface ganha `std::string Describe(PlayerHandle)` para o log de diagnóstico (classe/nome completo do pawn).

## Caminhos de ferramentas (usados nos comandos)

```powershell
$CMAKE = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$CTEST = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$ROOT  = "C:\Users\robso\OneDrive\CasteloBruxoRP\Launcher_livre"
```

## Estrutura de arquivos

```
Launcher_livre/
  client/
    CMakeLists.txt                 raiz do build; opção CBMP_WITH_UE4SS (ON/OFF)
    core/
      CMakeLists.txt               lib estática cbmp_core (sem UE4SS)
      include/cbmp/Transform.hpp   Vec3/Rot3/Transform + FormatTransform + OffsetForward
      include/cbmp/Log.hpp         Level, FormatLine, FileLogger
      include/cbmp/Store.hpp       Store, DetectStore, StoreName
      include/cbmp/IGame.hpp       PlayerHandle, GameError, IGame
      include/cbmp/SpikeController.hpp
      src/Transform.cpp
      src/Log.cpp
      src/Store.cpp
      src/SpikeController.cpp
    tests/
      CMakeLists.txt               executável cbmp_tests + ctest
      TestFramework.hpp            macros CHECK/TEST mínimas
      FakeGame.hpp                 IGame falso para testes
      test_main.cpp
      test_transform.cpp
      test_log.cpp
      test_store.cpp
      test_spike_controller.cpp
    src/                           o mod cb-mp.dll (só com CBMP_WITH_UE4SS=ON)
      CMakeLists.txt
      dllmain.cpp                  start_mod/uninstall_mod
      CbMpMod.hpp / CbMpMod.cpp    CppUserModBase: tick, F8, fila do game thread
      SafeCall.hpp                 converte SEH/exceções em GameError
      CrashDump.hpp / CrashDump.cpp minidump em crash
      FileHash.hpp / FileHash.cpp  SHA-256 do executável (BCrypt)
      game/Ue4ssGame.hpp / .cpp    IGame sobre UE4SS
    deps/RE-UE4SS/                 baixado pelo script, não editar
  tools/
    fetch-ue4ss.ps1                baixa RE-UE4SS no commit fixo
    build.ps1                      configura + compila (tests ou mod)
    package-spike.ps1              monta dist/cb-mp-spike.zip
  package/
    INSTALAR.bat
    DESINSTALAR.bat
    LEIA-ME.txt
    mods.txt
```

---

### Task 1: Núcleo puro + testes (sem UE4SS)

Não depende de nada externo. Pode ser feita antes do pré-requisito da Task 2.

**Files:**
- Create: `client/CMakeLists.txt`
- Create: `client/core/CMakeLists.txt`
- Create: `client/core/include/cbmp/Transform.hpp`, `Log.hpp`, `Store.hpp`, `IGame.hpp`, `SpikeController.hpp`
- Create: `client/core/src/Transform.cpp`, `Log.cpp`, `Store.cpp`, `SpikeController.cpp`
- Create: `client/tests/CMakeLists.txt`, `TestFramework.hpp`, `FakeGame.hpp`, `test_main.cpp`, `test_transform.cpp`, `test_log.cpp`, `test_store.cpp`, `test_spike_controller.cpp`
- Create: `tools/build.ps1`

**Interfaces:**
- Consumes: nada.
- Produces (usado pelas Tasks 3–5):
  - `namespace cbmp { struct Vec3 {double x,y,z;}; struct Rot3 {double pitch,yaw,roll;}; struct Transform {Vec3 pos; Rot3 rot;}; }`
  - `std::string cbmp::FormatTransform(const Transform&)`
  - `cbmp::Transform cbmp::OffsetForward(const Transform&, double distanceCm)`
  - `enum class cbmp::Level { Info, Warn, Error }`
  - `std::string cbmp::FormatLine(Level, std::string_view msg, const std::tm& when, int millis)`
  - `class cbmp::FileLogger { bool Open(const std::filesystem::path&); void Write(Level, std::string_view); }` e `cbmp::FileLogger& cbmp::Log()`
  - `enum class cbmp::Store { Steam, Epic, Unknown }`, `Store cbmp::DetectStore(std::wstring_view exePath)`, `const char* cbmp::StoreName(Store)`
  - `struct cbmp::PlayerHandle { void* ptr; }`, `struct cbmp::GameError : std::runtime_error`, `class cbmp::IGame`
  - `class cbmp::SpikeController { SpikeController(IGame&, LogFn); void Tick(double nowSeconds); void OnToggleClone(); bool Disabled() const; }`

- [ ] **Step 1: CMake raiz e do core**

`client/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.22)
project(CbMp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CBMP_WITH_UE4SS "Compilar UE4SS e o mod cb-mp.dll" ON)

if(CBMP_WITH_UE4SS)
    add_subdirectory(deps/RE-UE4SS)
endif()

add_subdirectory(core)

if(CBMP_WITH_UE4SS)
    add_subdirectory(src)
else()
    enable_testing()
    add_subdirectory(tests)
endif()
```

`client/core/CMakeLists.txt`:
```cmake
add_library(cbmp_core STATIC
    src/Transform.cpp
    src/Log.cpp
    src/Store.cpp
    src/SpikeController.cpp
)
target_include_directories(cbmp_core PUBLIC include)
target_compile_features(cbmp_core PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(cbmp_core PRIVATE /W4 /utf-8)
endif()
```

- [ ] **Step 2: Framework de teste mínimo e CMake dos testes**

`client/tests/TestFramework.hpp`:
```cpp
#pragma once
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace cbmp_test {
struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }
inline int& Failures() { static int f = 0; return f; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); } };
}

#define CBMP_CAT2(a, b) a##b
#define CBMP_CAT(a, b) CBMP_CAT2(a, b)
#define TEST(name) \
    static void name(); \
    static ::cbmp_test::Registrar CBMP_CAT(reg_, name){#name, &name}; \
    static void name()
#define CHECK(expr) \
    do { if (!(expr)) { ++::cbmp_test::Failures(); \
        std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK falhou: " #expr "\n"; } } while (0)
#define CHECK_EQ(a, b) \
    do { auto _va = (a); auto _vb = (b); if (!(_va == _vb)) { ++::cbmp_test::Failures(); \
        std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ falhou: " #a " == " #b \
                  << "\n  esquerda: " << _va << "\n  direita:  " << _vb << "\n"; } } while (0)
```

`client/tests/test_main.cpp`:
```cpp
#include "TestFramework.hpp"

int main() {
    for (auto& t : ::cbmp_test::Registry()) {
        int before = ::cbmp_test::Failures();
        t.fn();
        std::cout << (::cbmp_test::Failures() == before ? "[ OK ] " : "[FAIL] ") << t.name << "\n";
    }
    std::cout << ::cbmp_test::Registry().size() << " testes, " << ::cbmp_test::Failures() << " falhas\n";
    return ::cbmp_test::Failures() == 0 ? 0 : 1;
}
```

`client/tests/CMakeLists.txt`:
```cmake
add_executable(cbmp_tests
    test_main.cpp
    test_transform.cpp
    test_log.cpp
    test_store.cpp
    test_spike_controller.cpp
)
target_link_libraries(cbmp_tests PRIVATE cbmp_core)
if(MSVC)
    target_compile_options(cbmp_tests PRIVATE /utf-8)
endif()
add_test(NAME cbmp_tests COMMAND cbmp_tests)
```

`tools/build.ps1`:
```powershell
param(
    [ValidateSet("tests", "mod")] [string]$Target = "tests"
)
$ErrorActionPreference = "Stop"
$CMAKE = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$CTEST = Join-Path (Split-Path $CMAKE) "ctest.exe"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "client"

if ($Target -eq "tests") {
    $build = Join-Path $root "build\tests"
    & $CMAKE -S $src -B $build -G "Visual Studio 18 2026" -A x64 -DCBMP_WITH_UE4SS=OFF
    if ($LASTEXITCODE) { throw "configure falhou" }
    & $CMAKE --build $build --config Release
    if ($LASTEXITCODE) { throw "build falhou" }
    & $CTEST --test-dir $build -C Release --output-on-failure
    if ($LASTEXITCODE) { throw "testes falharam" }
} else {
    $build = Join-Path $root "build\mod"
    & $CMAKE -S $src -B $build -G "Visual Studio 18 2026" -A x64 -DCBMP_WITH_UE4SS=ON
    if ($LASTEXITCODE) { throw "configure falhou" }
    # Compila tudo: UE4SS.dll, o proxy dwmapi.dll e cb-mp.dll.
    & $CMAKE --build $build --config Game__Shipping__Win64
    if ($LASTEXITCODE) { throw "build falhou" }
}
```

- [ ] **Step 3: Escrever testes de Transform (falhando)**

`client/tests/test_transform.cpp`:
```cpp
#include "TestFramework.hpp"
#include <cbmp/Transform.hpp>
#include <cmath>

using namespace cbmp;

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

TEST(format_transform_mostra_posicao_e_rotacao) {
    Transform t{{1.5, -2.0, 300.25}, {0.0, 90.0, -5.5}};
    CHECK_EQ(FormatTransform(t), std::string("pos=(1.50, -2.00, 300.25) rot=(p 0.00, y 90.00, r -5.50)"));
}

TEST(offset_forward_yaw_zero_anda_em_x) {
    Transform t{{100, 100, 50}, {0, 0, 0}};
    Transform r = OffsetForward(t, 200);
    CHECK(Near(r.pos.x, 300)); CHECK(Near(r.pos.y, 100)); CHECK(Near(r.pos.z, 50));
    CHECK(Near(r.rot.yaw, 0));
}

TEST(offset_forward_yaw_90_anda_em_y) {
    Transform t{{0, 0, 0}, {10, 90, 0}};
    Transform r = OffsetForward(t, 200);
    CHECK(Near(r.pos.x, 0)); CHECK(Near(r.pos.y, 200));
    CHECK(Near(r.rot.pitch, 10)); // rotação preservada
}
```

- [ ] **Step 4: Escrever testes de Log e Store (falhando)**

`client/tests/test_log.cpp`:
```cpp
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
```

`client/tests/test_store.cpp`:
```cpp
#include "TestFramework.hpp"
#include <cbmp/Store.hpp>

using namespace cbmp;

TEST(detecta_steam) {
    CHECK(DetectStore(L"D:\\SteamLibrary\\steamapps\\common\\Hogwarts Legacy\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") == Store::Steam);
}
TEST(detecta_epic) {
    CHECK(DetectStore(L"C:\\Program Files\\Epic Games\\HogwartsLegacy\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") == Store::Epic);
}
TEST(detecta_desconhecida) {
    CHECK(DetectStore(L"E:\\Jogos\\HL\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe") == Store::Unknown);
}
TEST(nome_da_loja) {
    CHECK_EQ(std::string(StoreName(Store::Steam)), std::string("Steam"));
    CHECK_EQ(std::string(StoreName(Store::Epic)), std::string("Epic"));
    CHECK_EQ(std::string(StoreName(Store::Unknown)), std::string("desconhecida"));
}
```

- [ ] **Step 5: Escrever FakeGame e testes do SpikeController (falhando)**

`client/tests/FakeGame.hpp`:
```cpp
#pragma once
#include <cbmp/IGame.hpp>
#include <string>
#include <vector>

struct FakeGame : cbmp::IGame {
    bool hasPlayer = false;
    bool throwOnGetPlayer = false;
    cbmp::Transform playerTransform{};
    int spawnCalls = 0;
    int destroyCalls = 0;
    std::vector<cbmp::Transform> spawnedAt;
    int playerTag = 1;
    int cloneTag = 2;

    std::optional<cbmp::PlayerHandle> GetLocalPlayer() override {
        if (throwOnGetPlayer) throw cbmp::GameError("falha simulada");
        if (!hasPlayer) return std::nullopt;
        return cbmp::PlayerHandle{&playerTag};
    }
    std::optional<cbmp::Transform> GetTransform(cbmp::PlayerHandle) override { return playerTransform; }
    std::optional<cbmp::PlayerHandle> SpawnClone(const cbmp::Transform& t) override {
        ++spawnCalls; spawnedAt.push_back(t);
        return cbmp::PlayerHandle{&cloneTag};
    }
    void Destroy(cbmp::PlayerHandle) override { ++destroyCalls; }
    std::string Describe(cbmp::PlayerHandle) override { return "FakePawn /Game/Fake.FakePawn"; }
};
```

`client/tests/test_spike_controller.cpp`:
```cpp
#include "TestFramework.hpp"
#include "FakeGame.hpp"
#include <cbmp/SpikeController.hpp>
#include <cmath>

using namespace cbmp;

struct Captured { std::vector<std::pair<Level, std::string>> lines; };

static SpikeController::LogFn Capture(Captured& c) {
    return [&c](Level l, const std::string& m) { c.lines.emplace_back(l, m); };
}
static int Count(const Captured& c, const std::string& needle) {
    int n = 0;
    for (auto& [l, m] : c.lines) if (m.find(needle) != std::string::npos) ++n;
    return n;
}

TEST(loga_transform_uma_vez_por_segundo) {
    FakeGame g; g.hasPlayer = true; g.playerTransform = {{1, 2, 3}, {0, 0, 0}};
    Captured c; SpikeController s(g, Capture(c));
    s.Tick(0.0); s.Tick(0.5); s.Tick(1.0); s.Tick(1.6); s.Tick(2.0);
    CHECK_EQ(Count(c, "pos=("), 3); // em 0.0, 1.0 e 2.0
}

TEST(descreve_player_quando_encontra) {
    FakeGame g; g.hasPlayer = true;
    Captured c; SpikeController s(g, Capture(c));
    s.Tick(0.0); s.Tick(1.0);
    CHECK_EQ(Count(c, "player encontrado: FakePawn"), 1);
}

TEST(avisa_a_cada_30s_sem_player) {
    FakeGame g;
    Captured c; SpikeController s(g, Capture(c));
    for (double t = 0; t <= 61.0; t += 0.5) s.Tick(t);
    CHECK_EQ(Count(c, "aguardando player"), 2); // em 30 s e 60 s
    CHECK(!s.Disabled());
}

TEST(f8_cria_clone_2m_a_frente_e_f8_de_novo_remove) {
    FakeGame g; g.hasPlayer = true; g.playerTransform = {{0, 0, 0}, {0, 0, 0}};
    Captured c; SpikeController s(g, Capture(c));
    s.OnToggleClone();
    CHECK_EQ(g.spawnCalls, 1);
    CHECK(std::fabs(g.spawnedAt.at(0).pos.x - 200.0) < 1e-6);
    s.OnToggleClone();
    CHECK_EQ(g.destroyCalls, 1);
    s.OnToggleClone();
    CHECK_EQ(g.spawnCalls, 2);
}

TEST(f8_sem_player_so_avisa) {
    FakeGame g;
    Captured c; SpikeController s(g, Capture(c));
    s.OnToggleClone();
    CHECK_EQ(g.spawnCalls, 0);
    CHECK_EQ(Count(c, "F8: player não encontrado"), 1);
}

TEST(desativa_apos_5_erros_do_jogo) {
    FakeGame g; g.throwOnGetPlayer = true;
    Captured c; SpikeController s(g, Capture(c));
    for (int i = 0; i < 4; ++i) s.Tick(i * 1.0);
    CHECK(!s.Disabled());
    s.Tick(4.0);
    CHECK(s.Disabled());
    CHECK_EQ(Count(c, "mod desativado"), 1);
    s.Tick(5.0); s.OnToggleClone(); // desativado: não faz nada
    CHECK_EQ(Count(c, "falha simulada"), 5);
}
```

- [ ] **Step 6: Rodar e confirmar que falha (não compila)**

Run: `pwsh -File tools/build.ps1 -Target tests`
Expected: erro de compilação `cbmp/Transform.hpp: No such file` (ou equivalente MSVC `C1083`).

- [ ] **Step 7: Implementar headers e fontes do core**

`client/core/include/cbmp/Transform.hpp`:
```cpp
#pragma once
#include <string>

namespace cbmp {
struct Vec3 { double x{}, y{}, z{}; };
struct Rot3 { double pitch{}, yaw{}, roll{}; };
struct Transform { Vec3 pos{}; Rot3 rot{}; };

// "pos=(1.50, -2.00, 300.25) rot=(p 0.00, y 90.00, r -5.50)"
std::string FormatTransform(const Transform& t);

// Desloca no plano XY na direção do yaw (graus). Rotação preservada.
Transform OffsetForward(const Transform& t, double distanceCm);
}
```

`client/core/src/Transform.cpp`:
```cpp
#include <cbmp/Transform.hpp>
#include <cmath>
#include <format>
#include <numbers>

namespace cbmp {
std::string FormatTransform(const Transform& t) {
    return std::format("pos=({:.2f}, {:.2f}, {:.2f}) rot=(p {:.2f}, y {:.2f}, r {:.2f})",
                       t.pos.x, t.pos.y, t.pos.z, t.rot.pitch, t.rot.yaw, t.rot.roll);
}

Transform OffsetForward(const Transform& t, double distanceCm) {
    const double rad = t.rot.yaw * std::numbers::pi / 180.0;
    Transform r = t;
    r.pos.x += std::cos(rad) * distanceCm;
    r.pos.y += std::sin(rad) * distanceCm;
    return r;
}
}
```

`client/core/include/cbmp/Log.hpp`:
```cpp
#pragma once
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace cbmp {
enum class Level { Info, Warn, Error };

// "[2026-11-09 14:03:07.005] INFO  msg"  (INFO / AVISO / ERRO, alinhado em 5 chars + espaço)
std::string FormatLine(Level level, std::string_view msg, const std::tm& when, int millis);

class FileLogger {
public:
    bool Open(const std::filesystem::path& path); // append, UTF-8
    void Write(Level level, std::string_view msg); // thread-safe, flush a cada linha
private:
    std::mutex m_mutex;
    std::ofstream m_out;
};

FileLogger& Log(); // instância global do mod
}
```

`client/core/src/Log.cpp`:
```cpp
#include <cbmp/Log.hpp>
#include <chrono>
#include <format>

namespace cbmp {
static const char* LevelTag(Level l) {
    switch (l) {
        case Level::Info: return "INFO ";
        case Level::Warn: return "AVISO";
        case Level::Error: return "ERRO ";
    }
    return "?????";
}

std::string FormatLine(Level level, std::string_view msg, const std::tm& w, int millis) {
    return std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] {} {}",
                       w.tm_year + 1900, w.tm_mon + 1, w.tm_mday, w.tm_hour, w.tm_min, w.tm_sec,
                       millis, LevelTag(level), msg);
}

bool FileLogger::Open(const std::filesystem::path& path) {
    std::lock_guard lock(m_mutex);
    m_out.open(path, std::ios::out | std::ios::app | std::ios::binary);
    return m_out.is_open();
}

void FileLogger::Write(Level level, std::string_view msg) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &tt);
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::lock_guard lock(m_mutex);
    if (!m_out.is_open()) return;
    m_out << FormatLine(level, msg, local, ms) << '\n';
    m_out.flush();
}

FileLogger& Log() {
    static FileLogger instance;
    return instance;
}
}
```

`client/core/include/cbmp/Store.hpp`:
```cpp
#pragma once
#include <string_view>

namespace cbmp {
enum class Store { Steam, Epic, Unknown };
Store DetectStore(std::wstring_view exePath); // heurística pelo caminho
const char* StoreName(Store s);
}
```

`client/core/src/Store.cpp`:
```cpp
#include <cbmp/Store.hpp>
#include <algorithm>
#include <cwctype>
#include <string>

namespace cbmp {
static std::wstring Lower(std::wstring_view s) {
    std::wstring r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return r;
}

Store DetectStore(std::wstring_view exePath) {
    const std::wstring p = Lower(exePath);
    if (p.find(L"\\steamapps\\") != std::wstring::npos) return Store::Steam;
    if (p.find(L"\\epic games\\") != std::wstring::npos) return Store::Epic;
    return Store::Unknown;
}

const char* StoreName(Store s) {
    switch (s) {
        case Store::Steam: return "Steam";
        case Store::Epic: return "Epic";
        case Store::Unknown: return "desconhecida";
    }
    return "desconhecida";
}
}
```

`client/core/include/cbmp/IGame.hpp`:
```cpp
#pragma once
#include <cbmp/Transform.hpp>
#include <optional>
#include <stdexcept>
#include <string>

namespace cbmp {
struct PlayerHandle {
    void* ptr{};
    bool operator==(const PlayerHandle&) const = default;
};

// Lançada pela camada do jogo quando algo falha (inclusive SEH convertida).
struct GameError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Única porta de entrada para o jogo. Só client/src/game/ implementa.
// Todos os métodos devem ser chamados no game thread.
class IGame {
public:
    virtual ~IGame() = default;
    virtual std::optional<PlayerHandle> GetLocalPlayer() = 0;
    virtual std::optional<Transform> GetTransform(PlayerHandle p) = 0;
    virtual std::optional<PlayerHandle> SpawnClone(const Transform& at) = 0;
    virtual void Destroy(PlayerHandle p) = 0;
    virtual std::string Describe(PlayerHandle p) = 0;
};
}
```

`client/core/include/cbmp/SpikeController.hpp`:
```cpp
#pragma once
#include <cbmp/IGame.hpp>
#include <cbmp/Log.hpp>
#include <functional>
#include <optional>
#include <string>

namespace cbmp {
class SpikeController {
public:
    using LogFn = std::function<void(Level, const std::string&)>;

    SpikeController(IGame& game, LogFn log);

    void Tick(double nowSeconds); // chamado todo frame, no game thread
    void OnToggleClone();         // F8, no game thread
    bool Disabled() const { return m_disabled; }

    static constexpr int kMaxErrors = 5;
    static constexpr double kWaitWarnInterval = 30.0;
    static constexpr double kCloneDistanceCm = 200.0;

private:
    void ReportError(const std::string& what);

    IGame& m_game;
    LogFn m_log;
    bool m_disabled = false;
    int m_errors = 0;
    std::optional<double> m_start;
    std::optional<double> m_lastTransformLog;
    double m_lastWaitWarn = 0.0;
    bool m_described = false;
    std::optional<PlayerHandle> m_clone;
};
}
```

`client/core/src/SpikeController.cpp`:
```cpp
#include <cbmp/SpikeController.hpp>
#include <format>

namespace cbmp {
SpikeController::SpikeController(IGame& game, LogFn log) : m_game(game), m_log(std::move(log)) {}

void SpikeController::ReportError(const std::string& what) {
    ++m_errors;
    m_log(Level::Error, std::format("erro do jogo ({}/{}): {}", m_errors, kMaxErrors, what));
    if (m_errors >= kMaxErrors) {
        m_disabled = true;
        m_log(Level::Error, "mod desativado após erros repetidos; mande o cb-mp.log");
    }
}

void SpikeController::Tick(double now) {
    if (m_disabled) return;
    if (!m_start) m_start = now;
    try {
        auto player = m_game.GetLocalPlayer();
        if (!player) {
            if (now - *m_start - m_lastWaitWarn >= kWaitWarnInterval) {
                m_lastWaitWarn = now - *m_start;
                m_log(Level::Warn, std::format("aguardando player há {:.0f} s (menu/carregamento?)", m_lastWaitWarn));
            }
            return;
        }
        if (!m_described) {
            m_described = true;
            m_log(Level::Info, "player encontrado: " + m_game.Describe(*player));
        }
        if (!m_lastTransformLog || now - *m_lastTransformLog >= 1.0) {
            m_lastTransformLog = now;
            if (auto t = m_game.GetTransform(*player)) {
                m_log(Level::Info, FormatTransform(*t));
            } else {
                m_log(Level::Warn, "não consegui ler o transform do player");
            }
        }
    } catch (const std::exception& e) {
        ReportError(e.what());
    }
}

void SpikeController::OnToggleClone() {
    if (m_disabled) return;
    try {
        if (m_clone) {
            m_game.Destroy(*m_clone);
            m_clone.reset();
            m_log(Level::Info, "F8: clone removido");
            return;
        }
        auto player = m_game.GetLocalPlayer();
        if (!player) { m_log(Level::Warn, "F8: player não encontrado"); return; }
        auto t = m_game.GetTransform(*player);
        if (!t) { m_log(Level::Warn, "F8: sem transform do player"); return; }
        const Transform at = OffsetForward(*t, kCloneDistanceCm);
        m_clone = m_game.SpawnClone(at);
        if (m_clone) m_log(Level::Info, "F8: clone criado em " + FormatTransform(at) + " -> " + m_game.Describe(*m_clone));
        else m_log(Level::Warn, "F8: spawn do clone falhou (ver linhas acima)");
    } catch (const std::exception& e) {
        ReportError(e.what());
    }
}
}
```

- [ ] **Step 8: Rodar testes**

Run: `pwsh -File tools/build.ps1 -Target tests`
Expected: `16 testes, 0 falhas` e ctest `100% tests passed`.

- [ ] **Step 9: Checkpoint** — build `tests` verde. Sem commit (sem git por enquanto).

---

### Task 2: Pré-requisito Epic + baixar RE-UE4SS fixado

**Files:**
- Create: `tools/fetch-ue4ss.ps1`

**Interfaces:**
- Consumes: nada.
- Produces: `client/deps/RE-UE4SS/` no commit `4fb9b615a24c724041ef23e2c4073f2610c4c6d0` com submódulos `deps/first/Unreal` (UEPseudo) e `deps/first/patternsleuth`.

- [ ] **Step 1 (Robson, manual): vincular GitHub à Epic**

1. Entrar em https://www.epicgames.com/account/connections → aba "Contas" → conectar GitHub (conta que está no `gh` deste PC).
2. Aceitar o e-mail de convite para a organização `EpicGames` no GitHub (https://github.com/EpicGames).
3. Verificar acesso:

Run: `gh api repos/Re-UE4SS/UEPseudo --jq .full_name`
Expected: `Re-UE4SS/UEPseudo` (antes do vínculo dá `404 Not Found`).

- [ ] **Step 2: Script de download**

`tools/fetch-ue4ss.ps1`:
```powershell
# Baixa RE-UE4SS no commit fixo. Usa git só como ferramenta de download.
$ErrorActionPreference = "Stop"
$COMMIT = "4fb9b615a24c724041ef23e2c4073f2610c4c6d0"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "client\deps\RE-UE4SS"

# Submódulos usam git@github.com (SSH). Redireciona para HTTPS com o login do gh.
gh auth setup-git
$env:GIT_CONFIG_COUNT = "1"
$env:GIT_CONFIG_KEY_0 = "url.https://github.com/.insteadOf"
$env:GIT_CONFIG_VALUE_0 = "git@github.com:"

if (-not (Test-Path $dest)) {
    git clone https://github.com/UE4SS-RE/RE-UE4SS.git $dest
    if ($LASTEXITCODE) { throw "clone falhou" }
}
git -C $dest fetch origin $COMMIT
git -C $dest checkout --force $COMMIT
if ($LASTEXITCODE) { throw "checkout falhou" }
git -C $dest submodule update --init --recursive
if ($LASTEXITCODE) { throw "submódulos falharam (vínculo GitHub-Epic feito?)" }
$head = git -C $dest rev-parse HEAD
if ($head -ne $COMMIT) { throw "HEAD $head != $COMMIT" }
Write-Host "RE-UE4SS pronto em $dest ($COMMIT)"
```

- [ ] **Step 3: Rodar**

Run: `pwsh -File tools/fetch-ue4ss.ps1`
Expected: última linha `RE-UE4SS pronto em ...\client\deps\RE-UE4SS (4fb9b615...)`; existe `client/deps/RE-UE4SS/deps/first/Unreal/CMakeLists.txt`.

- [ ] **Step 4: Checkpoint** — dependência baixada.

---

### Task 3: Mod esqueleto compila e loga "olá"

**Files:**
- Create: `client/src/CMakeLists.txt`, `client/src/dllmain.cpp`, `client/src/CbMpMod.hpp`, `client/src/CbMpMod.cpp`, `client/src/FileHash.hpp`, `client/src/FileHash.cpp`

**Interfaces:**
- Consumes: `cbmp::Log()`, `cbmp::DetectStore`, `cbmp::StoreName` (Task 1); UE4SS em `client/deps/RE-UE4SS` (Task 2).
- Produces: classe `CbMpMod : RC::CppUserModBase` com `std::filesystem::path ModDir()`; `std::string Sha256Hex(const std::filesystem::path&)`; DLL `cb-mp.dll`.

- [ ] **Step 1: CMake do mod**

`client/src/CMakeLists.txt`:
```cmake
set(TARGET cb-mp)
add_library(${TARGET} SHARED
    dllmain.cpp
    CbMpMod.cpp
    FileHash.cpp
)
target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE cbmp_core bcrypt)
target_compile_options(${TARGET} PRIVATE /utf-8)
```

- [ ] **Step 2: Hash do executável**

`client/src/FileHash.hpp`:
```cpp
#pragma once
#include <filesystem>
#include <string>

// SHA-256 em hex minúsculo; "erro" se não conseguir ler.
std::string Sha256Hex(const std::filesystem::path& file);
```

`client/src/FileHash.cpp`:
```cpp
#include "FileHash.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <format>
#include <fstream>
#include <vector>

std::string Sha256Hex(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return "erro";
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "erro";
    std::string result = "erro";
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        bool ok = true;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = static_cast<ULONG>(in.gcount());
            if (n && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), n, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32]{};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            result.clear();
            for (unsigned char b : digest) result += std::format("{:02x}", b);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}
```

- [ ] **Step 3: Classe do mod (só log de inicialização)**

`client/src/CbMpMod.hpp`:
```cpp
#pragma once
#include <Mod/CppUserModBase.hpp>
#include <filesystem>

class CbMpMod final : public RC::CppUserModBase {
public:
    CbMpMod();
    ~CbMpMod() override;
    auto on_unreal_init() -> void override;

    static std::filesystem::path ModDir(); // pasta ...\Mods\cb-mp\ (pai de dlls\)
};
```

`client/src/CbMpMod.cpp`:
```cpp
#include "CbMpMod.hpp"
#include "FileHash.hpp"
#include <cbmp/Log.hpp>
#include <cbmp/Store.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <format>

using namespace RC;

static std::filesystem::path ThisDllPath() {
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisDllPath), &self);
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

static std::filesystem::path GameExePath() {
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

std::filesystem::path CbMpMod::ModDir() {
    // .../cb-mp/dlls/main.dll -> .../cb-mp
    return ThisDllPath().parent_path().parent_path();
}

CbMpMod::CbMpMod() {
    ModName = STR("cb-mp");
    ModVersion = STR("0.1.0-spike");
    ModDescription = STR("Castelo Bruxo multiplayer - spike de injecao");
    ModAuthors = STR("Castelo Bruxo RP");

    cbmp::Log().Open(ModDir() / "cb-mp.log");
    const auto exe = GameExePath();
    cbmp::Log().Write(cbmp::Level::Info, "==== cb-mp 0.1.0-spike iniciando ====");
    cbmp::Log().Write(cbmp::Level::Info, "exe: " + exe.string());
    cbmp::Log().Write(cbmp::Level::Info, std::format("loja: {}", cbmp::StoreName(cbmp::DetectStore(exe.wstring()))));
    cbmp::Log().Write(cbmp::Level::Info, "sha256 exe: " + Sha256Hex(exe));
    cbmp::Log().Write(cbmp::Level::Info, "UE4SS commit: 4fb9b615a24c724041ef23e2c4073f2610c4c6d0");
    Output::send<LogLevel::Verbose>(STR("[cb-mp] carregado\n"));
}

CbMpMod::~CbMpMod() {
    cbmp::Log().Write(cbmp::Level::Info, "cb-mp descarregado");
}

auto CbMpMod::on_unreal_init() -> void {
    cbmp::Log().Write(cbmp::Level::Info, "Unreal inicializado (UE4SS on_unreal_init)");
}
```

`client/src/dllmain.cpp`:
```cpp
#include "CbMpMod.hpp"

#define CBMP_API __declspec(dllexport)
extern "C" {
CBMP_API RC::CppUserModBase* start_mod() { return new CbMpMod(); }
CBMP_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
```

- [ ] **Step 4: Compilar**

Run: `pwsh -File tools/build.ps1 -Target mod`
Expected: build termina sem erro; existem `cb-mp.dll`, `UE4SS.dll` e `dwmapi.dll` sob `build/mod` (localizar com `Get-ChildItem build\mod -Recurse -Include cb-mp.dll,UE4SS.dll,dwmapi.dll`). Primeira compilação do UE4SS leva vários minutos (Rust + C++); rodar em background.

- [ ] **Step 5: Checkpoint** — `cb-mp.dll` gerado; testes da Task 1 continuam verdes (`pwsh -File tools/build.ps1 -Target tests`).

---

### Task 4: `Ue4ssGame` + tick no game thread + F8 + proteção de erro

**Files:**
- Create: `client/src/SafeCall.hpp`, `client/src/CrashDump.hpp`, `client/src/CrashDump.cpp`, `client/src/game/Ue4ssGame.hpp`, `client/src/game/Ue4ssGame.cpp`
- Modify: `client/src/CMakeLists.txt` (fontes novas, `/EHa`, `dbghelp`)
- Modify: `client/src/CbMpMod.hpp`, `client/src/CbMpMod.cpp` (controller, hook de tick, F8)

**Interfaces:**
- Consumes: `cbmp::IGame`, `cbmp::SpikeController`, `cbmp::GameError`, `cbmp::Log()` (Task 1); `CbMpMod::ModDir()` (Task 3).
- Produces: `template<class F> auto SafeCall(const char* what, F&& f)`; `void InstallCrashDump(const std::filesystem::path& dir)`; `class Ue4ssGame final : public cbmp::IGame`.

- [ ] **Step 1: SafeCall**

`client/src/SafeCall.hpp`:
```cpp
#pragma once
#include <cbmp/IGame.hpp>
#include <string>

// Requer /EHa: catch(...) também pega exceções SEH (access violation etc.).
template <class F>
auto SafeCall(const char* what, F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const cbmp::GameError&) {
        throw;
    } catch (const std::exception& e) {
        throw cbmp::GameError(std::string(what) + ": " + e.what());
    } catch (...) {
        throw cbmp::GameError(std::string(what) + ": exceção SEH (provável ponteiro inválido)");
    }
}
```

- [ ] **Step 2: Minidump de crash**

`client/src/CrashDump.hpp`:
```cpp
#pragma once
#include <filesystem>

// Grava cb-mp-AAAAMMDD-HHMMSS.dmp em `dir` se o processo crashar.
void InstallCrashDump(const std::filesystem::path& dir);
```

`client/src/CrashDump.cpp`:
```cpp
#include "CrashDump.hpp"
#include <cbmp/Log.hpp>
#include <Windows.h>
#include <DbgHelp.h>
#include <chrono>
#include <format>

static std::filesystem::path g_dumpDir;
static LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

static LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
    const auto now = std::chrono::zoned_time{std::chrono::current_zone(), std::chrono::system_clock::now()};
    const auto name = std::format("cb-mp-{:%Y%m%d-%H%M%S}.dmp", std::chrono::floor<std::chrono::seconds>(now.get_local_time()));
    const auto path = g_dumpDir / name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithIndirectlyReferencedMemory, &mei, nullptr, nullptr);
        CloseHandle(file);
        cbmp::Log().Write(cbmp::Level::Error, "CRASH: dump gravado em " + path.string());
    }
    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashDump(const std::filesystem::path& dir) {
    g_dumpDir = dir;
    g_previous = SetUnhandledExceptionFilter(&OnCrash);
}
```

Nota: o Unreal pode instalar o próprio filtro depois e engolir o nosso. Se o `.dmp` não aparecer num crash real, o log registra isso e o próximo subprojeto troca por `AddVectoredExceptionHandler`.

- [ ] **Step 3: Ue4ssGame**

`client/src/game/Ue4ssGame.hpp`:
```cpp
#pragma once
#include <cbmp/IGame.hpp>
#include <String/StringType.hpp>

namespace RC::Unreal { class UObject; }

// Única classe que conhece UE4SS/Unreal. Chamar só no game thread.
class Ue4ssGame final : public cbmp::IGame {
public:
    std::optional<cbmp::PlayerHandle> GetLocalPlayer() override;
    std::optional<cbmp::Transform> GetTransform(cbmp::PlayerHandle p) override;
    std::optional<cbmp::PlayerHandle> SpawnClone(const cbmp::Transform& at) override;
    void Destroy(cbmp::PlayerHandle p) override;
    std::string Describe(cbmp::PlayerHandle p) override;

private:
    RC::Unreal::UObject* FindPlayerController();
    RC::StringType m_pcClassName;     // classe real do PlayerController, descoberta por varredura
    double m_lastScan = -1e9;
};
```

`client/src/game/Ue4ssGame.cpp`:
```cpp
#include "Ue4ssGame.hpp"
#include "../SafeCall.hpp"
#include <cbmp/Log.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>
#include <chrono>
#include <format>

using namespace RC;
using namespace RC::Unreal;

#define CBMP_T(s) FromCharTypePtr<TCHAR>(STR(s))

static double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static bool IsDefaultObject(UObject* o) {
    return to_string(o->GetName()).starts_with("Default__");
}

// Sobe a hierarquia de classes procurando um nome (IsA por nome, sem depender de StaticClass).
static bool IsAByName(UObject* o, const StringType& className) {
    for (UStruct* s = o->GetClassPrivate(); s; s = s->GetSuperStruct()) {
        if (s->GetName() == className) return true;
    }
    return false;
}

static UObject* ReadObjectProp(UObject* owner, const TCHAR* prop) {
    if (!owner) return nullptr;
    auto** v = owner->GetValuePtrByPropertyNameInChain<UObject*>(prop);
    return v ? *v : nullptr;
}

UObject* Ue4ssGame::FindPlayerController() {
    return SafeCall("FindPlayerController", [&]() -> UObject* {
        if (!m_pcClassName.empty()) {
            if (UObject* pc = UObjectGlobals::FindFirstOf(m_pcClassName)) return pc;
        }
        if (UObject* pc = UObjectGlobals::FindFirstOf(STR("PlayerController"))) {
            m_pcClassName = pc->GetClassPrivate()->GetName();
            return pc;
        }
        // Varredura cara: no máximo a cada 5 s, só enquanto não achou.
        const double now = NowSeconds();
        if (now - m_lastScan < 5.0) return nullptr;
        m_lastScan = now;
        UObject* found = nullptr;
        const StringType target = STR("PlayerController");
        UObjectGlobals::ForEachUObject([&](void* raw, int32_t, int32_t) {
            auto* o = static_cast<UObject*>(raw);
            if (o && !IsDefaultObject(o) && IsAByName(o, target)) { found = o; return LoopAction::Break; }
            return LoopAction::Continue;
        });
        if (found) {
            m_pcClassName = found->GetClassPrivate()->GetName();
            cbmp::Log().Write(cbmp::Level::Info, "PlayerController descoberto por varredura: classe " + to_string(m_pcClassName));
        }
        return found;
    });
}

std::optional<cbmp::PlayerHandle> Ue4ssGame::GetLocalPlayer() {
    UObject* pc = FindPlayerController();
    if (!pc) return std::nullopt;
    return SafeCall("GetLocalPlayer", [&]() -> std::optional<cbmp::PlayerHandle> {
        UObject* pawn = ReadObjectProp(pc, CBMP_T("Pawn"));
        if (!pawn) pawn = ReadObjectProp(pc, CBMP_T("AcknowledgedPawn"));
        if (!pawn) return std::nullopt;
        return cbmp::PlayerHandle{pawn};
    });
}

std::optional<cbmp::Transform> Ue4ssGame::GetTransform(cbmp::PlayerHandle p) {
    return SafeCall("GetTransform", [&]() -> std::optional<cbmp::Transform> {
        auto* actor = static_cast<UObject*>(p.ptr);
        UObject* root = ReadObjectProp(actor, CBMP_T("RootComponent"));
        if (!root) return std::nullopt;
        auto* loc = root->GetValuePtrByPropertyNameInChain<FVector>(CBMP_T("RelativeLocation"));
        auto* rot = root->GetValuePtrByPropertyNameInChain<FRotator>(CBMP_T("RelativeRotation"));
        if (!loc || !rot) return std::nullopt;
        return cbmp::Transform{{loc->X(), loc->Y(), loc->Z()}, {rot->GetPitch(), rot->GetYaw(), rot->GetRoll()}};
    });
}

std::optional<cbmp::PlayerHandle> Ue4ssGame::SpawnClone(const cbmp::Transform& at) {
    auto player = GetLocalPlayer();
    if (!player) return std::nullopt;
    return SafeCall("SpawnClone", [&]() -> std::optional<cbmp::PlayerHandle> {
        auto* pawn = static_cast<UObject*>(player->ptr);
        // Mundo: Engine -> GameViewport -> World (todos UPROPERTY).
        UObject* engine = UObjectGlobals::FindFirstOf(STR("GameEngine"));
        UObject* viewport = ReadObjectProp(engine, CBMP_T("GameViewport"));
        auto* world = static_cast<UWorld*>(ReadObjectProp(viewport, CBMP_T("World")));
        if (!world) {
            cbmp::Log().Write(cbmp::Level::Warn, "SpawnClone: UWorld não encontrado via GameEngine.GameViewport.World");
            return std::nullopt;
        }
        UClass* cls = pawn->GetClassPrivate();
        cbmp::Log().Write(cbmp::Level::Info, "SpawnClone: spawnando classe " + to_string(cls->GetFullName()));
        FVector loc{};
        loc.SetX(at.pos.x); loc.SetY(at.pos.y); loc.SetZ(at.pos.z);
        FRotator rot{};
        rot.SetPitch(0.0); rot.SetYaw(at.rot.yaw); rot.SetRoll(0.0);
        AActor* actor = world->SpawnActor(cls, &loc, &rot);
        if (!actor) return std::nullopt;
        return cbmp::PlayerHandle{actor};
    });
}

void Ue4ssGame::Destroy(cbmp::PlayerHandle p) {
    SafeCall("Destroy", [&]() {
        auto* actor = static_cast<UObject*>(p.ptr);
        UFunction* fn = actor->GetFunctionByNameInChain(CBMP_T("K2_DestroyActor"));
        if (!fn) throw cbmp::GameError("K2_DestroyActor não encontrada");
        actor->ProcessEvent(fn, nullptr);
    });
}

std::string Ue4ssGame::Describe(cbmp::PlayerHandle p) {
    return SafeCall("Describe", [&]() -> std::string {
        auto* o = static_cast<UObject*>(p.ptr);
        return std::format("{} | {}", to_string(o->GetClassPrivate()->GetName()), to_string(o->GetFullName()));
    });
}
```

Pontos que só o teste no jogo confirma (o log cobre todos): nome da classe do PlayerController no HL, se `SpawnActor` com a classe do jogador cria um bruxo visível ou um ator vazio, e se `RelativeLocation` do root = posição no mundo (vale enquanto o capsule não estiver anexado a nada). Se a compilação reclamar de algum nome de API (`GetFunctionByNameInChain`, `SetPitch`, `World.hpp`), procurar o nome certo em `client/deps/RE-UE4SS/deps/first/Unreal/include/Unreal/` com `Select-String` e ajustar só este arquivo.

- [ ] **Step 4: Ligar tudo no mod**

`client/src/CbMpMod.hpp` (substituir inteiro):
```cpp
#pragma once
#include <Mod/CppUserModBase.hpp>
#include <atomic>
#include <filesystem>
#include <memory>

namespace cbmp { class SpikeController; }
class Ue4ssGame;

class CbMpMod final : public RC::CppUserModBase {
public:
    CbMpMod();
    ~CbMpMod() override;
    auto on_unreal_init() -> void override;

    static std::filesystem::path ModDir();

private:
    void OnGameTick(); // game thread

    std::unique_ptr<Ue4ssGame> m_game;
    std::unique_ptr<cbmp::SpikeController> m_controller;
    std::atomic<int> m_pendingToggles{0}; // F8 vem de outra thread; consumido no tick
};
```

`client/src/CbMpMod.cpp` (substituir inteiro):
```cpp
#include "CbMpMod.hpp"
#include "CrashDump.hpp"
#include "FileHash.hpp"
#include "game/Ue4ssGame.hpp"
#include <cbmp/Log.hpp>
#include <cbmp/SpikeController.hpp>
#include <cbmp/Store.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/Handler.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UEngine.hpp>
#include <Windows.h>
#include <chrono>
#include <format>

using namespace RC;

static std::filesystem::path ThisDllPath() {
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisDllPath), &self);
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

static std::filesystem::path GameExePath() {
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

static double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path CbMpMod::ModDir() {
    return ThisDllPath().parent_path().parent_path();
}

CbMpMod::CbMpMod() {
    ModName = STR("cb-mp");
    ModVersion = STR("0.1.0-spike");
    ModDescription = STR("Castelo Bruxo multiplayer - spike de injecao");
    ModAuthors = STR("Castelo Bruxo RP");

    cbmp::Log().Open(ModDir() / "cb-mp.log");
    InstallCrashDump(ModDir());
    const auto exe = GameExePath();
    cbmp::Log().Write(cbmp::Level::Info, "==== cb-mp 0.1.0-spike iniciando ====");
    cbmp::Log().Write(cbmp::Level::Info, "exe: " + exe.string());
    cbmp::Log().Write(cbmp::Level::Info, std::format("loja: {}", cbmp::StoreName(cbmp::DetectStore(exe.wstring()))));
    cbmp::Log().Write(cbmp::Level::Info, "sha256 exe: " + Sha256Hex(exe));
    cbmp::Log().Write(cbmp::Level::Info, "UE4SS commit: 4fb9b615a24c724041ef23e2c4073f2610c4c6d0");

    register_keydown_event(Input::Key::F8, [this]() { m_pendingToggles.fetch_add(1); });
    Output::send<LogLevel::Verbose>(STR("[cb-mp] carregado\n"));
}

CbMpMod::~CbMpMod() {
    cbmp::Log().Write(cbmp::Level::Info, "cb-mp descarregado");
}

auto CbMpMod::on_unreal_init() -> void {
    cbmp::Log().Write(cbmp::Level::Info, "Unreal inicializado; instalando hook de tick");
    m_game = std::make_unique<Ue4ssGame>();
    m_controller = std::make_unique<cbmp::SpikeController>(
        *m_game, [](cbmp::Level l, const std::string& m) { cbmp::Log().Write(l, m); });
    Unreal::Hook::RegisterEngineTickPreCallback(
        [this](auto&, Unreal::UEngine*, float, bool) { OnGameTick(); },
        {true, true, STR("cb-mp"), STR("GameTick")});
}

void CbMpMod::OnGameTick() {
    if (!m_controller) return;
    for (int n = m_pendingToggles.exchange(0); n > 0; --n) m_controller->OnToggleClone();
    m_controller->Tick(NowSeconds());
}
```

`client/src/CMakeLists.txt` (substituir inteiro):
```cmake
set(TARGET cb-mp)
add_library(${TARGET} SHARED
    dllmain.cpp
    CbMpMod.cpp
    FileHash.cpp
    CrashDump.cpp
    game/Ue4ssGame.cpp
)
target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE cbmp_core bcrypt dbghelp)
target_compile_options(${TARGET} PRIVATE /utf-8 /EHa)
```

- [ ] **Step 5: Compilar mod e rodar testes**

Run: `pwsh -File tools/build.ps1 -Target mod`
Expected: sem erros. Se algum nome da API do UE4SS não bater, ajustar apenas `game/Ue4ssGame.cpp` ou os includes de `CbMpMod.cpp` conforme os headers reais em `client/deps/RE-UE4SS`.

Run: `pwsh -File tools/build.ps1 -Target tests`
Expected: `16 testes, 0 falhas`.

- [ ] **Step 6: Checkpoint** — mod completo compila; núcleo testado.

---

### Task 5: Pacote `cb-mp-spike.zip`

**Files:**
- Create: `package/mods.txt`, `package/INSTALAR.bat`, `package/DESINSTALAR.bat`, `package/LEIA-ME.txt`
- Create: `tools/package-spike.ps1`

**Interfaces:**
- Consumes: saídas do build `build/mod` (`UE4SS.dll`, `dwmapi.dll`, `cb-mp.dll`) e `client/deps/RE-UE4SS/assets/UE4SS-settings.ini`.
- Produces: `dist/cb-mp-spike.zip` com layout:
  ```
  INSTALAR.bat, DESINSTALAR.bat, LEIA-ME.txt
  payload/dwmapi.dll
  payload/ue4ss/UE4SS.dll
  payload/ue4ss/UE4SS-settings.ini
  payload/ue4ss/Mods/mods.txt
  payload/ue4ss/Mods/cb-mp/enabled.txt
  payload/ue4ss/Mods/cb-mp/dlls/main.dll
  ```

- [ ] **Step 1: Arquivos do pacote**

`package/mods.txt`:
```
cb-mp : 1

; Built-in keybinds, do not move up!
Keybinds : 0
```

`package/INSTALAR.bat`:
```bat
@echo off
setlocal
cd /d "%~dp0"
if not exist "HogwartsLegacy.exe" (
  echo ERRO: extraia este zip DENTRO da pasta Phoenix\Binaries\Win64 do jogo
  echo        ^(onde fica o HogwartsLegacy.exe^) e rode de novo.
  pause & exit /b 1
)
if exist "cbmp-backup" (
  echo Ja instalado. Rode DESINSTALAR.bat antes de instalar de novo.
  pause & exit /b 1
)
mkdir "cbmp-backup"
if exist "dwmapi.dll" move "dwmapi.dll" "cbmp-backup\dwmapi.dll" >nul
if exist "ue4ss" move "ue4ss" "cbmp-backup\ue4ss" >nul
xcopy /e /i /y "payload\*" "." >nul
echo Instalado. Abra o jogo, carregue um save, ande 10 s, aperte F8 duas vezes e saia.
echo Depois mande: ue4ss\Mods\cb-mp\cb-mp.log  (e qualquer .dmp na mesma pasta)
pause
```

`package/DESINSTALAR.bat`:
```bat
@echo off
setlocal
cd /d "%~dp0"
if not exist "cbmp-backup" ( echo Nada para desinstalar. & pause & exit /b 0 )
if exist "ue4ss\Mods\cb-mp\cb-mp.log" copy /y "ue4ss\Mods\cb-mp\cb-mp.log" "cb-mp-ultimo.log" >nul
if exist "dwmapi.dll" del /q "dwmapi.dll"
if exist "ue4ss" rmdir /s /q "ue4ss"
if exist "cbmp-backup\dwmapi.dll" move "cbmp-backup\dwmapi.dll" "dwmapi.dll" >nul
if exist "cbmp-backup\ue4ss" move "cbmp-backup\ue4ss" "ue4ss" >nul
rmdir /s /q "cbmp-backup"
echo Desinstalado. Log salvo em cb-mp-ultimo.log
pause
```

`package/LEIA-ME.txt`:
```
CB-MP spike 0.1.0 - teste de injeção (Castelo Bruxo RP)

1. Feche o jogo.
2. Ache a pasta do jogo:
   Steam: biblioteca > botão direito no jogo > Gerenciar > Procurar arquivos locais
   Epic:  biblioteca > ... > Gerenciar > ícone de pasta
   Entre em Phoenix\Binaries\Win64 (tem o HogwartsLegacy.exe).
3. Extraia TODO o conteúdo deste zip ali dentro.
4. Rode INSTALAR.bat.
5. Abra o jogo, carregue um save.
6. Ande uns 10 segundos.
7. Aperte F8 (deve aparecer um clone do bruxo 2 m à frente). Espere 5 s.
8. Aperte F8 de novo (clone some). Saia do jogo.
9. Mande o arquivo Phoenix\Binaries\Win64\ue4ss\Mods\cb-mp\cb-mp.log
   (e qualquer arquivo .dmp que estiver na mesma pasta).
10. Para remover tudo: DESINSTALAR.bat.

Uma janela de console do UE4SS pode abrir junto com o jogo. É normal.
```

- [ ] **Step 2: Script de empacotamento**

`tools/package-spike.ps1`:
```powershell
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\mod"
$assets = Join-Path $root "client\deps\RE-UE4SS\assets"
$pkg = Join-Path $root "package"
$stage = Join-Path $root "dist\stage"
$zip = Join-Path $root "dist\cb-mp-spike.zip"

function Find-One([string]$name) {
    $hits = Get-ChildItem $build -Recurse -Filter $name | Where-Object { $_.FullName -match "Game__Shipping__Win64" }
    if ($hits.Count -ne 1) { throw "esperava 1 '$name' em $build, achei $($hits.Count): $($hits.FullName -join '; ')" }
    return $hits[0].FullName
}

$ue4ss = Find-One "UE4SS.dll"
$proxy = Find-One "dwmapi.dll"
$mod = Find-One "cb-mp.dll"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$p = Join-Path $stage "payload"
New-Item -ItemType Directory -Force "$p\ue4ss\Mods\cb-mp\dlls" | Out-Null
Copy-Item $proxy "$p\dwmapi.dll"
Copy-Item $ue4ss "$p\ue4ss\UE4SS.dll"
Copy-Item (Join-Path $assets "UE4SS-settings.ini") "$p\ue4ss\UE4SS-settings.ini"
Copy-Item (Join-Path $pkg "mods.txt") "$p\ue4ss\Mods\mods.txt"
Copy-Item $mod "$p\ue4ss\Mods\cb-mp\dlls\main.dll"
New-Item -ItemType File "$p\ue4ss\Mods\cb-mp\enabled.txt" | Out-Null
Copy-Item (Join-Path $pkg "INSTALAR.bat"), (Join-Path $pkg "DESINSTALAR.bat"), (Join-Path $pkg "LEIA-ME.txt") $stage

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "Pacote: $zip"
Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
```

- [ ] **Step 3: Gerar e conferir**

Run: `pwsh -File tools/package-spike.ps1`
Expected: `Pacote: ...\dist\cb-mp-spike.zip` e a lista contém exatamente os 9 arquivos do layout acima.

- [ ] **Step 4: Checkpoint** — zip pronto para enviar ao Robson.

---

### Task 6: Rodada de teste no PC do jogo (Robson) e análise

**Files:**
- Create: `docs/spike-resultados.md` (registro do resultado)

**Interfaces:**
- Consumes: `dist/cb-mp-spike.zip`.
- Produces: veredito do spike por item do critério de sucesso.

- [ ] **Step 1 (Robson):** seguir `LEIA-ME.txt` no PC do jogo; mandar `cb-mp.log` (+ `.dmp` se houver) e dizer se o clone apareceu e como ficou (bruxo completo, boneco sem roupa, invisível, nada).

- [ ] **Step 2: Conferir o log contra o critério**

| Critério | Linha esperada no log |
|---|---|
| Loja, hash, UE4SS | `loja: Steam` ou `loja: Epic`, `sha256 exe: <64 hex>`, `UE4SS commit: 4fb9...` |
| Player achado | `player encontrado: <Classe> \| <nome completo>` |
| Posição 1x/s mudando | linhas `pos=(...)` ~1 por segundo, valores mudando enquanto anda |
| F8 cria | `F8: clone criado em pos=(...) -> <Classe> \| ...` |
| F8 remove | `F8: clone removido` |
| Sem crash | nenhuma linha `CRASH`, nenhum `.dmp` |

- [ ] **Step 3: Registrar em `docs/spike-resultados.md`**

```markdown
# Spike CB-MP — resultados

Data do teste: AAAA-MM-DD
Loja testada: Steam | Epic
Hash do exe: <sha256>

| Critério | Resultado | Evidência (linha do log) |
|---|---|---|
| Loja/hash/UE4SS no log | ok/falhou | ... |
| Posição 1x/s | ok/falhou | ... |
| F8 cria clone | ok/falhou/parcial (descrever aparência) | ... |
| F8 remove clone | ok/falhou | ... |
| Sem crash | ok/falhou | ... |

Classe do PlayerController: <da linha "PlayerController descoberto"> 
Classe do pawn: <da linha "player encontrado">
Próximos passos: ...
```

Se algum item falhar: corrigir em `client/src/game/Ue4ssGame.cpp` (único arquivo que conhece o jogo), gerar novo zip (Task 5 Step 3) e repetir esta task. A outra loja (Steam/Epic) fica registrada como pendente se não houver cópia para testar.

- [ ] **Step 4: Checkpoint final** — spike aprovado ou lista objetiva do que falta; base para a spec do Subprojeto 2 (netcode).
