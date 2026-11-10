#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cbserver {

struct HubConfig {
  std::string url, serverId, key;
};

// sim.spawn: onde um jogador novo aparece. DevTrustFirstInput só em modo dev (devPublicKey e sem hub): o jogador
// fica pendente até o primeiro Input, cuja posição vira o spawn sem validação de movimento.
enum class SpawnMode { Origin, DevTrustFirstInput };

// db: MySQL/MariaDB dos scripts (cb.db) e dos bans. Ausente/null = sem banco (bans em scripting.bansFile).
// Erros de validação nunca repetem a senha.
struct DbSettings {
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user, password, database;
  int pool = 4;           // conexões (1..64)
  int timeoutMs = 5000;   // por consulta (100..600000)
  std::string tls = "auto";  // auto | off | required | verify
};

// admin: painel admin do servidor. publicUrl (anunciada no heartbeat como `adminUrl`) é normalizada para uma ORIGEM
// (scheme://host[:porta]): https obrigatório, exceto loopback (127.0.0.0/8, ::1, localhost) em dev.
// O painel só sobe se houver quem possa entrar: `admins` (ids Discord, 17 a 20 dígitos) ou `devToken` (só em modo
// dev: auth.devPublicKey sem hub; ≥ 32 caracteres).
struct AdminSettings {
  std::optional<std::string> publicUrl;
  int port = 30510;
  std::string bind = "127.0.0.1";  // IP literal
  std::vector<std::string> admins;
  std::optional<std::string> devToken;
  std::vector<std::string> trustedProxies;  // IPs/CIDRs cujo X-Forwarded-For é aceito (padrão: nenhum)
  bool enabled() const { return !admins.empty() || devToken.has_value(); }
};

// Bind loopback (127.0.0.0/8, ::1).
bool isLoopbackBind(std::string_view ip);

// Normaliza a origem do painel admin (mesmas regras de normalizeAdminOrigin em @cbmp/contracts): sem caminho
// (exceto "/"), query, fragmento, credenciais, espaços/controles ou não-ASCII; ≤ 256 chars; esquema/host em
// minúsculas, porta padrão removida. nullopt se inválida.
std::optional<std::string> normalizeAdminOrigin(std::string_view raw);

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
  std::optional<DbSettings> db;
  AdminSettings admin;

  // Lançam std::runtime_error (pt-BR: campo + motivo).
  static ServerConfig load(const std::filesystem::path& path);
  static ServerConfig fromJson(const nlohmann::json& j);
};

}  // namespace cbserver
