#include "cbscript/host.hpp"

#include <cbmp/safe_path.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

// Decisões (ver também host.hpp e .superpowers/sdd/sub3/task-3-report.md):
// - Mutexes vistos pela simulação (qMu, cmdMu, playersMu, statusMu, beatMu) só protegem trocas de fila/ponteiro ou
//   cópias pequenas; a thread de scripts nunca segura nenhum deles enquanto chama o Lua ou um gancho.
// - Abortos por tempo detectados dentro de um gancho/tarefa são adiados (pendingTimeouts) até o gancho voltar: parar
//   um resource no meio da iteração de running() invalidaria o que o despachante está percorrendo.
// - Back-off: o n-ésimo aborto dentro da janela agenda o reinício para agora + base·2^(n-1) (relógio injetado);
//   no maxTimeouts-ésimo o resource fica Failed e só volta por ação do admin. Quem depende de um resource parado
//   ou aguardando reinício entra na lista bringBack dele (em cadeia) e volta quando ele subir.
// - Supervisor: batimento = pilha de chamadas abertas + instante do último início/fim (qualquer nível), sob beatMu.
// - Abandono: ~ScriptHost com chamada presa marca `abandoned` (sob beatMu, atômico em relação ao fim da chamada) e
//   desanexa a thread; quando a chamada voltar, endCall()/log() a estacionam para sempre.

namespace cbscript {

namespace fs = std::filesystem;
using State = ResourceStatus::State;

namespace {

std::string u8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

bool validName(const std::string& n) { return n.size() <= 64 && cbmp::safeSegment(n); }

std::string secs(double s) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", s);
  return b;
}

std::atomic<size_t> g_parked{0};

// Estaciona a thread atual para sempre (cv nunca sinalizada; objetos vazados para não depender da saída do processo).
[[noreturn]] void parkForever() {
  static auto* m = new std::mutex;
  static auto* cv = new std::condition_variable;
  std::unique_lock<std::mutex> lk(*m);
  g_parked.fetch_add(1);
  for (;;) cv->wait(lk);
}

constexpr size_t kBackstopFactor = 4;

}  // namespace

namespace detail {

struct Task {
  std::function<void(ScriptContext&)> run;
  std::function<void()> cancel;  // host parado ou destruído antes de rodar
};

// Promessa de um pedido de controle; resolvida uma única vez (pela tarefa, pelo cancelamento ou pelo supervisor).
struct Ctl {
  std::promise<bool> p;
  std::atomic<bool> done{false};
  void resolve(bool v) {
    if (done.exchange(true)) return;
    try {
      p.set_value(v);
    } catch (const std::future_error&) {
    }
  }
};

enum class Start { Up, Failed, Waiting };

struct Slot {
  HostedResource h;
  fs::path dir;
  State state = State::Stopped;
  std::string lastError;
  std::string fixedError;              // manifest inválido/duplicado na varredura
  std::deque<double> timeouts;         // instantes (relógio injetado) dos abortos por tempo
  std::optional<double> restartAt;     // reinício automático agendado
  std::vector<std::string> bringBack;  // dependentes a (re)subir quando este subir
  bool timeoutPending = false;
  bool stopping = false;
  std::chrono::steady_clock::duration busy{};
  double cpuMsPerSec = 0;

  void setManifest(ResourceManifest m) { h.manifest_ = std::move(m); }
};

struct HostImpl final : ScriptContext {
  // ---- configuração (imutável depois do construtor) ----
  fs::path dir;
  Limits limits;
  std::function<double()> clock;
  HostOptions opt;
  double abortSec = 0;

  // ---- fila Sim→Script e tarefas (qMu) ----
  mutable std::mutex qMu;
  std::condition_variable qCv;
  std::deque<SimToScript> events;
  std::deque<Task> tasks;
  bool stopReq = false;
  // Eventos aceitos e ainda não consumidos: na fila + no lote em processamento (a capacidade e o teto contam ambos).
  std::atomic<size_t> pending{0};
  std::atomic<bool> backstopLogged{false};
  std::atomic<uint64_t> eventsDropped{0}, backstopDropped{0};

  // ---- pedidos de controle em aberto (ctlMu) ----
  std::mutex ctlMu;
  std::vector<std::weak_ptr<Ctl>> ctls;

  // ---- fila Script→Sim (cmdMu) ----
  std::mutex cmdMu;
  std::vector<ScriptToSim> cmds;
  std::atomic<uint64_t> cmdsDropped{0};

  // ---- foto dos jogadores (playersMu: só a troca do ponteiro) ----
  mutable std::mutex playersMu;
  PlayersSnapshot players_ = std::make_shared<const std::vector<PlayerView>>();

  // ---- status publicado (statusMu) ----
  mutable std::mutex statusMu;
  std::vector<ResourceStatus> statusSnap;
  std::string haltReason;
  std::atomic<bool> halted{false};

