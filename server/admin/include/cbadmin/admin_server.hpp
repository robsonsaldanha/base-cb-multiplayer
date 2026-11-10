#pragma once
// Painel admin embutido no cb-server (spec sub3, "Painel admin" e "Login via Hub").
//
// Threads: o HTTP do painel roda num pool próprio (separado do HTTP do jogo, da simulação e da thread de scripts).
// Nada aqui toca o World: leituras vêm da foto publicada pelo tick (ScriptHost::players) e do status publicado do
// host; ações vão pelas filas existentes (canal prioritário da ScriptBridge para kick/teleporte/kick por did,
// ScriptHost::{start,stop,restart}Resource, EvCommand do console, bans pela thread de scripts). Respostas de ações
// assíncronas podem ser 202.
//
// Login (modo Hub): GET /auth/start gera `state` (32 bytes, cookie HttpOnly SameSite=Strict Path=/auth 10 min) e
// redireciona ao Hub; o Hub volta para <publicUrl>/auth/callback#ticket=…&state=…; o JS do painel manda
// POST /auth/login {ticket, state}. O servidor confere o state (cookie == corpo, tempo constante, uso único), o
// ticket (verifyTicket com aud "<serverId>#admin"), o jti (cache próprio, TTL ≥ o do ticket) e o did em `admins`.
// Modo dev: POST /auth/login {devToken} (só com auth.devPublicKey; tempo constante).
//
// Segurança em toda resposta: CSP estrita, X-Frame-Options DENY, Referrer-Policy no-referrer, Cache-Control
// no-store, nosniff. Host fora da lista (origem de publicUrl; nomes de loopback quando o bind é loopback ou curinga)
// → 421 em qualquer rota (DNS rebinding). Mutações: Origin igual a uma origem do painel (ausente/null → 403),
// cabeçalho X-CB-Admin: 1, Content-Type application/json, corpo ≤ 64 KiB. X-Forwarded-* nunca é usado.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cbscript/messages.hpp"

namespace cbserver {
class KeyProvider;
class ScriptBridge;
}  // namespace cbserver
namespace cbscript {
class ScriptHost;
class BanStore;
}  // namespace cbscript

namespace cbadmin {

struct AdminOptions {
  std::string bind = "127.0.0.1";
  int port = 30510;  // 0 = porta efêmera (testes; ver AdminServer::boundPort)
  std::optional<std::string> publicUrl;  // origem normalizada (normalizeAdminOrigin)
  std::vector<std::string> admins;       // ids Discord
  std::optional<std::string> devToken;   // só em modo dev (validado na config)
  std::string serverId;                  // aud dos tickets = serverId + "#admin"
  std::string hubUrl;                    // vazio = sem Hub (login só por devToken)
  std::filesystem::path logsDir = "logs";
  std::string dbPassword;  // linhas de log que a contenham nunca são devolvidas
  int threads = 4;

  double sessionAbsoluteSec = 8 * 3600.0, sessionIdleSec = 3600.0;
  size_t maxSessionsPerDid = 5, maxSessions = 1024;
  double stateTtlSec = 600;
  size_t maxPendingStates = 256;
  size_t maxJti = 10000;
  // Login (POST /auth/login e GET /auth/start): por IP (IPv6 agrupado por /64) e global, por minuto.
  int loginPerIpPerMin = 10, loginGlobalPerMin = 60;
  size_t maxRateKeys = 10000;
  // API por sessão (token bucket): requisições/s e rajada; console à parte.
  double apiPerSec = 20, apiBurst = 40, consolePerSec = 1, consoleBurst = 5;
  std::chrono::milliseconds actionWait{3000};  // espera por start/stop/restart antes de responder 202
  std::chrono::milliseconds consoleWait{2000};  // espera pela resposta de um comando de script
};

enum class ResourceOp { Start, Stop, Restart };

// O que o painel consome do servidor. Todas as funções são chamadas das threads do painel e não podem bloquear a
// simulação. Vazias = recurso indisponível (503).
struct AdminBackend {
  cbserver::KeyProvider* keys = nullptr;
  std::function<int64_t()> unixNow;   // tickets e auditoria (vazio = system_clock)
  std::function<double()> monoNow;    // sessões e limites, em segundos (vazio = steady_clock)
  std::function<cbscript::PlayersSnapshot()> players;
  std::function<bool(cbscript::ScriptToSim)> simCommand;  // kick / kick por did / teleporte (canal prioritário)
  std::function<std::vector<cbscript::ResourceStatus>()> resources;
  std::function<std::future<bool>(ResourceOp, std::string)> resourceOp;
  std::function<bool(cbscript::EvCommand)> scriptCommand;  // console → comandos registrados dos resources
  // Grava o ban (assíncrono, pela thread de scripts) e expulsa quem tiver o did. false = indisponível agora.
  std::function<bool(const std::string& did, const std::string& reason, int64_t durationS, const std::string& by)> ban;
  std::function<nlohmann::json()> metrics;
  std::function<void(const std::string&)> audit;  // uma linha JSONL (sem '\n')
};

// Ligação padrão com o servidor real (main.cpp e testes de integração).
AdminBackend makeBackend(cbscript::ScriptHost& host, cbserver::ScriptBridge& bridge,
                         std::shared_ptr<cbscript::BanStore> bans, cbserver::KeyProvider& keys,
                         std::function<nlohmann::json()> metrics, std::function<void(const std::string&)> audit,
                         std::function<int64_t()> unixNow);

class AdminServer {
 public:
  AdminServer(AdminOptions opt, AdminBackend be);
  ~AdminServer();
  AdminServer(const AdminServer&) = delete;
  AdminServer& operator=(const AdminServer&) = delete;

  void start();  // lança std::runtime_error se não abrir a porta
  void stop();
  int boundPort() const;

  // Diagnóstico/testes.
  size_t sessionCount() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace cbadmin
