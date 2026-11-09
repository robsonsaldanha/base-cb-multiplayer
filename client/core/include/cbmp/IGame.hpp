#pragma once
#include <cbmp/Transform.hpp>
#include <optional>
#include <stdexcept>
#include <string>

namespace cbmp {
struct PlayerHandle {
    void* ptr{};
    bool operator==(const PlayerHandle&) const = default;
};

// Lançada pela camada do jogo quando algo falha (inclusive SEH convertida).
struct GameError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Única porta de entrada para o jogo. Só client/src/game/ implementa.
// Todos os métodos devem ser chamados no game thread.
class IGame {
public:
    virtual ~IGame() = default;
    virtual std::optional<PlayerHandle> GetLocalPlayer() = 0;
    virtual std::optional<Transform> GetTransform(PlayerHandle p) = 0;
    virtual std::optional<PlayerHandle> SpawnClone(const Transform& at) = 0;
    virtual void Destroy(PlayerHandle p) = 0;
    virtual std::string Describe(PlayerHandle p) = 0;
    // Texto curto para o log quando o player não aparece (PlayerController, Pawn...).
    virtual std::string Diagnose() = 0;
};
}
