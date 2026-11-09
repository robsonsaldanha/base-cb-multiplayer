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
}  // namespace cbmp
