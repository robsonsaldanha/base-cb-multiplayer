#include <cbmp/client/WorldTracker.hpp>

#include <cmath>

#include <cbmp/client/Limits.hpp>

namespace cbmp::client {
bool IsMenuWorld(std::string_view worldName) { return worldName == "RootLevel"; }

WorldPhase WorldTracker::Update(const WorldSample& w, const std::optional<LocalState>& local, double now) {
    if (!m_started) {
        m_started = true;
        m_worldId = w.worldId;
        m_pawnId = w.pawnId;
        m_stableSince = now;
    } else if (w.worldId != m_worldId) {
        m_change.worldChanged = true;
        m_worldId = w.worldId;
        m_pawnId = w.pawnId;
        m_stableSince = now;
    } else if (w.pawnId != m_pawnId) {
        m_change.pawnChanged = true;
        m_pawnId = w.pawnId;
        m_stableSince = now;
    }
    m_worldName = w.worldName;

    const bool menu = IsMenuWorld(w.worldName);
    const bool stable = now - m_stableSince >= limits::kInWorldStableSec;
    const bool awayFromOrigin = local && std::hypot(local->pos.x, local->pos.y) > limits::kOriginRadiusCm;
    if (w.hasPawn && local && !menu && !w.frontendHint && stable && awayFromOrigin) {
        m_phase = WorldPhase::InWorld;
    } else if (menu || w.frontendHint) {
        m_phase = WorldPhase::Frontend;
    } else {
        m_phase = WorldPhase::Loading;
    }
    return m_phase;
}

WorldPhase WorldTracker::Phase() const { return m_phase; }

WorldChange WorldTracker::TakeChange() {
    const WorldChange c = m_change;
    m_change = {};
    return c;
}

const std::string& WorldTracker::WorldName() const { return m_worldName; }
}  // namespace cbmp::client
