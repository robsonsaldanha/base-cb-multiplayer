# cb-server

Servidor dedicado de jogo: rede UDP (GameNetworkingSockets), HTTP na mesma porta (TCP) para `/info`, manifest, arquivos
e `/metrics`, simulação a 20 Hz com validação de movimento e anti-cheat, scripts Lua 5.4 por resource (com MySQL/MariaDB
e HTTPS de saída) e painel admin web com login Discord via Hub. Specs:
`docs/superpowers/specs/2026-11-09-sub2-cb-server-design.md` e `docs/superpowers/specs/2026-11-10-sub3-lua-mysql-admin-design.md`.
Guia de quem escreve resources: [`docs/RESOURCES.md`](../docs/RESOURCES.md).

## Build

Requer CMake, Ninja (Linux), vcpkg (`VCPKG_ROOT`) no commit fixado em `.github/workflows/native.yml` e compilador C++23 (MSVC do VS 2026 ou GCC ≥ 13).

Linux (Ubuntu 24.04; dependências em `docs/LOAD-TEST.md`):

```bash
cmake --preset linux-release && cmake --build --preset linux-release
ctest --test-dir build-native/linux-release --output-on-failure
```

Windows (Visual Studio, PowerShell). Caminhos longos (como pastas do OneDrive) quebram o build do GameNetworkingSockets;
aponte os pacotes do vcpkg para um caminho curto:

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset windows-release -DVCPKG_INSTALLED_DIR=C:/cbmp-vi
cmake --build --preset windows-release
```

Binário: `build-native/<preset>/server/cb-server` (em `Release/` no Windows).

## Executar

```bash
cb-server --config server.cfg
```

Copie `server/server.cfg.example` como ponto de partida. **Ele não sobe como está**: preencha `hub` (produção)
**ou** `auth.devPublicKey` (desenvolvimento, nunca os dois). Para gerar uma chave dev: `cb-bots --dev-seed <hex de
64 dígitos> --print-dev-key`. A config é validada na subida; em erro o servidor mostra
o campo e o motivo (pt-BR) e não inicia (chaves desconhecidas só geram aviso). Ctrl+C / SIGTERM encerram com parada limpa.

## server.cfg

JSON; todos os campos são opcionais. Valores abaixo são os padrões.

| Campo | Padrão | Validação |
|---|---|---|
| `name` | `"Meu servidor"` | 3 a 64 caracteres (sem espaços nas pontas) |
| `description` | `""` | até 500 caracteres |
| `tags` | `["rp"]` | até 10; cada uma `[a-z0-9-]`, 1 a 24 caracteres |
| `locale` | `"pt-BR"` | `xx` ou `xx-YY` (ex. `en`, `pt-BR`) |
| `maxPlayers` | `512` | inteiro 1 a 2048 |
| `port` | `30500` | 1 a 65535 (UDP do jogo e TCP do HTTP usam o mesmo número) |
| `hub` | `null` | objeto `{url, serverId, key}`; `serverId` começa com `srv_`, `key` com `sk_`, `url` é `http(s)://...` |
| `auth.devPublicKey` | `null` | base64url canônico de 32 bytes (chave pública Ed25519) |
| `net.pendingPerIp` | `4` | 1 a 1024 (conexões ainda não autenticadas por IP) |
| `net.pendingGlobal` | `256` | 1 a 65536 |
| `net.maxOutKbpsPerClient` | `256` | 1 a 1000000 (ainda não aplicado; ver limites) |
| `sim.tickHz` | `20` | 1 a 60 |
| `sim.streamRadiusM` | `300` | 50 a 2000 (metros) |
| `sim.maxVisible` | `128` | 8 a 256 (entidades por snapshot) |
| `sim.spawn.mode` | `"origin"` | `origin` ou `devTrustFirstInput` (só dev) |
| `movement.maxSpeedCmS.{idle,walk,run,broom,swim,fall,apparate}` | 50 / 400 / 900 / 4000 / 400 / 6000 / 0 | números >= 0, em cm/s |
| `movement.tolerance` | `0.2` | 0 a 1 |
| `events.perSecond` / `events.burst` | `20` / `40` | 1 a 100000 (token bucket por jogador) |
| `metrics.token` | `null` | texto; habilita acesso remoto ao `/metrics` |
| `resourcesDir` | `"resources"` | caminho |
| `logsDir` | `"logs"` | caminho |
| `scripting.memoryLimitMB` | `256` | 1 a 65536; teto de memória de cada resource Lua |
| `scripting.warnMs` | `50` | 1 a 600000; chamada ao Lua mais longa que isso gera aviso da vigia |
| `scripting.abortMs` | `5000` | 1 a 600000, maior que `warnMs`; chamada abortada e resource reiniciado (3× em 60 s = parado) |
| `scripting.bansFile` | `"bans.json"` | caminho não vazio; bans quando não há `db` |
| `scripting.eventOutKBps` | `8` | 1 a 1000000; cota por jogador (KiB/s) de eventos servidor → cliente vindos de scripts |
| `scripting.eventOutBurstKB` | `32` | 8 a 1000000; rajada dessa cota |
| `db` | `null` | objeto (abaixo) ou `null` = sem banco |
| `db.host` | — | obrigatório, 1 a 255 caracteres |
| `db.port` | `3306` | 1 a 65535 |
| `db.user` | — | obrigatório, 1 a 80 caracteres |
| `db.password` | `""` | texto (nunca aparece em logs, erros ou métricas) |
| `db.database` | — | obrigatório, 1 a 64 caracteres |
| `db.pool` | `4` | 1 a 64 conexões |
| `db.timeoutMs` | `5000` | 100 a 600000; prazo de cada consulta (fila + execução) |
| `db.tls` | `"auto"` | `auto`, `off`, `required` ou `verify` (abaixo) |
| `admin.admins` | `[]` | ids Discord (texto, 17 a 20 dígitos), até 100; repetidos são ignorados. Vazio e sem `devToken` = painel desligado. Também são quem pode usar pelo chat os comandos de script com `admin = true` (lido na subida; em modo dev, quem tem a chave privada dev entra com qualquer did, inclusive o de um admin) |
| `admin.port` | `30510` | 1 a 65535, diferente de `port` |
| `admin.bind` | `"127.0.0.1"` | IP literal sem porta (`127.0.0.1`, `::1`, `0.0.0.0`, `::`, IP da máquina) |
| `admin.publicUrl` | `null` | origem `https://host[:porta]` (http só em loopback), sem caminho; anunciada ao Hub (`adminUrl`). Sem ela não há login via Hub |
| `admin.trustedProxies` | `[]` | até 32 IPs ou CIDRs (`127.0.0.1`, `10.0.0.0/8`, `::1`) de proxies cujo `X-Forwarded-For` vale para achar o IP do cliente |
| `admin.devToken` | `null` | só em modo dev (`auth.devPublicKey` e `hub` null); 32 a 256 caracteres ASCII visíveis. No modo Hub é erro de config |

