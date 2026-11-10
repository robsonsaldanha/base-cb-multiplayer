// DbPool e cb.db contra um banco falso (sem MariaDB): fila, threads, prazos, reconexão, transações, mapeamento de
// tipos, permissões, cancelamento no stop. A integração com MariaDB real fica em test_db_mysql.cpp.
#include <catch2/catch_test_macros.hpp>

#include <future>
#include <set>
#include <thread>

#include "api_env.hpp"
#include "cbscript/db.hpp"
#include "fake_db.hpp"

using namespace cbscript;

namespace {

struct Waiter {
  std::shared_ptr<std::promise<DbResult>> p = std::make_shared<std::promise<DbResult>>();
  std::shared_future<DbResult> f = p->get_future().share();
  DbPool::Done done() {
    auto pp = p;
    return [pp](DbResult r) { pp->set_value(std::move(r)); };
  }
  bool ready(std::chrono::milliseconds w = std::chrono::milliseconds(0)) const {
    return f.wait_for(w) == std::future_status::ready;
  }
  DbResult get() const {
    return guarded("db result", [f = f] { return f.get(); });
  }
};

DbRequest req(DbOp op, std::string sql = {}, uint64_t tx = 0, std::vector<DbValue> params = {}) {
  DbRequest r;
  r.op = op;
  r.sql = std::move(sql);
  r.tx = tx;
  r.params = std::move(params);
  return r;
}

DbResult sync(DbPool& pool, DbRequest r) {
  Waiter w;
  std::string err;
  const uint64_t id = pool.submit(std::move(r), w.done(), err);
  if (id == 0) return DbResult::fail("submit: " + err);
  return w.get();
}

std::shared_ptr<FakeDb> rowsDb() {
  auto db = std::make_shared<FakeDb>();
  db->handler = [](const DbRequest& r) {
    DbResult res;
    res.ok = true;
    if (r.sql.rfind("SELECT", 0) == 0) {
      res.columns = {"id", "name"};
      res.rows.push_back({DbValue::integer(1), DbValue::text("ana")});
    }
    res.affected = 1;
    return res;
  };
  return db;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ utilitários

TEST_CASE("db: placeholders are counted outside literals, identifiers and comments") {
  REQUIRE(countPlaceholders("") == 0);
  REQUIRE(countPlaceholders("SELECT ?") == 1);
  REQUIRE(countPlaceholders("SELECT '?', \"?\", `?` FROM t WHERE a = ? AND b = ?") == 2);
  REQUIRE(countPlaceholders("SELECT 'it''s ?', 'a\\'?', \"x\"\"?\", `a``?` , ?") == 1);
  REQUIRE(countPlaceholders("SELECT ? -- ?\n, ? # ?\n, /* ? */ ?") == 3);
  REQUIRE(countPlaceholders("SELECT 1--?") == 1);  // "--" sem espaço não é comentário
  REQUIRE(countPlaceholders("SELECT ? --") == 1);   // "--" no fim é comentário
  REQUIRE(countPlaceholders("SELECT 'abc") == -1);
  REQUIRE(countPlaceholders("SELECT /* x") == -1);
  REQUIRE(countPlaceholders("SELECT `x") == -1);
  REQUIRE(countPlaceholders("SELECT \"x\\\"") == -1);
}

TEST_CASE("db: dates and times are formatted as ISO text") {
  REQUIRE(formatDbTime(DbTimeKind::Date, false, 2024, 1, 2, 0, 0, 0, 0, 0) == "2024-01-02");
  REQUIRE(formatDbTime(DbTimeKind::DateTime, false, 2024, 1, 2, 3, 4, 5, 0, 0) == "2024-01-02T03:04:05");
  REQUIRE(formatDbTime(DbTimeKind::DateTime, false, 2024, 12, 31, 23, 59, 58, 120000, 3) == "2024-12-31T23:59:58.120");
  REQUIRE(formatDbTime(DbTimeKind::DateTime, false, 2024, 1, 2, 3, 4, 5, 123456, 6) == "2024-01-02T03:04:05.123456");
  REQUIRE(formatDbTime(DbTimeKind::Time, true, 0, 0, 0, 838, 59, 59, 0, 0) == "-838:59:59");
  REQUIRE(formatDbTime(DbTimeKind::Time, false, 0, 0, 0, 1, 2, 3, 5, 6) == "01:02:03.000005");
  REQUIRE(formatDbTime(DbTimeKind::Date, false, 0, 0, 0, 0, 0, 0, 0, 0) == "0000-00-00");
}

TEST_CASE("db: connection URL parsing keeps the password out of describe()") {
  DbConfig c;
  std::string err;
  REQUIRE(parseDbUrl("mysql://root:te%40st@127.0.0.1:3307/cbtest?tls=off&pool=2&timeoutMs=1500", c, err));
  REQUIRE(c.user == "root");
  REQUIRE(c.password == "te@st");
  REQUIRE(c.host == "127.0.0.1");
  REQUIRE(c.port == 3307);
  REQUIRE(c.database == "cbtest");
  REQUIRE(c.tls == DbConfig::Tls::Off);
  REQUIRE(c.pool == 2);
  REQUIRE(c.timeoutMs == 1500);
  REQUIRE(c.describe() == "root@127.0.0.1:3307/cbtest");
  REQUIRE(c.describe().find("te@st") == std::string::npos);

  DbConfig d;
  REQUIRE(parseDbUrl("mysql://u@h/db", d, err));
  REQUIRE(d.port == 3306);
  REQUIRE(d.password.empty());
  REQUIRE(d.tls == DbConfig::Tls::Auto);
  REQUIRE_FALSE(parseDbUrl("http://u@h/db", d, err));
  REQUIRE_FALSE(parseDbUrl("mysql://u@h", d, err));
  REQUIRE_FALSE(parseDbUrl("mysql://u@h:abc/db", d, err));
  REQUIRE_FALSE(parseDbUrl("mysql://u@h/db?tls=talvez", d, err));
}

// ------------------------------------------------------------------------------------------------------- pool

TEST_CASE("db pool: requests run on the pool threads, results come back and metrics count them") {
  auto db = rowsDb();
  std::set<std::thread::id> threads;
  std::mutex tmu;
  db->handler = [&, h = db->handler](const DbRequest& r) {
    {
      std::lock_guard<std::mutex> lk(tmu);
      threads.insert(std::this_thread::get_id());
    }
    return h(r);
  };
  DbPool pool(fakeCfg(2), fakeFactory(db), fastPoolOpt());
  std::vector<Waiter> ws(20);
  for (auto& w : ws) {
    std::string err;
    REQUIRE(pool.submit(req(DbOp::Query, "SELECT 1"), w.done(), err) > 0);
  }
  for (auto& w : ws) {
    DbResult r = w.get();
    REQUIRE(r.ok);
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.rows[0][1].s == "ana");
  }
  {
    std::lock_guard<std::mutex> lk(tmu);
    REQUIRE_FALSE(threads.count(std::this_thread::get_id()));
    REQUIRE(threads.size() >= 1);
    REQUIRE(threads.size() <= 2);
  }
  auto m = pool.metrics();
  REQUIRE(m.queries == 20);
  REQUIRE(m.errors == 0);
  REQUIRE(m.size == 2);
  REQUIRE(m.connected >= 1);
  REQUIRE(m.busy == 0);
  REQUIRE(m.p99Ms >= m.p50Ms);
}

TEST_CASE("db pool: a request that outlived its deadline in the queue fails without running") {
  auto db = rowsDb();
  db->blockOn = "SLEEP";
  DbPool pool(fakeCfg(1, 150), fakeFactory(db), fastPoolOpt());
  Waiter a, b;
  std::string err;
  REQUIRE(pool.submit(req(DbOp::Query, "SELECT SLEEP(1)"), a.done(), err) > 0);
  REQUIRE(pool.submit(req(DbOp::Query, "SELECT 2"), b.done(), err) > 0);
  DbResult ra = a.get(), rb = b.get();
  REQUIRE_FALSE(ra.ok);
  REQUIRE(ra.timedOut);
  REQUIRE_FALSE(rb.ok);
  REQUIRE(rb.timedOut);
  REQUIRE(contains(rb.error, "tempo esgotado"));
  REQUIRE(db->count("SELECT 2") == 0);  // nunca chegou ao banco
  auto m = pool.metrics();
  REQUIRE(m.timeouts == 2);
  REQUIRE(m.errors == 2);
}

TEST_CASE("db pool: reconnects after a lost connection; while the database is down requests fail fast") {
  auto db = rowsDb();
  db->setDown(true);
  auto opt = fastPoolOpt();
  opt.reconnectMin = opt.reconnectMax = std::chrono::milliseconds(1000);  // folga para o 2º pedido cair na espera
  DbPool pool(fakeCfg(1, 5000), fakeFactory(db), opt);
  auto t0 = std::chrono::steady_clock::now();
  DbResult r1 = sync(pool, req(DbOp::Query, "SELECT 1"));
  REQUIRE_FALSE(r1.ok);
  REQUIRE(contains(r1.error, "sem conexão com o banco"));
  REQUIRE(contains(r1.error, "Can't connect"));
  REQUIRE_FALSE(contains(r1.error, "segredo-super"));
  DbResult r2 = sync(pool, req(DbOp::Query, "SELECT 1"));  // dentro da espera: falha sem tentar de novo
  REQUIRE_FALSE(r2.ok);
  REQUIRE(contains(r2.error, "indisponível"));
  REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
  {
    std::lock_guard<std::mutex> lk(db->mu);
    REQUIRE(db->connectFails == 1);
  }
  db->setDown(false);
  REQUIRE(waitUntil([&] { return sync(pool, req(DbOp::Query, "SELECT 1")).ok; }));
  {
    std::lock_guard<std::mutex> lk(db->mu);
    REQUIRE(db->connects == 1);
    db->loseNext = true;
  }
  DbResult lost = sync(pool, req(DbOp::Query, "SELECT 1"));
  REQUIRE_FALSE(lost.ok);
  REQUIRE(sync(pool, req(DbOp::Query, "SELECT 1")).ok);  // reconecta sozinho
  {
    std::lock_guard<std::mutex> lk(db->mu);
    REQUIRE(db->connects == 2);
  }
  REQUIRE(pool.metrics().errors >= 3);
}

TEST_CASE("db pool: transactions pin one connection; commit, rollback, abort and idle timeout release it") {
  auto db = rowsDb();
  auto opt = fastPoolOpt();
  opt.txIdle = std::chrono::milliseconds(150);
  DbPool pool(fakeCfg(2), fakeFactory(db), opt);
  std::string err;

  // Begin → id da transação; os pedidos dela vão para a mesma conexão.
  Waiter b1;
  const uint64_t t1 = pool.submit(req(DbOp::Begin), b1.done(), err);
  REQUIRE(t1 > 0);
  REQUIRE(b1.get().ok);
  REQUIRE(sync(pool, req(DbOp::Exec, "UPDATE a SET x = 1", t1)).ok);
  REQUIRE(sync(pool, req(DbOp::Insert, "INSERT INTO a VALUES (1)", t1)).ok);
  REQUIRE(pool.metrics().activeTx == 1);
  // Com 2 conexões, só 1 transação por vez: o 2º Begin espera o commit do 1º.
  Waiter b2;
  const uint64_t t2 = pool.submit(req(DbOp::Begin), b2.done(), err);
  REQUIRE(t2 > 0);
  REQUIRE(sync(pool, req(DbOp::Query, "SELECT avulso")).ok);  // consultas avulsas seguem
  REQUIRE_FALSE(b2.ready(std::chrono::milliseconds(50)));
  REQUIRE(sync(pool, req(DbOp::Commit, {}, t1)).ok);
  REQUIRE(b2.get().ok);
  auto ops = db->snapshot();
  int conn = -1;
  for (auto& o : ops)
    if (o.sql == "BEGIN") {
      conn = o.conn;
      break;
    }
  for (auto& o : ops)
    if (o.sql == "UPDATE a SET x = 1" || o.sql == "INSERT INTO a VALUES (1)" || o.sql == "COMMIT") REQUIRE(o.conn == conn);
  // Pedido de transação encerrada: recusado no submit.
  REQUIRE(pool.submit(req(DbOp::Exec, "UPDATE z", t1), [](DbResult) {}, err) == 0);
  REQUIRE(contains(err, "transação"));
  // abortTx → ROLLBACK.
  pool.abortTx(t2);
  REQUIRE(waitUntil([&] { return db->count("ROLLBACK") == 1; }));
  REQUIRE(waitUntil([&] { return pool.metrics().activeTx == 0; }));
  // Ociosa por mais de txIdle → ROLLBACK.
  Waiter b3;
  const uint64_t t3 = pool.submit(req(DbOp::Begin), b3.done(), err);
  REQUIRE(b3.get().ok);
  REQUIRE(waitUntil([&] { return db->count("ROLLBACK") == 2; }));
  REQUIRE(waitUntil([&] { return pool.metrics().activeTx == 0; }));  // o ROLLBACK é gravado antes de soltar
  REQUIRE(pool.submit(req(DbOp::Exec, "UPDATE late", t3), [](DbResult) {}, err) == 0);
  // Begin ainda na fila e abortado: nunca roda.
  db->blockOn = "SLEEP";
  Waiter s1, s2;
  REQUIRE(pool.submit(req(DbOp::Query, "SELECT SLEEP(1)"), s1.done(), err) > 0);
  REQUIRE(pool.submit(req(DbOp::Query, "SELECT SLEEP(2)"), s2.done(), err) > 0);
  REQUIRE(waitUntil([&] { return db->blockedNow() == 2; }));
  Waiter b4;
  const uint64_t t4 = pool.submit(req(DbOp::Begin), b4.done(), err);
  pool.abortTx(t4);
  db->release();
  REQUIRE(s1.get().ok);
  REQUIRE(s2.get().ok);
  REQUIRE(sync(pool, req(DbOp::Query, "SELECT fim")).ok);
  REQUIRE_FALSE(b4.ready());
  REQUIRE(db->count("BEGIN") == 3);
}

TEST_CASE("db pool: invalid requests are refused at submit") {
  auto db = rowsDb();
  DbPool pool(fakeCfg(1), fakeFactory(db), fastPoolOpt());
  std::string err;
  REQUIRE(pool.submit(req(DbOp::Query, ""), [](DbResult) {}, err) == 0);
  REQUIRE(pool.submit(req(DbOp::Exec, "UPDATE x", 999), [](DbResult) {}, err) == 0);
  REQUIRE(contains(err, "transação"));
  REQUIRE(pool.submit(req(DbOp::Commit, {}, 0), [](DbResult) {}, err) == 0);
  REQUIRE(pool.submit(req(DbOp::Query, std::string(70 * 1024, ' ') + "SELECT 1"), [](DbResult) {}, err) == 0);
  REQUIRE(contains(err, "SQL"));
  REQUIRE(pool.submit(req(DbOp::Query, "SELECT ?"), [](DbResult) {}, err) == 0);
  REQUIRE(contains(err, "parâmetro"));
  REQUIRE(pool.submit(req(DbOp::Query, "SELECT 'x"), [](DbResult) {}, err) == 0);
}

TEST_CASE("db pool: results over the row limit are refused with a clear error") {
  auto db = std::make_shared<FakeDb>();
  db->handler = [](const DbRequest&) {
    DbResult r;
    r.ok = true;
    r.columns = {"n"};
    for (int i = 0; i < 11; ++i) r.rows.push_back({DbValue::integer(i)});
    return r;
  };
  auto opt = fastPoolOpt();
  opt.limits.maxRows = 10;
  DbPool pool(fakeCfg(1), fakeFactory(db), opt);
  DbResult r = sync(pool, req(DbOp::Query, "SELECT n"));
  REQUIRE_FALSE(r.ok);
  REQUIRE(contains(r.error, "grande demais"));
}

TEST_CASE("db pool: destroying the pool with a stuck connection does not hang") {
  auto db = rowsDb();
  db->blockOn = "SLEEP";
  db->honorTimeout = false;
  auto pool = std::make_unique<DbPool>(fakeCfg(1, 100), fakeFactory(db), fastPoolOpt());
  Waiter a, b;
  std::string err;
  REQUIRE(pool->submit(req(DbOp::Query, "SELECT SLEEP(99)"), a.done(), err) > 0);
  REQUIRE(pool->submit(req(DbOp::Query, "SELECT depois"), b.done(), err) > 0);
  REQUIRE(waitUntil([&] { return db->blockedNow() == 1; }));
  auto t0 = std::chrono::steady_clock::now();
  guarded("pool destroy", [&pool] {
    pool.reset();
    return 0;
  });
  REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
  DbResult rb = b.get();  // o que estava na fila falha com "encerrado"
  REQUIRE_FALSE(rb.ok);
  REQUIRE(contains(rb.error, "encerrado"));
  db->release();  // a thread desanexada termina sozinha
  REQUIRE(a.get().ok);
}

// --------------------------------------------------------------------------------------------------- cb.db (API)

namespace {

const nlohmann::json kDbPerm = {{"db", true}};

struct DbEnv : ApiEnv {
  std::shared_ptr<FakeDb> fdb = std::make_shared<FakeDb>();
  std::shared_ptr<DbPool> pool;
  explicit DbEnv(int timeoutMs = 5000, DbPoolOptions po = fastPoolOpt()) {
    pool = std::make_shared<DbPool>(fakeCfg(2, timeoutMs), fakeFactory(fdb), po);
    api.db = pool;
  }
  ~DbEnv() {
    fdb->release();
    if (host)
      guarded("destroy", [this] {
        host.reset();
        return 0;
      });
  }
};

DbResult typedRows(const DbRequest& r) {
  DbResult res;
  res.ok = true;
  if (r.sql.rfind("SELECT n", 0) == 0) {
    res.columns = {"n"};
    res.rows.push_back({DbValue::integer(5)});
  } else if (r.sql.rfind("SELECT", 0) == 0) {
    res.columns = {"id", "name", "score", "nada", "preco"};
    res.rows.push_back({DbValue::integer(1), DbValue::text("ana"), DbValue::number(1.5), DbValue::null(),
                        DbValue::text("10.50")});
    res.rows.push_back({DbValue::integer(2), DbValue::text(std::string("bi\0a", 4)), DbValue::number(2),
                        DbValue::null(), DbValue::text("0.01")});
  } else if (r.sql.rfind("UPDATE", 0) == 0) {
    res.affected = 3;
  } else if (r.sql.rfind("INSERT", 0) == 0) {
    res.insertId = 42;
    res.affected = 1;
  }
  return res;
}

}  // namespace

TEST_CASE("api: cb.db.query/exec/insert map values and resume the callback") {
  DbEnv env;
  env.fdb->handler = typedRows;
  env.addRes("a", R"(
    cb.on('resourceStart', function()
      local rows = cb.db.query('SELECT * FROM t WHERE a = ? AND b = ? AND c = ? AND d = ? AND e = ? AND f = ?',
                               7, 2.5, 'x', nil, true, cb.null)
      cb.log.info('n=' .. #rows .. ' id=' .. rows[1].id .. ' ' .. math.type(rows[1].id) .. ' name=' .. rows[1].name ..
                  ' score=' .. rows[1].score .. ' ' .. math.type(rows[1].score) .. ' null=' ..
                  tostring(rows[1].nada == cb.null) .. ' preco=' .. rows[1].preco .. ' ' .. type(rows[1].preco) ..
                  ' len2=' .. #rows[2].name)
      cb.log.info('upd=' .. cb.db.exec('UPDATE t SET a = ?', 1))
      cb.log.info('ins=' .. cb.db.insert('INSERT INTO t VALUES (?)', false))
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(env.waitLog("[a] ins=42"));
  INFO(env.allLogs());
  REQUIRE(env.hasLog("[a] n=2 id=1 integer name=ana score=1.5 float null=true preco=10.50 string len2=4"));
  REQUIRE(env.hasLog("[a] upd=3"));
  auto ops = env.fdb->snapshot();
  REQUIRE(ops.size() == 3);
  REQUIRE(ops[0].op == DbOp::Query);
  const auto& p = ops[0].params;
  REQUIRE(p.size() == 6);
  REQUIRE((p[0].type == DbValue::Type::Int && p[0].i == 7));
  REQUIRE((p[1].type == DbValue::Type::Double && p[1].d == 2.5));
  REQUIRE((p[2].type == DbValue::Type::Text && p[2].s == "x"));
  REQUIRE(p[3].type == DbValue::Type::Null);
  REQUIRE((p[4].type == DbValue::Type::Int && p[4].i == 1));
  REQUIRE(p[5].type == DbValue::Type::Null);
  REQUIRE(ops[1].op == DbOp::Exec);
  REQUIRE(ops[2].op == DbOp::Insert);
  REQUIRE((ops[2].params.size() == 1 && ops[2].params[0].i == 0));
}

TEST_CASE("api: cb.db needs permissions.db and a configured database") {
  SECTION("sem permissão") {
    DbEnv env;
    env.addRes("a", R"(
      cb.on('resourceStart', function()
        local ok, e = pcall(cb.db.query, 'SELECT 1')
        cb.log.info('perm ' .. tostring(ok) .. ' ' .. e)
      end)
    )");
    env.boot();
    REQUIRE(env.waitLog("perm false"));
    REQUIRE(env.hasLog("permissions.db"));
    REQUIRE(env.fdb->snapshot().empty());
  }
  SECTION("sem banco") {
    ApiEnv env;
    env.addRes("a", R"(
      cb.on('resourceStart', function()
        local ok, e = pcall(cb.db.exec, 'UPDATE x')
        cb.log.info('nodb ' .. tostring(ok) .. ' ' .. e)
      end)
    )", {}, nlohmann::json::object(), kDbPerm);
    env.boot();
    REQUIRE(env.waitLog("nodb false"));
    REQUIRE(env.hasLog("banco não configurado"));
  }
}

TEST_CASE("api: cb.db validates arguments and refuses calls where it cannot wait") {
  DbEnv env;
  env.fdb->handler = typedRows;
  env.addRes("b", R"(
    return { f = function() return cb.db.query('SELECT 1') end }
  )", {}, nlohmann::json::object(), kDbPerm);
  env.addRes("a", R"(
    local function try(tag, f, ...)
      local ok, e = pcall(f, ...)
      cb.log.info(tag .. ' ' .. tostring(ok) .. ' ' .. tostring(e))
    end
    try('top', cb.db.query, 'SELECT 1')
    cb.on('resourceStart', function()
      try('count', cb.db.query, 'SELECT ?, ?', 1)
      try('type', cb.db.query, 'SELECT ?', {})
      try('fn', cb.db.exec, 'UPDATE ?', print)
      try('sql', cb.db.query, 42)
      try('long', cb.db.query, string.rep(' ', 70000) .. 'SELECT 1')
      try('malformed', cb.db.query, "SELECT 'x")
      try('sort', table.sort, {3, 2, 1}, function(x, y) cb.db.query('SELECT 1') return x < y end)
      try('export', function() return cb.resource('b').f() end)
      try('ok', cb.db.query, 'SELECT 1')
    end)
  )", {"b"}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(env.waitLog("[a] ok true"));
  INFO(env.allLogs());
  REQUIRE(env.hasLog("[a] top false"));
  REQUIRE(env.hasLog("resourceStart"));
  REQUIRE(env.hasLog("[a] count false"));
  REQUIRE(env.hasLog("2 '?'"));
  REQUIRE(env.hasLog("[a] type false"));
  REQUIRE(env.hasLog("parâmetro 1"));
  REQUIRE(env.hasLog("[a] fn false"));
  REQUIRE(env.hasLog("[a] sql false"));
  REQUIRE(env.hasLog("[a] long false"));
  REQUIRE(env.hasLog("[a] malformed false"));
  REQUIRE(env.hasLog("[a] sort false"));
  REQUIRE(env.hasLog("[a] export false"));
  auto sqls = env.fdb->sqls();
  REQUIRE(sqls.size() == 1);  // só o 'ok' chegou ao banco
}

TEST_CASE("api: a query past its deadline raises a timeout and the late result is discarded") {
  DbEnv env(1000);
  env.fdb->handler = typedRows;
  env.fdb->blockOn = "SLEEP";
  env.fdb->honorTimeout = false;  // o pool não responde: só o prazo do lado do script salva
  env.addRes("a", R"(
    cb.on('resourceStart', function()
      local ok, e = pcall(cb.db.query, 'SELECT SLEEP(10)')
      cb.log.info('timeout ' .. tostring(ok) .. ' ' .. tostring(e))
      cb.log.info('next ' .. cb.db.query('SELECT n')[1].n)
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(waitUntil([&] { return env.fdb->blockedNow() == 1; }));
  env.sync();
  REQUIRE_FALSE(env.hasLog("timeout"));
  env.clock->advance(2.5);  // > 1 s + 1 s de folga
  REQUIRE(env.waitLog("[a] timeout false"));
  REQUIRE(env.hasLog("tempo esgotado"));
  REQUIRE(env.waitLog("[a] next 5"));  // a outra conexão segue atendendo
  env.fdb->release();
  REQUIRE(waitUntil([&] { return env.fdb->blockedNow() == 0; }));
  env.sync();
  env.sync();
  REQUIRE(env.countLog("timeout") == 1);
}

TEST_CASE("api: stopping a resource cancels its queries and rolls back its transaction") {
  DbEnv env;
  env.fdb->handler = typedRows;
  env.fdb->blockOn = "SLEEP";
  env.addRes("a", R"(
    cb.on('resourceStart', function()
      cb.timers.after(0, function()
        cb.db.query('SELECT SLEEP(1)')
        cb.log.info('depois1')
      end)
      cb.db.transaction(function(tx)
        tx.exec('UPDATE a SET x = 1')
        tx.query('SELECT SLEEP(2)')
        cb.log.info('depois2')
      end)
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(waitUntil([&] { return env.fdb->blockedNow() == 2; }));
  REQUIRE(env.call(env.host->stopResource("a")));
  env.fdb->release();
  REQUIRE(waitUntil([&] { return env.fdb->count("ROLLBACK") == 1; }));
  env.sync();
  env.sync();
  INFO(env.allLogs());
  REQUIRE_FALSE(env.hasLog("depois"));
  REQUIRE(env.fdb->count("COMMIT") == 0);
}

TEST_CASE("api: cb.db.transaction commits, rolls back on error, refuses nesting and plain calls inside") {
  DbEnv env;
  env.fdb->handler = typedRows;
  env.addRes("a", R"(
    cb.on('resourceStart', function()
      local r = cb.db.transaction(function(tx)
        tx.exec('UPDATE a SET x = ?', 1)
        return tx.insert('INSERT INTO a VALUES (?)', 2)
      end)
      cb.log.info('commit ' .. r)
      local ok, e = pcall(cb.db.transaction, function(tx) tx.exec('UPDATE b') error('falhou aqui') end)
      cb.log.info('rollback ' .. tostring(ok) .. ' ' .. tostring(e))
      ok, e = pcall(cb.db.transaction, function(tx) cb.db.transaction(function() end) end)
      cb.log.info('nested ' .. tostring(ok) .. ' ' .. tostring(e))
      ok, e = pcall(cb.db.transaction, function(tx) cb.db.query('SELECT 1') end)
      cb.log.info('plain ' .. tostring(ok) .. ' ' .. tostring(e))
      local leaked
      cb.db.transaction(function(tx) leaked = tx end)
      ok, e = pcall(leaked.exec, 'UPDATE c')
      cb.log.info('leaked ' .. tostring(ok) .. ' ' .. tostring(e))
      ok, e = pcall(cb.db.transaction, 'x')
      cb.log.info('notfn ' .. tostring(ok))
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(env.waitLog("[a] notfn false"));
  INFO(env.allLogs());
  REQUIRE(env.hasLog("[a] commit 42"));
  REQUIRE(env.hasLog("[a] rollback false"));
  REQUIRE(env.hasLog("falhou aqui"));
  REQUIRE(env.hasLog("[a] nested false"));
  REQUIRE(env.hasLog("aninhada"));
  REQUIRE(env.hasLog("[a] plain false"));
  REQUIRE(env.hasLog("tx.query"));
  REQUIRE(env.hasLog("[a] leaked false"));
  REQUIRE(env.hasLog("encerrada"));
  const std::vector<std::string> want = {"BEGIN",  "UPDATE a SET x = ?", "INSERT INTO a VALUES (?)", "COMMIT",
                                         "BEGIN",  "UPDATE b",           "ROLLBACK",                 "BEGIN",
                                         "ROLLBACK", "BEGIN",            "ROLLBACK",                 "BEGIN",
                                         "COMMIT"};
  REQUIRE(env.fdb->sqls() == want);
  REQUIRE(env.pool->metrics().activeTx == 0);
}

TEST_CASE("api: a watchdog abort inside a transaction rolls it back") {
  DbEnv env;
  env.fdb->handler = typedRows;
  env.addRes("a", R"(
    cb.on('resourceStart', function()
      cb.db.transaction(function(tx)
        tx.exec('UPDATE a SET x = 1')
        while true do end
      end)
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(waitUntil([&] { return env.fdb->count("ROLLBACK") == 1; }));
  REQUIRE(env.fdb->count("COMMIT") == 0);
  REQUIRE(waitUntil([&] { return env.pool->metrics().activeTx == 0; }));
}

TEST_CASE("api: a command handler can reply after awaiting the database") {
  DbEnv env;
  env.fdb->handler = typedRows;
  env.addRes("a", R"(
    cb.commands.register('conta', function()
      local rows = cb.db.query('SELECT n FROM t')
      return 'total=' .. rows[1].n
    end)
    cb.commands.register('quebra', function()
      cb.db.query('SELECT n FROM t')
      error('ruim')
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  env.command(std::nullopt, "conta");
  REQUIRE(waitUntil([&] { return env.hasReply("total=5"); }));
  env.command(std::nullopt, "quebra");
  REQUIRE(waitUntil([&] { return env.hasReply("erro ao executar 'quebra' (ver log)"); }));
}

TEST_CASE("api: resuming a coroutine that waits for the database from outside raises an error in it") {
  DbEnv env;
  env.fdb->handler = typedRows;
  env.fdb->blockOn = "SLEEP";
  env.addRes("a", R"(
    local waiting
    cb.on('resourceStart', function()
      waiting = coroutine.running()
      local ok, e = pcall(cb.db.query, 'SELECT SLEEP(1)')
      cb.log.info('voltou ' .. tostring(ok) .. ' ' .. tostring(e))
    end)
    cb.commands.register('cutuca', function()
      local ok, e = coroutine.resume(waiting, {{ id = 1 }})
      return 'cutucou ' .. tostring(ok)
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(waitUntil([&] { return env.fdb->blockedNow() == 1; }));
  env.command(std::nullopt, "cutuca");
  REQUIRE(waitUntil([&] { return env.hasReply("cutucou true"); }));
  REQUIRE(env.waitLog("[a] voltou false"));
  REQUIRE(env.hasLog("por fora"));
  env.fdb->release();
  REQUIRE(waitUntil([&] { return env.fdb->blockedNow() == 0; }));
  env.sync();
  env.sync();
  REQUIRE(env.countLog("voltou") == 1);
}

TEST_CASE("api: results over the limits and too many pending queries raise errors") {
  auto po = fastPoolOpt();
  po.limits.maxRows = 10;
  DbEnv env(5000, po);
  env.api.maxDbPending = 4;
  env.fdb->blockOn = "SLEEP";
  env.fdb->handler = [](const DbRequest& r) {
    DbResult res;
    res.ok = true;
    res.columns = {"n"};
    if (r.sql.find("big") != std::string::npos)
      for (int i = 0; i < 11; ++i) res.rows.push_back({DbValue::integer(i)});
    return res;
  };
  env.addRes("a", R"(
    cb.on('resourceStart', function()
      local ok, e = pcall(cb.db.query, 'SELECT big')
      cb.log.info('big ' .. tostring(ok) .. ' ' .. tostring(e))
      for i = 1, 6 do
        cb.timers.after(0, function()
          local ok2, e2 = pcall(cb.db.query, 'SELECT SLEEP(' .. i .. ')')
          if not ok2 then cb.log.info('pend ' .. tostring(e2)) end
        end)
      end
    end)
  )", {}, nlohmann::json::object(), kDbPerm);
  env.boot();
  REQUIRE(env.waitLog("[a] big false"));
  REQUIRE(env.hasLog("grande demais"));
  REQUIRE(env.waitLog("pendentes"));
  INFO(env.allLogs());
  REQUIRE(env.countLog("pend ") == 2);
}
