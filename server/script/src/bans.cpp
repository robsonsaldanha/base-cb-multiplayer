#include "cbscript/bans.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cbscript/db.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cbscript {

namespace fs = std::filesystem;
using nlohmann::json;
using std::chrono::milliseconds;

namespace {

constexpr int64_t kMaxUnix = int64_t{1} << 40;

bool expired(const Ban& b, int64_t now) { return b.expiresAt != 0 && now >= b.expiresAt; }

std::string u8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

Ban fromJson(const json& j, const std::string& where) {
  auto fail = [&](const char* why) { return std::runtime_error("bans: arquivo inválido '" + where + "': " + why); };
  if (!j.is_object()) throw fail("cada ban deve ser um objeto");
  Ban b;
  if (!j.contains("did") || !j["did"].is_string() || j["did"].get<std::string>().empty())
    throw fail("'did' deve ser texto não vazio");
  b.did = j["did"].get<std::string>();
  if (j.contains("reason")) {
    if (!j["reason"].is_string()) throw fail("'reason' deve ser texto");
    b.reason = j["reason"].get<std::string>();
  }
  if (j.contains("by")) {
    if (!j["by"].is_string()) throw fail("'by' deve ser texto");
    b.by = j["by"].get<std::string>();
  }
  if (b.did.size() > 64) throw fail("'did' longo demais");
  for (const char* k : {"createdAt", "expiresAt"}) {
    if (!j.contains(k)) continue;
    if (!j[k].is_number_integer() || j[k].get<int64_t>() < 0 || j[k].get<int64_t>() > kMaxUnix)
      throw fail("'createdAt'/'expiresAt' devem ser inteiros entre 0 e 2^40 (Unix s)");
    (k[0] == 'c' ? b.createdAt : b.expiresAt) = j[k].get<int64_t>();
  }
  return b;
}

// Grava `text` em `file` de forma atômica: temporário + flush + fsync, rename, fsync da pasta (POSIX).
bool writeAtomic(const fs::path& file, const std::string& text, std::string& err) {
  fs::path tmp = file;
  tmp += ".tmp";
  std::error_code ec;
  if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
  {
#if defined(_WIN32)
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0) f = nullptr;
#else
    FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
    if (!f) {
      err = "não foi possível gravar '" + u8(tmp) + "'";
      return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#if defined(_WIN32)
    ok = ok && FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)))) != 0;
#else
    ok = ok && ::fsync(fileno(f)) == 0;
#endif
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
      err = "falha ao gravar '" + u8(tmp) + "'";
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    err = "não foi possível substituir '" + u8(file) + "': " + ec.message();
    return false;
  }
#if !defined(_WIN32)
  // POSIX: o rename só é durável depois do fsync da pasta.
  const fs::path dir = file.has_parent_path() ? file.parent_path() : fs::path(".");
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
#endif
  return true;
}

