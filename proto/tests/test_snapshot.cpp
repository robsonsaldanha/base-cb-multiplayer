#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <cbmp/messages.hpp>
#include <cbmp/quantize.hpp>

using namespace cbmp;

namespace {
PlayerState st(int32_t x, int32_t y, int32_t z, uint16_t yaw = 0, uint8_t pitch = 128, Vec3s vel = {},
               MoveState m = MoveState::Walk) {
  PlayerState s;
  s.pos = {x, y, z};
  s.yaw = yaw;
  s.pitch = pitch;
  s.vel = vel;
  s.state = m;
  return s;
}
Snapshot rt(const Snapshot& s) { return std::get<Snapshot>(decode(encode(Message{s}))); }
SnapshotEntity make(uint16_t id, const PlayerState* known, const PlayerState& cur) {
  SnapshotEntity e;
  REQUIRE(makeSnapshotEntity(id, known, cur, e));
  return e;
}
}  // namespace

TEST_CASE("quantizers are stable on their own lattice") {
  for (unsigned q = 0; q < 1024; ++q) REQUIRE(yawTo10(yawFrom10(static_cast<uint16_t>(q))) == q);
  for (unsigned q = 0; q < 64; ++q) REQUIRE(pitchTo6(pitchFrom6(static_cast<uint8_t>(q))) == q);
  for (int q = -1024; q <= 1023; ++q) REQUIRE(velToQ(velFromQ(static_cast<int16_t>(q))) == q);
  REQUIRE(velToQ(32767) == 1023);
  REQUIRE(velToQ(-32768) == -1024);
  REQUIRE(velToQ(13) == 1);
  REQUIRE(velToQ(-12) == -1);
  REQUIRE(yawTo10(65535) == 0);  // volta completa
}

TEST_CASE("snapshot entity not in baseline is absolute and full") {
  auto cur = st(123456, -7, 99, 40000, 200, {400, -300, 0}, MoveState::Run);
  auto e = make(5, nullptr, cur);
  REQUIRE(e.mask == kMaskAll);
  REQUIRE(e.posAbs == 7);
  Snapshot s{10, 3, 0, 0, 1, {e}};
  auto d = rt(s);
  REQUIRE(d.ents.size() == 1);
  auto known = applySnapshotEntity(PlayerState{}, d.ents[0]);
  REQUIRE(known.pos == cur.pos);
  REQUIRE(known.yaw == yawFrom10(yawTo10(cur.yaw)));
  REQUIRE(known.pitch == pitchFrom6(pitchTo6(cur.pitch)));
  REQUIRE(known.vel == Vec3s{velFromQ(velToQ(400)), velFromQ(velToQ(-300)), 0});
  REQUIRE(known.state == MoveState::Run);
  // aplicar no cliente == registro do servidor
  REQUIRE(known == applySnapshotEntity(PlayerState{}, e));
}

TEST_CASE("snapshot entity delta uses position classes and small encoding") {
  auto base = applySnapshotEntity(PlayerState{}, make(1, nullptr, st(1000, 2000, 300)));
  auto cur = base;
  cur.pos = {base.pos.x + 20, base.pos.y - 5000, base.pos.z};  // classes 1, 2, 0
  SnapshotEntity e;
  REQUIRE(makeSnapshotEntity(1, &base, cur, e));
  REQUIRE(e.mask == kMaskPos);
  REQUIRE(e.posAbs == 0);
  auto d = rt(Snapshot{10, 0, 9, 0, 1, {e}});
  REQUIRE(applySnapshotEntity(base, d.ents[0]).pos == cur.pos);
  // grande salto → eixo absoluto
  cur.pos.x = base.pos.x + 1'000'000;
  REQUIRE(makeSnapshotEntity(1, &base, cur, e));
  REQUIRE((e.posAbs & 1) == 1);
  d = rt(Snapshot{10, 0, 9, 0, 1, {e}});
  REQUIRE(applySnapshotEntity(base, d.ents[0]).pos == cur.pos);
  // entidade com posição de 8 bits e nada mais: header 12 B + ~4 B
  cur = base;
  cur.pos.x += 3;
  REQUIRE(makeSnapshotEntity(1, &base, cur, e));
  REQUIRE(encode(Message{Snapshot{10, 0, 9, 0, 1, {e}}}).size() <= 16);
}

