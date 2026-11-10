// DbPool e utilitários de banco (ver db.hpp). O backend libmariadb fica em db_mysql.cpp.
#include "cbscript/db.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>

namespace cbscript {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// ------------------------------------------------------------------------------------------------ utilitários

std::string DbConfig::describe() const {
  return user + "@" + host + ":" + std::to_string(port) + "/" + database;
}

std::string dbTooBig(const DbLimits& lim) {
  return "resultado grande demais (máximo " + std::to_string(lim.maxRows) + " linhas e " +
         std::to_string(lim.maxBytes >> 20) + " MiB); use LIMIT ou filtre mais";
}

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool pctDecode(std::string_view in, std::string& out) {
  out.clear();
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') {
      out += in[i];
      continue;
    }
    if (i + 2 >= in.size()) return false;
    const int h = hexVal(in[i + 1]), l = hexVal(in[i + 2]);
    if (h < 0 || l < 0) return false;
    out += static_cast<char>(h * 16 + l);
    i += 2;
  }
  return true;
}

bool parseIntIn(std::string_view s, int lo, int hi, int& out) {
  if (s.empty() || s.size() > 9) return false;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

}  // namespace

bool parseDbUrl(std::string_view url, DbConfig& out, std::string& err) {
  constexpr std::string_view scheme = "mysql://";
  if (url.substr(0, scheme.size()) != scheme) {
    err = "a URL deve começar com mysql://";
    return false;
  }
  std::string_view rest = url.substr(scheme.size());
  DbConfig c;
  std::string_view query;
  if (auto q = rest.find('?'); q != std::string_view::npos) {
    query = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos || slash + 1 >= rest.size()) {
    err = "falta o nome do banco (mysql://usuário@host/banco)";
    return false;
  }
  c.database = std::string(rest.substr(slash + 1));
  std::string_view auth, hostport = rest.substr(0, slash);
  if (auto at = hostport.rfind('@'); at != std::string_view::npos) {
    auth = hostport.substr(0, at);
    hostport = hostport.substr(at + 1);
  }
  if (!auth.empty()) {
    std::string_view u = auth, p;
    if (auto colon = auth.find(':'); colon != std::string_view::npos) {
      u = auth.substr(0, colon);
      p = auth.substr(colon + 1);
    }
    if (!pctDecode(u, c.user) || !pctDecode(p, c.password)) {
      err = "usuário/senha com %XX inválido";
      return false;
    }
  }
  if (auto colon = hostport.rfind(':'); colon != std::string_view::npos && hostport.find(']') == std::string_view::npos) {
    if (!parseIntIn(hostport.substr(colon + 1), 1, 65535, c.port)) {
      err = "porta inválida";
      return false;
    }
    hostport = hostport.substr(0, colon);
  }
  if (hostport.empty()) {
    err = "falta o host";
    return false;
  }
  c.host = std::string(hostport);
  while (!query.empty()) {
    std::string_view kv = query.substr(0, query.find('&'));
    query = kv.size() < query.size() ? query.substr(kv.size() + 1) : std::string_view{};
    const auto eq = kv.find('=');
    const std::string_view k = kv.substr(0, eq), v = eq == std::string_view::npos ? std::string_view{} : kv.substr(eq + 1);
    if (k == "tls") {
      if (v == "auto") c.tls = DbConfig::Tls::Auto;
      else if (v == "off") c.tls = DbConfig::Tls::Off;
      else if (v == "required") c.tls = DbConfig::Tls::Required;
      else if (v == "verify") c.tls = DbConfig::Tls::Verify;
      else {
        err = "tls deve ser auto, off, required ou verify";
        return false;
      }
    } else if (k == "pool") {
      if (!parseIntIn(v, 1, 64, c.pool)) {
        err = "pool deve ser 1..64";
        return false;
      }
    } else if (k == "timeoutMs") {
      if (!parseIntIn(v, 100, 600000, c.timeoutMs)) {
        err = "timeoutMs deve ser 100..600000";
        return false;
      }
    } else if (!k.empty()) {
      err = "parâmetro desconhecido na URL: " + std::string(k);
      return false;
    }
  }
  out = std::move(c);
  return true;
}