int64_t asInt(const DbValue& v) {
  if (v.type == DbValue::Type::Int) return v.i;
  if (v.type == DbValue::Type::Double) return static_cast<int64_t>(v.d);
  if (v.type == DbValue::Type::Text) {
    try {
      return std::stoll(v.s);
    } catch (...) {
    }
  }
  return 0;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ backends

struct BanStore::Backend {
  virtual ~Backend() = default;
  virtual const char* name() const = 0;
  // Depois de o cache mudar (thread de scripts). `existed`: havia ban no cache.
  virtual bool put(const Ban& b, std::string& err) = 0;
  virtual bool erase(const std::string& did, bool existed, std::string& err) = 0;
  virtual size_t pending() const { return 0; }
  virtual bool flush(milliseconds) { return true; }
};

namespace {

// bans.json (ou só memória com caminho vazio). Cópia própria do escritor (thread de scripts): o arquivo é regravado
// inteiro a cada mudança.
class JsonBackend final : public BanStore::Backend {
 public:
  JsonBackend(fs::path file, std::function<int64_t()> now) : file_(std::move(file)), now_(std::move(now)) {}
  const char* name() const override { return file_.empty() ? "memória" : "json"; }
  void seed(const Ban& b) { writer_[b.did] = b; }
  bool put(const Ban& b, std::string& err) override {
    writer_[b.did] = b;
    return persist(err);
  }
  bool erase(const std::string& did, bool, std::string& err) override {
    return writer_.erase(did) > 0 ? persist(err) : true;
  }

 private:
  bool persist(std::string& err) {
    if (file_.empty()) return true;
    const int64_t t = now_();
    std::erase_if(writer_, [t](const auto& kv) { return expired(kv.second, t); });
    std::vector<const Ban*> v;
    v.reserve(writer_.size());
    for (const auto& [d, b] : writer_) v.push_back(&b);
    std::sort(v.begin(), v.end(), [](const Ban* a, const Ban* b) { return a->did < b->did; });
    json arr = json::array();
    for (const Ban* b : v)
      arr.push_back({{"did", b->did}, {"reason", b->reason}, {"by", b->by}, {"createdAt", b->createdAt},
                     {"expiresAt", b->expiresAt}});
    return writeAtomic(file_, arr.dump(2, ' ', false, json::error_handler_t::replace) + "\n", err);
  }

  fs::path file_;
  std::function<int64_t()> now_;
  std::unordered_map<std::string, Ban> writer_;
};

constexpr const char* kCreate =
    "CREATE TABLE IF NOT EXISTS cb_bans ("
    "did VARCHAR(64) COLLATE utf8mb4_bin NOT NULL PRIMARY KEY, "
    "reason VARCHAR(255) NOT NULL DEFAULT '', "
    "created_at BIGINT NOT NULL, "
    "expires_at BIGINT NULL, "
    "created_by VARCHAR(128) NOT NULL DEFAULT '', "
    "KEY cb_bans_expires (expires_at)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
constexpr const char* kPurge = "DELETE FROM cb_bans WHERE expires_at IS NOT NULL AND expires_at <= ?";
// Carga em páginas por did (o pool limita cada resultado a 10 000 linhas / 8 MiB).
constexpr size_t kPage = 5000;
constexpr const char* kSelect =
    "SELECT did, reason, created_at, expires_at, created_by FROM cb_bans "
    "WHERE (expires_at IS NULL OR expires_at > ?) AND did > ? ORDER BY did LIMIT 5000";
constexpr const char* kReplace =
    "REPLACE INTO cb_bans (did, reason, created_at, expires_at, created_by) VALUES (?, ?, ?, ?, ?)";
constexpr const char* kDelete = "DELETE FROM cb_bans WHERE did = ?";

}  // namespace

// MySQL: thread própria que faz a carga inicial e depois grava, em ordem, as mudanças enfileiradas.
class BanMysqlBackend final : public BanStore::Backend {
 public:
  BanMysqlBackend(std::shared_ptr<DbPool> db, std::function<int64_t()> now, BanStoreDbOptions opt,
                  std::function<void(std::vector<Ban>)> merge)
      : db_(std::move(db)), now_(std::move(now)), opt_(std::move(opt)), merge_(std::move(merge)) {
    th_ = std::thread([this] { run(); });
  }
  ~BanMysqlBackend() override {
    size_t left = 0;
    {
      // Dá até 2 s para as gravações pendentes (só se o banco está respondendo: carga inicial feita).
      std::unique_lock<std::mutex> lk(mu_);
      if (loaded_) cv_.wait_for(lk, milliseconds(2000), [&] { return q_.empty(); });
      stop_ = true;
      left = q_.size();
      cv_.notify_all();
    }
    if (th_.joinable()) th_.join();
    if (left > 0)
      log(true, "bans: " + std::to_string(left) + " gravação(ões) não gravada(s) no banco (cb_bans) no encerramento");
  }
  const char* name() const override { return "mysql"; }
  bool put(const Ban& b, std::string&) override {
    push(Op{true, b});
    return true;
  }
  bool erase(const std::string& did, bool, std::string&) override {
    Ban b;
    b.did = did;
    push(Op{false, std::move(b)});  // mesmo sem ban no cache: o banco pode ter (carga ainda não terminou)
    return true;
  }
  size_t pending() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }
  bool flush(milliseconds limit) override {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, limit, [&] { return stop_ || (loaded_ && q_.empty()); }) && loaded_ && q_.empty();
  }

 private:
  struct Op {
    bool put = true;
    Ban b;
  };

  void log(bool error, const std::string& m) const {
    try {
      if (opt_.log)
        opt_.log(error, m);
      else if (error)
        spdlog::error("{}", m);
      else
        spdlog::warn("{}", m);
    } catch (...) {
    }
  }

  void push(Op op) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.size() >= opt_.maxPendingWrites && !q_.empty()) {
      // A da frente pode estar em andamento: descarta a seguinte (a mais antiga ainda não iniciada).
      const size_t at = q_.size() > 1 ? 1 : 0;
      log(true, "bans: fila de gravações cheia; descartada a gravação de '" + q_[at].b.did + "'");
      q_.erase(q_.begin() + static_cast<std::ptrdiff_t>(at));
    }
    q_.push_back(std::move(op));
    cv_.notify_all();
  }

  // Pedido síncrono (só nesta thread), com prazo; volta cedo se o backend for encerrado.
  DbResult call(DbRequest r) {
    auto p = std::make_shared<std::promise<DbResult>>();
    auto f = p->get_future();
    std::string err;
    if (db_->submit(std::move(r), [p](DbResult x) { p->set_value(std::move(x)); }, err) == 0)
      return DbResult::fail(err);
    const auto limit = std::chrono::steady_clock::now() + milliseconds(db_->config().timeoutMs) + std::chrono::seconds(2);
    while (f.wait_for(milliseconds(50)) != std::future_status::ready) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return DbResult::fail("encerrado");
      }
      if (std::chrono::steady_clock::now() > limit) return DbResult::fail("sem resposta do banco");
    }
    return f.get();
  }

  static DbRequest req(DbOp op, const char* sql, std::vector<DbValue> params = {}) {
    DbRequest r;
    r.op = op;
    r.sql = sql;
    r.params = std::move(params);
    return r;
  }

  bool load(std::string& err) {
    DbResult r = call(req(DbOp::Exec, kCreate));
    if (!r.ok) {
      err = r.error;
      return false;
    }
    const int64_t now = now_();
    r = call(req(DbOp::Exec, kPurge, {DbValue::integer(now)}));
    if (!r.ok) {
      err = r.error;
      return false;
    }
    std::vector<Ban> v;
    std::string after;
    for (;;) {
      r = call(req(DbOp::Query, kSelect, {DbValue::integer(now), DbValue::text(after)}));
      if (!r.ok) {
        err = r.error;
        return false;
      }
      for (const auto& row : r.rows) {
        if (row.size() < 5 || row[0].type != DbValue::Type::Text || row[0].s.empty()) continue;
        Ban b;
        b.did = row[0].s;
        b.reason = row[1].s;
        b.createdAt = asInt(row[2]);
        b.expiresAt = row[3].type == DbValue::Type::Null ? 0 : asInt(row[3]);
        b.by = row[4].s;
        v.push_back(std::move(b));
      }
      if (r.rows.size() < kPage || r.rows.back().empty()) break;
      after = r.rows.back()[0].s;
    }
    merge_(std::move(v));
    return true;
  }

  DbResult apply(const Op& op) {
    if (op.put) {
      const Ban& b = op.b;
      return call(req(DbOp::Exec, kReplace,
                      {DbValue::text(b.did), DbValue::text(b.reason), DbValue::integer(b.createdAt),
                       b.expiresAt != 0 ? DbValue::integer(b.expiresAt) : DbValue::null(), DbValue::text(b.by)}));
    }
    return call(req(DbOp::Exec, kDelete, {DbValue::text(op.b.did)}));
  }

  // Grava a fila em ordem até esvaziar (true) ou até uma falha transitória (false: tentar de novo depois). Erro
  // permanente (dbPermanentError): a gravação é descartada com log e auditoria, e a fila segue.
  bool drainWrites() {
    for (;;) {
      Op op;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_ || q_.empty()) return true;
        op = q_.front();
      }
      DbResult r = apply(op);
      if (!r.ok && !dbPermanentError(r)) {
        log(true, "bans: gravação de '" + op.b.did + "' em cb_bans falhou (nova tentativa): " + r.error);
        return false;
      }
      if (!r.ok) {
        log(true, "bans: gravação de '" + op.b.did + "' em cb_bans DESCARTADA (erro permanente " +
                      std::to_string(r.code) + "): " + r.error + " (o ban vale só até reiniciar)");
        if (opt_.audit) {
          try {
            opt_.audit(json{{"type", "ban_write_dropped"},
                            {"did", op.b.did},
                            {"op", op.put ? "ban" : "unban"},
                            {"error", r.error},
                            {"code", r.code},
                            {"ts", now_()}}
                           .dump(-1, ' ', false, json::error_handler_t::replace));
          } catch (...) {
          }
        }
      }
      std::lock_guard<std::mutex> lk(mu_);
      if (!q_.empty()) q_.pop_front();
      cv_.notify_all();
    }
  }

  // Espera crescente entre tentativas; false se o backend foi encerrado.
  bool backoff(milliseconds& b) {
    b = b.count() == 0 ? opt_.retryMin : std::min(opt_.retryMax, b * 2);
    std::unique_lock<std::mutex> lk(mu_);
    return !cv_.wait_for(lk, b, [&] { return stop_; });
  }

  void run() {
    milliseconds wait{0};
    int loadFails = 0;
    for (;;) {
      bool isLoaded;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        isLoaded = loaded_;
      }
      std::string err;
      if (!isLoaded) {
        if (load(err)) {
          std::lock_guard<std::mutex> lk(mu_);
          loaded_ = true;
          wait = milliseconds(0);
          cv_.notify_all();
          continue;
        }
        ++loadFails;
        log(true, "bans: carga de cb_bans falhou (nova tentativa em " +
                      std::to_string((wait.count() == 0 ? opt_.retryMin : std::min(opt_.retryMax, wait * 2)).count()) +
                      " ms): " + err);
        if (loadFails == 5)
          log(true, "CRÍTICO: bans: a carga de cb_bans falhou 5 vezes seguidas; os bans gravados no banco NÃO estão "
                    "sendo aplicados (só valem os desta execução) até a carga funcionar. Último erro: " + err);
        // As gravações não esperam a carga (mudanças locais vencem a carga quando ela vier).
        (void)drainWrites();
        if (!backoff(wait)) return;
        continue;
      }
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
        if (stop_) return;
      }
      if (drainWrites()) {
        wait = milliseconds(0);
      } else if (!backoff(wait)) {
        return;
      }
    }
  }

  std::shared_ptr<DbPool> db_;
  std::function<int64_t()> now_;
  BanStoreDbOptions opt_;
  std::function<void(std::vector<Ban>)> merge_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Op> q_;  // a da frente está em andamento (sai só depois de gravada)
  bool loaded_ = false;
  bool stop_ = false;
  std::thread th_;
};

