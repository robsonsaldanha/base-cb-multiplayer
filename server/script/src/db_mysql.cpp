// Backend libmariadb (MariaDB Connector/C) do DbPool: uma conexão por objeto, usada só pela thread do pool dona dela.
// Só prepared statements (cache por conexão); resultado lido linha a linha (sem guardar tudo no cliente) e cortado
// nos limites; prazo no servidor (max_statement_time no MariaDB, max_execution_time no MySQL) e no cliente
// (timeout de leitura). Mensagens de erro vêm da libmariadb/servidor, que nunca incluem a senha.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <mysql.h>

#include "cbscript/db.hpp"

namespace cbscript {
namespace {

using Clock = std::chrono::steady_clock;

// Códigos (errmsg.h / mysqld_error.h), fixos aqui para não depender da versão dos cabeçalhos.
constexpr unsigned kCrConnectionError = 2002, kCrConnHostError = 2003, kCrServerGone = 2006, kCrServerLost = 2013,
                   kCrServerLostExtended = 2055, kCrSslConnection = 2026;
constexpr unsigned kErQueryInterrupted = 1317, kErStatementTimeout = 1969 /* MariaDB */, kErQueryTimeout = 3024 /* MySQL */;
constexpr size_t kStmtCache = 64;

bool connectionLost(unsigned e) {
  return e == kCrServerGone || e == kCrServerLost || e == kCrServerLostExtended || e == kCrConnectionError ||
         e == kCrConnHostError || e == kCrSslConnection;
}

bool isLocalHost(const std::string& h) { return h == "localhost" || h == "127.0.0.1" || h == "::1" || h == "[::1]"; }

std::once_flag gLibInit;

class MysqlConn final : public DbConnection {
 public:
  explicit MysqlConn(DbConfig cfg) : cfg_(std::move(cfg)) { mysql_thread_init(); }
  ~MysqlConn() override {
    close();
    mysql_thread_end();
  }

  bool connected() const override { return my_ != nullptr; }