int countPlaceholders(std::string_view sql) {
  int n = 0;
  const size_t len = sql.size();
  for (size_t i = 0; i < len; ++i) {
    const char c = sql[i];
    if (c == '\'' || c == '"' || c == '`') {
      // Literal/identificador: aspas dobradas ficam dentro; barra invertida escapa (não nos identificadores).
      size_t j = i + 1;
      bool closed = false;
      while (j < len) {
        if (sql[j] == '\\' && c != '`') {
          j += 2;
          continue;
        }
        if (sql[j] == c) {
          if (j + 1 < len && sql[j + 1] == c) {
            j += 2;
            continue;
          }
          closed = true;
          break;
        }
        ++j;
      }
      if (!closed) return -1;
      i = j;
    } else if (c == '#') {
      while (i < len && sql[i] != '\n') ++i;
    } else if (c == '-' && i + 1 < len && sql[i + 1] == '-' &&
               (i + 2 >= len || sql[i + 2] == ' ' || sql[i + 2] == '\t' || sql[i + 2] == '\n' || sql[i + 2] == '\r')) {
      while (i < len && sql[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < len && sql[i + 1] == '*') {
      const size_t end = sql.find("*/", i + 2);
      if (end == std::string_view::npos) return -1;
      i = end + 1;
    } else if (c == '?') {
      ++n;
    }
  }
  return n;
}

std::string formatDbTime(DbTimeKind kind, bool negative, unsigned year, unsigned month, unsigned day, unsigned hour,
                         unsigned minute, unsigned second, unsigned long micro, unsigned decimals) {
  char buf[96];
  int n = 0;
  auto frac = [&](char* p, size_t cap) -> int {
    if (decimals == 0) return 0;
    char f[8];
    std::snprintf(f, sizeof f, "%06lu", micro % 1000000ul);
    const unsigned d = std::min(decimals, 6u);
    return std::snprintf(p, cap, ".%.*s", static_cast<int>(d), f);
  };
  switch (kind) {
    case DbTimeKind::Date:
      n = std::snprintf(buf, sizeof buf, "%04u-%02u-%02u", year % 10000u, month % 100u, day % 100u);
      break;
    case DbTimeKind::Time:
      n = std::snprintf(buf, sizeof buf, "%s%02u:%02u:%02u", negative ? "-" : "", hour, minute % 100u, second % 100u);
      n += frac(buf + n, sizeof buf - static_cast<size_t>(n));
      break;
    case DbTimeKind::DateTime:
      n = std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02u", year % 10000u, month % 100u, day % 100u,
                        hour % 100u, minute % 100u, second % 100u);
      n += frac(buf + n, sizeof buf - static_cast<size_t>(n));
      break;
  }
  return std::string(buf, static_cast<size_t>(std::max(0, n)));
}

// ------------------------------------------------------------------------------------------------------- pool

namespace detail {

struct DbJob {
  uint64_t id = 0;
  DbRequest req;
  DbPool::Done done;
  Clock::time_point deadline;
};

struct DbWorker {
  int index = 0;
  std::unique_ptr<DbConnection> conn;  // só a thread do worker
  // Sob DbPoolState::mu:
  uint64_t tx = 0;  // transação fixada (0 = livre)
  std::deque<DbJob> txq;
  bool txAbort = false;
  Clock::time_point txIdleUntil{};
  bool exited = false;
  // Só a thread do worker:
  Clock::time_point lastUsed{};
  Clock::time_point nextConnect{};
  milliseconds backoff{0};
  std::string lastConnectErr;
  bool wasConnected = false;
  std::thread th;
};

struct DbPoolState {
  DbConfig cfg;
  DbConnectionFactory factory;
  DbPoolOptions opt;
  size_t maxTx = 1;

  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<DbJob> q;  // pedidos avulsos e Begin
  std::unordered_map<uint64_t, DbWorker*> txs;  // transação → worker fixado
  std::vector<std::unique_ptr<DbWorker>> workers;
  bool stop = false;
  uint64_t nextId = 1;
  size_t activeTx = 0;
  size_t exited = 0;
  std::condition_variable exitCv;

  std::atomic<uint64_t> queries{0}, errors{0}, timeouts{0};
  std::atomic<int> busy{0}, connected{0};
  mutable std::mutex latMu;
  std::vector<double> lat = std::vector<double>(1024, 0.0);
  size_t latHead = 0, latCount = 0;
  mutable uint64_t lastQueries = 0;
  mutable Clock::time_point lastAt = Clock::now();

  void log(bool error, const std::string& m) const {
    try {
      if (opt.log)
        opt.log(error, m);
      else if (error)
        spdlog::error("{}", m);
      else
        spdlog::warn("{}", m);
    } catch (...) {
    }
  }
  milliseconds timeout() const { return milliseconds(cfg.timeoutMs); }
  void record(double ms) {
    std::lock_guard<std::mutex> lk(latMu);
    lat[latHead] = ms;
    latHead = (latHead + 1) % lat.size();
    latCount = std::min(latCount + 1, lat.size());
  }
  static void finish(DbJob& j, DbResult r) {
    if (!j.done) return;
    try {
      j.done(std::move(r));
    } catch (...) {
    }
  }
  // Solta a transação do worker (sob mu). Os pedidos dela que sobraram na fila voltam para falhar fora do lock.
  void endTx(DbWorker& w, std::deque<DbJob>& orphans) {
    if (w.tx == 0) return;
    txs.erase(w.tx);
    w.tx = 0;
    w.txAbort = false;
    for (auto& j : w.txq) orphans.push_back(std::move(j));
    w.txq.clear();
    if (activeTx > 0) --activeTx;
    cv.notify_all();
  }
  void failAll(std::deque<DbJob>& js, const std::string& why) {
    for (auto& j : js) finish(j, DbResult::fail(why));
    js.clear();
  }

  // Garante a conexão do worker. false + err se não deu (falha rápida durante a espera entre tentativas).
  bool ensure(DbWorker& w, std::string& err) {
    auto now = Clock::now();
    if (w.conn->connected() && w.lastUsed != Clock::time_point{} && now - w.lastUsed > opt.pingAfterIdle) {
      if (!w.conn->ping()) w.conn->close();
    }
    if (w.conn->connected()) return true;
    if (w.wasConnected) {
      w.wasConnected = false;
      connected.fetch_sub(1);
      log(false, "banco: conexão " + std::to_string(w.index) + " perdida; reconectando");
    }
    if (now < w.nextConnect) {
      err = "banco indisponível (reconectando; último erro: " + w.lastConnectErr + ")";
      return false;
    }
    std::string e;
    if (!w.conn->connect(e)) {
      w.backoff = w.backoff.count() == 0 ? opt.reconnectMin : std::min(opt.reconnectMax, w.backoff * 2);
      w.nextConnect = Clock::now() + w.backoff;
      if (e != w.lastConnectErr) log(true, "banco: sem conexão com " + cfg.describe() + ": " + e);
      w.lastConnectErr = e;
      err = "sem conexão com o banco: " + e;
      return false;
    }
    if (!w.lastConnectErr.empty()) log(false, "banco: conexão " + std::to_string(w.index) + " restabelecida");
    w.lastConnectErr.clear();
    w.backoff = milliseconds(0);
    w.wasConnected = true;
    connected.fetch_add(1);
    return true;
  }

  DbResult execute(DbWorker& w, const DbJob& j) {
    std::string err;
    if (!ensure(w, err)) return DbResult::fail(err);
    busy.fetch_add(1);
    const auto t0 = Clock::now();
    const auto left = std::max(milliseconds(1), std::chrono::duration_cast<milliseconds>(j.deadline - t0));
    DbResult r;
    try {
      r = w.conn->run(j.req, opt.limits, left);
    } catch (const std::exception& e) {
      r = DbResult::fail(std::string("erro interno do banco: ") + e.what());
    } catch (...) {
      r = DbResult::fail("erro interno do banco");
    }
    const auto t1 = Clock::now();
    busy.fetch_sub(1);
    w.lastUsed = t1;
    record(std::chrono::duration<double, std::milli>(t1 - t0).count());
    if (r.ok) {
      size_t bytes = 0;
      for (const auto& row : r.rows)
        for (const auto& v : row) bytes += 16 + v.s.size();
      if (r.rows.size() > opt.limits.maxRows || bytes > opt.limits.maxBytes) r = DbResult::fail(dbTooBig(opt.limits));
    }
    if (!w.conn->connected() && w.wasConnected) {
      w.wasConnected = false;
      connected.fetch_sub(1);
    }
    return r;
  }

  void count(const DbResult& r) {
    queries.fetch_add(1);
    if (!r.ok) errors.fetch_add(1);
    if (r.timedOut) timeouts.fetch_add(1);
  }

  void workerMain(DbWorker& w) {
    try {
      w.conn = factory();
    } catch (...) {
    }
    for (;;) {
      DbJob job;
      bool have = false, rollbackNow = false;
      std::deque<DbJob> orphans;
      {
        std::unique_lock<std::mutex> lk(mu);
        for (;;) {
          if (stop) break;
          if (w.tx != 0) {
            if (!w.txq.empty()) {
              job = std::move(w.txq.front());
              w.txq.pop_front();
              have = true;
              break;
            }
            if (w.txAbort || Clock::now() >= w.txIdleUntil) {
              rollbackNow = true;
              break;
            }
            cv.wait_until(lk, w.txIdleUntil);
            continue;
          }
          auto it = q.begin();
          for (; it != q.end(); ++it)
            if (it->req.op != DbOp::Begin || activeTx < maxTx) break;
          if (it != q.end()) {
            job = std::move(*it);
            q.erase(it);
            have = true;
            if (job.req.op == DbOp::Begin) {
              ++activeTx;
              w.tx = job.id;
              w.txAbort = false;
              w.txIdleUntil = Clock::now() + opt.txIdle;
              txs[job.id] = &w;
            }
            break;
          }
          cv.wait(lk);
        }
        if (stop && !have && !rollbackNow) {
          endTx(w, orphans);
          lk.unlock();
          failAll(orphans, "banco encerrado");
          break;
        }
      }
      if (!w.conn) {
        if (have) finish(job, DbResult::fail("banco: falha ao criar a conexão"));
        std::lock_guard<std::mutex> lk(mu);
        endTx(w, orphans);
        continue;
      }
      if (rollbackNow) {
        if (w.conn->connected()) {
          DbRequest rb;
          rb.op = DbOp::Rollback;
          (void)w.conn->run(rb, opt.limits, timeout());
        }
        {
          std::lock_guard<std::mutex> lk(mu);
          endTx(w, orphans);
        }
        failAll(orphans, "transação encerrada (rollback)");
        continue;
      }
      const bool inTx = job.req.op == DbOp::Begin || job.req.tx != 0;
      DbResult r;
      if (Clock::now() >= job.deadline) {
        r = DbResult::fail("tempo esgotado na fila do banco (" + std::to_string(cfg.timeoutMs) + " ms)", true);
      } else {
        r = execute(w, job);
      }
      count(r);
      bool end = false;
      if (inTx) {
        end = job.req.op == DbOp::Commit || job.req.op == DbOp::Rollback || (job.req.op == DbOp::Begin && !r.ok) ||
              !w.conn->connected();
        if (!end && r.timedOut) {
          // Comando da transação estourou o prazo: o estado dela é incerto; rollback já.
          DbRequest rb;
          rb.op = DbOp::Rollback;
          if (w.conn->connected()) (void)w.conn->run(rb, opt.limits, timeout());
          end = true;
        }
      }
      {
        std::lock_guard<std::mutex> lk(mu);
        if (end)
          endTx(w, orphans);
        else if (inTx)
          w.txIdleUntil = Clock::now() + opt.txIdle;
      }
      failAll(orphans, "transação encerrada");
      finish(job, std::move(r));
    }
    if (w.conn) {
      w.conn->close();
      if (w.wasConnected) connected.fetch_sub(1);
      w.wasConnected = false;
    }
    w.conn.reset();
    std::lock_guard<std::mutex> lk(mu);
    w.exited = true;
    ++exited;
    exitCv.notify_all();
  }
};

}  // namespace detail

DbPool::DbPool(DbConfig cfg, DbConnectionFactory factory, DbPoolOptions opt)
    : st_(std::make_shared<detail::DbPoolState>()) {
  auto& s = *st_;
  cfg.pool = std::clamp(cfg.pool, 1, 64);
  cfg.timeoutMs = std::max(1, cfg.timeoutMs);
  s.cfg = std::move(cfg);
  s.factory = std::move(factory);
  s.opt = std::move(opt);
  s.maxTx = static_cast<size_t>(std::max(1, s.cfg.pool - 1));
  for (int i = 0; i < s.cfg.pool; ++i) {
    auto w = std::make_unique<detail::DbWorker>();
    w->index = i + 1;
    s.workers.push_back(std::move(w));
  }
  auto self = st_;
  for (auto& w : s.workers) {
    detail::DbWorker* wp = w.get();
    w->th = std::thread([self, wp] { self->workerMain(*wp); });
  }
}

DbPool::~DbPool() {
  auto& s = *st_;
  std::deque<detail::DbJob> left;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    s.stop = true;
    left.swap(s.q);
    for (auto& w : s.workers) {
      if (w->tx != 0) w->txAbort = true;
      for (auto& j : w->txq) left.push_back(std::move(j));
      w->txq.clear();
    }
    s.cv.notify_all();
  }
  s.failAll(left, "banco encerrado");
  {
    std::unique_lock<std::mutex> lk(s.mu);
    s.exitCv.wait_for(lk, s.opt.shutdownWait, [&] { return s.exited == s.workers.size(); });
  }
  for (auto& w : s.workers) {
    bool done;
    {
      std::lock_guard<std::mutex> lk(s.mu);
      done = w->exited;
    }
    if (!w->th.joinable()) continue;
    if (done) {
      w->th.join();
    } else {
      // Conexão presa (rede, DNS...): a thread fica com o estado (shared_ptr) e termina sozinha quando voltar.
      s.log(true, "banco: conexão " + std::to_string(w->index) + " presa no encerramento; thread desanexada");
      w->th.detach();
    }
  }
}

