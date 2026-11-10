#include "cbserver/grid.hpp"

#include <algorithm>
#include <stdexcept>

namespace cbserver {

Grid::Grid(int32_t cellCm) : cell_(cellCm) {
  if (cellCm <= 0) throw std::invalid_argument("célula da grade deve ser positiva");
}

int64_t Grid::cellOf(int64_t v) const {
  int64_t q = v / cell_;
  if (v % cell_ != 0 && v < 0) --q;  // divisão com piso
  return q;
}

uint64_t Grid::key(int64_t cx, int64_t cy) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
}

void Grid::upsert(EntityId id, const cbmp::Vec3i& pos) {
  const uint64_t k = key(cellOf(pos.x), cellOf(pos.y));
  auto it = where_.find(id);
  if (it != where_.end()) {
    if (it->second == k) return;
    remove(id);
  }
  cells_[k].push_back(id);
  where_[id] = k;
}

void Grid::remove(EntityId id) {
  auto it = where_.find(id);
  if (it == where_.end()) return;
  auto c = cells_.find(it->second);
  if (c != cells_.end()) {
    auto& v = c->second;
    auto p = std::find(v.begin(), v.end(), id);
    if (p != v.end()) {
      *p = v.back();
      v.pop_back();
    }
    if (v.empty()) cells_.erase(c);
  }
  where_.erase(it);
}

void Grid::query(const cbmp::Vec3i& pos, int32_t radiusCm, std::vector<EntityId>& out) const {
  out.clear();
  const int64_t r = std::max<int64_t>(0, radiusCm);
  const int64_t x0 = cellOf(pos.x - r), x1 = cellOf(pos.x + r);
  const int64_t y0 = cellOf(pos.y - r), y1 = cellOf(pos.y + r);
  for (int64_t cx = x0; cx <= x1; ++cx)
    for (int64_t cy = y0; cy <= y1; ++cy) {
      auto it = cells_.find(key(cx, cy));
      if (it != cells_.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    }
}

}  // namespace cbserver
