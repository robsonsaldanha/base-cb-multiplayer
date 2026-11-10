#include "cbserver/http.hpp"

#include <httplib.h>
#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "cbmp/messages.hpp"

namespace cbserver {
namespace {

void sendJson(httplib::Response& res, const nlohmann::json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}

bool isLoopback(const std::string& a) { return a == "127.0.0.1" || a == "::1" || a == "::ffff:127.0.0.1"; }

constexpr size_t kChunk = 64 * 1024;

struct FileStream {
  std::ifstream in;
  std::vector<char> buf = std::vector<char>(kChunk);
};

}  // namespace

bool metricsAllowed(const std::string& remoteAddr, const std::optional<std::string>& authHeader,
                    const std::optional<std::string>& token) {
  if (isLoopback(remoteAddr)) return true;
  if (!token || token->empty() || !authHeader) return false;
  const std::string expected = "Bearer " + *token;
  if (authHeader->size() != expected.size()) return false;
  return sodium_memcmp(authHeader->data(), expected.data(), expected.size()) == 0;
}

HttpServer::HttpServer(const ServerConfig& cfg, const ResourceStore& store, Metrics& metrics,
                       std::function<nlohmann::json()> info)
    : cfg_(cfg), store_(store), metrics_(metrics), info_(std::move(info)), svr_(std::make_unique<httplib::Server>()) {
  manifestBody_ =
      store_.manifestJson(info_().value("serverId", std::string()), cbmp::kProtocolVersion).dump();

  svr_->Get("/info", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, info_()); });

  svr_->Get("/cb/manifest", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(manifestBody_, "application/json");
  });

  svr_->Get(R"(/cb/files/([0-9a-fA-F]{64}))", [this](const httplib::Request& req, httplib::Response& res) {
    std::string sha = req.matches[1];
    std::transform(sha.begin(), sha.end(), sha.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto path = store_.fileBySha(sha);
    if (!path) {
      sendJson(res, {{"error", "not_found"}}, 404);
      return;
    }
    std::error_code ec;
    auto size = static_cast<size_t>(std::filesystem::file_size(*path, ec));
    auto st = std::make_shared<FileStream>();
    st->in.open(*path, std::ios::binary);
    if (ec || !st->in) {
      sendJson(res, {{"error", "not_found"}}, 404);
      return;
    }
    res.set_header("Cache-Control", "public, max-age=31536000, immutable");
    // No máximo 64 KiB por chamada, reutilizando o mesmo buffer.
    res.set_content_provider(size, "application/octet-stream",
                             [st](size_t offset, size_t length, httplib::DataSink& sink) {
                               st->in.clear();
                               st->in.seekg(static_cast<std::streamoff>(offset));
                               size_t want = std::min(length, kChunk);
                               st->in.read(st->buf.data(), static_cast<std::streamsize>(want));
                               auto n = static_cast<size_t>(st->in.gcount());
                               if (n == 0) return false;
                               return sink.write(st->buf.data(), n);
                             });
  });

  svr_->Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
    std::optional<std::string> auth;
    if (req.has_header("Authorization")) auth = req.get_header_value("Authorization");
    if (!metricsAllowed(req.remote_addr, auth, cfg_.metricsToken)) {
      sendJson(res, {{"error", "forbidden"}}, 403);
      return;
    }
    sendJson(res, metrics_.snapshot());
  });

  svr_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.status == 404 && res.body.empty()) sendJson(res, {{"error", "not_found"}}, 404);
  });
  svr_->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
    sendJson(res, {{"error", "internal"}}, 500);
  });
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start(const std::string& host, int port) {
  if (thread_.joinable()) throw std::runtime_error("Servidor HTTP já iniciado");
  if (port == 0) {
    boundPort_ = svr_->bind_to_any_port(host);
  } else {
    boundPort_ = svr_->bind_to_port(host, port) ? port : -1;
  }
  if (boundPort_ <= 0) throw std::runtime_error("Não foi possível abrir a porta HTTP em " + host);
  thread_ = std::thread([this] { svr_->listen_after_bind(); });
  svr_->wait_until_ready();
}

void HttpServer::stop() {
  if (svr_) svr_->stop();
  if (thread_.joinable()) thread_.join();
}

}  // namespace cbserver
