# Subprojeto 3 — Lua + MySQL + painel admin — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resources Lua 5.4 isolados por resource numa thread de script, com API `cb.*`, MySQL/MariaDB assíncrono, HTTP de saída controlado, e painel admin web com login Discord via Hub — mantendo os limites do CI de carga.

**Architecture:** Nova lib `server/script` (`cbscript`) com `ScriptHost` (thread única, filas, corrotinas), `Resource` (um `lua_State` sandbox por resource), API `cb.*` registrada via sol2. A simulação (`World`) publica uma foto imutável dos jogadores por tick e troca mensagens com o `ScriptHost` por filas MPSC. Banco e HTTP de saída em pools próprios que respondem pela fila do script. Painel admin é um segundo `httplib::Server` (porta própria) com sessão por cookie, cujo login usa ticket `aud = "<serverId>#admin"` emitido por rota nova do Hub.

**Tech Stack:** C++23, Lua 5.4.7 (vcpkg override), sol2 3.5, libmariadb 3.4 (LGPL, dinâmica), cpp-httplib 0.65 (cliente HTTPS + servidor admin), nlohmann-json, spdlog, Catch2; Hub em TS (Fastify) para a rota nova.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-11-10-sub3-lua-mysql-admin-design.md` (fonte da verdade); `docs/PENDENCIAS.md` seção "Subprojeto 3".
- Branch `sub3-scripting`; commit por task (mensagem + linha em branco + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); push só com OK do controlador; nunca tocar `castelobruxo/*`; nada de código MafiaHub/arcane-mp/castelo-mp.
- Build local Windows exige `-DVCPKG_INSTALLED_DIR=C:/cbmp-vi` (caminho curto); `C:\cbmp-vi` é cache, nunca apagar. Novos ports serão instalados lá (primeira vez demora).
- TEST_CASE em ASCII; sem warnings MSVC /W4; portável a GCC ≥ 13 com `-Wall -Wextra -Wpedantic` + ASan/UBSan (CI).
- **A simulação nunca espera a thread de script** (só filas não bloqueantes). Estado do `World` só é tocado na thread da simulação.
- Sandbox: sem `io`, `debug`, `package` nativo, `dofile`, `loadfile`, `os` além de `time/clock/date`; `load` só modo `"t"`; `require` só dentro de `<resource>/server/` com caminho seguro; memória por estado ≤ `scripting.memoryLimitMB` (padrão 256); vigia 50 ms aviso / 5 s aborto + restart com back-off (3× em 1 min → parado).
- Payloads de evento: JSON UTF-8 ≤ 4096 bytes nos dois sentidos.
- SQL só parametrizado. HTTP de saída: https obrigatório, só domínios de `permissions.http`, timeout 10 s, resposta ≤ 1 MiB, nunca IP privado/loopback (exceto modo de teste explícito).
- Painel admin: bind padrão `127.0.0.1`, porta 30510; cookie HttpOnly + SameSite=Strict (+Secure se `publicUrl` https), 8 h; ações exigem Origin do painel; tudo auditado com `did`.
- Ticket de admin: mesmo formato/assinatura/regras dos tickets de jogo, `aud = "<serverId>#admin"`, TTL 300 s; Hub redireciona só para `<adminUrl>/auth/callback#ticket=…`.
- CI de carga continua: 256 bots / 120 s / 2 vCPU, tick p99 ≤ 25 ms, banda ≤ 64 kbps, RSS ≤ 512 MB, 0 quedas — agora com o eco feito em Lua.

## Estrutura de arquivos

```
vcpkg.json                         + lua (override 5.4.7), sol2, libmariadb
server/script/                     lib cbscript
  include/cbscript/host.hpp        ScriptHost: thread, filas, ciclo de vida dos resources
  include/cbscript/resource.hpp    Resource: lua_State sandbox, require, memória, vigia
  include/cbscript/api.hpp         registro da API cb.* (players, events, commands, timers, log, audit, config, resource)
  include/cbscript/messages.hpp    tipos das filas Sim→Script e Script→Sim
  include/cbscript/json_lua.hpp    JSON ↔ tabela Lua
  include/cbscript/db.hpp          DbPool (libmariadb) + cb.db
  include/cbscript/http_out.hpp    HttpOutPool + cb.http
  include/cbscript/bans.hpp        BanStore (MySQL ou bans.json)
  src/*.cpp  tests/*.cpp
server/admin/                      painel admin
  include/cbadmin/admin_server.hpp src/admin_server.cpp
  web/index.html web/app.js web/app.css   (embutidos via CMake → header gerado)
  tests/*.cpp
server/src/main.cpp                integração (World ↔ ScriptHost, AdminServer, heartbeat adminUrl)
server/include/cbserver/world.hpp  + foto de jogadores, emit de Event para cliente, gancho de eventos, comandos de script
resources.example/echo/            resource Lua de exemplo (eco para bots)
packages/contracts/src/schemas.ts  + adminUrl no HeartbeatBody
hub/src/routes/admin-login.ts      GET /v1/auth/server-admin
docs/RESOURCES.md                  guia de quem escreve resources
```

---

### Task 1: Dependências, alvo `cbscript` e Lua 5.4 no ar

**Files:** Modify `vcpkg.json` (add `lua` com override `5.4.7`, `sol2`, `libmariadb`), root `CMakeLists.txt` (find_package de lua/sol2/libmariadb; `add_subdirectory(server/script)` antes de `server`), `server/CMakeLists.txt` (link `cbscript`); Create `server/script/CMakeLists.txt`, `server/script/tests/test_lua_smoke.cpp`; Modify `.github/workflows/native.yml` (job `linux` ganha service `mariadb:11` com env `CBMP_TEST_MYSQL=mysql://root:test@127.0.0.1:3306/cbtest` e health-check).

- [ ] Branch `sub3-scripting` a partir de `main`.
- [ ] `vcpkg.json`: `"overrides": [{"name": "lua", "version": "5.4.7"}]` (se o vcpkg não aceitar a versão exata, usar a 5.4.x disponível no histórico do port e registrar). Confirmar nomes de targets em `C:/cbmp-vi/x64-windows/share/{lua,sol2,unofficial-libmariadb|libmariadb}`.
- [ ] Teste (falhando, depois verde): `sol::state` abre `base,string,table,math,utf8,coroutine`; `_VERSION == "Lua 5.4"`; um script `return 1+1` retorna 2; `mysql_get_client_info()` retorna versão não vazia (link do libmariadb ok).
- [ ] Build local (primeira instalação dos ports em background) + ctest verde; YAML válido.
- [ ] Commit — `build(script): Lua 5.4 + sol2 + libmariadb e alvo cbscript`

### Task 2: `Resource` — sandbox, require, memória, vigia

**Files:** Create `server/script/include/cbscript/resource.hpp`, `src/resource.cpp`, `tests/test_resource.cpp`.

**Interfaces:**
```cpp
namespace cbscript {
struct ResourceManifest { std::string name, version; std::vector<std::string> serverScripts, dependencies;
                          nlohmann::json config; std::vector<std::string> httpAllow; bool db = false; };
ResourceManifest loadManifest(const std::filesystem::path& dir);       // valida (nome seguro, caminhos seguros)
struct Limits { size_t memoryBytes = 256u << 20; std::chrono::milliseconds warnAfter{50}, abortAfter{5000}; };
class Resource {
 public:
  Resource(ResourceManifest m, std::filesystem::path dir, Limits lim, std::function<void(std::string)> warn);
  sol::state& lua();                                   // estado sandbox já preparado
  bool runMain(std::string& err);                      // executa serverScripts em ordem; guarda retorno do último como exports
  sol::object exports() const;
  // Executa fn como corrotina com vigia; retorna erro (vazio = ok). Se abortado por tempo, `timedOut=true`.
  struct CallResult { bool ok; bool timedOut; bool yielded; std::string error; };
  CallResult call(sol::protected_function fn, sol::variadic_args args);   // (forma exata livre; documentar)
  size_t memoryUsed() const;
};
}
```
- [ ] Testes (RED→GREEN): cada item proibido (`io`, `os.execute`, `os.exit`, `os.getenv`, `debug.getinfo`, `package.loadlib`, `dofile`, `loadfile`, `load` com bytecode `"\27Lua..."`) → nil/erro; `os.time/clock/date` funcionam; `require("util.mod")` carrega `server/util/mod.lua` e cacheia; `require("../x")` e caminhos absolutos recusados; alocação acima do limite → erro "memória" e o estado continua usável; laço infinito → aviso após 50 ms (callback de aviso chamado) e aborto em 5 s (use limites curtos no teste: 20 ms/200 ms); erro em script → `err` com traceback e nome do resource; `loadManifest` recusa nome inválido e caminho com `..`.
- [ ] Commit — `feat(script): resource Lua com sandbox, require local, limite de memória e vigia`

### Task 3: `ScriptHost`, filas e ciclo de vida

**Files:** Create `include/cbscript/{host,messages,json_lua}.hpp`, `src/{host,json_lua}.cpp`, `tests/{test_host,test_json_lua}.cpp`.

**Interfaces:**
```cpp
namespace cbscript {
struct PlayerView { uint16_t id; std::string name, did, sub; int32_t x, y, z; uint8_t state; };
using PlayersSnapshot = std::shared_ptr<const std::vector<PlayerView>>;
// Sim → Script
struct EvPlayerJoin { PlayerView p; };  struct EvPlayerLeave { uint16_t id; std::string did; std::string reason; };
struct EvClientEvent { uint16_t id; std::string name; std::string json; };
struct EvCommand { std::optional<uint16_t> from; std::string adminDid; std::string line; std::function<void(std::string)> reply; };
using SimToScript = std::variant<EvPlayerJoin, EvPlayerLeave, EvClientEvent, EvCommand>;
// Script → Sim
struct CmdKick { uint16_t id; std::string reason; };
struct CmdEmit { int32_t target; std::string name; std::string json; };   // -1 = todos
struct CmdSetPosition { uint16_t id; int32_t x, y, z; };
using ScriptToSim = std::variant<CmdKick, CmdEmit, CmdSetPosition>;
struct ResourceStatus { std::string name; enum class State { Stopped, Running, Failed } state; std::string lastError; double cpuMsPerSec; size_t memBytes; };
class ScriptHost {
 public:
  ScriptHost(std::filesystem::path resourcesDir, Limits, std::function<double()> now);
  ~ScriptHost();                                       // para a thread
  void start();                                        // carrega manifests, ordem topológica, sobe todos
  // Chamados pela simulação (não bloqueiam):
  bool post(SimToScript ev);                           // false se fila cheia (descarta; Join/Leave nunca descartados)
  void publishPlayers(PlayersSnapshot);
  void drainCommands(std::vector<ScriptToSim>& out);
  // Chamados pelo painel/console (thread-safe; executados na thread de script):
  std::future<bool> startResource(std::string), stopResource(std::string), restartResource(std::string);
  std::vector<ResourceStatus> status() const;
};
}
```
- [ ] Regras (testadas): ordem topológica; ciclo/dependência ausente → só os afetados falham; stop para dependentes antes e os religa no restart; erro em callback não derruba o resource; aborto por tempo → restart com back-off (3× em 60 s → `Failed`, sem novas tentativas); fila de eventos limitada (65 536) com métrica de descarte; `json_lua` ida e volta (tabelas aninhadas, arrays, unicode, profundidade máx. 32, rejeita > 4096 bytes), números inteiros vs float preservados.
- [ ] Commit — `feat(script): host de scripts com thread própria, filas e ciclo de vida`

### Task 4: API `cb.*` (sem banco/HTTP) + integração com o `World`

**Files:** Create `include/cbscript/api.hpp`, `src/api.cpp`, `tests/test_api.cpp`; Modify `server/include/cbserver/world.hpp` + `src/world.cpp` (foto de jogadores por tick; `queueEvent(to, Event)` servidor→cliente; eventos de cliente e join/leave entregues a um callback; remover o eco C++), `server/src/main.cpp` (instancia `ScriptHost`, publica foto, drena comandos, encaminha eventos, `cb:command` do chat), `server/src/config.cpp` (seção `scripting {memoryLimitMB, warnMs, abortMs}`), Create `resources.example/echo/{resource.json,server/main.lua}`; Modify `tools/loadtest.py` (copia `resources.example/echo` para a pasta de resources do servidor de teste).

- [ ] API conforme tabela da spec (sem `db`/`http`, que entram nas Tasks 5–6): `cb.on`, `cb.events.on/emit/onLocal/emitLocal`, `cb.players.list/get/kick/ban*/setPosition` (`ban` provisório só com `bans.json` até a Task 5), `cb.commands.register` (admin flag), `cb.timers.after/every/cancel`, `cb.log.*`, `cb.audit`, `cb.config`, `cb.resource(nome)`, `cb.null`.
- [ ] Todo callback em corrotina; `cb.events.emit` serializa JSON ≤ 4 KiB (erro Lua se maior).
- [ ] Testes: cada função contra host com fila falsa; integração com `World` real: jogador entra → `playerJoin` no Lua; evento de cliente `echo` → `cb.events.emit` de volta → `Event` sai na fila de `Outgoing` do World para aquele jogador; `setPosition` vira Correction com carência; `kick` vira KickOrder; timers disparam com relógio injetado e somem no stop.
- [ ] Rodar local: 64 bots 30 s com o eco Lua (mesmos números de antes, ±10%); registrar.
- [ ] Commit — `feat(script): API cb.* integrada ao mundo e eco de exemplo em Lua`

### Task 5: MySQL/MariaDB (`cb.db`) e bans

**Files:** Create `include/cbscript/{db,bans}.hpp`, `src/{db,bans}.cpp`, `tests/{test_db,test_bans}.cpp`; Modify `api.cpp` (`cb.db`, `cb.players.ban` definitivo), `config.cpp` (`db` + validação), `server/src/net.cpp` ou `auth.cpp` (checar ban por `did` na entrada → `Reject{"banido"}`), CI (service mariadb já da Task 1).

- [ ] `DbPool(cfg, threads)`: conexões persistentes, reconexão, prepared statements, timeout; `query/exec/insert/transaction` assíncronos com resposta via fila do script; mapeamento de tipos da spec; métricas (qps, p50/p99, erros).
- [ ] `cb.db.*` exige `permissions.db = true` no resource.
- [ ] `BanStore`: tabela `cb_bans` criada automaticamente (MySQL) ou `bans.json` (atômico: escreve temp + rename); expiração; consultado pela simulação na entrada (cache em memória atualizado pelo script host — a simulação não faz I/O).
- [ ] Testes: unitários de mapeamento/validação sem banco; integração com MariaDB real quando `CBMP_TEST_MYSQL` definido (CI sempre; local opcional via `docker run mariadb:11`): CRUD, tipos, NULL, DECIMAL, transação com rollback, timeout, consulta sem permissão recusada, ban impede entrada.
- [ ] Commit — `feat(script): MySQL assíncrono (cb.db) e bans persistentes`

### Task 6: HTTP de saída (`cb.http`)

**Files:** Create `include/cbscript/http_out.hpp`, `src/http_out.cpp`, `tests/test_http_out.cpp`; Modify `api.cpp`.

- [ ] `cb.http.get(url, {headers})` / `post(url, body, {headers})` → `{status, headers, body}`; regras do Global Constraints (https, allowlist exata de host, sem IP privado/loopback resolvido — checar endereço após DNS, timeout 10 s, ≤ 1 MiB, sem seguir redirect para fora da allowlist); pool de 4 threads; modo de teste `allowInsecureLoopbackForTests` só via construtor de teste.
- [ ] Testes com servidor httplib local (modo de teste): sucesso, domínio fora da lista recusado, resposta grande cortada com erro, timeout, redirect para fora recusado, IP privado recusado (sem modo de teste).
- [ ] Commit — `feat(script): HTTP de saída controlado (cb.http)`

### Task 7: Hub — `adminUrl` e rota `server-admin`

**Files:** Modify `packages/contracts/src/schemas.ts` (`adminUrl?: string` em `HeartbeatBody`: URL https, ou http só para host loopback), `hub/src/store.ts` + migração (coluna `admin_url` em `server_status`), `hub/src/routes/servers.ts` (gravar), Create `hub/src/routes/admin-login.ts`, `hub/test/admin-login.test.ts`; Modify `server/src/hub.cpp` (enviar `adminUrl` = `admin.publicUrl` quando configurado), `docs/superpowers/specs/2026-11-09-plataforma-design.md` (tabela da API).

- [ ] `GET /v1/auth/server-admin?serverId=…`: exige sessão de painel (cookie); sem sessão → redireciona para `/v1/auth/discord/login?client=panel` guardando o destino (state no cookie `cbmp_return`, só para esta rota); servidor inexistente → 404; sem `adminUrl` registrado → página HTML explicando; emite ticket `aud = serverId + "#admin"` (TTL 300 s, jti novo) e redireciona **303** para `${adminUrl}/auth/callback#ticket=${ticket}`; rate limit 10/min por usuário; auditoria no log do Hub.
- [ ] Testes (Vitest, TDD): sem sessão → login; com sessão e servidor com `adminUrl` → Location exato com fragmento; ticket verifica com `verifyTicket` e aud `#admin`; `adminUrl` maliciosa no heartbeat (http não-loopback, com credenciais, com fragmento) recusada pelo schema; servidor de outro dono também funciona (o painel do servidor decide quem é admin — documentar).
- [ ] Commit — `feat(hub): adminUrl no heartbeat e login de admin de servidor via Hub`

