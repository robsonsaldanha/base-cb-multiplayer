#include "CbMpMod.hpp"
#include "CrashDump.hpp"
#include "FileHash.hpp"
#include "Utf8Path.hpp"
#include "game/Ue4ssGame.hpp"

#include <Arming.hpp>
#include <WinConnectFs.hpp>
#include <WinProcessGate.hpp>
#include <cbmp/client/GnsTransport.hpp>
#include <cbmp/client/Limits.hpp>
#include <cbmp/client/Log.hpp>
#include <cbmp/client/Session.hpp>
#include <cbmp/client/Store.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UEngine.hpp>

#include <Windows.h>

#include <array>
#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#ifndef CBMP_MOD_VERSION
#error "CBMP_MOD_VERSION precisa vir do CMake (client/VERSION)"
#endif
#define CBMP_STR_EXPANDED(x) STR(x)

using namespace RC;

namespace cbmp::client {
namespace {
constexpr const char* kUe4ssCommit = "4fb9b615a24c724041ef23e2c4073f2610c4c6d0";
constexpr size_t kMaxModConfigBytes = 4096;

// Caminho completo de um módulo (nullptr = exe do processo). Vazio se a API falhar ou truncar.
std::filesystem::path ModulePath(HMODULE module) {
    std::wstring buf(32768, L'\0');  // caminhos longos
    const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return buf;
}

std::filesystem::path ThisDllPath() {
    HMODULE self{};
    // Se falhar, NÃO cair em GetModuleFileNameW(nullptr): isso devolveria o exe do jogo.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ThisDllPath), &self) ||
        !self) {
        return {};
    }
    return ModulePath(self);
}

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int64_t UnixNow() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

const char* AnimName(PuppetAnim a) {
    switch (a) {
        case PuppetAnim::Velocity: return "velocidade";
        case PuppetAnim::Ambulatory: return "ambulatory";
        case PuppetAnim::Off: return "off";
    }
    return "?";
}
}  // namespace

CbMpMod::CbMpMod() {
    ModName = STR("cb-mp");
    ModVersion = CBMP_STR_EXPANDED(CBMP_MOD_VERSION);
    ModDescription = STR("CB Multiplayer");
    ModAuthors = STR("Castelo Bruxo RP");

    // Nada pode escapar para o carregador do UE4SS (start_mod é extern "C"; /EHa: cobre também SEH). Falha = inerte.
    try {
        Arm();
    } catch (const std::exception& e) {
        AbandonArming(e.what());
    } catch (...) {
        AbandonArming("exceção SEH ou desconhecida");
    }
}

void CbMpMod::AbandonArming(const char* why) noexcept {
    // Fica inerte: on_unreal_init volta logo e o destrutor não registra nada. O crash handler, se já instalado, fica
    // (só grava .dmp na pasta crash).
    try {
        const bool logOpen = m_armed != nullptr;
        m_gate.reset();
        m_armed.reset();
        if (logOpen) Log().Write(Level::Error, std::string("falha ao armar o mod; inerte: ") + why);
    } catch (...) {
    }
}