// --------------------------------------------------------------------------------------------------- BanStore

int64_t BanStore::now() const {
  if (now_) return now_();
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

BanStore::BanStore(fs::path file, std::function<int64_t()> nowUnix) : now_(std::move(nowUnix)) {
  auto be = std::make_unique<JsonBackend>(file, [this] { return now(); });
  if (!file.empty()) {
    std::error_code ec;
    if (fs::exists(file, ec)) {
      std::ifstream f(file, std::ios::binary);
      if (!f) throw std::runtime_error("bans: não foi possível ler '" + u8(file) + "'");
      json j = json::parse(f, nullptr, false);
      if (j.is_discarded()) throw std::runtime_error("bans: JSON malformado em '" + u8(file) + "'");
      if (!j.is_array()) throw std::runtime_error("bans: '" + u8(file) + "' deve conter uma lista");
      const int64_t t = now();
      for (const auto& e : j) {
        Ban b = fromJson(e, u8(file));
        if (expired(b, t)) continue;
        be->seed(b);
        live_[b.did] = std::move(b);
      }
    }
  }
  be_ = std::move(be);
}

BanStore::BanStore(std::shared_ptr<DbPool> db, std::function<int64_t()> nowUnix, BanStoreDbOptions opt)
    : now_(std::move(nowUnix)) {
  if (!db) throw std::invalid_argument("bans: pool de banco nulo");
  loaded_ = false;
  be_ = std::make_unique<BanMysqlBackend>(std::move(db), [this] { return now(); }, std::move(opt),
                                          [this](std::vector<Ban> v) { merge(std::move(v)); });
}

BanStore::~BanStore() { be_.reset(); }  // antes dos membros que a thread do backend usa (mu_, live_)

void BanStore::merge(std::vector<Ban> loaded) {
  // O mapa novo é montado fora do mutex (a simulação lê o cache na entrada de cada jogador); sob o mutex só entram
  // as mudanças locais feitas antes da carga (poucas) e a troca.
  std::unordered_map<std::string, Ban> m;
  m.reserve(loaded.size() + 16);
  for (auto& b : loaded) {
    std::string d = b.did;
    m[std::move(d)] = std::move(b);
  }
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& d : touched_) {
    auto it = live_.find(d);
    if (it != live_.end())
      m[d] = it->second;
    else
      m.erase(d);
  }
  live_.swap(m);
  touched_.clear();
  loaded_ = true;
}

