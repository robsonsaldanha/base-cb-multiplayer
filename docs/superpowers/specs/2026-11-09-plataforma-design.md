# Plataforma multiplayer (base-cb-multiplayer) — Design

Data: 2026-11-09
Status: aprovado em conversa (partes 1–3), aguardando revisão da spec
Repo: `robsonsaldanha/base-cb-multiplayer` (privado)

Substitui a parte "servidor em C++ com runtime Node" da spec
`2026-11-09-cb-mp-spike-design.md`. O spike in-game daquela spec continua válido e aguarda teste.

## Visão

Plataforma no estilo FiveM/MTA para Hogwarts Legacy:

- **Hub**: comunidades publicam servidores; jogadores navegam pela lista.
- **Servidor base (`cb-server`)**: programa que a comunidade baixa e roda; carrega resources (scripts Lua, UI, assets).
- **Launcher**: login, lista de servidores, clique → baixa resources do servidor → abre o jogo já conectado.
- **Cliente no jogo**: mod (base do spike) que sincroniza jogadores, roda scripts Lua de cliente e UI.

O jogo é só a base (mundo, personagens, animações). Instalação, conexão, servidores e conteúdo são da plataforma.

## Requisitos

| Tema | Decisão |
|---|---|
| Escala | **até ~512 jogadores simultâneos por servidor** |
| Login | **Discord** (OAuth2) via Hub |
| Scripts | **Lua 5.4**, servidor e cliente; estado Lua por resource |
| Banco | **MySQL/MariaDB** nativo, assíncrono, na API Lua |
| Publicação | chave secreta do servidor + heartbeat 30 s |
| Download de resources | do **próprio servidor**, cache por sha256 |
| UI do Hub | launcher para jogadores + painel web pequeno para donos |
| Launcher | **Tauri 2** (decisão do Robson) |
| Releases | GitHub Releases do repo privado (repo/releases públicos decididos antes do lançamento) |
| Segurança | itens S1–S6 abaixo |
| Teste | tudo que não exige o jogo deve ser testável sem ele (bots, cliente falso) |

Fora do escopo: anti-cheat em nível de kernel; voz (decidir depois); loja/monetização; reescrita dos resources do CB (subprojeto 6).

## Componentes

```
                     ┌────────────── VPS da plataforma ──────────────┐
 Launcher (Tauri)   │ Hub (Node 22 + TS + Fastify + SQLite)          │
  - login Discord ───┼─► OAuth Discord, tickets Ed25519, lista,       │
  - lista/filtros    │   heartbeats, painel de donos, bans globais    │
  - download/cache   └────────────────────────────────────────────────┘
  - abre o jogo           ▲ heartbeat 30 s (HTTPS, chave do servidor)
        │                 │
        │ HTTP: manifest/arquivos     ┌──────────── máquina do dono ────────────┐
        └────────────────────────────►│ cb-server (C++)                          │
 Jogo + mod cb-mp ◄──UDP (GNS)───────►│  rede │ simulação 20 Hz │ Lua │ HTTP │ admin │
   (cliente Lua, UI)                  │  MySQL/MariaDB (pool C++)                │
                                      └──────────────────────────────────────────┘
```

## Contratos (compartilhados por todas as peças)

### Ticket de jogador (Ed25519)

- Formato: `base64url(payloadJSON) + "." + base64url(assinatura)`; assinatura Ed25519 sobre os bytes do payload.
- Payload: `{ "v": 1, "kid": "<id da chave>", "sub": "<id do usuário no Hub>", "did": "<discord id>", "name": "<nome exibido>", "aud": "<serverId>", "iat": <unix s>, "exp": <unix s>, "jti": "<uuid>" }`.
  Claim opcional `"nonce"` (texto, depois de `jti`): só em tickets de admin emitidos com `state`, `nonce = base64url(sha256(state))`; o painel do servidor exige e confere com o `state` do próprio cookie.
