#pragma once
// Ponte entre o World (thread da simulação) e o ScriptHost (thread de scripts). Tudo aqui roda na thread da
// simulação e nunca espera a thread de scripts: só post/publishPlayers/drainCommands (filas não bloqueantes).
//
// - Ganchos do World → eventos do host: entrada (EvPlayerJoin), saída (EvPlayerLeave com o motivo), evento de
//   cliente aceito pela cota (EvClientEvent), todos com a sessão do jogador. O evento de cliente `cb:command`
//   (payload: texto JSON com a linha) vira EvCommand do jogador; qualquer outro nome `cb:*` vindo do cliente é
//   descartado.
// - applyCommands (antes de World::tick): primeiro o canal prioritário do host (kick, kick por did, teleporte), sempre
//   e por inteiro; depois os emits, com no máximo kMaxCommandsPerTick comandos, kMaxBytesPerTick bytes de payload e
//   kMaxEventOutPerTick destinatários (broadcast = número de jogadores) por tick (o resto fica para os próximos ticks; só drena o host de novo quando o atraso acabar, então a fila do
//   host — limitada em itens e bytes — segura a pressão). CmdEmit → Event (broadcast com uma cópia só do payload;
//   nome > 64 ou payload > 4096 bytes → descartado e contado), CmdKick → KickOrder, CmdKickDid → KickOrder de todos
//   com o did, CmdSetPosition → setState (Correction com carência). Comando com sessão diferente da do jogador atual
//   daquele id (id reaproveitado) é ignorado.
// - publishPlayers (depois de World::tick): foto imutável dos jogadores para cb.players.* (buffers reaproveitados
//   quando a thread de scripts já soltou a foto antiga).
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
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
  size_t backlog() const { return backlog_.size(); }               // comandos esperando o próximo tick

 private:
  void applyOne(cbscript::ScriptToSim& c, double now);  // kick / kick por did / teleporte

  World& world_;
  cbscript::ScriptHost& host_;
  std::vector<cbscript::ScriptToSim> drained_;  // reaproveitado
  std::deque<cbscript::ScriptToSim> backlog_;
  std::vector<std::shared_ptr<std::vector<cbscript::PlayerView>>> snapPool_;
  uint64_t eventsDropped_ = 0, joinLeaveDropped_ = 0, commandsRejected_ = 0;
};

}  // namespace cbserver
