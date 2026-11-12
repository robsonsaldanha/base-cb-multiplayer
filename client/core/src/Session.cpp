#include <cbmp/client/Session.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <utility>

#include <cbmp/client/ClaimPolicy.hpp>
#include <cbmp/client/Limits.hpp>

namespace cbmp::client {
namespace {
constexpr double kTimeEps = 1e-9;  // arredondamento de soma de tempos (s)
constexpr std::string_view kStatusPrefix = "CB Multiplayer: ";

bool IsFinite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double Dist(const Vec3& a, const Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
Vec3 ToVec(const cbmp::Vec3i& v) {
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}
const char* WhatOf(const std::exception_ptr& p) {
    try {
        if (p) std::rethrow_exception(p);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
    }
    return "exceção desconhecida";
}
}  // namespace

std::string DisconnectStatusText(std::string_view reason) {
    constexpr std::string_view kRefused = "recusado: ", kKicked = "expulso: ";
    std::string phrase;
    if (reason == "sem_arquivo" || reason == "ticket_expirado") {
        phrase = "entrada expirou; feche o jogo e clique Jogar";
    } else if (reason == "arquivo_invalido") {
        phrase = "arquivo de entrada inválido";
    } else if (reason == "protocolo_diferente") {
        phrase = "versão do mod diferente da do servidor";
    } else if (reason == "sem_resposta" || reason == "sem_welcome") {
        phrase = "servidor não respondeu";
    } else if (reason.starts_with(kRefused)) {
        phrase = std::string(reason);
    } else if (reason.starts_with(kKicked)) {
        phrase = std::string(reason.substr(kKicked.size()));
    } else if (reason == "conexao_perdida") {
        phrase = "conexão perdida";
    } else if (reason == "servidor_invalido" || reason == "servidor_incompativel") {
        phrase = "servidor incompatível";
    } else if (reason == "erro_interno") {
        phrase = "erro interno";
    } else {
        phrase = std::string(reason);
    }
    return std::string(kStatusPrefix) + "desconectado — " + phrase;
}

Session::Session(SessionDeps deps, SessionConfig cfg)
    : m_deps(std::move(deps)),
      m_cfg(std::move(cfg)),
      m_net(m_deps.transport, *this),
      m_puppets(m_deps.game, m_store, m_interp,
                [this](Level level, std::string_view msg) {
                    // Linhas de um só disparo (trava do singleton, bonecos desligados) nunca são suprimidas.
                    if (msg.starts_with("bonecos_desligados") || msg.starts_with("singleton")) {
                        Log(level, msg);
                    } else {
                        LogLimited(LogCat::Puppets, level, msg);
                    }
                },
                m_cfg.maxPuppets) {}

SessionState Session::State() const { return m_state; }
OnlinePhase Session::Phase() const { return m_phase; }
const std::string& Session::DisconnectReason() const { return m_reason; }
uint16_t Session::PlayerId() const { return m_playerId; }
bool Session::HoldsTicket() const { return !m_ticket.empty(); }
const NetCounters& Session::Net() const { return m_net.Counters(); }
const PuppetStats& Session::Puppets() const { return m_puppets.Stats(); }
const SnapshotMirror& Session::Mirror() const { return m_mirror; }

void Session::Log(Level level, std::string_view msg) {
    if (!m_deps.log) return;
    try {
        m_deps.log(level, msg);
    } catch (...) {
    }
}

namespace {
const char* CatName(size_t cat) {
    static constexpr const char* kNames[] = {"Correction",  "EntityEnter", "EntityLeave", "Reject/Kick",
                                             "Welcome fora do handshake", "mensagens ignoradas", "bonecos"};
    return cat < std::size(kNames) ? kNames[cat] : "?";
}
}  // namespace

void Session::LogLimited(LogCat cat, Level level, std::string_view msg) {
    LogBudget& b = m_logBudget[static_cast<size_t>(cat)];
    if (b.windowStart < 0 || m_now - b.windowStart + kTimeEps >= limits::kServerLogWindowSec) {
        if (b.dropped > 0) {
            Log(Level::Warn, std::to_string(b.dropped) + " linhas suprimidas (" + CatName(static_cast<size_t>(cat)) + ")");
        }
        b = LogBudget{m_now, 0, 0};
    }
    if (b.used < limits::kServerLogLinesPerSec) {
        ++b.used;
        Log(level, msg);
    } else {
        ++b.dropped;
    }
}

// Fecha as janelas vencidas (ou todas, com force) e escreve o resumo das linhas suprimidas.
void Session::FlushSuppressed(double now, bool force) {
    for (size_t i = 0; i < m_logBudget.size(); ++i) {
        LogBudget& b = m_logBudget[i];
        if (b.dropped == 0) continue;
        if (!force && now - b.windowStart + kTimeEps < limits::kServerLogWindowSec) continue;
        Log(Level::Warn, std::to_string(b.dropped) + " linhas suprimidas (" + CatName(i) + ")");
        b = LogBudget{};
    }
}

// ---------------------------------------------------------------- tick

void Session::Tick(double nowSec) {
    if (m_shutdown || m_state == SessionState::Superseded) return;
    m_now = nowSec;
    try {
        MaybeSummarizeExceptions(nowSec);
        FlushSuppressed(nowSec, false);
        SampleWorld(nowSec);
        switch (m_state) {
            case SessionState::Armed: TickArmed(nowSec); break;
            case SessionState::Connecting: TickConnecting(nowSec); break;
            case SessionState::Handshake: TickHandshake(nowSec); break;
            case SessionState::Online: TickOnline(nowSec); break;
            case SessionState::Superseded:
            case SessionState::Disconnected: break;
        }
        FlushStatus();
    } catch (...) {
        NoteException(nowSec, WhatOf(std::current_exception()));
    }
}

void Session::NoteGameError() { ++m_gameErrors; }

void Session::SampleWorld(double now) {
    WorldSample w = m_lastWorld;
    bool worldOk = true;
    try {
        w = m_deps.game.SampleWorld();
    } catch (const GameError&) {
        NoteGameError();
        worldOk = false;
    }
    std::optional<LocalState> local;
    try {
        local = m_deps.game.ReadLocal();
    } catch (const GameError&) {
        NoteGameError();
    }
    if (worldOk) {
        m_lastWorld = w;
    } else {
        w.hasPawn = false;  // trata como "sem pawn"
        local.reset();
    }
    m_local = local;
    m_worldPhase = m_world.Update(w, m_local, now);
    m_change = m_world.TakeChange();  // só o que mudou neste tick

    std::string label = m_world.WorldName();
    if (m_worldPhase == WorldPhase::InWorld) label += " InWorld";
    if (label != m_worldLabel) {
        Log(Level::Info, "mundo: " + m_worldLabel + " → " + label);
        m_worldLabel = std::move(label);
    }
}

// ---------------------------------------------------------------- Armed

void Session::TickArmed(double now) {
    if (!m_firstTick) m_firstTick = now;
    if (m_owner) {
        Claim(now);  // violação de compartilhamento: tenta de novo
        return;
    }
    const auto decision = DecideClaim(m_deps.gate.IsCurrent(), m_worldPhase == WorldPhase::InWorld,
                                      now - *m_firstTick, m_deps.gate.Epoch());
    if (decision == ClaimDecision::Wait) return;
    if (decision == ClaimDecision::Superseded) {
        Log(Level::Info, "substituido epoca=" + std::to_string(m_deps.gate.Epoch()) + "→" +
                             std::to_string(m_deps.gate.Latest()));
        m_state = SessionState::Superseded;
        return;
    }
    if (!m_deps.gate.TryOwn()) {
        Log(Level::Info, "substituido: outro processo é o dono");
        m_state = SessionState::Superseded;
        return;
    }
    m_owner = true;
    Log(Level::Info, "dono epoca=" + std::to_string(m_deps.gate.Epoch()));
    Claim(now);
}

void Session::Claim(double now) {
    ClaimResult r = m_deps.connectFs.ClaimAndDelete();
    switch (r.status) {
        case ClaimRead::Busy:
            if (!m_busySince) {
                m_busySince = now;
                Log(Level::Info, "connect.json em uso; tentando de novo");
            }
            if (now - *m_busySince + kTimeEps >= limits::kClaimRetrySec) Disconnect("sem_arquivo");
            return;
        case ClaimRead::Missing: Disconnect("sem_arquivo"); return;
        case ClaimRead::Rejected: Disconnect("arquivo_invalido"); return;
        case ClaimRead::Ok: break;
    }

    const int64_t unixNow = m_deps.unixNow ? m_deps.unixNow() : 0;
    ConnectParse parse = ParseConnect(r.bytes, m_cfg.launchId, unixNow);
    SecureZero(r.bytes);
    if (!parse.info) {
        if (!parse.why.empty()) Log(Level::Warn, "connect.json recusado: " + parse.why);
        switch (parse.error) {
            case ConnectError::ProtocolMismatch: Disconnect("protocolo_diferente"); return;
            case ConnectError::TicketExpired: Disconnect("ticket_expirado"); return;
            case ConnectError::None:
            case ConnectError::Invalid: Disconnect("arquivo_invalido"); return;
        }
        return;
    }
    ConnectInfo& info = *parse.info;
    m_ticket = std::move(info.ticket);
    SecureZero(info.ticket);
    if (m_deps.setSecret) m_deps.setSecret(m_ticket);

    Log(Level::Info, "conectando a " + info.host + " porta " + std::to_string(info.port) + " servidor " +
                         SanitizeText(info.serverId, limits::kMaxServerIdBytes));
    bool connecting = false;
    try {
        connecting = m_deps.transport.Connect(info.host, info.port);
    } catch (...) {
        connecting = false;  // sem isso o próximo tick resgataria de novo um arquivo já apagado (revisão, M1)
    }
    if (!connecting) {
        Disconnect("sem_resposta");
        return;
    }
    m_state = SessionState::Connecting;
    m_stateSince = now;
}

// ---------------------------------------------------------------- Connecting / Handshake

void Session::TickConnecting(double now) {
    const LinkState link = m_deps.transport.State();
    if (link == LinkState::Connected) {
        const bool sent = m_net.SendHello(m_cfg.buildHash, m_ticket);  // zera m_ticket
        SecureZero(m_ticket);
        if (m_deps.clearSecret) m_deps.clearSecret();
        if (!sent) {
            // O transporte recusou o envio: o servidor nunca vai responder (revisão da Task 5, M1).
            Log(Level::Warn, "Hello não enviado");
            Disconnect("sem_resposta");
            return;
        }
        Log(Level::Info, "Hello enviado");
        m_state = SessionState::Handshake;
        m_stateSince = now;
        return;
    }
    if (link == LinkState::Closed) {
        Disconnect(ReasonFromEnd(m_deps.transport.End()));
        return;
    }
    if (now - m_stateSince + kTimeEps >= limits::kConnectTimeoutSec) Disconnect("sem_resposta");
}

void Session::TickHandshake(double now) {
    if (!PumpOk(now)) return;
    if (m_state != SessionState::Handshake) return;  // Welcome (ou desconexão) durante o Pump
    if (CheckClosed()) return;
    if (now - m_stateSince + kTimeEps >= limits::kWelcomeTimeoutSec) Disconnect("sem_welcome");
}

bool Session::PumpOk(double now) {
    const NetFault fault = m_net.Pump(now);
    if (m_state == SessionState::Disconnected) return false;
    if (fault != NetFault::None) {
        Disconnect("servidor_invalido");
        return false;
    }
    return true;
}

bool Session::CheckClosed() {
    if (m_deps.transport.State() != LinkState::Closed) return false;
    Disconnect(ReasonFromEnd(m_deps.transport.End()));
    return true;
}

std::string Session::ReasonFromEnd(const LinkEnd& end) const {
    const auto text = [&] {
        if (m_refusal && !m_refusal->empty()) return *m_refusal;
        std::string debug = SanitizeText(end.debug, limits::kMaxReasonBytes);
        if (!debug.empty()) return debug;
        return std::string("sem motivo");
    };
    if (end.code == limits::kEndRejected || end.code == limits::kEndLimits || end.code == limits::kEndProtocol) {
        return "recusado: " + text();
    }
    if (end.code == limits::kEndKicked) return "expulso: " + text();
    return m_state == SessionState::Connecting ? "sem_resposta" : "conexao_perdida";
}

// ---------------------------------------------------------------- Online

void Session::TickOnline(double now) {
    // Sair de Playing vem antes de tratar as mensagens: no tick em que o mundo (ou o pawn) troca, os bonecos já
    // foram soltos (ForgetAllPuppets se o mundo trocou) quando um EntityLeave, uma desconexão ou uma Correction
    // chegarem, e nada disso toca atores que morreram com o mundo antigo (revisão da Task 5, I1).
    const bool inWorld = m_worldPhase == WorldPhase::InWorld;
    if (m_phase == OnlinePhase::Playing && !inWorld) LeavePlaying(m_change.worldChanged);

    if (!PumpOk(now)) return;
    if (CheckClosed()) return;

    if (m_phase == OnlinePhase::AwaitWorld && inWorld) EnterPlaying(now);

    if (m_phase == OnlinePhase::Playing && m_local) {
        if (m_pendingTeleport && (!m_lastTeleport || now - *m_lastTeleport + kTimeEps >= limits::kTeleportMinIntervalSec)) {
            const Vec3 pos = *m_pendingTeleport;
            m_pendingTeleport.reset();
            RunPendingTeleport(pos, now);
        }
        if (m_sched.Due(now)) {
            if (auto st = m_sampler.Sample(*m_local)) {
                // O Input não é confiável: o salto (Apparate) se repete nos Input seguintes, senão um único pacote
                // perdido deixa o servidor com a posição antiga (spawn na origem) até a carência de 30 s.
                const bool jump = st->state == cbmp::MoveState::Apparate;
                if (jump) {
                    m_apparateLeft = limits::kApparateRepeatInputs - 1;
                } else if (m_apparateLeft > 0) {
                    st->state = cbmp::MoveState::Apparate;
                    --m_apparateLeft;
                }
                m_net.SendInput(++m_seq, *st);
                if (jump) Log(Level::Info, "Input Apparate seq=" + std::to_string(m_seq));
            }
        }
        m_puppets.Update(now, m_local->pos);
    }

    if (now + kTimeEps >= m_nextStats) {
        m_nextStats = now + limits::kStatsIntervalSec;
        Log(Level::Info, CountersLine());
    }
}

std::string Session::CountersLine() const {
    const NetCounters& c = m_net.Counters();
    const PuppetStats& p = m_puppets.Stats();
    return "contadores snapshots=" + std::to_string(c.snapshots) + " acks=" + std::to_string(c.acks) +
           " correcoes=" + std::to_string(c.corrections) + " bonecos=" + std::to_string(p.alive) +
           " erros_jogo=" + std::to_string(m_gameErrors + p.gameErrors) + " inputs=" + std::to_string(c.inputs) +
           " entradas=" + std::to_string(c.enters) + " saidas=" + std::to_string(c.leaves);
}

size_t Session::KnownRemotes() const {
    size_t n = 0;
    for (const auto& kv : m_store.All()) n += kv.first != m_playerId ? 1 : 0;
    return n;
}

void Session::EnterPlaying(double) {
    m_phase = OnlinePhase::Playing;
    m_sampler.MarkJump();
    m_sched.Reset();
    m_pendingTeleport.reset();
    if (!m_connectedShown) {
        m_connectedShown = true;
        // N = jogadores que o servidor já anunciou (EntityEnter), não bonecos: os bonecos só nascem em Playing, mais
        // adiante neste tick (revisão final, M1).
        QueueStatus(std::string(kStatusPrefix) + "conectado (" + std::to_string(KnownRemotes()) + " por perto)");
    }
}

void Session::LeavePlaying(bool worldChanged) {
    m_apparateLeft = 0;
    m_phase = OnlinePhase::AwaitWorld;
    m_pendingTeleport.reset();
    m_puppets.ReleaseAll(worldChanged);
}

// Correction adiada pelo limite de 1/s: mede de novo contra o pawn de agora. Nunca move mais que 20 m; se o jogador
// já está mais longe, ressincroniza com Apparate (revisão da Task 5, I2).
void Session::RunPendingTeleport(const Vec3& pos, double now) {
    if (!m_local || !IsFinite(m_local->pos)) return;
    const double dist = Dist(pos, m_local->pos);
    if (dist <= limits::kCorrectionIgnoreCm) return;
    if (dist > limits::kCorrectionTeleportMaxCm) {
        m_sampler.MarkJump();
        Log(Level::Info, "Correction adiada agora a " + std::to_string(std::llround(dist)) + " cm: ressincroniza com Apparate");
        return;
    }
    TryTeleport(pos, now);
}

void Session::TryTeleport(const Vec3& pos, double now) {
    m_lastTeleport = now;
    try {
        if (!m_deps.game.TeleportLocal(pos)) Log(Level::Warn, "TeleportLocal recusado pelo jogo");
    } catch (const GameError& e) {
        NoteGameError();
        Log(Level::Warn, std::string("TeleportLocal falhou: ") + e.what());
    }
}

// ---------------------------------------------------------------- INetSink

void Session::OnWelcome(const cbmp::Welcome& w) {
    try {
        if (m_state != SessionState::Handshake) {
            LogLimited(LogCat::StrayWelcome, Level::Warn, "Welcome fora do handshake ignorado");
            return;
        }
        const int hz = w.tickRateHz;
        if (hz < limits::kMinTickRateHz || hz > limits::kMaxTickRateHz) {
            Log(Level::Warn, "Welcome com tickRateHz=" + std::to_string(hz));
            Disconnect("servidor_incompativel");
            return;
        }
        m_playerId = w.playerId;
        m_sched.SetRate(hz);
        m_interp.SetTickRate(hz);
        m_interp.Clear();  // tick do servidor recomeça em 0 depois de um reinício
        m_mirror.Reset();
        m_puppets.SetSelf(w.playerId);
        Log(Level::Info, "Welcome playerId=" + std::to_string(w.playerId) + " tickRateHz=" + std::to_string(hz));
        m_state = SessionState::Online;
        m_phase = OnlinePhase::AwaitWorld;
        m_nextStats = m_now + limits::kStatsIntervalSec;
    } catch (...) {
        NoteException(m_now, WhatOf(std::current_exception()));
    }
}

void Session::OnSnapshot(const cbmp::Snapshot& s, double now) {
    try {
        if (m_state != SessionState::Online) return;
        const auto applied = m_mirror.OnSnapshot(s);
        if (!applied || !applied->record) return;
        const uint32_t tick = applied->record->tick;
        m_net.SendAck(tick);
        m_interp.OnTick(tick, now);
        const auto& ents = applied->record->ents;
        for (const uint16_t id : applied->changed) {
            if (!m_store.Known(id)) continue;  // sem EntityEnter: só histórico
            const auto it = std::lower_bound(ents.begin(), ents.end(), id,
                                             [](const MirrorEntity& e, uint16_t v) { return e.id < v; });
            if (it == ents.end() || it->id != id) continue;
            m_store.Update(id, it->s, now);
            m_interp.Push(id, tick, it->s);
        }
    } catch (...) {
        NoteException(now, WhatOf(std::current_exception()));
    }
}

void Session::OnEntityEnter(const cbmp::EntityEnter& e, double now) {
    try {
        if (m_state != SessionState::Online) return;
        if (!m_store.Enter(e, now)) {
            LogLimited(LogCat::Ignored, Level::Warn, "EntityEnter id=" + std::to_string(e.id) + " ignorado (cheio)");
            return;
        }
        m_interp.PushNow(e.id, e.s, now);
        const auto it = m_store.All().find(e.id);
        const std::string name = it != m_store.All().end() ? it->second.name : std::string();
        LogLimited(LogCat::EntityEnter, Level::Info, "EntityEnter id=" + std::to_string(e.id) + " nome=" + name);
    } catch (...) {
        NoteException(now, WhatOf(std::current_exception()));
    }
}

void Session::OnEntityLeave(const cbmp::EntityLeave& e) {
    try {
        if (m_state != SessionState::Online) return;
        m_store.Leave(e.id);
        m_mirror.Forget(e.id);
        m_interp.Remove(e.id);
        m_puppets.OnEntityLeave(e.id);
        LogLimited(LogCat::EntityLeave, Level::Info, "EntityLeave id=" + std::to_string(e.id));
    } catch (...) {
        NoteException(m_now, WhatOf(std::current_exception()));
    }
}

void Session::OnCorrection(const cbmp::Correction& c, double now) {
    try {
        if (m_state != SessionState::Online) return;
        const Vec3 target = ToVec(c.pos);
        // Posição local não finita conta como "sem pawn" (revisão da Task 5, M5).
        const bool playing = m_phase == OnlinePhase::Playing && m_local.has_value() && IsFinite(m_local->pos);
        const double dist = playing ? Dist(target, m_local->pos) : -1;
        LogLimited(LogCat::Correction, Level::Info,
                   "Correction seq=" + std::to_string(c.seq) + " distancia=" +
                       (playing ? std::to_string(std::llround(dist)) + " cm" : std::string("sem pawn")));

        if (!playing) {
            // AwaitWorld: guarda e ressincroniza no próximo Input (vale como correção longe).
            m_sampler.MarkJump();
            m_lastFarSeq = c.seq;
            return;
        }
        m_pendingTeleport.reset();  // só a mais recente vale
        if (dist <= limits::kCorrectionIgnoreCm) {
            m_lastFarSeq.reset();
            return;
        }
        if (dist <= limits::kCorrectionTeleportMaxCm) {
            m_lastFarSeq.reset();
            if (!m_lastTeleport || now - *m_lastTeleport + kTimeEps >= limits::kTeleportMinIntervalSec) {
                TryTeleport(target, now);
            } else {
                m_pendingTeleport = target;
            }
            return;
        }
        // Longe: não move; o próximo Input vai com Apparate.
        m_sampler.MarkJump();
        const bool repeated = m_lastFarSeq && *m_lastFarSeq == c.seq;
        m_lastFarSeq = c.seq;
        if (repeated && !m_positionRefused) {
            m_positionRefused = true;
            Log(Level::Warn, "posicao_recusada seq=" + std::to_string(c.seq));
            QueueStatus(std::string(kStatusPrefix) + "o servidor não aceitou sua posição");
        }
    } catch (...) {
        NoteException(now, WhatOf(std::current_exception()));
    }
}

void Session::OnRefusal(std::string_view sanitizedText, bool kick) {
    try {
        m_refusal = std::string(sanitizedText);
        LogLimited(LogCat::Refusal, Level::Warn, std::string(kick ? "Kick: " : "Reject: ") + m_refusal.value());
    } catch (...) {
        NoteException(m_now, WhatOf(std::current_exception()));
    }
}

// ---------------------------------------------------------------- desconexão, status, exceções

void Session::Disconnect(std::string reason) {
    if (m_state == SessionState::Disconnected || m_state == SessionState::Superseded) return;
    m_state = SessionState::Disconnected;
    m_reason = std::move(reason);
    if (m_reason == "servidor_invalido") {
        const NetCounters& c = m_net.Counters();
        std::size_t pending = 0;
        try {
            pending = m_deps.transport.Pending();
        } catch (...) {
        }
        Log(Level::Warn, "desconectado: " + m_reason + " recebidas=" + std::to_string(c.received) +
                             " invalidas=" + std::to_string(c.decodeErrors) + " grandes=" +
                             std::to_string(c.oversized) + " pendentes=" + std::to_string(pending));
    } else {
        Log(Level::Warn, "desconectado: " + m_reason);
    }
    try {
        // Mundo trocado neste tick: os atores já morreram, só ForgetAllPuppets (revisão da Task 5, I1). Fora de
        // Playing os bonecos já foram soltos.
        if (m_phase == OnlinePhase::Playing) m_puppets.ReleaseAll(m_change.worldChanged);
    } catch (...) {
    }
    m_phase = OnlinePhase::AwaitWorld;  // revisão da Task 5, M2
    try {
        m_deps.transport.Close();
    } catch (...) {
    }
    SecureZero(m_ticket);
    try {
        if (m_deps.clearSecret) m_deps.clearSecret();
    } catch (...) {
    }
    m_pendingTeleport.reset();
    m_store.Clear();
    m_interp.Clear();
    m_mirror.Reset();
    m_pendingStatus.clear();
    QueueStatus(DisconnectStatusText(m_reason));
}

void Session::Shutdown() {
    if (m_shutdown) return;
    m_shutdown = true;
    FlushSuppressed(m_now, true);
    Log(Level::Info, "encerrando");
    const bool linked = m_state == SessionState::Connecting || m_state == SessionState::Handshake ||
                        m_state == SessionState::Online;
    if (linked) {
        try {
            m_deps.transport.Close();
        } catch (...) {
        }
    }
    SecureZero(m_ticket);
    try {
        if (m_deps.clearSecret) m_deps.clearSecret();
    } catch (...) {
    }
}

void Session::QueueStatus(std::string text) {
    if (!m_cfg.statusOnScreen || m_statusDisabled) return;
    m_pendingStatus.push_back(std::move(text));
}

void Session::FlushStatus() {
    if (m_worldPhase != WorldPhase::InWorld) return;
    while (!m_pendingStatus.empty()) {
        const std::string text = std::move(m_pendingStatus.front());
        m_pendingStatus.pop_front();
        if (!m_cfg.statusOnScreen || m_statusDisabled) continue;
        bool ok = false;
        std::string why = "ShowStatus devolveu false";
        try {
            ok = m_deps.game.ShowStatus(text);
        } catch (const std::exception& e) {
            NoteGameError();
            why = std::string("ShowStatus falhou: ") + e.what();
        } catch (...) {
            NoteGameError();
            why = "ShowStatus falhou";
        }
        Log(Level::Info, "status: " + text);
        if (!ok) {
            m_statusDisabled = true;
            m_pendingStatus.clear();
            Log(Level::Warn, "status na tela desligado: " + why);
        }
    }
}

void Session::NoteException(double now, const char* what) {
    ++m_exceptionsTotal;
    ++m_exceptionsSinceSummary;
    if (m_exceptionsTotal == 1) {
        Log(Level::Error, std::string("exceção no tick: ") + what);
        m_exceptionsSinceSummary = 0;
        m_lastSummary = now;
    }
    m_exceptions.push_back(now);
    while (!m_exceptions.empty() && m_exceptions.front() < now - limits::kTickExceptionWindowSec) {
        m_exceptions.pop_front();
    }
    if (static_cast<int>(m_exceptions.size()) >= limits::kTickExceptionLimit) {
        try {
            Disconnect("erro_interno");
        } catch (...) {
        }
    }
}

void Session::MaybeSummarizeExceptions(double now) {
    if (m_exceptionsSinceSummary == 0 || now - m_lastSummary + kTimeEps < limits::kExceptionSummarySec) return;
    Log(Level::Warn, "exceções no tick: " + std::to_string(m_exceptionsSinceSummary) + " nos últimos " +
                         std::to_string(std::llround(now - m_lastSummary)) + " s (total " +
                         std::to_string(m_exceptionsTotal) + ")");
    m_exceptionsSinceSummary = 0;
    m_lastSummary = now;
}
}  // namespace cbmp::client