- Validade: `exp - iat = 300` s. Servidor aceita relógio com ±60 s de tolerância.
- `cb-server` valida offline com as chaves públicas do Hub (`GET /v1/keys/public`, cache; recarrega quando aparece `kid` desconhecido, no máximo 1x/min). Rejeita `aud` diferente do próprio `serverId`, expirado, ou `jti` já visto (cache de `jti` até expirar).
- Chave pública: 32 bytes raw da chave Ed25519 em **base64url** (sem padding).
- Regras de formato (qualquer desvio → `malformed`): payload em JSON **compacto** (nenhum espaço/tab/quebra de linha fora de strings); cada chave `v/kid/sub/did/name/aud/iat/exp/jti` aparece exatamente uma vez (contar ocorrências de `"<chave>":` no corpo == 1) e `nonce` no máximo uma vez (se presente, texto); as duas partes em base64url **canônico** (sem padding; reencodar os bytes decodificados dá o mesmo texto); `v`, `iat` e `exp` são literais inteiros simples (sem `1.0`, sem expoente) e inteiros seguros.
- Ordem das checagens (TS e C++ iguais): formato/charset → base64url canônico → JSON (válido, compacto, sem chave duplicada) → forma (tipos, inteiros) → `v` (`bad_version`) → `kid` (`unknown_kid`) → assinatura de 64 bytes (`bad_signature`) → `aud` (`wrong_audience`) → `iat > now+60` (`not_yet_valid`) → `exp+60 < now` (`expired`).
- Vetores de teste (payload, chave, assinatura esperada) ficam em `packages/contracts/test-vectors/` e são usados pelos testes TS **e** C++.
- Modo dev do servidor: aceita tickets assinados por uma chave dev local (config `auth.devPublicKey`), sem Hub.

### API do Hub (`/v1`, JSON, HTTPS)

| Método | Rota | Auth | Uso |
|---|---|---|---|
| GET | `/v1/auth/discord/login?client=launcher\|panel` | — | inicia OAuth; launcher exige `code_challenge` + `code_challenge_method=S256` |
| GET | `/v1/auth/discord/callback` | — | fim do OAuth; launcher recebe `cbmp://auth?code=…` ou `cbmp://auth?error=access_denied`; painel recebe cookie |
| POST | `/v1/auth/exchange` `{code, codeVerifier}` | — | launcher troca o código (uso único, 60 s) por `{accessToken, accessExpiresAt, refreshToken, refreshExpiresAt}` |
| POST | `/v1/auth/refresh` `{refreshToken}` | refresh token | renova sessão (gira os dois tokens) |
| POST | `/v1/auth/logout` | sessão (Bearer ou cookie) | encerra sessão; aceita `{refreshToken}` |
| GET | `/v1/me` | sessão (Bearer ou cookie) | perfil |
| POST | `/v1/tickets` `{serverId}` | sessão | emite ticket p/ aquele servidor; recusa (`banned_global`) só se o usuário está na lista global **e** aquele servidor optou por usá-la |
| GET | `/v1/keys/public` | — | `[{kid, publicKey}]`, `publicKey` = 32 bytes raw em base64url |
| GET | `/v1/servers` | — | lista pública (cache 5 s); cada item traz `featured`; ordem: destacados primeiro, depois jogadores desc, depois nome |
| GET | `/v1/servers/:id` | — | detalhe (`ServerDetail`: online → campos de conexão incl. `protocol`; offline → id/name/description/tags/locale; ambos com `featured`) |
| POST | `/v1/servers/heartbeat` | `Bearer <chave do servidor>` | anúncio do servidor; `adminUrl` opcional = origem do painel admin (`https://host[:porta]`, http só em loopback; sem caminho/query/fragmento/credenciais/IDN; ≤ 256; normalizada — mesmo algoritmo no C++, vetores em `test-vectors/admin-origin.json`). Inválida, ou loopback/IP privado/`*.localhost` sem `HUB_ALLOW_PRIVATE_SERVERS=1`, é **ignorada** (resposta traz `adminUrlIgnored: invalid\|private_host`; o heartbeat segue). Ausente apaga a anterior |
| GET | `/v1/auth/server-admin?serverId=srv_…[&state=…]` | cookie do painel | só **confirmação** (nunca ticket): página com servidor, `serverId`, origem de destino, identidade Discord e botão "Entrar como admin" (form POST + token CSRF; CSP `form-action 'self' <adminUrl>` — só o esquema para IPv6 literal —, `frame-ancestors 'none'`, `Referrer-Policy: same-origin` para o POST levar `Origin`). Sem sessão → `/v1/auth/discord/login?client=panel&return=<serverId>[&return_state=…]` (retorno guardado no state OAuth, uso único) e volta aqui. `state` opcional `[A-Za-z0-9_-]{16,64}` (do painel do servidor). serverId/state inválidos 400, inexistente 404, offline 503, sem `adminUrl` 409 |
| POST | `/v1/auth/server-admin` (form: `serverId`, `csrf`, `state?`) | cookie do painel + `Origin` = Hub | `Origin` ausente/estrangeiro ou token CSRF ruim/vencido (10 min; amarrado a sessão, servidor, state e origem mostrada) → 403. Emite ticket `aud = "<serverId>#admin"` (TTL 300 s; com `state`, claim `nonce = base64url(sha256(state))`, que o painel confere com o state do cookie — sem nonce ou diferente → 401) e **303** para `<adminUrl>/auth/callback#ticket=…[&state=…]`. Qualquer usuário logado recebe: o servidor decide quem é admin (`admin.admins`). 10 tickets/min por usuário; 30 req/min por IP (IPv6 por /64); auditado no log (did, serverId, resultado), mesmo com `HUB_LOG_LEVEL` alto |
| GET/POST/PATCH/DELETE | `/v1/owner/servers[...]` | sessão | painel: criar, listar, editar descrição, girar chave, apagar |
| GET | `/v1/bans/global` | chave do servidor | `[{discordId, reason, createdAt}]`; só se o servidor tem `useGlobalBans` (senão 403 `not_opted_in`) |
| POST/DELETE | `/v1/admin/bans/global` | sessão de admin da plataforma | gerenciar lista global |
| PATCH | `/v1/admin/servers/:id` `{featured}` | sessão de admin da plataforma | liga/desliga o destaque (`{featured: boolean}`, nada mais); responde `{id, featured}`; 401 sem login, 403 não admin, 404 servidor inexistente; auditado no log (did, serverId, valor, resultado), mesmo com `HUB_LOG_LEVEL` alto; aparece na lista em até 5 s (cache) |
| GET | `/v1/launcher/update/{target}/{arch}/{current_version}` | — | updater do launcher (Tauri v2 *dynamic update server*): `current_version` não-semver → 400 `invalid_version`; `target` ≠ `windows` ou `arch` ≠ `x86_64`, sem release ou versão já igual/maior → **204**; senão **200** `UpdateInfo {version, notes, pub_date, url, signature}` com o maior release `launcher-vX.Y.Z` (semver) que tem `*-setup.exe` + `*-setup.exe.sig`; `url` = `{HUB_PUBLIC_URL}/v1/dl/launcher-vX.Y.Z/<asset>`, `signature` = **conteúdo** do `.sig` (minisign; quem confere é o launcher) |
| GET | `/v1/mod/latest?protocol=N` | — | `ModInfo {version, protocol, url, sha256, size, signature}` do maior `mod-pN-vX.Y.Z` com `cbmp-mod-pN-vX.Y.Z.zip` + `.zip.sig` + `.zip.sha256` (sha256 lido do asset); nenhum → 404 `no_mod`; `protocol` inválido → 400 `invalid_query` |
| GET | `/v1/dl/{releaseTag}/{asset}` | — | repassa em stream o asset do GitHub (`Content-Type: application/octet-stream`, `Content-Length`); só tags `launcher-vX.Y.Z`/`mod-pN-vX.Y.Z` publicadas e assets listados nelas, senão 404 `not_found`; falha no GitHub → 502 `github_error` |

