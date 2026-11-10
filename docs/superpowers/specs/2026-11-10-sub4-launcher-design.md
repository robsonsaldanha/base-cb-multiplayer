# Subprojeto 4 — Launcher "CB Multiplayer" — Design

Data: 2026-11-10
Status: aprovado em conversa, aguardando revisão da spec
Base: `2026-11-09-plataforma-design.md` (seções "Launcher", "Hub", "Conteúdo do servidor", S1–S6) e `docs/PENDENCIAS.md`
(seção "Antes do launcher").

## Objetivo

Launcher para Windows onde o jogador entra com Discord, vê a lista de servidores (Castelo Bruxo em destaque), clica em
Jogar, recebe ticket, baixa e verifica o conteúdo do servidor, instala/confere o mod e abre o jogo já conectado.
Atualiza a si mesmo e ao mod pelo Hub, com assinatura.

Fora do escopo: o mod do cliente em si (subprojeto 5; aqui só a entrega), Linux/macOS, assinatura Authenticode,
voz, loja, notícias.

## Decisões

| Tema | Decisão |
|---|---|
| Nome | **CB Multiplayer** (neutro da plataforma); identificador `br.com.castelobruxo.cbmp`; dados em `%LOCALAPPDATA%\CBMultiplayer` |
| Destaque | campo `featured` no Hub (liga/desliga por admin da plataforma); Castelo Bruxo é o primeiro destacado; nada fixo no código |
| Plataforma | **Windows x64** apenas; instalador NSIS |
| Stack | **Tauri 2** (Rust) + UI **Svelte 5 + Vite + TypeScript**; testes `cargo test` + Vitest |
| Layout | menu lateral (Servidores, Favoritos, Recentes, Configurações) + destaque no topo + lista densa + barra de download fixa no rodapé |
| Auto-update | updater oficial do Tauri contra o **Hub**; Hub repassa os Releases do repo privado com token só no servidor; assinatura Ed25519 (minisign) obrigatória |
| Mod | artefato **separado**, pelo mesmo canal do Hub, por `protocol`, assinado com a mesma chave; instalado e conferido por hash a cada abertura |

## Arquitetura (`launcher/`)

```
launcher/
  ui/                 Svelte 5 + TS (só invoca comandos Tauri; sem fs/shell/http)
  src-tauri/src/
    auth.rs           PKCE, deep link cbmp://auth, troca/refresh, cofre (keyring → Credential Manager)
    hub.rs            cliente do Hub (lista, detalhe, ticket, me) — https; loopback só em build dev
    ping.rs           GET /info em paralelo, prazo 2 s, ≤ 16 simultâneos
    content.rs        manifest → diff → download paralelo verificado → cache → montagem por servidor
    game.rs           localizar Hogwarts Legacy (Steam: registro + libraryfolders.vdf; Epic: manifests; manual)
    modpkg.rs         mod por protocol: baixar, verificar assinatura, instalar, conferir hash
    launch.rs         arquivo de conexão + abrir o jogo (ou modo simular)
    update.rs         configuração do plugin updater (endpoint do Hub, chave pública embutida)
    commands.rs       superfície única exposta à UI (tipos serde, erros com código estável)
```

Plugins Tauri: `deep-link`, `single-instance` (2ª instância repassa o deep link à 1ª e sai), `updater`, `dialog`
(só o seletor de pasta do jogo, chamado pelo Rust). Sem `fs`, `shell`, `http` ou `opener` liberados à UI; links
externos passam por comando Rust com lista fixa (Discord, `https://` do servidor quando anunciado).

Código do `game.rs` escrito do zero (não copiar o launcher atual do CB nem nada de MafiaHub/arcane-mp/castelo-mp).

## Hub: mudanças

- `featured: boolean` em `server_status`/listagem (`ServerListing`, `ServerDetail`); lista ordena destacados primeiro.
  `PATCH /v1/admin/servers/:id {featured}` (sessão de admin da plataforma, auditado).
- Updates (Tauri dynamic update): `GET /v1/launcher/update/{target}/{arch}/{current_version}` →
  `204` sem update; `200 {version, notes, pub_date, url, signature}` quando há versão maior. Releases com tag
  `launcher-vX.Y.Z` contendo o instalador NSIS e o `.sig`.
- Mod: `GET /v1/mod/latest?protocol=N` → `{version, protocol, url, sha256, size, signature}` ou `404 no_mod`.
  Releases com tag `mod-pN-vX.Y.Z` contendo `cbmp-mod-pN-vX.Y.Z.zip` e o `.sig`.
- Downloads: `GET /v1/dl/{releaseTag}/{asset}` repassa (stream) o asset do GitHub. Só tags `launcher-v*`/`mod-p*-v*`
  e assets listados no release; nada de caminho livre. Token em `HUB_GITHUB_TOKEN`, repo em `HUB_RELEASES_REPO`;
  sem token → rotas respondem `503 updates_disabled`. Cache em memória da lista de releases (60 s). Rate limit por IP.