uint64_t DbPool::submit(DbRequest r, Done done, std::string& err) {
  auto& s = *st_;
  const bool control = r.op == DbOp::Begin || r.op == DbOp::Commit || r.op == DbOp::Rollback;
  if (control) {
    r.sql.clear();
    r.params.clear();
    if (r.op == DbOp::Begin && r.tx != 0) {
      err = "transação aninhada";
      return 0;
    }
    if (r.op != DbOp::Begin && r.tx == 0) {
      err = "commit/rollback sem transação";
      return 0;
    }
  } else {
    if (r.sql.empty()) {
      err = "SQL vazio";
      return 0;
    }
    if (r.sql.size() > s.opt.limits.maxSql) {
      err = "SQL longo demais (máximo " + std::to_string(s.opt.limits.maxSql / 1024) + " KiB)";
      return 0;
    }
    const int n = countPlaceholders(r.sql);
    if (n < 0) {
      err = "SQL malformado (texto entre aspas, identificador ou comentário sem fim)";
      return 0;
    }
    if (r.params.size() > s.opt.limits.maxParams) {
      err = "parâmetros demais (máximo " + std::to_string(s.opt.limits.maxParams) + ")";
      return 0;
    }
    if (static_cast<size_t>(n) != r.params.size()) {
      err = "a consulta tem " + std::to_string(n) + " '?' mas recebeu " + std::to_string(r.params.size()) +
            " parâmetro(s)";
      return 0;
    }
  }
  detail::DbJob j;
  j.req = std::move(r);
  j.done = std::move(done);
  j.deadline = Clock::now() + s.timeout();
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.stop) {
    err = "banco encerrado";
    return 0;
  }
  j.id = s.nextId++;
  const uint64_t id = j.id;
  if (j.req.tx != 0) {
    auto it = s.txs.find(j.req.tx);
    if (it == s.txs.end()) {
      err = "transação encerrada ou desconhecida";
      return 0;
    }
    it->second->txq.push_back(std::move(j));
  } else {
    s.q.push_back(std::move(j));
  }
  s.cv.notify_all();
  return id;
}

