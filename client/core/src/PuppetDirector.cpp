#include <cbmp/client/PuppetDirector.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <cbmp/client/Limits.hpp>

namespace cbmp::client {
namespace {
constexpr double kTimeEps = 1e-9;          // arredondamento de soma de tempos (s)
constexpr double kRecreateWindowSec = 60;  // "no máximo 5 por minuto"

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Finite(const PuppetPose& p) { return Finite(p.pos) && Finite(p.vel) && std::isfinite(p.yawDeg); }
double Dist(const Vec3& a, const Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
std::string Tag(uint16_t id) { return "boneco id=" + std::to_string(id); }
}  // namespace

PuppetDirector::PuppetDirector(IGame& game, const RemoteStore& store, Interp& interp, LogFn log, int maxPuppets)
    : m_game(game),
      m_store(store),
      m_interp(interp),
      m_log(std::move(log)),
      m_maxPuppets(std::clamp(maxPuppets, 0, limits::kHardMaxPuppets)) {}

void PuppetDirector::SetSelf(uint16_t playerId) { m_self = playerId; }

const PuppetStats& PuppetDirector::Stats() const { return m_stats; }

void PuppetDirector::Info(std::string_view msg) const {
    if (m_log) m_log(Level::Info, msg);
}

void PuppetDirector::Warn(std::string_view msg) const {
    if (m_log) m_log(Level::Warn, msg);
}

bool PuppetDirector::CountError(double now) {
    ++m_stats.gameErrors;
    m_errors.push_back(now);
    while (!m_errors.empty() && m_errors.front() < now - limits::kGameErrorWindowSec) m_errors.pop_front();
    if (static_cast<int>(m_errors.size()) >= limits::kGameErrorsToDisable && !m_disableRequest) {
        m_disableRequest = "erros";
    }
    return m_disableRequest.has_value();
}

void PuppetDirector::Destroy(uint16_t id, Slot& slot, std::string_view why) {
    if (!slot.puppet) return;
    const PuppetId pid = *slot.puppet;
    slot.puppet.reset();
    slot.moveErrors = 0;
    ++m_stats.destroyed;
    try {
        m_game.DestroyPuppet(pid);
    } catch (const GameError& e) {
        ++m_stats.gameErrors;  // sem janela: destruir nunca leva ao desligamento por si só
        Warn(Tag(id) + " DestroyPuppet falhou: " + e.what());
    }
    Info(Tag(id) + " destruído" + std::string(why));
}

void PuppetDirector::GiveUp(uint16_t id, Slot& slot) {
    slot.givenUp = true;
    slot.recreatePending = false;
    ++m_stats.givenUp;
    Warn(Tag(id) + " desistiu");
}

void PuppetDirector::HandleGone(uint16_t id, Slot& slot, double now) {
    slot.puppet.reset();
    slot.moveErrors = 0;
    while (!slot.recreates.empty() && slot.recreates.front() < now - kRecreateWindowSec) slot.recreates.pop_front();
    if (static_cast<int>(slot.recreates.size()) >= limits::kMaxRecreatesPerMinute) {
        GiveUp(id, slot);
        return;
    }
    slot.recreates.push_back(now);
    slot.recreatePending = true;
    slot.spawnFailures = 0;
    slot.retryAt = now + limits::kRecreateDelaySec;
    Info(Tag(id) + " sumiu no jogo; recria em 1 s");
}

void PuppetDirector::Spawn(uint16_t id, Slot& slot, const PuppetPose& pose, double now) {
    std::optional<PuppetId> pid;
    try {
        pid = m_game.SpawnPuppet(pose);
    } catch (const GameError& e) {
        Warn(Tag(id) + " SpawnPuppet falhou: " + e.what());
        CountError(now);
    }
    if (!pid) {
        ++slot.spawnFailures;
        if (slot.spawnFailures > limits::kSpawnRetries) {
            GiveUp(id, slot);
        } else {
            slot.retryAt = now + limits::kSpawnRetrySec;
        }
        return;
    }

    slot.puppet = *pid;
    slot.spawnFailures = 0;
    slot.moveErrors = 0;
    ++m_stats.created;
    if (slot.recreatePending) {
        slot.recreatePending = false;
        ++m_stats.recreated;
        Info(Tag(id) + " recriado");
    } else {
        Info(Tag(id) + " criado");
    }

    bool intact = true;
    try {
        intact = m_game.PlayerSingletonIntact();
    } catch (const GameError& e) {
        Warn(std::string("PlayerSingletonIntact falhou: ") + e.what());
        CountError(now);
        return;  // sem resposta: o boneco fica; confere de novo no próximo boneco criado
    }
    if (intact) {
        if (!m_singletonLogged) {
            m_singletonLogged = true;
            Info("singleton=ok");
        }
        return;
    }
    // O jogo passou a apontar para o boneco: destrói, confere de novo e desliga os bonecos na sessão.
    Destroy(id, slot, " (singleton trocado)");
    bool after = false;
    try {
        after = m_game.PlayerSingletonIntact();
    } catch (const GameError&) {
        ++m_stats.gameErrors;
    }
    Warn(std::string("singleton trocado pelo boneco; depois de destruir: ") + (after ? "ok" : "ainda trocado"));
    m_disableRequest = "singleton";
}

void PuppetDirector::Move(uint16_t id, Slot& slot, const PuppetPose& pose, double now) {
    PuppetResult res = PuppetResult::Failed;
    bool error = false;
    try {
        res = m_game.MovePuppet(*slot.puppet, pose);
    } catch (const GameError& e) {
        error = true;
        if (slot.moveErrors == 0) Warn(Tag(id) + " MovePuppet falhou: " + e.what());
    }
    if (!error && res == PuppetResult::Ok) {
        slot.moveErrors = 0;
        return;
    }
    if (!error && res == PuppetResult::Gone) {
        HandleGone(id, slot, now);
        return;
    }
    // GameError ou Failed: erro de jogo.
    ++slot.moveErrors;
    if (CountError(now)) return;  // desligamento pedido: Disable destrói tudo
    if (slot.moveErrors >= limits::kMoveErrorsToGone) {
        Destroy(id, slot, " (erros seguidos ao mover)");
        HandleGone(id, slot, now);
    }
}

void PuppetDirector::Disable(std::string_view why) {
    m_stats.disabled = true;
    m_stats.disabledWhy = std::string(why);
    Warn("bonecos_desligados: " + std::string(why));
    for (auto& [id, slot] : m_slots) Destroy(id, slot, " (bonecos desligados)");
    m_slots.clear();
    RefreshAlive();
}

void PuppetDirector::RefreshAlive() {
    int n = 0;
    for (const auto& [id, slot] : m_slots) n += slot.puppet ? 1 : 0;
    m_stats.alive = n;
}

void PuppetDirector::Update(double now, const Vec3& localPos) {
    if (m_stats.disabled) return;
    if (!std::isfinite(now) || !Finite(localPos)) return;

    // Entidades que sumiram do RemoteStore sem OnEntityLeave.
    for (auto it = m_slots.begin(); it != m_slots.end();) {
        if (!m_store.Known(it->first)) {
            Destroy(it->first, it->second, " (saiu)");
            it = m_slots.erase(it);
        } else {
            ++it;
        }
    }

    struct Cand {
        uint16_t id;
        double dist;
        PuppetPose pose;
    };
    std::vector<Cand> cands;
    std::set<uint16_t> hold;  // pose inválida neste tick: o boneco fica onde está
    for (const auto& [id, ent] : m_store.All()) {
        if (m_self && id == *m_self) continue;
        const auto sit = m_slots.find(id);
        if (sit != m_slots.end() && sit->second.givenUp) continue;
        const auto p = m_interp.Sample(id, now);
        if (!p) continue;
        const PuppetPose pose{p->pos, p->yawDeg, p->vel, p->moveState};
        if (!Finite(pose)) {
            if (sit != m_slots.end() && sit->second.puppet) hold.insert(id);
            continue;
        }
        const double d = Dist(pose.pos, localPos);
        if (d <= limits::kPuppetRadiusCm) cands.push_back({id, d, pose});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.dist != b.dist ? a.dist < b.dist : a.id < b.id; });
    const size_t room = static_cast<size_t>(std::max(0, m_maxPuppets - static_cast<int>(hold.size())));
    if (cands.size() > room) cands.resize(room);

