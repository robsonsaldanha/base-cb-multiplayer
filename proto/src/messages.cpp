#include <cbmp/messages.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cbmp {
namespace {
void putVec3i(BitWriter& w, const Vec3i& v) { w.writeI32(v.x); w.writeI32(v.y); w.writeI32(v.z); }
void putVec3s(BitWriter& w, const Vec3s& v) { w.writeI16(v.x); w.writeI16(v.y); w.writeI16(v.z); }
Vec3i getVec3i(BitReader& r) { Vec3i v; v.x = r.readI32(); v.y = r.readI32(); v.z = r.readI32(); return v; }
Vec3s getVec3s(BitReader& r) { Vec3s v; v.x = r.readI16(); v.y = r.readI16(); v.z = r.readI16(); return v; }
MoveState getState(BitReader& r) {
  auto s = static_cast<uint8_t>(r.readBits(4));
  if (s > kMaxMoveState) throw DecodeError("estado reservado");
  return static_cast<MoveState>(s);
}
void putFull(BitWriter& w, const PlayerState& s) {
  putVec3i(w, s.pos); w.writeU16(s.yaw); w.writeU8(s.pitch); putVec3s(w, s.vel); w.writeBits(static_cast<uint8_t>(s.state), 4);
}
PlayerState getFull(BitReader& r) {
  PlayerState s; s.pos = getVec3i(r); s.yaw = r.readU16(); s.pitch = r.readU8(); s.vel = getVec3s(r); s.state = getState(r); return s;
}
template <class> inline constexpr bool always_false = false;
}  // namespace

MsgType typeOf(const Message& m) {
  return std::visit([](const auto& x) -> MsgType {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, Hello>) return MsgType::Hello;
    else if constexpr (std::is_same_v<T, Welcome>) return MsgType::Welcome;
    else if constexpr (std::is_same_v<T, Reject>) return MsgType::Reject;
    else if constexpr (std::is_same_v<T, Input>) return MsgType::Input;
    else if constexpr (std::is_same_v<T, Snapshot>) return MsgType::Snapshot;
    else if constexpr (std::is_same_v<T, SnapshotAck>) return MsgType::SnapshotAck;
    else if constexpr (std::is_same_v<T, EntityEnter>) return MsgType::EntityEnter;
    else if constexpr (std::is_same_v<T, EntityLeave>) return MsgType::EntityLeave;
    else if constexpr (std::is_same_v<T, Event>) return MsgType::Event;
    else if constexpr (std::is_same_v<T, Correction>) return MsgType::Correction;
    else if constexpr (std::is_same_v<T, Kick>) return MsgType::Kick;
    else static_assert(always_false<T>);
  }, m);
}

bool isReliable(MsgType t) { return !(t == MsgType::Input || t == MsgType::Snapshot || t == MsgType::SnapshotAck); }

std::vector<uint8_t> encode(const Message& m) {
  BitWriter w;
  w.writeU8(static_cast<uint8_t>(typeOf(m)));
  std::visit([&](const auto& x) {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, Hello>) {
      w.writeU32(x.protocol); for (auto b : x.buildHash) w.writeU8(b); w.writeString(x.ticket, 12, kMaxTicketLen);
    } else if constexpr (std::is_same_v<T, Welcome>) {
      w.writeU16(x.playerId); w.writeU8(x.tickRateHz); w.writeU64(x.serverTimeMs);
    } else if constexpr (std::is_same_v<T, Reject> || std::is_same_v<T, Kick>) {
      w.writeString(x.reason, 8, kMaxReason);
    } else if constexpr (std::is_same_v<T, Input>) {
      w.writeU32(x.seq); putFull(w, x.s);
    } else if constexpr (std::is_same_v<T, Snapshot>) {
      if (x.ents.size() > kMaxSnapshotEntities) throw std::length_error("entidades demais");
      w.writeU32(x.tick); w.writeU32(x.ackInputSeq); w.writeU32(x.baselineTick); w.writeBits(x.ents.size(), 9);
      for (const auto& e : x.ents) {
        if (e.mask == 0 || e.mask > kMaskAll) throw std::invalid_argument("máscara inválida");
        w.writeU16(e.id); w.writeBits(e.mask, 4);
        if (e.mask & kMaskPos) putVec3i(w, e.s.pos);
        if (e.mask & kMaskRot) { w.writeU16(e.s.yaw); w.writeU8(e.s.pitch); }
        if (e.mask & kMaskVel) putVec3s(w, e.s.vel);
        if (e.mask & kMaskState) w.writeBits(static_cast<uint8_t>(e.s.state), 4);
      }
    } else if constexpr (std::is_same_v<T, SnapshotAck>) {
      w.writeU32(x.tick);
    } else if constexpr (std::is_same_v<T, EntityEnter>) {
      w.writeU16(x.id); putFull(w, x.s); w.writeString(x.name, 7, kMaxName);
    } else if constexpr (std::is_same_v<T, EntityLeave>) {
      w.writeU16(x.id);
    } else if constexpr (std::is_same_v<T, Event>) {
      if (x.name.empty()) throw std::invalid_argument("evento sem nome");
      w.writeString(x.name, 7, kMaxEventName); w.writeBlob(x.payload, 13, kMaxEventPayload);
    } else if constexpr (std::is_same_v<T, Correction>) {
      w.writeU32(x.seq); putVec3i(w, x.pos); putVec3s(w, x.vel);
    }
  }, m);
  return w.finish();
}