void CbMpMod::Arm() {
    // Thread do UE4SS. Inerte (sem ponteiro recente do launcher ou sem connect.json): para aqui, sem tocar em nada.
    const auto dll = ThisDllPath();
    auto armed = TryArm(dll.empty() ? std::filesystem::path{} : dll.parent_path(), UnixNow());
    if (!armed) return;
    m_armed = std::make_unique<Armed>(std::move(*armed));

    const auto logs = m_armed->container / "logs";
    const auto crash = m_armed->container / "crash";
    std::error_code ec;
    std::filesystem::create_directories(logs, ec);
    std::filesystem::create_directories(crash, ec);

    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    const auto pid = static_cast<uint32_t>(GetCurrentProcessId());
    const auto logPath = logs / LogFileName(local, pid);
    if (!Log().Open(logPath)) {
        Output::send<LogLevel::Error>(STR("[cb-mp] nao consegui abrir o log em {}\n"), logPath.wstring());
    }
    PruneLogs(logs, limits::kLogKeep);

    m_gate = std::make_unique<WinProcessGate>();  // entra na contagem de época
    Log().Write(Level::Info, std::format("==== cb-mp {} pid={} epoca={} ====", CBMP_MOD_VERSION, pid, m_gate->Epoch()));
    if (!m_gate->Ok()) Log().Write(Level::Warn, "portão de processos incompleto (seção ou mutex não criados)");
    Log().Write(Level::Info, std::format("armado: launchId={} launcher={} runDir={}", m_armed->pointer.launchId,
                                         m_armed->pointer.launcher, m_armed->pointer.runDir));
    Log().Write(Level::Info, "main.dll: " + ToUtf8(dll));
    const auto exe = ModulePath(nullptr);
    if (exe.empty()) Log().Write(Level::Warn, "não consegui obter o caminho do executável do jogo");
    Log().Write(Level::Info, "exe: " + ToUtf8(exe));
    Log().Write(Level::Info, std::format("loja: {}", StoreName(DetectStore(exe.wstring()))));
    Log().Write(Level::Info, "sha256 exe: " + Sha256Hex(exe));
    Log().Write(Level::Info, std::string("UE4SS commit: ") + kUe4ssCommit);
    InstallCrashDump(crash);

    const auto cfgPath = m_armed->container / "cb-mp.json";
    if (const auto bytes = ReadSmallFile(cfgPath, kMaxModConfigBytes)) {
        m_config = ParseModConfig(*bytes);
        Log().Write(Level::Info, "cb-mp.json lido");
    } else {
        Log().Write(Level::Info, "cb-mp.json ausente (ou > 4 KiB): padrões");
    }
    Log().Write(Level::Info,
                std::format("config: statusNaTela={} neutralizacaoExtra={} animacaoBoneco={} maxBonecos={}",
                            m_config.statusOnScreen, m_config.extraNeutralization, AnimName(m_config.puppetAnim),
                            m_config.maxPuppets));
}

CbMpMod::~CbMpMod() {
    // Thread do UE4SS. Primeiro desliga o tick (e espera um que esteja rodando), depois encerra a sessão sem tocar no
    // jogo (Session::Shutdown só fecha a conexão e zera o ticket).
    if (m_tickHookId) Unreal::Hook::UnregisterCallback(*m_tickHookId);
    if (m_tickGate) {
        std::lock_guard lock(m_tickGate->mutex);
        m_tickGate->owner = nullptr;
    }
    if (m_session) {
        try {
            m_session->Shutdown();
        } catch (...) {
        }
    }
    m_session.reset();
    m_connectFs.reset();
    m_transport.reset();
    m_game.reset();
    if (m_gnsStarted) GnsShutdown();
    if (m_armed) Log().Write(Level::Info, "cb-mp descarregado");
}

auto CbMpMod::on_unreal_init() -> void {
    if (!m_armed) return;
    // Nada pode escapar para o UE4SS (/EHa: cobre também SEH). Falha = log e sem sessão.
    try {
        StartSession();
    } catch (const std::exception& e) {
        AbandonSession(e.what());
    } catch (...) {
        AbandonSession("exceção SEH ou desconhecida");
    }
}

void CbMpMod::AbandonSession(const char* why) noexcept {
    try {
        const bool running = m_tickHookId.has_value();
        if (!running) {
            // Sem tick registrado: solta a sessão (GNS e portão ficam para o destrutor). O dono é zerado sob a trava
            // caso o registro tenha acontecido antes da falha.
            if (m_tickGate) {
                std::lock_guard lock(m_tickGate->mutex);
                m_tickGate->owner = nullptr;
            }
            m_session.reset();
            m_connectFs.reset();
            m_transport.reset();
            m_game.reset();
        }
        // Com o tick já registrado a sessão está de pé (só uma linha de log depois dele falhou): fica.
        Log().Write(Level::Error, std::string("falha ao iniciar a sessão: ") + why + (running ? "" : "; sem sessão"));
    } catch (...) {
    }
}

