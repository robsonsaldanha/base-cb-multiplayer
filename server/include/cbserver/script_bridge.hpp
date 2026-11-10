#pragma once
// Ponte entre o World (thread da simulação) e o ScriptHost (thread de scripts). Tudo aqui roda na thread da
// simulação e nunca espera a thread de scripts: só post/publishPlayers/drainCommands (filas não bloqueantes).
//
// - Ganchos do World → eventos do host: entrada (EvPlayerJoin), saída (EvPlayerLeave com o motivo), evento de
//   cliente aceito pela cota (EvClientEvent). O evento de cliente `cb:command` (payload: texto JSON com a linha)
//   vira EvCommand do jogador; qualquer outro nome `cb:*` vindo do cliente é descartado.
// - applyCommands (antes de World::tick): CmdEmit → Event para o jogador (ou todos com -1), CmdKick → KickOrder,
//   CmdSetPosition → setState (Correction com carência de carregamento). Kick/teleporte com `did` que não bate com o
//   jogador atual daquele id (id reaproveitado) são ignorados.
// - publishPlayers (depois de World::tick): foto imutável dos jogadores para cb.players.*.
#include <cstdint>
#include <vector>

#include "cbscript/host.hpp"
#include "cbserver/world.hpp"

namespace cbserver {

class ScriptBridge {
 public:
  ScriptBridge(World& world, cbscript::ScriptHost& host);
  ~ScriptBridge();  // remove os ganchos do World
  ScriptBridge(const ScriptBridge&) = delete;
  ScriptBridge& operator=(const ScriptBridge&) = delete;

  void applyCommands(double now);
  void publishPlayers();

  uint64_t eventsDropped() const { return eventsDropped_; }  // recusados pela fila do host (cheia/parado)

 private:
  World& world_;
  cbscript::ScriptHost& host_;
  std::vector<cbscript::ScriptToSim> cmds_;  // reaproveitado
  uint64_t eventsDropped_ = 0;
};

}  // namespace cbserver