    std::set<uint16_t> chosen;
    for (const auto& c : cands) chosen.insert(c.id);
    for (auto& [id, slot] : m_slots) {
        if (slot.puppet && !chosen.contains(id) && !hold.contains(id)) Destroy(id, slot, " (fora do alcance)");
    }

    for (const auto& c : cands) {
        Slot& slot = m_slots[c.id];
        if (slot.puppet) {
            Move(c.id, slot, c.pose, now);
        } else if (now + kTimeEps >= slot.retryAt) {
            Spawn(c.id, slot, c.pose, now);
        }
        if (m_disableRequest) break;
    }

    if (m_disableRequest) {
        const std::string why = *m_disableRequest;
        m_disableRequest.reset();
        Disable(why);
        return;
    }
    RefreshAlive();
}

void PuppetDirector::OnEntityLeave(uint16_t id) {
    const auto it = m_slots.find(id);
    if (it == m_slots.end()) return;
    Destroy(id, it->second, "");
    m_slots.erase(it);
    RefreshAlive();
}

void PuppetDirector::ReleaseAll(bool worldChanged) {
    if (worldChanged) {
        // Mundo trocado: os atores já morreram com o mundo; o jogo só esquece as referências.
        try {
            m_game.ForgetAllPuppets();
        } catch (const GameError& e) {
            ++m_stats.gameErrors;
            Warn(std::string("ForgetAllPuppets falhou: ") + e.what());
        }
        int n = 0;
        for (const auto& [id, slot] : m_slots) n += slot.puppet ? 1 : 0;
        if (n > 0) Info("bonecos esquecidos (mundo trocado): " + std::to_string(n));
    } else {
        for (auto& [id, slot] : m_slots) Destroy(id, slot, " (fora do mundo)");
    }
    m_slots.clear();  // zera tentativas, recriações e desistências
    RefreshAlive();
}
}  // namespace cbmp::client
