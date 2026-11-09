# Subprojeto 2 — `cb-server` + `cbmp-proto` + `cb-bots` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Servidor de jogo C++ autenticado por ticket do Hub, com sync de movimento autoritativo e streaming por distância, HTTP de resources, heartbeat no Hub, bots de carga e CI que barra regressão de desempenho (256 bots / 2 vCPU).

**Architecture:** `proto/` (lib compartilhada: bitstream, mensagens, quantização, ticket) → `server/` (lib `cbserver_core` testável + exe `cb-server`) → `bots/` (exe). Simulação em 1 thread dona do mundo; montagem de snapshots em pool de threads; rede em GameNetworkingSockets; HTTP em cpp-httplib. Tudo injetável por interfaces finas para testes sem rede.

**Tech Stack:** C++23, CMake ≥ 3.25 + presets, vcpkg manifest (baseline `ec48c11eae9448c8c6e7739d93e06fa2433aa95f`), GameNetworkingSockets 1.6.1, libsodium 1.0.22, cpp-httplib 0.65 (feature `openssl`), nlohmann-json 3.12, spdlog 1.17, Catch2 3.16. GCC ≥ 13 (Linux), MSVC VS 2026 (Windows). CI: GitHub Actions (ubuntu-24.04 2 vCPU, windows-2025).

