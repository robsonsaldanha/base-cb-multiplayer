#include "cbserver/hub.hpp"

#include <algorithm>
#include <array>
#include <exception>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cbmp/base64url.hpp>

namespace cbserver {
namespace {

std::string stripSlash(std::string u) {
  while (!u.empty() && u.back() == '/') u.pop_back();
  return u;
}

void configure(httplib::Client& c) {
  c.set_connection_timeout(5, 0);
  c.set_read_timeout(10, 0);
  c.set_write_timeout(10, 0);
  c.enable_server_certificate_verification(true);  // nunca desligar a verificação TLS
}

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  const auto b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Corta em `max` bytes sem partir um caractere UTF-8.
std::string clampUtf8(std::string s, size_t max) {
  if (s.size() <= max) return s;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  s.resize(n);
  return s;
}

}  // namespace

// ---------------- HubKeys ----------------

HubKeys::HubKeys(std::string hubUrl, std::function<double()> nowSec, std::chrono::milliseconds retryInterval)
    : url_(stripSlash(std::move(hubUrl))), now_(std::move(nowSec)), retry_(retryInterval) {
  haveKeys_ = fetchOnce();
  if (!haveKeys_) spdlog::error("hub: não foi possível carregar as chaves públicas; tentando de novo em background");
  th_ = std::thread([this] { loop(); });
}

HubKeys::~HubKeys() {
  {
    std::lock_guard lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

cbmp::PublicKeys HubKeys::keys() {
  std::lock_guard lk(m_);
  return keys_;
}

void HubKeys::requestRefresh() {
  {
    std::lock_guard lk(m_);
    if (now_() - lastFetchAt_ < kMinRefreshSec) return;
    refreshWanted_ = true;
  }
  cv_.notify_all();
}

bool HubKeys::fetchOnce() {
  ++fetches_;
  {
    std::lock_guard lk(m_);
    lastFetchAt_ = now_();
  }
  try {
    httplib::Client cli(url_);
    configure(cli);
    auto res = cli.Get("/v1/keys/public");
    if (!res) {
      spdlog::error("hub: falha ao buscar chaves ({})", httplib::to_string(res.error()));
      return false;
    }
    if (res->status != 200) {
      spdlog::error("hub: chaves públicas responderam HTTP {}", res->status);
      return false;
    }
    auto j = nlohmann::json::parse(res->body, nullptr, false);
    if (!j.is_array()) {
      spdlog::error("hub: resposta de chaves inválida");
      return false;
    }
    cbmp::PublicKeys fresh;
    for (const auto& e : j) {
      if (!e.is_object() || !e.contains("kid") || !e["kid"].is_string() || !e.contains("publicKey") ||
          !e["publicKey"].is_string()) {
        spdlog::warn("hub: entrada de chave malformada ignorada");
        continue;
      }
      const std::string kid = e["kid"];
      auto raw = cbmp::base64urlDecodeCanonical(e["publicKey"].get<std::string>());
      if (kid.empty() || !raw || raw->size() != 32) {
        spdlog::warn("hub: chave '{}' ignorada (não é base64url canônico de 32 bytes)", kid.substr(0, 64));
        continue;
      }
      std::array<uint8_t, 32> k{};
      std::copy(raw->begin(), raw->end(), k.begin());
      fresh[kid] = k;
    }
    if (fresh.empty()) {
      spdlog::error("hub: nenhuma chave pública válida na resposta");
      return false;
    }
    const size_t n = fresh.size();
    {
      std::lock_guard lk(m_);
      keys_ = std::move(fresh);
    }
    spdlog::info("hub: {} chave(s) pública(s) carregada(s)", n);
    return true;
  } catch (const std::exception& e) {
    spdlog::error("hub: erro ao buscar chaves: {}", e.what());
    return false;
  }
}

void HubKeys::loop() {
  std::unique_lock lk(m_);
  while (!stop_) {
    if (haveKeys_) {
      cv_.wait(lk, [this] { return stop_ || refreshWanted_; });
    } else if (!cv_.wait_for(lk, retry_, [this] { return stop_; })) {
      refreshWanted_ = true;
    }
    if (stop_) break;
    if (!refreshWanted_) continue;
    refreshWanted_ = false;
    lk.unlock();
    const bool ok = fetchOnce();
    lk.lock();
    if (ok) haveKeys_ = true;
  }
}

// ---------------- Heartbeat ----------------

Heartbeat::Heartbeat(std::string hubUrl, std::string key, HeartbeatInfo info, std::function<uint32_t()> players,
                     Options opt)
    : url_(stripSlash(std::move(hubUrl))),
      key_(std::move(key)),
      info_(std::move(info)),
      players_(std::move(players)),
      opt_(opt) {
  th_ = std::thread([this] { loop(); });
}

Heartbeat::~Heartbeat() { stop(); }

void Heartbeat::stop() {
  {
    std::lock_guard lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

bool Heartbeat::sendOnce() {
  ++attempts_;
  try {
    const int maxPlayers = std::clamp(info_.maxPlayers, 1, 2048);
    const int players = std::clamp<int>(static_cast<int>(players_ ? players_() : 0), 0, maxPlayers);
    nlohmann::json body{{"name", clampUtf8(trim(info_.name), 64)},
                        {"description", clampUtf8(info_.description, 500)},
                        {"players", players},
                        {"maxPlayers", maxPlayers},
                        {"gamePort", info_.gamePort},
                        {"httpPort", info_.httpPort},
                        {"version", info_.version},
                        {"protocol", info_.protocol},
                        {"tags", info_.tags},
                        {"locale", info_.locale}};
    if (!info_.adminUrl.empty()) body["adminUrl"] = info_.adminUrl;
    httplib::Client cli(url_);
    configure(cli);
    httplib::Headers h{{"Authorization", "Bearer " + key_}};
    auto res = cli.Post("/v1/servers/heartbeat", h,
                        body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    if (!res) {
      spdlog::warn("hub: heartbeat falhou ({})", httplib::to_string(res.error()));
      return false;
    }
    auto j = nlohmann::json::parse(res->body, nullptr, false);
    if (res->status != 200) {
      std::string code =
          j.is_object() && j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "";
      spdlog::warn("hub: heartbeat recusado (HTTP {} {})", res->status, code.substr(0, 40));
      return false;
    }
    if (j.is_object()) {
      const int listed = j.value("listed", false) ? 1 : 0;
      const std::string reason = j.contains("reason") && j["reason"].is_string() ? j["reason"].get<std::string>() : "";
      if (listed != lastListed_ || reason != lastReason_) {
        spdlog::info("hub: servidor {}{}{}", listed ? "listado" : "não listado", reason.empty() ? "" : " - ",
                     reason.substr(0, 80));
        lastListed_ = listed;
        lastReason_ = reason;
      }
      const std::string adminIgnored =
          j.contains("adminUrlIgnored") && j["adminUrlIgnored"].is_string() ? j["adminUrlIgnored"].get<std::string>() : "";
      if (adminIgnored != lastAdminIgnored_) {
        if (!adminIgnored.empty())
          spdlog::warn("hub: admin.publicUrl ignorada pelo Hub ({}); login de admin via Hub indisponível",
                       adminIgnored.substr(0, 40));
        lastAdminIgnored_ = adminIgnored;
      }
    }
    return true;
  } catch (const std::exception& e) {
    spdlog::warn("hub: erro no heartbeat: {}", e.what());
    return false;
  }
}

void Heartbeat::loop() {
  int failures = 0;
  std::unique_lock lk(m_);
  while (!stop_) {
    lk.unlock();
    const bool ok = sendOnce();
    lk.lock();
    auto delay = opt_.interval;
    if (ok) {
      failures = 0;
    } else {
      for (int i = 0; i < failures && delay < opt_.maxBackoff; ++i) delay *= 2;
      delay = std::min(delay, opt_.maxBackoff);
      ++failures;
    }
    lastDelayMs_ = delay.count();
    cv_.wait_for(lk, delay, [this] { return stop_; });
  }
}

}  // namespace cbserver
