#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <cbmp/messages.hpp>

#include "cbserver/grid.hpp"

namespace cbserver {

struct VisibilityParams {
  int32_t enterCm = 30000;
  int32_t exitCm = 33000;
  size_t maxVisible = 128;
};

enum class Tier : uint8_t { Near = 1, Mid = 2, Far = 4 };  // envia a cada 1, 2 ou 4 ticks (20/10/5 Hz)

// Distância 3D ao quadrado em cm², em int64 (cada eixo saturado para nunca transbordar).
int64_t distSq(const cbmp::Vec3i& a, const cbmp::Vec3i& b);
Tier tierFor(int64_t distSqCm);  // < 100 m Near, < 200 m Mid, senão Far

struct VisibleSet {
  std::vector<EntityId> ids;  // ordenado por id
};

// Novo conjunto visível: candidatos da grade; entra quem está < enter; quem já estava visível fica até
// > exit; limita a maxVisible pelos mais próximos (desempate por id); nunca inclui o próprio viewer.
VisibleSet computeVisible(EntityId viewer, const cbmp::Vec3i& viewerPos, const VisibleSet& previous,
                          const Grid& grid, const std::function<const cbmp::Vec3i&(EntityId)>& posOf,
                          const VisibilityParams& p, std::vector<EntityId>& scratch);

}  // namespace cbserver
