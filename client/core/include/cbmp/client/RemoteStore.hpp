#pragma once
// Entidades remotas anunciadas pelo servidor (EntityEnter/EntityLeave) com o último estado conhecido.
#include <cstdint>
#include <map>
#include <string>

#include <cbmp/messages.hpp>

namespace cbmp::client {
struct RemoteEntity {
    uint16_t id = 0;
    std::string name;  // já saneado (SanitizeText, ≤ 64 bytes)
    cbmp::PlayerState last;
    double lastAt = 0;
};

class RemoteStore {
public:
    bool Enter(const cbmp::EntityEnter& e, double now);  // false = cheio (255); reentrada do mesmo id substitui
    bool Leave(uint16_t id);
    bool Known(uint16_t id) const;
    void Update(uint16_t id, const cbmp::PlayerState& s, double now);  // id desconhecido: ignora
    const std::map<uint16_t, RemoteEntity>& All() const;
    void Clear();

private:
    std::map<uint16_t, RemoteEntity> m_ents;
};
}  // namespace cbmp::client
