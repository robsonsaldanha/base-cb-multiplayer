#include "cbscript/bans.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

namespace cbscript {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string u8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

Ban fromJson(const json& j, const std::string& where) {
  auto fail = [&](const char* why) { return std::runtime_error("bans: arquivo inválido '" + where + "': " + why); };
  if (!j.is_object()) throw fail("cada ban deve ser um objeto");
  Ban b;
  if (!j.contains("did") || !j["did"].is_string() || j["did"].get<std::string>().empty())
    throw fail("'did' deve ser texto não vazio");
  b.did = j["did"].get<std::string>();
  if (j.contains("reason")) {
    if (!j["reason"].is_string()) throw fail("'reason' deve ser texto");
    b.reason = j["reason"].get<std::string>();
  }
  if (j.contains("by")) {
    if (!j["by"].is_string()) throw fail("'by' deve ser texto");
    b.by = j["by"].get<std::string>();
  }
  for (const char* k : {"createdAt", "expiresAt"}) {
    if (!j.contains(k)) continue;
    if (!j[k].is_number_integer()) throw fail("'createdAt'/'expiresAt' devem ser inteiros (Unix s)");
    (k[0] == 'c' ? b.createdAt : b.expiresAt) = j[k].get<int64_t>();
  }
  return b;
}

}  // namespace

BanStore::BanStore(fs::path file) : file_(std::move(file)) {
  if (file_.empty()) return;
  std::error_code ec;
  if (!fs::exists(file_, ec)) return;
  std::ifstream f(file_, std::ios::binary);
  if (!f) throw std::runtime_error("bans: não foi possível ler '" + u8(file_) + "'");
  json j = json::parse(f, nullptr, false);
  if (j.is_discarded()) throw std::runtime_error("bans: JSON malformado em '" + u8(file_) + "'");
  if (!j.is_array()) throw std::runtime_error("bans: '" + u8(file_) + "' deve conter uma lista");
  for (const auto& e : j) {
    Ban b = fromJson(e, u8(file_));
    writer_[b.did] = b;
    live_[b.did] = std::move(b);
  }
}

std::optional<Ban> BanStore::check(const std::string& did, int64_t nowUnix) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = live_.find(did);
  if (it == live_.end()) return std::nullopt;
  if (it->second.expiresAt != 0 && nowUnix >= it->second.expiresAt) return std::nullopt;
  return it->second;
}

bool BanStore::add(Ban b, std::string& err) {
  writer_[b.did] = b;
  {
    std::lock_guard<std::mutex> lk(mu_);
    live_[b.did] = std::move(b);
  }
  return persist(err);
}

bool BanStore::remove(const std::string& did, bool& removed, std::string& err) {
  removed = writer_.erase(did) > 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    live_.erase(did);
  }
  return removed ? persist(err) : true;
}

std::vector<Ban> BanStore::list() const {
  std::vector<Ban> v;
  {
    std::lock_guard<std::mutex> lk(mu_);
    v.reserve(live_.size());
    for (const auto& [d, b] : live_) v.push_back(b);
  }
  std::sort(v.begin(), v.end(), [](const Ban& a, const Ban& b) { return a.did < b.did; });
  return v;
}

bool BanStore::persist(std::string& err) {
  if (file_.empty()) return true;
  std::vector<const Ban*> v;
  v.reserve(writer_.size());
  for (const auto& [d, b] : writer_) v.push_back(&b);
  std::sort(v.begin(), v.end(), [](const Ban* a, const Ban* b) { return a->did < b->did; });
  json arr = json::array();
  for (const Ban* b : v)
    arr.push_back({{"did", b->did}, {"reason", b->reason}, {"by", b->by}, {"createdAt", b->createdAt},
                   {"expiresAt", b->expiresAt}});
  // Grava num temporário e troca (o arquivo nunca fica pela metade).
  fs::path tmp = file_;
  tmp += ".tmp";
  std::error_code ec;
  if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      err = "não foi possível gravar '" + u8(tmp) + "'";
      return false;
    }
    f << arr.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
    if (!f) {
      err = "falha ao gravar '" + u8(tmp) + "'";
      return false;
    }
  }
  fs::rename(tmp, file_, ec);
  if (ec) {
    err = "não foi possível substituir '" + u8(file_) + "': " + ec.message();
    return false;
  }
  return true;
}

}  // namespace cbscript
