#pragma once
// Máquina de estados da sessão do mod (spec do sub5a, "Protocolo do cliente" e "Erros e falhas"): resgate do
// connect.json pela regra dos dois processos, conexão, Hello/Welcome, Input a 20 Hz em Playing, snapshots, bonecos,
// correções e a tabela de motivos de desconexão. Tudo roda no game thread, dentro de Tick, que nunca lança.
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <cbmp/client/ConnectFile.hpp>
#include <cbmp/client/IGame.hpp>
#include <cbmp/client/Interp.hpp>
#include <cbmp/client/LocalSampler.hpp>
#include <cbmp/client/Log.hpp>
#include <cbmp/client/NetClient.hpp>
#include <cbmp/client/ProcessGate.hpp>
#include <cbmp/client/PuppetDirector.hpp>
#include <cbmp/client/RemoteStore.hpp>
#include <cbmp/client/SendScheduler.hpp>
#include <cbmp/client/SnapshotMirror.hpp>
#include <cbmp/client/Transport.hpp>
#include <cbmp/client/WorldTracker.hpp>

namespace cbmp::client {
enum class SessionState { Armed, Superseded, Connecting, Handshake, Online, Disconnected };
enum class OnlinePhase { AwaitWorld, Playing };

struct SessionConfig {
    std::string launchId;
    std::array<uint8_t, 32> buildHash{};
    bool statusOnScreen = true;
    int maxPuppets = 32;
};

struct SessionDeps {
    IGame& game;
    ITransport& transport;
    IProcessGate& gate;
    IConnectFs& connectFs;
    LogFn log;
    std::function<int64_t()> unixNow;
    std::function<void(std::string_view)> setSecret;  // FileLogger::SetSecret (pode ser vazio)
    std::function<void()> clearSecret;
};

class Session final : private INetSink {
public:
    Session(SessionDeps deps, SessionConfig cfg);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void Tick(double nowSec);  // única entrada; nunca lança
    void Shutdown();           // jogo fechando: log `encerrando`, Close sem esperar
    SessionState State() const;
    OnlinePhase Phase() const;
    const std::string& DisconnectReason() const;  // "" até desconectar
    uint16_t PlayerId() const;
    bool HoldsTicket() const;  // testes: há ticket em memória?
    const NetCounters& Net() const;
    const PuppetStats& Puppets() const;
    const SnapshotMirror& Mirror() const;  // testes: histórico de snapshots

private:
    // INetSink (nunca lançam: falhas viram exceção de tick contada)
    void OnWelcome(const cbmp::Welcome& w) override;
    void OnSnapshot(const cbmp::Snapshot& s, double now) override;
    void OnEntityEnter(const cbmp::EntityEnter& e, double now) override;
    void OnEntityLeave(const cbmp::EntityLeave& e) override;
    void OnCorrection(const cbmp::Correction& c, double now) override;
    void OnRefusal(std::string_view sanitizedText, bool kick) override;

    // Linhas de log disparadas por mensagens do servidor: no máximo limits::kServerLogLinesPerSec por categoria a cada
    // janela de 1 s; as excedentes viram uma linha "N linhas suprimidas (<categoria>)" por janela. Transições de
    // estado e erros mostrados ao jogador nunca passam por aqui.
    enum class LogCat : uint8_t { Correction, EntityEnter, EntityLeave, Refusal, StrayWelcome, Ignored, Puppets, Count };
    struct LogBudget {
        double windowStart = -1;
        int used = 0;
        uint64_t dropped = 0;
    };

    void Log(Level level, std::string_view msg);
    void LogLimited(LogCat cat, Level level, std::string_view msg);
    void FlushSuppressed(double now, bool force);
    void SampleWorld(double now);
    void TickArmed(double now);
    void Claim(double now);
    void TickConnecting(double now);
    void TickHandshake(double now);
    void TickOnline(double now);
    bool PumpOk(double now);    // false = desconectou
    bool CheckClosed();         // true = desconectou
    void EnterPlaying(double now);
    void LeavePlaying(bool worldChanged);
    void RunPendingTeleport(const Vec3& pos, double now);
    void TryTeleport(const Vec3& pos, double now);
    void Disconnect(std::string reason);
    std::string ReasonFromEnd(const LinkEnd& end) const;
    void QueueStatus(std::string text);
    void FlushStatus();
    void NoteGameError();
    void NoteException(double now, const char* what);
    void MaybeSummarizeExceptions(double now);
    std::string CountersLine() const;

    SessionDeps m_deps;
    SessionConfig m_cfg;

    NetClient m_net;
    SnapshotMirror m_mirror;
    RemoteStore m_store;
    Interp m_interp;
    PuppetDirector m_puppets;
    WorldTracker m_world;
    LocalSampler m_sampler;
    SendScheduler m_sched;

    SessionState m_state = SessionState::Armed;
    OnlinePhase m_phase = OnlinePhase::AwaitWorld;
    std::string m_reason;
    uint16_t m_playerId = 0;
    bool m_shutdown = false;
    double m_now = 0;

    // Mundo (último tick)
    WorldSample m_lastWorld{};
    std::optional<LocalState> m_local;
    WorldPhase m_worldPhase = WorldPhase::Loading;
    WorldChange m_change;
    std::string m_worldLabel = "-";
    uint64_t m_gameErrors = 0;

    // Armed
    std::optional<double> m_firstTick;
    bool m_owner = false;
    std::optional<double> m_busySince;

    // Connecting / Handshake
    std::string m_ticket;
    double m_stateSince = 0;
    std::optional<std::string> m_refusal;

    // Online
    uint32_t m_seq = 0;
    bool m_connectedShown = false;
    double m_nextStats = 0;
    std::optional<double> m_lastTeleport;
    std::optional<Vec3> m_pendingTeleport;
    std::optional<uint32_t> m_lastFarSeq;  // seq da correção anterior, se ela foi longe (ou em AwaitWorld)
    bool m_positionRefused = false;

    // Status na tela
    std::deque<std::string> m_pendingStatus;
    bool m_statusDisabled = false;

    // Teto das linhas de log vindas do servidor (por categoria)
    std::array<LogBudget, static_cast<size_t>(LogCat::Count)> m_logBudget{};

    // Exceções no tick
    std::deque<double> m_exceptions;  // instantes (últimos 60 s)
    uint64_t m_exceptionsTotal = 0, m_exceptionsSinceSummary = 0;
    double m_lastSummary = 0;
};

// "CB Multiplayer: desconectado — <frase>" para o motivo de desconexão.
std::string DisconnectStatusText(std::string_view reason);
}  // namespace cbmp::client