void CbMpMod::StartSession() {
    Log().Write(Level::Info, "Unreal inicializado");
    std::string err;
    if (!GnsInit(&err)) {
        Log().Write(Level::Error, "GameNetworkingSockets não iniciou: " + err + "; sem sessão");
        return;
    }
    m_gnsStarted = true;

    std::array<uint8_t, 32> buildHash{};
    if (!Sha256Bytes(ThisDllPath(), buildHash)) Log().Write(Level::Warn, "sha256 do main.dll falhou; buildHash zerado");

    m_game = std::make_unique<Ue4ssGame>(m_config);
    m_transport = std::make_unique<GnsTransport>();
    m_connectFs = std::make_unique<WinConnectFs>(m_armed->connectJson);
    SessionDeps deps{*m_game,
                     *m_transport,
                     *m_gate,
                     *m_connectFs,
                     [](Level level, std::string_view msg) { Log().Write(level, msg); },
                     [] { return UnixNow(); },
                     [](std::string_view secret) { Log().SetSecret(secret); },
                     [] { Log().ClearSecret(); }};
    SessionConfig cfg;
    cfg.launchId = m_armed->pointer.launchId;
    cfg.buildHash = buildHash;
    cfg.statusOnScreen = m_config.statusOnScreen;
    cfg.maxPuppets = m_config.maxPuppets;
    m_session = std::make_unique<Session>(std::move(deps), std::move(cfg));

    m_tickGate = std::make_shared<TickGate>();
    {
        std::lock_guard lock(m_tickGate->mutex);  // publica tudo acima para o game thread
        m_tickGate->owner = this;
    }
    // bOnce = false: o callback roda em TODO tick (true o removeria após o primeiro).
    const auto id = Unreal::Hook::RegisterEngineTickPreCallback(
        [gate = m_tickGate](auto&, Unreal::UEngine*, float, bool) {
            std::lock_guard lock(gate->mutex);
            if (gate->owner) gate->owner->OnGameTick();
        },
        {false, true, STR("cb-mp"), STR("GameTick")});
    if (id != Unreal::Hook::ERROR_ID) {
        m_tickHookId = id;
        Log().Write(Level::Info, "tick registrado");
    } else {
        Log().Write(Level::Error, "falha ao registrar o tick (ERROR_ID) - veja UE4SS.log");
    }
}

void CbMpMod::OnGameTick() {
    // Nada pode escapar para o dispatcher de ganchos do UE4SS (/EHa: cobre também SEH).
    try {
        if (!m_firstTickLogged) {
            m_firstTickLogged = true;
            Log().Write(Level::Info, "primeiro tick");
        }
        if (m_session) m_session->Tick(NowSeconds());
    } catch (...) {
        try {
            OnTickException();
        } catch (...) {
        }
    }
}

void CbMpMod::OnTickException() {
    // 1ª ocorrência: linha própria. Depois: no máximo um resumo a cada 10 s.
    const double now = NowSeconds();
    if (!m_tickExceptionSeen) {
        m_tickExceptionSeen = true;
        m_lastTickExceptionLog = now;
        Log().Write(Level::Error, "exceção inesperada no tick (repetições serão resumidas a cada 10 s)");
        return;
    }
    ++m_tickExceptionsPending;
    if (now - m_lastTickExceptionLog >= limits::kExceptionSummarySec) {
        Log().Write(Level::Error,
                    std::format("exceção inesperada no tick: mais {} desde a última linha", m_tickExceptionsPending));
        m_tickExceptionsPending = 0;
        m_lastTickExceptionLog = now;
    }
}
}  // namespace cbmp::client
