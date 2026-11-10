#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "cbserver/config.hpp"
#include "cbserver/manifest.hpp"
#include "cbserver/metrics.hpp"

namespace httplib { class Server; }

namespace cbserver {

// 127.0.0.1/::1 sempre; senão só com "Bearer <token>" correto (comparação em tempo constante).
bool metricsAllowed(const std::string& remoteAddr, const std::optional<std::string>& authHeader,
                    const std::optional<std::string>& token);

class HttpServer {
 public:
  HttpServer(const ServerConfig& cfg, const ResourceStore& store, Metrics& metrics,
             std::function<nlohmann::json()> info);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // port 0 = porta efêmera (ver boundPort()). Lança std::runtime_error se não conseguir abrir.
  void start(const std::string& host, int port);
  // Escuta em IPv4 e IPv6 ao mesmo tempo: socket "::" com IPV6_V6ONLY=0 (Windows e Linux). Se não der, cai para
  // "0.0.0.0". Devolve o que ficou ativo: "dual-stack" ou "ipv4". Lança std::runtime_error se nada abrir.
  std::string startPublic(int port);
  void stop();
  int boundPort() const { return boundPort_; }

 private:
  const ServerConfig& cfg_;
  const ResourceStore& store_;
  Metrics& metrics_;
  std::function<nlohmann::json()> info_;
  std::unique_ptr<httplib::Server> svr_;
  std::thread thread_;
  int boundPort_ = 0;
  std::string manifestBody_;  // imutável: calculado uma vez
};

}  // namespace cbserver
