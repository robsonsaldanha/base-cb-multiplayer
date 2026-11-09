# cb-server

Servidor dedicado de jogo: rede UDP (GameNetworkingSockets), HTTP na mesma porta (TCP) para `/info`, manifest, arquivos
e `/metrics`, simulação a 20 Hz com validação de movimento e anti-cheat. Spec: `docs/superpowers/specs/2026-11-09-sub2-cb-server-design.md`.

## Build

Requer CMake, Ninja (Linux), vcpkg (`VCPKG_ROOT`) no commit fixado em `.github/workflows/native.yml` e compilador C++20.

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

Copie `server/server.cfg.example` como ponto de partida. A config é validada na subida; em erro o servidor mostra
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

### Modo dev vs modo Hub (mutuamente exclusivos)

- **Hub (produção)**: informe `hub` e deixe `auth.devPublicKey` ausente. O servidor busca as chaves do Hub para validar
  os tickets e envia heartbeats para aparecer na lista. `hub` junto com `devPublicKey` é erro de config.
  - Para um Hub local use `http://127.0.0.1:8787`, **não** `http://localhost:8787`: `localhost` pode resolver para `::1`
    primeiro e o Hub escuta só em IPv4.
  - Em produção use sempre `https://`.
- **Dev**: informe `auth.devPublicKey` e `hub: null`; o servidor aceita tickets assinados pela chave privada correspondente
  (usado por `cb-bots` e `tools/loadtest.py`, serverId `srv_dev`). **Nunca coloque `devPublicKey` em produção**: quem tiver a
  chave privada entra como qualquer jogador.
- `sim.spawn.mode = "devTrustFirstInput"` (o jogador aparece onde mandar o primeiro Input, sem validação) só vale em modo
  dev; fora dele a config é recusada. Em produção use `origin`.
- As velocidades de `movement.maxSpeedCmS` são provisórias até o subprojeto 5.

## Portas e firewall

Abra **UDP 30500** (jogo) e **TCP 30500** (HTTP: `/info`, `/cb/manifest`, `/cb/files/<sha256>`, `/metrics`), ou o `port`
configurado. O HTTP escuta em `0.0.0.0`.

## resources/

Cada subpasta de `resourcesDir` é um resource:

```
resources/<pasta>/
  resource.json        {"name": "...", "version": "..."}  (sem ele a pasta é ignorada com aviso)
  client/  ui/  files/    publicados ao cliente (manifest + /cb/files/<sha256>)
  server/                 nunca publicado
```

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

- `logs/server.log`: log geral (também no console), rotação de 10 MB x 5 arquivos.
- `logs/anticheat-AAAA-MM-DD.jsonl`: uma linha JSON por evento, com `type`, `ts` (unix), `id` (entidade) e, quando o
  jogador existe, `did` (conta Discord) e `sub`. Tipos: `correction` (`seq`, `pos`), `kick` (`reason`) e `net_close`
  (`reason`; conexão fechada por violação de rede).

## /metrics

`GET /metrics` devolve JSON: `tickP50Ms`, `tickP99Ms`, `tickMaxMs`, `players`, `pending`, `corrections`, `kicks`,
`droppedEvents`, `decodeErrors`, `sendFailures`, `avgKbpsPerPlayer`/`maxKbpsPerPlayer` (payload),
`wireAvgKbpsPerPlayer`/`wireMaxKbpsPerPlayer`, `rssBytes`, `cpuPercent`.

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
- Cliente que nunca alcança a correção do servidor só é expulso pelo teto de 60 a 120 s.

## Teste de carga

Veja `docs/LOAD-TEST.md`.
