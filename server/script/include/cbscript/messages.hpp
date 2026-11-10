#pragma once
// Tipos das filas entre a simulação e a thread de scripts (ver ScriptHost em host.hpp).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cbscript {

// Um jogador na "foto" do tick (imutável depois de publicada).
struct PlayerView {
  uint16_t id = 0;
  std::string name, did, sub;
  int32_t x = 0, y = 0, z = 0;
  uint8_t state = 0;
};
using PlayersSnapshot = std::shared_ptr<const std::vector<PlayerView>>;

// ---- Simulação → scripts ----
struct EvPlayerJoin {
  PlayerView p;
};
struct EvPlayerLeave {
  uint16_t id = 0;
  std::string did;
  std::string reason;
};
struct EvClientEvent {
  uint16_t id = 0;
  std::string name;
  std::string json;  // payload JSON (≤ 4096 bytes; validado na conversão para Lua)
};
struct EvCommand {
  std::optional<uint16_t> from;  // jogador que digitou (chat) ou vazio (console/painel)
  std::string adminDid;          // quem pediu, quando veio do painel
  std::string line;
  std::function<void(std::string)> reply;  // chamado na thread de scripts; precisa ser thread-safe do lado de quem pediu
};
using SimToScript = std::variant<EvPlayerJoin, EvPlayerLeave, EvClientEvent, EvCommand>;

// ---- Scripts → simulação ----
// `did` (opcional): só aplica se o jogador com esse id ainda tiver esse did (o id pode ter sido reaproveitado).
struct CmdKick {
  uint16_t id = 0;
  std::string reason;
  std::string did;
};
struct CmdEmit {
  int32_t target = -1;  // id do jogador ou -1 = todos
  std::string name;
  std::string json;
};
struct CmdSetPosition {
  uint16_t id = 0;
  int32_t x = 0, y = 0, z = 0;
  std::string did;
};
using ScriptToSim = std::variant<CmdKick, CmdEmit, CmdSetPosition>;

struct ResourceStatus {
  std::string name;
  enum class State { Stopped, Running, Failed } state = State::Stopped;
  std::string lastError;
  double cpuMsPerSec = 0;  // tempo de parede dentro do Lua por segundo (última janela de 1 s)
  size_t memBytes = 0;
};

}  // namespace cbscript
