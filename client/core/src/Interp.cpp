#include <cbmp/client/Interp.hpp>

#include <algorithm>
#include <cmath>

#include <cbmp/quantize.hpp>

namespace cbmp::client {
namespace {
// Teto de segurança da janela do relógio (um servidor não pode fazê-la crescer sem limite mandando muitos ticks no
// mesmo instante). Em uso normal ela tem no máximo kClockWindowSec × 60 Hz = 600 entradas.
constexpr size_t kMaxClockWindow = 4096;
constexpr size_t kDelayIntervals = 4;     // últimos intervalos válidos usados no atraso
constexpr double kMaxIntervalSec = 1.0;   // intervalo válido: < 1 s

Vec3 PosOf(const cbmp::PlayerState& s) {
    return {static_cast<double>(s.pos.x), static_cast<double>(s.pos.y), static_cast<double>(s.pos.z)};
}
Vec3 VelOf(const cbmp::PlayerState& s) {
    return {static_cast<double>(s.vel.x), static_cast<double>(s.vel.y), static_cast<double>(s.vel.z)};
}
double Dist(const Vec3& a, const Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
double NormYaw(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0) d += 360.0;
    if (d >= 360.0) d -= 360.0;
    return d;
}
InterpPose PoseOf(const cbmp::PlayerState& s) {
    InterpPose p;
    p.pos = PosOf(s);
    p.yawDeg = NormYaw(cbmp::dequantizeYaw(s.yaw));
    p.vel = VelOf(s);
    p.moveState = static_cast<uint8_t>(s.state);
    return p;
}
}  // namespace

void Interp::SetTickRate(int hz) {
    if (hz <= 0) return;
    m_hz = hz;
}

void Interp::OnTick(uint32_t tick, double arrival) {
    if (!std::isfinite(arrival)) return;
    const double value = arrival - tick / m_hz;
    // Mínimo deslizante: só ficam candidatos com valor estritamente crescente.
    while (!m_window.empty() && m_window.back().value >= value) m_window.pop_back();
    m_window.push_back({arrival, value});
    while (m_window.front().arrival < arrival - limits::kClockWindowSec) m_window.pop_front();
    while (m_window.size() > kMaxClockWindow) m_window.pop_front();

    const double minimum = m_window.front().value;
    if (m_hasOffset && std::abs(minimum - m_offset) > limits::kClockResetSec) {
        // Relógio do servidor mudou (reinício, salto): as amostras guardadas não valem mais.
        Clear();
        m_window.push_back({arrival, value});
    }
    m_offset = m_window.front().value;
    m_hasOffset = true;
}

void Interp::Push(uint16_t id, uint32_t tick, const cbmp::PlayerState& s) {
    if (!m_hasOffset) return;
    Add(id, tick / m_hz + m_offset, s);
}

void Interp::PushNow(uint16_t id, const cbmp::PlayerState& s, double arrival) {
    if (!std::isfinite(arrival)) return;
    Add(id, arrival, s);
}

void Interp::Add(uint16_t id, double t, const cbmp::PlayerState& s) {
    // Teto de faixas (defesa em profundidade: o chamador só empurra ids conhecidos, ≤ 255).
    if (!m_tracks.contains(id) && m_tracks.size() >= limits::kMaxEntities) return;
    Track& tr = m_tracks[id];
    if (!tr.q.empty() && t <= tr.q.back().t) return;
    tr.q.push_back({t, s});
    while (tr.q.size() > limits::kInterpQueue) tr.q.pop_front();
}

void Interp::Remove(uint16_t id) { m_tracks.erase(id); }

void Interp::Clear() {
    m_tracks.clear();
    m_window.clear();
    m_hasOffset = false;
    m_offset = 0;
}

double Interp::DelayOf(uint16_t id) const {
    const auto it = m_tracks.find(id);
    if (it == m_tracks.end()) return limits::kInterpDelaySec;
    const auto& q = it->second.q;
    double sum = 0;
    size_t n = 0;
    for (size_t i = q.size(); i >= 2 && n < kDelayIntervals; --i) {
        const double dt = q[i - 1].t - q[i - 2].t;
        if (dt < kMaxIntervalSec) {
            sum += dt;
            ++n;
        }
    }
    if (n == 0) return limits::kInterpDelaySec;
    return std::clamp(2.0 * sum / static_cast<double>(n), limits::kInterpDelaySec, limits::kMaxInterpDelaySec);
}

std::optional<InterpPose> Interp::Sample(uint16_t id, double now) {
    if (!std::isfinite(now)) return std::nullopt;
    const auto it = m_tracks.find(id);
    if (it == m_tracks.end() || it->second.q.empty()) return std::nullopt;
    Track& tr = it->second;
    const auto& q = tr.q;
    const double r = now - DelayOf(id);

    InterpPose pose;
    if (r < q.front().t) {
        pose = PoseOf(q.front().s);
    } else if (r >= q.back().t) {
        const Point& last = q.back();
        pose = PoseOf(last.s);
        const double dt = std::min(r - last.t, limits::kMaxExtrapolationSec);
        pose.pos = {pose.pos.x + pose.vel.x * dt, pose.pos.y + pose.vel.y * dt, pose.pos.z + pose.vel.z * dt};
    } else {
        // q.front().t ≤ r < q.back().t: B = primeira amostra com t > r.
        const auto bIt = std::upper_bound(q.begin(), q.end(), r, [](double v, const Point& p) { return v < p.t; });
        const Point& b = *bIt;
        const Point& a = *(bIt - 1);
        const Vec3 pa = PosOf(a.s), pb = PosOf(b.s);
        if (b.s.state == cbmp::MoveState::Apparate || Dist(pa, pb) > limits::kSnapDistanceCm) {
            pose = PoseOf(a.s);  // salto: segura A até r alcançar B, sem deslizar
        } else {
            const double k = (r - a.t) / (b.t - a.t);
            pose = PoseOf(b.s);
            pose.pos = {pa.x + (pb.x - pa.x) * k, pa.y + (pb.y - pa.y) * k, pa.z + (pb.z - pa.z) * k};
            const double ya = cbmp::dequantizeYaw(a.s.yaw), yb = cbmp::dequantizeYaw(b.s.yaw);
            double d = std::fmod(yb - ya, 360.0);
            if (d > 180.0) d -= 360.0;
            if (d < -180.0) d += 360.0;
            pose.yawDeg = NormYaw(ya + d * k);
        }
    }

    pose.snapped = !tr.hasLast || Dist(pose.pos, tr.last) > limits::kSnapDistanceCm;
    tr.hasLast = true;
    tr.last = pose.pos;
    return pose;
}
}  // namespace cbmp::client
