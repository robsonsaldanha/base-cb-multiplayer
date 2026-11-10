#pragma once
// Estatística de intervalo entre snapshots (histograma de 1 ms até 10 s).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cbbots {

class GapStats {
 public:
  static constexpr size_t kBuckets = 10001;

  void add(double gapSec) {
    gapSec = std::max(0.0, gapSec);
    max_ = std::max(max_, gapSec);
    const auto ms = static_cast<size_t>(std::llround(gapSec * 1000.0));
    ++hist_[std::min(ms, kBuckets - 1)];
    ++count_;
  }
  double maxSec() const { return max_; }
  uint64_t count() const { return count_; }
  double p99Ms() const {
    if (count_ == 0) return 0;
    const auto target = static_cast<uint64_t>(std::ceil(0.99 * static_cast<double>(count_)));
    uint64_t acc = 0;
    for (size_t ms = 0; ms < hist_.size(); ++ms) {
      acc += hist_[ms];
      if (acc >= target) return static_cast<double>(ms);
    }
    return static_cast<double>(kBuckets - 1);
  }

  // Intervalo final de um bot: do último snapshot (ou do Welcome, se nunca recebeu nenhum) até o fim da sessão.
  // Sem isso, um bot que para de receber snapshots (ou nunca recebe) não aparece no máximo.
  static double tailGap(double welcomeAt, double lastSnapAt, double endAt) {
    return std::max(0.0, endAt - (lastSnapAt >= 0 ? lastSnapAt : welcomeAt));
  }

 private:
  std::vector<uint64_t> hist_ = std::vector<uint64_t>(kBuckets, 0);
  double max_ = 0;
  uint64_t count_ = 0;
};

}  // namespace cbbots
