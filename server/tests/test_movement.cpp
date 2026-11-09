#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "cbserver/movement.hpp"

using cbmp::MoveState;
using cbmp::PlayerState;
using cbserver::MovementLimits;
using cbserver::MoveVerdict;
using cbserver::validateMove;

namespace {
MovementLimits limits() {
  MovementLimits l;
  l.maxSpeedCmS = {50, 400, 900, 4000, 400, 6000, 0};
  l.tolerance = 0.2;
  return l;
}
PlayerState at(int32_t x, MoveState s) {
  PlayerState p;
  p.pos = {x, 0, 0};
  p.state = s;
  return p;
}
}  // namespace

TEST_CASE("movement walking at 400 cm per second for 50 ms is ok") {
  REQUIRE(validateMove(at(0, MoveState::Walk), at(20, MoveState::Walk), 0.05, limits()) == MoveVerdict::Ok);
  REQUIRE(validateMove(at(0, MoveState::Walk), at(200, MoveState::Walk), 0.5, limits()) == MoveVerdict::Ok);
}

TEST_CASE("movement at three times walking speed is corrected") {
  REQUIRE(validateMove(at(0, MoveState::Walk), at(600, MoveState::Walk), 0.5, limits()) == MoveVerdict::Corrected);
}

TEST_CASE("movement on broom uses the larger limit of either state") {
  REQUIRE(validateMove(at(0, MoveState::Broom), at(1500, MoveState::Broom), 0.5, limits()) == MoveVerdict::Ok);
  REQUIRE(validateMove(at(0, MoveState::Walk), at(1500, MoveState::Broom), 0.5, limits()) == MoveVerdict::Ok);
  REQUIRE(validateMove(at(0, MoveState::Broom), at(1500, MoveState::Walk), 0.5, limits()) == MoveVerdict::Ok);
  REQUIRE(validateMove(at(0, MoveState::Walk), at(1500, MoveState::Walk), 0.5, limits()) == MoveVerdict::Corrected);
}

TEST_CASE("movement apparate allows only slack") {
  REQUIRE(validateMove(at(0, MoveState::Apparate), at(40, MoveState::Apparate), 1.0, limits()) == MoveVerdict::Ok);
  REQUIRE(validateMove(at(0, MoveState::Apparate), at(60, MoveState::Apparate), 1.0, limits()) == MoveVerdict::Corrected);
}

TEST_CASE("movement uses 3D distance and huge jumps do not overflow") {
  PlayerState a = at(0, MoveState::Fall);
  PlayerState b = a;
  b.pos.z = -2000;  // 6000 cm/s * 1.2 * 0.5 + 50 = 3650
  REQUIRE(validateMove(a, b, 0.5, limits()) == MoveVerdict::Ok);
  PlayerState far = at(INT32_MAX, MoveState::Walk);
  far.pos.y = INT32_MIN;
  REQUIRE(validateMove(at(INT32_MIN, MoveState::Walk), far, 0.05, limits()) == MoveVerdict::Corrected);
}

TEST_CASE("movement fall limit applies only to downward displacement") {
  PlayerState a = at(0, MoveState::Fall);
  PlayerState b = a;
  b.pos.z = -3000;  // queda: 6000 * 1.2 * 0.5 + 50 = 3650
  REQUIRE(validateMove(a, b, 0.5, limits()) == MoveVerdict::Ok);
  b.pos.z = 3000;  // "cair" para cima
  REQUIRE(validateMove(a, b, 0.5, limits()) == MoveVerdict::Corrected);
  // horizontal declarando queda: limitado pela corrida (900 * 1.2 * 0.5 + 50 = 590)
  b = a;
  b.pos.x = 3000;
  REQUIRE(validateMove(a, b, 0.5, limits()) == MoveVerdict::Corrected);
  b.pos.x = 500;
  REQUIRE(validateMove(a, b, 0.5, limits()) == MoveVerdict::Ok);
  // parado → "caindo" não vira 6000 cm/s em 3D
  PlayerState idle = at(0, MoveState::Idle);
  PlayerState fall = at(2500, MoveState::Fall);
  fall.pos.y = 2500;
  fall.pos.z = -100;
  REQUIRE(validateMove(idle, fall, 0.5, limits()) == MoveVerdict::Corrected);
  // andando → caindo: horizontal pelo maior não-queda (andar) — 400 * 1.2 * 0.5 + 50 = 290
  PlayerState walk = at(0, MoveState::Walk);
  PlayerState fall2 = at(280, MoveState::Fall);
  fall2.pos.z = -2000;
  REQUIRE(validateMove(walk, fall2, 0.5, limits()) == MoveVerdict::Ok);
  fall2.pos.x = 400;
  REQUIRE(validateMove(walk, fall2, 0.5, limits()) == MoveVerdict::Corrected);
}
