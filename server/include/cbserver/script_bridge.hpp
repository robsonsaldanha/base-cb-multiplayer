#pragma once
// Ponte entre o World (thread da simulação) e o ScriptHost (thread de scripts). Tudo aqui roda na thread da
// simulação e nunca espera a thread de scripts: só post/publishPlayers/drainCommands (filas não bloqueantes).
//
// - Ganchos do World → eventos do host: entrada (EvPlayerJoin), saída (EvPlayerLeave com o motivo), evento de
//   cliente aceito pela cota (EvClientEvent), todos com a sessão do jogador. O evento de cliente `cb:command`
//   (payload: texto JSON com a linha) vira EvCommand do jogador; qualquer outro nome `cb:*` vindo do cliente é
//   descartado.
// - applyCommands (antes de World::tick): drena o host todo tick. Primeiro o canal prioritário (kick, kick por did,
//   teleporte), coalescido (último teleporte por id+sessão, primeiro kick) e limitado a 2 × maxPlayers por tick;
//   depois os emits, em rodízio entre as origens (resources), com no máximo kMaxCommandsPerTick comandos,
//   kMaxBytesPerTick bytes de payload e kMaxEventOutPerTick destinatários (broadcast = número de jogadores) por tick;
//   cada origem acumula no máximo kMaxBacklogPerSource / kMaxBacklogBytesPerSource (o excesso é descartado e contado) (o resto fica para os próximos ticks; só drena o host de novo quando o atraso acabar, então a fila do
//   host — limitada em itens e bytes — segura a pressão). CmdEmit → Event (broadcast com uma cópia só do payload;
//   nome > 64 ou payload > 4096 bytes → descartado e contado), CmdKick → KickOrder, CmdKickDid → KickOrder de todos
//   com o did, CmdSetPosition → setState (Correction com carência). Comando com sessão diferente da do jogador atual
//   daquele id (id reaproveitado) é ignorado.
// - publishPlayers (depois de World::tick): foto imutável dos jogadores para cb.players.* (buffers reaproveitados
//   quando a thread de scripts já soltou a foto antiga).
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cbscript/host.hpp"
#include "cbserver/world.hpp"

namespace cbserver {

class ScriptBridge {
 public:
  static constexpr size_t kMaxCommandsPerTick = 2048;
  static constexpr size_t kMaxBytesPerTick = size_t{2} << 20;
  // Eventos de saída por tick (destinatários): um broadcast conta como o número de jogadores.
  static constexpr size_t kMaxEventOutPerTick = 8192;

  ScriptBridge(World& world, cbscript::ScriptHost& host);
  ~ScriptBridge();  // remove os ganchos do World
  ScriptBridge(const ScriptBridge&) = delete;
  ScriptBridge& operator=(const ScriptBridge&) = delete;

  void applyCommands(double now);
  void publishPlayers();

  uint64_t eventsDropped() const { return eventsDropped_; }        // eventos de cliente recusados pelo host
  uint64_t joinLeaveDropped() const { return joinLeaveDropped_; }  // Join/Leave recusados (teto rígido/host parado)
  uint64_t commandsRejected() const { return commandsRejected_; }  // CmdEmit inválidos (nome/payload grandes demais)
  uint64_t commandsDropped() const { return commandsDropped_; }    // emits acima do atraso máximo da origem
  size_t backlog() const;                                          // comandos esperando o próximo tick
  size_t priorityBacklog() const { return prioBacklog_.size(); }

  static constexpr size_t kMaxBacklogPerSource = 65536;
  static constexpr size_t kMaxBacklogBytesPerSource = size_t{16} << 20;

 private:
  void applyOne(cbscript::ScriptToSim& c, double now);  // kick / kick por did / teleporte
  void addPriority(cbscript::ScriptToSim c);
  bool validEmit(cbscript::CmdEmit& e);

  World& world_;
  cbscript::ScriptHost& host_;
  std::vector<cbscript::ScriptToSim> drained_;  // reaproveitado
  std::vector<cbscript::ScriptToSim> prioBacklog_;
  std::unordered_map<uint64_t, size_t> prioPos_, prioKick_;
  std::unordered_map<std::string, size_t> prioKickDid_;
  std::map<uint32_t, std::deque<cbscript::CmdEmit>> bySource_;  // emits pendentes por origem (resource)
  std::unordered_map<uint32_t, size_t> sourceBytes_;
  uint32_t rrLast_ = 0;
  std::vector<std::shared_ptr<std::vector<cbscript::PlayerView>>> snapPool_;
  uint64_t eventsDropped_ = 0, joinLeaveDropped_ = 0, commandsRejected_ = 0, commandsDropped_ = 0;
};

}  // namespace cbserver
