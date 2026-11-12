# Subprojeto 5a — Mod no jogo: conectar e se ver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dois jogadores se vendo andar no mesmo mundo: o launcher 0.2.0 instala e ativa o mod assinado 0.2.0 (UE4SS + `cb-mp`), grava `connect.json` + ponteiro e abre o jogo; o mod resgata o ticket, conecta no `cb-server` (protocolo 1, sem mudança), envia o estado local a 20 Hz e desenha os outros jogadores como bonecos interpolados.

**Architecture:** `client/core` (C++23 puro, namespace `cbmp::client`, testado com Catch2 no build nativo da raiz) contém toda a lógica: arquivo de conexão, regra dos dois processos, rede, espelho de snapshots, interpolação, bonecos e a máquina de estados `Session`. `client/src` é a casca fina Windows + UE4SS (`Ue4ssGame : IGame`, portas Win32) e só compila no build do mod. O launcher passa a instalar o pacote em `Win64\cbmp\atual\`, ativar o proxy do UE4SS por `dwmapi.dll` + `override.txt`, gravar o ponteiro `cbmp-run.json` e oferecer remoção, conflito com backup e elevação única.

**Tech Stack:** C++23 (MSVC / gcc 13), CMake + vcpkg (GameNetworkingSockets, nlohmann-json, Catch2), UE4SS v3.0.1 (`4fb9b615`), Python 3 (e2e), Rust/Tauri 2 + Svelte 5 (launcher), Node 24 (ferramentas), GitHub Actions.

## Global Constraints

**Fonte da verdade:** `docs/superpowers/specs/2026-11-11-sub5a-mod-conectar-design.md`. Em dúvida, vale a spec; se a spec e este plano divergirem num valor, pare e reporte (`NEEDS_CONTEXT`).

**Regras de trabalho (todas as tasks)**

- Branch `sub5a-mod`; um ou mais commits por task; mensagem em pt-BR no padrão do repositório (`feat(client): …`, `feat(launcher): …`), linha em branco e o trailer `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`; ao fim da task `git push origin sub5a-mod`.
- Sem WSL, Docker ou bancos locais. Nenhuma task roda o Hogwarts Legacy: esta máquina não tem o jogo. Tudo o que depende do jogo vai para o roteiro manual (Task 12).
- Sem segredos nem chaves privadas no repositório. Chave dev: `<pasta-da-chave-dev>\update.key`, fora do repositório (nunca copiar para o repo nem para a VPS, nunca imprimir).
- Dump do jogo (`<pasta-do-dump>\...`, fora do repositório) é só referência: em código e docs entram apenas nomes curtos de classes, funções e propriedades; nunca cabeçalhos, trechos ou listas em massa.
- Nenhum código de MafiaHub, arcane-mp ou HogwartsMP. Nunca tocar repositórios `castelobruxo/*`.
- UE4SS/Unreal só dentro de `client/src/game/` (e `CbMpMod.*`, `dllmain.cpp`, que são a casca do mod). `client/core` não inclui UE4SS nem `Windows.h` (há teste de guarda).
- Texto para o usuário e docs em pt-BR; identificadores de código em inglês, como o repositório já faz. Código novo do cliente: namespace `cbmp::client`, cabeçalhos em `<cbmp/client/...>`, métodos em PascalCase (como `IGame`), casos de teste Catch2 com nome começando por `client: `.
- Ambiente de teste online só por `tools/teste-online.mjs` (VPS: `ssh -p <porta-ssh> <usuario>@<vps>`); nunca tocar outros projetos na VPS; nunca imprimir segredos.
- Testes que têm de passar antes de cada commit estão no fim de cada task ("Verificação"). CI existente continua verde (`native`, `launcher`, Hub). Se o GitHub Actions não estiver rodando (cobrança da conta), registre no relatório e siga.

**Build local (Windows)**

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation
$env:VCPKG_ROOT='C:\vcpkg'
cmake --preset windows-release -DVCPKG_INSTALLED_DIR=C:/cbmp-vi      # nunca apagar C:\cbmp-vi
cmake --build --preset windows-release --target <alvo>
ctest --test-dir build-native/windows-release -C Release --output-on-failure -R "<filtro>"
```

- Build do mod (só Tasks 7 e 11): `tools/build.ps1 -Target mod` (UE4SS em `client/deps/RE-UE4SS`, git-ignored, com o submódulo privado `UEPseudo` já presente localmente). Pacotes vcpkg do mod em **`C:/cbmp-vi-mod`** (manifesto `client/vcpkg.json`, triplet `x64-windows-static-md`): nunca apontar o build do mod para `C:/cbmp-vi`, porque o vcpkg em modo manifesto removeria de lá os pacotes do build da raiz.
- Launcher: `cargo test --locked` em `launcher/src-tauri` (e com `--features local-test` e `--features online-test`); `npm test -w launcher/ui`; `npm run typecheck -w launcher/ui`. Ferramentas: `npm run test:tools`.

**Valores da spec (obrigatórios, copiar exatamente)**

- Versões: mod `0.2.0`, protocolo `1` (`cbmp::kProtocolVersion`), tag `mod-p1-v0.2.0`, zip `cbmp-mod-p1-v0.2.0.zip`; launcher `0.2.0`; UE4SS `3.0.1+4fb9b615` (commit `4fb9b615a24c724041ef23e2c4073f2610c4c6d0`). Protocolo e código do servidor **não mudam**.
- Pastas no jogo (`Win64` = `<jogo>\Phoenix\Binaries\Win64`): contêiner `Win64\cbmp\`; pacote em `Win64\cbmp\atual\` (troca: `atual.staging` → `atual`, anterior em `atual.old`); ponteiro `Win64\cbmp\cbmp-run.json`; `Win64\cbmp\logs\`, `Win64\cbmp\crash\`, `Win64\cbmp\cb-mp.json`; `Win64\dwmapi.dll`; `Win64\override.txt` com uma linha `cbmp\atual\ue4ss`; `Win64\cbmp-backup\`. A pasta `Win64\ue4ss\` do usuário nunca é tocada.
- Ponteiro: `{ "v": 1, "runDir": "<absoluto>", "launchId": "<32 hex>", "writtenAt": <Unix s>, "launcher": "0.2.0" }`; ≤ 4 KiB; `runDir` absoluto, sem `\\` no começo, último segmento `run`; idade ≤ 600 s, futuro ≤ 120 s; procurado subindo no máximo 5 pastas a partir da pasta do `main.dll`.
- `connect.json`: `{host, port, ticket, serverId, contentDir, protocol, contentReadOnly: true, launchId}`; ≤ 16 KiB; `host` IP literal; `launchId` igual ao do ponteiro.
- Dois processos: seção `Local\cbmp-mod-epoch-v1` (8 bytes), mutex `Local\cbmp-mod-owner-v1`; `kClaimDelay` = 45 s (época 1) / 5 s (época > 1); resgate com violação de compartilhamento repetido por até 5 s; log `cb-mp-<aaaammdd-hhmmss>-<pid>.log`, ficam os 10 mais novos.
- Prazos do launcher: `CONNECT_TTL` = 240 s; `TICKET_RENEW_AFTER_SECS` = 30; o mod desiste com `ticket_expirado` se faltarem menos de 10 s para o `exp`.
- Sessão: conectar 10 s; `Hello` em até 1 s; `Welcome` 10 s; `tickRateHz` aceito 5..60; `Input` a cada 50 ms (20 Hz), `seq` a partir de 1, sem rajada.
- `Correction`: ≤ 100 cm nada; ≤ 2000 cm `TeleportLocal` (no máximo 1/s); > 2000 cm ou `AwaitWorld` → próximo `Input` com `Apparate`.
- Estado local: fora de ±2 000 000 000 cm ou não finito → amostra pulada; velocidade ±32767 cm/s; chão: < 10 cm/s `Idle`, ≤ 380 `Walk`, ≤ 1000 `Run`, acima `Broom`; `Falling`→`Fall`, `Swimming`→`Swim`, `Flying`→`Broom`; salto (> 3000 cm entre envios, ou 1º `Input` em `InWorld`) → `Apparate`.
- `InWorld`: pawn válido, mundo fora da lista de menus (`RootLevel`), `frontendHint` falso, mesmos `worldId`/`pawnId` há ≥ 2 s, posição a mais de 100 cm da origem em XY.
- Interpolação: atraso 100 ms (até 400 ms por entidade); extrapolação ≤ 250 ms; salto direto se > 500 cm ou `Apparate`; relógio = mínimo em janela de 10 s, mudança > 2 s reinicia; fila de 32 amostras.
- Bonecos: raio 400 m; teto `maxBonecos` 32 (máximo 64); `SpawnActor` nulo → nova tentativa em 1 s, 3 vezes; `Gone` → recria após 1 s, no máximo 5 por minuto por entidade; 3 `GameError` seguidos no mesmo boneco → `Gone`; 20 `GameError` em 10 s → bonecos desligados.
- Limites de segurança: mensagem ≤ 8 KiB; ≤ 512 mensagens por tick; fila > 4096 ou > 20 erros de decodificação em 10 s → desconecta; ≤ 255 entidades; anel de 32 ticks; `partCount` ≤ 8; nomes ≤ 64 bytes; textos de `Reject`/`Kick` ≤ 255 bytes; exceções no tick: resumo a cada 10 s, 100 em 60 s → `erro_interno`.
- Motivos de desconexão (exatos): `sem_arquivo`, `arquivo_invalido`, `protocolo_diferente`, `ticket_expirado`, `sem_resposta`, `recusado: <texto>`, `sem_welcome`, `expulso: <texto>`, `conexao_perdida`, `servidor_invalido`, `servidor_incompativel`, `erro_interno`.
- Textos na tela (exatos): `CB Multiplayer: conectado (N por perto)`; `CB Multiplayer: desconectado — <frase>` com as frases `entrada expirou; feche o jogo e clique Jogar` (`sem_arquivo`, `ticket_expirado`), `arquivo de entrada inválido` (`arquivo_invalido`), `versão do mod diferente da do servidor` (`protocolo_diferente`), `servidor não respondeu` (`sem_resposta`, `sem_welcome`), `recusado: <texto>`, `<texto>` (expulso), `conexão perdida`, `servidor incompatível` (`servidor_invalido`, `servidor_incompativel`), `erro interno`; e `CB Multiplayer: o servidor não aceitou sua posição` (uma vez).
- Linhas de log que o roteiro manual procura (exatas, o resto da linha é livre): `substituido epoca=<a>→<b>`, `dono epoca=<n>`, `Hello enviado`, `Welcome playerId=<n>`, `mundo: <de> → <para>` (com `InWorld` quando entra), `EntityEnter id=<n>`, `EntityLeave id=<n>`, `boneco id=<n> criado`, `boneco id=<n> recriado`, `boneco id=<n> desistiu`, `boneco id=<n> destruído`, `Input Apparate`, `posicao_recusada`, `bonecos_desligados: <motivo>`, `singleton=ok`, `desconectado: <motivo>`, `encerrando`, e a cada 10 s `contadores snapshots=<n> acks=<n> correcoes=<n> bonecos=<n> erros_jogo=<n>`.
- `cb-mp.json`: `statusNaTela` (bool, padrão `true`), `neutralizacaoExtra` (bool, padrão `false`), `animacaoBoneco` (`"velocidade"` padrão, `"ambulatory"`, `"off"`), `maxBonecos` (0..64, padrão 32).
- Zip do mod: `cbmp-mod.json` = `{"layout":2,"version":"0.2.0","protocol":1,"ue4ss":"3.0.1+4fb9b615","minLauncher":"0.2.0"}`; `loader/dwmapi.dll`; `ue4ss/UE4SS.dll`; `ue4ss/UE4SS-settings.ini` (`ConsoleEnabled=0`, `GuiConsoleEnabled=0`, `GuiConsoleVisible=0`, `EnableHotReloadSystem=0`, `[CrashDump] EnableDumping=0`, `[Hooks]` intacto); `ue4ss/Mods/mods.txt` (`cb-mp : 1`, `Keybinds : 0`); `ue4ss/Mods/cb-mp/enabled.txt`; `ue4ss/Mods/cb-mp/dlls/main.dll`; `licenses/UE4SS-LICENSE.txt`; `licenses/THIRD-PARTY-NOTICES.txt`; `LEIA-ME.txt`.
- Erros novos do launcher (código → mensagem exata): `mod_conflict` → `Já existe outro carregador de mods (UE4SS) na pasta do jogo.`; `mod_needs_admin` → `O launcher não tem permissão para instalar o mod na pasta do jogo.`; `mod_incompatible` → `Este mod precisa de uma versão mais nova do launcher. Atualize e tente de novo.`; `mod_activation_failed` → `Não foi possível ativar o mod na pasta do jogo.`; `game_running` → `O Hogwarts Legacy está aberto. Feche o jogo e tente de novo.`
- Diálogo de conflito (exato): `Encontramos outro carregador de mods (UE4SS) na pasta do jogo. Para jogar no CB Multiplayer é preciso usar o do launcher. Seus arquivos não são apagados: ficam em Phoenix\Binaries\Win64\cbmp-backup e voltam quando você remover o mod do CB Multiplayer nas Configurações. Enquanto isso, seus outros mods do UE4SS ficam desligados.` Botão de elevação: `Instalar com permissão de administrador (uma vez)`. Botões das Configurações: `Remover mod do jogo`, `Abrir pasta de logs do mod`.
- Servidor de teste (`tools/teste-online/server.cfg.json`): `"movement": { "maxSpeedCmS": { "idle": 50, "walk": 600, "run": 1500, "broom": 12000, "swim": 1500, "fall": 8000, "apparate": 100000000 }, "tolerance": 0.2 }`.
- `mod.yml`: caminho `.github/workflows/mod.yml`; gatilhos `push` na `main` (caminhos `client/**`, `proto/**`, `tools/package-mod.ps1`, `tools/check-mod-zip.mjs`, o próprio arquivo) e `workflow_dispatch` sem entradas; checkout de `GITHUB_SHA` sem `ref:`; nunca `pull_request_target`; `windows-2025`; artefato `cbmp-mod` com exatamente um `.zip`; segredo `UE4SS_SUBMODULES_TOKEN`. **Sem o segredo o job termina verde, sem artefato, com um aviso claro** (decisão do plano; ver "Divergências").

## Divergências resolvidas pelo plano

1. **`mod.yml` sem o segredo.** A spec diz "falha com mensagem clara"; a regra do projeto é CI verde sem o segredo. Vale: os passos que precisam do UE4SS são pulados com `::notice::`, o job termina verde e **não** publica artefato. O `release.yml` continua fechado: sem artefato `cbmp-mod` ele não assina nada.
2. **Pasta dos pacotes vcpkg do mod.** O build da raiz usa `C:/cbmp-vi`; o do mod usa `C:/cbmp-vi-mod` (outro manifesto na mesma pasta apagaria pacotes do outro).
3. **`client/src/platform` também compila no build nativo no Windows** (não inclui UE4SS), para `WinConnectFs` e `WinProcessGate` terem teste automático. A spec só dizia que `client/src` compila no build do mod.
4. **Build do mod quebrado entre as Tasks 1 e 7.** A Task 1 troca o `IGame` e remove o `SpikeController`; `client/src` só volta a compilar na Task 7. O build do mod não está em CI nesse intervalo.
5. **Textos de tela sem frase na spec** (`servidor_invalido`, `servidor_incompativel`, `erro_interno`, `arquivo_invalido`, `protocolo_diferente`): definidos em Global Constraints.
6. **`TeleportLocal`**: a spec não diz qual função do jogo usar. Vale `Actor.K2_TeleportTo` (rotação atual) com reserva `Actor.K2_SetActorLocation`; `Biped_Player.SafeTeleportTo` fica anotado para a rodada manual.
7. **Mod de teste do ambiente local** (`tools/dev-env.mjs`, `tools/make-test-kit.mjs`): continua o `mod-p1-v0.1.0` só com LEIA-ME, que o launcher 0.2.0 recusa (`mod_invalid`) fora do modo simular; `dev-env.mjs` ganha `--mod-zip` para usar o pacote real. Registrado em `PENDENCIAS`.
8. **Kit de reconhecimento** (`tools/make-recon-kit.mjs`): continua empacotando o `cb-mp.dll`, agora inerte sem o launcher; o passo do F8 do roteiro antigo deixa de valer (nota na Task 12).
9. **12 tasks** (o alvo era 8–11): o modo elevado do launcher e a documentação ficaram em tasks próprias para cada uma ter um portão de revisão que faça sentido.

## Estrutura de arquivos

```
proto/CMakeLists.txt                         + alvo cbmp_codec (bitstream + messages); cbmp_proto liga nele
CMakeLists.txt                               + client/core, client/tests, client/src/platform (WIN32), client/headless
client/VERSION                               0.2.0
client/vcpkg.json                            manifesto do build do mod (gamenetworkingsockets, nlohmann-json)
client/CMakeLists.txt                        build do mod (UE4SS + core + platform + src)
client/core/include/cbmp/client/             Transform Log Store IGame Limits ModConfig ConnectFile ProcessGate ClaimPolicy
                                             Transport NetClient SnapshotMirror RemoteStore WorldTracker LocalSampler
                                             SendScheduler Interp PuppetDirector Session GnsTransport  (.hpp)
client/core/src/                             um .cpp por módulo
client/tests/                                cbmp_client_tests (Catch2): test_*.cpp, FakeGame.hpp, FakeTransport.hpp, FakeGate.hpp
client/tests/platform/                       cbmp_client_platform_tests (só Windows)
client/headless/                             cbmp-headless: main.cpp, ScriptedGame.{hpp,cpp}, FileConnectFs.hpp
client/src/platform/                         WinProcessGate, WinConnectFs (Win32, sem UE4SS)
client/src/game/                             Ue4ssGame, Reflect (única pasta com Unreal)
client/src/                                  CbMpMod, dllmain, CrashDump, FileHash, SafeCall, Utf8Path
tools/client-e2e.py                          integração: cb-server real + 2 cbmp-headless
tools/build.ps1  tools/package-mod.ps1  tools/check-mod-zip.mjs  tools/lib/zip.mjs
.github/workflows/mod.yml  .github/workflows/native.yml
launcher/src-tauri/src/modpkg.rs             layout 2, cbmp-mod.json, registro novo
launcher/src-tauri/src/modact.rs             ativação, conflito, backup, remoção, estado
launcher/src-tauri/src/admin_setup.rs        modo elevado --mod-admin-setup
launcher/src-tauri/src/{launch,play,settings,game,commands,lib,main}.rs
launcher/ui/src/routes/Settings.svelte  components/PlayStatus.svelte  lib/{api,errors}.ts
tools/teste-online.mjs  tools/teste-online/server.cfg.json  tools/dev-env.mjs  launcher/src-tauri/examples/dev_sign_mod.rs
docs/TESTE-NO-JOGO-5A.md  docs/COMO-FUNCIONA.md  docs/AMBIENTE-TESTE-ONLINE.md  docs/PENDENCIAS.md  launcher/README.md  client/README.md
```

Dependências entre tasks: 2→1; 3→1; 4→1,3; 5→1–4; 6→1–5; 7→1,2,4,5,6; 9→8; 10→8,9; 11→6–10; 12→todas. Precisam do build local do UE4SS: **7 e 11**.

---

### Task 1: Fundação do núcleo do cliente

Leva `client/core` para o build nativo da raiz com Catch2, namespace novo, `IGame` da spec, limites, log por processo e configuração do mod.

**Files:**
- Modify: `proto/CMakeLists.txt`, `CMakeLists.txt` (raiz), `.github/workflows/native.yml` (só `paths`)
- Create: `client/core/CMakeLists.txt` (reescrito), `client/tests/CMakeLists.txt` (reescrito), `client/VERSION`
- Move (git mv) para `client/core/include/cbmp/client/`: `Transform.hpp`, `Log.hpp`, `Store.hpp`; reescrever `IGame.hpp` lá
- Create: `client/core/include/cbmp/client/{Limits.hpp,ModConfig.hpp}`, `client/core/src/ModConfig.cpp`
- Modify: `client/core/src/{Transform.cpp,Log.cpp,Store.cpp}` (namespace, includes, portabilidade)
- Delete: `client/core/include/cbmp/SpikeController.hpp`, `client/core/src/SpikeController.cpp`, `client/tests/{TestFramework.hpp,test_main.cpp,test_spike_controller.cpp,FakeGame.hpp}`
- Test: `client/tests/{test_transform.cpp,test_log.cpp,test_store.cpp,test_mod_config.cpp,test_limits.cpp,test_guard.cpp}`

**Interfaces — consome:** `cbmp::escapeLogText` (`proto/include/cbmp/log_text.hpp`).

**Interfaces — produz:**

```cpp
// CMake: alvos cbmp_codec (proto: bitstream.cpp + messages.cpp, include proto/include, sem libsodium),
//        cbmp_client_core (STATIC; PUBLIC: cbmp_codec, nlohmann_json::nlohmann_json, ${CBMP_GNS_TARGET}),
//        cbmp_client_tests (Catch2::Catch2WithMain; define CBMP_CLIENT_CORE_DIR="<abs>/client/core").
// Todos os cabeçalhos abaixo: namespace cbmp::client, em <cbmp/client/X.hpp>.

// Transform.hpp (movido, sem mudança de conteúdo)
struct Vec3 { double x{}, y{}, z{}; };  struct Rot3 { double pitch{}, yaw{}, roll{}; };  struct Transform { Vec3 pos{}; Rot3 rot{}; };
std::string FormatTransform(const Transform& t);  Transform OffsetForward(const Transform& t, double distanceCm);

// IGame.hpp — exatamente o bloco da spec (MoveMode, LocalState, WorldSample, PuppetPose, PuppetId, PuppetResult, IGame) e:
struct GameError : std::runtime_error { using std::runtime_error::runtime_error; };

// Log.hpp
enum class Level { Info, Warn, Error };
using LogFn = std::function<void(Level, std::string_view)>;
std::string FormatLine(Level level, std::string_view msg, const std::tm& when, int millis);   // formato atual
std::string LogFileName(const std::tm& when, uint32_t pid);        // "cb-mp-20261111-201450-1234.log"
void PruneLogs(const std::filesystem::path& dir, size_t keep);     // mantém os `keep` cb-mp-*.log mais novos (pelo nome)
class FileLogger {
public:
    bool Open(const std::filesystem::path& path);                  // append, UTF-8
    bool IsOpen() const;
    void Write(Level level, std::string_view msg);                 // thread-safe, flush por linha
    void SetSecret(std::string_view secret);                       // < 16 bytes: ignorado
    void ClearSecret();                                            // sobrescreve a cópia com zeros
};
FileLogger& Log();

// Limits.hpp — namespace cbmp::client::limits, tudo inline constexpr
size_t kMaxMessageBytes = 8192, kMaxMessagesPerTick = 512, kMaxPendingMessages = 4096, kMaxEntities = 255,
       kHistoryTicks = 32, kMaxParts = 8, kMaxNameBytes = 64, kMaxReasonBytes = 255,
       kMaxPointerBytes = 4096, kMaxConnectBytes = 16384, kInterpQueue = 32, kLogKeep = 10;
int    kMaxDecodeErrors = 20, kPointerSearchLevels = 5, kMinTickRateHz = 5, kMaxTickRateHz = 60, kDefaultSendHz = 20,
       kDefaultMaxPuppets = 32, kHardMaxPuppets = 64, kSpawnRetries = 3, kMaxRecreatesPerMinute = 5,
       kMoveErrorsToGone = 3, kGameErrorsToDisable = 20, kTickExceptionLimit = 100, kMaxVelCmS = 32767;
int64_t kPointerMaxAgeSec = 600, kPointerMaxFutureSec = 120, kTicketMinRemainingSec = 10;
double kDecodeErrorWindowSec = 10, kClaimDelayFirstSec = 45, kClaimDelaySuccessorSec = 5, kClaimRetrySec = 5,
       kConnectTimeoutSec = 10, kHelloDeadlineSec = 1, kWelcomeTimeoutSec = 10,
       kInterpDelaySec = 0.100, kMaxInterpDelaySec = 0.400, kMaxExtrapolationSec = 0.250, kSnapDistanceCm = 500,
       kClockWindowSec = 10, kClockResetSec = 2,
       kCorrectionIgnoreCm = 100, kCorrectionTeleportMaxCm = 2000, kTeleportMinIntervalSec = 1,
       kMaxCoordCm = 2'000'000'000.0, kJumpDistanceCm = 3000, kIdleMaxCmS = 10, kWalkMaxCmS = 380, kRunMaxCmS = 1000,
       kInWorldStableSec = 2, kOriginRadiusCm = 100, kPuppetRadiusCm = 40000, kSpawnRetrySec = 1, kRecreateDelaySec = 1,
       kGameErrorWindowSec = 10, kTickExceptionWindowSec = 60, kExceptionSummarySec = 10, kStatsIntervalSec = 10;
int    kEndRejected = 1001, kEndLimits = 1002, kEndProtocol = 1003, kEndKicked = 1004;   // k_ESteamNetConnectionEnd_App_Min = 1000

// ModConfig.hpp
enum class PuppetAnim { Velocity, Ambulatory, Off };
struct ModConfig { bool statusOnScreen = true; bool extraNeutralization = false; PuppetAnim puppetAnim = PuppetAnim::Velocity; int maxPuppets = 32; };
ModConfig ParseModConfig(std::span<const uint8_t> bytes);   // JSON inválido, > 4 KiB ou campo inválido → padrão (campo a campo)
```

**Passos**

- [ ] **1. `cbmp_codec`.** Em `proto/CMakeLists.txt`: `add_library(cbmp_codec STATIC src/bitstream.cpp src/messages.cpp)` com `target_include_directories(cbmp_codec PUBLIC include)`; tirar esses dois arquivos de `cbmp_proto` e fazer `target_link_libraries(cbmp_proto PUBLIC cbmp_codec …)`. Os alvos de fuzz ficam como estão. Rodar `ctest -R "proto|snapshot|bitstream|ticket"` → tudo passa como antes.
- [ ] **2. Raiz.** Em `CMakeLists.txt`, depois de `add_subdirectory(proto)`: `add_subdirectory(client/core)` e `add_subdirectory(client/tests)`. `client/core/CMakeLists.txt`: `find_package(nlohmann_json CONFIG REQUIRED)`; `add_library(cbmp_client_core STATIC …)`; não chama `find_package(GameNetworkingSockets)` (usa `${CBMP_GNS_TARGET}` definido pela raiz). Opções de aviso herdadas da raiz (`/W4 /permissive- /utf-8 /EHsc`, `-Wall -Wextra -Wpedantic`): zero avisos novos.
- [ ] **3. Portar os testes para Catch2 (devem falhar: cabeçalhos ainda no lugar antigo).** `test_transform.cpp`, `test_log.cpp`, `test_store.cpp` com `TEST_CASE("client: …")` e os mesmos valores dos testes atuais. Rodar o build → falha de compilação por `<cbmp/client/...>` inexistente.
- [ ] **4. Mover e renomear.** `git mv` dos três cabeçalhos; namespace `cbmp::client`; `Log.cpp` portável (`localtime_s` no Windows, `localtime_r` fora; sem `Windows.h`). Apagar `SpikeController.*`, o `TestFramework` e o `FakeGame` antigo. Reescrever `IGame.hpp` com o bloco da spec + `GameError`. **Não mexer em `client/src/`** (volta a compilar na Task 7). Rodar → os três testes portados passam.
- [ ] **5. Testes novos (falham primeiro).**
  - `test_log.cpp`: `LogFileName` para 2026-11-11 20:14:50, pid 1234 → `cb-mp-20261111-201450-1234.log`; `PruneLogs` com 12 arquivos `cb-mp-*.log` + 1 `outro.txt` e `keep = 10` → sobram os 10 de nome maior e o `outro.txt`; com `SetSecret("0123456789abcdefTICKET")`, `Write` de uma linha que contém o segredo grava `linha omitida (continha segredo)` e o arquivo não contém o segredo; depois de `ClearSecret` a mesma linha é gravada; segredo de 8 bytes não filtra nada.
  - `test_mod_config.cpp`: vazio/`{`/`[]` → padrões; `{"statusNaTela":false,"neutralizacaoExtra":true,"animacaoBoneco":"ambulatory","maxBonecos":64}` → todos lidos; `maxBonecos` 65, −1 ou `"x"` → 32; `animacaoBoneco:"x"` → `Velocity`; `"off"` → `Off`; 5 KiB de entrada → padrões.
  - `test_limits.cpp`: `static_assert`/`REQUIRE` de cada valor listado em "Valores da spec" (um `REQUIRE` por constante).
  - `test_guard.cpp`: percorre `CBMP_CLIENT_CORE_DIR` (`.hpp`/`.cpp`) e falha, citando arquivo e linha, se achar `#include` de `Windows.h` (qualquer caixa), `<Unreal/`, `<Mod/`, `DynamicOutput`, `UE4SS` ou `<Helpers/`. Um segundo `TEST_CASE` prova o detector com um texto de exemplo.
- [ ] **6. Implementar** `LogFileName`, `PruneLogs`, filtro de segredo, `Limits.hpp`, `ModConfig`. Rodar → verde.
- [ ] **7. `native.yml`:** acrescentar a `paths` (push e pull_request) `client/core/**`, `client/tests/**`, `client/headless/**`, `client/src/platform/**`, `tools/client-e2e.py`. `client/VERSION` = `0.2.0` (uma linha).
- [ ] **8. Verificação.** `cmake --build --preset windows-release` (tudo) e `ctest --test-dir build-native/windows-release -C Release --output-on-failure` (tudo): verde, sem avisos novos.
- [ ] **9. Commit** — `feat(client): núcleo no build nativo com Catch2, IGame novo, limites, log por processo e cb-mp.json` — e push.

---

### Task 2: Arquivo de conexão e dois processos

Validação do ponteiro e do `connect.json`, regra de quem resgata, e as duas portas Win32 com teste real no Windows.

**Files:**
- Create: `client/core/include/cbmp/client/{ConnectFile.hpp,ProcessGate.hpp,ClaimPolicy.hpp}`, `client/core/src/{ConnectFile.cpp,ClaimPolicy.cpp}`
- Create: `client/src/platform/{CMakeLists.txt,WinConnectFs.hpp,WinConnectFs.cpp,WinProcessGate.hpp,WinProcessGate.cpp}`
- Create: `client/tests/FakeGate.hpp`, `client/tests/{test_connect_file.cpp,test_claim_policy.cpp}`, `client/tests/platform/{CMakeLists.txt,test_win_connect_fs.cpp,test_win_process_gate.cpp}`
- Modify: `CMakeLists.txt` (raiz: `if(WIN32)` → `client/src/platform` e `client/tests/platform`), `client/core/CMakeLists.txt`, `client/tests/CMakeLists.txt`

**Interfaces — consome:** Task 1 (`limits::*`, `cbmp_client_core`, `cbmp_client_tests`).

**Interfaces — produz:**

```cpp
// ConnectFile.hpp
struct RunPointer { std::string runDir; std::string launchId; int64_t writtenAt = 0; std::string launcher; };   // runDir em UTF-8, como veio
std::optional<RunPointer> ParsePointer(std::span<const uint8_t> bytes, int64_t nowUnix, std::string* why = nullptr);
bool IsValidRunDir(std::string_view utf8);          // "X:\...\run": letra de unidade, sem "\\" inicial, sem segmento "." ou "..", último segmento "run"
bool IsLaunchId(std::string_view s);                // 32 hex minúsculos
bool IsIpLiteral(std::string_view host);            // IPv4 a.b.c.d ou IPv6 (com ou sem colchetes); nunca nome
std::optional<std::filesystem::path> FindPointerFile(const std::filesystem::path& dllDir);   // dllDir e até 5 pais; nome "cbmp-run.json"
std::string ConnectPathFor(const RunPointer& p);    // runDir + "\connect.json"

struct ConnectInfo { std::string host; uint16_t port = 0; std::string ticket; std::string serverId; uint32_t protocol = 0; std::string launchId; };
enum class ConnectError { None, Invalid, ProtocolMismatch, TicketExpired };
struct ConnectParse { std::optional<ConnectInfo> info; ConnectError error = ConnectError::None; std::string why; };
ConnectParse ParseConnect(std::span<const uint8_t> bytes, std::string_view expectedLaunchId, int64_t nowUnix);
std::optional<int64_t> TicketExp(std::string_view ticket);   // claim "exp" do JSON antes do ponto; nullopt se não der para ler
void SecureZero(std::string& s);                              // sobrescreve com zeros (escrita volátil) e limpa

enum class ClaimRead { Ok, Missing, Busy, Rejected };
struct ClaimResult { ClaimRead status = ClaimRead::Missing; std::vector<uint8_t> bytes; };
class IConnectFs { public: virtual ~IConnectFs() = default;
    virtual bool Exists() = 0;                        // só consulta
    virtual ClaimResult ClaimAndDelete() = 0; };      // abre exclusivo, confere, lê e apaga

// ProcessGate.hpp
class IProcessGate { public: virtual ~IProcessGate() = default;
    virtual uint64_t Epoch() const = 0;               // valor guardado ao iniciar (≥ 1)
    virtual uint64_t Latest() = 0;                    // contador atual
    virtual bool IsCurrent() = 0;                     // Latest() == Epoch()
    virtual bool TryOwn() = 0; };                     // toma o mutex sem esperar; true também se já é dono

// ClaimPolicy.hpp
enum class ClaimDecision { Wait, Claim, Superseded };
ClaimDecision DecideClaim(bool isCurrent, bool inWorld, double secondsSinceFirstTick, uint64_t epoch);

// client/src/platform (alvo cbmp_client_platform, só WIN32; liga em cbmp_client_core; SEM UE4SS)
class WinConnectFs final : public cbmp::client::IConnectFs { public: explicit WinConnectFs(std::filesystem::path connectJson); … };
class WinProcessGate final : public cbmp::client::IProcessGate { public:
    WinProcessGate();                                                   // nomes da spec
    WinProcessGate(std::wstring epochName, std::wstring ownerName);     // testes
    bool Ok() const; … };

// client/tests/FakeGate.hpp
struct FakeGate : IProcessGate { uint64_t epoch = 1, latest = 1; bool ownOk = true; int ownCalls = 0; … };
struct FakeConnectFs : IConnectFs { bool exists = true; std::deque<ClaimResult> results; int claimCalls = 0; … };  // sem resultados na fila → Missing
```

**Regras**

- `ParsePointer`: > 4096 bytes, não-JSON, não-objeto, `v != 1`, `runDir` inválido, `launchId` inválido, `writtenAt` não inteiro, idade > 600 s, futuro > 120 s → `nullopt` (com `why` curto). Campos a mais são ignorados.
- `ParseConnect`: > 16 384 bytes, JSON inválido, `host` que não é IP literal, `port` fora de 1..65535, `ticket` vazio ou > 2048 bytes, `serverId` vazio ou > 64 bytes, `launchId` ausente ou diferente → `Invalid`; `protocol != cbmp::kProtocolVersion` → `ProtocolMismatch`; `TicketExp` legível e `exp − nowUnix < 10` → `TicketExpired`. `contentDir` nunca é lido. Em erro, o ticket lido é zerado antes de devolver.
- `DecideClaim`: `!isCurrent` → `Superseded`; `inWorld` → `Claim`; senão `Claim` quando `secondsSinceFirstTick ≥ 45` (época 1) ou `≥ 5` (época > 1); senão `Wait`.
- `WinConnectFs::ClaimAndDelete`: `CreateFileW(path, GENERIC_READ | DELETE, 0 /*sem compartilhar*/, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)`; `ERROR_FILE_NOT_FOUND`/`PATH_NOT_FOUND` → `Missing`; `ERROR_SHARING_VIOLATION` → `Busy`; depois confere `GetFileInformationByHandle` (não é pasta nem ponto de reanálise), tamanho ≤ 16 KiB e dono (`GetSecurityInfo`, `OWNER_SECURITY_INFORMATION`) igual ao SID do token do processo → senão `Rejected` (o arquivo some ao fechar mesmo assim); lê tudo; fecha. `Exists` usa `GetFileAttributesW`.
- `WinProcessGate`: `CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 8, L"Local\\cbmp-mod-epoch-v1")` + `MapViewOfFile`; `InterlockedIncrement64` no construtor; mutex `CreateMutexW(nullptr, FALSE, L"Local\\cbmp-mod-owner-v1")` + `WaitForSingleObject(h, 0)` (aceita `WAIT_ABANDONED`); mantém os handles até o destrutor. Nada de segredo em nenhum dos dois objetos.

**Passos**

- [ ] **1. Testes do núcleo (falham).** `test_connect_file.cpp`: ponteiro válido; cada recusa da lista acima (um `SECTION` por motivo, incluindo `runDir` = `\\srv\x\run`, `C:\a\..\run`, `C:\a\rum`, relativo; `writtenAt` 601 s atrás; 121 s à frente; exatamente 600 e 120 aceitos); `FindPointerFile` com árvore temporária `cbmp/atual/ue4ss/Mods/cb-mp/dlls` (acha em `cbmp/`), 6 níveis acima (não acha) e sem arquivo; `ConnectPathFor`; `connect.json` válido (igual ao que o launcher grava, com `contentDir` e `contentReadOnly`); cada recusa (tamanho, host `exemplo.com`, host `1.2.3`, porta 0, `launchId` diferente, protocolo 2 → `ProtocolMismatch`, ticket com `exp` em 9 s → `TicketExpired`, `exp` em 10 s aceito, ticket ilegível aceito); `IsIpLiteral` (`127.0.0.1`, `::1`, `[2001:db8::1]`, `256.1.1.1` falso, `a.b` falso); `SecureZero` deixa a string vazia. `test_claim_policy.cpp`: tabela completa (substituído; `InWorld` imediato; época 1 em 44,9 s `Wait` e 45 s `Claim`; época 2 em 4,9 s `Wait` e 5 s `Claim`).
- [ ] **2. Implementar o núcleo** (JSON com nlohmann, sem exceção escapando; base64url mínimo interno para o `exp`). Rodar `ctest -R "^client: "` → verde.
- [ ] **3. Testes da plataforma (falham).** `test_win_connect_fs.cpp` (pasta temporária): arquivo comum → `Ok` com os bytes e o arquivo não existe mais; ausente → `Missing`; aberto por outro handle sem compartilhamento → `Busy` e o arquivo continua lá; 17 KiB → `Rejected` e apagado; pasta no lugar do arquivo → `Rejected`/`Missing` sem apagar a pasta; `Exists` verdadeiro/falso. `test_win_process_gate.cpp` (nomes únicos por teste, `Local\cbmp-test-<pid>-<n>`): primeiro portão época 1 e `IsCurrent`; segundo portão (mesmos nomes) época 2, o primeiro deixa de ser atual; `TryOwn` do primeiro `true`, do segundo `false` (outra thread: mutex é reentrante por thread, então o segundo portão é criado e testado em `std::thread`); depois de destruir todos, um novo começa em 1.
- [ ] **4. Implementar a plataforma.** Rodar `ctest -R "^client: "` → verde no Windows (os dois executáveis).
- [ ] **5. Verificação.** Build e `ctest` completos da raiz, verdes.
- [ ] **6. Commit** — `feat(client): ponteiro do launcher, connect.json e regra dos dois processos` — e push.

---

### Task 3: Rede do cliente e espelho de snapshots

**Files:**
- Create: `client/core/include/cbmp/client/{Transport.hpp,NetClient.hpp,SnapshotMirror.hpp,RemoteStore.hpp}`, `client/core/src/{NetClient.cpp,SnapshotMirror.cpp,RemoteStore.cpp}`
- Create: `client/tests/FakeTransport.hpp`, `client/tests/{test_net_client.cpp,test_snapshot_mirror.cpp,test_mirror_e2e.cpp,test_remote_store.cpp}`
- Modify: `client/core/CMakeLists.txt`, `client/tests/CMakeLists.txt`
- Ler (não alterar): `proto/include/cbmp/messages.hpp`, `proto/src/messages.cpp`, `server/src/snapshots.cpp`, `proto/tests/test_snapshot.cpp`, `bots/src/bot.cpp`

**Interfaces — consome:** Task 1 (`limits::*`, `Level`, `LogFn`), `cbmp::` da `proto/` (`Message`, `encode`, `decode`, `isReliable`, `typeOf`, `applySnapshotEntity`, `makeSnapshotEntity`, `splitSnapshot`, `escapeLogText`).

**Interfaces — produz:**

```cpp
// Transport.hpp
enum class LinkState { Idle, Connecting, Connected, Closed };
struct LinkEnd { int code = 0; std::string debug; };
class ITransport { public: virtual ~ITransport() = default;
    virtual bool Connect(const std::string& ipLiteral, uint16_t port) = 0;        // false = falha imediata
    virtual bool Send(std::span<const uint8_t> bytes, bool reliable) = 0;
    virtual size_t Receive(std::vector<std::vector<uint8_t>>& out, size_t max) = 0; // acrescenta até `max` mensagens
    virtual size_t Pending() const = 0;                                            // ainda na fila local depois do Receive
    virtual LinkState State() = 0;
    virtual LinkEnd End() const = 0;                                               // válido em Closed
    virtual void Close() = 0; };                                                   // sem esperar

