#include "cbserver/visibility.hpp"

#include <algorithm>
#include <utility>

namespace cbserver {

namespace {
// 3 * (1.7e9)^2 ≈ 8.7e18 < INT64_MAX: soma dos quadrados nunca transborda.
constexpr int64_t kAxisClamp = 1'700'000'000;
int64_t axis(int32_t a, int32_t b) {
  const int64_t d = std::clamp<int64_t>(static_cast<int64_t>(a) - b, -kAxisClamp, kAxisClamp);
  return d * d;
}
constexpr int64_t sq(int64_t v) { return v * v; }
}  // namespace

int64_t distSq(const cbmp::Vec3i& a, const cbmp::Vec3i& b) { return axis(a.x, b.x) + axis(a.y, b.y) + axis(a.z, b.z); }

Tier tierFor(int64_t d2) {
  if (d2 < sq(10'000)) return Tier::Near;
  if (d2 < sq(20'000)) return Tier::Mid;
  return Tier::Far;
}

VisibleSet computeVisible(EntityId viewer, const cbmp::Vec3i& viewerPos, const VisibleSet& previous,
                          const Grid& grid, const std::function<const cbmp::Vec3i&(EntityId)>& posOf,
                          const VisibilityParams& p, std::vector<EntityId>& scratch) {
  // Buffer reaproveitado por thread: sem alocação por entidade em regime.
  thread_local std::vector<std::pair<int64_t, EntityId>> cand;
  cand.clear();
  const int64_t enter2 = sq(p.enterCm), exit2 = sq(std::max(p.enterCm, p.exitCm));
  grid.query(viewerPos, std::max(p.enterCm, p.exitCm), scratch);
  for (EntityId id : scratch) {
    if (id == viewer) continue;
    const int64_t d2 = distSq(viewerPos, posOf(id));
    if (d2 < enter2 || (d2 <= exit2 && std::binary_search(previous.ids.begin(), previous.ids.end(), id)))
      cand.emplace_back(d2, id);
  }
  if (cand.size() > p.maxVisible) {
    std::nth_element(cand.begin(), cand.begin() + static_cast<std::ptrdiff_t>(p.maxVisible), cand.end());
    cand.resize(p.maxVisible);
  }
  VisibleSet out;
  out.ids.reserve(cand.size());
  for (const auto& c : cand) out.ids.push_back(c.second);
  std::sort(out.ids.begin(), out.ids.end());
  return out;
}

}  // namespace cbserver