  bool connect(std::string& err) override {
    close();
    my_ = mysql_init(nullptr);
    if (my_ == nullptr) {
      err = "sem memória (mysql_init)";
      return false;
    }
    const unsigned secs = static_cast<unsigned>(std::ceil(cfg_.timeoutMs / 1000.0));
    unsigned connectTimeout = std::clamp(secs, 1u, 10u);
    unsigned ioTimeout = std::max(1u, secs) + 1;  // reserva: o prazo de verdade é o do servidor
    mysql_options(my_, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    mysql_options(my_, MYSQL_OPT_READ_TIMEOUT, &ioTimeout);
    mysql_options(my_, MYSQL_OPT_WRITE_TIMEOUT, &ioTimeout);
    mysql_options(my_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    my_bool on = 1, off = 0;
    mysql_options(my_, MYSQL_REPORT_DATA_TRUNCATION, &on);
    DbConfig::Tls tls = cfg_.tls;
    if (tls == DbConfig::Tls::Auto) tls = isLocalHost(cfg_.host) ? DbConfig::Tls::Off : DbConfig::Tls::Verify;
    switch (tls) {
      case DbConfig::Tls::Off:
        mysql_options(my_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &off);
        break;
      case DbConfig::Tls::Required:
        mysql_options(my_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &off);
        mysql_options(my_, MYSQL_OPT_SSL_ENFORCE, &on);
        break;
      default:
        mysql_options(my_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &on);
        mysql_options(my_, MYSQL_OPT_SSL_ENFORCE, &on);
        break;
    }
    const char* db = cfg_.database.empty() ? nullptr : cfg_.database.c_str();
    if (mysql_real_connect(my_, cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(), db,
                           static_cast<unsigned>(cfg_.port), nullptr, 0) == nullptr) {
      err = std::string(mysql_error(my_)) + " (" + std::to_string(mysql_errno(my_)) + ")";
      mysql_close(my_);
      my_ = nullptr;
      return false;
    }
    const char* info = mysql_get_server_info(my_);
    mariadb_ = info != nullptr && std::strstr(info, "MariaDB") != nullptr;
    // Prazo no servidor (vale para a sessão inteira). Falha aqui não impede o uso.
    std::string set = mariadb_ ? "SET SESSION max_statement_time = " + std::to_string(cfg_.timeoutMs / 1000.0)
                               : "SET SESSION max_execution_time = " + std::to_string(cfg_.timeoutMs);
    if (mysql_real_query(my_, set.data(), static_cast<unsigned long>(set.size())) == 0) {
      if (MYSQL_RES* r = mysql_store_result(my_)) mysql_free_result(r);
    }
    return true;
  }

  void close() override {
    for (auto& [sql, st] : cache_) mysql_stmt_close(st.st);
    cache_.clear();
    if (my_ != nullptr) mysql_close(my_);
    my_ = nullptr;
  }

  bool ping() override { return my_ != nullptr && mysql_ping(my_) == 0; }

  DbResult run(const DbRequest& r, const DbLimits& lim, std::chrono::milliseconds timeout) override {
    if (my_ == nullptr) return DbResult::fail("sem conexão com o banco");
    t0_ = Clock::now();
    timeout_ = timeout;
    switch (r.op) {
      case DbOp::Begin:
        return simple("START TRANSACTION");
      case DbOp::Commit:
        return simple("COMMIT");
      case DbOp::Rollback:
        return simple("ROLLBACK");
      default:
        return prepared(r, lim);
    }
  }

 private:
  struct Cached {
    MYSQL_STMT* st = nullptr;
    uint64_t used = 0;
  };

  bool late() const { return Clock::now() - t0_ >= timeout_ - std::chrono::milliseconds(50); }

  DbResult failFrom(unsigned code, const char* msg) {
    const std::string m = std::string(msg ? msg : "erro") + " (" + std::to_string(code) + ")";
    const bool timeout = code == kErStatementTimeout || code == kErQueryTimeout ||
                         ((code == kErQueryInterrupted || code == kCrServerLost || code == kCrServerLostExtended) && late());
    if (connectionLost(code)) close();
    if (timeout) return DbResult::fail("tempo esgotado (" + std::to_string(cfg_.timeoutMs) + " ms): " + m, true);
    return DbResult::fail(m);
  }

  DbResult simple(const char* sql) {
    if (mysql_real_query(my_, sql, static_cast<unsigned long>(std::strlen(sql))) != 0)
      return failFrom(mysql_errno(my_), mysql_error(my_));
    if (MYSQL_RES* res = mysql_store_result(my_)) mysql_free_result(res);
    DbResult out;
    out.ok = true;
    return out;
  }

  MYSQL_STMT* stmtFor(const std::string& sql, DbResult& fail) {
    auto it = cache_.find(sql);
    if (it != cache_.end()) {
      it->second.used = ++tick_;
      return it->second.st;
    }
    if (cache_.size() >= kStmtCache) {  // descarta o menos usado
      auto lru = std::min_element(cache_.begin(), cache_.end(),
                                  [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      mysql_stmt_close(lru->second.st);
      cache_.erase(lru);
    }
    MYSQL_STMT* st = mysql_stmt_init(my_);
    if (st == nullptr) {
      fail = DbResult::fail("sem memória (mysql_stmt_init)");
      return nullptr;
    }
    if (mysql_stmt_prepare(st, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
      const unsigned code = mysql_stmt_errno(st);
      const std::string msg = mysql_stmt_error(st);
      mysql_stmt_close(st);
      fail = failFrom(code, msg.c_str());
      return nullptr;
    }
    cache_[sql] = Cached{st, ++tick_};
    return st;
  }

  // Erro num statement em cache: tira do cache (pode ter ficado inválido) e devolve a falha.
  DbResult stmtFail(const std::string& sql, MYSQL_STMT* st) {
    const unsigned code = mysql_stmt_errno(st);
    const std::string msg = mysql_stmt_error(st);
    if (my_ != nullptr) {
      auto it = cache_.find(sql);
      if (it != cache_.end() && it->second.st == st) {
        mysql_stmt_close(st);
        cache_.erase(it);
      }
    }
    return failFrom(code, msg.c_str());
  }

  enum class Kind { Int, Double, Time, Str, Bit };
  struct Col {
    Kind kind = Kind::Str;
    DbTimeKind tk = DbTimeKind::DateTime;
    bool isUnsigned = false;
    unsigned decimals = 0;
    long long i = 0;
    double d = 0;
    MYSQL_TIME t{};
    unsigned long len = 0;
    my_bool isNull = 0, error = 0;
  };

  DbResult prepared(const DbRequest& r, const DbLimits& lim) {
    DbResult fail;
    MYSQL_STMT* st = stmtFor(r.sql, fail);
    if (st == nullptr) return fail;
    const unsigned long pc = mysql_stmt_param_count(st);
    if (pc != r.params.size())
      return DbResult::fail("o servidor contou " + std::to_string(pc) + " parâmetro(s) na consulta, mas recebeu " +
                            std::to_string(r.params.size()));
    std::vector<MYSQL_BIND> pb(r.params.size());
    std::vector<long long> ints(r.params.size());
    std::vector<double> dbls(r.params.size());
    std::vector<unsigned long> lens(r.params.size());
    for (size_t k = 0; k < r.params.size(); ++k) {
      const DbValue& v = r.params[k];
      MYSQL_BIND& b = pb[k];
      std::memset(&b, 0, sizeof b);
      switch (v.type) {
        case DbValue::Type::Null:
          b.buffer_type = MYSQL_TYPE_NULL;
          break;
        case DbValue::Type::Int:
          ints[k] = v.i;
          b.buffer_type = MYSQL_TYPE_LONGLONG;
          b.buffer = &ints[k];
          break;
        case DbValue::Type::Double:
          dbls[k] = v.d;
          b.buffer_type = MYSQL_TYPE_DOUBLE;
          b.buffer = &dbls[k];
          break;
        case DbValue::Type::Text:
          lens[k] = static_cast<unsigned long>(v.s.size());
          b.buffer_type = MYSQL_TYPE_STRING;
          b.buffer = const_cast<char*>(v.s.data());
          b.buffer_length = lens[k];
          b.length = &lens[k];
          break;
      }
    }
    if (!pb.empty() && mysql_stmt_bind_param(st, pb.data()) != 0) return stmtFail(r.sql, st);
    if (mysql_stmt_execute(st) != 0) return stmtFail(r.sql, st);

    DbResult out;
    out.ok = true;
    MYSQL_RES* meta = mysql_stmt_result_metadata(st);
    if (meta != nullptr) {
      const unsigned nf = mysql_num_fields(meta);
      MYSQL_FIELD* fields = mysql_fetch_fields(meta);
      std::vector<Col> cols(nf);
      std::vector<MYSQL_BIND> rb(nf);
      out.columns.reserve(nf);
      for (unsigned c = 0; c < nf; ++c) {
        const MYSQL_FIELD& f = fields[c];
        out.columns.emplace_back(f.name ? std::string(f.name, f.name_length) : std::string());
        Col& col = cols[c];
        MYSQL_BIND& b = rb[c];
        std::memset(&b, 0, sizeof b);
        b.is_null = &col.isNull;
        b.error = &col.error;
        b.length = &col.len;
        switch (f.type) {
          case MYSQL_TYPE_TINY:
          case MYSQL_TYPE_SHORT:
          case MYSQL_TYPE_INT24:
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_LONGLONG:
          case MYSQL_TYPE_YEAR:
            col.kind = Kind::Int;
            col.isUnsigned = (f.flags & UNSIGNED_FLAG) != 0;
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer = &col.i;
            b.is_unsigned = col.isUnsigned ? 1 : 0;
            break;
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DOUBLE:
            col.kind = Kind::Double;
            b.buffer_type = MYSQL_TYPE_DOUBLE;
            b.buffer = &col.d;
            break;
          case MYSQL_TYPE_DATE:
          case MYSQL_TYPE_NEWDATE:
          case MYSQL_TYPE_TIME:
          case MYSQL_TYPE_DATETIME:
          case MYSQL_TYPE_TIMESTAMP:
            col.kind = Kind::Time;
            col.tk = (f.type == MYSQL_TYPE_DATE || f.type == MYSQL_TYPE_NEWDATE) ? DbTimeKind::Date
                     : f.type == MYSQL_TYPE_TIME                                ? DbTimeKind::Time
                                                                                : DbTimeKind::DateTime;
            col.decimals = f.decimals <= 6 ? f.decimals : 0;
            b.buffer_type = f.type == MYSQL_TYPE_NEWDATE ? MYSQL_TYPE_DATE : f.type;
            b.buffer = &col.t;
            b.buffer_length = sizeof col.t;
            break;
          case MYSQL_TYPE_BIT:
            col.kind = Kind::Bit;
            b.buffer_type = MYSQL_TYPE_STRING;  // bytes; tamanho real em col.len (buffer 0: lido depois)
            break;
          default:  // DECIMAL, textos, BLOB, JSON, ENUM, SET, NULL...
            col.kind = Kind::Str;
            b.buffer_type = MYSQL_TYPE_STRING;
            break;
        }
      }
      if (mysql_stmt_bind_result(st, rb.data()) != 0) {
        mysql_free_result(meta);
        return stmtFail(r.sql, st);
      }
      size_t bytes = 0;
      for (;;) {
        const int rc = mysql_stmt_fetch(st);
        if (rc == MYSQL_NO_DATA) break;
        if (rc == 1) {
          mysql_free_result(meta);
          return stmtFail(r.sql, st);
        }
        if (out.rows.size() >= lim.maxRows) {
          mysql_free_result(meta);
          close();  // corta o envio do resto (reconecta no próximo pedido)
          return DbResult::fail(dbTooBig(lim));
        }
        std::vector<DbValue> row;
        row.reserve(nf);
        for (unsigned c = 0; c < nf; ++c) {
          Col& col = cols[c];
          if (col.isNull) {
            row.push_back(DbValue::null());
            bytes += 16;
            continue;
          }
          switch (col.kind) {
            case Kind::Int:
              if (col.isUnsigned &&
                  static_cast<unsigned long long>(col.i) > static_cast<unsigned long long>(std::numeric_limits<int64_t>::max()))
                row.push_back(DbValue::text(std::to_string(static_cast<unsigned long long>(col.i))));
              else
                row.push_back(DbValue::integer(static_cast<int64_t>(col.i)));
              break;
            case Kind::Double:
              row.push_back(DbValue::number(col.d));
              break;
            case Kind::Time:
              row.push_back(DbValue::text(formatDbTime(col.tk, col.t.neg != 0, col.t.year, col.t.month, col.t.day,
                                                       col.t.hour, col.t.minute, col.t.second, col.t.second_part,
                                                       col.decimals)));
              break;
            case Kind::Str:
            case Kind::Bit: {
              if (bytes + 16 + col.len > lim.maxBytes) {
                mysql_free_result(meta);
                close();
                return DbResult::fail(dbTooBig(lim));
              }
              std::string s(col.len, '\0');
              if (col.len > 0) {
                unsigned long got = 0;
                MYSQL_BIND b;
                std::memset(&b, 0, sizeof b);
                b.buffer_type = MYSQL_TYPE_STRING;
                b.buffer = s.data();
                b.buffer_length = col.len;
                b.length = &got;
                if (mysql_stmt_fetch_column(st, &b, c, 0) != 0) {
                  mysql_free_result(meta);
                  return stmtFail(r.sql, st);
                }
              }
              if (col.kind == Kind::Bit) {
                uint64_t v = 0;
                for (unsigned char ch : s) v = (v << 8) | ch;
                row.push_back(DbValue::integer(static_cast<int64_t>(v)));
              } else {
                row.push_back(DbValue::text(std::move(s)));
              }
              break;
            }
          }
          bytes += 16 + row.back().s.size();
        }
        if (bytes > lim.maxBytes) {
          mysql_free_result(meta);
          close();
          return DbResult::fail(dbTooBig(lim));
        }
        out.rows.push_back(std::move(row));
      }
      mysql_free_result(meta);
      out.affected = static_cast<int64_t>(out.rows.size());
    } else {
      const my_ulonglong a = mysql_stmt_affected_rows(st);
      out.affected = a == static_cast<my_ulonglong>(-1) ? 0 : static_cast<int64_t>(a);
    }
    out.insertId = static_cast<int64_t>(mysql_stmt_insert_id(st));
    mysql_stmt_free_result(st);
    return out;
  }

  DbConfig cfg_;
  MYSQL* my_ = nullptr;
  bool mariadb_ = false;
  std::unordered_map<std::string, Cached> cache_;
  uint64_t tick_ = 0;
  Clock::time_point t0_{};
  std::chrono::milliseconds timeout_{0};
};

}  // namespace

DbConnectionFactory mysqlConnectionFactory(DbConfig cfg) {
  // mysql_library_init não é thread-safe: uma vez, antes das threads do pool.
  std::call_once(gLibInit, [] { mysql_library_init(0, nullptr, nullptr); });
  return [cfg = std::move(cfg)] { return std::make_unique<MysqlConn>(cfg); };
}

}  // namespace cbscript