  // ---- batimento (beatMu) ----
  mutable std::mutex beatMu;
  std::vector<std::string> callStack;  // chamadas ao Lua abertas (resource de cada nível)
  double lastBeat = 0;                 // último início/fim de chamada, qualquer nível
  uint64_t beatSeq = 0;
  std::atomic<bool> abandoned_{false};  // escrito sob beatMu

  // ---- threads ----
  std::thread scriptThread, supThread;
  std::atomic<std::thread::id> scriptId{};
  std::atomic<bool> started{false}, finished{false};
  std::mutex supMu;
  std::condition_variable supCv;
  bool supStop = false;

  // ---- só na thread de scripts ----
  std::map<std::string, std::unique_ptr<Slot>> slots;
  std::set<fs::path> knownDirs;
  std::vector<Slot*> order;  // rodando, na ordem em que subiram
  std::vector<Slot*> pendingTimeouts;
  int depth = 0;
  std::chrono::steady_clock::time_point cpuWindow = std::chrono::steady_clock::now();

  HostImpl(fs::path d, Limits l, std::function<double()> now, HostOptions o)
      : dir(std::move(d)), limits(l), clock(std::move(now)), opt(std::move(o)) {
    abortSec = std::chrono::duration<double>(limits.abortAfter).count();
  }

  // ================= ScriptContext =================
  PlayersSnapshot players() const override {
    std::lock_guard<std::mutex> lk(playersMu);
    return players_;
  }
  bool emit(ScriptToSim cmd) override {
    std::lock_guard<std::mutex> lk(cmdMu);
    if (cmds.size() >= opt.commandQueueCapacity) {
      cmdsDropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    cmds.push_back(std::move(cmd));
    return true;
  }
  std::vector<HostedResource*> running() override {
    std::vector<HostedResource*> v;
    v.reserve(order.size());
    for (Slot* s : order)
      if (s->h.running_) v.push_back(&s->h);
    return v;
  }
  HostedResource* find(std::string_view name) override {
    auto it = slots.find(std::string(name));
    return it == slots.end() ? nullptr : &it->second->h;
  }
  Resource::CallResult invoke(HostedResource& r, const sol::reference& fn,
                              std::span<const sol::object> args) override {
    return guarded(r, [&](Resource& res) { return res.call(fn, args); });
  }
  Resource::CallResult resume(HostedResource& r, const sol::thread& co, std::span<const sol::object> args) override {
    return guarded(r, [&](Resource& res) { return res.resume(co, args); });
  }
  double now() const override { return clock(); }
  bool abandoned() const override { return abandoned_.load(); }
  void log(LogLevel lv, const std::string& msg) override {
    if (abandoned_.load()) {
      // Host destruído: nada de log. A thread de scripts (voltando de uma chamada presa) estaciona aqui.
      if (std::this_thread::get_id() == scriptId.load()) parkForever();
      return;
    }
    try {
      if (opt.log) {
        opt.log(lv, msg);
        return;
      }
      switch (lv) {
        case LogLevel::Info: spdlog::info("{}", msg); break;
        case LogLevel::Warn: spdlog::warn("{}", msg); break;
        case LogLevel::Error: spdlog::error("{}", msg); break;
        case LogLevel::Critical: spdlog::critical("{}", msg); break;
      }
    } catch (...) {
    }
  }

  // Roda um gancho/tarefa do usuário; exceções viram log (nunca derrubam a thread de scripts).
  template <class F>
  bool safely(const std::string& what, F&& f) {
    try {
      f();
      return true;
    } catch (const std::exception& e) {
      log(LogLevel::Error, what + ": " + e.what());
    } catch (...) {
      log(LogLevel::Error, what + ": exceção desconhecida");
    }
    return false;
  }

  // ================= batimento =================
  bool beginCall(const std::string& name) {
    const bool outer = depth++ == 0;
    std::lock_guard<std::mutex> lk(beatMu);
    callStack.push_back(name);
    lastBeat = clock();
    ++beatSeq;
    return outer;
  }
  void endCall() {
    --depth;
    std::unique_lock<std::mutex> lk(beatMu);
    if (abandoned_.load()) {
      lk.unlock();
      parkForever();  // a chamada presa voltou depois de ~ScriptHost: não toca em mais nada
    }
    callStack.pop_back();
    lastBeat = clock();
    ++beatSeq;
  }

  template <class F>
  Resource::CallResult guarded(HostedResource& r, F&& f) {
    Resource::CallResult res;
    auto it = slots.find(r.name());
    if (halted.load()) {
      res.error = "[" + r.name() + "] host de scripts parado";
      return res;
    }
    if (it == slots.end() || &it->second->h != &r || !r.res_ || !r.running_ || it->second->timeoutPending) {
      res.error = "[" + r.name() + "] resource não está rodando";
      return res;
    }
    Slot& s = *it->second;
    const bool outer = beginCall(r.name());
    const auto t0 = std::chrono::steady_clock::now();
    res = f(*r.res_);
    if (outer) s.busy += std::chrono::steady_clock::now() - t0;
    endCall();
    if (halted.load()) return res;
    if (!res.ok) {
      log(LogLevel::Error, res.error);
      if (res.timedOut && !s.stopping && !s.timeoutPending) {
        s.timeoutPending = true;
        pendingTimeouts.push_back(&s);
      }
    }
    return res;
  }

  // ================= status =================
  void publishStatus() {
    const auto now = std::chrono::steady_clock::now();
    const auto win = now - cpuWindow;
    if (win >= std::chrono::seconds(1)) {
      const double winSec = std::chrono::duration<double>(win).count();
      for (auto& [n, s] : slots) {
        s->cpuMsPerSec = std::chrono::duration<double, std::milli>(s->busy).count() / winSec;
        s->busy = {};
      }
      cpuWindow = now;
    }
    std::vector<ResourceStatus> v;
    v.reserve(slots.size());
    for (auto& [n, s] : slots)
      v.push_back(ResourceStatus{n, s->state, s->lastError, s->cpuMsPerSec, s->h.res_ ? s->h.res_->memoryUsed() : 0});
    std::lock_guard<std::mutex> lk(statusMu);
    if (halted.load()) return;  // o supervisor já escreveu o estado final
    statusSnap = std::move(v);
  }

  // ================= descoberta =================
  Slot& slotFor(const std::string& name) {
    auto& p = slots[name];
    if (!p) {
      p = std::make_unique<Slot>();
      ResourceManifest m;
      m.name = name;
      p->setManifest(std::move(m));
    }
    return *p;
  }

  // Varre a pasta e cria slots para pastas ainda não vistas. Manifest inválido → slot com o nome da pasta, Failed.
  void scan() {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      log(LogLevel::Warn, "pasta de resources não encontrada: " + u8(dir));
      return;
    }
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_directory(e2) && fs::exists(it->path() / "resource.json", e2)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (auto& d : dirs) {
      if (knownDirs.count(d)) continue;
      knownDirs.insert(d);
      const std::string folder = u8(d.filename());
      try {
        ResourceManifest m = loadManifest(d);
        auto it = slots.find(m.name);
        if (it != slots.end() && !it->second->dir.empty()) {
          Slot& s = *it->second;
          s.fixedError = "resource duplicado: '" + m.name + "' nas pastas '" + u8(s.dir.filename()) + "' e '" +
                         folder + "'";
          fail(s, s.fixedError);
          continue;
        }
        Slot& s = slotFor(m.name);
        s.dir = d;
        s.setManifest(std::move(m));
      } catch (const std::exception& e) {
        if (!validName(folder)) {
          log(LogLevel::Error, "resource ignorado (pasta com nome inválido '" + folder + "'): " + e.what());
          continue;
        }
        Slot& s = slotFor(folder);
        s.dir = d;
        s.fixedError = e.what();
        fail(s, s.fixedError);
      }
    }
  }

