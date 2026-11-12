#pragma once
#include <string>

namespace cbmp::client {
struct Vec3 { double x{}, y{}, z{}; };
struct Rot3 { double pitch{}, yaw{}, roll{}; };
struct Transform { Vec3 pos{}; Rot3 rot{}; };

// "pos=(1.50, -2.00, 300.25) rot=(p 0.00, y 90.00, r -5.50)"
std::string FormatTransform(const Transform& t);

// Desloca no plano XY na direção do yaw (graus). Rotação preservada.
Transform OffsetForward(const Transform& t, double distanceCm);
}