Regras:
- Chave do servidor: 32 bytes aleatórios, mostrada uma única vez; Hub guarda só `sha256`.
- Heartbeat: corpo exatamente `HeartbeatBody` de `@cbmp/contracts`: `{ name, description, players, maxPlayers, gamePort, httpPort, version, protocol, tags[], locale }` (sem `serverId`: o servidor é identificado pela chave; `name` é aparado). O IP vem da conexão (não do corpo) e pode ser IPv6 (o launcher usa `[host]:porta`). Antes de listar a 1ª vez, quando IP/porta HTTP mudam e a cada 10 min, o Hub chama `http://IP:httpPort/info` e confere `serverId`; probe falho espera 120 s antes de nova tentativa no mesmo endpoint. Sem heartbeat por 90 s → sai da lista.
- Probe: IPs privados/reservados (v4 e v6) são recusados (`reason: private_ip`, sem probe) salvo `HUB_ALLOW_PRIVATE_SERVERS=1`. Atrás de proxy, `HUB_TRUST_PROXY` é a **lista** de IPs/CIDRs dos proxies confiáveis (nunca booleano); vazio = IP direto da conexão.
- Autenticação da API: `Authorization: Bearer at_…` em todas as rotas de sessão. O cookie `cbmp_session` (HttpOnly, SameSite=Lax) só vale no painel (`/panel`, com checagem de Origin/Sec-Fetch-Site), em `GET /v1/me` e em `POST /v1/auth/logout`. Formulários (`application/x-www-form-urlencoded`) só são aceitos no painel.
- OAuth: `state` de uso único (10 min) amarrado ao navegador pelo cookie `cbmp_oauth`; callback com `state` diferente do cookie → `bad_state`. Launcher é cliente público: PKCE S256 obrigatório, `codeVerifier` com 43–128 chars `[A-Za-z0-9._~-]`.
- Rate limit por IP e por chave/sessão. Erros com código estável (`{error: "codigo", message}`).
- Releases (update/mod/dl): `HUB_GITHUB_TOKEN` (só leitura do repo privado; nunca logado, nunca repassado) + `HUB_RELEASES_REPO` (`dono/repo`). Sem token → as três rotas respondem `503 updates_disabled`. Lista de releases em cache por 60 s (inclui o texto dos `.sig`/`.sha256` ≤ 8 KiB); drafts e prereleases ignorados; release sem os assets exigidos é pulado. Falha ao atualizar (inclusive ao ler um `.sig`/`.sha256`) é logada sem o token, guardada por 15 s e serve a lista anterior se houver (nunca uma lista "sem a versão nova"). Download segue o redirect do GitHub sem o token (URL assinada reaproveitada por 60 s por asset), prazo de 15 s até os cabeçalhos e 30 s de ociosidade no corpo (sem prazo total); downloads simultâneos: no máximo 2 por IP (IPv6 por /64; acima → `429 too_many_downloads`) e `HUB_DL_MAX_CONCURRENT` no total (padrão 32; acima → `503 busy`); cliente que não drena o socket por 30 s é derrubado e a vaga volta; teto global de 80 chamadas/min à API do GitHub, das quais downloads usam no máximo 60% (acima → `503 busy`); `.sig`/`.sha256` que falha 3 vezes seguidas tira só o seu release da lista (com aviso). 60 req/min por IP (IPv6 por /64) nas três rotas. O Hub **não** confere assinaturas: o launcher confere com a chave pública embutida.
- Banco interno do Hub: SQLite (não exposto). MySQL é o banco dos servidores de jogo.
- Chave privada de assinatura só na VPS, com `kid` para rotação.

