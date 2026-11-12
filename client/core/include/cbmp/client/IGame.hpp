#pragma once
#include <cbmp/client/Transform.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// Única porta de entrada para o jogo; só client/src/game/ implementa. Todos os métodos rodam no game thread.
// Falha interna vira GameError; o núcleo nunca vê ponteiros do motor. worldId/pawnId são identificadores opacos
// (índice + número de série do objeto). PuppetPose.moveState é o valor numérico de cbmp::MoveState.
namespace cbmp::client {
enum class MoveMode : uint8_t { Ground, Falling, Swimming, Flying, Other };
struct LocalState { Vec3 pos; double yawDeg, pitchDeg; Vec3 vel; MoveMode mode; };   // cm, graus, cm/s
struct WorldSample { uint64_t worldId; std::string worldName; uint64_t pawnId; bool hasPawn; bool frontendHint; };
struct PuppetPose  { Vec3 pos; double yawDeg; Vec3 vel; uint8_t moveState; };
struct PuppetId    { uint32_t v{}; bool operator==(const PuppetId&) const = default; };
enum class PuppetResult { Ok, Gone, Failed };

// Lançada pela camada do jogo quando algo falha (inclusive SEH convertida).
struct GameError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class IGame {
public:
    virtual ~IGame() = default;
    virtual WorldSample SampleWorld() = 0;                       // barato; chamado a cada tick
    virtual std::optional<LocalState> ReadLocal() = 0;           // nullopt = sem pawn utilizável agora
    virtual bool TeleportLocal(const Vec3& pos) = 0;             // só correções curtas (≤ 20 m)
    virtual std::optional<PuppetId> SpawnPuppet(const PuppetPose& at) = 0;
    virtual PuppetResult MovePuppet(PuppetId id, const PuppetPose& to) = 0;   // Gone = o jogo destruiu o ator
    virtual void DestroyPuppet(PuppetId id) = 0;
    virtual void ForgetAllPuppets() = 0;                         // mundo trocou: os atores já morreram
    virtual bool PlayerSingletonIntact() = 0;                    // o jogo ainda aponta para o pawn local?
    virtual bool ShowStatus(std::string_view utf8) = 0;          // false = indisponível (segue só no log)
    virtual std::string Diagnose() = 0;
};
}
