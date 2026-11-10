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
// Sessões: tabelas de jogador têm `session` (único no processo; o id é reaproveitado, a sessão não).
// cb.events.emit(alvo, nome, dados, sessão?), cb.players.kick(id, motivo?, sessão?) e
// cb.players.setPosition(id, x, y, z, sessão?) aceitam a sessão opcional: sem ela, valem para o ocupante atual do id
// (o conectado quando o script chama); com ela, devolvem false se o id agora é de outra sessão. Em todos os casos a
// simulação ainda descarta o comando se o id mudar de dono antes de ele ser aplicado.
//
// Todo callback roda numa corrotina própria (ScriptContext::invoke). Eventos locais (cb.events.emitLocal) são
// entregues na mesma thread logo depois do gancho/evento atual terminar (nunca reentrantes).
//
// cb.db (permissions.db = true; ApiOptions::db):
// - cb.db.query(sql, ...) → lista de linhas {coluna = valor}; cb.db.exec → linhas afetadas; cb.db.insert → último
//   id inserido. Só '?' (um parâmetro por '?': nil/cb.null, boolean, integer, number, string). A corrotina do
//   callback cede e é retomada na thread de scripts quando o pool responder (onTick); erro (SQL, banco fora, tempo
//   esgotado) → erro Lua no callback.
// - cb.db.transaction(fn(tx)) → tx.query/exec/insert numa conexão dedicada; commit se fn terminar (devolve o 1º
//   valor de fn), rollback e o mesmo erro se fn falhar. Aninhar, ou usar cb.db.* direto dentro de fn, é erro.
//   Aborto pela vigia/stop do resource → rollback (abortTx); transação abandonada → rollback.
// - Recusado: no nível de cima do script principal (use resourceStart), dentro de cb.resource e onde não dá para
//   ceder (metamétodo/iterador chamado por C). Corrotina retomada por fora (coroutine.resume) enquanto espera o banco
//   → erro nela. Resource parado → pedidos cancelados e resultados descartados.
// - Comando (cb.commands) que espera o banco ainda responde com o texto que devolver no fim.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "cbscript/host.hpp"

namespace cbscript {

class BanStore;
class DbPool;

// Contadores da API (lidos de qualquer thread, para /metrics).
struct ApiStats {
  std::atomic<uint64_t> logDropped{0};    // linhas de cb.log acima de 200/s por resource
  std::atomic<uint64_t> auditDropped{0};  // linhas de cb.audit acima de 200/s (todas as fontes)
};

struct ApiOptions {
  // Uma linha JSONL de auditoria (sem '\n'), só da thread de scripts. Vazio = descartada.
  std::function<void(const std::string&)> audit;
  // Relógio Unix em segundos (auditoria e bans). Vazio = system_clock.
  std::function<int64_t()> unixNow;
  // Bans (bans.json ou MySQL). Nulo → cb.players.ban/unban dão erro.
  std::shared_ptr<BanStore> bans;
  // Banco (cb.db.*). Nulo → cb.db.* dão erro "banco não configurado". Resources precisam de permissions.db = true.
  std::shared_ptr<DbPool> db;
  size_t maxDbPending = 1024;  // pedidos cb.db em andamento por resource (acima disso: erro)
  double dbGraceSec = 1.0;     // prazo do lado do script = timeoutMs do pool + isto (relógio do host)
  // Jogador (por did) pode usar comandos admin=true pelo chat. Vazio = ninguém (só console/painel).
  std::function<bool(const std::string& did)> isAdmin;
  size_t maxTimersPerResource = 10000;
  double minEveryMs = 10;
  size_t maxLocalQueue = 65536;  // eventos locais aguardando entrega (acima disso emitLocal dá erro)
  size_t maxLocalBytes = 65536;  // JSON de um evento local / argumento de cb.resource
  std::shared_ptr<ApiStats> stats;  // opcional
};

void installApi(HostOptions& opt, ApiOptions api = {});

}  // namespace cbscript