- A assinatura **não** depende do Hub: o launcher confere com a chave pública embutida; Hub comprometido não consegue
  publicar launcher/mod.
- Pendência do sub3: claim `nonce` (hash do `state`) no ticket de admin e conferência no painel.

## `cb-server`: pendências do sub4

- HTTP dual-stack (`::` com `IPV6_V6ONLY=0`, ou dois binds) — o launcher pode receber host IPv6 do Hub.
- `ResourceStore` (manifest público) com a mesma regra de nome do `loadManifest` (`[A-Za-z0-9._-]{1,64}`).
- `resources.example/hello-client` com arquivos em `client/`, `ui/` e `files/` para testes de download.

## Fluxos

### Login
1. UI → `login()`. Rust gera `codeVerifier` (64 chars) + `state` (32 bytes), guarda em memória, abre o navegador do
   sistema em `{hub}/v1/auth/discord/login?client=launcher&code_challenge=…&code_challenge_method=S256&state=…`.
2. Deep link `cbmp://auth?code=…&state=…` (ou `error=…`) chega pela instância única; `state` tem de bater e ser de
   uso único (10 min).
3. `POST /v1/auth/exchange` → tokens no cofre (`keyring`, serviço `CBMultiplayer`); access token em memória.
4. Refresh automático antes de expirar; refresh falho → tela de login. Logout chama `/v1/auth/logout` e limpa o cofre.

### Lista
`GET /v1/servers` (última resposta guardada em disco para modo offline, marcada "desatualizada"), destaque no topo
(todos os `featured`), filtros (busca por nome, tags, idioma, com vagas), favoritos e recentes em
`settings.json` local; ping via `/info` ao abrir a tela e a cada 30 s (só visíveis).

### Jogar
1. **Ticket**: `POST /v1/tickets {serverId}` logo antes de abrir o jogo (TTL 300 s; se o download passar disso, pede
   outro). Erros do Hub (`banned_global`, etc.) viram mensagens.
2. **Manifest**: `GET http(s)://host:httpPort/cb/manifest`; `serverId` e `protocol` têm de bater com o Hub. Recusa
   caminho absoluto, `..`, `\`, segmentos vazios, nomes de resource fora de `[A-Za-z0-9._-]{1,64}`, mais de 10 000
   arquivos ou total acima de `maxServerContentMB` (padrão 2048).
3. **Diff** com o cache (`cache\<sha256>`).
4. **Download**: `GET /cb/files/<sha256>`, 4 conexões, arquivo `.part` com hash calculado durante a escrita; tamanho
   acima do declarado aborta; hash diferente → apaga e erro com o resource; retomada por `Range` quando possível;
   3 tentativas por arquivo com espera crescente; progresso (bytes, velocidade, arquivo) e cancelamento.
5. **Montagem**: `servers\<serverId>\<resource>\<path>` com hard link (ou cópia) do cache; sobras de versões antigas
   removidas.
6. **Mod**: versão para o `protocol` do servidor; ausente/alterado → baixa, confere assinatura, extrai (zip sem
   caminhos fora da pasta, sem links) na pasta do jogo, grava lista de arquivos com hash em `mods\installed.json`.
   - A assinatura cobre também versão e protocolo: o comentário confiável do `.sig` (assinado com a chave privada) é
     exatamente `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>`; protocolo, versão e sha256 têm de bater com o
     zip e com o que está sendo instalado, senão `mod_signature_invalid`. A versão registrada e usada na regra "nunca
     downgrade" é a assinada, nunca a anunciada pelo Hub.
   - Instalação atômica: extrai em `cbmp.staging` (mesma pasta pai), confere o hash de cada arquivo e só então troca
     (`cbmp` → `cbmp.old`, `cbmp.staging` → `cbmp`, apaga `.old`); sobras de uma queda são resolvidas na próxima vez.
     O zip assinado fica em `mods\package\` para reparo local.
7. **Abrir**: grava `%LOCALAPPDATA%\CBMultiplayer\run\connect.json` `{host, port, ticket, serverId, contentDir,
   protocol}` com ACL só do usuário; o mod lê e apaga; o launcher apaga após 60 s se ainda existir. Abre o
   executável do jogo direto (Steam/Epic cuidam do DRM). Nunca passa ticket por linha de comando ou variável de
   ambiente.
   - **Modo simular** (padrão enquanto não houver mod com `protocol` compatível): não abre o jogo; mostra "Pronto para
     abrir" e o conteúdo do arquivo de conexão sem o ticket. O `connect.json` (com o ticket, ACL só do usuário) é
     gravado mesmo assim e apagado em 60 s.

### Cache
Limite `cacheLimitMB` (padrão 10 240); acima dele, remove os arquivos menos usados que não estejam montados para um
servidor favorito ou recente. Botão "Limpar cache" em Configurações.

## Erros (código estável + mensagem pt-BR)

| Situação | Comportamento |
|---|---|
| Hub fora | lista em cache "desatualizada"; novas tentativas com espera crescente |
| Servidor sem `/info`/manifest | "Servidor indisponível"; a lista continua |
| Sessão expirada | volta ao login |
| `banned_global` / protocolo incompatível | mensagem e Jogar desativado |
| hash/tamanho/caminho inválido | aborta a entrada no servidor, nomeia o resource |
| disco cheio / permissão | mensagem com a pasta |
| jogo não encontrado | seletor de pasta; valida que é o Hogwarts Legacy (executável + estrutura esperada) |
| update/mod com assinatura inválida | recusa e registra; launcher atual continua |

Logs do launcher em `%LOCALAPPDATA%\CBMultiplayer\logs` (rotação), sem tokens, tickets ou códigos OAuth.

## Segurança

- Capabilities mínimas; CSP `default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self';
  connect-src ipc: http://ipc.localhost` (sem `unsafe-inline`/`eval`, sem conteúdo remoto na UI). Textos de
  servidores sempre como texto.