### Resource

```
meu-resource/
  resource.json
  server/*.lua        ← nunca sai do servidor
  client/*.lua        ← baixado
  ui/                 ← baixado (HTML/CSS/JS de UI + assets)
  files/              ← baixado (assets)
```

`resource.json`:
```json
{
  "name": "meu-resource",
  "version": "1.0.0",
  "server": ["server/main.lua"],
  "client": ["client/main.lua"],
  "ui": "ui/index.html",
  "files": ["files/**"],
  "dependencies": ["outro-resource"],
  "exports": ["funcaoPublica"],
  "permissions": { "http": ["api.exemplo.com"], "db": true }
}
```

Tipos de arquivo baixáveis (lista branca): `.lua`, `.html`, `.css`, `.js` (só dentro de `ui/`), `.json`, imagens (`.png .jpg .webp .svg`), fontes (`.woff .woff2 .ttf`), áudio (`.ogg .mp3 .wav`), vídeo (`.webm .mp4`). **Nunca** código nativo (`.dll .exe .so` etc.): o servidor recusa subir resource que os contenha.

### Manifest e arquivos (HTTP do `cb-server`)

- `GET /info` → `{ serverId, name, players, maxPlayers, protocol, version }` (usado pelo Hub e pelo ping do launcher).
- `GET /cb/manifest` → `{ serverId, protocol, resources: [{ name, version, files: [{ path, sha256, size }] }], totalSize }`.
- `GET /cb/files/<sha256>` → conteúdo (imutável, `Cache-Control: immutable`).
- Launcher guarda cache global endereçado por conteúdo; rejeita arquivo cujo hash não bate.

## `cb-server` (C++, Linux x64 e Windows x64)

### Threads

1. **Rede**: GameNetworkingSockets (BSD). Só recebe/envia; entrega mensagens por filas.
2. **Simulação**: tick 20 Hz. Aplica inputs, valida, atualiza grade espacial, monta snapshots por cliente.
3. **Scripts**: pool de threads Lua (um `lua_State` por resource; um resource executa em uma thread por vez). Comunicação com a simulação só por filas; scripts leem cópias de estado.
4. **HTTP**: `/info`, manifest/arquivos, `/metrics`; cliente HTTP para heartbeat e chaves do Hub.
5. **Admin**: painel web em porta própria.
6. **Banco**: pool de conexões MySQL/MariaDB (consultas assíncronas).