  Slot* findOrDiscover(const std::string& name) {
    auto it = slots.find(name);
    if (it != slots.end()) return it->second.get();
    if (!validName(name)) return nullptr;
    scan();
    it = slots.find(name);
    return it == slots.end() ? nullptr : it->second.get();
  }

  // ================= ciclo de vida =================
  void fail(Slot& s, const std::string& msg, bool logIt = true) {
    s.state = State::Failed;
    s.lastError = msg;
    s.restartAt.reset();
    if (logIt) log(LogLevel::Error, "[" + s.h.name() + "] " + msg);
    abandonBringBack(s, 0);
  }
  // s não vai mais subir sozinho: quem esperava por ele (em cadeia) fica Stopped com o motivo.
  void abandonBringBack(Slot& s, int level) {
    auto back = std::move(s.bringBack);
    s.bringBack.clear();
    for (auto& b : back) {
      auto it = slots.find(b);
      if (it == slots.end() || it->second->h.running_) continue;
      Slot& d = *it->second;
      if (d.state != State::Failed) {
        d.state = State::Stopped;
        d.lastError = "parado: a dependência '" + s.h.name() + "' falhou";
      }
      if (level < 64) abandonBringBack(d, level + 1);
    }
  }
  void addBringBack(Slot& s, const std::vector<std::string>& names) {
    for (auto& n : names)
      if (n != s.h.name() && std::find(s.bringBack.begin(), s.bringBack.end(), n) == s.bringBack.end())
        s.bringBack.push_back(n);
  }

  void destroyState(Slot& s) {
    s.h.running_ = false;
    s.h.api.reset();
    s.h.res_.reset();
    order.erase(std::remove(order.begin(), order.end(), &s), order.end());
  }

  // Dispara onStopping (resource rodando) e fecha o estado.
  void teardown(Slot& s) {
    if (s.h.running_ && opt.onStopping && !halted.load()) {
      s.stopping = true;
      safely("[" + s.h.name() + "] onStopping", [&] { opt.onStopping(*this, s.h); });
      s.stopping = false;
    }
    destroyState(s);
  }

