#include "cbserver/manifest.hpp"

#include <cbmp/safe_path.hpp>

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

// Caminho -> std::string UTF-8 sem lançar (generic_u8string não converte via locale).
std::string u8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

std::string lowerExt(const fs::path& p) {
  std::string e = u8(p.extension());
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

bool isNative(const std::string& ext) {
  static const std::set<std::string> kNative{".dll", ".exe", ".so",  ".dylib", ".bat", ".cmd",
                                             ".ps1", ".sh",  ".msi", ".scr",   ".com", ".jar", ".node"};
  return kNative.count(ext) > 0;
}

bool isWhitelisted(const std::string& ext, bool inUi) {
  static const std::set<std::string> kAlways{".lua",  ".html",  ".css", ".json", ".png", ".jpg", ".webp", ".svg",
                                             ".woff", ".woff2", ".ttf", ".ogg",  ".mp3", ".wav", ".webm", ".mp4"};
  if (ext == ".js") return inUi;
  return kAlways.count(ext) > 0;
}

bool safePath(const std::string& rel) { return cbmp::safeRelPath(rel); }

std::string hashFile(const fs::path& p, uint64_t& size) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Não foi possível ler o arquivo do resource: " + u8(p));
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
    spdlog::warn("Pasta de resources não encontrada: {}", u8(dir));
    return store;
  }

  std::vector<fs::path> resDirs;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_symlink(ec))
      throw std::runtime_error("Links simbólicos não são permitidos: " + u8(it->path()));
    if (it->is_directory(ec)) resDirs.push_back(it->path());
  }
  if (ec) throw std::runtime_error("Falha ao listar a pasta de resources: " + ec.message());
  std::sort(resDirs.begin(), resDirs.end());

  std::set<std::string> names;
  for (auto& rd : resDirs) {
    const std::string folder = u8(rd.filename());
    if (!safePath(folder))
      throw std::runtime_error("Nome de pasta de resource inválido (use apenas A-Z a-z 0-9 . _ -): " + folder);
    fs::path metaPath = rd / "resource.json";
    if (!fs::exists(metaPath)) {
      spdlog::warn("Pasta sem resource.json ignorada: {}", folder);
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
      throw std::runtime_error("resource.json inválido em '" + folder + "': " + ex.what());
    }
    if (!names.insert(entry.name).second)
      throw std::runtime_error("Resource duplicado: já existe um resource chamado '" + entry.name + "' (pasta '" +
                               folder + "')");

    std::vector<fs::path> files;
    std::error_code wec;
    fs::recursive_directory_iterator it(rd, fs::directory_options::none, wec), end;
    for (; !wec && it != end; it.increment(wec)) {
      if (it->is_symlink(wec))
        throw std::runtime_error("Resource '" + entry.name + "': links simbólicos não são permitidos: " +
                                 u8(it->path()));
      if (it->is_regular_file(wec)) files.push_back(it->path());
    }
    if (wec) throw std::runtime_error("Resource '" + entry.name + "': falha ao percorrer a pasta: " + wec.message());
    std::sort(files.begin(), files.end());

    for (auto& f : files) {
      if (isNative(lowerExt(f)))
        throw std::runtime_error("Resource '" + entry.name + "' contém arquivo nativo proibido: " +
                                 u8(fs::relative(f, rd)));
    }
    for (auto& f : files) {
      std::string rel = u8(fs::relative(f, rd));
      size_t slash = rel.find('/');
      std::string top = rel.substr(0, slash);
      bool published = slash != std::string::npos && (top == "client" || top == "ui" || top == "files");
      if (!published) {
        if (top != "server" && rel != "resource.json")
          spdlog::warn("Arquivo fora das pastas publicadas ignorado: {}/{}", entry.name, rel);
        continue;
      }
      if (!safePath(rel))
        throw std::runtime_error("Resource '" + entry.name + "': caminho publicado com nome não permitido "
                                 "(use apenas A-Z a-z 0-9 . _ -): " + rel);
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
