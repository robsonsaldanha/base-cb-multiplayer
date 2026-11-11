#pragma once
// Ambiente dos testes da API cb.*: ScriptHost real (relógio falso) + fila de comandos (simulação falsa).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "cbscript/api.hpp"
#include "cbscript/bans.hpp"
#include "cbscript/host.hpp"
#include "test_util.hpp"

namespace cbtest {

using namespace cbscript;
using State = ResourceStatus::State;

struct Clock {
  std::atomic<double> t{1000.0};
  double get() const { return t.load(); }
  void advance(double s) { t.store(t.load() + s); }
};

template <class P>
bool waitUntil(P pred, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
  auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

struct ApiEnv {
  TmpDir dir;
  std::shared_ptr<Clock> clock = std::make_shared<Clock>();
  mutable std::mutex mu;
  std::vector<std::pair<LogLevel, std::string>> logs;
  std::vector<std::string> audits;
  // deque, não vector: waitCmd devolve ponteiros para dentro de `cmds` e cada drain() seguinte acrescenta; num
  // vector a realocação invalidaria esses ponteiros (heap-use-after-free pego pelo ASan no CI Linux). push_back em
  // deque preserva referências aos elementos já guardados.
  std::deque<ScriptToSim> cmds;
  std::vector<std::string> replies;
  std::atomic<int64_t> unixTime{1'700'000'000};
  HostOptions opt;
  Limits lim = shortLimits();
  ApiOptions api;
  std::shared_ptr<BanStore> bans;
  std::unique_ptr<ScriptHost> host;

  ApiEnv() {
    opt.supervisorPeriod = std::chrono::milliseconds(5);
    opt.idleWait = std::chrono::milliseconds(2);
    opt.log = [this](LogLevel l, const std::string& m) {
      std::lock_guard<std::mutex> lk(mu);
      logs.emplace_back(l, m);
    };
    bans = std::make_shared<BanStore>(dir.p / "bans.json", [this] { return unixTime.load(); });
    api.bans = bans;
    api.audit = [this](const std::string& line) {
      std::lock_guard<std::mutex> lk(mu);
      audits.push_back(line);
    };
    api.unixNow = [this] { return unixTime.load(); };
  }
  ~ApiEnv() {
    if (host)
      guarded("destroy", [this] {
        host.reset();
        return 0;
      });
  }
  void addRes(const std::string& name, const std::string& code, const std::vector<std::string>& deps = {},
              nlohmann::json config = nlohmann::json::object(), nlohmann::json permissions = nullptr) {
    nlohmann::json j = {{"name", name},     {"version", "1.0.0"}, {"server", {"server/main.lua"}},
                        {"dependencies", deps}, {"config", config}};
    if (!permissions.is_null()) j["permissions"] = permissions;
    put(dir.p / "res" / name / "resource.json", j.dump());
    put(dir.p / "res" / name / "server" / "main.lua", code);
  }
  void boot() {
    installApi(opt, api);
    host = std::make_unique<ScriptHost>(dir.p / "res", lim, [c = clock] { return c->get(); }, opt);
    guarded("start", [this] {
      host->start();
      return 0;
    });
  }
  bool call(std::future<bool> f) {
    return guarded("control", [&f] { return f.get(); });
  }
  bool hasLog(const std::string& sub, LogLevel l = LogLevel::Info) const {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [lv, m] : logs)
      if (lv == l && contains(m, sub)) return true;
    return false;
  }
  bool waitLog(const std::string& sub, LogLevel l = LogLevel::Info,
               std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) const {
    return waitUntil([&] { return hasLog(sub, l); }, limit);
  }
  // Para testes que fazem E/S de disco dentro de uma chamada ao Lua (ex.: cb.players.ban grava bans.json com fsync
  // do arquivo e da pasta) e que NÃO testam a vigia. Com shortLimits() (aborto em 200 ms de tempo real) um fsync lento
  // (ASan + disco do runner do CI) aborta a chamada; o reinício do resource é agendado pelo relógio falso do host,
  // que não anda nestes testes, então o resource nunca volta e o waitLog estoura. Chamar antes de boot().
  void roomyWatchdog() {
    lim.warnAfter = std::chrono::seconds(20);
    lim.abortAfter = std::chrono::seconds(60);
  }
  // Prazo de espera dos testes com E/S de disco (bem abaixo do prazo do ctest).
  static constexpr std::chrono::milliseconds kDiskWait{60000};
  int countLog(const std::string& sub) const {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (auto& [lv, m] : logs) n += contains(m, sub) ? 1 : 0;
    return n;
  }
  // Posição (ordem) da primeira linha de log com `sub`; -1 se não houver.
  int logIndex(const std::string& sub) const {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < logs.size(); ++i)
      if (contains(logs[i].second, sub)) return static_cast<int>(i);
    return -1;
  }
  std::string allLogs() const {
    std::lock_guard<std::mutex> lk(mu);
    std::string s;
    for (auto& [lv, m] : logs) s += std::to_string(static_cast<int>(lv)) + ": " + m + "\n";
    return s;
  }
  std::vector<std::string> auditLines() const {
    std::lock_guard<std::mutex> lk(mu);
    return audits;
  }
  // Barreira: duas tarefas seguidas na thread de scripts garantem que tudo o que foi postado antes já foi
  // processado (tarefas rodam antes dos eventos da mesma volta do laço).
  void sync() {
    for (int i = 0; i < 2; ++i) {
      auto p = std::make_shared<std::promise<void>>();
      auto f = p->get_future();
      host->runOnScriptThread([p](ScriptContext&) { p->set_value(); });
      guarded("sync", [&f] {
        f.get();
        return 0;
      });
    }
  }
  void drain() {
    std::vector<ScriptToSim> out;
    host->drainCommands(out);
    for (auto& c : out) cmds.push_back(std::move(c));
  }
  // O ponteiro devolvido continua válido enquanto o ApiEnv existir (cmds só cresce, e é um deque).
  template <class T, class P>
  const T* waitCmd(P pred) {
    const T* found = nullptr;
    waitUntil([&] {
      drain();
      for (auto& c : cmds)
        if (const T* x = std::get_if<T>(&c); x && pred(*x)) {
          found = x;
          return true;
        }
      return false;
    });
    return found;
  }
  static uint32_t sessionOf(uint16_t id) { return 1000u + id; }
  void leave(uint16_t id, const std::string& why = "saiu") {
    REQUIRE(host->post(EvPlayerLeave{id, "d" + std::to_string(id), why, sessionOf(id)}));
  }
  void join(uint16_t id, const std::string& name, int32_t x = 0, int32_t y = 0, int32_t z = 0) {
    REQUIRE(host->post(EvPlayerJoin{PlayerView{id, name, "d" + std::to_string(id), "s" + std::to_string(id), x, y, z, 1, sessionOf(id)}}));
  }
  void event(uint16_t id, const std::string& name, const std::string& json = "{}") {
    REQUIRE(host->post(EvClientEvent{id, name, json, sessionOf(id)}));
  }
  void command(std::optional<uint16_t> from, const std::string& line, bool withReply = true) {
    EvCommand c;
    c.from = from;
    c.adminDid = from ? "" : "admin-did";
    c.line = line;
    if (withReply)
      c.reply = [this](std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        replies.push_back(std::move(s));
      };
    REQUIRE(host->post(std::move(c)));
  }
  bool hasReply(const std::string& s) const {
    std::lock_guard<std::mutex> lk(mu);
    return std::find(replies.begin(), replies.end(), s) != replies.end();
  }
  State state(const std::string& name) const {
    for (auto& s : host->status())
      if (s.name == name) return s.state;
    return State::Failed;
  }
};

}  // namespace cbtest

using namespace cbtest;