Portas padrão: jogo `30500/udp`, HTTP `30500/tcp`, admin `30510/tcp` (configuráveis).

### Escala (meta 512)

- Grade espacial de células de 64 m; raio de streaming padrão 300 m com histerese (sai em 330 m).
- Teto de 128 entidades por cliente (as mais próximas).
- Prioridade por distância: 20 Hz perto, 10 Hz médio, 5 Hz longe.
- Snapshot binário: posições quantizadas, delta contra o último snapshot confirmado; eventos em canal confiável.
- Protocolo em biblioteca C++ compartilhada `cbmp-proto` (versão inteira `protocol`), usada por servidor, bots e mod.

### Segurança

- **S1 Conexão/DDoS**: handshake com desafio do GNS; nada é alocado por cliente antes do ticket válido; limite de conexões por IP; teto de banda por cliente; ban automático de IP abusivo. Guia de hospedagem recomenda host com proteção de rede; Hub atrás de Cloudflare.
- **S2 Autoridade**: cliente envia input/posição; servidor valida velocidade máxima por estado (andar, correr, vassoura…), corrige posição impossível e registra flag. Combate/magias: cliente só **pede** (`cast {spellId, target, aim}`); servidor checa cooldown, mana, alcance, linha de visão aproximada e aplica dano. Checagens genéricas no núcleo; regras por servidor em Lua.
- **S3 Eventos**: token bucket por cliente e por tipo de evento; excesso grave → kick.
- **S4 Auditoria/bans**: log estruturado JSONL (dano, dinheiro, itens, admin, anti-cheat) com Discord ID, rotação diária; ban por Discord ID por servidor; lista global opcional do Hub.
- **S5 PC do jogador**: só tipos da lista branca; tudo verificado por hash; Lua de cliente e UI rodam isolados no jogo (sem arquivos/processos, só API da plataforma).
- **S6 Integridade do cliente**: launcher verifica próprios arquivos e mod; cliente informa hash do build; servidor recusa builds desconhecidos (lista de builds aceitos publicada com cada release). Anti-cheat básico no cliente (subprojeto 5): DLLs estranhas, alteração do código do mod em memória, speedhack por deriva de relógio — só **reporta**; servidor decide.

### Scripts Lua 5.4

- Biblioteca de binding: sol2 (MIT).
- Sandbox por resource: sem `io`, `os.execute`, `os.exit`, `loadlib`, `debug` (exceto traceback); `require` só dentro do resource; leitura de arquivos só na pasta do resource; HTTP só para domínios em `permissions.http`; banco só se `permissions.db`.
- Comunicação entre resources só por `exports` declarados.
- Assíncrono por corrotinas: `local rows = db.query("SELECT ... WHERE id = ?", id)` suspende só aquele fluxo.
- API de servidor (núcleo inicial): jogadores (listar, obter, kick, ban, posição, identificadores), eventos de rede (com limite), comandos/chat, timers, log de auditoria, banco, HTTP, exports, ciclo de vida (start/stop/restart a quente).
- API de cliente: apenas definida (tipos/stubs e documentação) até o subprojeto 5.
- Vigia: callback > 50 ms → aviso com nome do resource; travado > 5 s → interrompe (hook de instrução) e reinicia o resource.

### Banco MySQL/MariaDB

- Pool em C++ (MariaDB Connector/C, LGPL, link dinâmico), configurado em `server.cfg`.
- Só consultas parametrizadas; transações; timeouts; métricas de latência.

### Painel/console admin

- Web embutido no `cb-server` (porta admin). Login com Discord via ticket do Hub (`aud` = serverId, uso admin); admins por Discord ID em `server.cfg`.
- Funções: jogadores (kick/ban), logs de auditoria e anti-cheat, resources (start/stop/restart), comandos de console, métricas (tick, banda, CPU, memória, banco).

### Bots de carga (`cb-bots`, C++)

- Simula N jogadores com tickets dev: andam, mudam de estado, enviam eventos e casts.
- Mede RTT, taxa de snapshots, banda, erros.

## Launcher (Tauri 2: backend Rust + UI web em TS)

