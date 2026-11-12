#include <cbmp/client/LocalSampler.hpp>

#include <algorithm>
#include <cmath>

#include <cbmp/client/Limits.hpp>
#include <cbmp/quantize.hpp>

namespace cbmp::client {
namespace {
bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool InRange(const Vec3& v) {
    return std::abs(v.x) <= limits::kMaxCoordCm && std::abs(v.y) <= limits::kMaxCoordCm &&
           std::abs(v.z) <= limits::kMaxCoordCm;
}

int16_t ClampVel(double v) {
    const double c = std::clamp(v, -static_cast<double>(limits::kMaxVelCmS), static_cast<double>(limits::kMaxVelCmS));
    return static_cast<int16_t>(std::lround(c));
}
}  // namespace

cbmp::MoveState ClassifyMove(MoveMode mode, double speedCmS) {
    switch (mode) {
        case MoveMode::Falling: return cbmp::MoveState::Fall;
        case MoveMode::Swimming: return cbmp::MoveState::Swim;
        case MoveMode::Flying: return cbmp::MoveState::Broom;
        case MoveMode::Ground:
        case MoveMode::Other: break;
    }
    if (!(speedCmS >= limits::kIdleMaxCmS)) return cbmp::MoveState::Idle;  // NaN também
    if (speedCmS <= limits::kWalkMaxCmS) return cbmp::MoveState::Walk;
    if (speedCmS <= limits::kRunMaxCmS) return cbmp::MoveState::Run;
    return cbmp::MoveState::Broom;
}

void LocalSampler::MarkJump() { m_jumpPending = true; }

std::optional<cbmp::PlayerState> LocalSampler::Sample(const LocalState& s) {
    if (!Finite(s.pos) || !std::isfinite(s.yawDeg) || !Finite(s.vel) || !InRange(s.pos)) return std::nullopt;

    cbmp::PlayerState out{};
    out.pos = {static_cast<int32_t>(std::lround(s.pos.x)), static_cast<int32_t>(std::lround(s.pos.y)),
               static_cast<int32_t>(std::lround(s.pos.z))};
    // fmod antes da conversão para float: yaw finito mas enorme não estoura o float.
    out.yaw = cbmp::quantizeYaw(static_cast<float>(std::fmod(s.yawDeg, 360.0)));
    out.pitch = cbmp::quantizePitch(0.0f);
    out.vel = {ClampVel(s.vel.x), ClampVel(s.vel.y), ClampVel(s.vel.z)};
    out.state = ClassifyMove(s.mode, std::sqrt(s.vel.x * s.vel.x + s.vel.y * s.vel.y + s.vel.z * s.vel.z));

    bool jump = m_jumpPending;
    if (m_hasLast) {
        const double dx = static_cast<double>(out.pos.x) - m_last.x;
        const double dy = static_cast<double>(out.pos.y) - m_last.y;
        const double dz = static_cast<double>(out.pos.z) - m_last.z;
        jump = jump || std::sqrt(dx * dx + dy * dy + dz * dz) > limits::kJumpDistanceCm;
    }
    if (jump) out.state = cbmp::MoveState::Apparate;
    m_jumpPending = false;
    m_lastWasJump = jump;
    m_hasLast = true;
    m_last = out.pos;
    return out;
}

bool LocalSampler::LastWasJump() const { return m_lastWasJump; }
}  // namespace cbmp::client