  // Para s e, antes, os dependentes que estiverem rodando. Devolve os dependentes parados (na ordem de parada).
  std::vector<std::string> stopOne(Slot& s) {
    std::vector<std::string> stopped;
    std::vector<std::string> deps;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const auto& d = (*it)->h.manifest().dependencies;
      if ((*it)->h.running_ && std::find(d.begin(), d.end(), s.h.name()) != d.end()) deps.push_back((*it)->h.name());
    }
    for (auto& n : deps) {
      Slot& d = *slots.at(n);
      if (!d.h.running_) continue;
      auto sub = stopOne(d);
      stopped.insert(stopped.end(), sub.begin(), sub.end());
      stopped.push_back(n);
      d.state = State::Stopped;
      d.lastError = "parado junto com a dependência '" + s.h.name() + "'";
    }
    teardown(s);
    if (s.state == State::Running) s.state = State::Stopped;
    return stopped;
  }

  // Cria o estado, roda os scripts e dispara onStarted. Em sucesso (re)sobe quem esperava por ele (bringBack).
  Start launch(Slot& s) {
    if (s.h.running_) return Start::Up;
    const std::string name = s.h.name();
    try {
      s.h.res_ = std::make_unique<Resource>(s.h.manifest_, s.dir, limits,
                                            [this](std::string m) { log(LogLevel::Warn, m); });
    } catch (const std::exception& e) {
      destroyState(s);
      fail(s, std::string("falha ao criar o estado Lua: ") + e.what());
      return Start::Failed;
    } catch (...) {
      destroyState(s);
      fail(s, "falha ao criar o estado Lua");
      return Start::Failed;
    }
    if (opt.onSetup && !safely("[" + name + "] onSetup", [&] { opt.onSetup(*this, s.h); })) {
      destroyState(s);
      fail(s, "falha ao preparar a API (onSetup)", false);
      return Start::Failed;
    }
    std::string err;
    bool timedOut = false;
    const bool outer = beginCall(name);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = s.h.res_->runMain(err, &timedOut);
    if (outer) s.busy += std::chrono::steady_clock::now() - t0;
    endCall();
    if (halted.load()) return Start::Failed;
    if (!ok) {
      log(LogLevel::Error, err);
      destroyState(s);
      if (timedOut)
        handleTimeout(s);
      else
        fail(s, err, false);
      publishStatus();
      return s.restartAt ? Start::Waiting : Start::Failed;
    }
    s.h.running_ = true;
    s.state = State::Running;
    s.lastError.clear();
    order.push_back(&s);
    log(LogLevel::Info, "[" + name + "] iniciado");
    if (opt.onStarted) safely("[" + name + "] onStarted", [&] { opt.onStarted(*this, s.h); });
    publishStatus();
    auto back = std::move(s.bringBack);
    s.bringBack.clear();
    for (auto& b : back) {
      auto it = slots.find(b);
      if (it == slots.end() || it->second->h.running_ || it->second->state == State::Failed) continue;
      startFresh(b);
    }
    return Start::Up;
  }

  Start startFresh(const std::string& name) {
    std::vector<std::string> chain;
    std::set<std::string> failedNow;
    return startOne(name, chain, failedNow);
  }

  // Sobe `name` (e, antes, as dependências). `chain` detecta ciclos; `failedNow` evita sobrescrever a causa original.
  // Waiting: algo na cadeia aguarda reinício; `name` entrou no bringBack da dependência e sobe quando ela subir.
  Start startOne(const std::string& name, std::vector<std::string>& chain, std::set<std::string>& failedNow) {
    Slot* sp = findOrDiscover(name);
    if (sp == nullptr) return Start::Failed;
    Slot& s = *sp;
    if (s.h.running_) return Start::Up;
    if (failedNow.count(name) || halted.load()) return Start::Failed;
    if (s.restartAt) return Start::Waiting;  // reinício próprio agendado (start manual zera antes de chamar)
    auto inChain = std::find(chain.begin(), chain.end(), name);
    if (inChain != chain.end()) {
      std::string cyc;
      for (auto it = inChain; it != chain.end(); ++it) cyc += *it + " -> ";
      cyc += name;
      for (auto it = inChain; it != chain.end(); ++it) {
        fail(*slots.at(*it), "ciclo de dependências: " + cyc);
        failedNow.insert(*it);
      }
      publishStatus();
      return Start::Failed;
    }
    auto failHere = [&](const std::string& msg) {
      if (!failedNow.count(name)) fail(s, msg);
      failedNow.insert(name);
      publishStatus();
      return Start::Failed;
    };
    if (!s.fixedError.empty()) return failHere(s.fixedError);
    try {
      ResourceManifest m = loadManifest(s.dir);
      if (m.name != name) throw std::runtime_error("o nome em resource.json mudou para '" + m.name + "'");
      s.setManifest(std::move(m));
    } catch (const std::exception& e) {
      return failHere(e.what());
    }
    chain.push_back(name);
    const std::vector<std::string> deps = s.h.manifest().dependencies;
    for (const auto& dep : deps) {
      Slot* d = findOrDiscover(dep);
      if (d == nullptr) {
        chain.pop_back();
        return failHere("dependência ausente: '" + dep + "'");
      }
      const Start r = startOne(dep, chain, failedNow);
      if (r == Start::Waiting) {
        chain.pop_back();
        addBringBack(*d, {name});
        s.state = State::Stopped;
        s.lastError = "aguardando a dependência '" + dep + "' reiniciar";
        publishStatus();
        return Start::Waiting;
      }
      if (r == Start::Failed) {
        chain.pop_back();
        if (failedNow.count(name)) return Start::Failed;  // já marcado (ciclo)
        return failHere("dependência '" + dep + "' não subiu");
      }
    }
    chain.pop_back();
    if (s.h.running_) return Start::Up;  // subiu no meio (bringBack de uma dependência)
    const Start r = launch(s);
    if (r == Start::Failed && s.state == State::Failed) failedNow.insert(name);
    return r;
  }

