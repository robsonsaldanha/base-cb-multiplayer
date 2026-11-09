#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
#include <cbmp/messages.hpp>
using namespace cbmp;

static PlayerState ps(int x) { return PlayerState{{x, -x, 1000}, 12345, 128, {100, -200, 0}, MoveState::Run}; }
template <class T> static T roundtrip(const T& m) { return std::get<T>(decode(encode(Message{m}))); }

TEST_CASE("Hello/Welcome/Reject ida e volta") {
  Hello h{kProtocolVersion, {}, "abc.def"}; h.buildHash[0] = 7; h.buildHash[31] = 9;
  auto h2 = roundtrip(h); CHECK(h2.protocol == 1); CHECK(h2.buildHash == h.buildHash); CHECK(h2.ticket == "abc.def");
  auto w = roundtrip(Welcome{42, 20, 1'800'000'000'000ull}); CHECK(w.playerId == 42); CHECK(w.tickRateHz == 20); CHECK(w.serverTimeMs == 1'800'000'000'000ull);
  CHECK(roundtrip(Reject{"ticket inválido"}).reason == "ticket inválido");
}
TEST_CASE("Input ida e volta preserva todos os campos") {
  auto i = roundtrip(Input{77, ps(5000)}); CHECK(i.seq == 77); CHECK(i.s == ps(5000));
}
TEST_CASE("Snapshot so carrega campos da mascara") {
  Snapshot s{100, 77, 98, {}};
  s.ents.push_back({1, kMaskAll, ps(1)});
  s.ents.push_back({2, kMaskPos, ps(2)});
  auto bytesFull = encode(Message{s}).size();
  auto d = roundtrip(s);
  REQUIRE(d.ents.size() == 2);
  CHECK(d.ents[0].mask == kMaskAll); CHECK(d.ents[0].s == ps(1));
  CHECK(d.ents[1].mask == kMaskPos); CHECK(d.ents[1].s.pos == ps(2).pos);
  Snapshot only{100, 77, 98, {{2, kMaskPos, ps(2)}}};
  CHECK(encode(Message{only}).size() < bytesFull);
}
TEST_CASE("Snapshot recusa mascara zero e entidades demais") {
  Snapshot s{1, 1, 1, {{1, 0, ps(1)}}};
  CHECK_THROWS(encode(Message{s}));
  Snapshot big{1, 1, 1, std::vector<SnapshotEntity>(kMaxSnapshotEntities + 1, SnapshotEntity{1, kMaskPos, ps(1)})};
  CHECK_THROWS(encode(Message{big}));
}
TEST_CASE("Event, Correction, Kick, EntityEnter/Leave, SnapshotAck") {
  auto e = roundtrip(Event{"chat", {1, 2, 3}}); CHECK(e.name == "chat"); CHECK(e.payload == std::vector<uint8_t>{1, 2, 3});
  auto c = roundtrip(Correction{9, {1, 2, 3}, {4, 5, 6}}); CHECK(c.seq == 9); CHECK(c.pos == Vec3i{1, 2, 3}); CHECK(c.vel == Vec3s{4, 5, 6});
  CHECK(roundtrip(Kick{"flood"}).reason == "flood");
  auto en = roundtrip(EntityEnter{3, ps(7), "Jogador"}); CHECK(en.id == 3); CHECK(en.s == ps(7)); CHECK(en.name == "Jogador");
  CHECK(roundtrip(EntityLeave{3}).id == 3);
  CHECK(roundtrip(SnapshotAck{55}).tick == 55);
}
TEST_CASE("decode recusa lixo") {
  CHECK_THROWS_AS(decode(std::vector<uint8_t>{}), DecodeError);
  CHECK_THROWS_AS(decode(std::vector<uint8_t>{0xEE}), DecodeError);         // tipo desconhecido
  auto b = encode(Message{Input{1, ps(1)}}); b.pop_back();
  CHECK_THROWS_AS(decode(b), DecodeError);                                   // truncado
  auto ok = encode(Message{Kick{"x"}}); ok.push_back(0xFF);
  CHECK_THROWS_AS(decode(ok), DecodeError);                                  // sobra
  BitWriter w; w.writeU8(static_cast<uint8_t>(MsgType::Input)); w.writeU32(1);
  w.writeI32(0); w.writeI32(0); w.writeI32(0); w.writeU16(0); w.writeU8(0); w.writeI16(0); w.writeI16(0); w.writeI16(0);
  w.writeBits(9, 4);                                                         // estado reservado
  CHECK_THROWS_AS(decode(w.finish()), DecodeError);
}