Message decode(std::span<const uint8_t> bytes) {
  if (bytes.empty()) throw DecodeError("mensagem vazia");
  BitReader r(bytes);
  const auto type = static_cast<MsgType>(r.readU8());
  Message out;
  switch (type) {
    case MsgType::Hello: { Hello h; h.protocol = r.readU32(); for (auto& b : h.buildHash) b = r.readU8(); h.ticket = r.readString(12, kMaxTicketLen); out = std::move(h); break; }
    case MsgType::Welcome: { Welcome x; x.playerId = r.readU16(); x.tickRateHz = r.readU8(); x.serverTimeMs = r.readU64(); out = x; break; }
    case MsgType::Reject: out = Reject{r.readString(8, kMaxReason)}; break;
    case MsgType::Kick: out = Kick{r.readString(8, kMaxReason)}; break;
    case MsgType::Input: { Input x; x.seq = r.readU32(); x.s = getFull(r); out = x; break; }
    case MsgType::Snapshot: {
      Snapshot s; s.tick = r.readU32(); s.ackInputSeq = r.readU32(); s.baselineTick = r.readU32();
      const auto n = r.readBits(9);
      if (n > kMaxSnapshotEntities) throw DecodeError("entidades demais");
      s.ents.reserve(static_cast<size_t>(n));
      for (uint64_t i = 0; i < n; ++i) {
        SnapshotEntity e; e.id = r.readU16(); e.mask = static_cast<uint8_t>(r.readBits(4));
        if (e.mask == 0) throw DecodeError("máscara vazia");
        if (e.mask & kMaskPos) e.s.pos = getVec3i(r);
        if (e.mask & kMaskRot) { e.s.yaw = r.readU16(); e.s.pitch = r.readU8(); }
        if (e.mask & kMaskVel) e.s.vel = getVec3s(r);
        if (e.mask & kMaskState) e.s.state = getState(r);
        s.ents.push_back(e);
      }
      out = std::move(s); break;
    }
    case MsgType::SnapshotAck: out = SnapshotAck{r.readU32()}; break;
    case MsgType::EntityEnter: { EntityEnter x; x.id = r.readU16(); x.s = getFull(r); x.name = r.readString(7, kMaxName); out = std::move(x); break; }
    case MsgType::EntityLeave: out = EntityLeave{r.readU16()}; break;
    case MsgType::Event: {
      Event e; e.name = r.readString(7, kMaxEventName);
      if (e.name.empty()) throw DecodeError("evento sem nome");
      e.payload = r.readBlob(13, kMaxEventPayload); out = std::move(e); break;
    }
    case MsgType::Correction: { Correction c; c.seq = r.readU32(); c.pos = getVec3i(r); c.vel = getVec3s(r); out = c; break; }
    default: throw DecodeError("tipo desconhecido");
  }
  r.expectEnd();
  return out;
}
}  // namespace cbmp