### Modo dev vs modo Hub (mutuamente exclusivos)

- **Hub (produção)**: informe `hub` e deixe `auth.devPublicKey` ausente. O servidor busca as chaves do Hub para validar
  os tickets e envia heartbeats para aparecer na lista. `hub` junto com `devPublicKey` é erro de config.
  - Para um Hub local use `http://127.0.0.1:8787`, **não** `http://localhost:8787`: `localhost` pode resolver para `::1`
    primeiro e o Hub escuta só em IPv4.
  - Em produção use sempre `https://`.
- **Dev**: informe `auth.devPublicKey` e `hub: null`; o servidor aceita tickets assinados pela chave privada correspondente
  (usado por `cb-bots` e `tools/loadtest.py`, serverId `srv_dev`). **Nunca coloque `devPublicKey` em produção**: quem tiver a
  chave privada entra como qualquer jogador, inclusive com o did de um admin de `admin.admins` (e então usa pelo chat
  os comandos de script com `admin = true`).
- `sim.spawn.mode = "devTrustFirstInput"` (o jogador aparece onde mandar o primeiro Input, sem validação) só vale em modo
  dev; fora dele a config é recusada. Em produção use `origin`.
- As velocidades de `movement.maxSpeedCmS` são provisórias até o subprojeto 5.

