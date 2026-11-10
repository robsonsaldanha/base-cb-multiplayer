#pragma once
// Banco MySQL/MariaDB (seção "Banco" da spec).
//
// DbPool: N conexões persistentes, cada uma numa thread própria do pool. Quem pede (thread de scripts, BanStore)
// nunca espera: submit() só enfileira e `done` é chamado depois, numa thread do pool. Reconexão automática (com
// espera crescente enquanto o banco estiver fora: os pedidos falham na hora, sem pagar o timeout de conexão cada
// um), prazo por pedido (fila + execução), limites de resultado, métricas.
//
// Transações: submit(Begin) devolve o id da transação; pedidos com `tx` = esse id vão para a mesma conexão (fixada
// até Commit/Rollback). abortTx() faz rollback e solta a conexão. Transação sem comando por `txIdle` → rollback.
// No máximo max(1, pool - 1) transações abertas ao mesmo tempo (sobra conexão para consultas avulsas); um Begin
// além disso espera na fila.
//
// O backend real é a libmariadb (MariaDB Connector/C, LGPL, link dinâmico): mysqlConnectionFactory(). Os testes
// usam um DbConnection falso. A senha nunca vai para log, erro ou métrica (DbConfig::describe() não a inclui).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cbscript {

struct DbConfig {
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user, password, database;
  int pool = 4;          // conexões (= threads), 1..64
  int timeoutMs = 5000;  // prazo de cada pedido: fila + execução
  // TLS: Auto = Off para localhost/127.0.0.1/::1 e Verify para os demais; Off = sem TLS; Required = TLS sem
  // verificar o certificado; Verify = TLS com verificação do certificado (padrão da libmariadb 3.4).
  enum class Tls { Auto, Off, Required, Verify } tls = Tls::Auto;

  std::string describe() const;  // "user@host:port/database" (sem a senha)
};

// mysql://user:senha@host:porta/banco?tls=off|required|verify|auto&pool=N&timeoutMs=N (testes de integração).
// Percent-encoding (%XX) aceito em usuário e senha.
bool parseDbUrl(std::string_view url, DbConfig& out, std::string& err);

struct DbValue {
  enum class Type : uint8_t { Null, Int, Double, Text };
  Type type = Type::Null;
  int64_t i = 0;
  double d = 0;
  std::string s;  // Text: texto ou binário (BLOB), DECIMAL, datas ISO, inteiros sem sinal acima de 2^63-1

  static DbValue null() { return {}; }
  static DbValue integer(int64_t v) {
    DbValue x;
    x.type = Type::Int;
    x.i = v;
    return x;
  }
  static DbValue number(double v) {
    DbValue x;
    x.type = Type::Double;
    x.d = v;
    return x;
  }
  static DbValue text(std::string v) {
    DbValue x;
    x.type = Type::Text;
    x.s = std::move(v);
    return x;
  }
};

enum class DbOp : uint8_t { Query, Exec, Insert, Begin, Commit, Rollback };

struct DbRequest {
  DbOp op = DbOp::Query;
  std::string sql;              // vazio em Begin/Commit/Rollback
  std::vector<DbValue> params;  // um por '?'
  uint64_t tx = 0;              // transação (id devolvido pelo submit do Begin); 0 = fora de transação
};

struct DbResult {
  bool ok = false;
  bool timedOut = false;
  std::string error;  // mensagem (pt-BR, ou a do servidor); nunca a senha
  std::vector<std::string> columns;
  std::vector<std::vector<DbValue>> rows;
  int64_t affected = 0;
  int64_t insertId = 0;

  static DbResult fail(std::string e, bool timeout = false) {
    DbResult r;
    r.error = std::move(e);
    r.timedOut = timeout;
    return r;
  }
};

struct DbLimits {
  size_t maxRows = 10000;
  size_t maxBytes = size_t{8} << 20;  // texto + 16 bytes por valor
  size_t maxSql = 64 * 1024;
  size_t maxParams = 1000;
};

// Mensagem do erro de resultado grande demais (o backend e o pool usam a mesma).
std::string dbTooBig(const DbLimits& lim);

