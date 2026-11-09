#include "cbserver/movement.hpp"

#include <algorithm>
#include <cstddef>

namespace cbserver {

namespace {
double allowedCm(double speed, double dtSec, const MovementLimits& l) {
  return speed * (1.0 + l.tolerance) * std::max(0.0, dtSec) + l.slackCm;
}
}  // namespace

MoveVerdict validateMove(const cbmp::PlayerState& last, const cbmp::PlayerState& next, double dtSec,
                         const MovementLimits& limits) {
  const auto a = static_cast<size_t>(last.state), b = static_cast<size_t>(next.state);
  if (a >= limits.maxSpeedCmS.size() || b >= limits.maxSpeedCmS.size()) return MoveVerdict::Corrected;
  // double: diferenças de int32 cabem com folga, sem overflow.
  const double dx = static_cast<double>(next.pos.x) - last.pos.x;
  const double dy = static_cast<double>(next.pos.y) - last.pos.y;
  const double dz = static_cast<double>(next.pos.z) - last.pos.z;

  constexpr auto kFall = static_cast<size_t>(cbmp::MoveState::Fall);
  if (a != kFall && b != kFall) {
    const double allowed = allowedCm(std::max(limits.maxSpeedCmS[a], limits.maxSpeedCmS[b]), dtSec, limits);
    return dx * dx + dy * dy + dz * dz > allowed * allowed ? MoveVerdict::Corrected : MoveVerdict::Ok;
  }
  // Queda: o limite alto vale só para descer. Horizontal (e subida) pelo maior estado que não é queda;
  // se os dois são queda, pelo limite de corrida.
  double other = 0;
  bool any = false;
  for (size_t s : {a, b})
    if (s != kFall) {
      other = std::max(other, limits.maxSpeedCmS[s]);
      any = true;
    }
  if (!any) other = limits.maxSpeedCmS[static_cast<size_t>(cbmp::MoveState::Run)];
  const double h = allowedCm(other, dtSec, limits);
  if (dx * dx + dy * dy > h * h) return MoveVerdict::Corrected;
  const double v = dz < 0 ? allowedCm(std::max(other, limits.maxSpeedCmS[kFall]), dtSec, limits) : h;
  return dz * dz > v * v ? MoveVerdict::Corrected : MoveVerdict::Ok;
}

}  // namespace cbserver
