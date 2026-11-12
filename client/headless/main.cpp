// cbmp-headless: o núcleo real do mod (Session + GnsTransport) com um jogo roteirizado, sem o Hogwarts Legacy.
// Usado pelo tools/client-e2e.py contra o cb-server real. O ticket só chega pelo connect.json (nunca pela linha de
// comando), que é lido e apagado no resgate.
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

#include <nlohmann/json.hpp>

#include <cbmp/client/ConnectFile.hpp>
#include <cbmp/client/GnsTransport.hpp>
#include <cbmp/client/Log.hpp>
#include <cbmp/client/ProcessGate.hpp>
#include <cbmp/client/Session.hpp>
#include <cbmp/log_text.hpp>

#include "FileConnectFs.hpp"
#include "ScriptedGame.hpp"

namespace {
using namespace cbmp::client;
using cbmp::headless::FileConnectFs;
using cbmp::headless::ScriptedGame;
using cbmp::headless::ScriptOptions;

constexpr const char* kUsage =
    "uso: cbmp-headless --connect <connect.json> --launch-id <32 hex> --start <x,y,z> --duration <s>\n"
    "                   --report <arquivo.json> [--walk-after <s>] [--walk-cm <cm>] [--speed <cm/s>] [--loss <0..90>]\n"
    "  Sai com 0 se a sessão chegou a Online, 2 se não chegou, 1 em erro de uso.\n";
constexpr auto kTickPeriod = std::chrono::milliseconds(16);

// Portão local: sempre o processo atual, época 2 (sucessor: resgata assim que entra em InWorld).
struct LocalGate final : IProcessGate {
    uint64_t Epoch() const override { return 2; }
    uint64_t Latest() override { return 2; }
    bool IsCurrent() override { return true; }
    bool TryOwn() override { return true; }
};

// Linhas de log em memória (e na saída padrão), com o mesmo filtro de segredo do FileLogger.
class CaptureLog {
public:
    void Write(Level level, std::string_view msg) {
        std::lock_guard lock(m_mutex);
        std::string text = cbmp::escapeLogText(msg);
        if (!m_secret.empty() && (text.find(m_secret) != std::string::npos || msg.find(m_secret) != std::string_view::npos))
            text = "linha omitida (continha segredo)";
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        std::string line = FormatLine(level, text, local, ms);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        m_lines.push_back(std::move(line));
    }
    void SetSecret(std::string_view s) {
        std::lock_guard lock(m_mutex);
        if (s.size() >= 16) m_secret.assign(s);
    }
    void ClearSecret() {
        std::lock_guard lock(m_mutex);
        SecureZero(m_secret);
    }
    std::vector<std::string> Lines() const {
        std::lock_guard lock(m_mutex);
        return m_lines;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
    std::string m_secret;
};

struct Args {
    std::filesystem::path connect, report;
    std::string launchId;
    ScriptOptions script;
    double durationSec = 0;
    int loss = 0;
};

double ParseNumber(const std::string& name, const std::string& v) {
    size_t used = 0;
    const double d = std::stod(v, &used);
    if (used != v.size() || !(d == d)) throw std::runtime_error("valor inválido para " + name + ": " + v);
    return d;
}

Vec3 ParseVec(const std::string& v) {
    Vec3 out;
    double* dst[] = {&out.x, &out.y, &out.z};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t comma = v.find(',', pos);
        if ((i < 2) != (comma != std::string::npos)) throw std::runtime_error("--start deve ser x,y,z");
        *dst[i] = ParseNumber("--start", v.substr(pos, i < 2 ? comma - pos : std::string::npos));
        pos = comma + 1;
    }
    return out;
}

Args ParseArgs(int argc, char** argv) {
    Args a;
    bool haveStart = false, haveDuration = false;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("falta valor para " + k);
            return argv[++i];
        };
        if (k == "--connect") {
            a.connect = std::filesystem::path(val());
        } else if (k == "--launch-id") {
            a.launchId = val();
        } else if (k == "--start") {
            a.script.start = ParseVec(val());
            haveStart = true;
        } else if (k == "--walk-after") {
            a.script.walkAfterSec = ParseNumber(k, val());
        } else if (k == "--walk-cm") {
            a.script.walkCm = ParseNumber(k, val());
        } else if (k == "--speed") {
            a.script.speedCmS = ParseNumber(k, val());
        } else if (k == "--duration") {
            a.durationSec = ParseNumber(k, val());
            haveDuration = true;
        } else if (k == "--loss") {
            a.loss = static_cast<int>(ParseNumber(k, val()));
        } else if (k == "--report") {
            a.report = std::filesystem::path(val());
        } else {
            throw std::runtime_error("argumento desconhecido: " + k);
        }
    }
    if (a.connect.empty()) throw std::runtime_error("falta --connect");
    if (!IsLaunchId(a.launchId)) throw std::runtime_error("--launch-id deve ter 32 hex minúsculos");
    if (!haveStart) throw std::runtime_error("falta --start");
    if (!haveDuration || a.durationSec <= 0) throw std::runtime_error("--duration deve ser > 0");
    if (a.report.empty()) throw std::runtime_error("falta --report");
    if (a.loss < 0 || a.loss > 90) throw std::runtime_error("--loss deve estar entre 0 e 90");
    if (a.script.walkAfterSec < 0 || a.script.walkCm < 0 || a.script.speedCmS < 0)
        throw std::runtime_error("--walk-after, --walk-cm e --speed não podem ser negativos");
    return a;
}

