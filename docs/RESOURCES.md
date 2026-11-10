# Guia de resources Lua do cb-server

Este guia é para quem escreve o lado servidor de um gamemode: scripts Lua 5.4 que reagem a jogadores, eventos,
comandos e timers, gravam no MySQL/MariaDB e chamam APIs HTTPS. Configuração e operação do servidor (portas, painel,
`server.cfg`) ficam em [`server/README.md`](../server/README.md). A spec de origem é
`docs/superpowers/specs/2026-11-10-sub3-lua-mysql-admin-design.md`.

Os blocos de código Lua deste guia são verificados no CI: todos precisam compilar, e os que começam com
`-- resource: <nome>` sobem de verdade num servidor de teste (`server/script/tests/test_docs.cpp`). Se você mudar um
exemplo, rode `ctest -R "^docs:"`.

## Sumário

1. [Visão geral](#visão-geral)
2. [Estrutura de um resource e `resource.json`](#estrutura-de-um-resource-e-resourcejson)
3. [Ciclo de vida](#ciclo-de-vida)
4. [Sandbox](#sandbox)
5. [Vigia, reinício e supervisor](#vigia-reinício-e-supervisor)
6. [Corrotinas: esperar sem travar](#corrotinas-esperar-sem-travar)
7. [JSON ↔ Lua](#json--lua)
8. [API `cb.*`](#api-cb)
9. [Banco de dados (`cb.db`)](#banco-de-dados-cbdb)
10. [HTTP de saída (`cb.http`)](#http-de-saída-cbhttp)
11. [Bans](#bans)
12. [Configuração do servidor que afeta scripts](#configuração-do-servidor-que-afeta-scripts)
13. [Limites (resumo)](#limites-resumo)
14. [Problemas comuns](#problemas-comuns)

## Visão geral

- Cada resource roda num **`lua_State` próprio** (isolado dos outros) dentro de uma **única thread de scripts**,
  separada da simulação. A simulação nunca espera os scripts: um script lento atrasa outros scripts, nunca o tick do
  jogo nem os snapshots.
- A comunicação é por filas. Eventos (entrada/saída de jogador, eventos de cliente, comandos) chegam aos scripts; os
  comandos dos scripts (`emit`, `kick`, `setPosition`) são aplicados pela simulação **no tick seguinte** (~50 ms a
  20 Hz).
- **Todo callback roda numa corrotina própria.** Por isso `cb.db.*` e `cb.http.*` "esperam" a resposta sem bloquear a
  thread: a corrotina cede, outros callbacks rodam, e ela é retomada quando o resultado chega.
- A API fica na tabela global `cb` (só leitura na prática: não substitua as funções). Erros de uso viram erro Lua em
  pt-BR com o nome da função (`cb.players.kick: id inválido`).
- Erro num callback: o servidor registra no log o nome do resource e o traceback; **o resource continua rodando**
  (só aquele callback falhou).

O menor resource possível:

```lua
-- resource: ola
cb.on('resourceStart', function()
  cb.log.info('pronto; jogadores online:', #cb.players.list())
end)

cb.on('playerJoin', function(p)
  cb.log.info(p.name .. ' entrou (id ' .. p.id .. ')')
  cb.events.emit(p.id, 'boasVindas', { texto = 'Bem-vindo, ' .. p.name .. '!' })
end)

cb.on('playerLeave', function(p, motivo)
  cb.log.info(p.name .. ' saiu: ' .. motivo)
end)
```

## Estrutura de um resource e `resource.json`

Cada subpasta de `resourcesDir` (padrão `resources/`) que tem `resource.json` é um resource:

```
resources/<nome>/
  resource.json
  server/        scripts Lua do servidor (nunca publicados ao cliente)
    main.lua
    util/texto.lua
  client/ ui/ files/   publicados ao cliente (manifest + /cb/files/<sha256>); ver server/README.md
```

Use o mesmo texto no nome da pasta e em `name`. Arquivos nativos (`.dll`, `.so`, `.exe`…) e links simbólicos em
qualquer lugar do resource impedem a subida (regras completas em `server/README.md`, seção `resources/`).

```json
{
  "name": "perfis",
  "version": "1.2.0",
  "server": ["server/main.lua"],
  "dependencies": ["economia"],
  "config": { "visitasParaBonus": 10, "mensagem": "Olá de novo!" },
  "permissions": { "db": true, "http": ["api.exemplo.com", "hooks.exemplo.com:8443"] }
}
```

| Campo | Obrigatório | Regras |
|---|---|---|
| `name` | sim | `A-Z a-z 0-9 . _ -`, até 64 caracteres, único entre os resources |
| `version` | sim | texto não vazio |
| `server` | não | lista de caminhos `server/....lua` (seguros: sem `..`, sem `\`, sem caminho absoluto). Rodam em ordem; o **valor retornado pelo último** vira os exports do resource (ver [`cb.resource`](#cbresourcenome)) |
| `dependencies` | não | nomes de outros resources; sobem antes deste |
| `config` | não | qualquer JSON; aparece em [`cb.config`](#cbconfig), somente leitura |
| `permissions.db` | não | `true` libera `cb.db.*` |
| `permissions.http` | não | lista de hosts que `cb.http.*` pode chamar (regras abaixo) |

Regras de `permissions.http` (validadas na carga; entrada inválida impede a subida do resource):

- só o **nome do host**, em **minúsculas**: `"api.exemplo.com"`. Nada de esquema (`https://`), caminho, `?`, `#`,
  `@`, nem curinga (`*.exemplo.com`);
- **sem IP literal** (`"1.2.3.4"`, `"[::1]"` são recusados): use o nome;
- o host é **exato**: `exemplo.com` não libera `www.exemplo.com` (liste os dois);
- sem porta = **só a 443**; outra porta só com `"host:porta"` (e então só aquela porta);
- nomes internacionais só em punycode (`xn--...`); repetidos são ignorados.

Um resource sem `server` (só arquivos para o cliente) é válido: sobe sem scripts.

## Ciclo de vida

- **Subida:** os resources sobem em ordem topológica das dependências (dependência primeiro; entre independentes, em
  ordem alfabética). Dependência ausente, ciclo (`a → b → a`), dependência que falhou, `resource.json` inválido ou erro
  no script principal deixam **só os resources envolvidos** em `Failed` (o motivo aparece no log e no painel); os
  outros sobem.
- **Script principal:** cada arquivo de `server` roda uma vez, sob a vigia. Use o nível de cima para registrar
  handlers, comandos e timers e para montar o estado. Depois que todos os arquivos rodaram, os handlers de
  `resourceStart` deste resource são chamados.
- **`stop`** (painel, console ou parada do servidor): para antes os dependentes que estão rodando, chama os handlers de
  `resourceStop`, remove handlers, comandos e timers, cancela consultas e pedidos HTTP pendentes (resultados
  descartados, transações desfeitas) e fecha o estado Lua. **Nada do estado Lua sobrevive**: guarde o que importa no
  banco.
- **`restart`**: `stop` + `start`, relendo `resource.json` do disco; os dependentes parados junto voltam depois.
  `start` manual também sobe dependências paradas.
- Iniciar um nome que o servidor ainda não conhece relê a pasta de resources: um resource novo pode subir sem
  reiniciar o servidor.

## Sandbox

O que existe no ambiente global:

| Disponível | Observação |
|---|---|
| `assert error getmetatable ipairs next pairs pcall rawequal rawget rawlen rawset select tonumber tostring type xpcall _VERSION` | iguais ao Lua 5.4 |
| `string` | sem `string.dump`; `getmetatable('')` devolve `false` |
| `table`, `math`, `utf8`, `coroutine` | completos |
| `os.time`, `os.clock`, `os.date` | o resto de `os` não existe |
| `load(texto, nome, modo, env)` | **só texto** (modo `"t"` sempre; bytecode é recusado). Sem `env` (ou `nil`), o chunk usa o ambiente do resource |
| `require(nome)` | ver abaixo |
| `setmetatable` | recusa metatable com `__gc` |
| `collectgarbage` | só `"count"` e `"step"` |
| `print(...)` | vai para o log do servidor (ver abaixo) |

Não existem: `io`, `debug`, `package`, `dofile`, `loadfile`, `warn`, `string.dump`, `os.execute`, `os.getenv`,
`os.remove`, `os.exit` etc. Não há acesso a arquivos.

**`require(nome)`** carrega `<resource>/server/<nome>.lua`, com `.` como separador de pasta (`require('util.texto')` →
`server/util/texto.lua`). Nomes só com `A-Z a-z 0-9 _` e `.`; nada fora de `server/`, sem links simbólicos. Cada
módulo roda uma vez por resource (cache); o módulo recebe o próprio nome em `...`; se ele não retornar nada, `require`
devolve `true`. Dependência circular entre módulos é erro.

```lua
-- arquivo: textos/server/util/texto.lua
local M = {}

function M.titulo(s)
  return (s:gsub('^%l', string.upper))
end

return M
```

```lua
-- resource: textos
local texto = require('util.texto')
cb.log.info(texto.titulo('olá, mundo'))
```

**Memória:** cada resource tem um teto (`scripting.memoryLimitMB`, padrão 256 MiB). Passar dele vira erro
`memória esgotada (limite N MiB)` no callback que alocou; o resource continua.

**`print`** junta os argumentos com tab e vai para o log como `[<resource>] print: ...`. Linhas acima de 64 KiB são
cortadas; acima de 200 linhas por segundo por resource o excesso é descartado (com uma linha de resumo). Caracteres de
controle, quebras de linha e controles bidi são escapados (`\n`, `\x1b`…): um script não consegue forjar linhas no log.

## Vigia, reinício e supervisor

- Toda chamada ao Lua (script principal, handler, timer, comando, export, retomada de corrotina) é medida.
  Passou de `scripting.warnMs` (padrão **50 ms**): aviso no log
  `[<resource>] vigia: execução levou N ms (...)`.
- Passou de `scripting.abortMs` (padrão **5 s**): a chamada é **abortada** com o erro
  `tempo esgotado: execução passou de 5000 ms e foi abortada pela vigia`. `pcall`, `xpcall`, `coroutine.resume` e
  `__close` **não** conseguem engolir esse erro. A vigia vale também dentro de funções da biblioteca que demoram
  (`string.find/match/gsub/gmatch` com padrões caros, `table.sort/insert/remove/move/concat`, `os.date`).
- Depois de um aborto o resource é **reiniciado** (estado Lua novo) com espera crescente: 1 s depois do 1º aborto,
  2 s depois do 2º. O **3º aborto em 60 s** deixa o resource em `Failed` (motivo: abortado por tempo 3 vezes), sem
  novas tentativas, até alguém dar `start`/`restart` pelo painel ou console. Dependentes parados junto voltam quando
  ele volta.
- **Supervisor:** uma única operação C sem pontos de checagem (por exemplo `string.rep` enorme perto do teto de
  memória) só pode ser interrompida quando termina. Se uma chamada ficar mais de `abortMs + 10 s` sem voltar, o
  supervisor registra um log **CRÍTICO**, marca todos os resources como `Failed` e para de entregar eventos aos
  scripts. A simulação continua; é preciso reiniciar o servidor.
- **Export travado:** uma chamada `cb.resource('x').f()` é síncrona e roda sob a vigia do chamador também. Se `f`
  travar, **os dois** resources levam o aborto (e o reinício com back-off).

Para trabalho pesado, divida em pedaços com `cb.timers.after(0, ...)` ou mova para o banco.

## Corrotinas: esperar sem travar

Todo callback roda numa corrotina nova. Quando ele chama `cb.db.*` ou `cb.http.*`, a corrotina **cede**; a thread de
scripts continua atendendo outros eventos; quando o resultado chega (na volta seguinte do laço, até ~10 ms depois),
a corrotina é retomada e a função devolve o resultado (ou lança o erro).

Consequências:

- **O mundo pode mudar durante a espera.** O jogador pode ter saído e o id dele pode ter sido reaproveitado por outra
  pessoa. Guarde `p.session` e passe-o para `emit`/`kick`/`setPosition` (ver [Sessões](#sessões)).
- **Onde não dá para esperar** (erro na hora):
  - no nível de cima do script principal: use `cb.on('resourceStart', function() ... end)`;
  - dentro de um export chamado por `cb.resource` (a chamada entre resources não espera);
  - dentro de funções chamadas pelo C que não podem ceder (comparador de `table.sort`, iteradores e metamétodos chamados
    pelo C);
  - `cb.http.*` dentro de `cb.db.transaction` (a transação seguraria a conexão do banco).
- **Corrotinas do próprio script** (`coroutine.create/wrap`): `cb.db`/`cb.http` dentro delas cedem para o **seu**
  `coroutine.resume`, que volta na hora sem valores. Quando o resultado chega, o servidor retoma essa corrotina
  sozinho. Não chame `coroutine.resume` nela de novo enquanto ela espera: isso gera o erro
  `cb.db: a corrotina foi retomada por fora (coroutine.resume) enquanto esperava o banco` dentro dela, e o resultado
  verdadeiro é descartado.

```lua
cb.on('playerJoin', function(p)
  local tarefa = coroutine.create(function()
    local linhas = cb.db.query('SELECT 1 AS um')
    cb.log.info('resultado chegou:', linhas[1].um)
  end)
  coroutine.resume(tarefa)  -- volta já: a corrotina está esperando o banco
  -- não retome `tarefa` de novo; o servidor faz isso quando o resultado chegar
end)
```

- Comandos (`cb.commands`) podem esperar: a resposta é o texto que o handler devolver no fim.

## JSON ↔ Lua

Payloads de eventos de cliente, `cb.events.emit`, eventos locais, `cb.resource`, `cb.audit` e corpos de
`cb.http.post` passam por JSON. O mapeamento é o mesmo em todos:

| JSON | Lua | Observação |
|---|---|---|
| objeto | tabela com chaves texto | chave `"1"` continua texto (não vira índice 1) |
| array | sequência `1..n` | |
| inteiro | integer | exato até ±(2^63 − 1); fora disso vira float (perde precisão) |
| número com `.` ou expoente | float | `1.0` continua float na volta (`"1.0"`) |
| texto | string | UTF-8 válido |
| `true`/`false` | boolean | |
| `null` | `cb.null` | |

Na volta (Lua → JSON):

- tabela com chaves exatamente `1..n` → array; **tabela vazia → `[]`** (então `{}` recebido do cliente volta como
  `[]`); outras tabelas → objeto, com chaves inteiras viradas texto (`{[5] = 'x'}` → `{"5":"x"}`);
- `nil` e `cb.null` → `null`. Um buraco numa lista (`{1, nil, 3}`) faz a tabela virar **objeto**
  (`{"1":1,"3":3}`): use `cb.null` para representar nulo dentro de listas;
- erro: função, userdata, corrotina, chave que não seja texto ou inteiro, NaN/infinito, texto que não seja UTF-8
  válido (dados binários precisam ser codificados, por exemplo em hex);
- profundidade máxima de **32** níveis de tabela; tamanho máximo do JSON: **4 KiB** para eventos de/para clientes,
  **64 KiB** para eventos locais, `cb.resource` e `cb.audit`, 1 MiB para corpos de `cb.http.post`;
- a mesma tabela em dois lugares é serializada nos dois; **ciclo** (tabela que contém a si mesma) é erro.

IDs grandes (Discord ID, ids do banco acima de 2^53) devem trafegar como **texto**: clientes JavaScript só representam
inteiros exatos até 2^53. `p.did` já é texto.

## API `cb.*`

### `cb.on(nome, fn)`

| Nome | Assinatura | Quando |
|---|---|---|
| `playerJoin` | `fn(p)` | jogador entrou (já autenticado e não banido) |
| `playerLeave` | `fn(p, motivo)` | jogador saiu; `motivo` é texto |
| `resourceStart` | `fn()` | este resource acabou de subir (script principal terminou) |
| `resourceStop` | `fn()` | este resource vai parar (último momento para limpar) |

Outros nomes são erro. Vários handlers para o mesmo nome rodam na ordem de registro; entre resources, na ordem em que
eles subiram.

### Tabela de jogador

`cb.players.list()`, `cb.players.get(id)` e os handlers recebem tabelas assim (cópias: mudar não afeta nada):

| Campo | Tipo | Significado |
|---|---|---|
| `id` | integer 1..65535 | id na partida; **é reaproveitado** depois que o jogador sai |
| `session` | integer | identifica esta conexão; único no processo, nunca reaproveitado |
| `name` | string | nome exibido |
| `did` | string | Discord ID (texto) |
| `sub` | string | id da conta no Hub |
| `x`, `y`, `z` | integer | posição em centímetros (foto do último tick) |
| `state` | integer | 0 parado, 1 andando, 2 correndo, 3 vassoura, 4 nadando, 5 caindo, 6 aparatando |

Posições vêm da **foto** que a simulação publica a cada tick: são de até um tick atrás.

### Sessões

Como ids são reaproveitados, toda referência a um jogador que atravessa uma espera (`cb.db`, `cb.http`, timer) deve
levar a sessão. `cb.events.emit`, `cb.players.kick` e `cb.players.setPosition` aceitam a sessão como último argumento
opcional: sem ela, valem para quem ocupa o id agora; com ela, devolvem `false` se o id já é de outra conexão. De todo
jeito, a simulação descarta o comando se o dono do id mudar antes de ele ser aplicado.

```lua
-- resource: saudacao
cb.on('playerJoin', function(p)
  local id, sessao = p.id, p.session
  cb.timers.after(5000, function()
    -- 5 s depois o jogador pode ter saído (e outro ter pegado o mesmo id)
    cb.events.emit(id, 'dica', { texto = 'Digite /ajuda para ver os comandos' }, sessao)
  end)
end)
```

### `cb.events`

**`cb.events.on(nome, fn(p, dados))`** recebe eventos enviados pelo cliente. `dados` é o payload JSON decodificado.

- Só chega evento de jogador conectado (depois do `playerJoin` dele) e da sessão atual.
- Nomes: 1 a 64 bytes, sem NUL; nomes começando com `cb:` são reservados ao motor (registrar um é erro; o cliente que
  manda um evento `cb:*` diferente de `cb:command` é ignorado).
- Payload acima de 4 KiB não existe (o protocolo recusa); JSON inválido é descartado com aviso no log (no máximo um
  por segundo).
- **Cota de entrada:** cada jogador pode mandar `events.perSecond` eventos por segundo com rajada `events.burst`
  (padrão 20/40, no `server.cfg`). Acima disso o jogador é **expulso** por flood de eventos.
- **Nunca confie no cliente:** valide tipos e valores de `dados`.

```lua
-- resource: feiticos
local permitidos = { lumos = true, nox = true, accio = true }

cb.events.on('lancarFeitico', function(p, dados)
  if type(dados) ~= 'table' or type(dados.nome) ~= 'string' or not permitidos[dados.nome] then
    return  -- payload inesperado: ignora
  end
  cb.events.emit(-1, 'feitico', { autor = p.id, nome = dados.nome })
end)
```

**`cb.events.emit(alvo, nome, dados, sessao?)` → boolean** envia um evento ao cliente. `alvo` = id do jogador ou
`-1` para todos os conectados. `dados` vira JSON (≤ 4 KiB, senão erro).

- Devolve `true` se o evento foi enfileirado; `false` se o jogador não está conectado, se a sessão não confere ou se a
  fila está cheia. `true` não garante entrega (o jogador pode sair antes do próximo tick).
- **Cota de saída por jogador:** `scripting.eventOutKBps` (padrão 8 KiB/s = 64 kbps) com rajada
  `scripting.eventOutBurstKB` (padrão 32 KiB). Cada evento conta nome + payload + 48 bytes. Acima da cota o evento é
  **descartado em silêncio** para aquele jogador (contado em `/metrics.scriptEventsOutDropped`).
- Por tick a simulação aplica no máximo 2048 comandos, 2 MiB de payload e 8192 entregas (um broadcast conta um por
  jogador); o resto fica para os ticks seguintes. Resources são atendidos em rodízio: um resource que inunda `emit`
  não atrasa os outros.

**`cb.events.onLocal(nome, fn(dados, origem))` / `cb.events.emitLocal(nome, dados)`** trocam eventos entre
resources do mesmo servidor. `origem` é o nome do resource que emitiu (o próprio resource também recebe).

- **A entrega é adiada:** `emitLocal` só enfileira; os handlers rodam logo depois que o callback atual terminar
  (nunca no meio dele, nunca de forma reentrante).
- `dados` é **copiado** via JSON (até 64 KiB): o receptor não vê a tabela original.
- Fila de até 65 536 eventos locais (acima disso `emitLocal` dá erro). Um evento local que emite outro entra na mesma
  sequência; a cada vez são entregues no máximo 64 rodadas encadeadas e o resto fica para a volta seguinte do laço.

```lua
-- resource: missoes
cb.events.onLocal('missaoConcluida', function(dados, origem)
  cb.log.info('missão', dados.missao, 'concluída por', dados.did, '(avisado por ' .. origem .. ')')
end)

cb.commands.register('concluir', function(src, args)
  local p = src ~= 0 and cb.players.get(src)  -- src 0 = console do painel
  if not p then return 'só jogadores' end
  cb.events.emitLocal('missaoConcluida', { did = p.did, missao = args[1] or 'sem-nome' })
  return 'ok'  -- os handlers de missaoConcluida rodam depois deste retorno
end)
```

### `cb.players`

| Função | Retorno | Comportamento |
|---|---|---|
| `list()` | lista de jogadores | conectados, da foto do último tick |
| `get(id)` | jogador ou `nil` | |
| `kick(id, motivo?, sessao?)` | boolean | expulsa no próximo tick; motivo padrão `"expulso"`, até 200 bytes; `false` se não conectado/outra sessão. Gera linha de auditoria `kick` |
| `ban(did, motivo?, duracaoS?)` | `true` | grava o ban (ver [Bans](#bans)) e expulsa toda conexão com esse `did`, inclusive quem acabou de entrar. `duracaoS` inteiro > 0 ou `nil` = permanente. Auditoria `ban` |
| `unban(did)` | boolean | `true` se havia ban. Auditoria `unban` |
| `setPosition(id, x, y, z, sessao?)` | boolean | teleporte em centímetros (inteiros de 32 bits). O cliente recebe uma correção do servidor com carência de carregamento de 30 s |

`kick` e `setPosition` vão por um canal prioritário: nunca ficam atrás de uma fila de `emit`. Vários `setPosition`
para o mesmo jogador no mesmo tick viram um só (vale o **último**); para `kick`, vale o primeiro. Uma ação do painel
admin e um comando de script para o mesmo jogador no mesmo tick seguem a mesma regra.

`did` em `ban`/`unban`: texto UTF-8 de 1 a 64 bytes, sem NUL.

### `cb.commands.register(nome, fn(src, args, linha), opcoes?)`

Registra um comando usado no chat do jogo (`/nome args`) e no console do painel admin.

- `nome`: 1 a 32 caracteres `A-Z a-z 0-9 _ - .`. O nome é **global**: outro resource registrar o mesmo nome é erro;
  o mesmo resource registrar de novo substitui. Some quando o resource para.
- `src`: id do jogador que digitou, ou `0` para console/painel.
- `args`: lista com as palavras depois do nome (separadas por espaço/tab).
- `linha`: a linha inteira sem a `/` inicial e sem espaços nas pontas (útil para texto livre).
- **Retorno:** se o handler devolver um texto, ele é a resposta: no chat vira o evento `cb:reply` para quem digitou
  (payload = texto JSON, cortado em 4 KiB sem quebrar UTF-8); no painel aparece no console. Sem texto, não há resposta.
  O handler pode esperar o banco/HTTP antes de devolver.
- Erro no handler responde `erro ao executar '<nome>' (ver log)`. Comando desconhecido responde
  `comando desconhecido: <nome>`.
- `opcoes.admin = true`:
  - **console do painel admin:** só comandos com `admin = true` são aceitos; os outros são recusados. Cada uso é
    auditado com o Discord ID do admin (`{"type":"command","by":"<did>",...}`);
  - **chat:** só jogadores cujo Discord ID está em `admin.admins` do `server.cfg` (a mesma lista do painel; mudar a
    lista exige reiniciar o servidor). Os demais recebem `sem permissão para '/<nome>'`; com a lista vazia (padrão),
    ninguém usa esses comandos pelo chat. Cada uso pelo chat é auditado (`{"type":"command","by":"<did>","id":...}`).
- Como o cliente manda um comando: evento `cb:command` com payload = texto JSON (`"/dado 20"`). A resposta volta no
  evento `cb:reply`.

```lua
-- resource: comandos
cb.commands.register('dado', function(src, args)
  local lados = math.tointeger(tonumber(args[1] or '6'))
  if not lados or lados < 2 or lados > 1000 then
    return 'uso: /dado [2..1000]'
  end
  return ('dado: %d (de %d)'):format(math.random(1, lados), lados)
end)

cb.commands.register('anunciar', function(src, args, linha)
  local texto = linha:match('^%S+%s+(.+)$')
  if not texto then return 'uso: anunciar <texto>' end
  cb.events.emit(-1, 'anuncio', { texto = texto })
  return 'anúncio enviado'
end, { admin = true })
```

### `cb.timers`

| Função | Retorno | Comportamento |
|---|---|---|
| `after(ms, fn)` | id | chama `fn()` uma vez depois de `ms` (≥ 0) |
| `every(ms, fn)` | id | chama `fn()` a cada `ms` (mínimo 10 ms). Atrasado, não tenta compensar as chamadas perdidas |
| `cancel(id)` | boolean | `true` se existia |

Resolução: a volta do laço da thread de scripts (até ~10 ms ocioso; mais se outro script estiver ocupado). No máximo
10 000 timers por resource. Todos somem quando o resource para.

```lua
-- resource: relogio
local avisos = 0
local id
id = cb.timers.every(60000, function()
  avisos = avisos + 1
  cb.events.emit(-1, 'hora', { unix = os.time(), aviso = avisos })
  if avisos >= 60 then cb.timers.cancel(id) end
end)
```

### `cb.log` e `cb.audit`

- **`cb.log.info(...)`, `cb.log.warn(...)`, `cb.log.error(...)`**: os argumentos passam por `tostring` e são
  unidos por espaço; a linha vai para o log do servidor com o prefixo `[<resource>]`. Até 64 KiB por linha e
  200 linhas por segundo por resource (o excesso é descartado e resumido). Controles e quebras de linha são escapados.
- **`cb.audit(tipo, dados)`** grava uma linha JSON em `logs/audit-AAAA-MM-DD.jsonl`:
  `{"type": tipo, "data": dados, "ts": <unix>, "resource": "<nome>"}`. `tipo`: texto de 1 a 64 bytes; `dados`:
  qualquer valor serializável (≤ 64 KiB). Se `dados` for uma tabela com `id` inteiro de um jogador conectado, a linha
  ganha `id`, `did` e `sub` dele. Limite de 200 linhas por segundo por resource (excesso vira uma linha
  `audit_dropped`). As linhas do motor (`kick`, `ban`, `unban`, `command`) nunca são limitadas.

```lua
-- resource: loja
-- manifest: {"config": {"moeda": "galeoes", "precos": {"varinha": 100, "vassoura": 250}}}
cb.commands.register('comprar', function(src, args)
  local item = args[1]
  local preco = item and cb.config.precos[item]
  if not preco then return 'item desconhecido' end
  cb.audit('compra', { id = src, item = item, preco = preco })
  return ('comprou %s por %d %s'):format(item, preco, cb.config.moeda)
end)

for item, preco in pairs(cb.config.precos) do
  cb.log.info('à venda:', item, preco)
end
```

### `cb.config`

A tabela `config` do `resource.json`, **somente leitura** (escrever é erro; `pairs`, `ipairs`, `#` funcionam; as
tabelas aninhadas também são só leitura). Se `config` não for um objeto, `cb.config` é o próprio valor. `null` no
JSON vira `cb.null`.

### `cb.resource(nome)`

Devolve um proxy para os exports de outro resource (o valor retornado pelo último script de `server` dele, que deve
ser uma tabela). Use `dependencies` para garantir que ele suba antes.

- `proxy.funcao(...)` chama a função exportada **na hora, de forma síncrona**, e devolve **um** valor.
- Argumentos e retorno são **copiados via JSON** (até 64 KiB cada): sem funções, sem userdata, sem tabelas
  compartilhadas por referência.
- `proxy.valor` (não função) devolve uma cópia.
- Erro: resource não está rodando, chamar a si mesmo, reentrância (o alvo está no meio de uma chamada), a função
  exportada falhar (`cb.resource: erro em 'x.f' (ver log)`), ou ceder (`cb.db`/`cb.http` dentro do export não são
  permitidos).
- Vigia: o tempo do export conta para os dois resources (ver [Vigia](#vigia-reinício-e-supervisor)).

```lua
-- resource: economia
local saldos = {}
local M = {}

function M.saldo(did)
  return saldos[did] or 0
end

function M.creditar(did, valor)
  saldos[did] = (saldos[did] or 0) + valor
  return saldos[did]
end

return M
```

```lua
-- resource: recompensa
-- manifest: {"dependencies": ["economia"]}
cb.on('playerJoin', function(p)
  local economia = cb.resource('economia')
  local novo = economia.creditar(p.did, 10)
  cb.events.emit(p.id, 'saldo', { valor = novo })
end)
```

### `cb.null`

Valor sentinela para `NULL` do banco e `null` do JSON. É um *lightuserdata*: compare com `==`
(`if v == cb.null then`). Atenção: **`cb.null` é verdadeiro** em `if`/`and`/`or`; `if linha.apelido then` é verdadeiro
para `NULL`.

## Banco de dados (`cb.db`)

Pré-requisitos: seção `db` no `server.cfg` (ver [`server/README.md`](../server/README.md#banco-de-dados)) e
`"permissions": {"db": true}` no `resource.json`. Sem eles, as chamadas dão erro.

| Função | Retorno |
|---|---|
| `cb.db.query(sql, ...)` | lista de linhas; cada linha é `{coluna = valor}` |
| `cb.db.exec(sql, ...)` | número de linhas afetadas |
| `cb.db.insert(sql, ...)` | último id inserido (`AUTO_INCREMENT`) |
| `cb.db.transaction(fn(tx))` | o primeiro valor que `fn` devolver |

- **Só SQL parametrizado:** um `?` por parâmetro. Parâmetros: `nil`/`cb.null` (NULL), boolean (0/1), integer, float,
  texto (binário permitido). Nunca concatene valores no SQL. O número de `?` (fora de aspas e comentários) tem de ser
  igual ao de parâmetros.
- As funções **esperam** (ver [Corrotinas](#corrotinas-esperar-sem-travar)): use dentro de handlers, nunca no nível de
  cima do script principal.
- Erro de SQL, banco fora do ar, tempo esgotado ou resultado grande demais viram **erro Lua** no callback. Use `pcall`
  se quiser tratar.

Tipos de coluna → Lua:

| MySQL/MariaDB | Lua |
|---|---|
| `TINYINT` … `BIGINT`, `YEAR`, `BIT` | integer |
| `BIGINT UNSIGNED` acima de 2^63 − 1 | texto decimal |
| `FLOAT`, `DOUBLE` | float |
| `DECIMAL` | **texto** (`"12.50"`), para não perder precisão |
| `DATE` | `"AAAA-MM-DD"` |
| `DATETIME`, `TIMESTAMP` | `"AAAA-MM-DDTHH:MM:SS"` (com fração se a coluna tiver) |
| `TIME` | `"[-]HH:MM:SS"` |
| texto, `BLOB`, `JSON`, `ENUM`, `SET` | texto (bytes) |
| `NULL` | `cb.null` |

Colunas com o mesmo nome se sobrescrevem na linha (`SELECT a.id, b.id`): use alias (`SELECT a.id AS a_id, ...`).

```lua
-- resource: perfis
-- manifest: {"permissions": {"db": true}, "config": {"bonusACada": 10}}
cb.on('resourceStart', function()
  cb.db.exec([[
    CREATE TABLE IF NOT EXISTS perfis (
      did VARCHAR(64) PRIMARY KEY,
      nome VARCHAR(64) NOT NULL,
      visitas INT NOT NULL DEFAULT 0,
      apelido VARCHAR(64) NULL
    )]])
end)

cb.on('playerJoin', function(p)
  local id, sessao = p.id, p.session
  cb.db.exec('INSERT INTO perfis (did, nome, visitas) VALUES (?, ?, 1) '
    .. 'ON DUPLICATE KEY UPDATE nome = VALUES(nome), visitas = visitas + 1', p.did, p.name)
  local linhas = cb.db.query('SELECT visitas, apelido FROM perfis WHERE did = ?', p.did)
  local perfil = linhas[1]
  if not perfil then return end
  local apelido = perfil.apelido ~= cb.null and perfil.apelido or p.name
  local bonus = perfil.visitas % cb.config.bonusACada == 0
  cb.events.emit(id, 'perfil', { apelido = apelido, visitas = perfil.visitas, bonus = bonus }, sessao)
end)
```

### Transações

```lua
-- resource: banco-galeoes
-- manifest: {"permissions": {"db": true}}
cb.commands.register('transferir', function(src, args)
  local de, para, valor = args[1], args[2], math.tointeger(tonumber(args[3] or ''))
  if not (de and para and valor and valor > 0) then return 'uso: transferir <de> <para> <valor>' end
  local ok, err = pcall(cb.db.transaction, function(tx)
    local r = tx.query('SELECT saldo FROM contas WHERE did = ? FOR UPDATE', de)
    if not r[1] or r[1].saldo < valor then error('saldo insuficiente', 0) end
    tx.exec('UPDATE contas SET saldo = saldo - ? WHERE did = ?', valor, de)
    tx.exec('UPDATE contas SET saldo = saldo + ? WHERE did = ?', valor, para)
  end)
  if not ok then return 'falhou: ' .. tostring(err) end
  return 'transferido'
end, { admin = true })
```

- `tx.query/exec/insert` rodam numa conexão dedicada. Se `fn` terminar, **COMMIT** e `transaction` devolve o 1º valor
  de `fn`; se `fn` lançar erro, **ROLLBACK** e o mesmo erro é relançado.
- Dentro de `fn` use **só `tx.*`**. Proibido (erro): `cb.db.*` direto, transação aninhada, usar `tx` depois do fim ou
  de outra corrotina, dois comandos ao mesmo tempo na mesma transação, `cb.http.*`.
- Corrotinas filhas (`coroutine.wrap`) criadas dentro de `fn` **não** estão na transação: um `cb.db.*` nelas usa
  outra conexão e pode esperar pelos bloqueios da própria transação.
- Aborto pela vigia, resource parado ou corrotina abandonada → ROLLBACK automático. Transação sem comando por 10 s →
  ROLLBACK pelo pool.
- No máximo `max(1, pool − 1)` transações abertas ao mesmo tempo (com `pool ≥ 2` sobra conexão para consultas
  avulsas). Com `db.pool: 1` cabe uma transação, que segura a única conexão (o servidor avisa no log): use `pool ≥ 2`.

### Limites e prazos

| Limite | Valor |
|---|---|
| prazo de cada consulta (fila + execução) | `db.timeoutMs` (padrão 5000 ms), +1 s de folga do lado do script |
| linhas / bytes por resultado | 10 000 linhas / 8 MiB (texto + 16 B por valor) → erro "grande demais" |
| SQL | 64 KiB; SQL + parâmetros de um pedido ≤ 1 MiB; até 1000 parâmetros |
| pedidos em andamento por resource | 1024 e 16 MiB de SQL + parâmetros |

- **Tempo esgotado em `exec`/`insert`/COMMIT** diz `(resultado incerto: o comando pode ter sido aplicado no banco)`:
  confira antes de repetir.
- No **MySQL** (não MariaDB) o prazo do servidor (`max_execution_time`) só vale para `SELECT`; nos outros comandos
  vale o timeout de leitura do cliente.
- `CALL` de procedure que devolve conjuntos de resultado não é suportado. `LOAD DATA LOCAL` é desligado.
- A fila do pool é única (FIFO) entre resources; cada resource é limitado pelos tetos acima.
- Conexão perdida: reconexão automática; enquanto o banco estiver fora, as consultas falham na hora
  (`banco indisponível (reconectando…)`).

## HTTP de saída (`cb.http`)

Pré-requisito: os hosts em `permissions.http` (regras na [seção do manifest](#estrutura-de-um-resource-e-resourcejson)).

| Função | Retorno |
|---|---|
| `cb.http.get(url, { headers = {...} }?)` | `{ status = n, headers = {...}, body = texto }` |
| `cb.http.post(url, corpo, { headers = {...} }?)` | idem |

- `corpo`: texto, ou tabela (enviada como JSON com `Content-Type: application/json`), ou `nil`.
- Qualquer status HTTP volta normalmente (404 não é erro). `headers` da resposta: nomes em minúsculas (se repetido,
  vale o último).
- Recusa (host fora da lista, endereço proibido), erro de TLS/DNS/conexão, tempo esgotado e resposta grande demais
  viram **erro Lua** no callback.
- As mesmas regras de espera de `cb.db` (nada no nível de cima, em `cb.resource`, em funções chamadas pelo C) e
  também proibido dentro de `cb.db.transaction`.

Regras de segurança:

- **só `https://`**; certificado e nome do host sempre verificados; URL ≤ 2048 bytes, sem usuário/senha, sem IP
  literal (inclusive formas como `127.1` ou `2130706433`);
- o host da URL tem de estar em `permissions.http` (exato; porta 443 ou a da entrada `host:porta`);
- o DNS é resolvido uma vez e **todos** os endereços precisam ser públicos: loopback, redes privadas (10/8,
  172.16/12, 192.168/16), CGNAT, link-local, multicast, ULA/link-local IPv6, IPv4 embutido em IPv6 etc. são recusados
  (proteção contra SSRF e DNS rebinding);
- redirecionamentos: até 3, cada salto com as mesmas regras; `https → http` nunca. Para outro host/porta os cabeçalhos
  do script são descartados e 307/308 com corpo são recusados; 301/302 de POST e 303 viram GET sem corpo;
- pedido: só GET/POST; cabeçalhos ≤ 8 KiB, sem CR/LF/NUL; não é permitido definir `Host`, `Content-Length`,
  `Transfer-Encoding`, `Connection`, `Accept-Encoding`, `TE`, `Upgrade`, `Keep-Alive`, `Proxy-*`, `Trailer`,
  `Expect`; corpo ≤ 1 MiB. `User-Agent` padrão: `cb-server`;
- resposta: corpo ≤ 1 MiB, cabeçalhos ≤ 64 KiB, sem compressão; prazo total de **10 s** (conexão, TLS, envio e
  leitura, somando os redirecionamentos), mais até 10 s de fila;
- por resource: até 64 pedidos pendentes, 4 MiB de corpos em andamento, no máximo 2 conexões simultâneas; a fila é
  dividida em rodízio entre resources;
- logs e mensagens de erro só mostram `https://host[:porta]`, nunca caminho, query, cabeçalhos ou corpo (tokens de
  webhook costumam ficar no caminho).

TLS e certificados:

- As CAs são as do sistema: no **Linux**, os caminhos padrão do OpenSSL (instale `ca-certificates`); no **Windows**,
  uma cópia das lojas ROOT e CA do Windows.
- Nada é baixado durante a verificação: o Windows **não** busca intermediários (AIA) nem raízes sob demanda. Um
  servidor que não manda a cadeia completa, ou cuja raiz não está na loja local, falha. Não há checagem de revogação.

```lua
-- resource: avisos-discord
-- manifest: {"permissions": {"http": ["discord.com"]}, "config": {"webhook": "https://discord.com/api/webhooks/ID/TOKEN"}}
cb.commands.register('avisar', function(src, args, linha)
  local texto = linha:match('^%S+%s+(.+)$')
  if not texto then return 'uso: avisar <texto>' end
  local ok, res = pcall(cb.http.post, cb.config.webhook, { content = texto })
  if not ok then
    cb.log.warn('webhook falhou:', res)
    return 'falhou (ver log)'
  end
  return 'enviado (HTTP ' .. res.status .. ')'
end, { admin = true })
```

## Bans

`cb.players.ban`, o painel e o console usam o mesmo armazenamento:

- **sem `db`** no `server.cfg`: arquivo `scripting.bansFile` (padrão `bans.json`), regravado de forma atômica a cada
  mudança;
- **com `db`**: tabela `cb_bans (did, reason, created_at, expires_at, created_by)`, criada se faltar.

Os bans ficam num cache em memória lido pela simulação na entrada de cada jogador (rápido, sem esperar o banco). Com
MySQL, `ban`/`unban` atualizam o cache na hora e a **gravação é adiada** (fila em ordem, com novas tentativas se o
banco estiver fora; até 100 000 gravações em fila, perdidas se o processo cair antes). Os bans do banco são lidos só
na subida (em páginas, com novas tentativas): bans gravados por outro servidor no mesmo banco só valem depois de
reiniciar, e enquanto a primeira carga não terminar os bans do banco não são aplicados (log CRÍTICO após 5 falhas;
`/metrics.db.bansLoaded`). Se a gravação falhar, o ban vale até o servidor reiniciar (erro no log).

## Configuração do servidor que afeta scripts

Detalhes e validação em [`server/README.md`](../server/README.md#servercfg).

| Campo | Padrão | Efeito |
|---|---|---|
| `resourcesDir` | `"resources"` | pasta dos resources |
| `scripting.memoryLimitMB` | `256` | teto de memória por resource |
| `scripting.warnMs` / `scripting.abortMs` | `50` / `5000` | aviso e aborto da vigia |
| `scripting.bansFile` | `"bans.json"` | arquivo de bans sem banco |
| `scripting.eventOutKBps` / `scripting.eventOutBurstKB` | `8` / `32` | cota de `cb.events.emit` por jogador |
| `events.perSecond` / `events.burst` | `20` / `40` | cota de eventos do cliente (acima: kick) |
| `db` | `null` | MySQL/MariaDB para `cb.db` e bans ([README](../server/README.md#banco-de-dados)) |
| `admin.*` | painel desligado | painel/console admin ([README](../server/README.md#painel-admin)) |

## Limites (resumo)

| Item | Limite |
|---|---|
| payload de evento cliente ↔ servidor | 4 KiB de JSON, profundidade 32; nome 1 a 64 bytes |
| eventos locais, `cb.resource`, `cb.audit` | 64 KiB de JSON |
| vigia | aviso 50 ms, aborto 5 s; 3 abortos em 60 s → `Failed` |
| memória por resource | 256 MiB |
| `print` / `cb.log` | 64 KiB por linha, 200 linhas/s por resource |
| `cb.audit` | 200 linhas/s por resource |
| timers | 10 000 por resource; `every` ≥ 10 ms |
| comandos | nome 1 a 32 caracteres; resposta no chat ≤ 4 KiB |
| `kick`/`ban` motivo | 200 bytes |
| saída por jogador | 8 KiB/s, rajada 32 KiB, 48 B por evento |
| fila de eventos para os scripts | 65 536 eventos de cliente (excesso descartado e contado); entrada/saída de jogador e comandos nunca são descartados abaixo de 4× isso |
| `cb.db` | 10 000 linhas / 8 MiB por resultado; 1024 pedidos e 16 MiB pendentes por resource |
| `cb.http` | 10 s; 1 MiB de corpo; 64 pendentes e 4 MiB por resource |

Contadores em `/metrics`: `scriptEventsDropped`, `scriptCommandsDropped`, `scriptCommandsRejected`,
`scriptEventsOutDropped`, `scriptLogDropped`, `scriptAuditDropped`, `scriptJoinLeaveDropped` e o objeto `db`.

## Problemas comuns

| Sintoma | Causa provável |
|---|---|
| `cb.db.query: só pode ser usado depois que o resource subiu` | chamada no nível de cima do script: mova para `resourceStart` ou um handler |
| `... não dá para esperar o banco aqui` | `cb.db`/`cb.http` dentro de `table.sort`, metamétodo ou iterador chamado pelo C |
| `cb.db.query: o resource não tem permissão de banco` | falta `"permissions": {"db": true}` |
| `cb.http.get: o resource não tem permissão de HTTP` / host recusado | host fora de `permissions.http` (exato, minúsculas, porta) |
| erro TLS só no Windows | o servidor remoto não manda o certificado intermediário |
| `if linha.campo then` verdadeiro para NULL | `cb.null` é verdadeiro: compare com `~= cb.null` |
| `{}` vira `[]` no cliente | tabela vazia é serializada como array |
| `[1,null,3]` vira objeto | buraco na lista: use `cb.null` |
| evento para o jogador não chegou | `emit` devolveu `false` (saiu/outra sessão) ou a cota de saída do jogador estourou (`scriptEventsOutDropped`) |
| handler de `emitLocal` "não rodou" | ele roda depois do callback atual terminar, não dentro dele |
| `vigia: execução levou N ms` | callback lento; divida o trabalho (timers) ou mova para o banco |
| resource em `Failed` "abortado por tempo 3 vezes" | laço longo ou infinito; corrija e dê `restart` pelo painel |
| `sem permissão para '/x'` no chat | comando `admin = true` e o Discord ID do jogador não está em `admin.admins` |