  void handleTimeout(Slot& s) {
    s.timeoutPending = false;
    const double t = clock();
    while (!s.timeouts.empty() && t - s.timeouts.front() >= opt.timeoutWindowSec) s.timeouts.pop_front();
    s.timeouts.push_back(t);
    const size_t n = s.timeouts.size();
    if (s.h.res_) addBringBack(s, stopOne(s));
    if (static_cast<int>(n) >= opt.maxTimeouts) {
      fail(s, "abortado por tempo " + std::to_string(n) + " vezes em " + secs(opt.timeoutWindowSec) +
                  " s; parado (sem novas tentativas até um start/restart manual)");
      return;
    }
    const double delay =
        std::min(opt.backoffBaseSec * std::pow(2.0, static_cast<double>(n) - 1.0), opt.backoffMaxSec);
    s.state = State::Stopped;
    s.restartAt = t + delay;
    s.lastError = "abortado por tempo (" + std::to_string(n) + "/" + std::to_string(opt.maxTimeouts) + " em " +
                  secs(opt.timeoutWindowSec) + " s); reinício em " + secs(delay) + " s";
    log(LogLevel::Warn, "[" + s.h.name() + "] " + s.lastError);
  }

  void settle() {
    while (!pendingTimeouts.empty() && !halted.load()) {
      auto v = std::move(pendingTimeouts);
      pendingTimeouts.clear();
      for (Slot* s : v) handleTimeout(*s);
    }
    pendingTimeouts.clear();
  }

  void pollRestarts() {
    const double t = clock();
    std::vector<std::string> due;
    for (auto& [n, s] : slots)
      if (s->restartAt && t >= *s->restartAt) due.push_back(n);
    for (auto& n : due) {
      if (halted.load()) return;
      Slot& s = *slots.at(n);
      s.restartAt.reset();
      if (s.h.running_) continue;
      log(LogLevel::Info, "[" + n + "] reiniciando após aborto por tempo");
      startFresh(n);  // Failed já abandona o bringBack; Waiting o mantém
      settle();
      publishStatus();
    }
  }

  void stopAll() {
    std::vector<std::string> names;
    for (auto it = order.rbegin(); it != order.rend(); ++it) names.push_back((*it)->h.name());
    for (auto& n : names) {
      Slot& s = *slots.at(n);
      if (s.h.running_) stopOne(s);
    }
    pendingTimeouts.clear();
    for (auto& [n, s] : slots) destroyState(*s);
  }

  // ================= operações (thread de scripts) =================
  void boot() {
    scan();
    std::vector<std::string> names;
    for (auto& [n, s] : slots) names.push_back(n);
    std::set<std::string> failedNow;
    for (auto& [n, s] : slots)
      if (!s->fixedError.empty()) failedNow.insert(n);
    for (auto& n : names) {
      if (halted.load()) break;
      std::vector<std::string> chain;
      startOne(n, chain, failedNow);
      settle();
    }
    publishStatus();
  }

  bool doStart(const std::string& name) {
    Slot* s = findOrDiscover(name);
    if (s == nullptr) {
      log(LogLevel::Warn, "start: resource desconhecido '" + name + "'");
      return false;
    }
    s->timeouts.clear();
    s->restartAt.reset();
    return startFresh(name) == Start::Up;
  }
  // Nomes de quem depende de `name`, direta ou transitivamente (pelos manifests conhecidos, rodando ou não).
  std::set<std::string> dependentsOf(const std::string& name) {
    std::set<std::string> out;
    std::vector<std::string> todo{name};
    while (!todo.empty()) {
      const std::string cur = todo.back();
      todo.pop_back();
      for (auto& [n, sl] : slots) {
        const auto& d = sl->h.manifest().dependencies;
        if (n != name && !out.count(n) && std::find(d.begin(), d.end(), cur) != d.end()) {
          out.insert(n);
          todo.push_back(n);
        }
      }
    }
    return out;
  }

