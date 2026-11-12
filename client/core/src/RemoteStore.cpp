#include <cbmp/client/RemoteStore.hpp>

#include <cbmp/client/Limits.hpp>
#include <cbmp/client/NetClient.hpp>

namespace cbmp::client {
bool RemoteStore::Enter(const cbmp::EntityEnter& e, double now) {
    if (!m_ents.contains(e.id) && m_ents.size() >= limits::kMaxEntities) return false;
    m_ents[e.id] = RemoteEntity{e.id, SanitizeText(e.name, limits::kMaxNameBytes), e.s, now};
    return true;
}

bool RemoteStore::Leave(uint16_t id) { return m_ents.erase(id) > 0; }

bool RemoteStore::Known(uint16_t id) const { return m_ents.contains(id); }

void RemoteStore::Update(uint16_t id, const cbmp::PlayerState& s, double now) {
    auto it = m_ents.find(id);
    if (it == m_ents.end()) return;
    it->second.last = s;
    it->second.lastAt = now;
}

const std::map<uint16_t, RemoteEntity>& RemoteStore::All() const { return m_ents; }

void RemoteStore::Clear() { m_ents.clear(); }
}  // namespace cbmp::client
