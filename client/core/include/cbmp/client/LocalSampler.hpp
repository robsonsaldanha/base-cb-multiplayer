#pragma once
// LocalState do jogo → cbmp::PlayerState do Input: quantização, amostras inválidas puladas, estado de movimento
// provisório e detecção de salto (Apparate).
#include <optional>

#include <cbmp/client/IGame.hpp>
#include <cbmp/messages.hpp>

namespace cbmp::client {
// Falling→Fall, Swimming→Swim, Flying→Broom; Ground/Other: < 10 Idle, ≤ 380 Walk, ≤ 1000 Run, acima Broom.
cbmp::MoveState ClassifyMove(MoveMode mode, double speedCmS);

class LocalSampler {
public:
    void MarkJump();  // próximo Sample sai com Apparate (entrada em InWorld, ressincronização)
    // nullopt = amostra pulada (valor não finito ou fora de ±2 000 000 000 cm); senão guarda a posição enviada.
    std::optional<cbmp::PlayerState> Sample(const LocalState& s);
    bool LastWasJump() const;

private:
    bool m_jumpPending = false, m_lastWasJump = false, m_hasLast = false;
    cbmp::Vec3i m_last;
};
}  // namespace cbmp::client
