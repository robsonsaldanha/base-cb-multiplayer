#include "cbserver/manifest.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace cbserver {
namespace {

std::string lowerExt(const fs::path& p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

bool isNative(const std::string& ext) {
  static const std::set<std::string> kNative{".dll", ".exe", ".so",  ".dylib", ".bat", ".cmd",
                                             ".ps1", ".sh",  ".msi", ".scr",   ".com", ".jar", ".node"};
  return kNative.count(ext) > 0;
}

bool isWhitelisted(const std::string& ext, bool inUi) {
  static const std::set<std::string> kAlways{".lua",  ".html", ".css", ".json", ".png",  ".jpg",  ".webp", ".svg",
                                             ".woff", ".woff2", ".ttf", ".ogg", ".mp3",  ".wav",  ".webm", ".mp4"};
  if (ext == ".js") return inUi;
  return kAlways.count(ext) > 0;
}

std::string hashFile(const fs::path& p, uint64_t& size) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Não foi possível ler o arquivo do resource: " + p.string());
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  std::array<char, 65536> buf;
  size = 0;
  while (f) {
    f.read(buf.data(), buf.size());
    auto n = f.gcount();
    if (n > 0) {
      crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(buf.data()),
                                static_cast<unsigned long long>(n));
      size += static_cast<uint64_t>(n);
    }
  }
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&st, h);
  char hex[crypto_hash_sha256_BYTES * 2 + 1];
  sodium_bin2hex(hex, sizeof hex, h, sizeof h);
  return hex;
}

}  // namespace

ResourceStore ResourceStore::scan(const fs::path& dir) {
  ResourceStore store;
  if (sodium_init() < 0) throw std::runtime_error("Falha ao iniciar libsodium");
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    spdlog::warn("Pasta de resources não encontrada: {}", dir.string());
    return store;
  }

  std::vector<fs::path> resDirs;
  for (auto& e : fs::directory_iterator(dir)) {
    if (e.is_directory()) resDirs.push_back(e.path());
  }
  std::sort(resDirs.begin(), resDirs.end());

  for (auto& rd : resDirs) {
    fs::path metaPath = rd / "resource.json";
    if (!fs::exists(metaPath)) {
      spdlog::warn("Pasta sem resource.json ignorada: {}", rd.string());
      continue;
    }
    ResourceEntry entry;
    try {
      std::ifstream mf(metaPath);
      auto meta = nlohmann::json::parse(mf);
      if (!meta.is_object() || !meta.contains("name") || !meta["name"].is_string() || !meta.contains("version") ||
          !meta["version"].is_string() || meta["name"].get<std::string>().empty())
        throw std::runtime_error("faltam 'name' e 'version' (texto)");
      entry.name = meta["name"].get<std::string>();
      entry.version = meta["version"].get<std::string>();
    } catch (const std::exception& ex) {
      throw std::runtime_error("resource.json inválido em " + rd.string() + ": " + ex.what());
    }

    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(rd)) {
      if (e.is_regular_file()) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    for (auto& f : files) {
      std::string ext = lowerExt(f);
      if (isNative(ext))
        throw std::runtime_error("Resource '" + entry.name + "' contém arquivo nativo proibido: " + f.string());
    }
    for (auto& f : files) {
      std::string rel = fs::relative(f, rd).generic_string();
      std::string top = rel.substr(0, rel.find('/'));
      if (top != "client" && top != "ui" && top != "files") continue;
      if (rel.find('/') == std::string::npos) continue;
      if (!isWhitelisted(lowerExt(f), top == "ui")) {
        spdlog::warn("Arquivo fora da lista branca ignorado: {}/{}", entry.name, rel);
        continue;
      }
      ManifestFile mfile;
      mfile.path = rel;
      mfile.sha256 = hashFile(f, mfile.size);
      store.bySha_.emplace(mfile.sha256, f);
      entry.files.push_back(std::move(mfile));
    }
    store.resources_.push_back(std::move(entry));
  }
  return store;
}

nlohmann::json ResourceStore::manifestJson(const std::string& serverId, uint32_t protocol) const {
  nlohmann::json resources = nlohmann::json::array();
  uint64_t total = 0;
  for (auto& r : resources_) {
    nlohmann::json files = nlohmann::json::array();
    for (auto& f : r.files) {
      files.push_back({{"path", f.path}, {"sha256", f.sha256}, {"size", f.size}});
      total += f.size;
    }
    resources.push_back({{"name", r.name}, {"version", r.version}, {"files", files}});
  }
  return {{"serverId", serverId}, {"protocol", protocol}, {"resources", resources}, {"totalSize", total}};
}

std::optional<fs::path> ResourceStore::fileBySha(const std::string& sha) const {
  auto it = bySha_.find(sha);
  if (it == bySha_.end()) return std::nullopt;
  return it->second;
}

}  // namespace cbserver