## Scripts Lua

Cada resource com `"server": [...]` no `resource.json` roda num estado Lua 5.4 isolado (sandbox sem `io`, `os.execute`,
`debug`, arquivos), numa thread de scripts separada da simulação: script lento ou travado nunca segura o tick. A API
(`cb.on`, `cb.events`, `cb.players`, `cb.commands`, `cb.timers`, `cb.log`, `cb.audit`, `cb.db`, `cb.http`,
`cb.config`, `cb.resource`) está em [`docs/RESOURCES.md`](../docs/RESOURCES.md).

- **Vigia:** chamada acima de `scripting.warnMs` gera aviso no log (`[<resource>] vigia: execução levou N ms`); acima
  de `scripting.abortMs` é abortada e o resource reinicia com espera (1 s, 2 s); 3 abortos em 60 s deixam o resource
  parado (`Failed`) até um `start`/`restart` pelo painel. Uma chamada presa por mais de `abortMs + 10 s` (operação C
  única) dispara o supervisor: log CRÍTICO, todos os resources `Failed`, scripts param de receber eventos; a simulação
  segue e é preciso reiniciar o servidor.
- **Ordem de subida:** pelas `dependencies`; ciclo, dependência ausente ou erro no script deixam só os envolvidos em
  `Failed` (motivo no log e no painel). Na subida o log lista os que não subiram.
- **HTTP de saída** (`cb.http`): só https, só hosts de `permissions.http` de cada resource, nunca endereços
  privados/loopback; CAs do sistema (no Linux, pacote `ca-certificates`; no Windows, as lojas ROOT/CA sem busca de
  intermediários na rede: servidores que não mandam a cadeia completa falham).
- O resource de exemplo `resources.example/echo` (eco dos bots de carga) não é carregado em produção: só o que estiver
  em `resourcesDir`.

## Banco de dados

Opcional. Com a seção `db`, `cb.db` fica disponível aos resources com `permissions.db` e os bans passam do
`bans.json` para a tabela `cb_bans` (criada se faltar):

```json
{
  "db": {
    "host": "127.0.0.1",
    "port": 3306,
    "user": "cbserver",
    "password": "troque-me",
    "database": "cbserver",
    "pool": 4,
    "timeoutMs": 5000,
    "tls": "auto"
  }
}
```

- MariaDB ou MySQL 8, via MariaDB Connector/C (`libmariadb`, LGPL, ligada dinamicamente). Sempre TCP (inclusive para
  `localhost`), `utf8mb4`, só consultas parametrizadas, `LOAD DATA LOCAL` desligado.
- `tls`: `auto` = `off` para `localhost`/`127.0.0.1`/`::1` e `verify` para os demais; `off` não verifica nada (e não
  impede o servidor de negociar TLS se exigir); `required` = TLS sem conferir o certificado; `verify` = TLS com
  certificado e nome conferidos pelas CAs do sistema. Não há opção de CA própria: com certificado autoassinado ou de
  CA interna use `required`. Erro de conexão em `verify` que pareça de TLS sugere `required` no log.
- O servidor sobe mesmo com o banco fora do ar: as consultas falham na hora enquanto ele reconecta (espera crescente).
  Os bans do banco só valem depois da primeira carga (log CRÍTICO após 5 falhas seguidas; `/metrics.db.bansLoaded`).
- `pool: 1` gera aviso: uma transação aberta segura a única conexão. No máximo `max(1, pool − 1)` transações ao mesmo tempo (com `pool: 1`, uma).
- Na subida o log mostra `banco: user@host:porta/banco (pool, timeout, tls)` (sem a senha), a pasta dos plugins de
  autenticação e `bans: backend mysql|json`.

## Distribuição

Além do executável, leve ao lado dele:

