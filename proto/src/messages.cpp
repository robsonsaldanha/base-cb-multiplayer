#include <cbmp/messages.hpp>
#include <cbmp/quantize.hpp>
#include <algorithm>
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

// ---- Snapshot compacto ----
struct BitCounter {
  size_t bits = 0;
  void writeBits(uint64_t, unsigned n) { bits += n; }
  void writeBool(bool) { bits += 1; }
  void writeU16(uint16_t) { bits += 16; }
  void writeU32(uint32_t) { bits += 32; }
  void writeI32(int32_t) { bits += 32; }
};
constexpr size_t kHeaderBits = 8 + 32 + 32 + 5 + 3 + 3 + 8;
constexpr int32_t kD8 = 128, kD14 = 8192;

int32_t axisOf(const Vec3i& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
int32_t& axisOf(Vec3i& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
int16_t axisOf(const Vec3s& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
int16_t& axisOf(Vec3s& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }

unsigned posClass(const SnapshotEntity& e, int a) {
  if (e.posAbs & (1u << a)) return 3;
  const int32_t d = axisOf(e.pos, a);
  if (d == 0) return 0;
  if (d >= -kD8 && d < kD8) return 1;
  if (d >= -kD14 && d < kD14) return 2;
  throw std::invalid_argument("delta de posição grande demais");
}
int32_t signExtend(uint64_t v, unsigned bits) {
  const uint64_t m = uint64_t{1} << (bits - 1);
  return static_cast<int32_t>(static_cast<int64_t>(v ^ m) - static_cast<int64_t>(m));
}

void checkEntity(const SnapshotEntity& e, uint16_t prev, bool first) {
  if (first ? e.id == 0 : e.id <= prev) throw std::invalid_argument("ids fora de ordem");
  if (e.mask == 0 || e.mask > kMaskAll) throw std::invalid_argument("máscara inválida");
  if (e.posAbs > 7) throw std::invalid_argument("posAbs inválido");
  if (e.yaw10 > 1023 || e.pitch6 > 63) throw std::invalid_argument("rotação fora da faixa");
  for (int a = 0; a < 3; ++a)
    if (axisOf(e.vel, a) < -1024 || axisOf(e.vel, a) > 1023) throw std::invalid_argument("velocidade fora da faixa");
  if (static_cast<uint8_t>(e.state) > kMaxMoveState) throw std::invalid_argument("estado reservado");
}

// id: 1 bit (gap ≤ 16 → 4 bits gap-1) senão u16; máscara u4; posição (kMaskAll: 1 bit "tudo absoluto" →
// 3×i32; senão por eixo classe u2 {0 zero, 1 i8, 2 i14, 3 i32 absoluto}); rot 10+6; vel 3×11; estado u4.
template <class W>
void putEntity(W& w, const SnapshotEntity& e, uint16_t prev) {
  const unsigned gap = static_cast<unsigned>(e.id) - prev;
  if (gap >= 1 && gap <= 16) { w.writeBool(true); w.writeBits(gap - 1, 4); }
  else { w.writeBool(false); w.writeU16(e.id); }
  w.writeBits(e.mask, 4);
  if (e.mask & kMaskPos) {
    const bool allAbs = e.mask == kMaskAll && e.posAbs == 7;
    if (e.mask == kMaskAll) w.writeBool(allAbs);
    if (allAbs) {
      w.writeI32(e.pos.x); w.writeI32(e.pos.y); w.writeI32(e.pos.z);
    } else {
      for (int a = 0; a < 3; ++a) {
        const unsigned c = posClass(e, a);
        w.writeBits(c, 2);
        const int32_t v = axisOf(e.pos, a);
        if (c == 1) w.writeBits(static_cast<uint32_t>(v), 8);
        else if (c == 2) w.writeBits(static_cast<uint32_t>(v), 14);
        else if (c == 3) w.writeI32(v);
      }
    }
  }
  if (e.mask & kMaskRot) { w.writeBits(e.yaw10, 10); w.writeBits(e.pitch6, 6); }
  if (e.mask & kMaskVel)
    for (int a = 0; a < 3; ++a) w.writeBits(static_cast<uint16_t>(axisOf(e.vel, a)), 11);
  if (e.mask & kMaskState) w.writeBits(static_cast<uint8_t>(e.state), 4);
}

SnapshotEntity getEntity(BitReader& r, uint16_t prev, bool first) {
  SnapshotEntity e;
  if (r.readBool()) {
    const auto id = static_cast<uint32_t>(prev) + static_cast<uint32_t>(r.readBits(4)) + 1;
    if (id > 0xFFFF) throw DecodeError("id fora da faixa");
    e.id = static_cast<uint16_t>(id);
  } else {
    e.id = r.readU16();
    if (first ? e.id == 0 : e.id <= prev) throw DecodeError("ids fora de ordem");
  }
  e.mask = static_cast<uint8_t>(r.readBits(4));
  if (e.mask == 0) throw DecodeError("máscara vazia");
  if (e.mask & kMaskPos) {
    const bool allAbs = e.mask == kMaskAll && r.readBool();
    if (allAbs) {
      e.posAbs = 7; e.pos.x = r.readI32(); e.pos.y = r.readI32(); e.pos.z = r.readI32();
    } else {
      for (int a = 0; a < 3; ++a) {
        const auto c = r.readBits(2);
        int32_t& v = axisOf(e.pos, a);
        if (c == 1) v = signExtend(r.readBits(8), 8);
        else if (c == 2) v = signExtend(r.readBits(14), 14);
        else if (c == 3) { v = r.readI32(); e.posAbs = static_cast<uint8_t>(e.posAbs | (1u << a)); }
      }
    }
  }
  if (e.mask & kMaskRot) { e.yaw10 = static_cast<uint16_t>(r.readBits(10)); e.pitch6 = static_cast<uint8_t>(r.readBits(6)); }
  if (e.mask & kMaskVel)
    for (int a = 0; a < 3; ++a) axisOf(e.vel, a) = static_cast<int16_t>(signExtend(r.readBits(11), 11));
  if (e.mask & kMaskState) e.state = getState(r);
  return e;
}

void checkHeader(const Snapshot& x) {
  if (x.ents.size() > kMaxSnapshotEntities) throw std::length_error("entidades demais");
  if (x.partCount < 1 || x.partCount > kMaxSnapshotParts || x.partIndex >= x.partCount)
    throw std::invalid_argument("partes inválidas");
  if (x.baselineTick != 0 && (x.baselineTick >= x.tick || x.tick - x.baselineTick > kMaxBaselineAge))
    throw std::invalid_argument("idade da baseline inválida");
}

template <class W>
void putSnapshot(W& w, const Snapshot& x) {
  checkHeader(x);
  w.writeU32(x.tick); w.writeU32(x.ackInputSeq);
  w.writeBits(x.baselineTick == 0 ? 0u : x.tick - x.baselineTick, 5);
  w.writeBits(x.partIndex, 3); w.writeBits(x.partCount - 1u, 3);
  w.writeBits(x.ents.size(), 8);
  uint16_t prev = 0;
  bool first = true;
  for (const auto& e : x.ents) {
    checkEntity(e, prev, first);
    putEntity(w, e, prev);
    prev = e.id;
    first = false;
  }
}
}  // namespace

PlayerState applySnapshotEntity(const PlayerState& baseline, const SnapshotEntity& e) {
  PlayerState s = baseline;
  if (e.mask & kMaskPos)
    for (int a = 0; a < 3; ++a) {
      const int32_t v = axisOf(e.pos, a);
      // soma em módulo 2^32: bem definida mesmo com baseline adulterada no cliente
      axisOf(s.pos, a) = (e.posAbs & (1u << a))
                             ? v
                             : static_cast<int32_t>(static_cast<uint32_t>(axisOf(baseline.pos, a)) + static_cast<uint32_t>(v));
    }
  if (e.mask & kMaskRot) { s.yaw = yawFrom10(e.yaw10); s.pitch = pitchFrom6(e.pitch6); }
  if (e.mask & kMaskVel)
    for (int a = 0; a < 3; ++a) axisOf(s.vel, a) = velFromQ(axisOf(e.vel, a));
  if (e.mask & kMaskState) s.state = e.state;
  return s;
}

bool makeSnapshotEntity(uint16_t id, const PlayerState* known, const PlayerState& cur, SnapshotEntity& out) {
  out.id = id;
  out.yaw10 = yawTo10(cur.yaw);
  out.pitch6 = pitchTo6(cur.pitch);
  for (int a = 0; a < 3; ++a) axisOf(out.vel, a) = velToQ(axisOf(cur.vel, a));
  out.state = cur.state;
  if (!known) {
    out.mask = kMaskAll;
    out.posAbs = 7;
    out.pos = cur.pos;
    return true;
  }
  uint8_t m = 0;
  out.posAbs = 0;
  out.pos = {};
  if (cur.pos != known->pos) {
    m |= kMaskPos;
    for (int a = 0; a < 3; ++a) {
      const int64_t d = static_cast<int64_t>(axisOf(cur.pos, a)) - axisOf(known->pos, a);
      if (d >= -kD14 && d < kD14) {
        axisOf(out.pos, a) = static_cast<int32_t>(d);
      } else {
        axisOf(out.pos, a) = axisOf(cur.pos, a);
        out.posAbs = static_cast<uint8_t>(out.posAbs | (1u << a));
      }
    }
  }
  if (out.yaw10 != yawTo10(known->yaw) || out.pitch6 != pitchTo6(known->pitch)) m |= kMaskRot;
  for (int a = 0; a < 3; ++a) {
    const int d = axisOf(out.vel, a) - velToQ(axisOf(known->vel, a));
    if (d >= 2 || d <= -2) { m |= kMaskVel; break; }
  }
  if (cur.state != known->state) m |= kMaskState;
  out.mask = m;
  return m != 0;
}

size_t encodedSize(const Snapshot& s) {
  BitCounter c;
  c.writeBits(0, 8);
  putSnapshot(c, s);
  return (c.bits + 7) / 8;
}

void splitSnapshot(const Snapshot& s, size_t maxBytes, std::vector<Snapshot>& out) {
  out.clear();
  const size_t maxBits = maxBytes * 8;
  auto fresh = [&] {
    Snapshot p;
    p.tick = s.tick;
    p.ackInputSeq = s.ackInputSeq;
    p.baselineTick = s.baselineTick;
    out.push_back(std::move(p));
  };
  fresh();
  size_t bits = kHeaderBits;
  uint16_t prev = 0;
  for (const auto& e : s.ents) {
    BitCounter c;
    putEntity(c, e, prev);
    if (!out.back().ents.empty() && (bits + c.bits > maxBits || out.back().ents.size() == kMaxSnapshotEntities)) {
      fresh();
      bits = kHeaderBits;
      c = BitCounter{};
      putEntity(c, e, 0);
    }
    out.back().ents.push_back(e);
    bits += c.bits;
    prev = e.id;
  }
  if (out.size() > kMaxSnapshotParts) throw std::length_error("partes demais");
  for (size_t i = 0; i < out.size(); ++i) {
    out[i].partIndex = static_cast<uint8_t>(i);
    out[i].partCount = static_cast<uint8_t>(out.size());
  }
}

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
      putSnapshot(w, x);
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
      Snapshot s; s.tick = r.readU32(); s.ackInputSeq = r.readU32();
      const auto age = static_cast<uint32_t>(r.readBits(5));
      // age == tick daria baselineTick 0, ambíguo com "sem baseline"
      if (age != 0 && age >= s.tick) throw DecodeError("idade da baseline inválida");
      s.baselineTick = age == 0 ? 0 : s.tick - age;
      s.partIndex = static_cast<uint8_t>(r.readBits(3));
      s.partCount = static_cast<uint8_t>(r.readBits(3) + 1);
      if (s.partIndex >= s.partCount) throw DecodeError("partes inválidas");
      const auto n = static_cast<size_t>(r.readBits(8));
      if (n * 9 > r.remainingBits()) throw DecodeError("fim inesperado");  // cada entidade tem ≥ 9 bits
      s.ents.reserve(n);
      uint16_t prev = 0;
      for (size_t i = 0; i < n; ++i) {
        s.ents.push_back(getEntity(r, prev, i == 0));
        prev = s.ents.back().id;
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
