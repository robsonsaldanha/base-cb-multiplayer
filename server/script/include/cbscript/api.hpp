#pragma once
// API cb.* dos scripts (tabela "API cb.*" da spec), sem banco/HTTP (Tasks 5–6).
//
// installApi() preenche os ganchos do ScriptHost:
// - onSetup: cria a tabela global `cb` no estado do resource (só funções C; acesso cru, nada de lua()["x"]).
// - onStarted / onStopping: resourceStart / resourceStop do próprio resource.
// - onEvent: playerJoin/playerLeave (cb.on), eventos de cliente (cb.events.on), comandos (cb.commands).
// - onTick: timers (cb.timers) pelo relógio do host (resolução = volta do laço da thread de scripts).
// Ganchos que já estavam em `opt` continuam sendo chamados, depois dos da API.
//
// Todo callback roda numa corrotina própria (ScriptContext::invoke). Eventos locais (cb.events.emitLocal) são
// entregues na mesma thread logo depois do gancho/evento atual terminar (nunca reentrantes).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "cbscript/host.hpp"

namespace cbscript {

class BanStore;

struct ApiOptions {
  // Uma linha JSONL de auditoria (sem '\n'), só da thread de scripts. Vazio = descartada.
  std::function<void(const std::string&)> audit;
  // Relógio Unix em segundos (auditoria e bans). Vazio = system_clock.
  std::function<int64_t()> unixNow;
  // Bans (bans.json até a Task 5). Nulo → cb.players.ban/unban dão erro.
  std::shared_ptr<BanStore> bans;
  // Jogador (por did) pode usar comandos admin=true pelo chat. Vazio = ninguém (só console/painel).
  std::function<bool(const std::string& did)> isAdmin;
  size_t maxTimersPerResource = 10000;
  double minEveryMs = 10;
  size_t maxLocalQueue = 65536;  // eventos locais aguardando entrega (acima disso emitLocal dá erro)
  size_t maxLocalBytes = 65536;  // JSON de um evento local / argumento de cb.resource
};

void installApi(HostOptions& opt, ApiOptions api = {});

}  // namespace cbscript
