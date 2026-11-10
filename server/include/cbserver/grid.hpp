#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <cbmp/messages.hpp>

namespace cbserver {

using EntityId = uint16_t;

// Grade espacial uniforme em XY (células quadradas de cellCm). Z é ignorado.
class Grid {
 public:
  explicit Grid(int32_t cellCm = 6400);
  void upsert(EntityId id, const cbmp::Vec3i& pos);  // move entre células se mudou
  void remove(EntityId id);                           // idempotente
  // Limpa `out` e devolve os ids nas células que intersectam o quadrado [pos-r, pos+r] (só XY).
  // Pode incluir entidades fora do raio; o chamador filtra pela distância.
  void query(const cbmp::Vec3i& pos, int32_t radiusCm, std::vector<EntityId>& out) const;
  size_t size() const { return where_.size(); }

 private:
  int64_t cellOf(int64_t v) const;
  static uint64_t key(int64_t cx, int64_t cy);

  int32_t cell_;
  std::unordered_map<uint64_t, std::vector<EntityId>> cells_;
  std::unordered_map<EntityId, uint64_t> where_;
};

}  // namespace cbserver
