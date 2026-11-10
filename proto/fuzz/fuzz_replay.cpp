// Driver sem libFuzzer para o alvo de proto_fuzz.cpp (todas as plataformas, roda no ctest):
//   proto_fuzz_replay                     sementes + 200 000 mutações determinísticas
//   proto_fuzz_replay --write-corpus DIR  grava as sementes (mensagens válidas codificadas) em DIR
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <cbmp/messages.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

std::vector<std::vector<uint8_t>> seeds() {
  using namespace cbmp;
  const PlayerState a{{1000, -2000, 300}, 12345, 128, {100, -200, 0}, MoveState::Run};
  PlayerState b = a;
  b.pos.x += 37;
  b.yaw = static_cast<uint16_t>(b.yaw + 900);
  b.state = MoveState::Walk;
  std::vector<Message> ms;
  Hello h;
  h.ticket = "eyJ2IjoxfQ.c2ln";
  h.buildHash[0] = 1;
  ms.emplace_back(h);
  ms.emplace_back(Welcome{7, 20, 123456789});
  ms.emplace_back(Reject{"ticket inválido"});
  ms.emplace_back(Input{42, a});
  ms.emplace_back(SnapshotAck{99});
  ms.emplace_back(EntityEnter{3, a, "Bot 3"});
  ms.emplace_back(EntityLeave{3});
  ms.emplace_back(Event{"echo", {1, 2, 3, 4, 5, 6, 7, 8}});
  ms.emplace_back(Correction{42, {1, 2, 3}, {4, 5, 6}});
  ms.emplace_back(Kick{"flood de eventos"});
  // Snapshot completo e delta (com baseline), e um grande dividido em partes.
  Snapshot full;
  full.tick = 100;
  full.ackInputSeq = 41;
  for (uint16_t id = 1; id <= 4; ++id) {
    SnapshotEntity e;
    PlayerState s = a;
    s.pos.x += id * 1000;
    if (makeSnapshotEntity(id, nullptr, s, e)) full.ents.push_back(e);
  }
  ms.emplace_back(full);
  Snapshot delta;
  delta.tick = 102;
  delta.baselineTick = 100;
  SnapshotEntity d;
  if (makeSnapshotEntity(2, &a, b, d)) delta.ents.push_back(d);
  ms.emplace_back(delta);
  Snapshot big;
  big.tick = 200;
  for (uint16_t id = 1; id <= 200; ++id) {
    SnapshotEntity e;
    PlayerState s = a;
    s.pos.y += id * 777;
    if (makeSnapshotEntity(id, nullptr, s, e)) big.ents.push_back(e);
  }
  std::vector<Snapshot> parts;
  splitSnapshot(big, kMaxSnapshotBytes, parts);
  for (auto& p : parts) ms.emplace_back(p);

  std::vector<std::vector<uint8_t>> out;
  for (const auto& m : ms) out.push_back(encode(m));
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const auto corpus = seeds();
  if (argc == 3 && std::string(argv[1]) == "--write-corpus") {
    std::filesystem::create_directories(argv[2]);
    for (size_t i = 0; i < corpus.size(); ++i) {
      std::ofstream f(std::filesystem::path(argv[2]) / ("seed_" + std::to_string(i)), std::ios::binary);
      f.write(reinterpret_cast<const char*>(corpus[i].data()), static_cast<std::streamsize>(corpus[i].size()));
    }
    std::printf("%zu sementes gravadas em %s\n", corpus.size(), argv[2]);
    return 0;
  }
  std::mt19937 rng(12345);
  size_t runs = 0;
  for (const auto& s : corpus) {
    LLVMFuzzerTestOneInput(s.data(), s.size());
    ++runs;
  }
  for (int i = 0; i < 200000; ++i) {
    std::vector<uint8_t> v = corpus[rng() % corpus.size()];
    const int edits = 1 + static_cast<int>(rng() % 4);
    for (int k = 0; k < edits; ++k) {
      switch (rng() % 4) {
        case 0:  // inverte um bit
          if (!v.empty()) v[rng() % v.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
          break;
        case 1:  // byte aleatório
          if (!v.empty()) v[rng() % v.size()] = static_cast<uint8_t>(rng());
          break;
        case 2:  // trunca
          if (!v.empty()) v.resize(rng() % v.size());
          break;
        default:  // acrescenta
          v.push_back(static_cast<uint8_t>(rng()));
          break;
      }
    }
    LLVMFuzzerTestOneInput(v.data(), v.size());
    ++runs;
  }
  std::printf("%zu entradas sem falha\n", runs);
  return 0;
}
