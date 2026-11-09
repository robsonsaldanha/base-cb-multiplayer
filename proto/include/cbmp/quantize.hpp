#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cbmp {
inline uint16_t quantizeYaw(float deg) {
  double d = std::fmod(static_cast<double>(deg), 360.0);
  if (d < 0) d += 360.0;
  return static_cast<uint16_t>(static_cast<uint32_t>(std::lround(d / 360.0 * 65536.0)) & 0xFFFFu);
}
inline float dequantizeYaw(uint16_t q) { return static_cast<float>(q) * 360.0f / 65536.0f; }
inline uint8_t quantizePitch(float deg) {
  const double c = std::clamp(static_cast<double>(deg), -90.0, 90.0);
  return static_cast<uint8_t>(std::lround((c + 90.0) / 180.0 * 255.0));
}
inline float dequantizePitch(uint8_t q) { return static_cast<float>(q) * 180.0f / 255.0f - 90.0f; }

// Quantização compacta dos snapshots. Cada par é estável na própria grade: to(from(q)) == q.
// yaw u16 → 10 bits (360/1024 graus).
inline uint16_t yawTo10(uint16_t yaw) { return static_cast<uint16_t>(((static_cast<uint32_t>(yaw) + 32u) >> 6) & 1023u); }
inline uint16_t yawFrom10(uint16_t q) { return static_cast<uint16_t>((q & 1023u) << 6); }
// pitch u8 → 6 bits.
inline uint8_t pitchTo6(uint8_t p) { return static_cast<uint8_t>((p * 63u + 127u) / 255u); }
inline uint8_t pitchFrom6(uint8_t q) { return static_cast<uint8_t>((std::min<unsigned>(q, 63u) * 255u + 31u) / 63u); }
// velocidade cm/s → passos de 12,5 cm/s, -1024..1023 (11 bits com sinal, ±128 m/s), satura.
inline int16_t velToQ(int16_t v) {
  const int32_t n = 2 * static_cast<int32_t>(v);  // em passos de 1/25; 25 é ímpar → nunca empata
  const int32_t q = n >= 0 ? (n + 12) / 25 : -((-n + 12) / 25);
  return static_cast<int16_t>(std::clamp(q, -1024, 1023));
}
inline int16_t velFromQ(int16_t q) {
  const int32_t n = static_cast<int32_t>(q) * 25;
  return static_cast<int16_t>((n + (n >= 0 ? 1 : -1)) / 2);
}
}  // namespace cbmp
