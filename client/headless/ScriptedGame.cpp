#include "ScriptedGame.hpp"

#include <algorithm>

namespace cbmp::headless {
using namespace cbmp::client;

ScriptedGame::ScriptedGame(ScriptOptions opt) : m_opt(opt), m_pos(opt.start) {}

void ScriptedGame::Advance(double elapsedSec, double unixSec) {
    const double dt = std::max(0.0, elapsedSec - m_lastElapsed);
    m_lastElapsed = elapsedSec;
    m_unix = unixSec;
    m_vel = {};
    if (elapsedSec >= m_opt.walkAfterSec && m_walked < m_opt.walkCm && m_opt.speedCmS > 0) {
        // Só o tempo depois do início da caminhada conta (o primeiro passo não ganha o tempo parado).
        const double walking = std::min(dt, elapsedSec - m_opt.walkAfterSec);
        const double step = std::min(m_opt.speedCmS * walking, m_opt.walkCm - m_walked);
        m_pos.x += step;
        m_walked += step;
        if (m_walked < m_opt.walkCm) m_vel = {m_opt.speedCmS, 0, 0};
    }
    m_local.push_back({unixSec, m_pos});
}

WorldSample ScriptedGame::SampleWorld() { return WorldSample{1, "Overland", 1, true, false}; }

std::optional<LocalState> ScriptedGame::ReadLocal() { return LocalState{m_pos, 0.0, 0.0, m_vel, MoveMode::Ground}; }

bool ScriptedGame::TeleportLocal(const Vec3& pos) {
    m_pos = pos;
    return true;
}

std::optional<PuppetId> ScriptedGame::SpawnPuppet(const PuppetPose& at) {
    PuppetRecord r;
    r.id = m_nextPuppet++;
    r.createdAt = m_unix;
    r.path.push_back({m_unix, at.pos});
    const uint32_t id = r.id;
    m_puppets.emplace(id, std::move(r));
    return PuppetId{id};
}

PuppetResult ScriptedGame::MovePuppet(PuppetId id, const PuppetPose& to) {
    const auto it = m_puppets.find(id.v);
    if (it == m_puppets.end() || it->second.destroyedAt) return PuppetResult::Gone;
    it->second.path.push_back({m_unix, to.pos});
    return PuppetResult::Ok;
}

void ScriptedGame::DestroyPuppet(PuppetId id) {
    const auto it = m_puppets.find(id.v);
    if (it != m_puppets.end() && !it->second.destroyedAt) it->second.destroyedAt = m_unix;
}

void ScriptedGame::ForgetAllPuppets() {
    for (auto& [id, r] : m_puppets)
        if (!r.destroyedAt) r.destroyedAt = m_unix;
}

bool ScriptedGame::PlayerSingletonIntact() { return true; }

bool ScriptedGame::ShowStatus(std::string_view utf8) {
    m_statuses.emplace_back(utf8);
    return true;
}

std::string ScriptedGame::Diagnose() { return "cbmp-headless ScriptedGame"; }
}  // namespace cbmp::headless