TEST_CASE("snapshot entity thresholds for rotation and velocity") {
  auto base = applySnapshotEntity(PlayerState{}, make(1, nullptr, st(0, 0, 0, 1000, 100, {400, 0, 0})));
  SnapshotEntity e;
  auto cur = base;
  REQUIRE_FALSE(makeSnapshotEntity(1, &base, cur, e));  // nada mudou
  cur.yaw = static_cast<uint16_t>(base.yaw + 10);  // < meio passo de 10 bits
  cur.vel.x = static_cast<int16_t>(base.vel.x + 13);  // 1 passo
  REQUIRE_FALSE(makeSnapshotEntity(1, &base, cur, e));
  cur.vel.x = static_cast<int16_t>(base.vel.x + 25);  // 2 passos
  REQUIRE(makeSnapshotEntity(1, &base, cur, e));
  REQUIRE(e.mask == kMaskVel);
  cur = base;
  cur.yaw = static_cast<uint16_t>(base.yaw + 64);
  REQUIRE(makeSnapshotEntity(1, &base, cur, e));
  REQUIRE(e.mask == kMaskRot);
  cur = base;
  cur.state = MoveState::Broom;
  REQUIRE(makeSnapshotEntity(1, &base, cur, e));
  REQUIRE(e.mask == kMaskState);
  REQUIRE(applySnapshotEntity(base, rt(Snapshot{2, 0, 1, 0, 1, {e}}).ents[0]).state == MoveState::Broom);
}

TEST_CASE("snapshot header roundtrip with baseline age and parts") {
  Snapshot s{1000, 77, 990, 2, 3, {}};
  auto d = rt(s);
  REQUIRE(d.tick == 1000);
  REQUIRE(d.ackInputSeq == 77);
  REQUIRE(d.baselineTick == 990);
  REQUIRE(d.partIndex == 2);
  REQUIRE(d.partCount == 3);
  REQUIRE(encode(Message{s}).size() == 12);  // 8+32+32+5+3+3+8 = 91 bits
  REQUIRE(rt(Snapshot{5, 0, 0, 0, 1, {}}).baselineTick == 0);
  CHECK_THROWS(encode(Message{Snapshot{1000, 0, 960, 0, 1, {}}}));  // idade > 31
  CHECK_THROWS(encode(Message{Snapshot{1000, 0, 1000, 0, 1, {}}}));  // idade 0 com baseline
  CHECK_THROWS(encode(Message{Snapshot{1000, 0, 0, 3, 3, {}}}));    // índice >= total
  CHECK_THROWS(encode(Message{Snapshot{1000, 0, 0, 0, 9, {}}}));    // total > 8
}

TEST_CASE("snapshot ids gap encoding and ordering") {
  auto cur = st(1, 2, 3);
  Snapshot s{10, 0, 0, 0, 1, {}};
  for (int id : {1, 2, 17, 18, 300, 65535}) s.ents.push_back(make(static_cast<uint16_t>(id), nullptr, cur));
  auto d = rt(s);
  REQUIRE(d.ents.size() == 6);
  REQUIRE(d.ents[2].id == 17);
  REQUIRE(d.ents[4].id == 300);
  REQUIRE(d.ents[5].id == 65535);
  Snapshot bad = s;
  std::swap(bad.ents[0], bad.ents[1]);
  CHECK_THROWS(encode(Message{bad}));
  bad = s;
  bad.ents[1].id = 1;  // repetido
  CHECK_THROWS(encode(Message{bad}));
  Snapshot zero{10, 0, 0, 0, 1, {make(1, nullptr, cur)}};
  zero.ents[0].mask = 0;
  CHECK_THROWS(encode(Message{zero}));
}