  // Parada pelo admin: cancela o reinício agendado e tira o resource e seus dependentes de toda lista bringBack, para
  // que nada disso volte sozinho quando uma dependência subir.
  bool doStop(const std::string& name) {
    Slot* s = findOrDiscover(name);
    if (s == nullptr) return false;
    const bool wasPending = s->restartAt.has_value();
    const std::set<std::string> deps = dependentsOf(name);
    // Só ganham mensagem nova os dependentes afetados por esta parada: rodando, aguardando (em algum bringBack) ou
    // com reinício próprio agendado. Os que já estavam parados mantêm o motivo anterior.
    std::set<std::string> affected;
    for (const auto& d : deps) {
      const Slot& ds = *slots.at(d);
      bool waiting = false;
      for (auto& [n, sl] : slots)
        if (std::find(sl->bringBack.begin(), sl->bringBack.end(), d) != sl->bringBack.end()) waiting = true;
      if (ds.h.running_ || ds.restartAt || waiting) affected.insert(d);
    }
    for (auto& [n, sl] : slots) {
      auto& bb = sl->bringBack;
      bb.erase(std::remove_if(bb.begin(), bb.end(),
                              [&](const std::string& x) { return x == name || deps.count(x) > 0; }),
               bb.end());
    }
    s->restartAt.reset();
    s->bringBack.clear();
    if (s->h.running_) stopOne(*s);
    s->state = State::Stopped;
    s->lastError = "parado pelo admin";
    for (const auto& d : deps) {
      Slot& ds = *slots.at(d);
      if (ds.h.running_) stopOne(ds);  // por segurança; stopOne(s) já parou os que rodavam
      ds.restartAt.reset();
      if (ds.state == State::Failed || !affected.count(d)) continue;
      ds.state = State::Stopped;
      ds.lastError = wasPending ? "a dependência '" + name + "' foi parada" : "parado junto com '" + name + "'";
    }
    log(LogLevel::Info, "[" + name + "] parado pelo admin");
    return true;
  }
  bool doRestart(const std::string& name) {
    Slot* s = findOrDiscover(name);
    if (s == nullptr) return false;
    s->timeouts.clear();
    s->restartAt.reset();
    if (s->h.running_) addBringBack(*s, stopOne(*s));
    s->state = State::Stopped;
    return startFresh(name) == Start::Up;
  }

  // ================= filas e threads =================
  bool enqueue(Task t) {
    {
      std::lock_guard<std::mutex> lk(qMu);
      if (!halted.load()) {
        tasks.push_back(std::move(t));
        qCv.notify_one();
        return true;
      }
    }
    if (t.cancel) t.cancel();
    return false;
  }

  std::future<bool> control(std::function<bool(HostImpl&)> f) {
    auto c = std::make_shared<Ctl>();
    auto fut = c->p.get_future();
    {
      std::lock_guard<std::mutex> lk(ctlMu);
      ctls.erase(std::remove_if(ctls.begin(), ctls.end(),
                                [](const std::weak_ptr<Ctl>& w) {
                                  auto p = w.lock();
                                  return !p || p->done.load();
                                }),
                 ctls.end());
      ctls.push_back(c);
    }
    enqueue(Task{[c, f](ScriptContext& ctx) {
                   auto& I = static_cast<HostImpl&>(ctx);
                   const bool r = f(I);
                   I.publishStatus();  // quem recebe o resultado já vê o status atualizado
                   c->resolve(r);
                 },
                 [c] { c->resolve(false); }});
    return fut;
  }

  void cancelAll(std::deque<Task>& ts, size_t from = 0) {
    for (size_t i = from; i < ts.size(); ++i)
      if (ts[i].cancel) ts[i].cancel();
    ts.clear();
  }

  void threadMain() {
    scriptId.store(std::this_thread::get_id());
    for (;;) {
      std::deque<Task> ts;
      std::deque<SimToScript> evs;
      bool stop = false;
      {
        std::unique_lock<std::mutex> lk(qMu);
        qCv.wait_for(lk, opt.idleWait, [&] { return !tasks.empty() || !events.empty() || stopReq; });
        ts.swap(tasks);
        evs.swap(events);
        stop = stopReq;
      }
      if (halted.load()) {
        cancelAll(ts);
        eventsDropped.fetch_add(evs.size());
        pending.fetch_sub(evs.size());
        break;
      }
      for (size_t i = 0; i < ts.size(); ++i) {
        if (halted.load()) {
          cancelAll(ts, i);
          break;
        }
        if (!safely("tarefa da thread de scripts", [&] { ts[i].run(*this); }) && ts[i].cancel) ts[i].cancel();
        settle();
        publishStatus();
      }
      for (size_t i = 0; i < evs.size(); ++i) {
        if (halted.load()) {
          eventsDropped.fetch_add(evs.size() - i);
          pending.fetch_sub(evs.size() - i);
          break;
        }
        if (opt.onEvent) safely("despacho de evento", [&] { opt.onEvent(*this, evs[i]); });
        if (pending.fetch_sub(1) - 1 < opt.eventQueueCapacity) backstopLogged.store(false);
        settle();
      }
      if (halted.load()) break;
      pollRestarts();
      if (opt.onTick) {
        safely("onTick", [&] { opt.onTick(*this); });
        settle();
      }
      publishStatus();
      if (stop) {
        stopAll();
        publishStatus();
        break;
      }
    }
    if (halted.load()) {
      // A chamada presa voltou (ou nunca houve): fecha os estados sem rodar mais Lua. O host fica parado.
      pendingTimeouts.clear();
      order.clear();
      for (auto& [n, s] : slots) {
        s->h.running_ = false;
        s->h.api.reset();
        s->h.res_.reset();
      }
    }
    finished.store(true);
  }

