#pragma once
// Banco falso para os testes do DbPool, de cb.db e do BanStore (sem MariaDB).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cbscript/db.hpp"

namespace cbtest {

struct FakeDb {
  struct Op {
    int conn = 0;  // número da conexão que executou
    cbscript::DbOp op = cbscript::DbOp::Query;
    std::string sql;
    std::vector<cbscript::DbValue> params;
    uint64_t tx = 0;
  };
  mutable std::mutex mu;
  std::condition_variable cv;
  // Resposta para cada pedido (fora do mutex). Vazio = ok sem linhas.
  std::function<cbscript::DbResult(const cbscript::DbRequest&)> handler;
  std::vector<Op> ops;      // tudo o que foi executado, em ordem ("BEGIN"/"COMMIT"/"ROLLBACK" em sql)
  std::string blockOn;      // pedidos cujo SQL contém isto esperam `released` (vazio = nenhum)
  bool released = false;
  bool honorTimeout = true;  // false = o bloqueio ignora o prazo (conexão "presa")
  int blocked = 0;           // pedidos esperando agora
  bool down = false;         // connect() falha
  bool loseNext = false;     // o próximo run() perde a conexão
  int connects = 0, connectFails = 0, nextConn = 1;

  void release() {
    std::lock_guard<std::mutex> lk(mu);
    released = true;
    cv.notify_all();
  }
  void setDown(bool d) {
    std::lock_guard<std::mutex> lk(mu);
    down = d;
  }
  std::vector<Op> snapshot() const {
    std::lock_guard<std::mutex> lk(mu);
    return ops;
  }
  std::vector<std::string> sqls() const {
    std::lock_guard<std::mutex> lk(mu);
    std::vector<std::string> v;
    for (auto& o : ops) v.push_back(o.sql);
    return v;
  }
  int count(const std::string& sub) const {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (auto& o : ops) n += o.sql.find(sub) != std::string::npos ? 1 : 0;
    return n;
  }
  int blockedNow() const {
    std::lock_guard<std::mutex> lk(mu);
    return blocked;
  }
};

class FakeConn : public cbscript::DbConnection {
 public:
  explicit FakeConn(std::shared_ptr<FakeDb> db) : db_(std::move(db)) {}
  bool connected() const override { return conn_ != 0; }
  bool connect(std::string& err) override {
    std::lock_guard<std::mutex> lk(db_->mu);
    if (db_->down) {
      ++db_->connectFails;
      err = "Can't connect to server on 'fake' (10061)";
      return false;
    }
    ++db_->connects;
    conn_ = db_->nextConn++;
    return true;
  }
  void close() override { conn_ = 0; }
  cbscript::DbResult run(const cbscript::DbRequest& r, const cbscript::DbLimits& lim,
                         std::chrono::milliseconds timeout) override {
    using cbscript::DbOp;
    FakeDb::Op op{conn_, r.op, r.sql, r.params, r.tx};
    if (r.op == DbOp::Begin) op.sql = "BEGIN";
    if (r.op == DbOp::Commit) op.sql = "COMMIT";
    if (r.op == DbOp::Rollback) op.sql = "ROLLBACK";
    std::function<cbscript::DbResult(const cbscript::DbRequest&)> h;
    {
      std::unique_lock<std::mutex> lk(db_->mu);
      db_->ops.push_back(op);
      if (db_->loseNext) {
        db_->loseNext = false;
        conn_ = 0;
        return cbscript::DbResult::fail("Lost connection to server during query");
      }
      if (!db_->blockOn.empty() && op.sql.find(db_->blockOn) != std::string::npos) {
        ++db_->blocked;
        db_->cv.notify_all();
        bool ok = true;
        if (db_->honorTimeout)
          ok = db_->cv.wait_for(lk, timeout, [this] { return db_->released; });
        else
          db_->cv.wait(lk, [this] { return db_->released; });
        --db_->blocked;
        if (!ok) return cbscript::DbResult::fail("tempo esgotado (servidor)", true);
      }
      h = db_->handler;
    }
    cbscript::DbResult res;
    if (h) {
      res = h(r);
    } else {
      res.ok = true;
    }
    if (res.ok && res.rows.size() > lim.maxRows) return cbscript::DbResult::fail(cbscript::dbTooBig(lim));
    return res;
  }

 private:
  std::shared_ptr<FakeDb> db_;
  int conn_ = 0;
};

inline cbscript::DbConnectionFactory fakeFactory(std::shared_ptr<FakeDb> db) {
  return [db] { return std::make_unique<FakeConn>(db); };
}

inline cbscript::DbConfig fakeCfg(int pool = 2, int timeoutMs = 5000) {
  cbscript::DbConfig c;
  c.host = "fake";
  c.user = "u";
  c.password = "segredo-super";
  c.database = "d";
  c.pool = pool;
  c.timeoutMs = timeoutMs;
  return c;
}

inline cbscript::DbPoolOptions fastPoolOpt() {
  cbscript::DbPoolOptions o;
  o.reconnectMin = std::chrono::milliseconds(50);
  o.reconnectMax = std::chrono::milliseconds(200);
  o.shutdownWait = std::chrono::milliseconds(300);
  o.log = [](bool, const std::string&) {};
  return o;
}

}  // namespace cbtest
