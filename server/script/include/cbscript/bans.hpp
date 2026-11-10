#pragma once
// BanStore: bans do servidor. Provisório (Task 4): só o arquivo bans.json; a Task 5 acrescenta o MySQL.
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cbscript {

struct Ban {
  std::string did, reason, by;  // by = quem baniu ("resource:<nome>", "admin:<did>"...)
  int64_t createdAt = 0;        // Unix (s)
  int64_t expiresAt = 0;        // Unix (s); 0 = permanente
};

// Threads: check() é chamado pela simulação (na entrada do jogador) e só segura o mutex por uma busca O(1).
// add/remove são da thread de scripts (único escritor): atualizam o mapa sob o mutex (O(1)) e regravam o arquivo
// fora dele, a partir da cópia do escritor.
class BanStore {
 public:
  // Lê `file` se existir (lista JSON de {did, reason, by, createdAt, expiresAt}). Arquivo malformado → lança
  // std::runtime_error (pt-BR). Caminho vazio = só memória (testes).
  explicit BanStore(std::filesystem::path file);

  // Ban em vigor para `did` em `nowUnix` (expirados não contam).
  std::optional<Ban> check(const std::string& did, int64_t nowUnix) const;

  // Grava (substitui o ban anterior do mesmo did). false + `err` se o arquivo não pôde ser gravado (o ban vale em
  // memória mesmo assim).
  bool add(Ban b, std::string& err);
  // Remove; `removed` diz se havia ban. false + `err` só em falha de gravação.
  bool remove(const std::string& did, bool& removed, std::string& err);

  std::vector<Ban> list() const;  // ordenado por did

 private:
  bool persist(std::string& err);

  std::filesystem::path file_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Ban> live_;    // sob mu_ (lido pela simulação)
  std::unordered_map<std::string, Ban> writer_;  // só a thread de scripts (fonte da gravação)
};

}  // namespace cbscript