**Nota sobre o nível de detalhe:** as peças de contrato e de lógica pura (bitstream, mensagens, quantização, base64url, grade, visibilidade, token bucket, validação de movimento, manifest, pool) vêm com código completo. As peças de integração com bibliotecas grandes (rede GNS, HTTP, heartbeat, bots) vêm com interface exata, comportamento exato e testes completos; o implementador escreve o corpo contra a API real da versão fixada.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-11-09-sub2-cb-server-design.md` (fonte da verdade) e `2026-11-09-plataforma-design.md`.
- Repo `robsonsaldanha/base-cb-multiplayer`. Branch `sub2-server`; commit por task (mensagem termina com linha em branco + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); sem push sem OK; nunca tocar `castelobruxo/*`; não ler/copiar código MafiaHub/arcane-mp/castelo-mp.
- Validação de ticket em C++ deve passar em **todos** os casos de `packages/contracts/test-vectors/tickets.json` e seguir a mesma ordem de checagens do `packages/contracts/src/ticket.ts`.
- Protocolo: `kProtocolVersion = 1`; bitstream LSB-first; inteiros little-endian; posição int32 cm; yaw u16; pitch u8; velocidade int16 cm/s; estado u4.
- Porta padrão 30500 (UDP jogo, TCP HTTP). Tick 20 Hz. Raio 300 m / saída 330 m / teto 128 / células 64 m / frequência <100 m 20 Hz, <200 m 10 Hz, resto 5 Hz.
- Segurança: nada alocado por cliente antes do ticket válido (só registro pendente pequeno); `Hello` em ≤ 5 s; `Hello` ≤ 4 KiB; pendentes por IP 4, global 256; uma sessão por `sub`; jti não reutilizável até `exp+60`.
- Limites do CI de carga: 256 bots, 120 s, 30% aglomerados; falha se tick p99 > 25 ms, banda média/jogador > 64 kbps, RSS > 512 MB, queda de bot, ou bot > 1 s sem snapshot.
- Nada de código nativo servido por HTTP (`.dll .exe .so .dylib .bat .cmd .ps1 .sh .msi .scr .com .jar .node`).
- Textos de log em pt-BR curtos; identificadores de código em inglês.

## Estrutura de arquivos

```
CMakeLists.txt  CMakePresets.json  vcpkg.json        raiz do build nativo (proto/server/bots; client/ continua separado)
.github/workflows/native.yml                         CI: build+testes Linux/Windows, fuzz curto, carga
proto/
  CMakeLists.txt
  include/cbmp/bitstream.hpp   src/bitstream.cpp
  include/cbmp/quantize.hpp
  include/cbmp/messages.hpp    src/messages.cpp
  include/cbmp/base64url.hpp   src/base64url.cpp
  include/cbmp/ticket.hpp      src/ticket.cpp
  tests/  test_bitstream.cpp test_messages.cpp test_base64url.cpp test_ticket.cpp
  fuzz/   proto_fuzz.cpp
server/
  CMakeLists.txt
  include/cbserver/config.hpp      src/config.cpp
  include/cbserver/grid.hpp        src/grid.cpp
  include/cbserver/visibility.hpp  src/visibility.cpp
  include/cbserver/rate.hpp
  include/cbserver/movement.hpp    src/movement.cpp
  include/cbserver/world.hpp       src/world.cpp          estado do mundo + tick (sem rede)
  include/cbserver/snapshots.hpp   src/snapshots.cpp      montagem paralela
  include/cbserver/pool.hpp        src/pool.cpp
  include/cbserver/metrics.hpp     src/metrics.cpp
  include/cbserver/manifest.hpp    src/manifest.cpp
  include/cbserver/auth.hpp        src/auth.cpp           chaves do Hub/dev + jti cache
  include/cbserver/http.hpp        src/http.cpp
  include/cbserver/hub.hpp         src/hub.cpp            heartbeat + fetch de chaves
  include/cbserver/net.hpp         src/net.cpp            GNS
  src/main.cpp
  tests/ ...
  server.cfg.example
bots/
  CMakeLists.txt  src/main.cpp  src/bot.cpp  src/bot.hpp
docs/LOAD-TEST.md
```

---

### Task 1: Esqueleto do build nativo + CI de build

**Files:** Create `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `proto/CMakeLists.txt`, `proto/tests/test_smoke.cpp`, `.github/workflows/native.yml`; Modify `.gitignore` (`build-native/`, `vcpkg_installed/`).

**Interfaces:** Produces: target `cbmp_proto` (STATIC), `cbmp_proto_tests` (Catch2), presets `linux-release`, `linux-asan`, `windows-release`; option `CBMP_BUILD_FUZZ`.

- [ ] **Step 1: Branch**

```bash
git checkout main && git pull --ff-only && git checkout -b sub2-server
```

- [ ] **Step 2: `vcpkg.json`**

```json
{
  "name": "cbmp-native",
  "version-string": "0.1.0",
  "builtin-baseline": "ec48c11eae9448c8c6e7739d93e06fa2433aa95f",
  "dependencies": [
    "gamenetworkingsockets",
    "libsodium",
    { "name": "cpp-httplib", "features": ["openssl"] },
    "nlohmann-json",
    "spdlog",
    "catch2"
  ]
}
```

- [ ] **Step 3: `CMakeLists.txt` (raiz)**

```cmake
cmake_minimum_required(VERSION 3.25)
project(cbmp_native LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(CBMP_BUILD_FUZZ "Alvo de fuzz (clang/libFuzzer)" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_subdirectory(proto)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/server/CMakeLists.txt)
  add_subdirectory(server)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bots/CMakeLists.txt)
  add_subdirectory(bots)
endif()
```

- [ ] **Step 4: `CMakePresets.json`**

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "base", "hidden": true, "binaryDir": "${sourceDir}/build-native/${presetName}",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" },
    { "name": "linux-release", "inherits": "base", "generator": "Ninja",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } },
    { "name": "linux-asan", "inherits": "base", "generator": "Ninja",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer" } },
    { "name": "linux-fuzz", "inherits": "base", "generator": "Ninja",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo", "CMAKE_CXX_COMPILER": "clang++", "CBMP_BUILD_FUZZ": "ON" } },
    { "name": "windows-release", "inherits": "base", "generator": "Visual Studio 18 2026", "architecture": "x64" }
  ],
  "buildPresets": [
    { "name": "linux-release", "configurePreset": "linux-release" },
    { "name": "linux-asan", "configurePreset": "linux-asan" },
    { "name": "linux-fuzz", "configurePreset": "linux-fuzz" },
    { "name": "windows-release", "configurePreset": "windows-release", "configuration": "Release" }
  ]
}
```

- [ ] **Step 5: `proto/CMakeLists.txt` mínimo + smoke test**

```cmake
add_library(cbmp_proto STATIC)
target_include_directories(cbmp_proto PUBLIC include)
target_sources(cbmp_proto PRIVATE src/bitstream.cpp)
add_executable(cbmp_proto_tests tests/test_smoke.cpp)
target_link_libraries(cbmp_proto_tests PRIVATE cbmp_proto Catch2::Catch2WithMain)
catch_discover_tests(cbmp_proto_tests)
```
`proto/src/bitstream.cpp` (provisório, substituído na Task 2): `// placeholder` + `namespace cbmp {}`.
`proto/tests/test_smoke.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
TEST_CASE("smoke") { REQUIRE(1 + 1 == 2); }
```

- [ ] **Step 6: Build local Windows**

```powershell
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg"
$CMAKE = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $CMAKE --preset windows-release
& $CMAKE --build --preset windows-release
& (Join-Path (Split-Path $CMAKE) ctest.exe) --test-dir build-native/windows-release -C Release --output-on-failure
```
Expected: vcpkg instala dependências (primeira vez demora — rodar em background), `100% tests passed`.

- [ ] **Step 7: CI `.github/workflows/native.yml` (build + testes; carga e fuzz entram na Task 8)**

```yaml
name: native
on:
  push:
    paths: ['proto/**', 'server/**', 'bots/**', 'CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'packages/contracts/test-vectors/**', '.github/workflows/native.yml']
  pull_request:
    paths: ['proto/**', 'server/**', 'bots/**', 'CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'packages/contracts/test-vectors/**', '.github/workflows/native.yml']
concurrency: { group: native-${{ github.ref }}, cancel-in-progress: true }
env:
  VCPKG_BINARY_SOURCES: "clear;files,${{ github.workspace }}/.vcpkg-cache,readwrite"
jobs:
  linux:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/cache@v4
        with: { path: .vcpkg-cache, key: vcpkg-linux-${{ hashFiles('vcpkg.json') }}, restore-keys: vcpkg-linux- }
      - run: sudo apt-get update && sudo apt-get install -y ninja-build
      - run: mkdir -p .vcpkg-cache && echo "VCPKG_ROOT=$VCPKG_INSTALLATION_ROOT" >> $GITHUB_ENV
      - run: cmake --preset linux-asan && cmake --build --preset linux-asan
      - run: ctest --test-dir build-native/linux-asan --output-on-failure
  windows:
    runs-on: windows-2025
    steps:
      - uses: actions/checkout@v4
      - uses: actions/cache@v4
        with: { path: .vcpkg-cache, key: vcpkg-win-${{ hashFiles('vcpkg.json') }}, restore-keys: vcpkg-win- }
      - run: |
          New-Item -ItemType Directory -Force .vcpkg-cache | Out-Null
          "VCPKG_ROOT=$env:VCPKG_INSTALLATION_ROOT" | Out-File -FilePath $env:GITHUB_ENV -Append
        shell: pwsh
      - run: cmake --preset windows-release -G "Visual Studio 17 2022" && cmake --build --preset windows-release
        shell: pwsh
      - run: ctest --test-dir build-native/windows-release -C Release --output-on-failure
        shell: pwsh
```
(Runner Windows tem VS 2022; o preset local usa VS 2026 — o `-G` sobrescreve só no CI. Se o configure reclamar de generator diferente do preset, criar preset `windows-ci` com "Visual Studio 17 2022".)

- [ ] **Step 8: Commit** — `git add CMakeLists.txt CMakePresets.json vcpkg.json proto .github .gitignore && git commit -m "build(native): esqueleto CMake/vcpkg/CI para proto, server e bots"`

---

### Task 2: Bitstream, quantização e mensagens

**Files:** Create `proto/include/cbmp/{bitstream,quantize,messages}.hpp`, `proto/src/{bitstream,messages}.cpp`, `proto/tests/{test_bitstream,test_messages}.cpp`; Modify `proto/CMakeLists.txt` (fontes/testes); remove `test_smoke.cpp`.

**Interfaces — Produces** (`namespace cbmp`):
- `struct DecodeError : std::runtime_error`
- `class BitWriter { void writeBits(uint64_t v, unsigned bits); void writeU8/U16/U32/U64; void writeI16/I32; void writeBool; void writeString(std::string_view, unsigned lenBits, size_t maxLen); void writeBlob(std::span<const uint8_t>, unsigned lenBits, size_t maxLen); std::vector<uint8_t> finish(); size_t bitSize() const; }`
- `class BitReader { explicit BitReader(std::span<const uint8_t>); uint64_t readBits(unsigned); readU8/U16/U32/U64/I16/I32/Bool; std::string readString(unsigned lenBits, size_t maxLen); std::vector<uint8_t> readBlob(unsigned lenBits, size_t maxLen); size_t remainingBits() const; void expectEnd() const; }`
- quantize: `uint16_t quantizeYaw(float deg)`, `float dequantizeYaw(uint16_t)`, `uint8_t quantizePitch(float deg)`, `float dequantizePitch(uint8_t)`
- messages: `kProtocolVersion = 1`, `enum class MsgType : uint8_t`, `enum class MoveState : uint8_t`, `struct Vec3i/Vec3s/PlayerState`, message structs, `using Message = std::variant<...>`, `std::vector<uint8_t> encode(const Message&)`, `Message decode(std::span<const uint8_t>)`, `MsgType typeOf(const Message&)`, limits `kMaxTicketLen = 2048`, `kMaxEventName = 64`, `kMaxEventPayload = 4096`, `kMaxSnapshotEntities = 256`, `kMaxReason = 255`, `kMaxName = 64`.

- [ ] **Step 1: Testes (falhando)** — `proto/tests/test_bitstream.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cbmp/bitstream.hpp>
#include <cbmp/quantize.hpp>
using namespace cbmp;

TEST_CASE("bits ida e volta com larguras variadas") {
  BitWriter w;
  w.writeBits(0b101, 3); w.writeBool(true); w.writeU16(0xBEEF); w.writeI32(-123456); w.writeBits(0x1F, 5); w.writeU64(0x0123456789ABCDEFull);
  auto bytes = w.finish();
  BitReader r(bytes);
  CHECK(r.readBits(3) == 0b101); CHECK(r.readBool()); CHECK(r.readU16() == 0xBEEF);
  CHECK(r.readI32() == -123456); CHECK(r.readBits(5) == 0x1F); CHECK(r.readU64() == 0x0123456789ABCDEFull);
}
TEST_CASE("LSB-first: primeiro bit escrito é o bit 0 do byte 0") {
  BitWriter w; w.writeBits(1, 1); w.writeBits(0, 7);
  auto b = w.finish(); REQUIRE(b.size() == 1); CHECK(b[0] == 0x01);
}
TEST_CASE("leitura além do fim lança DecodeError") {
  std::vector<uint8_t> one{0xFF}; BitReader r(one);
  r.readBits(8); CHECK_THROWS_AS(r.readBits(1), DecodeError);
}
TEST_CASE("string respeita maxLen na escrita e na leitura") {
  BitWriter w; w.writeString("abc", 8, 255); auto b = w.finish();
  BitReader r(b); CHECK(r.readString(8, 255) == "abc");
  BitWriter w2; CHECK_THROWS(w2.writeString(std::string(300, 'x'), 8, 255));
  BitWriter w3; w3.writeBits(10, 8); w3.writeBits(0, 80); auto b3 = w3.finish();
  BitReader r3(b3); CHECK_THROWS_AS(r3.readString(8, 5), DecodeError);
}
TEST_CASE("expectEnd tolera só bits de preenchimento zero") {
  BitWriter w; w.writeBits(3, 2); auto b = w.finish();
  BitReader r(b); r.readBits(2); CHECK_NOTHROW(r.expectEnd());
  std::vector<uint8_t> extra{0x03, 0x01}; BitReader r2(extra); r2.readBits(2); CHECK_THROWS_AS(r2.expectEnd(), DecodeError);
}
TEST_CASE("quantização de yaw/pitch") {
  CHECK(quantizeYaw(0.f) == 0); CHECK(quantizeYaw(360.f) == 0); CHECK(quantizeYaw(-90.f) == quantizeYaw(270.f));
  CHECK(std::abs(dequantizeYaw(quantizeYaw(123.4f)) - 123.4f) < 0.01f);
  CHECK(quantizePitch(-90.f) == 0); CHECK(quantizePitch(90.f) == 255);
  CHECK(std::abs(dequantizePitch(quantizePitch(10.f)) - 10.f) < 0.8f);
  CHECK(quantizePitch(200.f) == 255);
}
```

`proto/tests/test_messages.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <cbmp/messages.hpp>
using namespace cbmp;

static PlayerState ps(int x) { return PlayerState{{x, -x, 1000}, 12345, 128, {100, -200, 0}, MoveState::Run}; }
template <class T> static T roundtrip(const T& m) { return std::get<T>(decode(encode(Message{m}))); }

TEST_CASE("Hello/Welcome/Reject ida e volta") {
  Hello h{kProtocolVersion, {}, "abc.def"}; h.buildHash[0] = 7; h.buildHash[31] = 9;
  auto h2 = roundtrip(h); CHECK(h2.protocol == 1); CHECK(h2.buildHash == h.buildHash); CHECK(h2.ticket == "abc.def");
  auto w = roundtrip(Welcome{42, 20, 1'800'000'000'000ull}); CHECK(w.playerId == 42); CHECK(w.tickRateHz == 20); CHECK(w.serverTimeMs == 1'800'000'000'000ull);
  CHECK(roundtrip(Reject{"ticket inválido"}).reason == "ticket inválido");
}
TEST_CASE("Input ida e volta preserva todos os campos") {
  auto i = roundtrip(Input{77, ps(5000)}); CHECK(i.seq == 77); CHECK(i.s == ps(5000));
}
TEST_CASE("Snapshot só carrega campos da máscara") {
  Snapshot s{100, 77, 98, {}};
  s.ents.push_back({1, kMaskAll, ps(1)});
  s.ents.push_back({2, kMaskPos, ps(2)});
  auto bytesFull = encode(Message{s}).size();
  auto d = roundtrip(s);
  REQUIRE(d.ents.size() == 2);
  CHECK(d.ents[0].mask == kMaskAll); CHECK(d.ents[0].s == ps(1));
  CHECK(d.ents[1].mask == kMaskPos); CHECK(d.ents[1].s.pos == ps(2).pos);
  Snapshot only{100, 77, 98, {{2, kMaskPos, ps(2)}}};
  CHECK(encode(Message{only}).size() < bytesFull);
}
TEST_CASE("Snapshot recusa máscara zero e entidades demais") {
  Snapshot s{1, 1, 1, {{1, 0, ps(1)}}};
  CHECK_THROWS(encode(Message{s}));
  Snapshot big{1, 1, 1, std::vector<SnapshotEntity>(kMaxSnapshotEntities + 1, SnapshotEntity{1, kMaskPos, ps(1)})};
  CHECK_THROWS(encode(Message{big}));
}
TEST_CASE("Event, Correction, Kick, EntityEnter/Leave, SnapshotAck") {
  auto e = roundtrip(Event{"chat", {1, 2, 3}}); CHECK(e.name == "chat"); CHECK(e.payload == std::vector<uint8_t>{1, 2, 3});
  auto c = roundtrip(Correction{9, {1, 2, 3}, {4, 5, 6}}); CHECK(c.seq == 9); CHECK(c.pos == Vec3i{1, 2, 3}); CHECK(c.vel == Vec3s{4, 5, 6});
  CHECK(roundtrip(Kick{"flood"}).reason == "flood");
  auto en = roundtrip(EntityEnter{3, ps(7), "Jogador"}); CHECK(en.id == 3); CHECK(en.s == ps(7)); CHECK(en.name == "Jogador");
  CHECK(roundtrip(EntityLeave{3}).id == 3);
  CHECK(roundtrip(SnapshotAck{55}).tick == 55);
}
TEST_CASE("decode recusa lixo") {
  CHECK_THROWS_AS(decode(std::vector<uint8_t>{}), DecodeError);
  CHECK_THROWS_AS(decode(std::vector<uint8_t>{0xEE}), DecodeError);         // tipo desconhecido
  auto b = encode(Message{Input{1, ps(1)}}); b.pop_back();
  CHECK_THROWS_AS(decode(b), DecodeError);                                   // truncado
  auto ok = encode(Message{Kick{"x"}}); ok.push_back(0xFF);
  CHECK_THROWS_AS(decode(ok), DecodeError);                                  // sobra
  BitWriter w; w.writeU8(static_cast<uint8_t>(MsgType::Input)); w.writeU32(1);
  w.writeI32(0); w.writeI32(0); w.writeI32(0); w.writeU16(0); w.writeU8(0); w.writeI16(0); w.writeI16(0); w.writeI16(0);
  w.writeBits(9, 4);                                                         // estado reservado
  CHECK_THROWS_AS(decode(w.finish()), DecodeError);
}
```

Run: build + ctest → FAIL (headers inexistentes).

- [ ] **Step 2: `bitstream.hpp/.cpp`**

```cpp
// proto/include/cbmp/bitstream.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cbmp {
struct DecodeError : std::runtime_error { using std::runtime_error::runtime_error; };

// Bitstream LSB-first: o primeiro bit escrito é o bit 0 do byte 0.
class BitWriter {
public:
  void writeBits(uint64_t value, unsigned bits);
  void writeU8(uint8_t v) { writeBits(v, 8); }
  void writeU16(uint16_t v) { writeBits(v, 16); }
  void writeU32(uint32_t v) { writeBits(v, 32); }
  void writeU64(uint64_t v) { writeBits(v, 64); }
  void writeI16(int16_t v) { writeBits(static_cast<uint16_t>(v), 16); }
  void writeI32(int32_t v) { writeBits(static_cast<uint32_t>(v), 32); }
  void writeBool(bool b) { writeBits(b ? 1 : 0, 1); }
  void writeString(std::string_view s, unsigned lenBits, size_t maxLen);
  void writeBlob(std::span<const uint8_t> b, unsigned lenBits, size_t maxLen);
  std::vector<uint8_t> finish();
  size_t bitSize() const { return bits_; }
private:
  std::vector<uint8_t> buf_;
  size_t bits_ = 0;
};

class BitReader {
public:
  explicit BitReader(std::span<const uint8_t> data) : data_(data) {}
  uint64_t readBits(unsigned bits);
  uint8_t readU8() { return static_cast<uint8_t>(readBits(8)); }
  uint16_t readU16() { return static_cast<uint16_t>(readBits(16)); }
  uint32_t readU32() { return static_cast<uint32_t>(readBits(32)); }
  uint64_t readU64() { return readBits(64); }
  int16_t readI16() { return static_cast<int16_t>(readU16()); }
  int32_t readI32() { return static_cast<int32_t>(readU32()); }
  bool readBool() { return readBits(1) != 0; }
  std::string readString(unsigned lenBits, size_t maxLen);
  std::vector<uint8_t> readBlob(unsigned lenBits, size_t maxLen);
  size_t remainingBits() const { return data_.size() * 8 - pos_; }
  // Só bits de preenchimento (zero) até o fim do byte atual; nada depois.
  void expectEnd() const;
private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};
}  // namespace cbmp
```

```cpp
// proto/src/bitstream.cpp
#include <cbmp/bitstream.hpp>

namespace cbmp {
void BitWriter::writeBits(uint64_t value, unsigned bits) {
  if (bits == 0 || bits > 64) throw std::invalid_argument("bits fora de 1..64");
  if (bits < 64) value &= (uint64_t{1} << bits) - 1;
  for (unsigned i = 0; i < bits; ++i, ++bits_) {
    if ((bits_ & 7) == 0) buf_.push_back(0);
    if ((value >> i) & 1) buf_.back() |= static_cast<uint8_t>(1u << (bits_ & 7));
  }
}
void BitWriter::writeString(std::string_view s, unsigned lenBits, size_t maxLen) {
  if (s.size() > maxLen || (lenBits < 64 && s.size() >= (size_t{1} << lenBits))) throw std::length_error("string grande demais");
  writeBits(s.size(), lenBits);
  for (unsigned char c : s) writeU8(c);
}
void BitWriter::writeBlob(std::span<const uint8_t> b, unsigned lenBits, size_t maxLen) {
  if (b.size() > maxLen || (lenBits < 64 && b.size() >= (size_t{1} << lenBits))) throw std::length_error("blob grande demais");
  writeBits(b.size(), lenBits);
  for (uint8_t c : b) writeU8(c);
}
std::vector<uint8_t> BitWriter::finish() { return std::move(buf_); }

uint64_t BitReader::readBits(unsigned bits) {
  if (bits == 0 || bits > 64) throw DecodeError("bits fora de 1..64");
  if (bits > remainingBits()) throw DecodeError("fim inesperado");
  uint64_t v = 0;
  for (unsigned i = 0; i < bits; ++i, ++pos_) {
    if ((data_[pos_ >> 3] >> (pos_ & 7)) & 1) v |= uint64_t{1} << i;
  }
  return v;
}
std::string BitReader::readString(unsigned lenBits, size_t maxLen) {
  const auto n = readBits(lenBits);
  if (n > maxLen) throw DecodeError("string grande demais");
  if (n * 8 > remainingBits()) throw DecodeError("fim inesperado");
  std::string s(static_cast<size_t>(n), '\0');
  for (auto& c : s) c = static_cast<char>(readU8());
  return s;
}
std::vector<uint8_t> BitReader::readBlob(unsigned lenBits, size_t maxLen) {
  const auto n = readBits(lenBits);
  if (n > maxLen) throw DecodeError("blob grande demais");
  if (n * 8 > remainingBits()) throw DecodeError("fim inesperado");
  std::vector<uint8_t> b(static_cast<size_t>(n));
  for (auto& c : b) c = readU8();
  return b;
}
void BitReader::expectEnd() const {
  const size_t endOfByte = (pos_ + 7) & ~size_t{7};
  if (endOfByte != data_.size() * 8) throw DecodeError("bytes sobrando");
  for (size_t p = pos_; p < endOfByte; ++p)
    if ((data_[p >> 3] >> (p & 7)) & 1) throw DecodeError("preenchimento não-zero");
}
}  // namespace cbmp
```

- [ ] **Step 3: `quantize.hpp`**

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cbmp {
inline uint16_t quantizeYaw(float deg) {
  double d = std::fmod(static_cast<double>(deg), 360.0);
  if (d < 0) d += 360.0;
  return static_cast<uint16_t>(static_cast<uint32_t>(std::lround(d / 360.0 * 65536.0)) & 0xFFFFu);
}
inline float dequantizeYaw(uint16_t q) { return static_cast<float>(q) * 360.0f / 65536.0f; }
inline uint8_t quantizePitch(float deg) {
  const double c = std::clamp(static_cast<double>(deg), -90.0, 90.0);
  return static_cast<uint8_t>(std::lround((c + 90.0) / 180.0 * 255.0));
}
inline float dequantizePitch(uint8_t q) { return static_cast<float>(q) * 180.0f / 255.0f - 90.0f; }
}  // namespace cbmp
```

- [ ] **Step 4: `messages.hpp/.cpp`**

```cpp
// proto/include/cbmp/messages.hpp
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <cbmp/bitstream.hpp>

namespace cbmp {
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr size_t kMaxTicketLen = 2048, kMaxEventName = 64, kMaxEventPayload = 4096,
                        kMaxSnapshotEntities = 256, kMaxReason = 255, kMaxName = 64;

enum class MsgType : uint8_t {
  Hello = 1, Welcome = 2, Reject = 3,
  Input = 10, Snapshot = 11, SnapshotAck = 12, EntityEnter = 13, EntityLeave = 14,
  Event = 20, Correction = 21, Kick = 22,
};
enum class MoveState : uint8_t { Idle = 0, Walk = 1, Run = 2, Broom = 3, Swim = 4, Fall = 5, Apparate = 6 };
inline constexpr uint8_t kMaxMoveState = 6;

struct Vec3i { int32_t x = 0, y = 0, z = 0; bool operator==(const Vec3i&) const = default; };
struct Vec3s { int16_t x = 0, y = 0, z = 0; bool operator==(const Vec3s&) const = default; };
struct PlayerState {
  Vec3i pos; uint16_t yaw = 0; uint8_t pitch = 128; Vec3s vel; MoveState state = MoveState::Idle;
  bool operator==(const PlayerState&) const = default;
};

inline constexpr uint8_t kMaskPos = 1, kMaskRot = 2, kMaskVel = 4, kMaskState = 8, kMaskAll = 15;

struct Hello { uint32_t protocol = kProtocolVersion; std::array<uint8_t, 32> buildHash{}; std::string ticket; };
struct Welcome { uint16_t playerId = 0; uint8_t tickRateHz = 20; uint64_t serverTimeMs = 0; };
struct Reject { std::string reason; };
struct Input { uint32_t seq = 0; PlayerState s; };
struct SnapshotEntity { uint16_t id = 0; uint8_t mask = 0; PlayerState s; };
struct Snapshot { uint32_t tick = 0, ackInputSeq = 0, baselineTick = 0; std::vector<SnapshotEntity> ents; };
struct SnapshotAck { uint32_t tick = 0; };
struct EntityEnter { uint16_t id = 0; PlayerState s; std::string name; };
struct EntityLeave { uint16_t id = 0; };
struct Event { std::string name; std::vector<uint8_t> payload; };
struct Correction { uint32_t seq = 0; Vec3i pos; Vec3s vel; };
struct Kick { std::string reason; };

using Message = std::variant<Hello, Welcome, Reject, Input, Snapshot, SnapshotAck, EntityEnter, EntityLeave, Event, Correction, Kick>;

std::vector<uint8_t> encode(const Message& m);
Message decode(std::span<const uint8_t> bytes);   // lança DecodeError
MsgType typeOf(const Message& m);
bool isReliable(MsgType t);                        // Input/Snapshot/SnapshotAck = não confiável
}  // namespace cbmp
```

```cpp
// proto/src/messages.cpp
#include <cbmp/messages.hpp>
#include <stdexcept>

namespace cbmp {
namespace {
void putVec3i(BitWriter& w, const Vec3i& v) { w.writeI32(v.x); w.writeI32(v.y); w.writeI32(v.z); }
void putVec3s(BitWriter& w, const Vec3s& v) { w.writeI16(v.x); w.writeI16(v.y); w.writeI16(v.z); }
Vec3i getVec3i(BitReader& r) { Vec3i v; v.x = r.readI32(); v.y = r.readI32(); v.z = r.readI32(); return v; }
Vec3s getVec3s(BitReader& r) { Vec3s v; v.x = r.readI16(); v.y = r.readI16(); v.z = r.readI16(); return v; }
MoveState getState(BitReader& r) {
  auto s = static_cast<uint8_t>(r.readBits(4));
  if (s > kMaxMoveState) throw DecodeError("estado reservado");
  return static_cast<MoveState>(s);
}
void putFull(BitWriter& w, const PlayerState& s) {
  putVec3i(w, s.pos); w.writeU16(s.yaw); w.writeU8(s.pitch); putVec3s(w, s.vel); w.writeBits(static_cast<uint8_t>(s.state), 4);
}
PlayerState getFull(BitReader& r) {
  PlayerState s; s.pos = getVec3i(r); s.yaw = r.readU16(); s.pitch = r.readU8(); s.vel = getVec3s(r); s.state = getState(r); return s;
}
template <class> inline constexpr bool always_false = false;
}  // namespace

MsgType typeOf(const Message& m) {
  return std::visit([](const auto& x) -> MsgType {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, Hello>) return MsgType::Hello;
    else if constexpr (std::is_same_v<T, Welcome>) return MsgType::Welcome;
    else if constexpr (std::is_same_v<T, Reject>) return MsgType::Reject;
    else if constexpr (std::is_same_v<T, Input>) return MsgType::Input;
    else if constexpr (std::is_same_v<T, Snapshot>) return MsgType::Snapshot;
    else if constexpr (std::is_same_v<T, SnapshotAck>) return MsgType::SnapshotAck;
    else if constexpr (std::is_same_v<T, EntityEnter>) return MsgType::EntityEnter;
    else if constexpr (std::is_same_v<T, EntityLeave>) return MsgType::EntityLeave;
    else if constexpr (std::is_same_v<T, Event>) return MsgType::Event;
    else if constexpr (std::is_same_v<T, Correction>) return MsgType::Correction;
    else if constexpr (std::is_same_v<T, Kick>) return MsgType::Kick;
    else static_assert(always_false<T>);
  }, m);
}

bool isReliable(MsgType t) { return !(t == MsgType::Input || t == MsgType::Snapshot || t == MsgType::SnapshotAck); }

std::vector<uint8_t> encode(const Message& m) {
  BitWriter w;
  w.writeU8(static_cast<uint8_t>(typeOf(m)));
  std::visit([&](const auto& x) {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, Hello>) {
      w.writeU32(x.protocol); for (auto b : x.buildHash) w.writeU8(b); w.writeString(x.ticket, 12, kMaxTicketLen);
    } else if constexpr (std::is_same_v<T, Welcome>) {
      w.writeU16(x.playerId); w.writeU8(x.tickRateHz); w.writeU64(x.serverTimeMs);
    } else if constexpr (std::is_same_v<T, Reject> || std::is_same_v<T, Kick>) {
      w.writeString(x.reason, 8, kMaxReason);
    } else if constexpr (std::is_same_v<T, Input>) {
      w.writeU32(x.seq); putFull(w, x.s);
    } else if constexpr (std::is_same_v<T, Snapshot>) {
      if (x.ents.size() > kMaxSnapshotEntities) throw std::length_error("entidades demais");
      w.writeU32(x.tick); w.writeU32(x.ackInputSeq); w.writeU32(x.baselineTick); w.writeBits(x.ents.size(), 9);
      for (const auto& e : x.ents) {
        if (e.mask == 0 || e.mask > kMaskAll) throw std::invalid_argument("máscara inválida");
        w.writeU16(e.id); w.writeBits(e.mask, 4);
        if (e.mask & kMaskPos) putVec3i(w, e.s.pos);
        if (e.mask & kMaskRot) { w.writeU16(e.s.yaw); w.writeU8(e.s.pitch); }
        if (e.mask & kMaskVel) putVec3s(w, e.s.vel);
        if (e.mask & kMaskState) w.writeBits(static_cast<uint8_t>(e.s.state), 4);
      }
    } else if constexpr (std::is_same_v<T, SnapshotAck>) {
      w.writeU32(x.tick);
    } else if constexpr (std::is_same_v<T, EntityEnter>) {
      w.writeU16(x.id); putFull(w, x.s); w.writeString(x.name, 7, kMaxName);
    } else if constexpr (std::is_same_v<T, EntityLeave>) {
      w.writeU16(x.id);
    } else if constexpr (std::is_same_v<T, Event>) {
      if (x.name.empty()) throw std::invalid_argument("evento sem nome");
      w.writeString(x.name, 7, kMaxEventName); w.writeBlob(x.payload, 13, kMaxEventPayload);
    } else if constexpr (std::is_same_v<T, Correction>) {
      w.writeU32(x.seq); putVec3i(w, x.pos); putVec3s(w, x.vel);
    }
  }, m);
  return w.finish();
}

Message decode(std::span<const uint8_t> bytes) {
  if (bytes.empty()) throw DecodeError("mensagem vazia");
  BitReader r(bytes);
  const auto type = static_cast<MsgType>(r.readU8());
  Message out;
  switch (type) {
    case MsgType::Hello: { Hello h; h.protocol = r.readU32(); for (auto& b : h.buildHash) b = r.readU8(); h.ticket = r.readString(12, kMaxTicketLen); out = std::move(h); break; }
    case MsgType::Welcome: { Welcome x; x.playerId = r.readU16(); x.tickRateHz = r.readU8(); x.serverTimeMs = r.readU64(); out = x; break; }
    case MsgType::Reject: out = Reject{r.readString(8, kMaxReason)}; break;
    case MsgType::Kick: out = Kick{r.readString(8, kMaxReason)}; break;
    case MsgType::Input: { Input x; x.seq = r.readU32(); x.s = getFull(r); out = x; break; }
    case MsgType::Snapshot: {
      Snapshot s; s.tick = r.readU32(); s.ackInputSeq = r.readU32(); s.baselineTick = r.readU32();
      const auto n = r.readBits(9);
      if (n > kMaxSnapshotEntities) throw DecodeError("entidades demais");
      s.ents.reserve(static_cast<size_t>(n));
      for (uint64_t i = 0; i < n; ++i) {
        SnapshotEntity e; e.id = r.readU16(); e.mask = static_cast<uint8_t>(r.readBits(4));
        if (e.mask == 0) throw DecodeError("máscara vazia");
        if (e.mask & kMaskPos) e.s.pos = getVec3i(r);
        if (e.mask & kMaskRot) { e.s.yaw = r.readU16(); e.s.pitch = r.readU8(); }
        if (e.mask & kMaskVel) e.s.vel = getVec3s(r);
        if (e.mask & kMaskState) e.s.state = getState(r);
        s.ents.push_back(e);
      }
      out = std::move(s); break;
    }
    case MsgType::SnapshotAck: out = SnapshotAck{r.readU32()}; break;
    case MsgType::EntityEnter: { EntityEnter x; x.id = r.readU16(); x.s = getFull(r); x.name = r.readString(7, kMaxName); out = std::move(x); break; }
    case MsgType::EntityLeave: out = EntityLeave{r.readU16()}; break;
    case MsgType::Event: {
      Event e; e.name = r.readString(7, kMaxEventName);
      if (e.name.empty()) throw DecodeError("evento sem nome");
      e.payload = r.readBlob(13, kMaxEventPayload); out = std::move(e); break;
    }
    case MsgType::Correction: { Correction c; c.seq = r.readU32(); c.pos = getVec3i(r); c.vel = getVec3s(r); out = c; break; }
    default: throw DecodeError("tipo desconhecido");
  }
  r.expectEnd();
  return out;
}
}  // namespace cbmp
```

Atualizar `proto/CMakeLists.txt`: fontes `src/bitstream.cpp src/messages.cpp`; teste `cbmp_proto_tests` com `tests/test_bitstream.cpp tests/test_messages.cpp`.

- [ ] **Step 5: Rodar testes** → todos PASS (Windows release + Linux asan no CI).
- [ ] **Step 6: Commit** — `feat(proto): bitstream LSB-first, quantização e mensagens do protocolo v1`

---

### Task 3: Base64url canônico + validação de ticket em C++

**Files:** Create `proto/include/cbmp/{base64url,ticket}.hpp`, `proto/src/{base64url,ticket}.cpp`, `proto/tests/{test_base64url,test_ticket}.cpp`; Modify `proto/CMakeLists.txt` (link `unofficial-sodium::sodium` e `nlohmann_json::nlohmann_json`; `target_compile_definitions(cbmp_proto_tests PRIVATE CBMP_VECTORS="${CMAKE_SOURCE_DIR}/packages/contracts/test-vectors/tickets.json")`).

**Interfaces — Produces:**
- `std::optional<std::vector<uint8_t>> base64urlDecodeCanonical(std::string_view)` (nullopt se charset inválido, len%4==1, ou re-encode ≠ entrada); `std::string base64urlEncode(std::span<const uint8_t>)`
- `enum class TicketError { Malformed, BadVersion, UnknownKid, BadSignature, WrongAudience, Expired, NotYetValid }`; `std::string_view toString(TicketError)` → mesmos nomes do TS (`malformed`, `bad_version`, `unknown_kid`, `bad_signature`, `wrong_audience`, `expired`, `not_yet_valid`)
- `struct TicketPayload { int v; std::string kid, sub, did, name, aud, jti; int64_t iat, exp; }`
- `using PublicKeys = std::unordered_map<std::string, std::array<uint8_t, 32>>`
- `std::expected<TicketPayload, TicketError> verifyTicket(std::string_view ticket, const PublicKeys&, std::string_view audience, int64_t nowSeconds)`
- `class JtiCache { bool seenOrInsert(const std::string& jti, int64_t expiresAt, int64_t now); size_t size() const; }` (purga entradas vencidas a cada inserção amortizada)
- `constexpr int64_t kClockSkewSeconds = 60;`

- [ ] **Step 1: Testes (falhando)** — `proto/tests/test_ticket.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cbmp/base64url.hpp>
#include <cbmp/ticket.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
using namespace cbmp;

static nlohmann::json loadVectors() { std::ifstream f(CBMP_VECTORS); REQUIRE(f.good()); return nlohmann::json::parse(f); }

TEST_CASE("todos os vetores de tickets.json dão o mesmo resultado que o TS") {
  const auto v = loadVectors();
  PublicKeys keys;
  for (const auto& k : v["publicKeys"]) {
    auto raw = base64urlDecodeCanonical(k["publicKey"].get<std::string>());
    REQUIRE(raw); REQUIRE(raw->size() == 32);
    std::array<uint8_t, 32> a{}; std::copy(raw->begin(), raw->end(), a.begin());
    keys.emplace(k["kid"].get<std::string>(), a);
  }
  REQUIRE(v["cases"].size() >= 18);
  for (const auto& c : v["cases"]) {
    INFO(c["name"].get<std::string>());
    auto r = verifyTicket(c["ticket"].get<std::string>(), keys, c["audience"].get<std::string>(), c["nowSeconds"].get<int64_t>());
    const std::string got = r ? "ok" : std::string(toString(r.error()));
    CHECK(got == c["expected"].get<std::string>());
  }
}

TEST_CASE("base64url canônico") {
  CHECK(base64urlDecodeCanonical("AQID") == std::vector<uint8_t>{1, 2, 3});
  CHECK_FALSE(base64urlDecodeCanonical("AQI="));   // padding
  CHECK_FALSE(base64urlDecodeCanonical("AQ+D"));   // charset
  CHECK_FALSE(base64urlDecodeCanonical("AQIDA"));  // len%4==1
  CHECK_FALSE(base64urlDecodeCanonical("AR"));     // bits sobrando não-zero (canônico seria "AQ")
  CHECK(base64urlEncode(std::vector<uint8_t>{1, 2, 3}) == "AQID");
}

TEST_CASE("JtiCache recusa repetição até expirar") {
  JtiCache c;
  CHECK_FALSE(c.seenOrInsert("a", 1000, 900));
  CHECK(c.seenOrInsert("a", 1000, 950));
  CHECK_FALSE(c.seenOrInsert("a", 2000, 1000 + kClockSkewSeconds + 1));  // expirou + skew → pode de novo
}
```

- [ ] **Step 2: Implementar**

`base64url.cpp`: tabela `A-Z a-z 0-9 - _`; rejeitar qualquer outro char; rejeitar `len % 4 == 1`; decodificar; reencodar sem padding e comparar com a entrada (garante bits sobrando zero).

`ticket.cpp`: **portar exatamente** `verifyTicket` de `packages/contracts/src/ticket.ts` (ler o arquivo atual — inclui as regras adicionadas na revisão): mesma ordem; mesmas rejeições por texto cru (JSON compacto: sem whitespace; cada uma das chaves `v kid sub did name aud iat exp jti` aparece exatamente uma vez como `"<chave>":`; `v`/`iat`/`exp` literais inteiros simples sem `.`/`e`/`E`); parse com `nlohmann::json::parse(body, nullptr, /*allow_exceptions*/false)` → `discarded` = malformed; checar tipos (strings para kid/sub/did/name/aud/jti; inteiros seguros `|x| ≤ 2^53−1` para v/iat/exp); assinatura com `crypto_sign_verify_detached(sig, body, bodyLen, pk)` só se `sig.size()==64`; chamar `sodium_init()` uma vez (static). Comparar o resultado com os vetores — o teste acima é a verdade.

`JtiCache`: `std::unordered_map<std::string,int64_t>`; `seenOrInsert`: se existe e `now ≤ exp+skew` → true; senão insere/atualiza e retorna false; a cada 1024 inserções, apaga vencidos.

- [ ] **Step 3: Testes PASS** (Windows + Linux asan). Se algum vetor divergir, o C++ está errado — nunca editar `tickets.json`.
- [ ] **Step 4: Commit** — `feat(proto): base64url canônico e validação de ticket compatível com os vetores`

---

### Task 4: Config, métricas, manifest de resources e HTTP

**Files:** Create `server/CMakeLists.txt` (lib `cbserver_core` + exe `cb-server` + `cbserver_tests`), `server/include/cbserver/{config,metrics,manifest,http}.hpp`, `server/src/{config,metrics,manifest,http}.cpp`, `server/server.cfg.example`, `server/tests/{test_config,test_metrics,test_manifest,test_http}.cpp`.

**Interfaces — Produces:**
- `struct ServerConfig` com todos os campos do JSON da spec (mesmos nomes/defaults) + `static ServerConfig load(const std::filesystem::path&)` (lança `std::runtime_error` com mensagem pt-BR: campo + motivo) e `static ServerConfig fromJson(const nlohmann::json&)`; validações: `maxPlayers` 1..2048, `port` 1..65535, `sim.tickHz` 1..60, `streamRadiusM` 50..2000, `maxVisible` 8..256, `tolerance` 0..1, velocidades ≥ 0, `hub` completo ou ausente (serverId `^srv_`, key `^sk_`, url http(s)), `devPublicKey` null ou base64url canônico de 32 bytes.
- `class Metrics`: `void recordTick(std::chrono::microseconds)`, `void addBytesOut(uint16_t playerId, size_t)`, contadores atômicos `players, pending, corrections, kicks, droppedEvents`; `nlohmann::json snapshot() const` → `{tickP50Ms, tickP99Ms, tickMaxMs, players, pending, corrections, kicks, droppedEvents, avgKbpsPerPlayer, maxKbpsPerPlayer, rssBytes, cpuPercent}` (janela 60 s: anel de 1200 amostras; RSS via `/proc/self/statm` no Linux e `GetProcessMemoryInfo` no Windows).
- `struct ManifestFile { std::string path, sha256; uint64_t size; }`, `struct ResourceEntry { std::string name, version; std::vector<ManifestFile> files; }`, `class ResourceStore { static ResourceStore scan(const std::filesystem::path& dir); nlohmann::json manifestJson(const std::string& serverId, uint32_t protocol) const; std::optional<std::filesystem::path> fileBySha(const std::string&) const; }` — lança `std::runtime_error` se houver arquivo nativo em qualquer lugar do resource (inclusive `server/`) ou `resource.json` inválido; publica só `client/**`, `ui/**`, `files/**` com extensões da lista branca da spec da plataforma; sha256 via libsodium `crypto_hash_sha256`.
- `class HttpServer { HttpServer(const ServerConfig&, const ResourceStore&, Metrics&, std::function<nlohmann::json()> info); void start(const std::string& host, int port); void stop(); int boundPort() const; }` — rotas `/info`, `/cb/manifest`, `/cb/files/:sha` (só hex 64), `/metrics` (127.0.0.1/::1 ou header `Authorization: Bearer <metrics.token>`), 404 JSON `{error:"not_found"}`; respostas JSON com `Content-Type: application/json`; arquivos com `Cache-Control: public, max-age=31536000, immutable`.

- [ ] **Step 1: Testes (falhando)** — escrever:
  - `test_config.cpp`: example carrega com defaults; cada validação acima recusa com mensagem contendo o nome do campo (mínimo: maxPlayers 0, port 70000, hub sem key, devPublicKey inválida); `hub` ausente é aceito.
  - `test_metrics.cpp`: 1200 amostras 1..1200 µs → p50 ≈ 0.6 ms, p99 ≈ 1.188 ms, max 1.2 ms; janela descarta antigas.
  - `test_manifest.cpp`: monta pasta temporária com `res1/resource.json {name,version}`, `res1/client/main.lua`, `res1/ui/index.html`, `res1/server/main.lua`, `res1/files/a.png`, `res1/client/x.exe` → `scan` lança; sem o `.exe` → manifest lista 3 arquivos (não lista `server/`), sha256 corretos (comparar com `crypto_hash_sha256` calculado no teste), `totalSize` soma; `fileBySha` acha; arquivo com extensão fora da lista em `files/` é ignorado com aviso (não lança).
  - `test_http.cpp`: sobe `HttpServer` em `127.0.0.1:0` (porta efêmera via `bind_to_any_port`), usa `httplib::Client`: `/info` 200 JSON; `/cb/manifest` 200; `/cb/files/<sha válido>` 200 com bytes e header immutable; `/cb/files/zzz` 404; `/metrics` de 127.0.0.1 200; `/nada` 404 JSON.
- [ ] **Step 2: Implementar** conforme interfaces. `server.cfg.example` = JSON da spec (com `hub` comentado em README, pois JSON não tem comentário: deixar `"hub": null`).
- [ ] **Step 3: Testes PASS** (Windows + Linux asan).
- [ ] **Step 4: Commit** — `feat(server): config, métricas, manifest de resources e HTTP`

---

### Task 5: Mundo — grade, visibilidade, movimento, limites de evento, snapshots paralelos

**Files:** Create `server/include/cbserver/{grid,visibility,rate,movement,world,snapshots,pool}.hpp`, `server/src/{grid,visibility,movement,world,snapshots,pool}.cpp`, `server/tests/{test_grid,test_visibility,test_rate,test_movement,test_world,test_snapshots}.cpp`.

**Interfaces — Produces:**

```cpp
// grid.hpp
namespace cbserver {
using EntityId = uint16_t;
class Grid {
public:
  explicit Grid(int32_t cellCm = 6400);
  void upsert(EntityId id, const cbmp::Vec3i& pos);   // move entre células se mudou
  void remove(EntityId id);
  // Ids nas células que intersectam o quadrado [pos-r, pos+r] (só XY). Pode incluir fora do raio.
  void query(const cbmp::Vec3i& pos, int32_t radiusCm, std::vector<EntityId>& out) const;
  size_t size() const;
};
}
```

```cpp
// rate.hpp — token bucket por chave
namespace cbserver {
class TokenBucket {
public:
  TokenBucket(double perSecond, double burst) : rate_(perSecond), burst_(burst), tokens_(burst) {}
  bool take(double nowSec) {
    if (last_ >= 0) tokens_ = std::min(burst_, tokens_ + (nowSec - last_) * rate_);
    last_ = nowSec;
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0; return true;
  }
private:
  double rate_, burst_, tokens_; double last_ = -1;
};
}
```

```cpp
// movement.hpp
namespace cbserver {
struct MovementLimits { std::array<double, 7> maxSpeedCmS; double tolerance; double slackCm = 50; };
enum class MoveVerdict { Ok, Corrected };
// Se a distância desde a última posição aceita > maxSpeed(estado da última OU do novo, o maior)*(1+tol)*dt + slack → Corrected.
// maxSpeed 0 (apparate) → só slack.
MoveVerdict validateMove(const cbmp::PlayerState& last, const cbmp::PlayerState& next, double dtSec, const MovementLimits&);
}
```

```cpp
// visibility.hpp
namespace cbserver {
struct VisibilityParams { int32_t enterCm = 30000; int32_t exitCm = 33000; size_t maxVisible = 128; };
enum class Tier : uint8_t { Near = 1, Mid = 2, Far = 4 };   // envia a cada 1, 2 ou 4 ticks (20/10/5 Hz)
Tier tierFor(int64_t distSqCm);                               // <100 m Near, <200 m Mid, senão Far
struct VisibleSet { std::vector<EntityId> ids; };            // ordenado por id
// Calcula o novo conjunto: candidatos da grade; entra quem está < enter; quem já estava visível fica até > exit;
// limita a maxVisible pelos mais próximos (excluindo o próprio viewer).
VisibleSet computeVisible(EntityId viewer, const cbmp::Vec3i& viewerPos, const VisibleSet& previous,
                          const Grid& grid, const std::function<const cbmp::Vec3i&(EntityId)>& posOf,
                          const VisibilityParams& p, std::vector<EntityId>& scratch);
}
```

```cpp
// world.hpp — estado autoritativo, sem rede
namespace cbserver {
struct PlayerSlot {
  EntityId id; std::string sub, did, name;
  cbmp::PlayerState state; uint32_t lastInputSeq = 0; double lastInputTime = 0;
  std::deque<double> correctionTimes; VisibleSet visible;
  std::unordered_map<std::string, TokenBucket> eventBuckets; std::deque<double> eventViolations;
  // baselines de snapshot: anel de 32 ticks → estados enviados naquele tick
  std::array<std::pair<uint32_t, std::unordered_map<EntityId, cbmp::PlayerState>>, 32> sent;
  uint32_t lastAckTick = 0; double lastAckTime = 0;
};
struct Outgoing { EntityId to; cbmp::Message msg; };     // mensagens a enviar (confiáveis ou não, por tipo)
struct KickOrder { EntityId who; std::string reason; };
class World {
public:
  World(const ServerConfig&);
  std::optional<EntityId> addPlayer(std::string sub, std::string did, std::string name, double now); // nullopt se cheio
  void removePlayer(EntityId);
  void onInput(EntityId, const cbmp::Input&, double now);           // valida e aplica/corrige
  bool onEvent(EntityId, const cbmp::Event&, double now);            // false = descartado (limite)
  void onSnapshotAck(EntityId, uint32_t tick, double now);
  // Executa um tick: visibilidade, Enter/Leave, snapshots (via SnapshotBuilder), kicks pendentes.
  void tick(double now, class SnapshotBuilder&, std::vector<Outgoing>& out, std::vector<KickOrder>& kicks);
  uint32_t currentTick() const; size_t playerCount() const;
  const PlayerSlot* find(EntityId) const;
};
}
```

Regras de `World` (todas cobertas por teste):
- `addPlayer`: ids 1..maxPlayers reutilizados; estado inicial na origem `(0,0,0)` parado; aparece para os outros no próximo tick.
- `onInput`: ignora `seq ≤ lastInputSeq`; `validateMove` com `dt = now - lastInputTime` (mín. 1/tickHz); `Ok` → aplica estado; `Corrected` → mantém estado, emite `Correction{seq, pos, vel}` e registra; > 10 correções em 10 s → `KickOrder{"movimento inválido"}`.
- `onEvent`: bucket por nome (`events.perSecond/burst`); descarte conta violação; > 3 violações×burst em 10 s → `KickOrder{"flood de eventos"}`. Eventos aceitos são entregues ao "dono do jogo" (no subprojeto 2: eco para o próprio cliente apenas se nome == `"echo"`, para os bots validarem ida e volta; demais são descartados silenciosamente após contabilizar).
- `tick`: incrementa `tick`; para cada jogador: `computeVisible`; diferença com anterior → `EntityEnter{id, estado, name}` / `EntityLeave{id}`; então snapshots.
- Snapshot por cliente: baseline = entrada do anel com `tick == lastAckTick` (se `now - lastAckTime > 1 s` ou ausente → baseline vazia); para cada id visível cujo tier "toca" neste tick (`tick % tier == id % tier`), máscara = campos diferentes do baseline (ou `kMaskAll` se ausente no baseline); inclui se máscara ≠ 0; grava no anel o estado enviado (mesclado com baseline). Sempre envia snapshot (mesmo vazio) a cada tick Near para manter `ackInputSeq`.

```cpp
// pool.hpp — pool fixo; run() executa fn(i) para i em [0,n) dividido em blocos, bloqueia até terminar
namespace cbserver {
class TaskPool {
public:
  explicit TaskPool(unsigned threads);
  ~TaskPool();
  void parallelFor(size_t n, const std::function<void(size_t begin, size_t end, unsigned worker)>& fn);
  unsigned workers() const;
};
}
// snapshots.hpp
namespace cbserver {
class SnapshotBuilder {
public:
  explicit SnapshotBuilder(TaskPool&);
  // Monta, em paralelo, snapshots de todos os slots; cada worker usa buffers próprios; resultado agregado em out.
  void build(uint32_t tick, std::vector<PlayerSlot*>& slots, const std::function<const cbmp::PlayerState&(EntityId)>& stateOf, std::vector<Outgoing>& out);
};
}
```

- [ ] **Step 1: Testes (falhando)** — escrever, no mínimo:
  - grid: upsert/move/remove; query devolve vizinhos de células adjacentes e não de células longe; entidade exatamente na borda da célula.
  - visibility: entra < 300 m; não entra a 310 m; quem estava visível a 320 m continua; sai > 330 m; teto 128 com 200 candidatos mantém os 128 mais próximos; viewer nunca na própria lista; `tierFor` 99 m Near, 150 m Mid, 250 m Far.
  - rate: 40 de rajada aceitos, 41º recusado; após 0,5 s com 20/s recupera 10.
  - movement: andar a 400 cm/s por 0,05 s ok; 3× a velocidade → Corrected; estado de vassoura usa limite maior; apparate só slack.
  - world: dois jogadores a 10 m → no 1º tick cada um recebe `EntityEnter` do outro; afastar para 400 m → `EntityLeave`; input com seq repetido ignorado; teleporte → `Correction` e, após 11 em 10 s, `KickOrder`; flood de eventos → `KickOrder`; `addPlayer` além de `maxPlayers` → nullopt; evento `echo` volta para o remetente.
  - snapshots: com baseline confirmado, entidade parada não aparece; só posição mudou → máscara `kMaskPos`; sem ack por > 1 s → `kMaskAll`; tier Far só a cada 4 ticks; resultado de `build` com 1 worker == com 4 workers (mesmas mensagens, mesma ordem após ordenar por destinatário).
- [ ] **Step 2: Implementar** (código limpo, sem alocação por entidade no caminho quente: reusar `scratch`/buffers por worker).
- [ ] **Step 3: Testes PASS** (Windows + Linux asan).
- [ ] **Step 4: Commit** — `feat(server): mundo autoritativo com grade, visibilidade, limites e snapshots paralelos`

---

### Task 6: Rede (GameNetworkingSockets), autenticação e loop principal

**Files:** Create `server/include/cbserver/{auth,net}.hpp`, `server/src/{auth,net,main}.cpp`, `server/tests/test_auth.cpp`, `server/tests/test_net.cpp`; Modify `server/CMakeLists.txt` (link `GameNetworkingSockets::shared` ou o alvo exportado pelo port; o implementador confirma o nome em `vcpkg_installed/*/share/gamenetworkingsockets`).

**Interfaces — Produces:**
- `class KeyProvider { virtual ~KeyProvider(); virtual cbmp::PublicKeys keys() = 0; virtual void requestRefresh() = 0; }`; `class StaticKeys : KeyProvider` (dev / testes).
- `class Authenticator { Authenticator(KeyProvider&, std::string serverId, std::function<int64_t()> nowSec); std::expected<cbmp::TicketPayload, std::string> check(std::string_view ticket); }` — chama `verifyTicket`; se `unknown_kid`, `requestRefresh()` (o provider limita a 1/min) e recusa; usa `JtiCache`; razão em texto para `Reject`.
- `class NetServer`:
  - `NetServer(const ServerConfig&, Authenticator&, World&, Metrics&)`, `bool start(uint16_t port, std::string& err)`, `void stop()`.
  - `void pump(double now, std::vector<Inbound>& in)` — roda callbacks do GNS, recebe mensagens do poll group, aplica regras de conexão (abaixo) e devolve mensagens de jogadores autenticados como `Inbound{EntityId, cbmp::Message}`.
  - `void send(const std::vector<Outgoing>&)` — `k_nSteamNetworkingSend_Reliable` se `isReliable(type)`, senão `k_nSteamNetworkingSend_UnreliableNoDelay`; conta bytes em `Metrics`.
  - `void kick(EntityId, const std::string& reason)` — envia `Kick` e fecha com linger.
- Regras de conexão (todas testadas): aceitar conexão apenas se `pendingPerIp`/`pendingGlobal` não excedidos (senão `CloseConnection` com motivo); registro pendente = `{conn, ip, t0}`; primeira mensagem deve ser `Hello` ≤ 4 KiB em ≤ 5 s (senão fecha); `protocol != kProtocolVersion` → `Reject{"protocolo incompatível"}`; ticket inválido → `Reject{<razão>}`; válido → se `sub` já tem sessão, derruba a anterior com `Kick{"conectado em outro lugar"}`; `world.addPlayer` (servidor cheio → `Reject{"servidor cheio"}`); `Welcome`; mensagens de jogador não autenticado ou tipos que só o servidor envia → fecha; `DecodeError` → fecha e conta.
- `main.cpp`: `cb-server [--config server.cfg]`: carrega config; spdlog (console + arquivo `logs/server.log` rotativo) e logger de anti-cheat JSONL em `logs/anticheat-YYYY-MM-DD.jsonl`; `ResourceStore::scan`; `KeyProvider` (Hub: `HubKeys` da Task 7; dev: `StaticKeys(devPublicKey)`); `World`, `TaskPool(max(1, hw-1))`, `SnapshotBuilder`, `NetServer`, `HttpServer` (mesma porta TCP); loop: a cada `1/tickHz` s → `net.pump` → despachar Inbound para `world` → `world.tick` → `net.send`/`kick` → `metrics.recordTick`; dormir até o próximo tick (`std::this_thread::sleep_until`); `SIGINT/SIGTERM` (Linux) e `SetConsoleCtrlHandler` (Windows) → parada limpa.

- [ ] **Step 1: Testes (falhando)**
  - `test_auth.cpp` (sem rede): ticket válido (assinar com libsodium `crypto_sign_seed_keypair` + `crypto_sign_detached`, montando JSON compacto igual ao Hub) → ok; jti repetido → recusa; kid desconhecido → recusa e `requestRefresh` chamado; aud errado → recusa.
  - `test_net.cpp` (loopback real com GNS, servidor e cliente no mesmo processo, porta efêmera): handshake ok recebe `Welcome`; ticket inválido recebe `Reject` e conexão fecha; sem `Hello` em 5 s → fechada (usar relógio injetado ou timeout configurável reduzido a 200 ms no teste); 5ª conexão pendente do mesmo IP recusada; segundo login do mesmo `sub` derruba o primeiro com `Kick`; `Input` antes do `Hello` → fechada; mensagem lixo → fechada.
- [ ] **Step 2: Implementar.** Inicializar GNS uma vez por processo (`GameNetworkingSockets_Init`); callback de status via `k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged` no listen socket; poll group único; nunca bloquear o loop.
- [ ] **Step 3: Testes PASS** (Windows + Linux asan). Smoke manual: `cb-server --config server.cfg.example` sobe e responde `GET http://127.0.0.1:30500/info`.
- [ ] **Step 4: Commit** — `feat(server): rede GNS com handshake por ticket, limites de conexão e loop principal`