- Login Discord via navegador do sistema + deep link `cbmp://`; tokens guardados no cofre do sistema (Windows Credential Manager via plugin keyring do Tauri).
- Lista: filtros (nome, tags, idioma, ocupação), favoritos, ping (`/info`).
- Conectar: ticket → manifest → diff com cache → download paralelo com progresso → verificação → localizar jogo (Steam/Epic, lógica equivalente ao `GameLocator` do launcher atual) → instalar/atualizar mod → abrir jogo. Endereço + ticket passados por arquivo temporário com ACL do usuário, nunca por linha de comando.
- Auto-update do launcher e do mod via GitHub Releases (token de leitura em testes enquanto o repo for privado).
- Segurança Tauri: capabilities mínimas (sem shell/fs livres para a UI), CSP estrita, download, hash, cache e abertura do jogo feitos no backend Rust; nunca executa conteúdo baixado.

## Estrutura do repositório

```
client/              mod do jogo (spike existente; vira o cliente no subprojeto 5)
proto/               cbmp-proto (C++, compartilhado)
server/              cb-server (C++)
bots/                cb-bots (C++)
packages/contracts/  @cbmp/contracts (TS): tipos da API, ticket sign/verify, vetores de teste
hub/                 Hub (TS)
launcher/            Launcher (Tauri 2: Rust + UI TS)
tools/  package/  docs/
```

C++: CMake + vcpkg (GameNetworkingSockets, libsodium, Lua 5.4, sol2, cpp-httplib, nlohmann-json, spdlog, MariaDB Connector/C). TS: npm workspaces, Node 22 LTS, testes com Vitest.

## Subprojetos e critérios de pronto

| # | Subprojeto | Pronto quando |
|---|---|---|
| 1 | Contratos + Hub | login Discord no painel; criar servidor e girar chave; heartbeat de teste aparece em `/v1/servers` e some após 90 s; ticket emitido valida com `@cbmp/contracts`; vetores de teste gerados; testes automáticos verdes |
| 2 | Núcleo `cb-server` + `cbmp-proto` + `cb-bots` | servidor sobe em modo dev e com Hub; aparece no Hub via heartbeat; valida tickets (C++ passa nos vetores TS); serve `/info`, manifest e arquivos; **512 bots em 4 vCPUs: tick estável a 20 Hz, p99 < 25 ms, banda média < 64 kbps/jogador com 100 por perto, RAM < 1 GB**; teleporte e flood bloqueados; sem ticket ninguém entra |
| 3 | Lua + MySQL + admin | resource de exemplo em Lua responde a evento dos bots; sandbox impede `io`/`os.execute`/arquivos fora/HTTP não declarado; consulta MySQL assíncrona funciona; vigia reinicia resource travado; painel admin lista jogadores, faz kick e reinicia resource |
| 4 | Launcher | login, lista, clique → ticket → download com cache e verificação → "pronto para abrir"; auto-update via Releases; abrir o jogo implementado (testado quando houver jogo) |
| 5 | Cliente no jogo | conecta com ticket; jogadores se veem e se movem; Lua de cliente e UI isoladas; entra direto no mundo sem save do jogador; anti-cheat básico reporta |
| 6 | Migração CB | resources do CB reescritos em Lua sobre a API da plataforma (licença de cada `hmp-*` conferida antes) |

Cada subprojeto tem spec detalhada (quando necessário) e plano próprios. Subprojetos 1–4 não precisam do jogo.

## Riscos

- **Maior risco técnico está no subprojeto 5** (dentro do jogo): sincronizar animações/magias e pular o menu podem ser difíceis. Mitigação: testar o spike assim que houver PC com jogo, em paralelo aos subprojetos 1–4.
- UI HTML dentro do jogo exige renderizador web embutido (pesado). Decisão de tecnologia no subprojeto 5.
- 512 jogadores dependem do teste de carga; se não atingir, ajustar raio/teto/taxas antes do subprojeto 5.
- Repo privado impede auto-update público; decidir publicação antes de abrir a jogadores.
- Termos de uso da WB e marca: não usar "Hogwarts" no nome do produto. Não é parecer jurídico.
- Resources `hmp-*` podem ter origem nos samples da MafiaHub (licença restritiva): conferir antes de migrar.

## Pendências de decisão (não bloqueiam o subprojeto 1)

- Nome/domínio públicos da plataforma e do Hub (dev usa `localhost`).
- Onde hospedar o Hub (VPS atual do CB ou nova).
- Voz (proximidade) — se entra e quando.

## Relação com o castelo-mp

`castelobruxo/castelo-mp` (reconstrução do ArcaneMP sobre o MafiaHub Framework) é um projeto **separado**, por decisão do Robson (2026-11-09). Esta plataforma não lê, copia nem se baseia no código ou nas análises dele, para continuar independente da MafiaHub e do ArcaneMP.