void DbPool::cancel(uint64_t id) {
  auto& s = *st_;
  detail::DbJob gone;  // destruído fora do lock
  std::lock_guard<std::mutex> lk(s.mu);
  for (auto it = s.q.begin(); it != s.q.end(); ++it)
    if (it->id == id) {
      gone = std::move(*it);
      s.q.erase(it);
      return;
    }
  for (auto& w : s.workers)
    for (auto it = w->txq.begin(); it != w->txq.end(); ++it)
      if (it->id == id) {
        gone = std::move(*it);
        w->txq.erase(it);
        return;
      }
}

void DbPool::abortTx(uint64_t tx) {
  auto& s = *st_;
  detail::DbJob gone;
  std::lock_guard<std::mutex> lk(s.mu);
  for (auto it = s.q.begin(); it != s.q.end(); ++it)
    if (it->id == tx && it->req.op == DbOp::Begin) {  // ainda esperando conexão: nunca começa
      gone = std::move(*it);
      s.q.erase(it);
      return;
    }
  auto it = s.txs.find(tx);
  if (it == s.txs.end()) return;
  it->second->txAbort = true;
  s.cv.notify_all();
}

DbMetrics DbPool::metrics() const {
  const auto& s = *st_;
  DbMetrics m;
  m.queries = s.queries.load();
  m.errors = s.errors.load();
  m.timeouts = s.timeouts.load();
  m.busy = s.busy.load();
  m.connected = s.connected.load();
  m.size = s.cfg.pool;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    m.queued = s.q.size();
    for (auto& w : s.workers) m.queued += w->txq.size();
    m.activeTx = static_cast<int>(s.activeTx);
  }
  {
    std::lock_guard<std::mutex> lk(s.latMu);
    std::vector<double> v(s.lat.begin(), s.lat.begin() + static_cast<std::ptrdiff_t>(s.latCount));
    if (!v.empty()) {
      std::sort(v.begin(), v.end());
      m.p50Ms = v[(v.size() - 1) * 50 / 100];
      m.p99Ms = v[(v.size() - 1) * 99 / 100];
    }
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - s.lastAt).count();
    if (dt > 0) m.qps = static_cast<double>(m.queries - s.lastQueries) / dt;
    s.lastQueries = m.queries;
    s.lastAt = now;
  }
  return m;
}

const DbConfig& DbPool::config() const { return st_->cfg; }
const DbPoolOptions& DbPool::options() const { return st_->opt; }

}  // namespace cbscript