  void trip(const std::string& res, double elapsed) {
    const std::string reason = "chamada ao Lua em '" + res + "' sem progresso por " + secs(elapsed) +
                               " s (limite " + secs(abortSec + opt.supervisorGraceSec) +
                               " s; operação C não interrompível?). Host de scripts parado: nenhum resource roda "
                               "até reiniciar o servidor";
    if (halted.load()) return;  // só o supervisor chama trip()
    // CRÍTICO antes de `halted`: quem vê o host parado já encontra o log.
    log(LogLevel::Critical, "[" + res + "] " + reason);
    {
      std::lock_guard<std::mutex> lk(statusMu);
      if (halted.exchange(true)) return;
      haltReason = reason;
      for (auto& s : statusSnap) {
        s.state = State::Failed;
        s.lastError = s.name == res ? reason : "host de scripts parado pelo supervisor (chamada presa em '" + res + "')";
      }
    }
    std::deque<Task> ts;
    std::deque<SimToScript> evs;
    {
      std::lock_guard<std::mutex> lk(qMu);
      evs.swap(events);
      ts.swap(tasks);
    }
    eventsDropped.fetch_add(evs.size());
    pending.fetch_sub(evs.size());
    cancelAll(ts);
    // Pedidos de controle em aberto (inclusive o que está preso e os já retirados da fila pela thread de scripts).
    std::vector<std::weak_ptr<Ctl>> cs;
    {
      std::lock_guard<std::mutex> lk(ctlMu);
      cs.swap(ctls);
    }
    for (auto& w : cs)
      if (auto c = w.lock()) c->resolve(false);
    // evs/ts/cs destruídos aqui, fora dos locks
  }

  void supMain() {
    uint64_t warnedSeq = 0;
    std::unique_lock<std::mutex> lk(supMu);
    while (!supStop) {
      supCv.wait_for(lk, opt.supervisorPeriod);
      if (supStop) break;
      lk.unlock();
      bool in;
      double last;
      std::string res;
      uint64_t seq;
      {
        std::lock_guard<std::mutex> bl(beatMu);
        in = !callStack.empty();
        last = lastBeat;
        res = in ? callStack.back() : std::string();
        seq = beatSeq;
      }
      if (in && !halted.load() && !abandoned_.load()) {
        const double el = clock() - last;
        if (el > abortSec + opt.supervisorGraceSec) {
          trip(res, el);
        } else if (el > abortSec + 1.0 && seq != warnedSeq) {
          warnedSeq = seq;
          log(LogLevel::Warn, "[" + res + "] chamada ao Lua sem progresso há " + secs(el) +
                                  " s (passou do prazo de aborto e ainda não voltou); o supervisor para o host em " +
                                  secs(abortSec + opt.supervisorGraceSec) + " s");
        }
      }
      lk.lock();
    }
  }
};

}  // namespace detail

using detail::HostImpl;
using detail::Task;

size_t ScriptHost::parkedThreadCount() { return g_parked.load(); }

ScriptHost::ScriptHost(fs::path resourcesDir, Limits limits, std::function<double()> now, HostOptions opt)
    : impl_(std::make_shared<HostImpl>(std::move(resourcesDir), limits, std::move(now), std::move(opt))) {}

