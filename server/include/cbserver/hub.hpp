#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cbserver/auth.hpp"

namespace cbserver {

// Chaves públicas do Hub (GET {hub}/v1/keys/public). Busca inicial bloqueante; falha -> tenta de novo em background.
class HubKeys : public KeyProvider {
 public:
  HubKeys(std::string hubUrl, std::function<double()> nowSec,
          std::chrono::milliseconds retryInterval = std::chrono::seconds(30));
  ~HubKeys() override;
  HubKeys(const HubKeys&) = delete;
  HubKeys& operator=(const HubKeys&) = delete;

  cbmp::PublicKeys keys() override;  // cópia sob mutex
  // Agenda recarga em background; no máximo 1x a cada 60 s (relógio injetado).
  void requestRefresh() override;
  int fetchCount() const { return fetches_.load(); }

  static constexpr double kMinRefreshSec = 60;

 private:
  bool fetchOnce();  // true se carregou ao menos uma chave válida
  void loop();

  std::string url_;
  std::function<double()> now_;
  std::chrono::milliseconds retry_;
  std::mutex m_;
  std::condition_variable cv_;
  cbmp::PublicKeys keys_;
  double lastFetchAt_ = -1e18;
  bool refreshWanted_ = false, stop_ = false, haveKeys_ = false;
  std::atomic<int> fetches_{0};
  std::thread th_;
};

struct HeartbeatInfo {
  std::string name, description;
  int maxPlayers = 512;
  int gamePort = 30500, httpPort = 30500;
  std::string version;
  int protocol = 1;
  std::vector<std::string> tags;
  std::string locale = "pt-BR";
};

// POST {hub}/v1/servers/heartbeat a cada `interval`; em erro, espera interval, 2x, 4x... até maxBackoff.
class Heartbeat {
 public:
  struct Options {
    std::chrono::milliseconds interval{30000}, maxBackoff{120000};
  };
  Heartbeat(std::string hubUrl, std::string key, HeartbeatInfo info, std::function<uint32_t()> players,
            Options opt = {});
  ~Heartbeat();
  Heartbeat(const Heartbeat&) = delete;
  Heartbeat& operator=(const Heartbeat&) = delete;

  void stop();
  int attempts() const { return attempts_.load(); }
  std::chrono::milliseconds lastDelay() const { return std::chrono::milliseconds(lastDelayMs_.load()); }

 private:
  bool sendOnce();
  void loop();

  std::string url_, key_;
  HeartbeatInfo info_;
  std::function<uint32_t()> players_;
  Options opt_;
  std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<int> attempts_{0};
  std::atomic<int64_t> lastDelayMs_{0};
  int lastListed_ = -1;  // -1 desconhecido
  std::string lastReason_;
  std::thread th_;
};

}  // namespace cbserver
