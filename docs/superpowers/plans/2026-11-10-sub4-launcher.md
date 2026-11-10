# Subprojeto 4 — Launcher "CB Multiplayer" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Launcher Windows (Tauri 2 + Svelte 5) com login Discord via Hub, lista com destaque, download verificado do conteúdo do servidor, entrega do mod e abertura do jogo (modo simular até existir mod), auto-update assinado servido pelo Hub.

**Architecture:** Backend Rust (`launcher/src-tauri`) concentra rede, disco, cofre e processo; a UI Svelte só chama comandos tipados. Hub ganha `featured`, rotas de update/mod/download que repassam os Releases do repo privado (token só no Hub) e o `nonce` do ticket de admin. `cb-server` ganha HTTP dual-stack, validação de nome no manifest público e um resource de exemplo com arquivos de cliente.

**Tech Stack:** Rust 1.99 (stable, MSVC), Tauri 2 (+ plugins deep-link, single-instance, updater, dialog), reqwest (rustls), tokio, sha2, keyring, minisign-verify, zip, serde; Svelte 5 + Vite + TypeScript + Vitest; Hub Node 24/TS/Fastify/zod; C++23 no `cb-server`.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-11-10-sub4-launcher-design.md` (fonte da verdade); `docs/superpowers/specs/2026-11-09-plataforma-design.md` (API do Hub); `docs/PENDENCIAS.md` seção "Antes do launcher".
- Branch `sub4-launcher`; commit por task (mensagem + linha em branco + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); push só com OK do controlador; nunca tocar `castelobruxo/*`; nada de código MafiaHub/arcane-mp/castelo-mp nem do launcher atual do CB.
- Nome **CB Multiplayer**; identificador `br.com.castelobruxo.cbmp`; dados em `%LOCALAPPDATA%\CBMultiplayer` (`cache\`, `servers\`, `mods\`, `run\`, `logs\`, `settings.json`, `servers-cache.json`); deep link `cbmp://`.
- Windows x64 apenas; instalador NSIS. Código Rust portável quando custar nada, mas só testado em Windows.
- UI sem acesso a fs/shell/http/opener; CSP `default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; connect-src ipc: http://ipc.localhost`; textos de servidores sempre como texto (nunca `{@html}`).
- Conteúdo baixado nunca é executado/aberto pelo launcher. Tokens só no Credential Manager (`keyring`, serviço `CBMultiplayer`); access token só em memória; logs sem tokens, tickets, códigos OAuth.
- Updates e mod: minisign/Ed25519 obrigatório, chave pública embutida (`launcher/src-tauri/keys/update.pub`); chave privada **nunca** no repo (secret `CBMP_UPDATE_KEY` só no workflow de release). Testes usam par gerado no próprio teste.
- Limites: manifest ≤ 10 000 arquivos e total ≤ `maxServerContentMB` (2048); nome de resource `[A-Za-z0-9._-]{1,64}`; caminhos relativos, sem `..`, `\`, absolutos, segmentos vazios; download 4 conexões, 3 tentativas; cache `cacheLimitMB` (10 240) com LRU; ping prazo 2 s, ≤ 16 simultâneos, a cada 30 s.
- Ticket pedido logo antes de abrir (TTL 300 s; renovado se o download passar disso). Arquivo de conexão `run\connect.json` com ACL só do usuário, apagado em 60 s; nunca ticket em linha de comando ou variável de ambiente.
- Hub: `HUB_GITHUB_TOKEN`, `HUB_RELEASES_REPO` (sem token → `503 updates_disabled`); tags `launcher-vX.Y.Z` e `mod-pN-vX.Y.Z`; `/v1/dl` só repassa assets listados nesses releases.
- Sem WSL/Docker/bancos locais nos agentes. Build C++ local com `-DVCPKG_INSTALLED_DIR=C:/cbmp-vi` (nunca apagar). Rust: `cargo test` em `launcher/src-tauri`; UI: `npm test` em `launcher/ui` (workspace npm).
- CI existente continua verde (native: linux/windows/fuzz/load; Hub). Novo job `launcher` (windows-2025).

## Estrutura de arquivos

```
packages/contracts/src/schemas.ts      + featured, UpdateInfo, ModInfo, nonce no ticket de admin
hub/src/routes/admin-servers.ts        PATCH featured
hub/src/routes/releases.ts             /v1/launcher/update, /v1/mod/latest, /v1/dl
hub/src/github.ts                      cliente GitHub Releases (injetável; fake nos testes)
server/src/main.cpp, http_server.*     HTTP dual-stack
server/src/manifest.cpp                regra de nome do resource
resources.example/hello-client/        resource com client/ ui/ files/
launcher/
  package.json                          workspace npm (ui)
  ui/  (Svelte 5 + Vite + TS)           src/App.svelte, lib/api.ts (invoke tipado), lib/stores.ts,
                                        routes/{Login,Servers,Favorites,Recent,Settings}.svelte,
                                        components/{Featured,ServerRow,Filters,DownloadBar,ErrorBanner}.svelte
  src-tauri/
    Cargo.toml tauri.conf.json capabilities/default.json keys/update.pub
    src/main.rs lib.rs commands.rs error.rs paths.rs settings.rs
        auth.rs hub.rs ping.rs content.rs game.rs modpkg.rs launch.rs update.rs
    tests/  (integração Rust)
.github/workflows/launcher.yml          CI do launcher
.github/workflows/release.yml           workflow_dispatch: assina e publica launcher/mod
```

---

### Task 1: `cb-server` — pendências do launcher

**Files:** Modify `server/src/main.cpp` (+ onde o `httplib::Server` faz bind), `server/src/manifest.cpp`, `server/tests/*`; Create `resources.example/hello-client/{resource.json,client/main.lua,ui/index.html,ui/app.js,ui/style.css,files/logo.png}`; Modify `server/README.md`, `docs/PENDENCIAS.md`.

- [ ] Teste: servidor HTTP responde `/info` em `127.0.0.1` **e** `[::1]` (pular o caso IPv6 com SKIP se o SO não tiver IPv6 loopback).
- [ ] Bind dual-stack: socket `::` com `IPV6_V6ONLY=0` (Windows e Linux); se falhar, bind em `0.0.0.0` e `::` separados; log diz o que ficou ativo.
- [ ] Teste: `ResourceStore` recusa resource com `name` fora de `[A-Za-z0-9._-]{1,64}` (erro na subida, igual `loadManifest`).
- [ ] Implementar a regra compartilhando a função de validação já usada pelo `loadManifest`.
- [ ] `hello-client`: manifest publica 5 arquivos (lua, html, js, css, png) com tamanhos > 0; teste de manifest confere paths, sha256 e `totalSize`.
- [ ] ctest verde; carga 64 bots `--assert` OK.
- [ ] Commit — `feat(server): HTTP dual-stack, nome de resource validado no manifest e exemplo com arquivos de cliente`

### Task 2: Hub — `featured` e `nonce` do ticket de admin

**Files:** Modify `packages/contracts/src/schemas.ts` (+ tests), `hub/src/store.ts` + migração, `hub/src/routes/servers.ts`, `hub/src/routes/admin-login.ts`; Create `hub/src/routes/admin-servers.ts`, `hub/test/featured.test.ts`; Modify `server/admin/src/admin_server.cpp` (+ tests), spec da plataforma (tabela da API), `docs/PENDENCIAS.md`.

- [ ] Testes (Vitest): `featured` default false; `ServerListing`/`ServerDetail` trazem `featured`; `GET /v1/servers` ordena destacados primeiro (desempate: jogadores desc, nome); `PATCH /v1/admin/servers/:id {featured:true}` exige sessão de admin da plataforma (403 caso contrário), 404 servidor inexistente, auditado.
- [ ] Implementar (coluna `featured INTEGER NOT NULL DEFAULT 0`, migração idempotente).
- [ ] Testes: ticket de admin emitido com `state` traz claim `nonce = base64url(sha256(state))`; sem `state` não traz `nonce`. Contrato atualizado.
- [ ] Painel (`admin_server.cpp`): `/auth/login` exige `nonce` igual a `base64url(sha256(state do cookie))`; ticket sem nonce ou com nonce diferente → 401. Teste C++.
- [ ] Hub: `npm test` + `npm run typecheck`; C++: ctest verde.
- [ ] Commit — `feat(hub): servidores em destaque e nonce no ticket de admin`

### Task 3: Hub — updates, mod e downloads via Releases

**Files:** Create `hub/src/github.ts`, `hub/src/routes/releases.ts`, `hub/test/releases.test.ts`; Modify `hub/src/app.ts`, `hub/src/config.ts`, `packages/contracts/src/schemas.ts` (`UpdateInfo`, `ModInfo`), spec da plataforma, `hub/README.md`.

**Interfaces — produz:**
- `GET /v1/launcher/update/{target}/{arch}/{current_version}` → `204` | `200 UpdateInfo {version, notes, pub_date, url, signature}` (`url` = `{hubPublicUrl}/v1/dl/launcher-vX.Y.Z/<asset>`; `signature` = conteúdo do `.sig`).
- `GET /v1/mod/latest?protocol=N` → `200 ModInfo {version, protocol, url, sha256, size, signature}` | `404 {error:"no_mod"}`.
- `GET /v1/dl/{tag}/{asset}` → stream do asset; `404` se tag/asset não listado.

- [ ] `GitHubReleases` interface `{ list(): Promise<Release[]>; download(assetId): Promise<ReadableStream & {size}> }`; implementação real com `fetch` + `HUB_GITHUB_TOKEN` (header `Authorization: Bearer`, `Accept: application/octet-stream` no download); cache da lista 60 s; fake para testes.
- [ ] Testes com fake: sem token → 503 `updates_disabled` nas três rotas; update: versão atual menor → 200 com a maior `launcher-v*` (semver), igual/maior → 204, `target` ≠ `windows` ou `arch` ≠ `x86_64` → 204, versão inválida → 400; release sem `.sig` é ignorado; mod: maior `mod-pN-v*` do protocolo pedido, `sha256` lido de asset `<zip>.sha256`, sem → 404; `/v1/dl`: tag fora dos padrões → 404, asset não listado → 404, stream com `Content-Length` e `Content-Type: application/octet-stream`; rate limit por IP (60/min) → 429.
- [ ] Implementar. Nunca logar o token. Draft/prerelease ignorados.
- [ ] `npm test` + typecheck verdes.
- [ ] Commit — `feat(hub): updates do launcher e do mod servidos a partir dos Releases`

### Task 4: Launcher — esqueleto Tauri + Svelte + CI

**Files:** Create `launcher/**` (estrutura acima, telas vazias), `.github/workflows/launcher.yml`; Modify `package.json` raiz (workspace `launcher/ui`), `.gitignore`, `README.md`.

**Interfaces — produz:**
- `paths.rs`: `fn data_dir() -> PathBuf` (override por `CBMP_DATA_DIR` só em testes/dev), `cache_dir()`, `servers_dir()`, `mods_dir()`, `run_dir()`, `logs_dir()`.
- `error.rs`: `#[derive(Serialize)] struct AppError { code: &'static str, message: String }` (mensagem pt-BR) + `type AppResult<T> = Result<T, AppError>`.
- `settings.rs`: `Settings { hub_url, favorites: Vec<String>, recent: Vec<String>, game_dir: Option<PathBuf>, cache_limit_mb: u64, max_server_content_mb: u64, simulate_launch: bool }` com load/save atômico (tmp + rename).
- `commands.rs`: `get_settings`, `set_settings` (validação), `app_version`.
- `ui/src/lib/api.ts`: wrappers tipados de `invoke` com os mesmos nomes.

- [ ] `cargo test`: settings ida e volta, arquivo corrompido → padrões + backup `.bad`, validação (hub_url https ou http loopback em dev).
- [ ] Vitest: App monta layout A (menu lateral, área do destaque, lista, barra de rodapé) com `invoke` simulado.
- [ ] `tauri.conf.json`: identificador, janela 1100×700 mínimo 900×600, CSP do Global Constraints, `bundle.targets = ["nsis"]`, plugins registrados (deep-link `cbmp`, single-instance, updater com endpoint `{hub}/v1/launcher/update/{{target}}/{{arch}}/{{current_version}}` e `pubkey` de `keys/update.pub`, dialog). `capabilities/default.json` só com `core:default` restrito e os comandos do app.
- [ ] Teste Rust que lê `tauri.conf.json` + capabilities e falha se aparecer `fs:`, `shell:`, `http:`, `opener:` ou CSP com `unsafe-`.
- [ ] `launcher.yml`: windows-2025, cache cargo + npm, `npm ci`, `npm test -w launcher/ui`, `cargo test` (src-tauri), `npm run tauri build` com `bundle.createUpdaterArtifacts=false` sobrescrito no CI comum (sem chave de updater; só o workflow de release assina), upload do instalador NSIS como artefato.
- [ ] Commit — `feat(launcher): esqueleto Tauri 2 + Svelte 5, configurações e CI`

### Task 5: Launcher — login, cofre e cliente do Hub

**Files:** Create `src-tauri/src/{auth.rs,hub.rs}`, `src-tauri/tests/auth.rs`; Modify `commands.rs`, `lib.rs`, `ui/src/routes/Login.svelte`, `ui/src/lib/stores.ts`.

**Interfaces — produz:**
- `hub.rs`: `struct HubClient { base: Url, http: reqwest::Client }` com `servers() -> Vec<ServerListing>`, `server(id) -> ServerDetail`, `ticket(server_id, access) -> String`, `me(access) -> Me`, `exchange(code, verifier)`, `refresh(rt)`, `logout(...)`; erros do Hub mapeados para `AppError` pelo `error` estável.
- `auth.rs`: `trait TokenVault { get/set/clear }` (real: `keyring`; teste: memória); `struct Auth` com `begin_login() -> Url` (gera verifier 64 chars + state 32 bytes, guarda pendente 10 min), `on_deep_link(url) -> AppResult<()>` (só `cbmp://auth`, `state` de uso único), `access_token() -> AppResult<String>` (refresh automático com margem de 60 s; falha → `session_expired`), `logout()`.
- Comandos: `login`, `logout`, `me`, eventos `auth://changed`.

- [ ] Testes: PKCE (`code_challenge` = base64url(sha256(verifier)), comprimentos e alfabeto); deep link com state errado/repetido/vencido/esquema diferente/host diferente → ignorado com código; `error=access_denied` → `login_denied`; troca e refresh contra Hub falso (axum ou servidor `httptest`) incluindo rotação de tokens; refresh 401 → limpa cofre; nada de token em logs (captura de log no teste).
- [ ] Integrar `tauri-plugin-deep-link` + `tauri-plugin-single-instance` (2ª instância → argumento com a URL → `on_deep_link` na 1ª); abrir navegador via `tauri-plugin-opener` **só no Rust** (não exposto à UI).
- [ ] UI: tela de login (botão "Entrar com Discord", estado aguardando navegador, erro), perfil no topo (nome/avatar do `/v1/me`, avatar só se URL `https://cdn.discordapp.com/`), logout.
- [ ] Commit — `feat(launcher): login Discord com PKCE, deep link e cofre do Windows`

### Task 6: Launcher — conteúdo do servidor (manifest, download, cache, montagem)

**Files:** Create `src-tauri/src/content.rs`, `src-tauri/tests/content.rs`; Modify `commands.rs`.

**Interfaces — produz:**
- `validate_manifest(m: &Manifest, expected_server: &str, expected_protocol: u32, limits: &Limits) -> AppResult<()>`.
- `struct ContentStore { root }` com `missing(&Manifest) -> Vec<FileRef>`, `download(base: Url, files, progress: impl Fn(Progress), cancel: CancellationToken) -> AppResult<()>`, `mount(server_id, &Manifest) -> AppResult<PathBuf>`, `enforce_limit(limit_bytes, keep: &HashSet<String>)`, `clear()`.
- `Progress { done_bytes, total_bytes, bytes_per_sec, current: String }` (emitido à UI como `content://progress`, no máx. 10/s).

- [ ] Testes contra servidor HTTP local (tokio): baixa e verifica; hash errado → `hash_mismatch` com nome do resource e `.part` removido; corpo maior que `size` → aborta; `Range` retomado após queda; 3 tentativas com 5xx; cancelamento limpa `.part`; manifest com `..`, `/abs`, `C:\`, `a//b`, `\`, nome de resource inválido, 10 001 arquivos, total acima do limite → recusado antes de qualquer download; `serverId`/`protocol` diferentes → `manifest_mismatch`; montagem por hard link (fallback cópia) e remoção de sobras; LRU respeita `keep`; arquivos do cache somente leitura após verificação.
- [ ] Implementar com `reqwest` streaming + `sha2` incremental; 4 downloads simultâneos (`Semaphore`); escrita em `cache\tmp\*.part` e `rename` atômico.
- [ ] Commit — `feat(launcher): download verificado do conteúdo do servidor com cache e montagem`

### Task 7: Launcher — jogo, mod, abertura e auto-update

**Files:** Create `src-tauri/src/{game.rs,modpkg.rs,launch.rs,update.rs}`, `src-tauri/tests/{game.rs,modpkg.rs,launch.rs}`; Modify `commands.rs`, `settings.rs`.

**Interfaces — produz:**
- `game.rs`: `trait Registry { fn read_string(hive, key, value) -> Option<String> }`; `locate(reg: &dyn Registry, roots) -> Option<GameInstall { dir, exe, store: Steam|Epic|Manual }>`; `validate_game_dir(dir) -> AppResult<GameInstall>` (existe `HogwartsLegacy.exe` e `Phoenix\Binaries\Win64\`).
- `modpkg.rs`: `verify_minisign(data, sig, pubkey) -> AppResult<()>`; `ensure_mod(hub, protocol, game: &GameInstall) -> AppResult<ModState>` (baixa via `/v1/mod/latest`, confere sha256 + assinatura, extrai zip seguro, grava `mods\installed.json`); `check_installed(game) -> AppResult<bool>` (hash de cada arquivo).
- `launch.rs`: `write_connect_file(info) -> AppResult<PathBuf>` (ACL só do usuário via `SetNamedSecurityInfoW`, remoção agendada 60 s); `launch(game, simulate) -> AppResult<LaunchOutcome { Started | Simulated(ConnectPreview) }>` (`ConnectPreview` sem o ticket).
- Comando `play(server_id)` orquestra: detalhe → ticket → manifest → content → mod (se não simular) → connect → launch, com eventos `play://stage`.
- `update.rs`: checagem ao iniciar e a cada 6 h via plugin updater; comando `install_update`.

- [ ] Testes: Steam (`libraryfolders.vdf` falso com 2 bibliotecas, `appmanifest_990080.acf`), Epic (manifests `.item` falsos), nenhum → `None`; pasta manual inválida → `game_invalid`; minisign válido/ alterado/ chave errada/ sem `.sig`; zip com `..`, absoluto, symlink, arquivo > 512 MiB declarado → recusado; reinstala quando um arquivo instalado muda; `connect.json` com DACL só do SID atual (ler de volta com `GetNamedSecurityInfoW`), apagado após 60 s (relógio injetável); simulate → `Simulated` sem ticket no preview; ticket renovado se passou de 240 s desde a emissão.
- [ ] Commit — `feat(launcher): localizar o jogo, mod assinado, arquivo de conexão e auto-update`

### Task 8: Launcher — interface (layout A) e ping

**Files:** Create `src-tauri/src/ping.rs` (+ testes), `ui/src/routes/*`, `ui/src/components/*`, `ui/src/lib/*.test.ts`; Modify `commands.rs`.

**Interfaces — consome:** comandos das Tasks 4–7; eventos `auth://changed`, `content://progress`, `play://stage`. **Produz:** `ping_servers(ids) -> Vec<{id, ms: Option<u32>}>` (prazo 2 s, ≤ 16 simultâneos).

- [ ] Ping: teste com servidores locais (rápido, lento > 2 s → `None`, recusado → `None`).
- [ ] UI (Vitest + @testing-library/svelte): destaque mostra todos `featured` no topo com botão Jogar; filtros (busca, tags, idioma, com vagas) combinados; favoritos/recentes persistem via `set_settings`; lista "desatualizada" quando o Hub falha (usa `servers-cache.json` via comando `cached_servers`); barra de download com progresso/velocidade/cancelar; etapas do Jogar; erros por código com texto pt-BR (`banned_global`, `protocol_mismatch`, `hash_mismatch`, `disk_full`, `game_not_found`, `session_expired`, `server_unavailable`); nomes/descrições de servidor nunca interpretados como HTML (teste com `<img onerror>`); Configurações (pasta do jogo com seletor, limite de cache, limpar cache, modo simular, versão + "procurar atualização").
- [ ] Visual: tema escuro do mockup aprovado (roxo/dourado), sem fontes remotas.
- [ ] Commit — `feat(launcher): interface com destaque, filtros, favoritos, ping e progresso`

### Task 9: Integração, release e docs

**Files:** Create `launcher/src-tauri/tests/e2e.rs`, `.github/workflows/release.yml`, `launcher/README.md`; Modify `.github/workflows/launcher.yml`, `docs/PENDENCIAS.md`, `README.md`.

- [ ] E2E Rust (sem janela, `#[ignore]` local se faltar binário; roda no CI): sobe Hub (`node hub` com `HUB_DEV_LOGIN=1 HUB_ALLOW_PRIVATE_SERVERS=1`, dados temporários) + `cb-server` (do build do job, modo Hub) com `hello-client` → login dev (rota de dev do Hub) → `play` em modo simular → conteúdo montado bate com o manifest → `Simulated`. No CI: job `launcher` baixa o artefato `cb-server` do job windows ou compila só o alvo necessário.
- [ ] `release.yml` (`workflow_dispatch` com entradas `kind=launcher|mod`, `version`, `protocol`): assina com `CBMP_UPDATE_KEY` (minisign) e cria Release com a tag do padrão; para mod, empacota um zip de teste e o `.sha256`.
- [ ] `launcher/README.md`: build, dev com Hub local, chaves (gerar par, onde fica a pública, nunca commitar a privada), publicar release, aviso do SmartScreen para testers.
- [ ] PENDENCIAS: Authenticode; Linux; mod real (sub5); remover itens resolvidos.
- [ ] Verificação manual (no report): instalador do CI, login com o app Discord de teste, servidor local, update entre duas versões de teste via release de teste.
- [ ] Commit — `feat(launcher): teste de ponta a ponta, release assinada e documentação`

## Fim do subprojeto 4

Critério (spec): instalador Windows do CI; login Discord por deep link; lista com Castelo Bruxo em destaque, filtros, favoritos, ping; Jogar até "Pronto para abrir" com conteúdo verificado; abrir o jogo implementado; auto-update pelo Hub entre duas versões de teste; pendências do sub4 resolvidas; CI verde com o job `launcher`.
