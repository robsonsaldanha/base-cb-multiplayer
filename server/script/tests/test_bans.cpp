// BanStore com o backend MySQL contra um banco falso que imita a tabela cb_bans (sem MariaDB).
#include <catch2/catch_test_macros.hpp>

#include <map>

#include "api_env.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/db.hpp"
#include "fake_db.hpp"

using namespace cbscript;

namespace {

// Tabela cb_bans em memória, respondendo ao SQL que o BanStore usa.
struct FakeBanTable {
  std::mutex mu;
  std::map<std::string, Ban> rows;
  bool created = false;
  int selectFailCode = 0;               // != 0: o SELECT falha com esse código (permissão negada...)
  std::map<std::string, int> failDid;   // REPLACE deste did falha com o código
  int selects = 0;

  DbResult handle(const DbRequest& r) {
    std::lock_guard<std::mutex> lk(mu);
    DbResult res;
    res.ok = true;
    const std::string& s = r.sql;
    auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
    if (has("CREATE TABLE IF NOT EXISTS cb_bans")) {
      created = true;
    } else if (!created) {
      return DbResult::fail("Table 'd.cb_bans' doesn't exist");
    } else if (has("DELETE FROM cb_bans WHERE expires_at")) {
      const int64_t now = r.params.at(0).i;
      std::erase_if(rows, [now](const auto& kv) { return kv.second.expiresAt != 0 && kv.second.expiresAt <= now; });
    } else if (has("SELECT did, reason, created_at, expires_at, created_by FROM cb_bans")) {
      ++selects;
      if (selectFailCode != 0) {
        DbResult f = DbResult::fail("SELECT command denied to user");
        f.code = selectFailCode;
        return f;
      }
      const int64_t now = r.params.at(0).i;
      const std::string after = r.params.at(1).s;  // paginação por did
      const size_t lim = static_cast<size_t>(std::stoul(s.substr(s.rfind("LIMIT ") + 6)));
      res.columns = {"did", "reason", "created_at", "expires_at", "created_by"};
      for (auto it = rows.upper_bound(after); it != rows.end() && res.rows.size() < lim; ++it) {
        const Ban& b = it->second;
        if (b.expiresAt != 0 && b.expiresAt <= now) continue;
        res.rows.push_back({DbValue::text(b.did), DbValue::text(b.reason), DbValue::integer(b.createdAt),
                            b.expiresAt ? DbValue::integer(b.expiresAt) : DbValue::null(), DbValue::text(b.by)});
      }
    } else if (has("REPLACE INTO cb_bans")) {
      if (auto f = failDid.find(r.params.at(0).s); f != failDid.end()) {
        DbResult e = DbResult::fail("Incorrect string value");
        e.code = f->second;
        return e;
      }
      Ban b;
      b.did = r.params.at(0).s;
      b.reason = r.params.at(1).s;
      b.createdAt = r.params.at(2).i;
      b.expiresAt = r.params.at(3).type == DbValue::Type::Null ? 0 : r.params.at(3).i;
      b.by = r.params.at(4).s;
      rows[b.did] = b;
      res.affected = 1;
    } else if (has("DELETE FROM cb_bans WHERE did")) {
      res.affected = static_cast<int64_t>(rows.erase(r.params.at(0).s));
    } else {
      return DbResult::fail("SQL inesperado: " + s);
    }
    return res;
  }
  bool hasRow(const std::string& did) {
    std::lock_guard<std::mutex> lk(mu);
    return rows.count(did) > 0;
  }
  std::string reasonOf(const std::string& did) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = rows.find(did);
    return it == rows.end() ? std::string() : it->second.reason;
  }
};

struct BanDb {
  std::shared_ptr<FakeDb> fdb = std::make_shared<FakeDb>();
  std::shared_ptr<FakeBanTable> table = std::make_shared<FakeBanTable>();
  std::shared_ptr<DbPool> pool;
  std::atomic<int64_t> now{1'700'000'000};
  BanStoreDbOptions opt;
  std::mutex logMu;
  std::vector<std::string> logs;
  BanDb() {
    fdb->handler = [t = table](const DbRequest& r) { return t->handle(r); };
    pool = std::make_shared<DbPool>(fakeCfg(2, 2000), fakeFactory(fdb), fastPoolOpt());
    opt.retryMin = std::chrono::milliseconds(20);
    opt.retryMax = std::chrono::milliseconds(80);
    opt.log = [this](bool, const std::string& m) {
      std::lock_guard<std::mutex> lk(logMu);
      logs.push_back(m);
    };
  }
  ~BanDb() { fdb->release(); }
  std::unique_ptr<BanStore> store() {
    return std::make_unique<BanStore>(pool, [this] { return now.load(); }, opt);
  }
  bool logged(const std::string& sub) {
    std::lock_guard<std::mutex> lk(logMu);
    for (auto& l : logs)
      if (contains(l, sub)) return true;
    return false;
  }
};

}  // namespace

