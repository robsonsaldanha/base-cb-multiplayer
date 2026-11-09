#pragma once
#include <array>

#include <cbmp/messages.hpp>

namespace cbserver {

// Velocidade máxima (cm/s) indexada por cbmp::MoveState (0 parado .. 6 aparatando).
struct MovementLimits {
  std::array<double, 7> maxSpeedCmS{};
  double tolerance = 0.2;
  double slackCm = 50;
};
enum class MoveVerdict { Ok, Corrected };

// Distância 3D desde a última posição aceita > maxSpeed(maior entre o estado anterior e o novo)
// * (1+tol) * dt + slack → Corrected. maxSpeed 0 (aparatar) → só slack. Estado fora da tabela → Corrected.
// Se algum dos estados é queda: horizontal e subida limitados pelo maior estado que não é queda (corrida se
// ambos são queda); só a descida usa o limite de queda.
MoveVerdict validateMove(const cbmp::PlayerState& last, const cbmp::PlayerState& next, double dtSec,
                         const MovementLimits& limits);

}  // namespace cbserver
