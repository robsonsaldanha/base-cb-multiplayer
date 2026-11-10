#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cbserver {

struct HubConfig {
  std::string url, serverId, key;
};

// sim.spawn: onde um jogador novo aparece. DevTrustFirstInput só em modo dev (devPublicKey e sem hub): o jogador
// fica pendente até o primeiro Input, cuja posição vira o spawn sem validação de movimento.
enum class SpawnMode { Origin, DevTrustFirstInput };

struct ServerConfig {
  std::string name = "Meu servidor";
  std::string description;
  std::vector<std::string> tags{"rp"};
  std::string locale = "pt-BR";
  int maxPlayers = 512;
  int port = 30500;
  std::optional<HubConfig> hub;
  std::optional<std::string> devPublicKey;  // base64url de 32 bytes
  int pendingPerIp = 4, pendingGlobal = 256, maxOutKbpsPerClient = 256;
  int tickHz = 20;
  double streamRadiusM = 300;
  int maxVisible = 128;
  SpawnMode spawnMode = SpawnMode::Origin;
  double speedIdle = 50, speedWalk = 400, speedRun = 900, speedBroom = 4000, speedSwim = 400, speedFall = 6000,
         speedApparate = 0;
  double tolerance = 0.2;
  int eventsPerSecond = 20, eventsBurst = 40;
  std::optional<std::string> metricsToken;
  std::string resourcesDir = "resources";
  std::string logsDir = "logs";
  // scripting: limites por resource Lua (memória por estado, aviso e aborto da vigia) e arquivo de bans.
  int scriptMemoryLimitMB = 256, scriptWarnMs = 50, scriptAbortMs = 5000;
  std::string bansFile = "bans.json";
  // Cota por jogador de eventos servidor → cliente vindos de scripts (bytes de nome + payload).
  // Padrão 8 KiB/s = 64 kbps (alvo de banda por jogador da spec), rajada de 32 KiB.
  double scriptEventOutKBps = 8, scriptEventOutBurstKB = 32;

  // Lançam std::runtime_error (pt-BR: campo + motivo).
  static ServerConfig load(const std::filesystem::path& path);
  static ServerConfig fromJson(const nlohmann::json& j);
};

}  // namespace cbserver