TEST_CASE("bans mysql: the table is created, current bans are loaded at start and expired ones are ignored") {
  BanDb db;
  {
    std::lock_guard<std::mutex> lk(db.table->mu);
    db.table->created = true;
    db.table->rows["perm"] = Ban{"perm", "xingou", "admin:1", 10, 0};
    db.table->rows["old"] = Ban{"old", "velho", "admin:1", 10, db.now.load() - 5};
    db.table->rows["temp"] = Ban{"temp", "spam", "resource:x", 10, db.now.load() + 60};
  }
  auto s = db.store();
  REQUIRE(std::string(s->backend()) == "mysql");
  REQUIRE(s->flush(std::chrono::milliseconds(5000)));
  REQUIRE(s->loaded());
  REQUIRE(db.fdb->count("CREATE TABLE IF NOT EXISTS cb_bans") == 1);
  auto p = s->check("perm", db.now);
  REQUIRE(p);
  REQUIRE(p->reason == "xingou");
  REQUIRE(p->by == "admin:1");
  REQUIRE(p->expiresAt == 0);
  REQUIRE_FALSE(s->check("old", db.now));
  REQUIRE(s->check("temp", db.now));
  REQUIRE_FALSE(s->check("temp", db.now + 61));
  REQUIRE_FALSE(db.table->hasRow("old"));  // limpeza dos expirados na carga
  REQUIRE(s->list().size() == 2);
}

TEST_CASE("bans mysql: writes reach the database in order and local changes win over the initial load") {
  BanDb db;
  {
    std::lock_guard<std::mutex> lk(db.table->mu);
    db.table->created = true;
    db.table->rows["a"] = Ban{"a", "antigo", "admin:1", 10, 0};
    db.table->rows["b"] = Ban{"b", "bbb", "admin:1", 10, 0};
  }
  db.fdb->blockOn = "SELECT did";  // segura a carga inicial
  auto s = db.store();
  REQUIRE(waitUntil([&] { return db.fdb->blockedNow() == 1; }));
  REQUIRE_FALSE(s->loaded());
  std::string err;
  bool removed = false;
  REQUIRE(s->add(Ban{"a", "novo", "resource:r", db.now, 0}, err));
  REQUIRE(s->remove("b", removed, err));
  REQUIRE_FALSE(removed);  // ainda não estava no cache
  REQUIRE(s->add(Ban{"c", "ccc", "resource:r", db.now, 0}, err));
  REQUIRE(s->remove("c", removed, err));
  REQUIRE(removed);
  REQUIRE(s->check("a", db.now)->reason == "novo");
  db.fdb->release();
  REQUIRE(s->flush(std::chrono::milliseconds(5000)));
  REQUIRE(s->loaded());
  REQUIRE(s->pendingWrites() == 0);
  REQUIRE(s->check("a", db.now)->reason == "novo");  // a carga não sobrescreveu
  REQUIRE_FALSE(s->check("b", db.now));               // removido antes da carga: continua fora
  REQUIRE_FALSE(s->check("c", db.now));
  REQUIRE(db.table->reasonOf("a") == "novo");
  REQUIRE_FALSE(db.table->hasRow("b"));
  REQUIRE_FALSE(db.table->hasRow("c"));  // REPLACE e depois DELETE, em ordem
  // Recarga: outro BanStore vê o que foi gravado.
  auto again = db.store();
  REQUIRE(again->flush(std::chrono::milliseconds(5000)));
  REQUIRE(again->check("a", db.now)->reason == "novo");
  REQUIRE_FALSE(again->check("b", db.now));
}

TEST_CASE("bans mysql: with the database down at start it retries; bans hold in memory and are written later") {
  BanDb db;
  db.fdb->setDown(true);
  auto s = db.store();
  std::string err;
  REQUIRE(s->add(Ban{"x", "fora do ar", "resource:r", db.now, db.now + 3600}, err));
  REQUIRE(s->check("x", db.now));  // vale já, na memória
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE_FALSE(s->loaded());
  REQUIRE(s->pendingWrites() == 1);
  REQUIRE(db.logged("cb_bans"));
  db.fdb->setDown(false);
  REQUIRE(s->flush(std::chrono::milliseconds(5000)));
  REQUIRE(s->loaded());
  REQUIRE(db.table->hasRow("x"));
  REQUIRE(db.table->reasonOf("x") == "fora do ar");
}

