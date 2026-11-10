#pragma once
// ScriptHost: a thread de scripts, as filas com a simulação, o ciclo de vida dos resources e o supervisor.
//
// Threads:
// - simulação: post / publishPlayers / drainCommands. Nunca esperam a thread de scripts: os mutexes envolvidos só
//   protegem trocas O(1) de fila/ponteiro e nunca ficam presos enquanto o Lua roda.
// - scripts (1): todo o Lua, os ganchos (HostOptions::on*) e as tarefas (runOnScriptThread).
// - supervisor (1): olha o "batimento" (início/fim de cada chamada ao Lua, em qualquer nível de aninhamento). Sem
//   batimento por mais de abortAfter + supervisorGraceSec (relógio injetado) com chamada aberta → log CRÍTICO, todos
//   os resources Failed, eventos passam a ser descartados, pedidos de controle pendentes resolvem false e o host
//   fica parado para sempre (não religa sozinho, nem quando a chamada voltar).
// - Se o ScriptHost for destruído com a chamada ainda presa, a thread de scripts é desanexada e, quando (se) a
//   chamada voltar, estaciona para sempre sem tocar em log, ganchos ou Lua (o estado vaza de propósito).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cbscript/messages.hpp"
#include "cbscript/resource.hpp"

namespace cbscript {

enum class LogLevel { Info, Warn, Error, Critical };

namespace detail {
struct HostImpl;
struct Slot;
}  // namespace detail

// Um resource visto pelos ganchos e pela API da Task 4. Só existe e só é tocado na thread de scripts.
class HostedResource {
 public:
  const ResourceManifest& manifest() const { return manifest_; }
  const std::string& name() const { return manifest_.name; }
  // O estado sandbox. Válido do onSetup até o fim do onStopping (enquanto `hasState()`).
  Resource& lua() { return *res_; }
  bool hasState() const { return res_ != nullptr; }
  bool running() const { return running_; }
  // Estado da API (handlers, timers, refs no registro…). O host zera isto antes de destruir o Resource, então
  // referências Lua guardadas aqui morrem antes do estado.
  std::shared_ptr<void> api;