// Uma conexão. Só é usada pela thread do pool dona dela.
class DbConnection {
 public:
  virtual ~DbConnection() = default;
  virtual bool connected() const = 0;
  // (Re)conecta. false + err (sem senha) se não deu.
  virtual bool connect(std::string& err) = 0;
  virtual void close() = 0;
  // Executa o pedido (Begin/Commit/Rollback: START TRANSACTION/COMMIT/ROLLBACK). Perda de conexão → connected()
  // passa a ser false. Resultado acima de `lim` → erro dbTooBig (a conexão pode ser fechada para cortar o envio).
  virtual DbResult run(const DbRequest& r, const DbLimits& lim, std::chrono::milliseconds timeout) = 0;
  // Conexão ociosa há muito tempo: confere antes de usar. false → o pool reconecta.
  virtual bool ping() { return connected(); }
};
using DbConnectionFactory = std::function<std::unique_ptr<DbConnection>()>;

// Backend libmariadb (db_mysql.cpp).
DbConnectionFactory mysqlConnectionFactory(DbConfig cfg);

struct DbPoolOptions {
  DbLimits limits;
  std::chrono::milliseconds txIdle{10000};         // transação sem comando por isso → rollback
  std::chrono::milliseconds pingAfterIdle{30000};  // conexão ociosa por isso → ping antes de usar
  std::chrono::milliseconds reconnectMin{250}, reconnectMax{5000};  // espera entre tentativas de conexão
  std::chrono::milliseconds shutdownWait{3000};  // destrutor: espera as threads; depois as desanexa
  // Avisos do pool (conexão perdida/restabelecida...). Chamado das threads do pool. Vazio = spdlog.
  std::function<void(bool error, const std::string&)> log;
};

struct DbMetrics {
  uint64_t queries = 0;   // pedidos executados (inclui os com erro)
  uint64_t errors = 0;    // pedidos com erro (inclui tempo esgotado e banco fora)
  uint64_t timeouts = 0;  // tempo esgotado (fila ou execução)
  double qps = 0;         // consultas/s desde a leitura anterior de metrics()
  double p50Ms = 0, p99Ms = 0;  // latência de execução (últimos 1024 pedidos)
  int size = 0, busy = 0, connected = 0, activeTx = 0;
  size_t queued = 0;
};

namespace detail {
struct DbPoolState;
}

class DbPool {
 public:
  using Done = std::function<void(DbResult)>;
  DbPool(DbConfig cfg, DbConnectionFactory factory, DbPoolOptions opt = {});
  ~DbPool();
  DbPool(const DbPool&) = delete;
  DbPool& operator=(const DbPool&) = delete;

  // Enfileira; devolve o id (> 0). 0 + err: pool encerrado, transação desconhecida/encerrada, pedido inválido.
  // `done` é chamado uma vez, numa thread do pool (exceto se o pedido for cancelado antes de começar).
  uint64_t submit(DbRequest r, Done done, std::string& err);
  // Tira da fila sem chamar `done`. Já em execução: nada (o resultado chega e quem pediu o ignora).
  void cancel(uint64_t id);
  // Rollback da transação (aberta, ociosa ou ainda esperando conexão). Pedidos dela na fila falham.
  void abortTx(uint64_t tx);

  DbMetrics metrics() const;
  const DbConfig& config() const;
  const DbPoolOptions& options() const;

 private:
  std::shared_ptr<detail::DbPoolState> st_;  // compartilhado com as threads (sobrevive a um detach)
};

// ---- utilitários (testáveis sem banco) ----

// Marcadores '?' fora de literais ('...', "..."), identificadores (`...`) e comentários (-- , #, /* */).
// -1 = literal, identificador ou comentário sem fim.
int countPlaceholders(std::string_view sql);

// Data/hora do MySQL em texto ISO: DATE "AAAA-MM-DD", TIME "[-]HH:MM:SS[.ffffff]", DATETIME/TIMESTAMP
// "AAAA-MM-DDTHH:MM:SS[.ffffff]". `decimals` = casas de fração da coluna (0..6).
enum class DbTimeKind { Date, Time, DateTime };
std::string formatDbTime(DbTimeKind kind, bool negative, unsigned year, unsigned month, unsigned day, unsigned hour,
                         unsigned minute, unsigned second, unsigned long micro, unsigned decimals);

}  // namespace cbscript