TEST_CASE("bans mysql: destroying the store with the database down does not hang") {
  BanDb db;
  db.fdb->setDown(true);
  auto s = db.store();
  std::string err;
  REQUIRE(s->add(Ban{"y", "r", "resource:r", db.now, 0}, err));
  auto t0 = std::chrono::steady_clock::now();
  guarded("ban store destroy", [&s] {
    s.reset();
    return 0;
  });
  REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
  REQUIRE(db.logged("não gravad"));
}

TEST_CASE("api: cb.players.ban with the mysql store kicks, persists and survives a reload") {
  BanDb db;
  ApiEnv env;
  env.bans = db.store();
  env.api.bans = env.bans;
  env.addRes("a", R"(
    cb.on('playerJoin', function(p) cb.players.ban(p.did, 'trapaça', 60) end)
  )");
  env.boot();
  env.join(3, "bia");
  REQUIRE(env.waitCmd<CmdKickDid>([](const CmdKickDid& k) { return k.did == "d3"; }));
  REQUIRE(env.bans->check("d3", env.unixTime.load()));
  REQUIRE(env.bans->flush(std::chrono::milliseconds(5000)));
  REQUIRE(db.table->reasonOf("d3") == "trapaça");
  auto again = db.store();
  REQUIRE(again->flush(std::chrono::milliseconds(5000)));
  auto b = again->check("d3", env.unixTime.load());
  REQUIRE(b);
  REQUIRE(b->by == "resource:a");
  REQUIRE(b->expiresAt == env.unixTime.load() + 60);
}

TEST_CASE("bans mysql: more bans than the query row limit are loaded in pages") {
  BanDb db;
  {
    std::lock_guard<std::mutex> lk(db.table->mu);
    db.table->created = true;
    for (int i = 0; i < 12000; ++i) {
      char d[16];
      std::snprintf(d, sizeof d, "d%05d", i);
      db.table->rows[d] = Ban{d, "r", "admin:1", 10, 0};
    }
  }
  auto s = db.store();  // pool com o limite padrão de 10 000 linhas por consulta
  REQUIRE(s->flush(std::chrono::milliseconds(10000)));
  REQUIRE(s->loaded());
  REQUIRE(s->list().size() == 12000);
  REQUIRE(s->check("d00000", db.now));
  REQUIRE(s->check("d11999", db.now));
  REQUIRE(db.table->selects >= 3);
}

TEST_CASE("bans mysql: a write that fails permanently is dropped, logged and audited; the queue moves on") {
  BanDb db;
  std::vector<std::string> audits;
  std::mutex amu;
  db.opt.audit = [&](const std::string& line) {
    std::lock_guard<std::mutex> lk(amu);
    audits.push_back(line);
  };
  {
    std::lock_guard<std::mutex> lk(db.table->mu);
    db.table->failDid["ruim"] = 1366;  // Incorrect string value: repetir não adianta
  }
  auto s = db.store();
  REQUIRE(s->flush(std::chrono::milliseconds(5000)));
  std::string err;
  REQUIRE(s->add(Ban{"ruim", "x", "resource:r", db.now, 0}, err));
  REQUIRE(s->add(Ban{"bom", "y", "resource:r", db.now, 0}, err));
  REQUIRE(s->flush(std::chrono::milliseconds(5000)));
  REQUIRE(db.table->hasRow("bom"));
  REQUIRE_FALSE(db.table->hasRow("ruim"));
  REQUIRE(s->check("ruim", db.now));  // continua valendo em memória até reiniciar
  REQUIRE(db.logged("ruim"));
  std::lock_guard<std::mutex> lk(amu);
  REQUIRE(audits.size() == 1);
  REQUIRE(contains(audits[0], "ban_write_dropped"));
  REQUIRE(contains(audits[0], "ruim"));
}

TEST_CASE("bans mysql: writes still reach the database while the initial load keeps failing") {
  BanDb db;
  {
    std::lock_guard<std::mutex> lk(db.table->mu);
    db.table->created = true;
    db.table->selectFailCode = 1142;  // sem permissão de SELECT
  }
  auto s = db.store();
  std::string err;
  REQUIRE(s->add(Ban{"w", "z", "resource:r", db.now, 0}, err));
  REQUIRE(waitUntil([&] { return db.table->hasRow("w"); }));
  REQUIRE_FALSE(s->loaded());
  REQUIRE(waitUntil([&] { return db.logged("NÃO"); }));  // aviso crítico depois de várias falhas
}