 private:
  friend struct detail::HostImpl;
  friend struct detail::Slot;
  ResourceManifest manifest_;
  std::unique_ptr<Resource> res_;
  bool running_ = false;
};

// Serviços do host para os ganchos e tarefas. Só pode ser usado na thread de scripts.
class ScriptContext {
 public:
  virtual ~ScriptContext() = default;
  // Última foto publicada pela simulação (nunca nula: vazia antes da primeira).
  virtual PlayersSnapshot players() const = 0;
  // Enfileira um comando para a simulação; false se a fila estiver cheia (descartado e contado).
  virtual bool emit(ScriptToSim cmd) = 0;
  // Resources rodando, em ordem de subida (dependências antes dos dependentes).
  virtual std::vector<HostedResource*> running() = 0;
  virtual HostedResource* find(std::string_view name) = 0;
  // Chama fn no resource com batimento do supervisor e política de erros: erro → log com traceback (o resource
  // continua); aborto por tempo → o resource é parado e reiniciado com back-off depois que o gancho/tarefa atual
  // terminar (3 abortos em timeoutWindowSec → Failed). Resource fora do ar ou host parado → ok=false sem rodar nada.
  virtual Resource::CallResult invoke(HostedResource& r, const sol::reference& fn,
                                      std::span<const sol::object> args = {}) = 0;
  // Retoma uma corrotina que cedeu (respostas de banco/HTTP), com a mesma política de invoke.
  virtual Resource::CallResult resume(HostedResource& r, const sol::thread& co,
                                      std::span<const sol::object> args = {}) = 0;
  virtual double now() const = 0;
  virtual void log(LogLevel lv, const std::string& msg) = 0;
  // true depois que o ScriptHost foi destruído com uma chamada C presa (a thread foi desanexada). Contrato para os
  // bindings da Task 4/5: a thread só estaciona em dois pontos (fim da chamada ao Lua e ctx.log/now chamados dela);
  // entre a operação C presa voltar e esses pontos ainda pode rodar código do script. Todo binding que alcance algo
  // fora do HostImpl/estado Lua (pools de banco/HTTP, callbacks do usuário, objetos do main.cpp) precisa checar
  // abandoned() antes e, se true, não tocar em nada (devolver erro ao script ou chamar ctx.log, que estaciona).
  // emit()/players()/find()/running() só tocam o HostImpl (mantido vivo pela própria thread) e são seguros.
  virtual bool abandoned() const = 0;
};

struct HostOptions {
  // Contagem = eventos aceitos e ainda não consumidos (na fila + no lote que a thread de scripts está processando).
  // Acima disto, eventos de cliente são descartados. Join/Leave e comandos (raros, de admin) passam do limite até o
  // teto rígido de 4× a capacidade: dali em diante tudo é descartado, com log CRÍTICO (1 por episódio) e métrica.
  size_t eventQueueCapacity = 65536;
  size_t commandQueueCapacity = 65536;  // scripts → simulação
  size_t commandQueueBytes = size_t{16} << 20;  // teto de bytes (payloads) na fila scripts → simulação
  // CmdKick/CmdKickDid/CmdSetPosition vão para um canal prioritário, fora dos dois limites acima (uma enxurrada de
  // emits não os descarta). Teto de segurança; passar dele é log CRÍTICO.
  size_t priorityQueueCapacity = 65536;
  double supervisorGraceSec = 10.0;     // supervisor dispara em abortAfter + isto
  std::chrono::milliseconds supervisorPeriod{100};  // período real de checagem do supervisor
  std::chrono::milliseconds idleWait{10};  // espera máxima da thread de scripts sem trabalho (back-off, onTick)
  int maxTimeouts = 3;                     // abortos por tempo dentro da janela → Failed
  double timeoutWindowSec = 60.0;
  double backoffBaseSec = 1.0, backoffMaxSec = 30.0;  // atraso do reinício: base·2^(n-1), até o máximo
  // Log do host (padrão: spdlog). Chamado de qualquer thread do host.
  std::function<void(LogLevel, const std::string&)> log;
  // Ganchos (thread de scripts). onSetup: estado criado, antes dos scripts (registre a API cb.* aqui); exceção →
  // resource Failed. onStarted: scripts rodaram (resourceStart). onStopping: antes de fechar (resourceStop).
  std::function<void(ScriptContext&, HostedResource&)> onSetup, onStarted, onStopping;
  std::function<void(ScriptContext&, SimToScript&)> onEvent;  // cada evento da simulação, em ordem
  std::function<void(ScriptContext&)> onTick;                 // cada volta do laço (timers)
};

struct HostMetrics {
  uint64_t eventsDropped = 0;          // total de eventos descartados (inclui os do teto rígido)
  uint64_t eventsBackstopDropped = 0;  // descartados pelo teto rígido (4× a capacidade), inclusive Join/Leave
  uint64_t commandsDropped = 0;
  uint64_t priorityDropped = 0;  // comandos prioritários recusados (teto de segurança; nunca com host rodando normal)
  size_t eventQueueDepth = 0;  // aceitos e não consumidos (na fila + no lote em processamento)
  bool halted = false;  // supervisor disparou
  std::string haltReason;
  bool inLuaCall = false;  // batimento: há chamada ao Lua em andamento
};

class ScriptHost {
 public:
  // `now`: relógio em segundos (monotônico), chamado da thread de scripts e do supervisor (precisa ser thread-safe).
  ScriptHost(std::filesystem::path resourcesDir, Limits limits, std::function<double()> now,
             HostOptions opt = {});
  ~ScriptHost();  // para os resources (onStopping) e as threads
  ScriptHost(const ScriptHost&) = delete;
  ScriptHost& operator=(const ScriptHost&) = delete;

  // Lê os manifests, sobe tudo em ordem topológica e volta quando a subida terminar (ou o supervisor parar o host).
  // Ciclo, dependência ausente ou manifest inválido → só os afetados ficam Failed.
  void start();

  // ---- Chamados pela simulação (não bloqueiam) ----
  bool post(SimToScript ev);  // false se descartado (fila cheia para evento de cliente, teto rígido, host parado)
  void publishPlayers(PlayersSnapshot players);
  void drainCommands(std::vector<ScriptToSim>& out);  // prioritários e depois os demais, ao fim de `out`
  void drainPriority(std::vector<ScriptToSim>& out);  // só os prioritários (kick/teleporte), ao fim de `out`

  // ---- Painel/console (thread-safe; executados na thread de scripts) ----
  // start sobe também as dependências; stop para os dependentes antes; restart religa os dependentes que parou.
  // Os três zeram o histórico de abortos (decisão explícita do admin). Host parado → false na hora.
  std::future<bool> startResource(std::string name);
  std::future<bool> stopResource(std::string name);
  std::future<bool> restartResource(std::string name);
  std::vector<ResourceStatus> status() const;  // ordenado por nome
  HostMetrics metrics() const;

  // Roda fn na thread de scripts (respostas de banco/HTTP etc.). Nunca descartado, exceto com o host parado.
  void runOnScriptThread(std::function<void(ScriptContext&)> fn);

  // Diagnóstico: threads de scripts estacionadas para sempre no processo (host destruído com chamada C presa).
  static size_t parkedThreadCount();

 private:
  std::shared_ptr<detail::HostImpl> impl_;  // compartilhado com as threads (sobrevive a um detach)
};

}  // namespace cbscript