- Conteúdo baixado nunca é executado nem aberto pelo launcher.
- Deep link aceita só `cbmp://auth` com parâmetros válidos; qualquer outro é ignorado e registrado.
- Updates e mod: assinatura minisign/Ed25519 obrigatória (chave pública embutida; privada só em secret do CI de
  release, nunca no repo). Hub só repassa. A versão também é assinada (comentário confiável): mod
  `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>`; launcher no formato do Tauri (`...\tversion:X.Y.Z`, de
  `tauri signer sign --app-version`/`tauri build`) com `requireSignedVersion: true`, e o launcher confere de novo.
  Um Hub comprometido não consegue servir uma versão antiga assinada como se fosse nova.
- Pasta do jogo achada pela Epic (`%ProgramData%`, onde qualquer usuário local cria arquivos) só vale se o `.item`,
  a pasta do jogo, o executável e `Phoenix\Binaries\Win64` tiverem dono SYSTEM/Administradores/TrustedInstaller/o
  usuário atual e nenhuma outra conta com escrita.
- Tokens só no Credential Manager; access token só em memória.
- Authenticode fica em `PENDENCIAS` (exige certificado; sem ele há aviso do SmartScreen).

## Testes

- **Rust**: PKCE/state; deep link (válidos/ inválidos/ repetidos); `content` contra servidor HTTP local (hash errado,
  tamanho maior, caminhos maliciosos, retomada, cancelamento, limite e LRU do cache, montagem); `game` com árvores
  falsas de Steam/Epic e registro simulado (trait); `modpkg` e verificação de update com artefatos assinados,
  alterados e sem assinatura; zip malicioso (`..`, absolutos, links); `connect.json` (ACL, remoção).
- **UI (Vitest)**: lista, filtros, favoritos/recentes, destaque, estados de download, erros — com `invoke` simulado.
- **Hub (Vitest)**: `featured` (lista/ordem/PATCH auditado), update (204/200, comparação de versão, tags inválidas),
  mod por protocol, `/v1/dl` restrito e em stream, sem token → 503; GitHub falso.
- **Integração**: teste Rust sem janela: Hub local + `cb-server` com `hello-client` → login dev → ticket → manifest
  → download → montagem → "pronto" (modo simular).
- **CI**: job `launcher` (windows): `cargo test`, `npm test` da UI, `tauri build` sem assinatura (artefato do
  instalador); job de release (manual, `workflow_dispatch`) assina updater/mod com secret e cria o Release.
- **Manual**: instalador do CI, login com o app Discord de teste, servidor local, update entre duas versões de teste.

## Critério de pronto

Instalador Windows gerado no CI; login Discord por deep link; lista com Castelo Bruxo em destaque, filtros, favoritos
e ping; Jogar baixa e verifica o conteúdo até "Pronto para abrir" (modo simular); abrir o jogo implementado (teste
real no sub5); auto-update pelo Hub funcionando entre duas versões de teste; pendências do sub4 no `cb-server` e no
Hub resolvidas; CI verde com o job `launcher`.

## Riscos

- Steam DRM pode relançar o executável: por isso o arquivo de conexão tem caminho fixo e não depende de argumentos.
- Hard links exigem o cache e `servers\` no mesmo volume (sempre `%LOCALAPPDATA%`); cópia como fallback.
- Sem Authenticode, antivírus/SmartScreen podem alertar; documentar para testers.
- Repo privado: o Hub precisa do token; se o token vazar, só dá leitura (fine-grained, só `contents:read` do repo).