---

### Task 7: Integração com o Hub (chaves + heartbeat)

**Files:** Create `server/include/cbserver/hub.hpp`, `server/src/hub.cpp`, `server/tests/test_hub.cpp`; Modify `server/src/main.cpp`.

**Interfaces — Produces:**
- `class HubKeys : KeyProvider` — `HubKeys(std::string hubUrl, std::function<double()> nowSec)`; busca `GET {hub}/v1/keys/public` no início (bloqueante com timeout 10 s; falha → log de erro e tenta de novo a cada 30 s em background); `requestRefresh` agenda recarga no máximo 1x/60 s; `keys()` thread-safe (cópia sob mutex ou `std::atomic<std::shared_ptr>`).
- `class Heartbeat` — thread própria; a cada 30 s `POST {hub}/v1/servers/heartbeat` com `Authorization: Bearer <key>` e corpo `HeartbeatBody` (`name, description, players, maxPlayers, gamePort, httpPort, version, protocol, tags, locale`); loga `listed`/`reason` quando muda; erro de rede → back-off 30→60→120 s (máx.); nunca derruba o servidor; `stop()` acorda e encerra a thread.
- `main.cpp`: com `hub` configurado usa `HubKeys` + `Heartbeat`; sem `hub` usa `StaticKeys(devPublicKey)` e não anuncia; erro de config se nenhum dos dois.