std::optional<Ban> BanStore::check(const std::string& did, int64_t nowUnix) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = live_.find(did);
  if (it == live_.end()) return std::nullopt;
  if (expired(it->second, nowUnix)) return std::nullopt;
  return it->second;
}

bool BanStore::add(Ban b, std::string& err) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!loaded_) touched_.insert(b.did);
    live_[b.did] = b;
  }
  return be_->put(b, err);
}

bool BanStore::remove(const std::string& did, bool& removed, std::string& err) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!loaded_) touched_.insert(did);
    removed = live_.erase(did) > 0;
  }
  return be_->erase(did, removed, err);
}

std::vector<Ban> BanStore::list() const {
  std::vector<Ban> v;
  {
    std::lock_guard<std::mutex> lk(mu_);
    v.reserve(live_.size());
    for (const auto& [d, b] : live_) v.push_back(b);
  }
  std::sort(v.begin(), v.end(), [](const Ban& a, const Ban& b) { return a.did < b.did; });
  return v;
}

const char* BanStore::backend() const { return be_->name(); }

bool BanStore::loaded() const {
  std::lock_guard<std::mutex> lk(mu_);
  return loaded_;
}

size_t BanStore::pendingWrites() const { return be_->pending(); }

bool BanStore::flush(milliseconds limit) { return be_->flush(limit); }

}  // namespace cbscript
