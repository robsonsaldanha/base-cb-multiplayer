#include "CbMpMod.hpp"
#include "CrashDump.hpp"
#include "FileHash.hpp"
#include "Utf8Path.hpp"
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
#include <cstdlib>
#include <format>
#include <string>
#include <system_error>

using namespace RC;

// Caminho completo de um módulo (nullptr = exe do processo). Vazio se a API falhar ou truncar.
static std::filesystem::path ModulePath(HMODULE module) {
    std::wstring buf(32768, L'\0'); // long paths
    const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return buf;
}

static std::filesystem::path ThisDllPath() {
    HMODULE self{};
    // Se falhar, NÃO cair em GetModuleFileNameW(nullptr): isso devolveria o exe do jogo.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ThisDllPath), &self) ||
        !self) {
        return {};
    }
    return ModulePath(self);
}

static std::filesystem::path GameExePath() {
    return ModulePath(nullptr);
}

static double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// %LOCALAPPDATA%\CasteloBruxo\cb-mp (usado se a pasta do mod não for gravável). Vazio se não houver LOCALAPPDATA.
static std::filesystem::path FallbackLogDir() {
    wchar_t* buf = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&buf, &len, L"LOCALAPPDATA") != 0 || !buf) return {};
    std::filesystem::path dir = buf;
    free(buf);
    if (dir.empty()) return {};
    return dir / L"CasteloBruxo" / L"cb-mp";
}

std::filesystem::path CbMpMod::ModDir() {
    // .../cb-mp/dlls/main.dll -> .../cb-mp
    const auto dll = ThisDllPath();
    if (dll.empty()) {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        Output::send<LogLevel::Warning>(
            STR("[cb-mp] nao consegui obter o caminho da cb-mp.dll; usando a pasta atual do processo: {}\n"),
            cwd.wstring());
        return cwd;
    }
    return dll.parent_path().parent_path();
}

CbMpMod::CbMpMod() {
    ModName = STR("cb-mp");
    ModVersion = STR("0.1.0-spike");
    ModDescription = STR("Castelo Bruxo multiplayer - spike de injecao");
    ModAuthors = STR("Castelo Bruxo RP");

    const auto modDir = ModDir();
    std::filesystem::path logDir = modDir;
    std::filesystem::path logPath = modDir / "cb-mp.log";
    bool logOpen = cbmp::Log().Open(logPath);
    bool usingFallback = false;
    if (!logOpen) {
        Output::send<LogLevel::Error>(STR("[cb-mp] nao consegui abrir cb-mp.log em {}; tentando %LOCALAPPDATA%\\CasteloBruxo\\cb-mp\n"),
                                      logPath.wstring());
        const auto fallbackDir = FallbackLogDir();
        if (!fallbackDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(fallbackDir, ec);
            const auto fallbackPath = fallbackDir / "cb-mp.log";
            if (cbmp::Log().Open(fallbackPath)) {
                logOpen = true;
                usingFallback = true;
                logDir = fallbackDir;
                logPath = fallbackPath;
            }
        }
    }
    if (logOpen) {
        Output::send<LogLevel::Normal>(STR("[cb-mp] log em: {}\n"), logPath.wstring());
    } else {
        Output::send<LogLevel::Error>(STR("[cb-mp] nao consegui abrir nenhum cb-mp.log (pasta do mod nem LOCALAPPDATA); sem log do cb-mp\n"));
    }
    cbmp::Log().Write(cbmp::Level::Info, "==== cb-mp 0.1.0-spike iniciando ====");
    cbmp::Log().Write(cbmp::Level::Info, "log em: " + ToUtf8(logPath));
    if (usingFallback) {
        cbmp::Log().Write(cbmp::Level::Warn, "não consegui gravar na pasta do mod (" + ToUtf8(modDir) + "); usando LOCALAPPDATA");
    }
    InstallCrashDump(logDir);
    const auto exe = GameExePath();
    if (exe.empty()) cbmp::Log().Write(cbmp::Level::Warn, "não consegui obter o caminho do executável do jogo");
    cbmp::Log().Write(cbmp::Level::Info, "exe: " + ToUtf8(exe));
    cbmp::Log().Write(cbmp::Level::Info, std::format("loja: {}", cbmp::StoreName(cbmp::DetectStore(exe.wstring()))));
    cbmp::Log().Write(cbmp::Level::Info, "sha256 exe: " + Sha256Hex(exe));
    cbmp::Log().Write(cbmp::Level::Info, "UE4SS commit: 4fb9b615a24c724041ef23e2c4073f2610c4c6d0");

    // Roda na thread do UE4SS (o logger é thread-safe); o toggle em si é consumido no tick.
    register_keydown_event(Input::Key::F8, [this]() {
        try {
            cbmp::Log().Write(cbmp::Level::Info, "F8 recebido");
        } catch (...) {
        }
        m_pendingToggles.fetch_add(1);
    });
    Output::send<LogLevel::Verbose>(STR("[cb-mp] carregado\n"));
}

CbMpMod::~CbMpMod() {
    // Unload ocorre na thread do UE4SS, então um tick pode estar em andamento (risco aceito no spike).
    if (m_tickHookId) Unreal::Hook::UnregisterCallback(*m_tickHookId);
    cbmp::Log().Write(cbmp::Level::Info, "cb-mp descarregado");
}

auto CbMpMod::on_unreal_init() -> void {
    cbmp::Log().Write(cbmp::Level::Info, "Unreal inicializado; instalando hook de tick");
    m_game = std::make_unique<Ue4ssGame>();
    m_controller = std::make_unique<cbmp::SpikeController>(
        *m_game, [](cbmp::Level l, const std::string& m) { cbmp::Log().Write(l, m); });
    // bOnce = false: o callback precisa rodar em TODO tick (true o removeria após o primeiro).
    const auto id = Unreal::Hook::RegisterEngineTickPreCallback(
        [this](auto&, Unreal::UEngine*, float, bool) { OnGameTick(); },
        {false, true, STR("cb-mp"), STR("GameTick")});
    if (id != Unreal::Hook::ERROR_ID) {
        m_tickHookId = id;
    } else {
        cbmp::Log().Write(cbmp::Level::Error, "falha ao registrar hook de tick (ERROR_ID) - veja UE4SS.log");
    }
}

void CbMpMod::OnGameTick() {
    // Nada pode escapar para o dispatcher de hooks do UE4SS (/EHa: cobre também SEH).
    try {
        if (!m_firstTickLogged) {
            m_firstTickLogged = true;
            cbmp::Log().Write(cbmp::Level::Info, "primeiro tick recebido");
        }
        if (!m_controller) return;
        for (int n = m_pendingToggles.exchange(0); n > 0; --n) m_controller->OnToggleClone();
        m_controller->Tick(NowSeconds());
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
        cbmp::Log().Write(cbmp::Level::Error, "exceção inesperada no tick (repetições serão resumidas a cada 10 s)");
        return;
    }
    ++m_tickExceptionsPending;
    if (now - m_lastTickExceptionLog >= 10.0) {
        cbmp::Log().Write(cbmp::Level::Error,
                          std::format("exceção inesperada no tick: mais {} desde a última linha", m_tickExceptionsPending));
        m_tickExceptionsPending = 0;
        m_lastTickExceptionLog = now;
    }
}