- **Windows:** `libmariadb.dll` (e as demais DLLs que o build copia para `Release/`) e a pasta `plugins/libmariadb/`.
- **Linux:** `libmariadb.so*` (o triplet `vcpkg-triplets/x64-linux.cmake` liga só ela dinamicamente; confira com
  `ldd cb-server` na máquina de destino que ela é encontrada, ajustando `LD_LIBRARY_PATH` ou o RPATH do pacote) e a
  pasta `plugins/libmariadb/`. Pacotes do sistema: `ca-certificates` (CAs do `cb.http` e do `db.tls = "verify"`).
- Sem `plugins/libmariadb/`, contas MySQL 8 com `caching_sha2_password` (padrão do MySQL 8) e contas `ed25519` do
  MariaDB não conectam. O build já copia a pasta para junto do `cb-server`.
- A libmariadb é LGPL 2.1: distribua o aviso de licença dela junto (ligação dinâmica; a DLL/.so pode ser trocada).

## Portas e firewall

Abra **UDP 30500** (jogo) e **TCP 30500** (HTTP: `/info`, `/cb/manifest`, `/cb/files/<sha256>`, `/metrics`), ou o `port`
configurado. O HTTP escuta em `0.0.0.0`. O painel admin (**TCP 30510**) escuta só em `127.0.0.1` por padrão: **não**
abra essa porta no firewall; publique o painel por um proxy reverso com TLS (abaixo).

## Painel admin

Web, embutido no executável (HTML/CSS/JS sem dependências externas). Sobe quando `admin.admins` (ou `admin.devToken`
em dev) não está vazio: `painel admin em http://127.0.0.1:30510` no log.

```json
"admin": {
  "publicUrl": "https://admin.meuservidor.com",
  "port": 30510,
  "bind": "127.0.0.1",
  "admins": ["123456789012345678"]
}
```

Telas: jogadores (kick, ban, teleporte), resources (estado, último erro, iniciar/parar/reiniciar), console
(`help`, `status`, `players`, `restart <resource>`, `kick <id> [motivo]`, `ban <id> [motivo]` e os comandos que os
resources registram com `admin = true` — os demais são recusados no console —, chamados com origem `0` = console e
auditados com o did do admin), logs
(`audit-*.jsonl` e `anticheat-*.jsonl` do dia mais recente, últimas 100/200/500 linhas, até 1 MiB) e métricas.

**Login (modo Hub):** "Entrar com Discord" → `GET /auth/start` (gera um `state` aleatório num cookie HttpOnly
`SameSite=Strict` de 10 min) → página de confirmação no Hub → volta em `<publicUrl>/auth/callback#ticket=…&state=…`
→ o JS manda o ticket em `POST /auth/login`. O servidor confere o `state`, a assinatura e a audiência
`<serverId>#admin` do ticket, o `jti` (uso único) e se o Discord ID está em `admin.admins`. Sessão: cookie HttpOnly
`SameSite=Strict` (`__Host-cbadmin` + `Secure` quando a `publicUrl` é https), 8 h no máximo e 1 h sem uso; até 5
sessões por conta; "Sair" invalida no servidor. Limites: só logins que **falham** contam (10/min por IP, IPv6 por
/64, e 60/min no total); login com ticket e state válidos entra sempre; `/auth/start` aceita 60/min por IP.

**Modo dev:** sem Hub, entre com `admin.devToken` (a conta aparece como `dev`). Nunca use em produção.

**Segurança:** toda resposta leva CSP estrita (sem `unsafe-inline`), `X-Frame-Options: DENY`, `Referrer-Policy:
no-referrer` e `Cache-Control: no-store`. Requisições com `Host` fora da lista (host da `publicUrl`; `127.0.0.1`/
`localhost`/`[::1]` com a porta do painel quando o bind é loopback) recebem 421 (proteção contra DNS rebinding). Ações
exigem `Origin` igual ao do painel, o cabeçalho `X-CB-Admin: 1` e JSON de até 64 KiB. `X-Forwarded-For` só é
lido quando a conexão vem de um IP de `admin.trustedProxies` (o IP do cliente é o primeiro endereço não confiável
da direita para a esquerda; porta e zona IPv6 são ignoradas); por padrão nunca. Logins recusados por limite não
geram uma linha cada: sai um resumo por minuto (`"result":"rate_limited"`, contagem e até 16 IPs). Uma ação do painel (teleporte, kick) pode ser sobrescrita no mesmo
tick por um comando de script para o mesmo jogador (o último teleporte vence).
Cada ação vai para `logs/audit-AAAA-MM-DD.jsonl` com o Discord ID do admin (`type` `admin_login`, `admin_logout` ou
`admin` com `action` `kick`/`ban`/`teleport`/`resource`/`console`). O painel roda em threads próprias e nunca toca a
simulação direto: lê a foto do tick e o status dos resources e age pelas filas da simulação e dos scripts.

