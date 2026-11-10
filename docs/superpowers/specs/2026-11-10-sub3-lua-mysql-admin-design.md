# Subprojeto 3 — Lua + MySQL + painel admin — Design

Data: 2026-11-10
Status: aprovado em conversa, aguardando revisão da spec
Base: `2026-11-09-plataforma-design.md` (seções "Scripts Lua 5.4", "Banco", "Painel/console admin") e o
`cb-server` do subprojeto 2. Pendências relevantes: `docs/PENDENCIAS.md` (seção "Subprojeto 3").

## Objetivo

Permitir que comunidades programem servidores em Lua com isolamento, acesso assíncrono a MySQL/MariaDB e um painel
web de administração com login Discord via Hub — sem afetar o desempenho da simulação (CI de carga mantém os limites).

Fora do escopo: Lua no cliente (subprojeto 5), UI HTML dentro do jogo, voz, NPCs, launcher (subprojeto 4).

## Decisões

| Tema | Decisão |
|---|---|
| Linguagem | **Lua 5.4.7** (override de versão no vcpkg; o baseline traz 5.5) + **sol2 3.5** |
| Isolamento | um `lua_State` por resource |
| Threads | **uma thread de script** separada da simulação; filas nos dois sentidos |
| API | própria, por módulos: `cb.on`, `cb.events`, `cb.players`, `cb.commands`, `cb.timers`, `cb.log`, `cb.audit`, `cb.db`, `cb.http`, `cb.config`, `cb.resource` |
| Banco | MariaDB Connector/C (`libmariadb`, LGPL, link dinâmico), pool em threads próprias |
| Painel | web embutido no `cb-server`, porta própria (padrão 30510), **bind padrão 127.0.0.1** |
| Login admin | Discord via Hub: ticket com `aud = "<serverId>#admin"`; admins por Discord ID no `server.cfg` |

## Runtime Lua

### Carga e ciclo de vida

- `resources/<nome>/resource.json` ganha `"server": ["server/main.lua", ...]`, `"dependencies": [...]`,
  `"config": {...}` (livre, exposto em `cb.config`), `"permissions": {"http": ["dominio"], "db": true}`.
- Ordem de subida: topológica pelas dependências; ciclo ou dependência ausente → resource não sobe (erro no log e
  no painel); os demais sobem.
- `start/stop/restart` a quente (painel ou console). `stop`: dispara `resourceStop`, remove handlers, cancela timers,
  cancela consultas pendentes (resultado descartado), fecha o estado. Dependentes de um resource parado são parados
  antes dele e religados depois do restart.
- Erro em callback: log com nome do resource e traceback; o resource continua (só o callback falha).

### Sandbox

- Bibliotecas: `base` sem `dofile`, `loadfile`, `collectgarbage("count")` permitido; `load` só com modo `"t"` e
  ambiente explícito; `string`, `table`, `math`, `utf8`, `coroutine`; `os` reduzido a `time`, `clock`, `date`.
  Sem `io`, `debug` (exceto `traceback` interno), `package` nativo.
- `require(nome)`: carrega `<resource>/server/<nome com / >.lua` (mesma regra de caminho seguro do manifest), cache por
  resource; nada fora da pasta.
- Memória: alocador por estado com teto (`scripting.memoryLimitMB`, padrão 256) → erro de memória no callback.
- Vigia: hook de instruções; callback > 50 ms → aviso com nome do resource; > 5 s → o callback é abortado com erro e o
  resource é reiniciado (com back-off se repetir 3× em 1 min: fica parado e o painel mostra o motivo).

### Threads e filas

- **Simulação → scripts**: eventos (`playerJoin`, `playerLeave`, evento de cliente, comando) e, a cada tick, uma
  "foto" imutável dos jogadores (`id, name, did, sub, pos, state`) publicada via `std::atomic<std::shared_ptr>`.
- **Scripts → simulação**: comandos (`kick`, `ban`, `emit`, `setPosition`), aplicados no próximo tick.
- **Scripts → banco/HTTP**: pedidos para os pools; respostas voltam pela fila da thread de script e retomam a
  corrotina.
- Todo callback roda dentro de uma corrotina, então `cb.db.*` e `cb.http.*` podem "esperar" sem travar.
- Saturação: fila de eventos limitada (padrão 65 536); acima disso descarta eventos de cliente (conta em métrica) e
  nunca descarta `playerJoin/Leave`.

### API `cb.*`

| Função | Comportamento |
|---|---|
| `cb.on(nome, fn)` | `playerJoin(p)`, `playerLeave(p, motivo)`, `resourceStart()`, `resourceStop()` |
| `cb.events.on(nome, fn(p, data))` | evento de cliente; payload JSON (≤ 4 KiB) decodificado em tabela; cota por jogador já existente |
| `cb.events.emit(alvo, nome, data)` | servidor → cliente (`alvo` = id ou `-1` para todos); `data` serializada em JSON (≤ 4 KiB) |
| `cb.events.onLocal / emitLocal` | entre resources, mesma thread |
| `cb.players.list()` / `get(id)` | da foto do tick: `{id, name, did, sub, x, y, z, state}` |
| `cb.players.kick(id, motivo)` | kick + auditoria |
| `cb.players.ban(did, motivo, duracaoS?)` | grava ban (MySQL se configurado, senão `bans.json`) + kick se conectado; checado na entrada |
| `cb.players.setPosition(id, x, y, z)` | teleporte do servidor (com carência de carregamento) |
| `cb.commands.register(nome, fn(src, args), {admin=bool})` | console admin e chat (evento de cliente `cb:command`) |
| `cb.timers.after/every/cancel` | resolução de tick; cancelados no stop |
| `cb.log.info/warn/error` | log do servidor com nome do resource |
| `cb.audit(tipo, dados)` | linha JSONL de auditoria com `did` quando houver jogador |
| `cb.db.query/exec/insert/transaction` | ver "Banco" |
| `cb.http.get/post(url, ...)` | só domínios em `permissions.http`; https obrigatório; timeout 10 s; resposta ≤ 1 MiB |
| `cb.config` | `config` do `resource.json` (somente leitura) |
| `cb.resource(nome)` | tabela de exports do outro resource (retorno do script principal) |
| `cb.null` | sentinela para NULL do banco |