- [ ] **Step 1: Testes (falhando)** — `test_hub.cpp` com um `httplib::Server` falso em 127.0.0.1:0: (a) `HubKeys` carrega 2 chaves e `requestRefresh` dentro de 60 s não refaz a busca, depois de 60 s refaz (relógio injetado); (b) `Heartbeat` envia corpo com os campos e header corretos (capturar no servidor falso), intervalo configurável reduzido no teste; (c) servidor falso fora do ar → back-off e nenhuma exceção.
- [ ] **Step 2: Implementar.**
- [ ] **Step 3: Teste de integração manual** (documentar no report): Hub local (`HUB_DEV_LOGIN=1 HUB_ALLOW_PRIVATE_SERVERS=1 npm start -w @cbmp/hub`), criar servidor no painel, colar `serverId`/`key` no `server.cfg`, subir `cb-server` → aparece em `GET /v1/servers` em ≤ 30 s; parar → some em ≤ 90 s.
- [ ] **Step 4: Commit** — `feat(server): chaves públicas e heartbeat do Hub`

---

### Task 8: Bots, teste de carga e fuzz no CI

**Files:** Create `bots/CMakeLists.txt`, `bots/src/{main,bot}.cpp`, `bots/src/bot.hpp`, `proto/fuzz/proto_fuzz.cpp`, `tools/loadtest.py`; Modify `proto/CMakeLists.txt` (alvo fuzz quando `CBMP_BUILD_FUZZ`), `.github/workflows/native.yml` (jobs `fuzz` e `load`).

