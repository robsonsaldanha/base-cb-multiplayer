#include "cbserver/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// psapi.h depois de windows.h
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace cbserver {
namespace {

double percentile(const std::vector<int64_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  double pos = p * static_cast<double>(sorted.size() - 1);
  auto lo = static_cast<size_t>(std::floor(pos));
  auto hi = std::min(lo + 1, sorted.size() - 1);
  double frac = pos - static_cast<double>(lo);
  return static_cast<double>(sorted[lo]) * (1 - frac) + static_cast<double>(sorted[hi]) * frac;
}

double rssBytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return static_cast<double>(pmc.WorkingSetSize);
  return 0;
#else
  std::ifstream f("/proc/self/statm");
  long size = 0, resident = 0;
  if (f >> size >> resident) return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE));
  return 0;
#endif
}

double processCpuSeconds() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  auto to = [](const FILETIME& ft) {
    return static_cast<double>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) * 1e-7;
  };
  return to(k) + to(u);
#else
  timespec ts{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

}  // namespace

void Metrics::recordTick(std::chrono::microseconds d) {
  std::lock_guard lk(mu_);
  if (ticks_.size() < kWindow) {
    ticks_.push_back(d.count());
  } else {
    ticks_[head_] = d.count();
    head_ = (head_ + 1) % kWindow;
  }
}

void Metrics::addBytesOut(uint16_t playerId, size_t bytes) {
  std::lock_guard lk(mu_);
  auto now = std::chrono::steady_clock::now();
  if (now - bytesStart_ > std::chrono::seconds(60)) {
    bytes_.clear();
    bytesStart_ = now;
  }
  bytes_[playerId] += bytes;
}

nlohmann::json Metrics::snapshot() const {
  std::vector<int64_t> sorted;
  double avgKbps = 0, maxKbps = 0, cpu = 0;
  {
    std::lock_guard lk(mu_);
    sorted = ticks_;
    auto now = std::chrono::steady_clock::now();
    double secs = std::max(1.0, std::chrono::duration<double>(now - bytesStart_).count());
    double sum = 0;
    for (auto& [id, b] : bytes_) {
      (void)id;
      double kbps = static_cast<double>(b) * 8.0 / 1000.0 / secs;
      sum += kbps;
      maxKbps = std::max(maxKbps, kbps);
    }
    if (!bytes_.empty()) avgKbps = sum / static_cast<double>(bytes_.size());

    double cpuNow = processCpuSeconds();
    if (lastCpuSec_ >= 0) {
      double wall = std::chrono::duration<double>(now - lastCpuAt_).count();
      if (wall > 0) cpu = (cpuNow - lastCpuSec_) / wall * 100.0;
    }
    lastCpuSec_ = cpuNow;
    lastCpuAt_ = now;
  }
  std::sort(sorted.begin(), sorted.end());
  double maxUs = sorted.empty() ? 0 : static_cast<double>(sorted.back());
  return {{"tickP50Ms", percentile(sorted, 0.50) / 1000.0},
          {"tickP99Ms", percentile(sorted, 0.99) / 1000.0},
          {"tickMaxMs", maxUs / 1000.0},
          {"players", players.load()},
          {"pending", pending.load()},
          {"corrections", corrections.load()},
          {"kicks", kicks.load()},
          {"droppedEvents", droppedEvents.load()},
          {"avgKbpsPerPlayer", avgKbps},
          {"maxKbpsPerPlayer", maxKbps},
          {"rssBytes", static_cast<uint64_t>(rssBytes())},
          {"cpuPercent", cpu}};
}

}  // namespace cbserver
