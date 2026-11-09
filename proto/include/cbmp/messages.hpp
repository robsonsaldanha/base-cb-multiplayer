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
                        kMaxSnapshotEntities = 256, kMaxReason = 255, kMaxName = 64;

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
struct SnapshotEntity { uint16_t id = 0; uint8_t mask = 0; PlayerState s; };
struct Snapshot { uint32_t tick = 0, ackInputSeq = 0, baselineTick = 0; std::vector<SnapshotEntity> ents; };
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
}  // namespace cbmp
