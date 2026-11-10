// Integração com MariaDB/MySQL real. Só roda com CBMP_TEST_MYSQL definido (no CI: service mariadb:11), por exemplo
//   CBMP_TEST_MYSQL=mysql://root:test@127.0.0.1:3306/cbtest
// Sem a variável, cada caso é pulado (SKIP). Tabelas com nome aleatório, apagadas no fim; dids aleatórios em cb_bans.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <future>
#include <optional>
#include <random>

#include "api_env.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/db.hpp"

using namespace cbscript;

namespace {

std::string envVar(const char* name) {
#if defined(_MSC_VER)
  char* v = nullptr;
  size_t len = 0;
  if (_dupenv_s(&v, &len, name) != 0 || v == nullptr) return {};
  std::string s(v);
  std::free(v);
  return s;
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

std::optional<DbConfig> mysqlCfg() {
  const std::string url = envVar("CBMP_TEST_MYSQL");
  if (url.empty()) return std::nullopt;
  DbConfig c;
  std::string err;
  if (!parseDbUrl(url, c, err)) FAIL("CBMP_TEST_MYSQL inválido: " << err);
  return c;
}

#define NEED_MYSQL(cfg)                                                        \
  auto cfg##_opt = mysqlCfg();                                                 \
  if (!cfg##_opt) SKIP("CBMP_TEST_MYSQL não definido (integração MySQL pulada)"); \
  DbConfig cfg = *cfg##_opt

std::string suffix() {
  std::random_device rd;
  return std::to_string(rd()) + std::to_string(rd() % 100000);
}

DbRequest req(DbOp op, std::string sql = {}, std::vector<DbValue> params = {}, uint64_t tx = 0) {
  DbRequest r;
  r.op = op;
  r.sql = std::move(sql);
  r.params = std::move(params);
  r.tx = tx;
  return r;
}

struct Sync {
  uint64_t id = 0;
  DbResult res;
};

// Pedido síncrono (para o teste) com prazo rígido.
Sync run(DbPool& pool, DbRequest r) {
  auto p = std::make_shared<std::promise<DbResult>>();
  auto f = p->get_future();
  std::string err;
  Sync s;
  s.id = pool.submit(std::move(r), [p](DbResult x) { p->set_value(std::move(x)); }, err);
  if (s.id == 0) {
    s.res = DbResult::fail("submit: " + err);
    return s;
  }
  s.res = guarded("mysql request", [&f] { return f.get(); }, std::chrono::seconds(30));
  return s;
}

DbResult must(DbPool& pool, DbRequest r) {
  const std::string sql = r.sql;
  Sync s = run(pool, std::move(r));
  INFO("SQL: " << sql << " → " << s.res.error);
  REQUIRE(s.res.ok);
  return s.res;
}

DbPoolOptions quiet() {
  DbPoolOptions o;
  o.log = [](bool, const std::string&) {};
  return o;
}

// Apaga a tabela no fim do caso, mesmo se uma asserção falhar.
struct DropTable {
  DbPool& pool;
  std::string name;
  ~DropTable() { (void)run(pool, req(DbOp::Exec, "DROP TABLE IF EXISTS " + name)); }
};

int colIndex(const DbResult& r, const std::string& name) {
  for (size_t i = 0; i < r.columns.size(); ++i)
    if (r.columns[i] == name) return static_cast<int>(i);
  return -1;
}

}  // namespace

TEST_CASE("mysql: CRUD and type mapping (integers, unsigned, float, DECIMAL, dates, BLOB, NULL)") {
  NEED_MYSQL(cfg);
  DbPool pool(cfg, mysqlConnectionFactory(cfg), quiet());
  const std::string t = "cbt_types_" + suffix();
  DropTable drop{pool, t};
  must(pool, req(DbOp::Exec,
                 "CREATE TABLE " + t +
                     " (id INT AUTO_INCREMENT PRIMARY KEY, i BIGINT, u BIGINT UNSIGNED, ti TINYINT, f FLOAT, "
                     "d DOUBLE, dc DECIMAL(10,2), s VARCHAR(50), b BLOB, dt DATETIME, dt6 DATETIME(6), da DATE, "
                     "tm TIME, y YEAR, n INT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"));
  const std::string blob("\0\x01\xff" "abc", 6);
  DbResult ins = must(
      pool, req(DbOp::Insert,
                "INSERT INTO " + t +
                    " (i, u, ti, f, d, dc, s, b, dt, dt6, da, tm, y, n) VALUES (?, 18446744073709551615, ?, ?, ?, ?, ?, "
                    "?, ?, ?, ?, ?, ?, ?)",
                {DbValue::integer(-9007199254740993LL), DbValue::integer(-5), DbValue::number(1.5),
                 DbValue::number(2.25), DbValue::text("12.34"), DbValue::text("ação ✓"), DbValue::text(blob),
                 DbValue::text("2024-01-02 03:04:05"), DbValue::text("2024-01-02 03:04:05.123456"),
                 DbValue::text("2024-01-02"), DbValue::text("-01:02:03"), DbValue::integer(2024), DbValue::null()}));
  REQUIRE(ins.insertId == 1);
  REQUIRE(ins.affected == 1);
  must(pool, req(DbOp::Insert, "INSERT INTO " + t + " (i, s) VALUES (?, ?)", {DbValue::integer(7), DbValue::text("b")}));

  DbResult q = must(pool, req(DbOp::Query, "SELECT * FROM " + t + " WHERE id = ?", {DbValue::integer(1)}));
  REQUIRE(q.rows.size() == 1);
  const auto& row = q.rows[0];
  auto col = [&](const char* name) -> const DbValue& {
    const int i = colIndex(q, name);
    REQUIRE(i >= 0);
    return row[static_cast<size_t>(i)];
  };
  REQUIRE(col("id").type == DbValue::Type::Int);
  REQUIRE(col("i").i == -9007199254740993LL);
  REQUIRE(col("u").type == DbValue::Type::Text);  // acima de 2^63-1: texto
  REQUIRE(col("u").s == "18446744073709551615");
  REQUIRE((col("ti").type == DbValue::Type::Int && col("ti").i == -5));
  REQUIRE((col("f").type == DbValue::Type::Double && col("f").d == 1.5));
  REQUIRE((col("d").type == DbValue::Type::Double && col("d").d == 2.25));
  REQUIRE((col("dc").type == DbValue::Type::Text && col("dc").s == "12.34"));
  REQUIRE(col("s").s == "ação ✓");
  REQUIRE(col("b").s == blob);
  REQUIRE(col("dt").s == "2024-01-02T03:04:05");
  REQUIRE(col("dt6").s == "2024-01-02T03:04:05.123456");
  REQUIRE(col("da").s == "2024-01-02");
  REQUIRE(col("tm").s == "-01:02:03");
  REQUIRE((col("y").type == DbValue::Type::Int && col("y").i == 2024));
  REQUIRE(col("n").type == DbValue::Type::Null);

  DbResult upd = must(pool, req(DbOp::Exec, "UPDATE " + t + " SET s = ? WHERE i = ?", {DbValue::text("c"), DbValue::integer(7)}));
  REQUIRE(upd.affected == 1);
  DbResult all = must(pool, req(DbOp::Query, "SELECT id, s FROM " + t + " ORDER BY id"));
  REQUIRE(all.rows.size() == 2);
  REQUIRE(all.rows[1][1].s == "c");
  DbResult del = must(pool, req(DbOp::Exec, "DELETE FROM " + t + " WHERE id = ?", {DbValue::integer(2)}));
  REQUIRE(del.affected == 1);
  DbResult none = must(pool, req(DbOp::Query, "SELECT id FROM " + t + " WHERE id = ?", {DbValue::integer(2)}));
  REQUIRE(none.rows.empty());
  DbResult nul = must(pool, req(DbOp::Query, "SELECT ? AS a, ? AS b", {DbValue::null(), DbValue::text("x")}));
  REQUIRE(nul.rows[0][0].type == DbValue::Type::Null);
  REQUIRE(nul.rows[0][1].s == "x");
  auto m = pool.metrics();
  REQUIRE(m.queries == 9);  // CREATE, 2 INSERT, SELECT *, UPDATE, SELECT, DELETE, SELECT, SELECT ? (o DROP vem depois)
  REQUIRE(m.connected >= 1);
}

TEST_CASE("mysql: transactions roll back and commit on a dedicated connection") {
  NEED_MYSQL(cfg);
  DbPool pool(cfg, mysqlConnectionFactory(cfg), quiet());
  const std::string t = "cbt_tx_" + suffix();
  DropTable drop{pool, t};
  must(pool, req(DbOp::Exec, "CREATE TABLE " + t + " (id INT PRIMARY KEY) ENGINE=InnoDB"));
  Sync b = run(pool, req(DbOp::Begin));
  REQUIRE(b.res.ok);
  must(pool, req(DbOp::Insert, "INSERT INTO " + t + " VALUES (?)", {DbValue::integer(1)}, b.id));
  DbResult inside = must(pool, req(DbOp::Query, "SELECT COUNT(*) AS c FROM " + t, {}, b.id));
  REQUIRE(inside.rows[0][0].i == 1);
  must(pool, req(DbOp::Rollback, {}, {}, b.id));
  REQUIRE(must(pool, req(DbOp::Query, "SELECT COUNT(*) FROM " + t)).rows[0][0].i == 0);

  Sync c = run(pool, req(DbOp::Begin));
  REQUIRE(c.res.ok);
  must(pool, req(DbOp::Insert, "INSERT INTO " + t + " VALUES (?)", {DbValue::integer(2)}, c.id));
  must(pool, req(DbOp::Commit, {}, {}, c.id));
  REQUIRE(must(pool, req(DbOp::Query, "SELECT COUNT(*) FROM " + t)).rows[0][0].i == 1);

  // abortTx → rollback.
  Sync d = run(pool, req(DbOp::Begin));
  REQUIRE(d.res.ok);
  must(pool, req(DbOp::Insert, "INSERT INTO " + t + " VALUES (?)", {DbValue::integer(3)}, d.id));
  pool.abortTx(d.id);
  REQUIRE(waitUntil([&] { return pool.metrics().activeTx == 0; }, std::chrono::milliseconds(10000)));
  REQUIRE(must(pool, req(DbOp::Query, "SELECT COUNT(*) FROM " + t)).rows[0][0].i == 1);
}

TEST_CASE("mysql: a slow query times out and the pool keeps working") {
  NEED_MYSQL(cfg);
  cfg.timeoutMs = 1000;
  DbPool pool(cfg, mysqlConnectionFactory(cfg), quiet());
  must(pool, req(DbOp::Query, "SELECT 1"));
  auto t0 = std::chrono::steady_clock::now();
  Sync s = run(pool, req(DbOp::Query, "SELECT SLEEP(5) AS z"));
  const auto el = std::chrono::steady_clock::now() - t0;
  INFO("ok=" << s.res.ok << " err=" << s.res.error);
  REQUIRE_FALSE(s.res.ok);
  REQUIRE(s.res.timedOut);
  REQUIRE(el < std::chrono::milliseconds(4500));
  REQUIRE(waitUntil([&] { return run(pool, req(DbOp::Query, "SELECT 1")).res.ok; }, std::chrono::milliseconds(10000)));
  REQUIRE(pool.metrics().timeouts >= 1);
}

TEST_CASE("mysql: SQL errors and unreachable servers give clear errors without the password") {
  NEED_MYSQL(cfg);
  {
    DbPool pool(cfg, mysqlConnectionFactory(cfg), quiet());
    Sync s = run(pool, req(DbOp::Query, "SELEC 1"));
    REQUIRE_FALSE(s.res.ok);
    REQUIRE_FALSE(s.res.error.empty());
    Sync m = run(pool, req(DbOp::Query, "SELECT * FROM cbt_nao_existe_" + suffix()));
    REQUIRE_FALSE(m.res.ok);
    REQUIRE(must(pool, req(DbOp::Query, "SELECT 1")).rows.size() == 1);
  }
  {
    DbConfig bad = cfg;
    bad.host = "127.0.0.1";
    bad.port = 1;  // nada ouvindo
    bad.timeoutMs = 2000;
    DbPool pool(bad, mysqlConnectionFactory(bad), quiet());
    auto t0 = std::chrono::steady_clock::now();
    Sync s = run(pool, req(DbOp::Query, "SELECT 1"));
    REQUIRE_FALSE(s.res.ok);
    REQUIRE(contains(s.res.error, "sem conexão com o banco"));
    if (!cfg.password.empty()) REQUIRE_FALSE(contains(s.res.error, cfg.password));
    REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
  }
  {
    DbConfig wrong = cfg;
    wrong.password = "senha-errada-" + suffix();
    DbPool pool(wrong, mysqlConnectionFactory(wrong), quiet());
    Sync s = run(pool, req(DbOp::Query, "SELECT 1"));
    REQUIRE_FALSE(s.res.ok);
    REQUIRE_FALSE(contains(s.res.error, wrong.password));
  }
}

TEST_CASE("mysql: cb.db from Lua with permissions.db, refused without it") {
  NEED_MYSQL(cfg);
  ApiEnv env;
  auto pool = std::make_shared<DbPool>(cfg, mysqlConnectionFactory(cfg), quiet());
  env.api.db = pool;
  env.addRes("comdb", R"(
    cb.on('resourceStart', function()
      local rows = cb.db.query("SELECT 1 AS um, NULL AS nada, CAST(1.5 AS DECIMAL(4,2)) AS dc, 'ok' AS s, ? AS p", 'x')
      cb.log.info('um=' .. rows[1].um .. ' ' .. math.type(rows[1].um) .. ' nada=' .. tostring(rows[1].nada == cb.null) ..
                  ' dc=' .. rows[1].dc .. ' s=' .. rows[1].s .. ' p=' .. rows[1].p)
      local r = cb.db.transaction(function(tx) return tx.query('SELECT 2 AS dois')[1].dois end)
      cb.log.info('tx=' .. r)
    end)
  )", {}, nlohmann::json::object(), nlohmann::json{{"db", true}});
  env.addRes("semdb", R"(
    cb.on('resourceStart', function()
      local ok, e = pcall(cb.db.query, 'SELECT 1')
      cb.log.info('semdb ' .. tostring(ok) .. ' ' .. tostring(e))
    end)
  )");
  env.boot();
  REQUIRE(waitUntil([&] { return env.hasLog("[comdb] tx=2"); }, std::chrono::milliseconds(15000)));
  INFO(env.allLogs());
  REQUIRE(env.hasLog("[comdb] um=1 integer nada=true dc=1.50 s=ok p=x"));
  REQUIRE(env.waitLog("[semdb] semdb false"));
  REQUIRE(env.hasLog("permissions.db"));
}

TEST_CASE("mysql: bans persist in cb_bans and keep the player out after a reload") {
  NEED_MYSQL(cfg);
  auto pool = std::make_shared<DbPool>(cfg, mysqlConnectionFactory(cfg), quiet());
  const std::string did = "cbt_did_" + suffix();
  BanStoreDbOptions bo;
  bo.log = [](bool, const std::string&) {};
  auto now = [] {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  };
  {
    BanStore s(pool, now, bo);
    REQUIRE(s.flush(std::chrono::milliseconds(15000)));
    std::string err;
    REQUIRE(s.add(Ban{did, "trapaça", "admin:teste", now(), 0}, err));
    REQUIRE(s.add(Ban{did + "x", "expira", "admin:teste", now() - 100, now() - 10}, err));
    REQUIRE(s.flush(std::chrono::milliseconds(15000)));
  }
  {
    BanStore s(pool, now, bo);
    REQUIRE(s.flush(std::chrono::milliseconds(15000)));
    REQUIRE(s.loaded());
    auto b = s.check(did, now());
    REQUIRE(b);  // a admissão do NetServer recusa com "banido: trapaça"
    REQUIRE(b->reason == "trapaça");
    REQUIRE(b->by == "admin:teste");
    REQUIRE_FALSE(s.check(did + "x", now()));
    bool removed = false;
    std::string err;
    REQUIRE(s.remove(did, removed, err));
    REQUIRE(removed);
    REQUIRE(s.flush(std::chrono::milliseconds(15000)));
  }
  BanStore s(pool, now, bo);
  REQUIRE(s.flush(std::chrono::milliseconds(15000)));
  REQUIRE_FALSE(s.check(did, now()));
}

TEST_CASE("mysql: TLS with certificate verification (informative)") {
  NEED_MYSQL(cfg);
  cfg.tls = DbConfig::Tls::Verify;
  DbPool pool(cfg, mysqlConnectionFactory(cfg), quiet());
  Sync s = run(pool, req(DbOp::Query, "SELECT 1"));
  if (s.res.ok)
    SUCCEED("TLS com verificação funcionou");
  else
    WARN("TLS com verificação não conectou (esperado se o servidor não tiver certificado verificável): " << s.res.error);
}