### Task 8: Painel admin

**Files:** Create `server/admin/CMakeLists.txt` (embute `web/*` em header gerado), `include/cbadmin/admin_server.hpp`, `src/admin_server.cpp`, `web/{index.html,app.js,app.css}`, `tests/test_admin.cpp`; Modify `server/src/main.cpp`, `config.cpp` (`admin {port, bind, publicUrl, admins}` com validação: admins são ids Discord; `publicUrl` https ou http loopback).

- [ ] Rotas: `GET /` (app), `GET /auth/callback` (página que lê o fragmento e faz `POST /auth/login`), `POST /auth/login {ticket}` → valida (`verifyTicket` com aud `<serverId>#admin`, jti, `did ∈ admins`) e cria sessão; `POST /auth/logout`; `GET /auth/start` → redireciona ao Hub (`{hub}/v1/auth/server-admin?serverId=`); API JSON: `GET /api/players`, `POST /api/players/:id/kick`, `POST /api/players/:id/ban`, `POST /api/players/:id/teleport`, `GET /api/resources`, `POST /api/resources/:name/{start,stop,restart}`, `POST /api/console {line}`, `GET /api/logs?kind=audit|anticheat&tail=200`, `GET /api/metrics`. Ações passam pela simulação/script host via filas (nunca tocam o World direto). Tudo auditado.
- [ ] Segurança: CSP `default-src 'self'; frame-ancestors 'none'`, `X-Frame-Options DENY`, `Referrer-Policy no-referrer`, `Cache-Control no-store`; Origin obrigatório nas mutações; sessão 8 h em memória (id aleatório 32 bytes); limite de tentativas de login por IP; modo sem Hub (dev): `admin.devToken` aceito só quando `auth.devPublicKey` (dev) estiver ativo.
- [ ] Testes: login ok; ticket de jogo (aud sem `#admin`) recusado; não-admin recusado; jti repetido recusado; mutação sem Origin → 403; kick via API gera KickOrder e linha de auditoria; restart de resource via API reflete no status.
- [ ] Verificação manual (registrar no report): Hub local (`HUB_DEV_LOGIN=1 HUB_ALLOW_PRIVATE_SERVERS=1`), servidor com `hub` + `admin.publicUrl=http://127.0.0.1:30510` + seu did em `admins`, login pelo painel → Hub → volta logado; listar jogadores (bots), kick, restart do resource `echo`.
- [ ] Commit — `feat(admin): painel web com login Discord via Hub`

### Task 9: CI, docs e fechamento

**Files:** Modify `.github/workflows/native.yml` (load usa `resources.example/echo`; linux com MariaDB service rodando os testes de banco), `tools/loadtest.py` (cenário extra: script lento — resource de teste com laço de 2 s em `cb.events.on` não aumenta o tick p99 acima do limite e gera aviso de vigia), Create `docs/RESOURCES.md` (guia completo da API com exemplos), Modify `server/README.md` (seções scripting/db/admin/http), `docs/PENDENCIAS.md` (remover o que foi resolvido).

- [ ] CI verde (push com OK do controlador).
- [ ] Commit — `docs(script): guia de resources, README e CI do subprojeto 3`

## Fim do subprojeto 3

Critério (spec): eco Lua dentro dos limites no CI; sandbox bloqueia o proibido; MySQL assíncrono no CI; vigia reinicia resource travado; painel com login Discord via Hub (manual); `docs/RESOURCES.md`.
