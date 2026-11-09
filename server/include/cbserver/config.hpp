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
  double speedIdle = 50, speedWalk = 400, speedRun = 900, speedBroom = 4000, speedSwim = 400, speedFall = 6000,
         speedApparate = 0;
  double tolerance = 0.2;
  int eventsPerSecond = 20, eventsBurst = 40;
  std::optional<std::string> metricsToken;
  std::string resourcesDir = "resources";
  std::string logsDir = "logs";

  // Lançam std::runtime_error (pt-BR: campo + motivo).
  static ServerConfig load(const std::filesystem::path& path);
  static ServerConfig fromJson(const nlohmann::json& j);
};

}  // namespace cbserver