TEST_CASE("snapshot decoder rejects malformed input with DecodeError only") {
  Snapshot s{10, 0, 0, 0, 1, {make(3, nullptr, st(1, 2, 3)), make(9, nullptr, st(4, 5, 6))}};
  auto b = encode(Message{s});
  for (size_t n = 1; n < b.size(); ++n) {
    std::vector<uint8_t> t(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK_THROWS_AS(decode(t), DecodeError);
  }
  // índice >= total
  BitWriter w;
  w.writeU8(static_cast<uint8_t>(MsgType::Snapshot));
  w.writeU32(10); w.writeU32(0); w.writeBits(0, 5); w.writeBits(4, 3); w.writeBits(1, 3); w.writeBits(0, 8);
  CHECK_THROWS_AS(decode(w.finish()), DecodeError);
  // idade > tick
  BitWriter w2;
  w2.writeU8(static_cast<uint8_t>(MsgType::Snapshot));
  w2.writeU32(3); w2.writeU32(0); w2.writeBits(5, 5); w2.writeBits(0, 3); w2.writeBits(0, 3); w2.writeBits(0, 8);
  CHECK_THROWS_AS(decode(w2.finish()), DecodeError);
  // id explícito não crescente (1, depois 16 bits = 1)
  BitWriter w3;
  w3.writeU8(static_cast<uint8_t>(MsgType::Snapshot));
  w3.writeU32(3); w3.writeU32(0); w3.writeBits(0, 5); w3.writeBits(0, 3); w3.writeBits(0, 3); w3.writeBits(2, 8);
  w3.writeBool(true); w3.writeBits(0, 4); w3.writeBits(kMaskState, 4); w3.writeBits(1, 4);
  w3.writeBool(false); w3.writeU16(1); w3.writeBits(kMaskState, 4); w3.writeBits(1, 4);
  CHECK_THROWS_AS(decode(w3.finish()), DecodeError);
  // estado reservado
  BitWriter w4;
  w4.writeU8(static_cast<uint8_t>(MsgType::Snapshot));
  w4.writeU32(3); w4.writeU32(0); w4.writeBits(0, 5); w4.writeBits(0, 3); w4.writeBits(0, 3); w4.writeBits(1, 8);
  w4.writeBool(true); w4.writeBits(0, 4); w4.writeBits(kMaskState, 4); w4.writeBits(9, 4);
  CHECK_THROWS_AS(decode(w4.finish()), DecodeError);
}

TEST_CASE("snapshot split 128 full entities into parts under 1100 bytes") {
  Snapshot s{50, 9, 0, 0, 1, {}};
  for (uint16_t i = 0; i < 128; ++i)
    s.ents.push_back(make(static_cast<uint16_t>(1 + i * 300), nullptr,
                          st(1'000'000 + i, -2'000'000, 70000, 12345, 7, {1200, -900, 300}, MoveState::Broom)));
  REQUIRE(encodedSize(s) > kMaxSnapshotBytes);
  std::vector<Snapshot> parts;
  splitSnapshot(s, kMaxSnapshotBytes, parts);
  REQUIRE(parts.size() >= 2);
  std::vector<SnapshotEntity> all;
  for (size_t i = 0; i < parts.size(); ++i) {
    auto bytes = encode(Message{parts[i]});
    REQUIRE(bytes.size() <= kMaxSnapshotBytes);
    REQUIRE(bytes.size() == encodedSize(parts[i]));
    auto d = std::get<Snapshot>(decode(bytes));
    REQUIRE(d.tick == 50);
    REQUIRE(d.ackInputSeq == 9);
    REQUIRE(d.partIndex == i);
    REQUIRE(d.partCount == parts.size());
    all.insert(all.end(), d.ents.begin(), d.ents.end());
  }
  REQUIRE(all.size() == 128);
  for (size_t i = 0; i < all.size(); ++i) {
    REQUIRE(all[i].id == s.ents[i].id);
    REQUIRE(applySnapshotEntity(PlayerState{}, all[i]) == applySnapshotEntity(PlayerState{}, s.ents[i]));
  }
  // pequeno: uma parte só
  Snapshot small{50, 9, 0, 0, 1, {s.ents[0]}};
  splitSnapshot(small, kMaxSnapshotBytes, parts);
  REQUIRE(parts.size() == 1);
  REQUIRE(parts[0].partCount == 1);
}