**Interfaces — Produces:**
- `cb-bots --server HOST:PORT --dev-seed <hex32> --server-id <id> --count N --area <m> --cluster <0..1> --duration <s> --report <file> [--teleport-every <s>] [--flood]`
  - Cada bot: gera ticket assinado com a seed dev (`sub`=`bot_<i>`, `did`=`9000000000000000<i>`, `name`=`Bot <i>`, `aud`=server-id, `iat`=now, `exp`=now+300, `jti` aleatório; JSON compacto na ordem `v,kid,sub,did,name,aud,iat,exp,jti`, `kid`="dev"); conecta (abrir conexões em rampa de 50/s); manda `Input` 20 Hz com movimento aleatório suave dentro da área (fração `cluster` num círculo de 30 m no centro); a cada 2 s um `Event{"echo"}`; responde `SnapshotAck`; mede RTT pelo eco e intervalo máximo entre snapshots.
  - `--teleport-every`: bot 0 teleporta (espera `Correction` e depois `Kick`); `--flood`: bot 0 manda 500 eventos/s (espera `Kick`).
  - Relatório JSON: `{connected, failedToConnect, disconnected, kicked, maxSnapshotGapMs (máx entre bots), p99SnapshotGapMs, avgRttMs, bytesIn, durationS}`.
