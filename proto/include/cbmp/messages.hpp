#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <cbmp/bitstream.hpp>

namespace cbmp {
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr size_t kMaxTicketLen = 2048, kMaxEventName = 64, kMaxEventPayload = 4096,
                        kMaxSnapshotEntities = 255, kMaxSnapshotBytes = 1100, kMaxSnapshotParts = 8,
                        kMaxBaselineAge = 31, kMaxReason = 255, kMaxName = 64;

enum class MsgType : uint8_t {
  Hello = 1, Welcome = 2, Reject = 3,
  Input = 10, Snapshot = 11, SnapshotAck = 12, EntityEnter = 13, EntityLeave = 14,
  Event = 20, Correction = 21, Kick = 22,
};
enum class MoveState : uint8_t { Idle = 0, Walk = 1, Run = 2, Broom = 3, Swim = 4, Fall = 5, Apparate = 6 };
inline constexpr uint8_t kMaxMoveState = 6;

struct Vec3i { int32_t x = 0, y = 0, z = 0; bool operator==(const Vec3i&) const = default; };
struct Vec3s { int16_t x = 0, y = 0, z = 0; bool operator==(const Vec3s&) const = default; };
struct PlayerState {
  Vec3i pos; uint16_t yaw = 0; uint8_t pitch = 128; Vec3s vel; MoveState state = MoveState::Idle;
  bool operator==(const PlayerState&) const = default;
};

inline constexpr uint8_t kMaskPos = 1, kMaskRot = 2, kMaskVel = 4, kMaskState = 8, kMaskAll = 15;

struct Hello { uint32_t protocol = kProtocolVersion; std::array<uint8_t, 32> buildHash{}; std::string ticket; };
struct Welcome { uint16_t playerId = 0; uint8_t tickRateHz = 20; uint64_t serverTimeMs = 0; };
struct Reject { std::string reason; };
struct Input { uint32_t seq = 0; PlayerState s; };
// Entidade de snapshot já na forma do fio (valores quantizados / deltas). O cliente obtém o estado com
// applySnapshotEntity(baseline, e); o servidor guarda exatamente o mesmo resultado como baseline futura.
struct SnapshotEntity {
  uint16_t id = 0;
  uint8_t mask = 0;          // kMaskPos | kMaskRot | kMaskVel | kMaskState (≠ 0)
  uint8_t posAbs = 0;        // bit por eixo (x=1,y=2,z=4): valor absoluto; senão delta vs baseline (|d| < 8192)
  Vec3i pos;                 // delta ou absoluto por eixo (só com kMaskPos)
  uint16_t yaw10 = 0;        // 0..1023 (kMaskRot)
  uint8_t pitch6 = 0;        // 0..63 (kMaskRot)
  Vec3s vel;                 // passos de 12,5 cm/s, -1024..1023 (kMaskVel)
  MoveState state = MoveState::Idle;  // kMaskState
};
// Snapshot. No fio: tick u32, ackInputSeq u32, idade da baseline u5 (0 = sem baseline; senão tick - baselineTick),
// parte u3, total-1 u3, n u8, entidades com ids estritamente crescentes. Um tick grande vira até 8 partes
// (cada uma ≤ kMaxSnapshotBytes); o cliente só confirma (SnapshotAck) o tick quando recebe todas as partes, e o
// servidor trata o ack do tick como cobrindo todas.
struct Snapshot {
  uint32_t tick = 0, ackInputSeq = 0, baselineTick = 0;
  uint8_t partIndex = 0, partCount = 1;
  std::vector<SnapshotEntity> ents;
};
struct SnapshotAck { uint32_t tick = 0; };
struct EntityEnter { uint16_t id = 0; PlayerState s; std::string name; };
struct EntityLeave { uint16_t id = 0; };
struct Event { std::string name; std::vector<uint8_t> payload; };
struct Correction { uint32_t seq = 0; Vec3i pos; Vec3s vel; };
struct Kick { std::string reason; };

using Message = std::variant<Hello, Welcome, Reject, Input, Snapshot, SnapshotAck, EntityEnter, EntityLeave, Event, Correction, Kick>;

std::vector<uint8_t> encode(const Message& m);
Message decode(std::span<const uint8_t> bytes);   // lança DecodeError
MsgType typeOf(const Message& m);
bool isReliable(MsgType t);                        // Input/Snapshot/SnapshotAck = não confiável

// Estado conhecido pelo cliente após aplicar `e` sobre a baseline (pura; usada no servidor e no cliente).
PlayerState applySnapshotEntity(const PlayerState& baseline, const SnapshotEntity& e);
// Monta a entidade a enviar dado o estado conhecido pelo cliente (nullptr = fora da baseline → completa,
// posição absoluta). Retorna false se nada mudou o bastante para enviar.
bool makeSnapshotEntity(uint16_t id, const PlayerState* known, const PlayerState& cur, SnapshotEntity& out);
// Tamanho codificado em bytes (sem codificar).
size_t encodedSize(const Snapshot& s);
// Divide em partes de até maxBytes cada (partIndex/partCount preenchidos). `out` é limpo antes.
void splitSnapshot(const Snapshot& s, size_t maxBytes, std::vector<Snapshot>& out);
}  // namespace cbmp
