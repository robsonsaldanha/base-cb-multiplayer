#pragma once
// BanStore: bans do servidor, num cache em memória com um de dois backends de gravação:
// - json: arquivo bans.json (ou só memória, com caminho vazio);
// - mysql: tabela cb_bans(did, reason, created_at, expires_at, created_by), criada se faltar.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cbscript {

class DbPool;

struct Ban {
  std::string did, reason, by;  // by = quem baniu ("resource:<nome>", "admin:<did>"...)
  int64_t createdAt = 0;        // Unix (s)
  int64_t expiresAt = 0;        // Unix (s); 0 = permanente
};

struct BanStoreDbOptions {
  // Espera entre tentativas (carga inicial e gravações) enquanto o banco estiver fora: dobra até o máximo.
  std::chrono::milliseconds retryMin{1000}, retryMax{30000};
  size_t maxPendingWrites = 100000;  // acima disso a gravação mais antiga é descartada (log de erro)
  // Avisos/erros (thread própria do backend). Vazio = spdlog.
  std::function<void(bool error, const std::string&)> log;
  // Linha JSONL de auditoria quando uma gravação é descartada por erro permanente ({"type":"ban_write_dropped"...}).
  std::function<void(const std::string&)> audit;
};

// Threads: check() é chamado pela simulação (na entrada do jogador) e só segura o mutex por uma busca O(1) no cache;
// a simulação nunca faz I/O. add/remove têm UM escritor só (a thread de scripts; o painel da Task 7/8 também passa
// por ela) e nunca esperam o banco.
// - json: atualizam o cache e regravam o arquivo (temporário com flush + fsync, depois rename) na hora. Expirados são
//   descartados ao carregar e ao gravar.
// - mysql: atualizam o cache e enfileiram a gravação (REPLACE/DELETE) para uma thread própria, que grava em ordem e
//   tenta de novo enquanto o banco estiver fora. A carga inicial (CREATE TABLE IF NOT EXISTS, limpeza dos
//   expirados, SELECT dos vigentes) roda nessa thread, com novas tentativas; até ela terminar, só valem os bans
//   gravados nesta execução. Mudanças locais feitas antes da carga terminar têm precedência sobre o que veio do
//   banco.
class BanStore {
 public:
  // json: lê `file` se existir (lista JSON de {did, reason, by, createdAt, expiresAt}). Arquivo malformado → lança
  // std::runtime_error (pt-BR). Caminho vazio = só memória (testes). `nowUnix`: relógio (vazio = system_clock).
  explicit BanStore(std::filesystem::path file, std::function<int64_t()> nowUnix = {});
  // mysql (não bloqueia: a carga é assíncrona; ver loaded()).
  BanStore(std::shared_ptr<DbPool> db, std::function<int64_t()> nowUnix = {}, BanStoreDbOptions opt = {});
  ~BanStore();  // mysql: tenta esvaziar a fila de gravações por até 2 s; o que sobrar é registrado no log
  BanStore(const BanStore&) = delete;
  BanStore& operator=(const BanStore&) = delete;

  // Ban em vigor para `did` em `nowUnix` (expirados não contam).
  std::optional<Ban> check(const std::string& did, int64_t nowUnix) const;

  // Grava (substitui o ban anterior do mesmo did). false + `err` se o arquivo não pôde ser gravado (o ban vale em
  // memória mesmo assim). mysql: sempre true (a gravação é assíncrona; falhas vão para o log).
  bool add(Ban b, std::string& err);
  // Remove; `removed` diz se havia ban (no cache). false + `err` só em falha de gravação do arquivo.
  bool remove(const std::string& did, bool& removed, std::string& err);

  std::vector<Ban> list() const;  // ordenado por did

  const char* backend() const;  // "json", "memória" ou "mysql"
  bool loaded() const;          // mysql: carga inicial concluída (json: sempre true)
  size_t pendingWrites() const;  // mysql: gravações ainda não confirmadas pelo banco
  // mysql: espera a carga inicial e as gravações pendentes até `limit`. true se terminou tudo.
  bool flush(std::chrono::milliseconds limit);

  struct Backend;

 private:
  int64_t now() const;
  void merge(std::vector<Ban> loaded);  // mysql: carga inicial (thread do backend)

  std::function<int64_t()> now_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Ban> live_;   // sob mu_ (lido pela simulação)
  std::unordered_set<std::string> touched_;     // sob mu_: dids mudados antes da carga inicial (mysql)
  bool loaded_ = true;                          // sob mu_
  std::unique_ptr<Backend> be_;
};

}  // namespace cbscript
