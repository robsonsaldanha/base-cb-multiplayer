#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace cbserver {

class Metrics {
 public:
  static constexpr size_t kWindow = 1200;  // 60 s a 20 Hz

  void recordTick(std::chrono::microseconds d);
  void addBytesOut(uint16_t playerId, size_t bytes);
  nlohmann::json snapshot() const;

  std::atomic<int64_t> players{0}, pending{0}, corrections{0}, kicks{0}, droppedEvents{0}, decodeErrors{0}, sendFailures{0};

 private:
  mutable std::mutex mu_;
  std::vector<int64_t> ticks_;  // anel, em microssegundos
  size_t head_ = 0;
  std::unordered_map<uint16_t, uint64_t> bytes_;
  std::chrono::steady_clock::time_point bytesStart_ = std::chrono::steady_clock::now();
  mutable double lastCpuSec_ = -1;
  mutable std::chrono::steady_clock::time_point lastCpuAt_{};
};

}  // namespace cbserver