// NetClient.hpp
struct NetCounters { uint64_t received = 0, oversized = 0, decodeErrors = 0, snapshots = 0, acks = 0, events = 0, corrections = 0, enters = 0, leaves = 0, inputs = 0; };
class INetSink { public: virtual ~INetSink() = default;
    virtual void OnWelcome(const cbmp::Welcome& w) = 0;
    virtual void OnSnapshot(const cbmp::Snapshot& s, double now) = 0;
    virtual void OnEntityEnter(const cbmp::EntityEnter& e, double now) = 0;
    virtual void OnEntityLeave(const cbmp::EntityLeave& e) = 0;
    virtual void OnCorrection(const cbmp::Correction& c, double now) = 0;
    virtual void OnRefusal(std::string_view sanitizedText, bool kick) = 0; };      // Reject (kick=false) ou Kick
enum class NetFault { None, TooManyDecodeErrors, QueueOverflow };
class NetClient { public:
    NetClient(ITransport& transport, INetSink& sink);
    bool SendHello(const std::array<uint8_t, 32>& buildHash, std::string& ticket);  // confiável; zera `ticket` e o buffer codificado
    void SendInput(uint32_t seq, const cbmp::PlayerState& s);                       // não confiável
    void SendAck(uint32_t tick);                                                    // não confiável
    NetFault Pump(double now);                                                      // ≤ 512 mensagens por chamada
    const NetCounters& Counters() const; };
