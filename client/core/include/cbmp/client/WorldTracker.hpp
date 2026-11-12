#pragma once
// Decide se o jogador está no menu (Frontend), carregando (Loading) ou dentro do mundo (InWorld) a partir de
// IGame::SampleWorld/ReadLocal. InWorld exige, ao mesmo tempo: pawn válido, LocalState presente, mundo fora da lista
// de menus, frontendHint falso, mesmos worldId/pawnId há ≥ 2 s e posição a mais de 100 cm da origem em XY (durante o
// carregamento o pawn fica em (0,0,z) caindo).
#include <optional>
#include <string>
#include <string_view>

#include <cbmp/client/IGame.hpp>

namespace cbmp::client {
enum class WorldPhase { Frontend, Loading, InWorld };

struct WorldChange {
    bool worldChanged = false, pawnChanged = false;
};

bool IsMenuWorld(std::string_view worldName);  // "RootLevel"

class WorldTracker {
public:
    WorldPhase Update(const WorldSample& w, const std::optional<LocalState>& local, double now);
    WorldPhase Phase() const;
    WorldChange TakeChange();  // acumulado desde a última chamada; zera
    const std::string& WorldName() const;

private:
    bool m_started = false;
    uint64_t m_worldId = 0, m_pawnId = 0;
    double m_stableSince = 0;
    WorldPhase m_phase = WorldPhase::Loading;
    WorldChange m_change;
    std::string m_worldName;
};
}  // namespace cbmp::client
