#include "cbserver/http.hpp"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace cbserver {
namespace {

void sendJson(httplib::Response& res, const nlohmann::json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}

bool isLoopback(const std::string& a) { return a == "127.0.0.1" || a == "::1" || a == "::ffff:127.0.0.1"; }

}  // namespace

HttpServer::HttpServer(const ServerConfig& cfg, const ResourceStore& store, Metrics& metrics,
                       std::function<nlohmann::json()> info)
    : cfg_(cfg), store_(store), metrics_(metrics), info_(std::move(info)), svr_(std::make_unique<httplib::Server>()) {
  svr_->Get("/info", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, info_()); });

  svr_->Get("/cb/manifest", [this](const httplib::Request&, httplib::Response& res) {
    std::string id = info_().value("serverId", std::string());
    sendJson(res, store_.manifestJson(id, 1));
  });

  svr_->Get(R"(/cb/files/([0-9a-fA-F]{64}))", [this](const httplib::Request& req, httplib::Response& res) {
    std::string sha = req.matches[1];
    std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto path = store_.fileBySha(sha);
    if (!path) {
      sendJson(res, {{"error", "not_found"}}, 404);
      return;
    }
    std::error_code ec;
    auto size = static_cast<size_t>(std::filesystem::file_size(*path, ec));
    if (ec) {
      sendJson(res, {{"error", "not_found"}}, 404);
      return;
    }
    res.set_header("Cache-Control", "public, max-age=31536000, immutable");
    auto in = std::make_shared<std::ifstream>(*path, std::ios::binary);
    res.set_content_provider(size, "application/octet-stream",
                             [in](size_t offset, size_t length, httplib::DataSink& sink) {
                               in->clear();
                               in->seekg(static_cast<std::streamoff>(offset));
                               std::string buf(length, '\0');
                               in->read(buf.data(), static_cast<std::streamsize>(length));
                               auto n = static_cast<size_t>(in->gcount());
                               if (n == 0) return false;
                               sink.write(buf.data(), n);
                               return true;
                             });
  });

  svr_->Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
    bool ok = isLoopback(req.remote_addr);
    if (!ok && cfg_.metricsToken && !cfg_.metricsToken->empty())
      ok = req.get_header_value("Authorization") == "Bearer " + *cfg_.metricsToken;
    if (!ok) {
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