std::string SanitizeText(std::string_view raw, size_t maxBytes);                    // corta sem partir UTF-8 e aplica escapeLogText

// SnapshotMirror.hpp
struct MirrorEntity { uint16_t id = 0; cbmp::PlayerState s; };
struct TickRecord { uint32_t tick = 0; std::vector<MirrorEntity> ents; };          // ids crescentes
struct AppliedTick { const TickRecord* record = nullptr; std::vector<uint16_t> changed; };   // changed = ids recebidos neste tick
class SnapshotMirror { public:
    std::optional<AppliedTick> OnSnapshot(const cbmp::Snapshot& part);              // valor = tick completo e aplicado: confirme com SnapshotAck
    void Forget(uint16_t id);                                                       // EntityLeave: apaga o id de todos os registros
    void Reset();
    uint32_t LastAppliedTick() const;
    const TickRecord* Find(uint32_t tick) const;
    uint64_t Dropped() const; };                                                    // partes/ticks descartados

// RemoteStore.hpp
struct RemoteEntity { uint16_t id = 0; std::string name; cbmp::PlayerState last; double lastAt = 0; };   // name já saneado
class RemoteStore { public:
    bool Enter(const cbmp::EntityEnter& e, double now);          // false = cheio (255); reentrada do mesmo id substitui
    bool Leave(uint16_t id);
    bool Known(uint16_t id) const;
    void Update(uint16_t id, const cbmp::PlayerState& s, double now);              // id desconhecido: ignora
    const std::map<uint16_t, RemoteEntity>& All() const;
    void Clear(); };

// client/tests/FakeTransport.hpp
struct FakeTransport : ITransport {
    struct Sent { std::vector<uint8_t> bytes; bool reliable; };
    std::vector<Sent> sent; std::deque<std::vector<uint8_t>> inbox; LinkState state = LinkState::Idle; LinkEnd end;
    bool connectOk = true; int connectCalls = 0, closeCalls = 0; std::string lastHost; uint16_t lastPort = 0;
    void Push(const cbmp::Message& m);            // encode → inbox
    void PushRaw(std::vector<uint8_t> b);
    std::vector<cbmp::Message> SentMessages() const; … };   // Connect: state = Connecting (o teste muda para Connected)
```

**Regras**

- `NetClient::Pump`: `Receive(buf, 512)`; para cada mensagem: tamanho > 8192 → `oversized++` e segue; `cbmp::decode` lança, ou o tipo é `Hello`/`Input`/`SnapshotAck` (sentido errado) → `decodeErrors++` e registra o instante; mais de 20 erros dentro de 10 s → `TooManyDecodeErrors`; depois do laço, `Pending() > 4096` → `QueueOverflow`. `Event` → `events++` e descarta. `Reject`/`Kick` → `OnRefusal(SanitizeText(reason, 255), kick)`. Nada do servidor é usado como caminho.
- `SendHello`: `Hello{kProtocolVersion, buildHash, ticket}`; depois do `Send`, sobrescreve com zeros o buffer codificado, a cópia dentro da mensagem e `ticket` (`SecureZero`).
- `SnapshotMirror::OnSnapshot`:
  1. `partCount` > 8 ou `ents.size()` > 255 → descarta;
  2. `tick < tick em montagem` → descarta; `tick == último aplicado` → descarta; `tick > tick em montagem` → reinicia a montagem (zera partes); `partCount` diferente do da montagem em curso → descarta a parte;
  3. guarda a parte (repetida: ignora); faltando partes → `nullopt`;
  4. completo: se `baselineTick != 0` e o registro dele não está no anel → descarta o tick inteiro, **sem** ack (`nullopt`);
  5. registro = cópia do registro da baseline (vazio se `baselineTick == 0`); para cada entidade recebida (todas as partes, por id): `applySnapshotEntity(estado na baseline ou PlayerState{}, e)` e insere/atualiza; mais de 255 entidades no resultado → descarta sem ack;
  6. grava no anel de 32 (o mais velho sai), devolve `AppliedTick{registro, ids recebidos}`.
- `Forget(id)`: remove o id de todos os registros do anel e da montagem em curso.
- `RemoteStore`: nome = `SanitizeText(name, 64)`.

**Passos**

- [ ] **1. Testes (falham).**
  - `test_net_client.cpp`: `SendHello` manda 1 mensagem confiável que decodifica para `Hello` com protocolo 1, o hash e o ticket, e deixa a string do chamador vazia; `SendInput`/`SendAck` não confiáveis; despacho de cada tipo para o `INetSink` de teste; `Event{"cb:reply"}` só conta; mensagem de 8193 bytes conta em `oversized` e não chega ao sink; 20 mensagens inválidas em 10 s → `None`, a 21ª → `TooManyDecodeErrors`; 21 espalhadas em 30 s → `None`; 600 mensagens na caixa → uma chamada processa 512; `Pending` 4097 → `QueueOverflow`; `Input` vindo do servidor conta como erro; `Reject` com 300 bytes e `\n` → texto ≤ 255 bytes com `\n` escapado; `SanitizeText` não parte um caractere UTF-8 de 3 bytes no corte.
  - `test_snapshot_mirror.cpp`: tick de 1 parte sem baseline; 3 partes fora de ordem (ack só na última); parte repetida; parte de tick antigo no meio da montagem; tick novo reinicia a montagem; baseline presente (delta aplicado sobre o registro); baseline fora do anel → sem ack e `Dropped` sobe; entidade fora da baseline aplicada sobre `PlayerState{}`; `Forget` apaga de todos os registros e uma baseline posterior não a ressuscita; 33 ticks → o primeiro saiu do anel; `partCount` inconsistente; 256 entidades → descarta.
  - `test_mirror_e2e.cpp` (**teste espelho**, fecha a pendência): mini-servidor de teste com 40 entidades andando (RNG fixo `std::mt19937{1234}`), 600 ticks; a cada tick monta o snapshot como `server/src/snapshots.cpp` (baseline = registro do último tick confirmado se ≤ 31 ticks; `makeSnapshotEntity` contra o estado conhecido; `splitSnapshot(…, 400, …)` para forçar várias partes; registro do servidor = baseline restrita aos visíveis + aplicado), embaralha as partes, perde 30% delas e 30% dos acks; a cada 50 ticks uma entidade sai (o servidor faz o `forget`, o cliente recebe `Forget`) e outra entra. Asserção: para todo tick que o cliente confirmou, `mirror.Find(tick)->ents` é igual (ids e `PlayerState`) ao registro do servidor para o mesmo tick. Exigir ≥ 100 ticks confirmados e ≥ 1 tick descartado por baseline ausente (ajuste a idade máxima do teste para provocar).
  - `test_remote_store.cpp`: enter/leave/known/update; 256º `Enter` → `false`; nome com controle escapado e cortado em 64.
- [ ] **2. Implementar** os quatro módulos. Rodar `ctest -R "^client: "` → verde.
- [ ] **3. Verificação.** Build e `ctest` completos, verdes.
- [ ] **4. Commit** — `feat(client): NetClient, espelho de snapshots com histórico e teste espelho com perda` — e push.

---

### Task 4: Mundo, amostragem, interpolação e bonecos

**Files:**
- Create: `client/core/include/cbmp/client/{WorldTracker.hpp,LocalSampler.hpp,SendScheduler.hpp,Interp.hpp,PuppetDirector.hpp}`, `client/core/src/{WorldTracker.cpp,LocalSampler.cpp,SendScheduler.cpp,Interp.cpp,PuppetDirector.cpp}`
- Create: `client/tests/FakeGame.hpp`, `client/tests/{test_world_tracker.cpp,test_local_sampler.cpp,test_send_scheduler.cpp,test_interp.cpp,test_puppet_director.cpp}`
- Modify: `client/core/CMakeLists.txt`, `client/tests/CMakeLists.txt`

**Interfaces — consome:** Task 1 (`IGame` e tipos, `limits::*`, `LogFn`, `Vec3`), Task 3 (`RemoteStore`, `RemoteEntity`), `cbmp::PlayerState`, `cbmp::MoveState`, `cbmp::quantizeYaw/dequantizeYaw/quantizePitch` (`proto/include/cbmp/quantize.hpp`).

**Interfaces — produz:**

```cpp
// WorldTracker.hpp
enum class WorldPhase { Frontend, Loading, InWorld };
struct WorldChange { bool worldChanged = false, pawnChanged = false; };
bool IsMenuWorld(std::string_view worldName);                        // "RootLevel"
class WorldTracker { public:
    WorldPhase Update(const WorldSample& w, const std::optional<LocalState>& local, double now);
    WorldPhase Phase() const;
    WorldChange TakeChange();                                        // acumulado desde a última chamada; zera
    const std::string& WorldName() const; };

// LocalSampler.hpp
cbmp::MoveState ClassifyMove(MoveMode mode, double speedCmS);
class LocalSampler { public:
    void MarkJump();                                                 // próximo Sample sai com Apparate (entrada em InWorld, ressincronização)
    std::optional<cbmp::PlayerState> Sample(const LocalState& s);    // nullopt = amostra pulada; guarda a posição enviada
    bool LastWasJump() const; };

// SendScheduler.hpp
class SendScheduler { public:
    void SetRate(int hz);                                            // período = 1/hz
    void Reset();                                                    // próximo Due é verdadeiro
    bool Due(double now); };                                         // verdadeiro no máximo 1x por período; próximo = now + período

// Interp.hpp
struct InterpPose { Vec3 pos; double yawDeg = 0; Vec3 vel; uint8_t moveState = 0; bool snapped = false; };
class Interp { public:
    void SetTickRate(int hz);
    void OnTick(uint32_t tick, double arrival);                      // um por tick aplicado: alimenta o relógio
    void Push(uint16_t id, uint32_t tick, const cbmp::PlayerState& s);            // amostra de snapshot
    void PushNow(uint16_t id, const cbmp::PlayerState& s, double arrival);        // EntityEnter (sem tick)
    void Remove(uint16_t id);
    void Clear();
    std::optional<InterpPose> Sample(uint16_t id, double now);       // nullopt = sem amostra
    double DelayOf(uint16_t id) const; };

// PuppetDirector.hpp
struct PuppetStats { int alive = 0; uint64_t created = 0, destroyed = 0, recreated = 0, givenUp = 0, gameErrors = 0; bool disabled = false; std::string disabledWhy; };
class PuppetDirector { public:
    PuppetDirector(IGame& game, const RemoteStore& store, Interp& interp, LogFn log, int maxPuppets);
    void SetSelf(uint16_t playerId);
    void Update(double now, const Vec3& localPos);                   // chamar só em Playing
    void OnEntityLeave(uint16_t id);
    void ReleaseAll(bool worldChanged);                              // true → ForgetAllPuppets; false → DestroyPuppet de cada um
    const PuppetStats& Stats() const; };

// client/tests/FakeGame.hpp — implementa o IGame novo
struct FakeGame : IGame {
    WorldSample world{1, "Overland", 10, true, false}; std::optional<LocalState> local;
    std::map<uint32_t, PuppetPose> puppets; uint32_t nextId = 1;
    int spawnCalls = 0, moveCalls = 0, destroyCalls = 0, forgetAllCalls = 0, spawnFailures = 0 /* próximos N SpawnPuppet → nullopt */;
    std::set<uint32_t> gone; bool throwOnMove = false, throwOnSpawn = false, throwOnSample = false;
    bool singletonIntact = true, statusOk = true; std::vector<std::string> statuses; std::vector<Vec3> teleports; bool teleportOk = true; … };
