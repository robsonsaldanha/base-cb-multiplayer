# Subprojeto 2 — Núcleo do `cb-server` + `cbmp-proto` + `cb-bots` — Design

Data: 2026-11-09
Status: aprovado em conversa, aguardando revisão da spec
Base: `2026-11-09-plataforma-design.md` (seções "cb-server", "Contratos", "Subprojetos" linha 2)

## Objetivo

Servidor de jogo em C++ que aceita jogadores autenticados por ticket do Hub, sincroniza movimento com
autoridade do servidor e streaming por distância, anuncia-se no Hub, serve resources por HTTP e aguenta a
meta de carga medida por bots — tudo testável **sem o jogo**.

Fora do escopo: Lua, MySQL, painel admin (subprojeto 3); voz; NPCs/objetos; cliente no jogo (subprojeto 5).

## Módulos

| Pasta | Alvo | Responsabilidade |
|---|---|---|
| `proto/` | lib `cbmp-proto` | bitstream, mensagens, quantização, validação de ticket (libsodium). Única peça compartilhada com o mod (subprojeto 5). |
| `server/` | exe `cb-server` | config, rede (GameNetworkingSockets), simulação, snapshots, HTTP, heartbeat, métricas |
| `bots/` | exe `cb-bots` | N jogadores simulados, cenário configurável, relatório JSON |

Build: CMake ≥ 3.25 + vcpkg (manifest mode). Dependências: GameNetworkingSockets (BSD-3), libsodium (ISC),
cpp-httplib (MIT), nlohmann-json (MIT), spdlog (MIT), Catch2 (BSL-1.0) para testes. Plataformas: Linux x64
(GCC ≥ 13) e Windows x64 (MSVC do VS 2026). C++23.

## Conexão

Canal confiável do GNS:

1. Cliente envia `Hello{protocol, buildHash[32], ticket}`.
2. Servidor valida o ticket **com as mesmas regras do `@cbmp/contracts`** e passa em **todos os casos de
   `packages/contracts/test-vectors/tickets.json`**: formato/charset, base64url canônico, JSON compacto sem
   chaves duplicadas, forma com inteiros literais simples, versão, `kid`, assinatura Ed25519 (64 bytes),
   `aud == serverId`, `iat ≤ now+60`, `exp+60 ≥ now`. Além disso, `jti` não pode ter sido visto (cache até
   `exp+60`). `protocol` diferente → recusa.
3. Resposta `Welcome{playerId, tickRateHz, serverTimeMs}` ou `Reject{reason}` e fechamento.
4. Antes do ticket válido nada é alocado por cliente: prazo de 5 s para `Hello`, limite de conexões
   pendentes por IP (padrão 4) e global (padrão 256), tamanho máximo de `Hello` 4 KiB.
5. Um mesmo `sub` (usuário do Hub) só pode ter uma sessão: nova conexão válida derruba a anterior.

Chaves do Hub: `GET {hub}/v1/keys/public` (raw 32 bytes base64url) no início, cache em memória, recarga quando
aparece `kid` desconhecido (no máximo 1x/min). Modo dev: `auth.devPublicKey` (base64url) aceita tickets
assinados localmente, sem Hub.

## Mensagens de jogo

Cabeçalho: `u8 type`. Tudo little-endian / bitstream LSB-first.

| Mensagem | Direção | Canal | Conteúdo |
|---|---|---|---|
| `Input` | C→S | não confiável, 20 Hz | `u32 seq`, posição (3×i32 cm), yaw u16, pitch u8, velocidade (3×i16 cm/s), estado u4 |
| `Snapshot` | S→C | não confiável | `u32 tick`, `u32 ackInputSeq`, `u5 baselineAge` (0 = sem baseline), `u3 partIndex`, `u3 partCount`, `u8` quantidade, entidades `{id em gap, máscara u4, posição delta por eixo com classe de tamanho, velocidade 11 bits×3 (12,5 cm/s), yaw 10 bits, pitch 6 bits, estado u4}`; cada parte ≤ 1100 B; o cliente confirma o tick só com todas as partes |
| `SnapshotAck` | C→S | não confiável | `u32 tick` recebido |
| `EntityEnter` / `EntityLeave` | S→C | confiável | id, estado completo / id |
| `Event` | ambos | confiável | `u16 nameLen`, nome, `u16 payloadLen`, payload (≤ 4 KiB) |
| `Correction` | S→C | confiável | posição/velocidade autoritativas + `seq` |
| `Kick` | S→C | confiável | motivo (string curta) |

Delta: cada snapshot por cliente é codificado contra o último snapshot que o cliente confirmou
(`SnapshotAck`); sem confirmação recente (> 1 s), envia estado completo.

Estados de movimento (u4): 0 parado, 1 andando, 2 correndo, 3 vassoura, 4 nadando, 5 caindo, 6 aparatando;
7–15 reservados.

## Autoridade e segurança

- Velocidade máxima por estado no `server.cfg` (padrões folgados, calibrados no subprojeto 5) com tolerância
  de 20%. Posição impossível → `Correction` + linha no log de anti-cheat; > 10 correções em 10 s **ou** 5 janelas
  de correção seguidas ignoradas → `Kick`.
  Correções iniciadas pelo servidor (spawn/teleporte) têm carência de carregamento e não contam para kick.
- Limite de eventos: token bucket por cliente e por nome de evento (padrão 20/s, rajada 40); excesso > 3x
  em 10 s → `Kick`.
- Banda máxima de saída por cliente (padrão 256 kbps); se excedida, reduz frequência dos mais distantes.
- Log de auditoria/anti-cheat em JSONL com `did` (Discord ID), rotação diária.