const char* StateName(SessionState s) {
    switch (s) {
        case SessionState::Armed: return "Armed";
        case SessionState::Superseded: return "Superseded";
        case SessionState::Connecting: return "Connecting";
        case SessionState::Handshake: return "Handshake";
        case SessionState::Online: return "Online";
        case SessionState::Disconnected: return "Disconnected";
    }
    return "?";
}

double UnixNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

nlohmann::json Point(const cbmp::headless::PathPoint& p) { return nlohmann::json::array({p.t, p.pos.x, p.pos.y, p.pos.z}); }

int Run(const Args& args) {
    std::string err;
    if (!GnsInit(&err)) {
        std::fprintf(stderr, "erro: GameNetworkingSockets_Init: %s\n", err.c_str());
        return 2;
    }
    CaptureLog log;
    ScriptedGame game(args.script);
    bool online = false;
    nlohmann::json report;
    {
        GnsTransport transport(GnsOptions{args.loss});
        LocalGate gate;
        FileConnectFs connectFs(args.connect);
        SessionConfig cfg;
        cfg.launchId = args.launchId;
        Session session(SessionDeps{game, transport, gate, connectFs,
                                    [&log](Level l, std::string_view m) { log.Write(l, m); },
                                    [] { return static_cast<int64_t>(UnixNow()); },
                                    [&log](std::string_view s) { log.SetSecret(s); },
                                    [&log] { log.ClearSecret(); }},
                        cfg);

        const double startedAt = UnixNow();
        const auto t0 = std::chrono::steady_clock::now();
        auto next = t0;
        for (;;) {
            next += kTickPeriod;
            std::this_thread::sleep_until(next);
            const auto now = std::chrono::steady_clock::now();
            if (now - next > 4 * kTickPeriod) next = now;  // atrasou muito: não tenta compensar com rajada
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            game.Advance(elapsed, UnixNow());
            session.Tick(std::chrono::duration<double>(now.time_since_epoch()).count());
            if (session.State() == SessionState::Online) online = true;
            if (elapsed >= args.durationSec) break;
        }
        session.Shutdown();
        const double endedAt = UnixNow();

        const NetCounters& net = session.Net();
        report["state"] = StateName(session.State());
        report["reason"] = session.DisconnectReason();
        report["playerId"] = session.PlayerId();
        report["online"] = online;
        report["startedAt"] = startedAt;
        report["endedAt"] = endedAt;
        report["inputs"] = net.inputs;
        report["snapshots"] = net.snapshots;
        report["acks"] = net.acks;
        auto& local = report["local"] = nlohmann::json::array();
        for (const auto& p : game.LocalPath()) local.push_back(Point(p));
        auto& puppets = report["puppets"] = nlohmann::json::array();
        for (const auto& [id, r] : game.Puppets()) {
            nlohmann::json pj{{"id", r.id}, {"createdAt", r.createdAt}};
            pj["destroyedAt"] = r.destroyedAt ? nlohmann::json(*r.destroyedAt) : nlohmann::json(nullptr);
            auto& path = pj["path"] = nlohmann::json::array();
            for (const auto& p : r.path) path.push_back(Point(p));
            puppets.push_back(std::move(pj));
        }
        report["statuses"] = game.Statuses();
    }
    // Deixa o GNS mandar o fechamento antes de encerrar (como o cb-bots).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    GnsShutdown();
    report["log"] = log.Lines();

    std::ofstream f(args.report, std::ios::binary);
    f << report.dump(1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    if (!f) {
        std::fprintf(stderr, "erro: não foi possível gravar o relatório\n");
        return 2;
    }
    return online ? 0 : 2;
}
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // sleep de 1 ms de verdade (padrão do Windows é ~15,6 ms)
#endif
    Args args;
    try {
        args = ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\n%s", e.what(), kUsage);
        return 1;
    }
    try {
        return Run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\n", e.what());
        return 2;
    }
}