```

**Regras**

- `WorldTracker`: `InWorld` quando, ao mesmo tempo, `hasPawn`, `local` presente, `!IsMenuWorld(worldName)`, `!frontendHint`, `worldId` e `pawnId` iguais há ≥ 2 s, e `hypot(x, y) > 100`. Senão `Frontend` se `IsMenuWorld(worldName)` ou `frontendHint`; senão `Loading`. Mudança de `worldId` marca `worldChanged`; de `pawnId` (mesmo mundo) marca `pawnChanged`; qualquer uma reinicia os 2 s.
- `LocalSampler::Sample`: qualquer componente de `pos`, `yawDeg` ou `vel` não finito, ou `|pos|` de algum eixo > 2 000 000 000 → `nullopt` (e não altera a posição guardada). `pos` = `lround`; `yaw` = `quantizeYaw`; `pitch` = `quantizePitch(0)`; `vel` limitada a ±32767 por eixo; estado = `ClassifyMove(mode, norma da velocidade)`; se `MarkJump` pendente ou distância à última posição enviada > 3000 cm → `Apparate` (e consome a marca). `ClassifyMove`: `Falling`→`Fall`, `Swimming`→`Swim`, `Flying`→`Broom`; `Ground` e `Other`: < 10 `Idle`, ≤ 380 `Walk`, ≤ 1000 `Run`, acima `Broom`.
- `Interp` (todas as amostras guardadas no relógio local):
  - `OnTick`: candidato = `arrival − tick/hz`; deslocamento = mínimo dos candidatos dos últimos 10 s; se o novo mínimo difere do deslocamento em uso por mais de 2 s → `Clear()` das filas e adota o novo.
  - `Push`: `t = tick/hz + deslocamento` (sem `OnTick` ainda: ignora); `PushNow`: `t = arrival`. Fila por entidade de 32 (a mais velha sai); amostra com `t` ≤ a última é ignorada.
  - atraso da entidade = `clamp(2 × média dos últimos 4 intervalos entre amostras consecutivas menores que 1 s, 0.100, 0.400)`; sem intervalo válido → 0.100.
  - `Sample`: `r = now − atraso`. Antes da primeira amostra → a primeira. Entre A e B: se `B.state == Apparate` ou `|B.pos − A.pos| > 500` → pose de A (até `r` alcançar B), sem interpolar; senão posição linear, yaw pelo menor arco (`dequantizeYaw`), velocidade de B. Depois da última amostra L: `pos = L.pos + L.vel × min(r − t_L, 0.250)`. `snapped = true` no primeiro `Sample` de uma entidade e sempre que a pose devolvida estiver a mais de 500 cm da pose devolvida antes.
- `PuppetDirector::Update` (só entidades `Known`, nunca `playerId`):
  - candidatas = entidades com pose (`Interp::Sample`) a ≤ 40 000 cm de `localPos`, ordenadas pela distância, cortadas em `maxPuppets` (0..64; 0 = nenhum boneco);
  - boneco de entidade que saiu das candidatas → `DestroyPuppet`;
  - sem boneco: `SpawnPuppet(pose)`; `nullopt` → nova tentativa em 1 s, até 3 vezes, depois desiste da entidade (`boneco id=N desistiu`); sucesso → `boneco id=N criado`, depois `PlayerSingletonIntact()`: verdadeiro → `singleton=ok` (uma vez por sessão); falso → `DestroyPuppet`, confere de novo, desliga os bonecos (`bonecos_desligados: singleton`) e destrói os demais;
  - com boneco: `MovePuppet`; `Gone` → agenda recriação em 1 s (`boneco id=N recriado` ao recriar), no máximo 5 por minuto por entidade, depois `desistiu`; `GameError` em `MovePuppet` 3 vezes seguidas no mesmo boneco → `DestroyPuppet` e trata como `Gone`; `Failed` conta como erro de jogo;
  - 20 `GameError` em 10 s no total → `bonecos_desligados: erros` e destrói todos; desligado, `Update` não faz mais nada;
  - nenhum valor não finito chega ao `IGame` (pose conferida antes de cada chamada; pose inválida = boneco não é movido naquele tick).
- `OnEntityLeave` → `DestroyPuppet` + `boneco id=N destruído`. `ReleaseAll(true)` → `ForgetAllPuppets()` e esquece os ids; `ReleaseAll(false)` → `DestroyPuppet` de cada um. As contagens de tentativa e desistência são zeradas no `ReleaseAll`.

**Passos**

- [ ] **1. Testes (falham).**
  - `test_world_tracker.cpp`: sem pawn → `Loading`; `RootLevel` → `Frontend`; `frontendHint` → `Frontend`; `Overland` com pawn em (0,0,90) → `Loading`; em (5000,0,0) por 1,9 s → `Loading`, em 2,0 s → `InWorld`; troca de `pawnId` → volta a `Loading`, `TakeChange().pawnChanged`; troca de `worldId` → `worldChanged`; `TakeChange` zera.
  - `test_local_sampler.cpp`: NaN/inf em cada campo → `nullopt`; 2 000 000 001 → `nullopt`; velocidade 40000 → 32767; cada linha do mapeamento de estado, com as fronteiras 9,9/10/380/380,1/1000/1000,1; primeiro `Sample` depois de `MarkJump` → `Apparate`, o seguinte não; salto de 3001 cm → `Apparate`, 3000 não; amostra pulada não muda a referência do salto.
  - `test_send_scheduler.cpp`: 20 Hz → verdadeiro em 0, falso em 0,049, verdadeiro em 0,05; tick atrasado em 1 s gera **um** verdadeiro e o próximo só 0,05 s depois; `SetRate(60)`; `Reset`.
  - `test_interp.cpp` (20 Hz, entidade andando a 300 cm/s em X, chegadas com jitter de ±15 ms de RNG fixo): pose em `now` fica entre 100 e 130 ms atrás da verdade (erro ≤ 5 cm contra a posição verdadeira em `now − 0,1` descontado o jitter mínimo); sem amostras novas, extrapola até 250 ms e para (posição em +250 ms igual à de +400 ms); amostra `Apparate` → sem posições intermediárias (`snapped`); salto de 600 cm sem `Apparate` → idem; yaw 350° → 10° passa por 0° e não por 180°; entidade com amostras a cada 200 ms → `DelayOf` = 0,400; a cada 50 ms → 0,100; relógio saltando 3 s → filas limpas; `PushNow` antes de qualquer tick funciona; 40 amostras → 32 na fila.
  - `test_puppet_director.cpp`: cria para entidade conhecida e perto; não cria para o próprio `playerId`; não cria a 40 001 cm; teto 2 com 3 entidades → as 2 mais perto, e troca quando a ordem muda; `spawnFailures = 3` → 4 chamadas em ~3 s e depois `desistiu`; `Gone` → recria depois de 1 s; 6 `Gone` em 1 min → desiste; 3 `GameError` seguidos em `MovePuppet` → destrói e recria; 20 `GameError` em 10 s → `disabled` e nenhum boneco; singleton trocado → `disabled`, `disabledWhy == "singleton"`, boneco destruído; `ReleaseAll(true)` chama `ForgetAllPuppets` uma vez e nenhum `DestroyPuppet`; `ReleaseAll(false)` destrói cada um; `OnEntityLeave` destrói; `maxPuppets = 0` → nenhum `SpawnPuppet`; pose com NaN nunca chega ao `FakeGame` (ele faz `REQUIRE(std::isfinite(...))` em todo método).
- [ ] **2. Implementar** os cinco módulos. Rodar `ctest -R "^client: "` → verde.
- [ ] **3. Verificação.** Build e `ctest` completos, verdes.
- [ ] **4. Commit** — `feat(client): WorldTracker, amostragem local, interpolação e direção dos bonecos` — e push.

---

### Task 5: `Session` — a máquina de estados

**Files:**
- Create: `client/core/include/cbmp/client/Session.hpp`, `client/core/src/Session.cpp`
- Create: `client/tests/{test_session.cpp,test_session_errors.cpp,SessionFixture.hpp}`
- Modify: `client/core/CMakeLists.txt`, `client/tests/CMakeLists.txt`

**Interfaces — consome:** tudo das Tasks 1–4 (assinaturas nos blocos "produz" delas).

**Interfaces — produz:**

```cpp
// Session.hpp
enum class SessionState { Armed, Superseded, Connecting, Handshake, Online, Disconnected };
enum class OnlinePhase { AwaitWorld, Playing };
struct SessionConfig { std::string launchId; std::array<uint8_t, 32> buildHash{}; bool statusOnScreen = true; int maxPuppets = 32; };
struct SessionDeps {
    IGame& game; ITransport& transport; IProcessGate& gate; IConnectFs& connectFs;
    LogFn log;
    std::function<int64_t()> unixNow;
    std::function<void(std::string_view)> setSecret;   // FileLogger::SetSecret (pode ser vazio)
    std::function<void()> clearSecret; };
class Session final : private INetSink { public:
    Session(SessionDeps deps, SessionConfig cfg);
    void Tick(double nowSec);                          // única entrada; nunca lança
    void Shutdown();                                   // jogo fechando: log `encerrando`, Close sem esperar
    SessionState State() const;
    OnlinePhase Phase() const;
    const std::string& DisconnectReason() const;       // "" até desconectar
    uint16_t PlayerId() const;
    bool HoldsTicket() const;                          // testes: há ticket em memória?
    const NetCounters& Net() const;
    const PuppetStats& Puppets() const; };
