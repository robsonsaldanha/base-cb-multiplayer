#include "cbserver/movement.hpp"

#include <algorithm>
#include <cstddef>

namespace cbserver {

MoveVerdict validateMove(const cbmp::PlayerState& last, const cbmp::PlayerState& next, double dtSec,
                         const MovementLimits& limits) {
  const auto a = static_cast<size_t>(last.state), b = static_cast<size_t>(next.state);
  if (a >= limits.maxSpeedCmS.size() || b >= limits.maxSpeedCmS.size()) return MoveVerdict::Corrected;
  const double speed = std::max(limits.maxSpeedCmS[a], limits.maxSpeedCmS[b]);
  const double allowed = speed * (1.0 + limits.tolerance) * std::max(0.0, dtSec) + limits.slackCm;
  // double: diferenças de int32 cabem com folga, sem overflow.
  const double dx = static_cast<double>(next.pos.x) - last.pos.x;
  const double dy = static_cast<double>(next.pos.y) - last.pos.y;
  const double dz = static_cast<double>(next.pos.z) - last.pos.z;
  return dx * dx + dy * dy + dz * dz > allowed * allowed ? MoveVerdict::Corrected : MoveVerdict::Ok;
}

}  // namespace cbserver