- `tools/loadtest.py`: sobe `cb-server` com config gerada (dev key derivada da seed, `maxPlayers` = count+16, hub null), espera `/info`, roda `cb-bots`, lê `/metrics` a cada 5 s, encerra, avalia limites da spec e imprime tabela; `--assert` sai 1 se violar (repete 1x antes de falhar, como a spec manda); também roda cenários de segurança curtos (`--teleport-every 1`, `--flood`, ticket com seed errada → todos recusados) e exige o resultado esperado.
- `proto_fuzz.cpp`: `LLVMFuzzerTestOneInput` chama `cbmp::decode` dentro de try/catch(`DecodeError`); se decodificou, reencoda e decodifica de novo e exige igualdade de tipo.
- CI: job `fuzz` (ubuntu, preset `linux-fuzz`, `-max_total_time=120`); job `load` (ubuntu, `linux-release`, `python3 tools/loadtest.py --count 256 --duration 120 --cluster 0.3 --assert`), anexa o relatório como artifact.

- [ ] **Step 1:** Implementar bots e script; rodar local (Windows) com 64 bots por 30 s para validar o fluxo; anotar números no report.
- [ ] **Step 2:** Fuzz local no Linux não é possível nesta máquina; garantir que o alvo compila no CI.
- [ ] **Step 3:** Commit — `feat(bots): bots de carga, script de teste de carga/segurança e fuzz no CI`
- [ ] **Step 4:** **Push do branch** (único push do plano, necessário para rodar o CI) — pedir OK ao controlador antes. Ajustar até o CI ficar verde; registrar no report os números do job `load`.

---

### Task 9: Documentação e fechamento

**Files:** Create `docs/LOAD-TEST.md`, `server/README.md`; Modify `README.md` raiz (seção "Nativo").

- [ ] `docs/LOAD-TEST.md`: passo a passo para alugar VPS Linux 4 vCPU (ex. Hetzner CPX31), instalar dependências, build `linux-release`, rodar `tools/loadtest.py --count 512 --duration 300 --cluster 0.3 --assert`, interpretar a tabela, e onde arquivar o resultado; aviso: obrigatório antes de produção.
- [ ] `server/README.md`: build local Windows/Linux, `server.cfg` (campo a campo), modo dev vs Hub, portas/firewall (UDP+TCP 30500), pasta `resources/` e regras de publicação, logs.
- [ ] Commit — `docs(server): README do servidor e roteiro do teste de carga de 512`

## Fim do subprojeto 2

Critério (spec): build Linux + Windows; unitários e fuzz curto verdes; carga do CI dentro dos limites; segurança verde; aparece no Hub e some ao parar; manifest/files servindo exemplo; `LOAD-TEST.md` escrito.