std::string DisconnectStatusText(std::string_view reason);   // "CB Multiplayer: desconectado — <frase>"
```

**Fluxo de `Tick(now)`** (tudo dentro de `try/catch(...)`):

1. `SampleWorld()` e `ReadLocal()` (cada um em `try`: `GameError` → conta erro de jogo e trata como "sem pawn"); `WorldTracker::Update`; mudança de mundo → log `mundo: <de> → <para>` (nome do mundo, com ` InWorld` no fim quando a fase passa a `InWorld`).
2. Por estado:
   - **Armed**: guarda o instante do primeiro tick; `DecideClaim(gate.IsCurrent(), fase == InWorld, now − primeiro, gate.Epoch())`. `Superseded` → log `substituido epoca=<Epoch>→<Latest>` e estado `Superseded` (para sempre; ticks seguintes não fazem nada). `Claim` → `gate.TryOwn()` falso → `Superseded` (log `substituido: outro processo é o dono`); verdadeiro → log `dono epoca=<n>` e resgate: `ClaimAndDelete()`: `Busy` → tenta de novo nos próximos ticks por até 5 s, depois `sem_arquivo`; `Missing` → `sem_arquivo`; `Rejected` → `arquivo_invalido`; `Ok` → `ParseConnect(bytes, cfg.launchId, unixNow())` (os bytes lidos são zerados depois): `Invalid` → `arquivo_invalido`, `ProtocolMismatch` → `protocolo_diferente`, `TicketExpired` → `ticket_expirado`; válido → `setSecret(ticket)`, `transport.Connect(host, port)` (falso → `sem_resposta`), estado `Connecting`.
   - **Connecting**: `State()` `Connected` → `NetClient::SendHello` no mesmo tick (log `Hello enviado`), `clearSecret()`, estado `Handshake`; `Closed` → motivo pelo fim; 10 s → `sem_resposta`.
   - **Handshake**: `Pump`; `OnWelcome` com `tickRateHz` fora de 5..60 → `servidor_incompativel`; válido → guarda `playerId`, `SendScheduler::SetRate(tickRateHz)`, `Interp::SetTickRate`, log `Welcome playerId=<n>`, estado `Online/AwaitWorld`; 10 s sem `Welcome` → `sem_welcome`; `Closed` → motivo pelo fim.
   - **Online**: `Pump` (falta → `servidor_invalido`); `Closed` → motivo pelo fim. Fase: `InWorld` → `Playing` (ao entrar: `LocalSampler::MarkJump()`, `SendScheduler::Reset()`, status `conectado`); saiu de `InWorld` → `AwaitWorld` e `PuppetDirector::ReleaseAll(worldChanged)`. Em `Playing`: `Due(now)` → `Sample(local)`; com valor → `SendInput(++seq, estado)` (primeiro `seq` = 1; `Apparate` → log `Input Apparate`); `PuppetDirector::Update(now, local.pos)`. Em `AwaitWorld`: nenhum `Input`. A cada 10 s, log `contadores snapshots=… acks=… correcoes=… bonecos=… erros_jogo=…`.
   - **Disconnected** / **Superseded**: nada de rede; em `Disconnected`, o status pendente é mostrado uma vez quando a fase for `InWorld`.
3. Exceção que escapar de 1–2: conta; primeira ocorrência gera uma linha, depois um resumo a cada 10 s; 100 em 60 s → `Disconnected(erro_interno)`.

**Mensagens recebidas (INetSink)**

- `OnSnapshot`: `SnapshotMirror::OnSnapshot`; com valor → `SendAck(tick)`, `Interp::OnTick(tick, now)`, e para cada id em `changed` que seja `Known`: `RemoteStore::Update` + `Interp::Push`. Vale em `AwaitWorld` e `Playing`.
- `OnEntityEnter`: `RemoteStore::Enter` + `Interp::PushNow` + log `EntityEnter id=<n>` (nome saneado). `OnEntityLeave`: `RemoteStore::Leave`, `SnapshotMirror::Forget`, `Interp::Remove`, `PuppetDirector::OnEntityLeave`, log `EntityLeave id=<n>`.
- `OnCorrection{seq, pos, vel}` (log de cada uma com a distância): em `AwaitWorld` → guarda como última correção e marca ressincronização (`MarkJump`); em `Playing`: distância ao pawn ≤ 100 cm → nada; ≤ 2000 cm → `TeleportLocal(pos)` se o último teleporte foi há ≥ 1 s (senão fica pendente e é tentada de novo nos ticks seguintes; só a mais recente é mantida); > 2000 cm → não move, `MarkJump()`. Segunda correção longe (> 2000 cm) com o mesmo `seq` da anterior → log `posicao_recusada` e status `CB Multiplayer: o servidor não aceitou sua posição`, ambos uma vez por sessão.
- `OnRefusal(texto, kick)`: guarda o texto; o estado só muda quando o transporte fecha.

**Motivo pelo fim (`LinkEnd.code`)**: 1001, 1002, 1003 → `recusado: <texto guardado, ou o debug saneado, ou "sem motivo">`; 1004 → `expulso: <idem>`; outro código: em `Connecting` → `sem_resposta`, depois → `conexao_perdida`.

**Desconectar** (`Disconnected(motivo)`, final): log `desconectado: <motivo>` (com os contadores quando for `servidor_invalido`), `ReleaseAll(false)`, `transport.Close()`, ticket zerado (`SecureZero`) e `clearSecret()`, status `DisconnectStatusText(motivo)` pendente.

**Status na tela**: só com `cfg.statusOnScreen`, só em `InWorld`, só em mudança de estado; `ShowStatus` devolvendo `false` ou lançando → recurso desligado na sessão (log uma vez). Textos em Global Constraints. `N por perto` = `Puppets().alive` no momento.

**Passos**

- [ ] **1. `SessionFixture.hpp`:** monta `FakeGame`, `FakeTransport`, `FakeGate`, `FakeConnectFs`, relógio manual (`now`, `unix`), captura de log em `std::vector<std::string>`, e ajudantes `ValidConnect(launchId)` (bytes de um `connect.json` válido com ticket cujo `exp` é `unix + 300`), `RunFor(seconds, step = 0.016)`, `GoOnline()` (leva até `Online/Playing` com `Welcome{playerId 7, 20 Hz}`), `LogHas(substr)`.
- [ ] **2. Testes de caminho feliz (falham)** — `test_session.cpp`:
  - época 1 no menu: nada acontece até 45 s; aos 45 s resgata, `Connecting`; `Connected` → 1 `Hello` confiável no mesmo tick, `HoldsTicket()` falso logo depois, `setSecret` chamado antes e `clearSecret` depois; época 2: resgata aos 5 s; `InWorld` antes do prazo: resgata na hora; `gate.latest = 2` com época 1 → `Superseded`, log `substituido epoca=1→2`, `claimCalls == 0` para sempre; `ownOk = false` → `Superseded`;
  - `AwaitWorld`: 0 `Input` enviados em 5 s, mas snapshots são confirmados (`SnapshotAck`) e `EntityEnter` registrado; ao entrar em `InWorld`: primeiro `Input` tem `seq == 1` e estado `Apparate`; em 1 s de `Playing` saem 20 ± 1 `Input` com `seq` crescente; tick atrasado em 1 s não gera rajada;
  - `tickRateHz = 30` → ~30 `Input`/s; outro jogador: `EntityEnter` + snapshots → 1 boneco criado (`FakeGame.puppets`), movido a cada tick; `EntityLeave` → destruído e o id some de `mirror.Find(...)`; snapshot que traz o próprio `playerId` não cria boneco;
  - sair de `InWorld` (pawn trocado) → `ReleaseAll(false)`, sem `Input`; voltar → `Input` com `Apparate` e boneco recriado; mundo trocado → `ForgetAllPuppets`;
  - `Correction`: a 50 cm → nada; a 1500 cm → 1 `TeleportLocal`; duas em 0,5 s → só 1 teleporte no primeiro segundo e o segundo depois; a 5000 cm → nenhum teleporte e o próximo `Input` vai com `Apparate`; segunda a 5000 cm com o mesmo `seq` → `posicao_recusada` e o status, uma vez só; em `AwaitWorld` → nenhum teleporte;
  - status: `conectado (0 por perto)` uma vez ao entrar em `Playing`; `statusOnScreen = false` → nenhum; `statusOk = false` → uma tentativa e nunca mais; contadores no log a cada 10 s; `Shutdown()` → `encerrando` e `closeCalls == 1`.
- [ ] **3. Testes da tabela de erros (falham)** — `test_session_errors.cpp`, um `TEST_CASE` por linha da tabela "Erros e falhas" da spec: arquivo ausente → `sem_arquivo`; `Busy` por 5 s → `sem_arquivo` (e `Busy` por 2 s seguido de `Ok` conecta); `launchId` diferente → `arquivo_invalido`; protocolo 2 → `protocolo_diferente` e `connectCalls == 0`; ticket vencendo em 5 s → `ticket_expirado`; `Connect` falso → `sem_resposta`; 10 s em `Connecting` → `sem_resposta`; `Reject{"banido"}` + fim 1001 → `recusado: banido`; sem `Welcome` em 10 s → `sem_welcome`; `Welcome{tickRateHz 4}` e `61` → `servidor_incompativel`; `Kick{"conectado em outro lugar"}` + fim 1004 → `expulso: conectado em outro lugar`; fim com outro código em `Online` → `conexao_perdida`; 21 mensagens inválidas → `servidor_invalido`; `Pending` 5000 → `servidor_invalido`; `FakeGame.throwOnSample` por 100 ticks em 60 s → segue vivo (erro de jogo contido), e exceção genérica injetada 100 vezes (um `ITransport` de teste cujo `State()` lança `std::runtime_error`) → `erro_interno`; em todos: `HoldsTicket()` falso no fim, `closeCalls ≥ 1`, bonecos zerados, e o texto de `DisconnectStatusText` igual ao de Global Constraints; desconexão no menu → status mostrado uma vez só quando entra em `InWorld`. Teste do log: em nenhum caso o texto do ticket aparece nas linhas capturadas (o fixture usa um ticket com marcador `TICKETSECRETO`).
- [ ] **4. Implementar `Session`.** Rodar `ctest -R "^client: "` → verde.
- [ ] **5. Verificação.** Build e `ctest` completos, verdes.
- [ ] **6. Commit** — `feat(client): Session com resgate, handshake, Input a 20 Hz, correções e tabela de erros` — e push.

---

### Task 6: `GnsTransport`, `cbmp-headless` e integração `client-e2e`

Núcleo real contra o `cb-server` real, sem o jogo.

**Files:**
- Create: `client/core/include/cbmp/client/GnsTransport.hpp`, `client/core/src/GnsTransport.cpp`
- Create: `client/headless/{CMakeLists.txt,main.cpp,ScriptedGame.hpp,ScriptedGame.cpp,FileConnectFs.hpp}`
- Create: `tools/client-e2e.py`
- Modify: `bots/src/main.cpp` (opção `--print-ticket`), `CMakeLists.txt` (raiz: `add_subdirectory(client/headless)`), `client/core/CMakeLists.txt`, `.github/workflows/native.yml` (passos nos jobs `linux` e `windows`)
- Ler: `bots/src/bot.cpp` (conexão, envio, fim), `tools/loadtest.py` (como sobe o servidor em modo dev)

**Interfaces — consome:** Tasks 1–5.

**Interfaces — produz:**

```cpp
// GnsTransport.hpp
struct GnsOptions { int fakeLossPercent = 0; };                 // só headless/testes
bool GnsInit(std::string* err = nullptr);                        // GameNetworkingSockets_Init; idempotente
void GnsShutdown();                                              // GameNetworkingSockets_Kill; idempotente
class GnsTransport final : public ITransport { public: explicit GnsTransport(GnsOptions opt = {}); ~GnsTransport() override; … };
```

- `cbmp-headless` (executável, `client/headless/`): `--connect <connect.json> --launch-id <32 hex> --start <x,y,z> [--walk-after <s>] [--walk-cm <cm>] [--speed <cm/s>] --duration <s> [--loss <0..90>] --report <arquivo.json>`. Sai com 0 quando a sessão chegou a `Online`, 2 se desconectou antes, 1 em erro de uso.
- Relatório JSON: `{"state","reason","playerId","online":bool,"startedAt","endedAt","inputs","snapshots","acks","local":[[t,x,y,z],…],"puppets":[{"id","createdAt","destroyedAt"|null,"path":[[t,x,y,z],…]}],"log":[…linhas…]}` com `t` em segundos Unix (double).
- `cb-bots --dev-seed <hex64> --server-id <id> --print-ticket [--index N]`: imprime um ticket dev (`makeTicket`, `iat` = agora, `jti` aleatório) e sai.
- `tools/client-e2e.py [--server-bin P] [--bots-bin P] [--headless-bin P] [--out-dir D] [--case walk|loss|server-down|all]`: sai com 0 só se todos os casos pedidos passam.

**Regras**

- `GnsTransport`: `Connect` → `SteamNetworkingIPAddr::ParseString("ip:porta")` (IPv6 entre colchetes) + `ConnectByIPAddress`; `Send` com `k_nSteamNetworkingSend_Reliable` ou `k_nSteamNetworkingSend_UnreliableNoDelay` (como `bots/src/bot.cpp`); `Receive` chama `RunCallbacks()`, puxa `ReceiveMessagesOnConnection` para uma fila própria (para de puxar quando a fila passa de 4097), entrega até `max`; `State()` por `GetConnectionInfo`: `Connecting`/`FindingRoute` → `Connecting`, `Connected` → `Connected`, `ClosedByPeer`/`ProblemDetectedLocally` → **drena a fila mais uma vez**, guarda `m_eEndReason` e `m_szEndDebug`, `CloseConnection` e fica `Closed` (as mensagens já puxadas continuam disponíveis para `Receive`); `Close()` → `CloseConnection(conn, 0, "fim", false)`. Sem callback global. `fakeLossPercent > 0` → `k_ESteamNetworkingConfig_FakePacketLoss_Send` e `_Recv` na conexão. `static_assert(k_ESteamNetConnectionEnd_App_Min == 1000)`.
- `ScriptedGame : IGame`: mundo `Overland` (`worldId 1`), pawn `pawnId 1` em `--start` desde o início; depois de `--walk-after` (padrão 6 s) anda `--walk-cm` (padrão 2000) em +X a `--speed` (padrão 300) e para; `ReadLocal` devolve posição, yaw 0, velocidade e `Ground`; bonecos em memória, cada `MovePuppet` grava `[t, x, y, z]`; `TeleportLocal` move o pawn; `PlayerSingletonIntact` verdadeiro; `ShowStatus` guarda a linha. `FileConnectFs : IConnectFs` (portável, só para o headless): `Exists` = arquivo existe; `ClaimAndDelete` = lê e apaga (≤ 16 KiB).
- `main.cpp`: portão local sempre atual com época 2 (resgata ao entrar em `InWorld`); laço de 16 ms (`sleep_until`), `Session::Tick(steady)` com `unixNow` do relógio do sistema; termina em `--duration` chamando `Shutdown()`; grava o relatório. O ticket nunca vai para a linha de comando: só pelo arquivo.
- `client-e2e.py` (só biblioteca padrão; reaproveita o jeito do `loadtest.py`: porta livre, `cb-bots --print-dev-key`, espera `/info`, encerra com `CTRL_BREAK_EVENT`/`SIGTERM`): configuração do servidor `{"name":"e2e","port":P,"hub":null,"auth":{"devPublicKey":K},"maxPlayers":8,"resourcesDir":…,"logsDir":…,"sim":{"tickHz":20,"streamRadiusM":300,"maxVisible":128,"spawn":{"mode":"origin"}},"movement":{…bloco de Global Constraints…}}`. Para cada cliente: ticket por `cb-bots --print-ticket --index i`, `connect.json` gravado em pasta temporária com `launchId` aleatório.
  - **walk**: A em `100000,50000,0` (`--walk-after 6 --duration 16`), B em `100500,50000,0` (`--duration 34`). Passa se: os dois `online`; B tem exatamente 1 boneco cujo caminho, a partir do primeiro ponto em que o boneco se moveu, fica a ≤ 60 cm do trecho do caminho local de A entre `t − 0,3` e `t − 0,1`; nenhum passo do boneco > 40 cm entre pontos consecutivos depois do primeiro; `destroyedAt − endedAt(A) ≤ 15`; nenhum dos dois logs do servidor contém `movimento inválido`; A enviou ≥ 150 `Input`.
  - **loss**: o mesmo com `--loss 20` nos dois clientes.
  - **server-down**: um cliente `--duration 40`; 8 s depois de `online` o servidor é morto (`kill`); passa se o relatório termina com `reason == "conexao_perdida"`.
  - Em falha: imprime o motivo e deixa relatórios e logs em `--out-dir` (padrão `build-native/client-e2e`).

**Passos**

- [ ] **1. `--print-ticket`** em `bots/src/main.cpp` (reusa `makeTicket`); conferir à mão: `cb-bots --dev-seed <64 hex> --server-id srv_dev --print-ticket` imprime `xxx.yyy`. Rodar `ctest -R "ticket"` → verde.
- [ ] **2. Escrever `tools/client-e2e.py`** com os três casos e rodar → falha em "cbmp-headless não encontrado" (esperado).
- [ ] **3. `GnsTransport`** e **`cbmp-headless`** (`target_link_libraries(cbmp-headless PRIVATE cbmp_client_core)`; `winmm` no Windows como o `cb-bots`). Compilar: `cmake --build --preset windows-release --target cb-server cb-bots cbmp-headless`.
- [ ] **4. Rodar** `python tools/client-e2e.py --case all` até passar 3 vezes seguidas. Se um critério falhar por erro do núcleo, corrija o núcleo com teste unitário novo que reproduza (não afrouxe o critério).
- [ ] **5. CI.** Em `native.yml`: job `linux`, depois do `ctest`: `python3 tools/client-e2e.py --case all --out-dir client-e2e-out`; job `windows`, depois do `ctest`: `python tools/client-e2e.py --case all --out-dir client-e2e-out`; nos dois, `actions/upload-artifact@v4` com `if: failure()` para `client-e2e-out`.
- [ ] **6. Verificação.** `ctest` completo verde; `python tools/client-e2e.py --case all` verde; `python tools/loadtest.py --count 64 --duration 30 --assert` verde (o `cb-bots` mudou).
- [ ] **7. Commit** — `feat(client): GnsTransport, cbmp-headless e integração client-e2e com o cb-server real` — e push.

---

### Task 7: Camada do jogo (UE4SS) e build do mod — **precisa do build local do UE4SS**

Casca fina: tudo o que for lógica já está no núcleo. Nada aqui pode ser testado sem o jogo além de compilar e empacotar; o comportamento vai para o roteiro manual (Task 12).

**Files:**
- Rewrite: `client/CMakeLists.txt`, `client/src/CMakeLists.txt`, `client/src/CbMpMod.hpp`, `client/src/CbMpMod.cpp`, `client/src/game/Ue4ssGame.hpp`, `client/src/game/Ue4ssGame.cpp`, `tools/build.ps1`
- Create: `client/vcpkg.json`, `client/src/game/Reflect.hpp`, `client/src/game/Reflect.cpp`, `client/src/Arming.hpp`, `client/src/Arming.cpp`
- Modify: `client/src/CrashDump.cpp`, `client/src/FileHash.{hpp,cpp}`, `client/src/SafeCall.hpp` (namespace `cbmp::client`), `client/src/Utf8Path.hpp` (se preciso)
- Referência local (não versionada, não copiar): `client/deps/RE-UE4SS/deps/first/Unreal/include/Unreal/` (`Hooks/Hooks.hpp`, `World.hpp`, `FWeakObjectPtr.hpp`, `UObjectGlobals.hpp`, `CoreUObject/UObject/Class.hpp`, `FProperty.hpp`, `FString.hpp`)

**Interfaces — consome:** `IGame` e tipos (Task 1), `ParseModConfig`, `FileLogger`, `LogFileName`, `PruneLogs`; `FindPointerFile`, `ParsePointer`, `ConnectPathFor`, `WinConnectFs`, `WinProcessGate` (Task 2); `Session`, `SessionDeps`, `SessionConfig` (Task 5); `GnsInit`, `GnsShutdown`, `GnsTransport` (Task 6).

**Interfaces — produz:**

- `tools/build.ps1 -Target mod` → `build\mod\…\Game__Shipping__Win64\` com `UE4SS.dll`, `dwmapi.dll` e `cb-mp.dll` (nome do alvo CMake continua `cb-mp`; o pacote o renomeia para `main.dll`).
- `client/vcpkg.json`: `{"name":"cbmp-mod","version-string":"0.2.0","builtin-baseline":"ec48c11eae9448c8c6e7739d93e06fa2433aa95f","dependencies":["gamenetworkingsockets","nlohmann-json"]}`.
- Definição de compilação `CBMP_MOD_VERSION` lida de `client/VERSION`.
- `Ue4ssGame(cbmp::client::ModConfig cfg)` implementando `cbmp::client::IGame`.

**Build**

- `client/CMakeLists.txt` (projeto próprio): `add_subdirectory(deps/RE-UE4SS)`; `find_package(GameNetworkingSockets CONFIG REQUIRED)` e a mesma escolha de alvo da raiz (`CBMP_GNS_TARGET`); `set(CMAKE_MAP_IMPORTED_CONFIG_GAME__SHIPPING__WIN64 Release)`; `add_library(cbmp_codec STATIC ../proto/src/bitstream.cpp ../proto/src/messages.cpp)` com `../proto/include` (o `proto/CMakeLists.txt` não é incluído: ele pede libsodium e Catch2); `add_subdirectory(core)`; `add_subdirectory(src/platform)`; `add_subdirectory(src)`. Sem testes neste projeto. `client/core/CMakeLists.txt` continua servindo aos dois builds (não cria testes; não redefine `cbmp_codec`).
- `client/src/CMakeLists.txt`: alvo `cb-mp` (SHARED) com `dllmain.cpp CbMpMod.cpp Arming.cpp FileHash.cpp CrashDump.cpp game/Ue4ssGame.cpp game/Reflect.cpp`; `PUBLIC UE4SS`; `PRIVATE cbmp_client_core cbmp_client_platform bcrypt dbghelp`; `/utf-8 /EHa`; `CBMP_MOD_VERSION`.
- `tools/build.ps1`: só `-Target mod`; configura com `-DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DVCPKG_INSTALLED_DIR=C:/cbmp-vi-mod` (parâmetro `-VcpkgInstalled` com esse padrão; o CI passa outro) e compila `--config Game__Shipping__Win64`. Falha com mensagem clara se `client\deps\RE-UE4SS\deps\first\Unreal\include` não existir ("rode tools/fetch-ue4ss.ps1").

**`Arming` (thread do UE4SS, só leitura)**

```cpp
struct Armed { std::filesystem::path container;   // Win64\cbmp
               cbmp::client::RunPointer pointer; std::filesystem::path connectJson; };
std::optional<Armed> TryArm(const std::filesystem::path& dllDir, int64_t nowUnix);
```

`FindPointerFile(dllDir)` → lê (≤ 4 KiB) → `ParsePointer` → confere que `connect.json` existe (`GetFileAttributesW`, sem abrir). Qualquer falha → `nullopt`: **Inerte** (não abre log, não registra tick, não inicia o GNS, não cria objeto nomeado, não grava nada, não chama `InstallCrashDump`).

**`CbMpMod`**

- Construtor: `ModName = cb-mp`, `ModVersion = CBMP_MOD_VERSION`; `TryArm`. Armado: cria `container\logs` e `container\crash`; `Log().Open(container\logs\<LogFileName(agora, pid)>)`; `PruneLogs(logs, 10)`; cria o `WinProcessGate` (entra na contagem de época); primeira linha `==== cb-mp <versão> pid=<pid> epoca=<n> ====`; registra exe, loja (`DetectStore`), sha256 do exe e o commit do UE4SS como hoje; `InstallCrashDump(container\crash)`; lê `container\cb-mp.json` (≤ 4 KiB) → `ParseModConfig`. Sem F8, sem `register_keydown_event`.
- `on_unreal_init`: inerte → retorna. Armado: `GnsInit` (falha → log e fica sem sessão); cria `Ue4ssGame`, `GnsTransport`, `WinConnectFs(connectJson)`, `Session` (`buildHash` = sha256 binário do próprio `main.dll`; `launchId` do ponteiro; `setSecret`/`clearSecret` do `Log()`); registra `RegisterEngineTickPreCallback` com `{false, true, STR("cb-mp"), STR("GameTick")}`; log do primeiro tick.
- Tick: `Session::Tick(NowSeconds())` dentro do `try/catch(...)` atual (mantém o resumo de exceções a cada 10 s).
- Destrutor: `Session::Shutdown()`, desregistra o tick, `GnsShutdown()`, log `cb-mp descarregado`.
- `CrashDump.cpp`: `MiniDumpNormal` no lugar de `MiniDumpWithIndirectlyReferencedMemory`.
- `FileHash`: acrescentar `bool Sha256Bytes(const std::filesystem::path&, std::array<uint8_t, 32>& out)`.

**`Reflect`** (única forma de chamar o jogo)

```cpp
using ArgValue = std::variant<bool, int32_t, float, double, RC::Unreal::FVector, RC::Unreal::FRotator, RC::Unreal::UObject*, RC::StringType>;
struct Arg { const RC::Unreal::TCHAR* name; ArgValue value; };
// false = a função não existe nesta classe. Lança GameError se um parâmetro pedido não existir ou tiver tipo inesperado.
bool CallByName(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* function, std::initializer_list<Arg> args,
                void* returnOut = nullptr, size_t returnSize = 0);
