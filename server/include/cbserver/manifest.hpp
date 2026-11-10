#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace cbserver {

struct ManifestFile {
  std::string path, sha256;
  uint64_t size = 0;
};

struct ResourceEntry {
  std::string name, version;
  std::vector<ManifestFile> files;
};

class ResourceStore {
 public:
  // Lança std::runtime_error se houver arquivo nativo ou resource.json inválido.
  static ResourceStore scan(const std::filesystem::path& dir);
  nlohmann::json manifestJson(const std::string& serverId, uint32_t protocol) const;
  std::optional<std::filesystem::path> fileBySha(const std::string& sha) const;
  const std::vector<ResourceEntry>& resources() const { return resources_; }

 private:
  std::vector<ResourceEntry> resources_;
  std::unordered_map<std::string, std::filesystem::path> bySha_;
};

}  // namespace cbserver