ScriptHost::~ScriptHost() {
  HostImpl& I = *impl_;
  {
    std::lock_guard<std::mutex> lk(I.qMu);
    I.stopReq = true;
  }
  I.qCv.notify_all();
  if (I.scriptThread.joinable()) {
    for (;;) {
      if (I.finished.load()) {
        I.scriptThread.join();
        break;
      }
      bool abandon = false;
      {
        std::lock_guard<std::mutex> lk(I.beatMu);
        if (I.halted.load() && !I.callStack.empty()) {
          I.abandoned_.store(true);  // sob beatMu: endCall verá a marca ou já terá fechado a chamada
          abandon = true;
        }
      }
      if (abandon) {
        // Chamada C presa: a thread fica com o HostImpl (shared_ptr) e estaciona para sempre se voltar.
        I.scriptThread.detach();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  {
    std::lock_guard<std::mutex> lk(I.supMu);
    I.supStop = true;
  }
  I.supCv.notify_all();
  if (I.supThread.joinable()) I.supThread.join();
  std::deque<Task> left;
  {
    std::lock_guard<std::mutex> lk(I.qMu);
    left.swap(I.tasks);
  }
  I.cancelAll(left);
  std::vector<std::weak_ptr<detail::Ctl>> cs;
  {
    std::lock_guard<std::mutex> lk(I.ctlMu);
    cs.swap(I.ctls);
  }
  for (auto& w : cs)
    if (auto c = w.lock()) c->resolve(false);
}

void ScriptHost::start() {
  HostImpl& I = *impl_;
  if (I.started.exchange(true)) return;
  auto self = impl_;
  I.scriptThread = std::thread([self] { self->threadMain(); });
  I.supThread = std::thread([self] { self->supMain(); });
  auto done = std::make_shared<std::promise<void>>();
  auto fut = done->get_future();
  auto setDone = [done] {
    try {
      done->set_value();
    } catch (const std::future_error&) {
    }
  };
  I.enqueue(Task{[setDone](ScriptContext& c) {
                   static_cast<HostImpl&>(c).boot();
                   setDone();
                 },
                 setDone});
  while (fut.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
    if (I.halted.load()) break;
}

bool ScriptHost::post(SimToScript ev) {
  HostImpl& I = *impl_;
  if (I.halted.load()) {
    I.eventsDropped.fetch_add(1);
    return false;
  }
  const bool clientEvent = std::holds_alternative<EvClientEvent>(ev);
  const size_t cap = I.opt.eventQueueCapacity;
  bool critical = false, dropped = false;
  {
    std::lock_guard<std::mutex> lk(I.qMu);
    const size_t n = I.pending.load();
    if (I.halted.load() || (clientEvent && n >= cap)) {
      I.eventsDropped.fetch_add(1);
      return false;
    }
    if (n >= cap * kBackstopFactor) {  // teto rígido: nada mais entra (nem Join/Leave/comandos)
      dropped = true;
      I.eventsDropped.fetch_add(1);
      I.backstopDropped.fetch_add(1);
      critical = !I.backstopLogged.exchange(true);
    } else {
      I.events.push_back(std::move(ev));
      I.pending.fetch_add(1);
    }
  }
  if (critical)
    I.log(LogLevel::Critical, "fila de eventos dos scripts no teto rígido (" + std::to_string(cap * kBackstopFactor) +
                                  "): descartando inclusive entradas/saídas de jogadores e comandos");
  if (dropped) return false;
  I.qCv.notify_one();
  return true;
}

void ScriptHost::publishPlayers(PlayersSnapshot players) {
  if (!players) players = std::make_shared<const std::vector<PlayerView>>();
  HostImpl& I = *impl_;
  {
    std::lock_guard<std::mutex> lk(I.playersMu);
    I.players_.swap(players);
  }
  // a foto antiga (se for a última referência) é liberada aqui, fora do lock
}

void ScriptHost::drainCommands(std::vector<ScriptToSim>& out) {
  HostImpl& I = *impl_;
  std::vector<ScriptToSim> got;
  {
    std::lock_guard<std::mutex> lk(I.cmdMu);
    got.swap(I.cmds);
  }
  if (out.empty()) {
    out.swap(got);
  } else {
    out.insert(out.end(), std::make_move_iterator(got.begin()), std::make_move_iterator(got.end()));
  }
}

std::future<bool> ScriptHost::startResource(std::string name) {
  return impl_->control([name](HostImpl& I) { return I.doStart(name); });
}
std::future<bool> ScriptHost::stopResource(std::string name) {
  return impl_->control([name](HostImpl& I) { return I.doStop(name); });
}
std::future<bool> ScriptHost::restartResource(std::string name) {
  return impl_->control([name](HostImpl& I) { return I.doRestart(name); });
}

std::vector<ResourceStatus> ScriptHost::status() const {
  std::lock_guard<std::mutex> lk(impl_->statusMu);
  return impl_->statusSnap;
}

HostMetrics ScriptHost::metrics() const {
  HostImpl& I = *impl_;
  HostMetrics m;
  m.eventsDropped = I.eventsDropped.load();
  m.eventsBackstopDropped = I.backstopDropped.load();
  m.commandsDropped = I.cmdsDropped.load();
  {
    std::lock_guard<std::mutex> lk(I.qMu);
    m.eventQueueDepth = I.pending.load();
  }
  {
    std::lock_guard<std::mutex> lk(I.statusMu);
    m.haltReason = I.haltReason;
  }
  m.halted = I.halted.load();
  {
    std::lock_guard<std::mutex> lk(I.beatMu);
    m.inLuaCall = !I.callStack.empty();
  }
  return m;
}

void ScriptHost::runOnScriptThread(std::function<void(ScriptContext&)> fn) {
  impl_->enqueue(Task{std::move(fn), nullptr});
}

}  // namespace cbscript