```

`UFunction` resolvida com `GetFunctionByNameInChain` e guardada em cache por (classe, nome); buffer zerado do tamanho dos parâmetros (maior `offset + size` entre as propriedades da função); cada argumento gravado na propriedade de mesmo nome (percorrendo `ForEachProperty`); retorno copiado da propriedade de retorno. **Nenhum offset fixo no código.**

**`Ue4ssGame`** (cada método público inteiro dentro de `SafeCall`; nomes só os da spec)

- Mundo: `GameEngine` → `GameViewport` → `World`. `worldId`/`pawnId` = índice + número de série do objeto (`FWeakObjectPtr`), nunca o ponteiro. `worldName` = nome do objeto do mundo. Pawn = `PlayerController.Pawn` (ou `AcknowledgedPawn`). `frontendHint` = `UIManager.IsInFrontendLevel` quando `UIManager` for encontrado; senão `false`.
- `ReadLocal`: posição e rotação do `RootComponent` (`RelativeLocation`, `RelativeRotation`); velocidade = propriedade `Velocity` do componente `CharacterMovement`; modo pela propriedade `MovementMode` (1, 2 → `Ground`; 3 → `Falling`; 4 → `Swimming`; 5 → `Flying`; outro → `Other`).
- `TeleportLocal`: `K2_TeleportTo(DestLocation, DestRotation = rotação atual)`; função ausente → `K2_SetActorLocation(NewLocation, bSweep=false, bTeleport=true)`.
- `SpawnPuppet`: `UWorld::SpawnActor(classe do pawn local, &loc, &rot)`; nulo → `nullopt`. Depois, cada passo num `SafeCall` próprio (falha registra uma vez por sessão e segue): `SetActorEnableCollision(false)`; `Controller` não nulo → `DetachFromControllerPendingDestroy`; `SetInvulnerable(true)`; componente da classe `CognitionStimuliSourceComponent`: `SetAutoRegisterAsSource(false)` e `UnregisterFromPerceptionSystem`; `EnablePawnInteraction(false)`. Com `extraNeutralization`: `Deactivate` + `SetComponentTickEnabled(false)` nos componentes das classes `CameraStackComponent`, `InputWatcherComponent` e `BP_PlayerPerceptionPoint_C`, e `StartSettingTargetableByAiState(false, …)`. Guarda `FWeakObjectPtr` do ator num mapa `PuppetId → fraca` (ids a partir de 1, nunca reutilizados).
- `MovePuppet`: referência fraca inválida → `Gone`. `K2_SetActorLocationAndRotation(NewLocation, NewRotation = (0, yaw, 0), bSweep=false, bTeleport=true)`; ausente → `K2_TeleportTo`; ausente → `K2_SetWorldLocationAndRotation` no `RootComponent`; nenhuma → `Failed`. A alternativa que funcionou fica fixada para a sessão. Animação conforme `puppetAnim`: `Velocity` → grava a velocidade na propriedade `Velocity` do `CharacterMovement` do boneco; `Ambulatory` → também `SetWorldSpeedAndDirection` e `SetIsInIdle`; `Off` → nada. Falha de animação desliga só a animação (log uma vez).
- `DestroyPuppet`: fraca válida → `K2_DestroyActor`; remove do mapa. `ForgetAllPuppets`: limpa o mapa sem chamar o jogo.
- `PlayerSingletonIntact`: `GetTheBipedPlayer` (função estática de `Biped_Player`, chamada no objeto padrão da classe) comparado com o pawn local; função não encontrada → `true` (log uma vez).
- `ShowStatus`: `AddTextTickerNotification` (estática de `UIManager`), texto no primeiro parâmetro de texto da função; `UIManager` ou a função ausentes → `false`.
- `Diagnose`: como hoje (controller, pawn), mais o nome do mundo.

**Passos**

- [ ] **1. Build.** `client/vcpkg.json`, `client/CMakeLists.txt`, `client/src/CMakeLists.txt`, `tools/build.ps1`. Rodar `tools\build.ps1 -Target mod` → falha de compilação em `CbMpMod.cpp`/`Ue4ssGame.cpp` (API antiga): confirma que o vcpkg instalou em `C:\cbmp-vi-mod` e o UE4SS configurou. **Conferir que `C:\cbmp-vi` não foi alterado** (`Get-ChildItem C:\cbmp-vi | Measure-Object` antes e depois).
- [ ] **2. `Arming` + `CbMpMod`** (inércia, log, portão, crash, config, sessão, tick) e `CrashDump`/`FileHash`.
- [ ] **3. `Reflect` + `Ue4ssGame`.**
- [ ] **4. Compilar** `tools\build.ps1 -Target mod` → sem erro e sem aviso novo em `client/src` (`/W4`); os três DLLs existem.
- [ ] **5. Conferências sem o jogo** (registrar a saída no relatório):
  - `dumpbin /dependents <cb-mp.dll>` não lista `GameNetworkingSockets.dll`, `libprotobuf*.dll`, `libcrypto*.dll` nem CRT de depuração (GNS estático, `/MD`);
  - `dumpbin /exports <cb-mp.dll>` lista `start_mod` e `uninstall_mod`;
  - `Select-String -Path client\src -Pattern 'F8|SpikeController|MiniDumpWithIndirectlyReferencedMemory' -Recurse` → nada;
  - `Select-String -Path client\core,client\headless -Pattern 'Unreal/|<Mod/|Windows\.h' -Recurse` → nada;
  - o build nativo da raiz e `ctest` continuam verdes (o núcleo não mudou de comportamento).
- [ ] **6. Lista para o roteiro manual** no relatório da task: cada suposição que só o jogo confirma (nomes de função/propriedade usados, qual alternativa de movimento, `frontendHint`, texto do ticker, animação).
- [ ] **7. Commit** — `feat(client): mod cb-mp 0.2.0 — camada do jogo no UE4SS, inércia sem o launcher e sessão no tick` — e push.

---

### Task 8: Launcher — pacote em `atual\`, ativação, conflito e remoção

**Files:**
- Modify: `launcher/src-tauri/src/modpkg.rs`, `launcher/src-tauri/src/game.rs`, `launcher/src-tauri/src/lib.rs` (`pub mod modact;`), `launcher/src-tauri/Cargo.toml` (feature `Win32_System_Diagnostics_ToolHelp` do `windows-sys`), `launcher/src-tauri/tests/modpkg.rs`
- Create: `launcher/src-tauri/src/modact.rs`, `launcher/src-tauri/tests/modact.rs`, `launcher/src-tauri/tests/common/modzip.rs` (zip v2 de teste assinado com par gerado no teste)

**Interfaces — consome:** o que já existe em `modpkg.rs` (`ModInstaller`, `Installed`, `InstalledFile`, `SignedMod`, `verify_mod_file`, `extract_zip`, `recover_mod_dir`, `mod_invalid`), `game.rs` (`GameInstall`, `BINARIES`), `error.rs` (`AppError`), `launch::acl`.

**Interfaces — produz:**

```rust
// modpkg.rs
pub const MOD_FOLDER: &str = "cbmp";            // contêiner
pub const CURRENT_FOLDER: &str = "atual";
pub const LAYOUT: u32 = 2;
pub const MOD_META_FILE: &str = "cbmp-mod.json";
pub const LOADER_IN_ZIP: &str = "loader/dwmapi.dll";
pub const MAX_LOADER_HISTORY: usize = 8;
pub fn win64_dir(game: &GameInstall) -> PathBuf;        // <jogo>\Phoenix\Binaries\Win64
pub fn container_dir(game: &GameInstall) -> PathBuf;    // Win64\cbmp
pub fn mod_dir(game: &GameInstall) -> PathBuf;          // Win64\cbmp\atual  (mudou)
#[derive(Deserialize)] #[serde(rename_all = "camelCase")]
pub struct ModMeta { pub layout: u32, pub version: String, pub protocol: u32, pub ue4ss: String, pub min_launcher: String }
pub fn check_mod_meta(bytes: &[u8], signed: &SignedMod, launcher: &semver::Version) -> AppResult<ModMeta>;
pub struct LoaderRef { pub sha256: String, pub size: u64 }
pub struct BackupEntry { pub name: String, pub stored: String }       // nome original em Win64 → nome em cbmp-backup
// Installed ganha (todos com #[serde(default)]): layout: u32, loader: Option<LoaderRef>, loader_history: Vec<String>,
//   backups: Vec<BackupEntry>, removed: bool
impl ModInstaller {
    pub fn with_launcher_version(self, v: semver::Version) -> Self;   // padrão: CARGO_PKG_VERSION
    pub fn save_record(&self, r: &Installed) -> AppResult<()>;        // gravação atômica do installed.json
    pub fn package_zip(&self) -> PathBuf;  pub fn package_sig(&self) -> PathBuf;   // mods\package\cbmp-mod.zip(.sig)
    pub fn pubkey(&self) -> &str;
}

// game.rs
pub trait Processes { fn names(&self) -> Vec<String>; }
pub struct SystemProcesses;                              // CreateToolhelp32Snapshot
pub fn game_running_with(p: &dyn Processes) -> bool;     // algum nome == "HogwartsLegacy.exe" (sem diferenciar caixa)
pub fn game_running() -> bool;
pub fn game_running_error() -> AppError;                 // code "game_running", mensagem exata

// modact.rs
pub const LOADER_NAME: &str = "dwmapi.dll";
pub const OVERRIDE_NAME: &str = "override.txt";
pub const OVERRIDE_LINE: &str = "cbmp\\atual\\ue4ss";
pub const BACKUP_FOLDER: &str = "cbmp-backup";
pub const CONFLICT_DIALOG: &str = "…texto exato de Global Constraints…";
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Activation { Active, Missing { loader: bool, override_file: bool }, Conflict { loader: bool, override_file: bool }, LegacyUe4ss }
pub fn inspect(win64: &Path, rec: &Installed) -> AppResult<Activation>;
pub fn activate(win64: &Path, current: &Path, rec: &mut Installed, confirm_conflict: bool) -> AppResult<()>;
pub fn ensure_active(game: &GameInstall, mods: &ModInstaller, confirm_conflict: bool) -> AppResult<()>;   // inspect + activate + save_record
pub fn remove_mod(game: &GameInstall, mods: &ModInstaller) -> AppResult<()>;
#[derive(Serialize)] #[serde(tag = "state", rename_all = "camelCase")]
pub enum ModStatus { NotInstalled, Installed { version: String }, Conflict }
pub fn status(game: Option<&GameInstall>, mods: &ModInstaller) -> ModStatus;
```

**Regras**

- **Instalação**: `ensure` extrai em `cbmp\atual.staging`, relê e confere os hashes, troca (`atual` → `atual.old`, `atual.staging` → `atual`, apaga `atual.old`); `recover_mod_dir(cbmp\atual)` na entrada. O contêiner `cbmp\` é criado se faltar e **nunca** renomeado; `cbmp-run.json`, `logs\`, `crash\` e `cb-mp.json` sobrevivem a qualquer instalação. Falha de permissão ao criar `Win64\cbmp` ou gravar dentro dele → `AppError::new("mod_needs_admin", <mensagem exata>)`.
- **`cbmp-mod.json`** lido do zip antes de extrair (e de novo no reparo): ausente, JSON inválido, `layout != 2`, `version` ou `protocol` diferentes dos assinados → `mod_invalid`; `min_launcher` inválido ou maior que a versão do launcher → `mod_incompatible`. O zip também tem de conter `loader/dwmapi.dll`, `ue4ss/UE4SS.dll` e `ue4ss/Mods/cb-mp/dlls/main.dll` → senão `mod_invalid`.
- **Registro**: `layout = 2`; `loader = {sha256, size}` do `atual\loader\dwmapi.dll`; ao trocar de loader, o hash anterior entra em `loader_history` (sem repetir, no máximo 8, o mais velho sai). A ordem de gravação do registro (depois da troca) não muda nesta task; a pendência R1-m1 continua aberta.
- **Migração do layout antigo** (`layout` ausente/1 no registro, mesma pasta de jogo): antes da primeira instalação em `atual\`, apaga de `cbmp\` só os arquivos listados em `files` do registro antigo (e as pastas que ficarem vazias); nada mais em `cbmp\` é tocado.
- **`removed = true`** (depois de `remove_mod`): `ensure` reinstala do pacote local (`mods\package\`, assinatura conferida de novo), sem baixar, e limpa a marca; a versão do registro continua valendo como piso do anti-downgrade.
- **`inspect`**: `dwmapi.dll` ausente → falta; presente com sha256 igual a `rec.loader` ou a algum de `rec.loader_history` → nosso; senão → estranho. `override.txt` ausente → falta; conteúdo, sem espaços e quebras nas pontas, igual a `cbmp\atual\ue4ss` → nosso; senão → estranho. Algum estranho → `Conflict{…}`. `Win64\xinput1_3.dll` **e** `Win64\UE4SS.dll` presentes → `LegacyUe4ss` (tem precedência). Nada faltando nem estranho → `Active`.
- **`activate`**: `LegacyUe4ss` → `mod_conflict` com detalhe pedindo para o jogador remover o UE4SS antigo (`xinput1_3.dll` e `UE4SS.dll`); o launcher não mexe. `Conflict` sem confirmação → `mod_conflict`. Com confirmação: cria `Win64\cbmp-backup\` e **move** (rename no mesmo volume) cada arquivo estranho para lá; nome já existente ganha sufixo `.1`, `.2`…; registra em `rec.backups`. Depois copia `atual\loader\dwmapi.dll` → `Win64\dwmapi.dll` (arquivo temporário + rename; também quando o nosso é de hash antigo) e grava `override.txt` com a linha + `\r\n`. Permissão negada em `Win64` → `mod_needs_admin`; outra falha → `mod_activation_failed`. Nunca sobrescreve nem apaga arquivo estranho.
- **`remove_mod`**: apaga `Win64\dwmapi.dll` e `Win64\override.txt` **só se forem os nossos**; devolve cada item de `rec.backups` para `Win64\` só se o destino estiver vazio (senão fica em `cbmp-backup\` e continua no registro); apaga `cbmp\atual`, `cbmp\atual.staging`, `cbmp\atual.old` e `cbmp\cbmp-run.json`; mantém `cbmp\logs\`, `cbmp\crash\`, `cbmp\cb-mp.json`, `cbmp-backup\` (se sobrou algo) e `mods\package\`; registro fica com `removed = true`, `files = []`, hash do loader movido para `loader_history`. Não confere se o jogo está aberto (isso é do comando, Task 9).
- **`status`**: sem jogo ou sem registro ou `removed` → `NotInstalled`; `inspect` `Conflict`/`LegacyUe4ss` → `Conflict`; senão `Installed{version}`.

**Passos**

- [ ] **1. `tests/common/modzip.rs`:** `pub fn build(version: &str, protocol: u32, loader: &[u8]) -> Vec<u8>` (zip com o layout de Global Constraints; DLLs com bytes de teste) e `pub fn sign(...)` com par minisign gerado no teste (o mesmo jeito de `tests/modpkg.rs`). Ajustar os testes existentes de `tests/modpkg.rs` para o zip v2 e para `cbmp\atual` (os que testavam o zip só com LEIA-ME passam a esperar `mod_invalid`).
- [ ] **2. Testes novos (falham)** — `tests/modpkg.rs`: instala em `cbmp\atual` e `installed.json` tem `layout 2`, `loader` e os arquivos; `cbmp\cbmp-run.json` e `cbmp\logs\x.log` pré-existentes continuam lá depois de instalar e de atualizar; `cbmp-mod.json` ausente / `layout 1` / versão diferente da assinada / protocolo diferente → `mod_invalid` e nada instalado; `minLauncher` `0.3.0` com launcher `0.2.0` → `mod_incompatible`; `minLauncher` `0.2.0` ok; zip sem `loader/dwmapi.dll` → `mod_invalid`; migração: registro antigo com `LEIA-ME.txt` em `cbmp\` + um arquivo estranho `cbmp\meu.txt` → o `LEIA-ME.txt` sai, `meu.txt` fica, mod novo em `atual\`; queda entre as renomeações (`atual.old` presente, `atual` ausente) → recuperado; `removed = true` → reinstala do pacote local sem chamar o Hub (Hub falso conta pedidos de download: 0).
- [ ] **3. Testes novos (falham)** — `tests/modact.rs` (pasta de jogo falsa em `tempfile`): tudo ausente → `Missing{true,true}`; depois de `activate` → `Active`, `dwmapi.dll` igual ao do pacote e `override.txt` == `cbmp\atual\ue4ss\r\n`; loader com hash do histórico → tratado como nosso e atualizado; `dwmapi.dll` estranho → `Conflict{loader:true,…}`, `activate(…, false)` → `mod_conflict` e o arquivo intacto; `activate(…, true)` → arquivo movido para `cbmp-backup\dwmapi.dll` com o mesmo conteúdo e registrado; segundo conflito com nome repetido → `dwmapi.dll.1`; `override.txt` estranho idem; `xinput1_3.dll` + `UE4SS.dll` → `LegacyUe4ss`, `activate(…, true)` → `mod_conflict` e nada movido; `remove_mod`: apaga os nossos, devolve o backup, apaga `atual*` e o ponteiro, mantém `logs\` e `mods\package\`, registro com `removed` e a mesma `version`; `remove_mod` com `dwmapi.dll` trocado por um estranho depois da ativação → o estranho fica; destino do backup ocupado → fica em `cbmp-backup\`; `status` nos três estados; `game_running_with` com lista falsa (`hogwartslegacy.exe` → verdadeiro; `HogwartsLegacy.exe.bak` → falso). Teste de integração `fluxo_completo_em_pasta_falsa`: instalar → ativar → detectar conflito plantado → confirmar → remover → backup de volta (critério de pronto 4 da spec, sem o ponteiro, que entra na Task 9).
- [ ] **4. Implementar.** `cargo test --locked` → verde.
- [ ] **5. Verificação.** `cargo test --locked`, `cargo test --locked --features local-test`, `cargo test --locked --features online-test`, `cargo clippy --locked --all-targets` sem aviso novo.
- [ ] **6. Commit** — `feat(launcher): mod em cbmp\atual com cbmp-mod.json, ativação do carregador, conflito com backup e remoção` — e push.

---

### Task 9: Launcher 0.2.0 — ponteiro, Jogar, configurações e interface

**Files:**
- Modify: `launcher/src-tauri/src/{launch.rs,play.rs,settings.rs,commands.rs,lib.rs,content.rs}`, `launcher/src-tauri/build.rs`, `launcher/src-tauri/capabilities/default.json`, `launcher/src-tauri/Cargo.toml` + `Cargo.lock` (versão `0.2.0`), `launcher/src-tauri/tauri.conf.json` (versão), `launcher/ui/package.json` (versão) e `package-lock.json` da raiz
- Modify: `launcher/src-tauri/tests/{launch.rs,config_guard.rs,e2e.rs}`, testes internos de `settings.rs` e `play.rs`
- Modify: `launcher/ui/src/lib/{api.ts,errors.ts,errors.test.ts}`, `launcher/ui/src/routes/{Settings.svelte,Settings.test.ts}`, `launcher/ui/src/components/{PlayStatus.svelte,Play.test.ts}`

**Interfaces — consome:** Task 8 (`modact::{ensure_active, remove_mod, status, ModStatus, CONFLICT_DIALOG}`, `modpkg::{container_dir, win64_dir}`, `game::{Processes, SystemProcesses, game_running_with, game_running_error}`).

**Interfaces — produz:**

```rust
// launch.rs
pub const CONNECT_TTL: Duration = Duration::from_secs(240);
pub const POINTER_FILE: &str = "cbmp-run.json";
pub fn new_launch_id() -> String;                                   // 16 bytes do getrandom → 32 hex minúsculos
// ConnectInfo ganha `pub launch_id: String`; connect.json ganha "launchId" (ConnectPreview não muda)
pub fn pointer_json(run_dir: &Path, launch_id: &str, written_at: u64, launcher: &str) -> Vec<u8>;
pub fn write_pointer(container: &Path, run_dir: &Path, launch_id: &str, written_at: u64, launcher: &str) -> AppResult<PathBuf>;
pub fn remove_pointer_if_ours(container: &Path, launch_id: &str);   // apaga só se o launchId do arquivo for o nosso
pub async fn expire_launch(connect: PathBuf, written: Vec<u8>, container: Option<PathBuf>, launch_id: String, delay: Duration);