`echo` em C++ sai do servidor; um resource de exemplo `resources.example/echo` faz o eco em Lua.

## Banco (MySQL/MariaDB)

- Config: `db: { host, port, user, password, database, pool: 4, timeoutMs: 5000 }` (ou `null` = sem banco).
- Só consultas parametrizadas (`?`), via prepared statements; nada de concatenação.
- Tipos: inteiros → integer Lua (int64), float/double → number, DECIMAL → string, datas → string ISO, BLOB → string
  binária, NULL → `cb.null`.
- `cb.db.transaction(function(tx) tx.exec(...) end)`: commit se a função terminar, rollback em erro.
- Timeout por consulta; falha de conexão → erro no callback (não derruba o resource); reconexão automática.
- Métricas: consultas/s, latência p50/p99, erros, conexões ocupadas.
- Bans do servidor: tabela `cb_bans(did, reason, created_at, expires_at, created_by)` criada automaticamente.

## Painel admin

- `admin: { port: 30510, bind: "127.0.0.1", publicUrl: null, admins: ["<discord id>", ...] }`.
- `publicUrl` (ex. `https://admin.meuservidor.com`) é o endereço anunciado ao Hub no heartbeat (`adminUrl`); sem ele,
  o login via Hub fica desabilitado e o painel mostra como configurar.
- Interface HTML/CSS/JS sem framework, embutida no executável; CSP estrita; sem recursos externos.
- Telas: jogadores (kick, ban, teleporte), resources (status, último erro, iniciar/parar/reiniciar), console
  (comandos internos `status`, `players`, `restart <res>`, `kick`, `ban`, e os registrados com `admin=true`),
  logs ao vivo (auditoria/anti-cheat, últimas N linhas, atualização periódica), métricas.
- Sessão: cookie HttpOnly, `SameSite=Strict`, `Secure` quando `publicUrl` for https, 8 h; ações exigem Origin igual
  ao `publicUrl`/origem do painel; tudo auditado com `did`.

### Login via Hub (rota nova no Hub)

1. Painel → `{hub}/v1/auth/server-admin?serverId=<id>`.
2. Hub exige sessão de painel (se não houver, faz login Discord e volta).
3. Hub confere que o servidor existe e tem `adminUrl` anunciado; emite ticket com `aud = "<serverId>#admin"`
   (mesmo formato/assinatura dos tickets de jogo, TTL 300 s).
4. Hub redireciona **somente** para `<adminUrl>/auth/callback#ticket=<ticket>` (fragmento, não query).
5. JS do painel envia o ticket em `POST /auth/login`; servidor valida offline (`verifyTicket` com audiência
   `<serverId>#admin`, jti não repetido), confere `did` em `admin.admins`, cria a sessão.

Heartbeat ganha campo opcional `adminUrl` (https obrigatório, exceto loopback em dev); `HeartbeatBody` e a API do Hub
são atualizados em `@cbmp/contracts`.

## Testes

- Unitários C++: sandbox (cada função proibida bloqueada; `require` fora da pasta recusado; limite de memória), vigia
  (aviso e aborto; back-off de reinício), ordem de dependências e ciclos, ciclo de vida (timers/handlers somem no
  stop), API `cb.*` contra simulação falsa (eventos, emit, kick, setPosition, foto), corrotinas com respostas
  assíncronas falsas, JSON ↔ Lua.
- MySQL: testes de integração contra MariaDB real — no CI via service container `mariadb:11`; localmente quando
  `CBMP_TEST_MYSQL` estiver definido (ex. Docker), senão pulados.
- Painel: HTTP com ticket de admin válido/ inválido/ de jogo/ de não-admin; CSRF; ações auditadas.
- Hub (TS): rota `server-admin` (exige sessão, servidor com `adminUrl`, redireciona só para ele, ticket com aud
  `#admin`); `adminUrl` no heartbeat validado.
- Carga (CI): o resource `echo` em Lua substitui o eco C++; mesmos limites do subprojeto 2.

## Critério de pronto

Resource Lua de exemplo responde aos bots no CI de carga dentro dos limites; sandbox bloqueia `io`/`os.execute`/
arquivos fora/HTTP não declarado; consulta MySQL assíncrona funciona (CI); vigia reinicia resource travado; painel
lista jogadores, faz kick e reinicia resource com login Discord via Hub (verificação manual com Hub local); docs do
desenvolvedor de resources (`docs/RESOURCES.md`) escritas.

## Riscos

- sol2 + Lua 5.4 por override de versão: se o override falhar no vcpkg, usar overlay port fixo.
- Thread única de script pode virar gargalo com gamemodes pesados → métricas por resource (tempo de CPU) para guiar
  uma futura divisão em várias threads.
- Login via Hub depende de `publicUrl` acessível pelo navegador do admin (proxy com TLS documentado).
