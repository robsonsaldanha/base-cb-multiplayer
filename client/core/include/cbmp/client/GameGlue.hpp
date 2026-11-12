#pragma once
// Peças puras da camada do jogo (client/src/game/Ue4ssGame usa; aqui sem UE4SS, para serem testadas no build da
// raiz): conversões dos valores lidos do motor, alternativas de chamada que ficam fixadas na sessão, a tabela
// PuppetId → referência fraca e o "registrar no log uma vez só".
#include <cbmp/client/IGame.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cbmp::client {
// CharacterMovementComponent.MovementMode (EMovementMode do UE 4.27): 1 Walking e 2 NavWalking → Ground; 3 Falling;
// 4 Swimming; 5 Flying; o resto (0 None, 6 Custom…) → Other.
MoveMode MoveModeFromMovementMode(uint8_t movementMode);

// Identificador opaco de um UObject = índice no GUObjectArray + número de série (o par de um FWeakObjectPtr).
// 0 = nenhum (série 0 ou índice negativo). Nunca o ponteiro: um objeto novo no mesmo endereço ou índice dá outro valor.
uint64_t ObjectKey(int32_t index, int32_t serial);

// UTF-8 → UTF-16 (texto para a tela). Byte inválido, sequência cortada, forma longa demais, substituto codificado ou
// acima de U+10FFFF → U+FFFD por byte problemático; fora do BMP → par substituto.
std::u16string Utf8ToUtf16(std::string_view utf8);

// Resultado de tentar uma alternativa: Absent = ela não existe neste jogo (função ou propriedade ausente), tenta a
// próxima; Done = existe e foi chamada. Falha de verdade é exceção (GameError), que passa direto.
enum class Attempt { Absent, Done };

// Alternativas tentadas em ordem; a primeira que existe fica fixada para a sessão. Sem nenhuma, fica esgotada e não
// tenta mais.
class FallbackLadder {
public:
    explicit FallbackLadder(size_t steps) : m_steps(steps) {}

    // step(i) → Attempt. Fixada: só ela (Absent → nullopt, continua fixada). Sem fixada: 0, 1, … até um Done, que
    // fixa. Exceção de um passo passa para quem chamou sem fixar nada. Devolve o índice que rodou.
    template <class F>
    std::optional<size_t> Run(F&& step) {
        if (m_pinned) {
            if (step(*m_pinned) == Attempt::Done) return m_pinned;
            return std::nullopt;
        }
        if (m_exhausted) return std::nullopt;
        for (size_t i = 0; i < m_steps; ++i) {
            if (step(i) == Attempt::Done) {
                m_pinned = i;
                return m_pinned;
            }
        }
        m_exhausted = true;
        return std::nullopt;
    }

    std::optional<size_t> Pinned() const { return m_pinned; }
    bool Exhausted() const { return m_exhausted; }

private:
    size_t m_steps;
    std::optional<size_t> m_pinned;
    bool m_exhausted = false;
};

// PuppetId → referência (no jogo, FWeakObjectPtr). Ids a partir de 1, nunca reutilizados (nem depois de Clear).
template <class Handle>
class PuppetTable {
public:
    // nullopt só se os 2^32 − 1 ids acabaram.
    std::optional<PuppetId> Add(Handle h) {
        if (m_next == 0) return std::nullopt;
        const PuppetId id{m_next++};
        m_items.emplace(id.v, std::move(h));
        return id;
    }
    Handle* Find(PuppetId id) {
        const auto it = m_items.find(id.v);
        return it == m_items.end() ? nullptr : &it->second;
    }
    bool Remove(PuppetId id) { return m_items.erase(id.v) > 0; }
    void Clear() { m_items.clear(); }
    size_t Size() const { return m_items.size(); }

private:
    uint32_t m_next = 1;
    std::unordered_map<uint32_t, Handle> m_items;
};

// "Registrar uma vez por sessão": First(chave) é true só na primeira chamada de cada chave.
class OnceSet {
public:
    bool First(std::string_view key) { return m_seen.emplace(key).second; }

private:
    std::set<std::string, std::less<>> m_seen;
};
}  // namespace cbmp::client