// play.rs
pub const TICKET_RENEW_AFTER_SECS: i64 = 30;
// PlayContext ganha: pub processes: Arc<dyn Processes + Send + Sync>, pub launcher_version: String, pub confirm_conflict: bool
// content.rs
pub async fn resolve_game_ip(host: &str, dev: bool) -> AppResult<std::net::IpAddr>;   // literal ou DNS; só endereço que ip_allowed aceita; IPv4 primeiro

// settings.rs
pub const SETTINGS_VERSION: u32 = 2;
// Settings ganha `pub settings_version: u32` (JSON "settingsVersion"); simulate_launch padrão = false

// comandos novos (commands.rs + build.rs + capability)
mod_status() -> ModStatus
remove_mod() -> ModStatus                       // recusa com game_running; usa a trava do play (busy)
resolve_mod_conflict() -> ModStatus             // diálogo nativo com CONFLICT_DIALOG; aceito → ensure_active(…, true)
open_mod_logs() -> ()                           // abre Win64\cbmp\logs no Explorer (ShellExecuteW só no Rust); pasta ausente → erro `mod_logs_missing`
```

```ts
// api.ts
export type ModStatus = { state: "notInstalled" } | { state: "installed"; version: string } | { state: "conflict" };
export const modStatus = (): Promise<ModStatus> => call("mod_status");
export const removeMod = (): Promise<ModStatus> => call("remove_mod");
export const resolveModConflict = (): Promise<ModStatus> => call("resolve_mod_conflict");
export const openModLogs = (): Promise<void> => call("open_mod_logs");
```

**Regras**

- **`play`** (não simular): no começo, `game_running_with(ctx.processes)` → erro `game_running`. Etapa `Mod`: `ensure` e depois `modact::ensure_active(game, &ctx.mods, false)` (erros `mod_conflict`, `mod_needs_admin`, `mod_activation_failed` param o Jogar). Etapa `Connect`: ticket renovado se passaram mais de 30 s desde a emissão; `host` trocado por `resolve_game_ip(&host, RELAXED_NETWORK)` (falha → `server_unavailable`); `launch_id = new_launch_id()`; grava `connect.json` (com `launchId`, IP literal) e **depois** `write_pointer(container_dir(game), run_dir, launch_id, agora Unix, versão)`; falha de permissão no ponteiro → `mod_needs_admin`. Agenda `expire_launch(…, 240 s)`: apaga o `connect.json` se ainda for o mesmo conteúdo **e** apaga o ponteiro se ainda for o nosso (com ou sem `connect.json`). No modo simular nada de mod, ponteiro ou conferência de jogo aberto; o `connect.json` continua sendo gravado como hoje (com `launchId`).
- **Ponteiro**: JSON exato de Global Constraints (`v`, `runDir`, `launchId`, `writtenAt`, `launcher`), gravado em `cbmp-run.json.tmp` + rename; sem ticket, sem `host`.
- **`launch::remove_stale`** (abertura do launcher) continua apagando só o `connect.json` velho.
- **Configurações**: arquivo sem `settingsVersion` ou com valor < 2 → `simulate_launch = false` nessa carga e o arquivo é regravado com `settingsVersion: 2` (uma vez; depois o valor escolhido pelo usuário vale). `SettingsUpdate` continua com `simulateLaunch`. Arquivo novo → `false`.
- **Comandos**: `remove_mod` e `resolve_mod_conflict` pegam a trava do play (`PlayGate::try_begin`, `busy` se ocupada) e rodam em `spawn_blocking`; `remove_mod` recusa com `game_running`. Cada comando novo entra em `build.rs`, em `capabilities/default.json` (`allow-…`) e no `generate_handler!`.
- **UI — Configurações**: seção "Mod do jogo" com o estado (`Não instalado` / `Instalado <versão>` / `Conflito com outro carregador de mods`), botão `Remover mod do jogo` (com confirmação em duas etapas, como "Limpar cache"; desabilitado em `notInstalled`) e botão `Abrir pasta de logs do mod`. A seção "Modo simular" passa a se chamar "Avançado" e mantém a caixa do modo simular (texto: "Modo simular: prepara tudo e não abre o jogo"). Valor padrão exibido quando não há configurações: desmarcado.
- **UI — Jogar**: `errors.ts` ganha os cinco códigos com as mensagens exatas, mais `mod_logs_missing: "A pasta de logs do mod ainda não existe."`. Em `mod_conflict`, `PlayStatus` mostra a mensagem, o texto do diálogo como explicação e o botão `Usar o carregador do CB Multiplayer`, que chama `resolveModConflict()` e, se voltar `installed`, repete o Jogar. Em `mod_needs_admin` mostra a mensagem e o caminho manual: "Feche o launcher e abra de novo como administrador uma vez, ou instale o jogo fora de Arquivos de Programas." (o botão de elevação entra na Task 10). Nada de `{@html}`.
- **Versão** `0.2.0` em `Cargo.toml`, `Cargo.lock`, `tauri.conf.json`, `launcher/ui/package.json`, `package-lock.json`.

**Passos**

- [ ] **1. Testes Rust (falham).**
  - `tests/launch.rs`: `connect.json` tem `launchId` de 32 hex e os campos antigos; `new_launch_id` dá valores diferentes e `^[0-9a-f]{32}$`; `pointer_json` tem exatamente as chaves `v, runDir, launchId, writtenAt, launcher`, `v == 1`, e não contém o ticket nem o host; `write_pointer` grava por rename (sem `.tmp` sobrando) e sobrescreve o anterior; `remove_pointer_if_ours` apaga o nosso e mantém o de outro `launchId`; `expire_launch` com relógio pausado do tokio: aos 239 s os dois arquivos existem, aos 240 s os dois somem; com `connect.json` já apagado (mod leu) o ponteiro some mesmo assim; com `connect.json` regravado por outro play o arquivo novo fica e o ponteiro novo fica.
  - `play.rs` (testes internos/`tests/e2e.rs` com Hub e servidor falsos existentes): ticket emitido há 31 s é renovado, há 29 s não; `host` nome (`localhost`) vira `127.0.0.1` no `connect.json`; host que resolve para endereço proibido com `dev = false` → `server_unavailable`; jogo "aberto" (lista falsa) → `game_running` antes de qualquer pedido ao Hub; sem simular, o ponteiro aparece em `<jogo falso>\Phoenix\Binaries\Win64\cbmp\cbmp-run.json` com o mesmo `launchId` do `connect.json`, e `dwmapi.dll`/`override.txt` estão ativos; conflito plantado → `mod_conflict` e nada gravado em `run\`.
  - `settings.rs`: padrão `simulate_launch == false`; arquivo antigo `{"simulateLaunch": true}` → carregado como `false` e regravado com `settingsVersion: 2`; arquivo `{"settingsVersion": 2, "simulateLaunch": true}` → `true`; ida e volta.
  - `tests/config_guard.rs`: `CONNECT_TTL == 240 s`; `TICKET_RENEW_AFTER_SECS == 30`; `modact::OVERRIDE_LINE == "cbmp\\atual\\ue4ss"`; `modpkg::LAYOUT == 2`; `SETTINGS_VERSION == 2`; versão `0.2.0` igual em `Cargo.toml` e `tauri.conf.json`; os comandos novos em `build.rs` batem com a capability; nenhum plugin novo.
- [ ] **2. Implementar o Rust.** `cargo test --locked` → verde.
- [ ] **3. Testes da UI (falham)** — Vitest: `errors.test.ts` (as cinco mensagens exatas); `Settings.test.ts` (estado nos três valores; remover pede confirmação e chama `remove_mod`; erro `game_running` aparece; botão de logs chama `open_mod_logs`; caixa do modo simular desmarcada por padrão dentro de "Avançado"); `Play.test.ts` (`mod_conflict` mostra o botão, o clique chama `resolve_mod_conflict` e repete o `play`; `mod_needs_admin` mostra o caminho manual; texto do servidor continua sem HTML).
- [ ] **4. Implementar a UI.** `npm test -w launcher/ui` e `npm run typecheck -w launcher/ui` → verdes.
- [ ] **5. Verificação.** `cargo test --locked` (três perfis), `cargo clippy --locked --all-targets`, `npm test -w launcher/ui`, `npm run typecheck -w launcher/ui`, `npm run build -w launcher/ui && npm run check:dist -w launcher/ui`.
- [ ] **6. Commit** — `feat(launcher): 0.2.0 — ponteiro do mod, launchId, prazos novos, jogo aberto, remover mod e modo simular desligado por padrão` — e push.

---

### Task 10: Launcher — instalação com permissão de administrador (uma vez)

**Files:**
- Create: `launcher/src-tauri/src/admin_setup.rs`, `launcher/src-tauri/tests/admin_setup.rs`
- Modify: `launcher/src-tauri/src/{main.rs,lib.rs,commands.rs,launch.rs (acl)}`, `launcher/src-tauri/build.rs`, `launcher/src-tauri/capabilities/default.json`, `launcher/src-tauri/tests/config_guard.rs`
- Modify: `launcher/ui/src/lib/api.ts`, `launcher/ui/src/components/{PlayStatus.svelte,Play.test.ts}`

**Interfaces — consome:** Task 8 (`modpkg::{verify_mod_file, win64_dir, container_dir, LOADER_IN_ZIP, check_mod_meta}`, `modact::{inspect, activate, OVERRIDE_LINE}`), Task 9 (comandos e `PlayStatus`), `game::validate_game_dir`, `launch::acl`.

**Interfaces — produz:**

```rust
// admin_setup.rs
pub const FLAG: &str = "--mod-admin-setup";
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AdminArgs { pub game_dir: PathBuf, pub user_sid: String, pub package_dir: PathBuf, pub confirm_conflict: bool }
pub fn wants_admin_setup(args: &[String]) -> bool;                  // args[1] == FLAG
pub fn parse_args(args: &[String]) -> Result<AdminArgs, String>;    // --game-dir <p> --user-sid <S-1-…> --package-dir <p> [--confirm-conflict]
pub fn to_command_line(a: &AdminArgs) -> String;                    // parâmetros para o ShellExecuteExW (com aspas corretas)
pub fn validate(a: &AdminArgs, system_root: &Path) -> AppResult<GameInstall>;
pub fn run(a: &AdminArgs, pubkey: &str) -> AppResult<()>;           // o trabalho elevado
pub fn exit_code(r: &AppResult<()>) -> i32;                         // 0 ok; 2 argumentos; 3 assinatura; 4 conflito; 5 outros
pub fn request(a: &AdminArgs) -> AppResult<()>;                     // runas + espera (até 120 s) + traduz o código de saída
// launch::acl
pub fn grant_modify(path: &Path, sid: &str) -> AppResult<()>;       // ACE de Modificar herdável para o SID, sem tirar as existentes
// comando
mod_admin_setup() -> ModStatus
```

```ts
export const modAdminSetup = (): Promise<ModStatus> => call("mod_admin_setup");
```

**Regras**

- `main.rs`: antes de iniciar o Tauri, se `wants_admin_setup(args)` → `parse_args` → `run` → `std::process::exit(exit_code(..))`. Sem janela, sem instância única, sem rede. O modo não aceita ticket, URL nem nome de arquivo solto.
- `validate`: `validate_game_dir(game_dir)`; nenhum componente do caminho do jogo (até `Win64`) nem de `package_dir` é ponto de reanálise (`FILE_ATTRIBUTE_REPARSE_POINT`); o jogo fica fora de `system_root` (`%SystemRoot%`, comparação sem diferenciar caixa); `user_sid` casa com `^S-1-5-21(-\d{1,10}){4}$`; `package_dir` absoluto e existente. Falha → `invalid_admin_args`.
- `run`: `validate`; confere de novo a assinatura de `<package_dir>\cbmp-mod.zip` com `<package_dir>\cbmp-mod.zip.sig` e a chave embutida (`verify_mod_file`) e o `cbmp-mod.json` (`check_mod_meta`); cria `Win64\cbmp` e `grant_modify(Win64\cbmp, user_sid)`; extrai **só** `loader/dwmapi.dll` do zip (tamanho ≤ 64 MiB) e, pelas regras de `modact` (ausente ou nosso → grava por arquivo temporário + rename; estranho → só com `confirm_conflict`, movendo para `cbmp-backup\`), grava `Win64\dwmapi.dll` e `Win64\override.txt`. Não instala o pacote, não grava ponteiro, não toca em mais nada. O registro `installed.json` não é escrito pelo processo elevado: os movimentos para `cbmp-backup\` ficam num arquivo `Win64\cbmp\backups.json` (lista `{name, stored}`), que o launcher normal incorpora ao registro na próxima `ensure_active` e apaga.
- `request`: `ShellExecuteExW` com verbo `runas`, o próprio executável (`std::env::current_exe`), `SEE_MASK_NOCLOSEPROCESS`, janela oculta; `ERROR_CANCELLED` (UAC recusado) → `mod_needs_admin` com o caminho manual; espera o processo (120 s) e lê o código: 0 → ok; 4 → `mod_conflict`; 3 → `mod_signature_invalid`; outro → `mod_activation_failed`.
- Comando `mod_admin_setup`: trava do play; recusa com `game_running`; monta `AdminArgs` (pasta do jogo das configurações, SID de `acl::current_user_sid()`, `mods\package`); `confirm_conflict` só depois do diálogo nativo `CONFLICT_DIALOG` quando `inspect` deu conflito; `request`; devolve `status`. Daí em diante `ensure`/`ensure_active`/ponteiro funcionam sem elevação dentro de `cbmp\`. Loader novo (hash diferente) cai de novo em `mod_needs_admin`, e o botão resolve de novo.
- UI: em `mod_needs_admin`, `PlayStatus` mostra o botão `Instalar com permissão de administrador (uma vez)` → `modAdminSetup()` → se `installed`, repete o Jogar; erro → mostra a mensagem com o caminho manual.
- **Pacote pendente**: hoje o `ensure` só guarda o zip em `mods\package\` depois de instalar (`modpkg.rs`, `download_and_install`). Quando a instalação falha com `mod_needs_admin`, o zip já conferido e o `.sig` passam a ser guardados em `mods\pending\cbmp-mod.zip(.sig)` (nunca em `mods\package\`, que continua sendo só o pacote da versão registrada). `mod_admin_setup` passa `package_dir = mods\pending` se ele existir, senão `mods\package`. Depois de um `ensure` bem-sucedido, `mods\pending\` é apagado. Sem nenhum dos dois pacotes → erro `mod_unavailable` ("Clique em Jogar uma vez para baixar o mod e tente de novo.").

**Passos**

- [ ] **1. Testes (falham)** — `tests/admin_setup.rs`: `parse_args` aceita a forma completa e recusa argumento a mais, repetido, sem valor, caminho relativo; `to_command_line` → `parse_args` ida e volta com espaços e acentos no caminho; `validate` recusa: pasta que não é do jogo, jogo dentro de um `system_root` falso, SID `S-1-5-18` e `S-1-5-21-1`, `package_dir` relativo, junção no caminho (criar com `std::os::windows::fs::symlink_dir`; se o teste não tiver privilégio para criar, `eprintln!` e pula só esse caso); `run` em pasta de jogo falsa (sem elevar): com zip assinado pelo par do teste → `Win64\cbmp` existe com ACE de Modificar para o SID atual (lida de volta com `acl`), `dwmapi.dll` igual ao do zip, `override.txt` certo, **nada** em `cbmp\atual`; assinatura de outra chave → `mod_signature_invalid` e nada gravado; `dwmapi.dll` estranho sem `confirm_conflict` → `mod_conflict` e intacto; com → movido e `backups.json` gravado; `exit_code` para cada caso; `ensure_active` incorpora `backups.json` ao registro e o apaga; instalação que falha por permissão deixa o pacote em `mods\pending\` (e não em `mods\package\`), e um `ensure` bem-sucedido depois apaga `mods\pending\`. `config_guard.rs`: comando novo na capability; `main.rs` trata `FLAG` antes de `cbmp_launcher_lib::run()`.
- [ ] **2. Implementar.** `cargo test --locked` → verde. **Não disparar o UAC nos testes** (`request` só é chamado pelo comando).
- [ ] **3. UI + teste** (`Play.test.ts`: botão aparece em `mod_needs_admin`, chama `mod_admin_setup`, repete o `play`).
- [ ] **4. Verificação.** `cargo test --locked` (três perfis), `cargo clippy --locked --all-targets`, `npm test -w launcher/ui`, `npm run typecheck -w launcher/ui`.
- [ ] **5. Commit** — `feat(launcher): instalação do carregador com permissão de administrador (uma vez) para pastas protegidas` — e push.

---

### Task 11: Pacote do mod, `mod.yml` e publicação no ambiente de teste — **precisa do build local do UE4SS**

**Files:**
- Create: `tools/package-mod.ps1`, `tools/check-mod-zip.mjs`, `tools/lib/zip.mjs`, `tools/test/check-mod-zip.test.mjs`, `tools/test/mod-workflow.test.mjs`, `.github/workflows/mod.yml`, `client/package/LEIA-ME.txt`, `client/package/THIRD-PARTY-NOTICES.txt`
- Delete: `tools/package-spike.ps1`, `package/` (INSTALAR.bat, DESINSTALAR.bat, LEIA-ME.txt, mods.txt)
- Modify: `launcher/src-tauri/examples/dev_sign_mod.rs` (`--zip`), `tools/teste-online.mjs`, `tools/teste-online/server.cfg.json`, `tools/dev-env.mjs` (`--mod-zip`), `tools/test/*.test.mjs` afetados

**Interfaces — consome:** Task 7 (`tools/build.ps1 -Target mod`, `client/VERSION`), Tasks 8–10 (launcher 0.2.0), layout do zip de Global Constraints.

**Interfaces — produz:**

- `tools/package-mod.ps1 [-BuildDir build\mod] [-Out dist\cbmp-mod.zip]` → zip no layout de Global Constraints.
- `node tools/check-mod-zip.mjs <zip> [--version X.Y.Z] [--protocol N]` → sai 0 e imprime `ok <sha256>`; senão lista os problemas e sai 1. Biblioteca: `export function checkModZip(buffer, {version, protocol}) → string[]` (problemas; vazio = ok).
- `tools/lib/zip.mjs`: `readZip(buffer) → Map<nome, Buffer>` (stored + deflate por `node:zlib`, com as mesmas recusas de caminho do launcher: `..`, absoluto, `\`, `:`) e `writeStoredZip(entries) → Buffer` (para os testes).
- `dev_sign_mod --zip <arquivo>`: assina o zip dado em vez de gerar o de teste (confere que o `cbmp-mod.json` de dentro tem a `version` e o `protocol` dos argumentos).
- `node tools/teste-online.mjs --build-releases --mod-zip <zip>`; `MOD_VERSION = '0.2.0'`; `LAUNCHER_VERSIONS` com `'0.2.0'`; `PUBLISHED['0.1.3']`.

**Regras**

- **`package-mod.ps1`**: acha exatamente um `UE4SS.dll`, um `dwmapi.dll` e um `cb-mp.dll` em `Game__Shipping__Win64` (como o script antigo); monta: `cbmp-mod.json` (versão de `client/VERSION`, conteúdo exato de Global Constraints, UTF-8 sem BOM), `loader/dwmapi.dll`, `ue4ss/UE4SS.dll`, `ue4ss/UE4SS-settings.ini` (de `client/deps/RE-UE4SS/assets/UE4SS-settings.ini`, trocando só `ConsoleEnabled`, `GuiConsoleEnabled`, `GuiConsoleVisible`, `EnableHotReloadSystem` e `EnableDumping` para `0`; chave ausente → erro; `[Hooks]` intocado), `ue4ss/Mods/mods.txt` (`cb-mp : 1` e `Keybinds : 0`, CRLF), `ue4ss/Mods/cb-mp/enabled.txt` (vazio), `ue4ss/Mods/cb-mp/dlls/main.dll` (o `cb-mp.dll`), `licenses/UE4SS-LICENSE.txt` (cópia de `client/deps/RE-UE4SS/LICENSE`), `licenses/THIRD-PARTY-NOTICES.txt` (de `client/package/`: GameNetworkingSockets, protobuf, abseil, OpenSSL, nlohmann-json, com os textos de licença de `C:\cbmp-vi-mod\x64-windows-static-md\share\<porta>\copyright`), `LEIA-ME.txt` (de `client/package/`, pt-BR: o que é, que é instalado pelo launcher, como remover pelo launcher, onde ficam os logs). Zip com caminhos em `/`. Nada mais no zip (sem `.pdb`, sem `Keybinds`).
- **`check-mod-zip.mjs`** confere: lista exata de arquivos (nem a mais, nem a menos); `cbmp-mod.json` com exatamente `layout 2`, `version`, `protocol`, `ue4ss "3.0.1+4fb9b615"`, `minLauncher "0.2.0"` (e iguais a `--version`/`--protocol` se dados); DLLs começam com `MZ` e têm > 4 KiB; ini com as cinco chaves em `0` e seção `[Hooks]` presente; `mods.txt` com as duas linhas; licenças não vazias e a de terceiros cita os cinco nomes; `LEIA-ME.txt` não vazio; nenhum caminho inseguro.
- **`mod.yml`**: conforme Global Constraints. `permissions: contents: read`. Passos: checkout (sem `ref:`); passo `token` que escreve `have=true|false` em `$GITHUB_OUTPUT` conforme `secrets.UE4SS_SUBMODULES_TOKEN` esteja vazio (o segredo entra só por `env:` desse passo e do de clone); se `false`: `echo "::notice title=mod.yml::UE4SS_SUBMODULES_TOKEN não configurado: o submódulo privado UEPseudo não pode ser baixado. Build do mod pulado; nenhum artefato cbmp-mod foi gerado."` e os demais passos têm `if: steps.token.outputs.have == 'true'`. Com token: vcpkg no commit `ec48c11eae9448c8c6e7739d93e06fa2433aa95f`; clone do `UE4SS-RE/RE-UE4SS` no commit `4fb9b615a24c724041ef23e2c4073f2610c4c6d0` em `client/deps/RE-UE4SS` com submódulos recursivos por HTTPS usando o token (`url.https://x-access-token:<token>@github.com/.insteadOf`, configuração só do passo, removida depois); `tools/build.ps1 -Target mod -VcpkgInstalled D:/vi-mod`; `tools/package-mod.ps1`; `node tools/check-mod-zip.mjs dist/cbmp-mod.zip --version <client/VERSION> --protocol 1`; `actions/upload-artifact@v4` nome `cbmp-mod`, caminho `dist/cbmp-mod.zip`, `if-no-files-found: error`; resumo (`$GITHUB_STEP_SUMMARY`) com `GITHUB_SHA` e o sha256 do zip. Ações de terceiros fixadas por commit como em `launcher.yml`.
- **`mod-workflow.test.mjs`** (texto, sem dependência nova): o arquivo existe no caminho exato; tem `push` com `branches: [main]` e os cinco caminhos; `workflow_dispatch` sem `inputs`; não contém `pull_request_target` nem `pull_request`; nenhuma linha `ref:` nos passos de checkout do repositório; `runs-on: windows-2025`; artefato `cbmp-mod`; contém o aviso do token; o token nunca aparece em `run:` fora de `env`.
- **`teste-online.mjs`**: `MOD_VERSION = '0.2.0'`; `buildMod` exige `--mod-zip` quando o release `mod-p1-v0.2.0` ainda não existe na pasta de trabalho (erro claro dizendo como gerar: `tools/build.ps1 -Target mod`, `tools/package-mod.ps1`), roda `checkModZip` antes de assinar e chama `dev_sign_mod … --zip <zip>`; `notes.txt` = `Mod 0.2.0 (protocolo 1): conectar e se ver.`; o release `mod-p1-v0.1.0` existente na pasta de trabalho/VPS não é tocado. `LAUNCHER_VERSIONS = ['0.1.0','0.1.1','0.1.2','0.1.3','0.2.0']`; `PUBLISHED['0.1.3']` = sha256 do instalador 0.1.3 **publicado** (baixar de `https://cbmultiplayer.cntoficial.com.br/v1/dl/launcher-v0.1.3/CB%20Multiplayer%20(Teste)_0.1.3_x64-setup.exe` e calcular; conferir que bate com o da pasta de trabalho). `--check`: o passo do mod exige `version === '0.2.0'` e roda `checkModZip` no zip baixado; cabeçalho de ajuda atualizado.
- **`server.cfg.json`**: acrescentar o bloco `movement` exato; `description` sem "(modo simular)": `Servidor de teste do CB Multiplayer: conecte pelo launcher e veja os outros jogadores.`
- **`dev-env.mjs`**: opção `--mod-zip <zip>`: com ela publica `mod-p1-v0.2.0` assinado com a chave dev no Hub local; sem ela continua o mod de teste 0.1.0 (o cabeçalho avisa que o launcher 0.2.0 só aceita esse mod no modo simular).
- **Publicação** (depois de tudo verde e commitado):
  1. `tools\build.ps1 -Target mod`; `tools\package-mod.ps1`; `node tools/check-mod-zip.mjs dist/cbmp-mod.zip --version 0.2.0 --protocol 1`.
  2. Releases antigas na pasta de trabalho (`%TEMP%\cbmp-teste-online\releases`): se faltar alguma `launcher-v0.1.x` ou `mod-p1-v0.1.0`, copiar da VPS só para leitura: `scp -P <porta-ssh> -r <usuario>@<vps>:/var/www/html_cbmultiplayer/releases/<tag> <pasta>\releases\`.
  3. `node tools/teste-online.mjs --build-releases --mod-zip dist/cbmp-mod.zip` (gera `mod-p1-v0.2.0` e `launcher-v0.2.0`, assinados com a chave dev local).
  4. `$env:CBMP_TESTE_SSH='<usuario>@<vps>'; $env:CBMP_TESTE_SSH_PORT='<porta-ssh>'; node tools/teste-online.mjs --deploy` (sem `--server-tar`: o servidor não muda de código; o `server.cfg` novo vai junto e o serviço reinicia).
  5. `node tools/teste-online.mjs --status` e `node tools/teste-online.mjs --check` → tudo ok (inclui `0.1.3 → 0.2.0`, mod `v0.2.0`, layout do zip).
  6. Confirmar pelo `--status` que o `cb-server` está ativo depois do deploy (o `server.cfg` novo foi aceito). Que o servidor aceita a posição com `Apparate` é conferido no roteiro manual (o servidor de teste só aceita ticket do Hub, então o `cbmp-headless` não entra nele).
  Se a VPS, a chave ou a rede não estiverem disponíveis: **não improvisar**; terminar com `DONE_WITH_CONCERNS` listando exatamente quais dos passos 2–5 faltam.

**Passos**

- [ ] **1. Testes (falham)** — `check-mod-zip.test.mjs`: zip bom (montado com `writeStoredZip` e DLLs de teste com `MZ`) → `[]`; um caso por defeito: arquivo faltando, arquivo a mais, `layout 1`, versão diferente de `--version`, ini com `GuiConsoleEnabled=1`, ini sem `[Hooks]`, `mods.txt` sem `Keybinds : 0`, licença vazia, terceiros sem "OpenSSL", DLL sem `MZ`, caminho `../x`. `mod-workflow.test.mjs` como acima. Rodar `npm run test:tools` → falham.
- [ ] **2. Implementar** `zip.mjs`, `check-mod-zip.mjs`, `mod.yml`. `npm run test:tools` → verde.
- [ ] **3. `package-mod.ps1`** + `client/package/*`; apagar `tools/package-spike.ps1` e `package/` (`git rm -r`; antes `git grep -n "package-spike\|package/INSTALAR\|package\\\\mods.txt"` → só docs antigas). Build local do mod, empacotar, `check-mod-zip` → `ok`.
- [ ] **4. `dev_sign_mod --zip`** (teste no próprio exemplo: zip com `cbmp-mod.json` de versão diferente do argumento → erro) e `cargo test --locked` em `launcher/src-tauri`.
- [ ] **5. `teste-online.mjs`, `server.cfg.json`, `dev-env.mjs`** + testes de `tools/test` afetados. `npm run test:tools` → verde.
- [ ] **6. Commit** — `feat(mod): pacote cbmp-mod 0.2.0, verificador do zip, mod.yml e ambiente de teste com mod e launcher 0.2.0` — e push.
- [ ] **7. Publicação** (itens 1–6 de "Publicação"). Registrar no relatório: sha256 do zip do mod, sha256 do instalador 0.2.0, saída resumida do `--check` (sem segredos).
- [ ] **8. Commit** do que a publicação mudar no repositório (nada além de `PUBLISHED`, se preciso) e push.

---

### Task 12: Documentação, roteiro manual e pendências

**Files:**
- Create: `docs/TESTE-NO-JOGO-5A.md`, `client/README.md`
- Modify: `docs/COMO-FUNCIONA.md`, `docs/AMBIENTE-TESTE-ONLINE.md`, `docs/TESTE-ONLINE.md`, `docs/PENDENCIAS.md`, `docs/RECONHECIMENTO-JOGO.md` (nota), `launcher/README.md`, `README.md` (raiz, se citar o spike ou `tools/build.ps1 -Target tests`)
- Ler: a spec inteira, os relatórios `.superpowers/sdd/sub5a/task-*-report.md` (seções "para o roteiro manual" e preocupações)

**Interfaces — consome:** tudo o que as Tasks 1–11 entregaram (nomes de arquivos, comandos, textos de tela e de log de Global Constraints).

**Conteúdo obrigatório**

- **`docs/TESTE-NO-JOGO-5A.md`** (pt-BR, para dois testadores sem contexto): pré-requisitos (jogo instalado, um save existente, launcher de teste); os 10 passos da seção "Manual no jogo" da spec, na mesma ordem e com o mesmo conteúdo, cada um com "o que anotar"; a tabela "o que os logs têm de provar" com a linha exata de log a procurar (de Global Constraints) e em qual arquivo (`Win64\cbmp\logs\cb-mp-<data>-<pid>.log`, dois por abertura); onde ficam os arquivos para enviar; critério: passos 3, 4, 6 e 8 aprovados = 5a validado, os demais viram pendência; **anexo "O que só o jogo responde"** com todas as conferências adiadas pelas tasks: qual processo sobrevive e os tempos entre os dois; se o `UIManager.AddTextTickerNotification` mostra texto livre; qual alternativa de `MovePuppet` funcionou; boneco cai/atravessa/empurra; inimigos reagem ao boneco; câmera/HUD/singleton; animação (`"velocidade"` / `"ambulatory"` / `"off"` no `cb-mp.json`) e `neutralizacaoExtra`; critério de `InWorld` (nomes de mundo vistos no log `mundo:`); quadros por segundo com N bonecos; Epic (permissão, elevação, abrir pelo executável); mais de 4 min até o menu; `K2_TeleportTo` numa correção curta; e como ligar cada chave do `cb-mp.json`.
- **`client/README.md`**: arquitetura (núcleo × casca), como compilar e testar o núcleo, o e2e, o build do mod (UE4SS local, `C:/cbmp-vi-mod`), e o **contrato do cliente por escrito** (fecha a pendência): snapshots em partes, descarte, histórico, ack, `EntityEnter/Leave`, `Correction`, carência, `Event` descartado no 5a (`cb:command`/`cb:reply` são do 5c).
- **`launcher/README.md`**: layout na pasta do jogo, ponteiro e `launchId`, prazos (240 s / 30 s), ativação/conflito/backup, remover mod, modo elevado, erros novos, `mod.yml` real e o segredo `UE4SS_SUBMODULES_TOKEN`, como publicar o mod (oficial e de teste).
- **`docs/AMBIENTE-TESTE-ONLINE.md` / `docs/TESTE-ONLINE.md`**: mod 0.2.0, launcher 0.2.0, `--mod-zip`, configuração de movimento do servidor de teste e o que ela permite (cliente pode se teleportar), como conferir.
- **`docs/COMO-FUNCIONA.md`**: o caminho novo do Jogar até os bonecos, em linguagem simples.
- **`docs/RECONHECIMENTO-JOGO.md`**: nota no topo — o reconhecimento foi feito em 2026-11-11; a partir do mod 0.2.0 o `cb-mp` é inerte sem o launcher e o teste do F8 não existe mais.
- **`docs/PENDENCIAS.md`**:
  - **saem** (resolvidos): "Contrato do cliente por escrito" (menos os eventos `cb:command`/`cb:reply`, que passam a uma linha com dono 5c), "Teste espelho fim a fim", "Quantização com NaN/inf", "Mod real e a entrega pelo launcher", "Contrato do `connect.json` para o mod", "Pasta de dados por build (M-8)", "Mod na pasta do jogo sem administrador" (vira o item novo da Epic), a seção "Mod do jogo (spike)" (fica só a regra dos dados privados);
  - **ficam, atualizados**: estados de movimento confiáveis e velocidades (dono 5e; no servidor de teste o cliente pode declarar `Apparate`), spawn pelo gamemode (5e), teto com kick, política de teleporte, comandos admin pelo chat (5c), margem do ticket M-1 (medir com os logs da rodada manual), abrir o executável direto (Epic);
  - **entram**: rodada manual do 5a pendente (`docs/TESTE-NO-JOGO-5A.md`); `UE4SS_SUBMODULES_TOKEN` a criar pelo Robson (sem ele o `mod.yml` pula o build e o release oficial do mod não sai); UE4SS sempre carregado no single-player → carregador próprio (5b); enxugar ganchos do UE4SS (5b); boneco = clone do jogador → classe leve, animação e aparência (5b); teleporte longo seguro e limites calibrados (5e); renovação contínua do ticket e reconexão; Epic não verificada (permissão, elevação, executável direto, `UE4SS.log` em pasta protegida); `buildHash` ainda não recusado pelo servidor (S6); mod de teste local 0.1.0 só no modo simular / `make-test-kit.mjs` sem mod real; kit de reconhecimento com passo do F8 obsoleto; textos de status e `TeleportLocal` definidos pelo plano, a confirmar no jogo; registro do mod gravado depois da troca (já listado) continua.

**Passos**

- [ ] **1. Escrever** `docs/TESTE-NO-JOGO-5A.md` e `client/README.md`.
- [ ] **2. Atualizar** os demais documentos.
- [ ] **3. Conferência cruzada** (registrar no relatório): cada texto de tela, linha de log, nome de arquivo e comando citado nos docs existe no código (`git grep -n -F "<texto>"` para cada um da lista de Global Constraints); cada item do "Critério de pronto" da spec tem onde ser conferido; nenhum trecho de dump, só nomes curtos (`git diff --stat` + leitura).
- [ ] **4. Verificação.** `npm run test:tools`; links relativos dos docs abrem arquivos que existem (`node -e` percorrendo os `](…)` dos arquivos alterados).
- [ ] **5. Commit** — `docs(sub5a): roteiro de teste no jogo, contrato do cliente, launcher 0.2.0 e pendências` — e push.

---

## Fim do subprojeto 5a

Critério (spec): `native.yml` verde com `cbmp_client_tests` e `client-e2e` (Linux e Windows) e `launcher.yml` verde; `mod.yml` gera o artefato `cbmp-mod` quando o segredo existe (sem ele: verde, com aviso; o zip local passa no `check-mod-zip.mjs`); `mod-p1-v0.2.0` e launcher `0.2.0` publicados no ambiente de teste com `teste-online.mjs --check` verde e o servidor com a configuração de movimento nova; o launcher instala, ativa, grava o ponteiro, detecta conflito, remove e devolve o backup numa pasta de jogo falsa; documentação atualizada; rodada manual em lote depois (passos 3, 4, 6 e 8 aprovados = 5a validado).