### Proxy reverso com TLS (produção)

Deixe `admin.bind` em `127.0.0.1` e ponha um proxy com HTTPS na frente. A `publicUrl` é o endereço que o navegador
do admin usa; o proxy precisa repassar o `Host` original (senão o painel responde 421). Exemplo (nginx):

```nginx
server {
  listen 443 ssl;
  server_name admin.meuservidor.com;
  ssl_certificate     /etc/letsencrypt/live/admin.meuservidor.com/fullchain.pem;
  ssl_certificate_key /etc/letsencrypt/live/admin.meuservidor.com/privkey.pem;
  location / {
    proxy_pass http://127.0.0.1:30510;
    proxy_set_header Host $http_host;   # $http_host mantém a porta (ex. :8443); $host a perde
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
  }
}
```

Com Caddy: `admin.meuservidor.com { reverse_proxy 127.0.0.1:30510 }` (Caddy já repassa o `Host` e cuida do
certificado). Atrás do proxy todos os acessos vêm do IP do proxy: configure `"trustedProxies": ["127.0.0.1"]`
(o IP de onde o proxy conecta) para os limites e a auditoria usarem o IP real do cliente. Sem isso, os limites por
IP valem para o proxy inteiro: um único cliente com 60 inícios por minuto impede qualquer um de *começar* um login
(quem já tem ticket e state válidos ainda entra), e o servidor avisa no log ao subir. O painel tem poucas threads (8) e segura uma por
conexão aberta: exponha-o só atrás de um proxy que bufferiza as requisições (nginx/Caddy), nunca direto. `bind` fora do loopback sem `publicUrl` https gera aviso no log: o tráfego (inclusive o cookie de sessão)
iria sem TLS.

**Hub local de teste:** `publicUrl` `http://127.0.0.1:30510` só é aceita pelo Hub com `HUB_ALLOW_PRIVATE_SERVERS=1`.

## resources/

Cada subpasta de `resourcesDir` é um resource:

```
resources/<pasta>/
  resource.json        {"name": "...", "version": "...", "server": [...], "dependencies": [...], "config": {...},
                        "permissions": {"db": true, "http": ["host"]}}  (sem ele a pasta é ignorada com aviso)
  client/  ui/  files/    publicados ao cliente (manifest + /cb/files/<sha256>)
  server/                 scripts Lua do servidor; nunca publicado
```