## Simulação

Tick 20 Hz (50 ms):

1. Drenar fila da rede; aplicar inputs válidos em ordem de `seq` (descartando seq antigo); gerar correções.
2. Atualizar grade espacial (células de 64 m).
3. Visibilidade por cliente: raio 300 m, sai em 330 m (histerese), teto 128 entidades mais próximas;
   frequência por distância: < 100 m 20 Hz, < 200 m 10 Hz, resto 5 Hz. Gera `EntityEnter/Leave`.
4. Montagem de snapshots **em paralelo**: pool de `max(1, núcleos−1)` threads, lotes de clientes, buffer por
   thread, sem trava compartilhada.
5. Entregar à rede; medir duração do tick.

A rede roda nas threads do GNS; a simulação é a única dona do estado do mundo.

## HTTP (mesma porta, TCP)

- `GET /info` → `ServerInfo` (`@cbmp/contracts`).
- `GET /cb/manifest` → `ResourceManifest`; `GET /cb/files/<sha256>` → conteúdo, `Cache-Control: immutable`.
  Fonte: pasta `resources/` do servidor; só arquivos de cliente da lista branca da plataforma
  (`client/`, `ui/`, `files/`; nunca `server/`), recusa subir se houver extensão nativa (`.dll .exe .so …`).
- `GET /metrics` → JSON, só para 127.0.0.1/::1 (ou com `metrics.token`).

## Hub

Heartbeat a cada 30 s (`POST /v1/servers/heartbeat`, `Bearer sk_…`, corpo `HeartbeatBody`). Falhas de rede
não derrubam o servidor; log com back-off. Sem `hub.url` configurado → não anuncia (modo LAN/dev).

## Configuração (`server.cfg`, JSON)

```json
{
  "name": "Meu servidor",
  "description": "",
  "tags": ["rp"],
  "locale": "pt-BR",
  "maxPlayers": 512,
  "port": 30500,
  "hub": { "url": "https://hub.exemplo", "serverId": "srv_...", "key": "sk_..." },
  "auth": { "devPublicKey": null },
  "net": { "pendingPerIp": 4, "pendingGlobal": 256, "maxOutKbpsPerClient": 256 },
  "sim": { "tickHz": 20, "streamRadiusM": 300, "maxVisible": 128 },
  "movement": { "maxSpeedCmS": { "idle": 50, "walk": 400, "run": 900, "broom": 4000, "swim": 400, "fall": 6000, "apparate": 0 }, "tolerance": 0.2 },
  "events": { "perSecond": 20, "burst": 40 },
  "metrics": { "token": null },
  "resourcesDir": "resources",
  "logsDir": "logs"
}
```

`apparate` = 0 significa "teleporte só por evento autorizado do servidor" (subprojeto 3).

## Métricas

Tick p50/p99/máximo (janela de 60 s), bytes enviados por cliente (média/máx), jogadores, conexões pendentes,
correções, kicks, eventos descartados, RSS, CPU do processo.
Banda por jogador (média/máx em kbps): bytes de payload das mensagens GNS enviadas à conexão (sem cabeçalhos
IP/UDP/GNS) ÷ tempo conectado, média/máximo entre os jogadores conectados; a estimativa "no fio" do GNS
(`m_flOutBytesPerSec`) é só informativa.

## Bots (`cb-bots`)

`cb-bots --server 127.0.0.1:30500 --dev-seed <hex 64> --server-id <id> --count N --area 2000 --cluster 0.3 --duration 120
--report report.json`

Cada bot assina seu ticket com a chave dev, conecta, anda dentro da área (fração `cluster` concentrada em
um ponto), troca estado, envia eventos, confirma snapshots. Relatório: conectados, quedas, RTT, intervalo
máximo entre snapshots por bot, bytes recebidos, rejeições.

## Testes

- Unitários (Catch2): bitstream e quantização (ida e volta, limites), vetores de ticket em C++ (todos os casos
  de `tickets.json` + cache de `jti`), grade e visibilidade (histerese, teto, frequência), token bucket,
  validação de movimento, manifest (lista branca, recusa de nativos).
- Fuzz (libFuzzer, Linux, alvo `proto_fuzz`): decodificador de todas as mensagens não pode travar nem ler fora
  do buffer (ASan + UBSan).
- Integração (CI): servidor em modo dev + bots.
  - **Carga (CI, ubuntu 2 vCPU): 256 bots, 120 s, 30% aglomerados. Falha se tick p99 > 25 ms, banda média por
    jogador > 64 kbps, RSS > 512 MB, algum bot cair, ou algum bot ficar > 1 s sem snapshot.**
  - Segurança: ticket inválido/expirado/aud errado/jti repetido recusado; teleporte corrigido; flood de
    eventos → kick; `Hello` lento → desconexão em 5 s.
- Manual (`docs/LOAD-TEST.md`): **512 bots em VPS Linux 4 vCPU** com os mesmos limites — obrigatório antes de
  produção.

## Critério de pronto

Build Linux + Windows; unitários e fuzz (curto no CI) verdes; carga do CI dentro dos limites; testes de
segurança verdes; `cb-server` com `hub` configurado aparece no Hub (verificado pelo probe) e some ao parar;
`/cb/manifest` e `/cb/files` servem uma pasta de resources de exemplo; `LOAD-TEST.md` escrito.

## Riscos

- GameNetworkingSockets via vcpkg puxa OpenSSL + protobuf: build lento no CI → cache do vcpkg (binary cache).
- Medidas do CI oscilam (runner compartilhado): limites com margem; repetir 1x antes de falhar.
- Valores de velocidade são placeholders até haver dados do jogo.
