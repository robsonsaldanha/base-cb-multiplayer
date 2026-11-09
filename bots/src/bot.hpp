#pragma once
#include <array>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace cbbots {

struct BotOptions {
  std::string host = "127.0.0.1";
  uint16_t port = 30500;
  std::array<uint8_t, 32> seed{};   // seed Ed25519 da chave dev (assina os tickets)
  std::string serverId = "srv_dev";  // aud dos tickets
  int count = 16;
  double areaM = 2000;      // lado do quadrado (centrado na origem) onde andam os bots espalhados
  double cluster = 0.3;     // fração de bots num círculo de 30 m no centro
  double durationS = 30;    // duração total (inclui a rampa de conexões)
  double rampPerSec = 50;   // conexões abertas por segundo
  double teleportEvery = 0; // > 0: bot 0 é um trapaceiro persistente que teleporta a cada N s
  bool flood = false;       // bot 0 manda 500 eventos/s
  uint64_t rngSeed = 1;
};

// Ticket assinado com a seed dev, no formato do Hub: base64url(JSON compacto) "." base64url(assinatura).
// JSON na ordem v,kid,sub,did,name,aud,iat,exp,jti (kid "dev", exp = iat + 300).
std::string makeTicket(const std::array<uint8_t, 32>& seed, int index, const std::string& aud, int64_t iat,
                       const std::string& jti);
// Chave pública (base64url, 32 bytes raw) da seed — o valor de auth.devPublicKey do servidor.
std::string devPublicKeyB64(const std::array<uint8_t, 32>& seed);

// Roda o cenário e devolve o relatório:
// {connected, failedToConnect, rejected, disconnected, kicked, maxSnapshotGapMs, p99SnapshotGapMs, avgRttMs,
//  bytesIn, durationS, ...extras}. Bot 0 de cenário (teleporte/flood) fica fora das contagens e das estatísticas
// de snapshot; o resultado dele vai em "scenarioBot".
nlohmann::json runBots(const BotOptions& opt);

}  // namespace cbbots