Os campos de scripts (`server`, `dependencies`, `config`, `permissions`) estão em
[`docs/RESOURCES.md`](../docs/RESOURCES.md#estrutura-de-um-resource-e-resourcejson).

"Erro" impede a subida; "ignorado" só gera aviso no log.

- nome da pasta e todo caminho publicado só com `A-Z a-z 0-9 . _ -` (e `/` entre segmentos): erro;
- `name` e `version` de `resource.json` são texto e `name` é único entre os resources: erro;
- extensões permitidas nas pastas publicadas: `.lua .html .css .json .png .jpg .webp .svg .woff .woff2 .ttf .ogg .mp3 .wav .webm .mp4`,
  e `.js` somente em `ui/`; as demais são ignoradas;
- arquivos fora de `client/ ui/ files/ server/` são ignorados;
- links simbólicos: erro;
- arquivos nativos (`.dll .exe .so .dylib .bat .cmd .ps1 .sh .msi .scr .com .jar .node`) em qualquer lugar do resource,
  inclusive `server/`: erro.

O manifest traz caminho, SHA-256 e tamanho de cada arquivo; o cliente baixa por hash.

## Logs

- `logs/server.log`: log geral (também no console), rotação de 10 MB x 5 arquivos. Inclui o que os scripts escrevem
  (`cb.log`, `print`, erros com traceback, avisos da vigia), sempre com `[<resource>]` e com controles escapados.
- `logs/audit-AAAA-MM-DD.jsonl`: auditoria — `cb.audit` dos scripts, `kick`/`ban`/`unban` feitos por scripts,
  `command` (comandos do console e comandos de admin do chat), `ban_write_dropped` (gravação de ban recusada pelo
  banco), `admin_login`/`admin_logout`/`admin` (painel).
- `logs/anticheat-AAAA-MM-DD.jsonl`: uma linha JSON por evento, com `type`, `ts` (unix), `id` (entidade) e, quando o
  jogador existe, `did` (conta Discord) e `sub`. Tipos: `correction` (`seq`, `pos`), `kick` (`reason`) e `net_close`
  (`reason`; conexão fechada por violação de rede).

## /metrics

`GET /metrics` devolve JSON: `tickP50Ms`, `tickP99Ms`, `tickMaxMs`, `players`, `pending`, `corrections`, `kicks`,
`droppedEvents`, `decodeErrors`, `sendFailures`, `encodeErrors`, `avgKbpsPerPlayer`/`maxKbpsPerPlayer` (payload),
`wireAvgKbpsPerPlayer`/`wireMaxKbpsPerPlayer`, `rssBytes`, `cpuPercent` e os contadores dos scripts:
`scriptEventsDropped` (eventos de cliente descartados com a fila dos scripts cheia), `scriptJoinLeaveDropped`,
`scriptCommandsDropped`/`scriptCommandsRejected` (comandos de script descartados/inválidos),
`scriptEventsOutDropped` (eventos acima da cota `scripting.eventOut*`), `scriptLogDropped`, `scriptAuditDropped`.
Com `db` configurado há também o objeto `db`: `queries`, `errors`, `timeouts`, `qps`, `p50Ms`, `p99Ms`, `busy`,
`connected`, `size`, `queued`, `activeTx`, `bansLoaded`, `bansPendingWrites`.

Acesso: só de loopback, ou de fora com `Authorization: Bearer <metrics.token>`. Cuidado: um proxy reverso na mesma
máquina conecta por loopback e, sem filtro, expõe o `/metrics` a todos; bloqueie essa rota no proxy.

## Regras de kick

- mais de 10 incidentes de movimento em 10 s;
- 5 janelas de correção seguidas ignoradas pelo cliente;
- flood de eventos acima de `events.*`;
- violações de rede (mensagens inválidas, conexão lenta demais).

Correções emitidas pelo próprio servidor (spawn, teleporte) têm carência de 30 s para o cliente carregar antes de contarem.

## Limites conhecidos

- O teto de saída por cliente (`net.maxOutKbpsPerClient`) não é aplicado ainda.
- Limite de taxa de novas conexões por IP: adiado (existe só o limite de pendentes).
- Velocidades provisórias até o subprojeto 5.
- Cliente que nunca alcança uma correção feita pelo servidor (spawn/teleporte) **não é expulso**: a Correction é
  reenviada uma vez após 30 s e os inputs dele seguem ignorados (fica parado no servidor). O teto com kick continua
  pendente (`docs/PENDENCIAS.md`).
- HTTP escuta só em IPv4 (`0.0.0.0`); o jogo (GNS) escuta em IPv4 e IPv6. Corrigir antes do launcher (subprojeto 4).
- Uma única thread roda todos os scripts: um resource pesado atrasa os outros (nunca a simulação).
- Sessões do painel, bans lidos do MySQL e limites do painel ficam em memória/na subida: restart desloga os admins;
  bans gravados por outro servidor no mesmo banco só valem depois de reiniciar.

## Teste de carga

Veja `docs/LOAD-TEST.md`.
