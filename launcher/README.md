# CB Multiplayer — launcher

Launcher Windows (x64) da plataforma: Tauri 2 (Rust) + UI Svelte 5 + Vite + TypeScript. Spec:
`docs/superpowers/specs/2026-11-10-sub4-launcher-design.md`.

```
launcher/
  ui/          Svelte 5 + TS — só chama comandos Tauri (src/lib/api.ts); sem fs/shell/http/opener
  src-tauri/   backend Rust, tauri.conf.json, capabilities/, keys/update.pub
```

## Comandos

Na raiz do repositório (a UI é o workspace npm `launcher/ui`):

```bash
npm ci
npm test -w launcher/ui                 # Vitest (jsdom)
npm run typecheck -w launcher/ui        # svelte-check
npm run build -w launcher/ui            # gera launcher/ui/dist (exigido antes de compilar o Rust)
(cd launcher/src-tauri && cargo test)   # inclui o guarda de capabilities/CSP (tests/config_guard.rs)
npm run tauri -w launcher/ui -- dev     # janela de desenvolvimento
npm run tauri -w launcher/ui -- build   # instalador NSIS em launcher/src-tauri/target/release/bundle/nsis/
```

Requisitos no Windows: Rust stable (MSVC), Visual Studio com C++ e Windows SDK, Node 24, WebView2 (já vem no
Windows 11).

## Builds: oficial e "Teste local"

Há dois perfis de build, com a **mesma** CSP, capabilities, plugins e chave pública (`tests/config_guard.rs` confere
os dois):

| | Oficial | Teste local (`--features local-test`) |
|---|---|---|
| Nome / identificador | CB Multiplayer / `br.com.castelobruxo.cbmp` | CB Multiplayer (Teste local) / `br.com.castelobruxo.cbmp.localtest` |
| Executável | `CBMultiplayer.exe` | `CBMultiplayerTesteLocal.exe` |
| Hub | `settings::DEFAULT_HUB_URL` (fixo) | `http://127.0.0.1:8787` (fixo) |
| Servidores | só endereços públicos | aceita loopback e rede privada |
| Updater | endpoint do Hub em `tauri.conf.json` | Hub local (`tauri.localtest.conf.json`, http liberado) |
| Dados | `%LOCALAPPDATA%\CBMultiplayer` | `%LOCALAPPDATA%\CBMultiplayer-TesteLocal` |
| Cofre (Credential Manager) | serviço `CBMultiplayer` | serviço `CBMultiplayer (Teste local)` |

No Git Bash, na raiz do repositório (os mesmos comandos do CI):

```bash
# oficial: instalador em launcher/src-tauri/target/release/bundle/nsis/
npm run tauri -w launcher/ui -- build --ci --config '{"bundle":{"createUpdaterArtifacts":false}}' -- --locked
# teste local: só para os testes manuais com tools/dev-env.mjs; nunca publicado
npm run tauri -w launcher/ui -- build --ci --features local-test --config ../src-tauri/tauri.localtest.conf.json \
  --config '{"bundle":{"createUpdaterArtifacts":false}}' -- --locked
```

O PowerShell estraga as aspas do JSON ao repassar para o `npm`: use o Git Bash (ou deixe o `tools/dev-env.mjs`
gerar o build de teste). O CI (`.github/workflows/launcher.yml`) gera os dois instaladores como artefatos
(`cb-multiplayer-nsis` e `cb-multiplayer-teste-local-nsis`) e falha se o binário oficial contiver qualquer marca do
perfil de teste (`127.0.0.1:8787`, `Teste local`, `localtest`, `TesteLocal`).

Os dois builds registram o esquema `cbmp://`: com os dois instalados, o login cai no que foi instalado por último.
Mantenha só um instalado (ou reinstale o que vai usar). Isso fica em `docs/PENDENCIAS.md`.

**Pasta de dados diferente no Teste local**: `run\connect.json` e `mods\` ficam em
`%LOCALAPPDATA%\CBMultiplayer-TesteLocal` nesse build. O mod do jogo (subprojeto 5) não pode fixar o nome da pasta:
precisa receber do launcher onde está o `run\` ou conhecer os dois nomes (em `docs/PENDENCIAS.md`).

## Desenvolvimento com Hub local (`tools/dev-env.mjs`)

Sobe, só nesta máquina, um Hub de teste (`HUB_DEV_LOGIN=1`, `HUB_ALLOW_PRIVATE_SERVERS=1`, dados em
`%TEMP%\cbmp-dev-env`, releases locais em `HUB_RELEASES_DIR`) e um `cb-server` em modo Hub com o `hello-client`,
cadastrado como **"Castelo Bruxo (teste)"** em destaque:

```bash
node tools/dev-env.mjs --build-releases   # launcher "Teste local" 0.1.0 e 0.1.1 + mod de teste, assinados com a chave dev
node tools/dev-env.mjs --run              # Hub em http://127.0.0.1:8787 + cb-server na porta 30500; Ctrl+C encerra
node tools/dev-env.mjs --stop             # encerra o que um --run anterior deixou rodando
node tools/dev-env.mjs --help
```

- O `cb-server` vem de `build-native\windows-release\server\Release\cb-server.exe` (`server/README.md`; preset
  `windows-release` com `-DVCPKG_INSTALLED_DIR=C:/cbmp-vi`) ou de `--cb-server <exe>` / `CBMP_CB_SERVER`.
- A chave dev é lida de `%USERPROFILE%\.cbmp-devkeys\update.key` (ou `--key` / `CBMP_DEV_KEY`; senha em
  `CBMP_KEY_PASSWORD`). Ela é recusada se estiver dentro do repositório e nunca é copiada nem impressa.
- `--build-releases` reaproveita um release pronto só se o código que entra no launcher não mudou desde o build (o
  registro fica em `releases\launcher-vX.Y.Z\.source-stamp`); `--force` refaz tudo.
- Sem app Discord configurado, o "Entrar com Discord" abre um formulário do próprio Hub ("Entrar como dev", conta
  fictícia). Com `DISCORD_CLIENT_ID` e `DISCORD_CLIENT_SECRET` de um app de **teste** nas variáveis de ambiente do
  terminal (nunca em arquivo), o login vai ao Discord.
- O passo a passo completo dos testes manuais está em `docs/TESTES-MANUAIS-SUB4.md`.

O launcher em `tauri dev` (debug) também aceita o Hub local: `CBMP_HUB_URL=http://127.0.0.1:8787` (e
`CBMP_DATA_DIR` para outra pasta de dados).

## Testes

- `cargo test` (em `launcher/src-tauri`, também com `--features local-test`): unidade, guarda de configuração,
  Hub/`cb-server` falsos (axum) e os testes do assinador de release (`examples/release_sign.rs`, `test = true`).
- **Ponta a ponta** (`tests/e2e.rs`): `tools/dev-env.mjs --run`, em portas livres e pasta temporária, sobe o Hub real
  (node) e o `cb-server` real com o `hello-client`. O teste faz o login de dev pelo mesmo caminho do botão Entrar
  (PKCE + state → formulário do Hub → `cbmp://auth`), acha o servidor em destaque, roda o `play` em modo simular e
  confere que a pasta montada tem exatamente os arquivos do manifest (sha256), que eles são os do `hello-client` e o
  `connect.json`. Precisa de `node` no PATH, `npm ci` na raiz e do `cb-server` (padrão
  `build-native\windows-release\...`, ou `CBMP_CB_SERVER`). Sem o binário o teste é **pulado com aviso**; com
  `CBMP_E2E=1` a falta vira falha. No CI roda no job `e2e` de `launcher.yml`, que compila só o alvo `cb-server`.

  ```bash
  (cd launcher/src-tauri && cargo test --test e2e -- --nocapture)
  ```

## Dados

`%LOCALAPPDATA%\CBMultiplayer` (`settings.json`, `cache\`, `servers\`, `mods\`, `run\`, `logs\`); no build de
Teste local, `%LOCALAPPDATA%\CBMultiplayer-TesteLocal`. A raiz vem da pasta
conhecida `FOLDERID_LocalAppData` (`SHGetKnownFolderPath`), não da variável de ambiente. Em builds de debug
`CBMP_DATA_DIR` troca essa pasta (testes/dev); o release ignora a variável.

`settings.json` é gravado de forma atômica (`.tmp` + rename). Arquivo que não é JSON é renomeado para
`settings.json.bad` e o launcher volta aos padrões; campo inválido isolado volta sozinho ao padrão (original copiado
para `.bad`). Arquivo existente mas ilegível (permissão, em uso) não é sobrescrito: o launcher abre com os padrões e
recusa gravar.

A UI só altera favoritos, recentes, limites e modo simular (`set_settings` recebe `SettingsUpdate`, que recusa
campos a mais). `hubUrl` é fixo por build (o valor do arquivo é ignorado) e `gameDir` só muda pelo Rust (seletor de
pasta / localizador do jogo).

## Hub

O endereço padrão é `https://hub.cbmp.invalid` — **placeholder**, precisa ser trocado pelo endereço real do Hub
(`settings::DEFAULT_HUB_URL` e o endpoint do updater em `tauri.conf.json`) antes da primeira versão pública. Só
`https://` é aceito; em builds de debug também `http://` de loopback (`localhost`, `127.0.0.1`, `[::1]`) e a variável
`CBMP_HUB_URL` sobrepõe o padrão. O release ignora `CBMP_HUB_URL`.

## Auto-update

O updater do Tauri consulta `{hub}/v1/launcher/update/{{target}}/{{arch}}/{{current_version}}` e só aceita pacotes
assinados com a chave cuja parte pública está em `src-tauri/keys/update.pub` (embutida no binário e repetida em
`plugins.updater.pubkey` do `tauri.conf.json`; o teste `config_guard` exige que as duas sejam iguais).

`bundle.createUpdaterArtifacts` fica `false` no `tauri.conf.json`: o build comum (local e CI) não precisa da chave
privada. Só o workflow de release (e o `dev-env --build-releases`, com a chave dev) liga essa opção e assina.

A checagem roda ao abrir e a cada 6 h (`update://available` para a UI); nada é instalado sozinho. `check_update`
devolve `{available, version, notes}` e `install_update` baixa (3 tentativas com espera crescente: o Hub derruba quem
fica abaixo de 32 KiB/s e não serve `Range`), confere a assinatura e instala (o instalador fecha o launcher). Pacote
anunciado fora da origem do Hub é recusado. A versão também precisa estar assinada: `requireSignedVersion: true` no
`tauri.conf.json` (o comentário confiável do `.sig` traz `version:X.Y.Z`, gravado por `tauri build` ou
`tauri signer sign --app-version`) e o launcher confere de novo que a versão assinada é a anunciada. Assim um Hub
comprometido não consegue oferecer um launcher antigo assinado como se fosse novo.

## Chaves

Um único par Ed25519 (minisign, formato do Tauri) assina o launcher e o mod.

- **Pública**: `src-tauri/keys/update.pub` e `plugins.updater.pubkey` no `tauri.conf.json` (mesmo conteúdo). Fica no
  repositório.
- **Privada**: **nunca** no repositório, em issue, log ou artefato. Só fora do repo, na máquina de quem gerou (com
  backup seguro), e no secret `CBMP_UPDATE_KEY` do environment `release` (senha, se houver, em
  `CBMP_UPDATE_KEY_PASSWORD`; nunca como secret do repositório, veja "Chaves" abaixo).

> **A chave atual é de DESENVOLVIMENTO** (key id `BDCB478091E91629`; a privada está em
> `%USERPROFILE%\.cbmp-devkeys\update.key`, fora do repo). O `release.yml` recusa assinar enquanto ela estiver em
> `keys/update.pub`.

Gerar o par de produção e trocar (antes do primeiro release real):

1. Rode os testes manuais (`docs/TESTES-MANUAIS-SUB4.md`) **antes** da troca. O build "Teste local" embute
   `keys/update.pub`: depois da troca, os releases de teste assinados com a chave dev deixam de valer (update e mod de
   teste passam a ser recusados). Para testar de novo depois, assine os testes com um par de teste cuja pública esteja
   embutida, ou decida embutir a pública dev só no perfil `local-test`.
2. `npm run tauri -w launcher/ui -- signer generate -w <pasta fora do repo>\cbmp-update.key` (com senha forte).
3. Copie o conteúdo de `cbmp-update.key.pub` para `src-tauri/keys/update.pub` e para `plugins.updater.pubkey`.
4. No GitHub (Settings → Environments), crie o environment `release`: em "Deployment branches and tags" restrinja a
   `main` (só a branch `main`) e ligue "Required reviewers". Dentro dele (Environment secrets), crie
   `CBMP_UPDATE_KEY` com o conteúdo de `cbmp-update.key` e `CBMP_UPDATE_KEY_PASSWORD` com a senha. **Nunca** como
   secret do repositório (Settings → Secrets → Actions): esses ficam legíveis por qualquer workflow de qualquer
   branch. Se já existir um `CBMP_UPDATE_KEY` ou `CBMP_UPDATE_KEY_PASSWORD` como secret do repositório, **apague-o**
   (o do environment só o encobre no job de release; nas outras branches ele continuaria legível). O `release.yml` só
   roda no environment `release` e recusa qualquer ref que não seja `refs/heads/main`.
5. Commit da pública e `cargo test` (o `config_guard` confere as duas cópias). Launchers já instalados com a chave
   antiga não aceitam updates assinados com a nova: a troca tem de acontecer antes de distribuir qualquer instalador.

## Publicar um release

Workflow manual `.github/workflows/release.yml` (Actions → release → Run workflow, **sempre a partir da `main`**),
num repositório que o Hub lê (`HUB_RELEASES_REPO`, com `HUB_GITHUB_TOKEN` só no servidor do Hub). O job usa o
environment `release` (passo 4 de "Chaves"), então espera a aprovação de um revisor antes de ter acesso à chave.
`dry_run=true` faz tudo (testes, build, assinatura e conferência) sem criar a tag nem o Release: use-o com a chave
real para ensaiar o workflow antes do primeiro release. É seguro, porque nada é publicado e as assinaturas ficam só no
runner, que é descartado. Não troque a chave para o ensaio: a conferência usa o `keys/update.pub` da `main` e o
secret do environment, e trocar os dois e depois desfazer é fácil de errar.

- `kind=launcher`, `version=X.Y.Z`: roda os testes, faz o build oficial com `version` e `createUpdaterArtifacts`
  ligados, assinando com `CBMP_UPDATE_KEY`; confere a assinatura com `keys/update.pub` e `version:X.Y.Z` no comentário
  confiável (mesma função do launcher); cria o Release `launcher-vX.Y.Z` com `CBMultiplayer_X.Y.Z_x64-setup.exe` e o
  `.sig` (sem espaço no nome: o GitHub troca espaços em nomes de asset).
- `kind=mod`, `version=X.Y.Z`, `protocol=N`, `mod_run_id=<id da execução que gerou o artefato "cbmp-mod">`: baixa o
  zip, assina com o comentário confiável exato `cbmp-mod protocol=N version=X.Y.Z sha256=<hex>`, confere e cria o
  Release `mod-pN-vX.Y.Z` com `cbmp-mod-pN-vX.Y.Z.zip`, `.zip.sig` e `.zip.sha256`. O build que gera o artefato
  `cbmp-mod` é do subprojeto 5.
  - **Proveniência (falha fechado):** antes de baixar, o workflow consulta a execução `mod_run_id` na API e só segue
    se ela for deste repositório (código e fork incluídos), do workflow `.github/workflows/mod.yml`, disparada por
    `push` ou `workflow_dispatch` (nunca `pull_request`/`pull_request_target`) na `main` e concluída com sucesso.
    Além disso, o `head_sha` da execução tem de estar na branch `main` (igual à ponta ou ancestral dela, pela API
    `compare`), e o repositório não pode ter uma tag chamada `main`: `head_branch` também vale para tags, e só ele não
    prova nada. O `head_sha` vai para as notas do Release.
  - **Contrato do `mod.yml` (subprojeto 5):** o caminho tem de ser exatamente `.github/workflows/mod.yml` (se mudar,
    mude `MOD_WORKFLOW` no `release.yml` junto). Ele tem de fazer checkout exatamente de `GITHUB_SHA` (o padrão do
    `actions/checkout`, sem `ref:`), não pode ter entradas de ref, branch ou fonte, nem usar `pull_request_target`.
    O check confia em `head_sha`, então construir outro commit dentro de uma execução "confiável" anularia a proteção.

O workflow recusa: chave dev em `keys/update.pub`, secret ausente, versão fora de `X.Y.Z`, tag já existente,
assinatura que não confere. Assinar e conferir fica em `examples/release_sign.rs` (testado no `cargo test` com pares
gerados no teste); o `examples/dev_sign_mod.rs` (mod de teste do dev-env) usa o mesmo código (`examples/common/`).

## Instalação e SmartScreen

O instalador NSIS é por usuário (sem administrador). Ele **não** tem assinatura Authenticode (exige certificado; em
`docs/PENDENCIAS.md`), então o Windows SmartScreen avisa "O Windows protegeu o computador" na primeira execução:
"Mais informações" → "Executar assim mesmo". A integridade dos updates não depende disso (minisign, acima).

## Jogo, mod e Jogar

- **Pasta do jogo**: na abertura, se `gameDir` estiver vazio, procura na Steam (`SteamPath` do HKCU ou `InstallPath`
  do HKLM WOW6432Node → `steamapps\libraryfolders.vdf` → `appmanifest_990080.acf` → `steamapps\common\<installdir>`)
  e na Epic (`<FOLDERID_ProgramData>\Epic\EpicGamesLauncher\Data\Manifests\*.item`). `pick_game_dir` abre o seletor
  (plugin dialog, só pelo Rust). Toda pasta precisa ter `HogwartsLegacy.exe` e `Phoenix\Binaries\Win64` (senão
  `game_invalid`). Como qualquer usuário local cria arquivos em `%ProgramData%`, um resultado da Epic só vale se o
  `.item`, a pasta do jogo, o executável e `Phoenix\Binaries\Win64` tiverem dono SYSTEM, Administradores,
  TrustedInstaller ou o usuário atual e nenhuma outra conta com direito de escrita (dono e DACL lidos com
  `GetNamedSecurityInfoW`).
- **Mod**: `GET /v1/mod/latest?protocol=N` → zip baixado do próprio Hub (3 tentativas), tamanho e sha256 conferidos,
  assinatura minisign com a mesma chave do updater. O comentário confiável da assinatura tem de ser exatamente
  `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>` e bater com o protocolo pedido, a versão anunciada e o zip
  (senão `mod_signature_invalid`); a versão registrada e comparada é a assinada. O zip é validado inteiro antes de
  gravar (sem `..`, absolutos, `\`, `:`, links, criptografia; cada arquivo até 512 MiB declarados) e instalado de
  forma atômica em `<jogo>\Phoenix\Binaries\Win64\cbmp\`: extrai em `cbmp.staging`, relê os hashes e só então troca
  (`cbmp` → `cbmp.old` → apagado); qualquer falha antes da troca deixa o mod atual intacto, e sobras de uma queda são
  resolvidas na próxima vez. `mods\installed.json` guarda versão, protocolo e sha256 de cada arquivo; o zip assinado e
  o `.sig` ficam em `mods\package\` para reparo: arquivo alterado → reinstala do pacote local (assinatura conferida de
  novo) ou baixa de novo. Nunca faz downgrade: se o Hub oferecer versão menor que a instalada no mesmo protocolo, a
  instalada fica (`mod_altered` se ela estiver alterada e não houver pacote local para reparar).
- **Jogar** (`play(serverId)`, um por vez; outro pedido → `busy`; `cancel_play` interrompe, inclusive pedidos ao Hub
  em andamento): detalhe do servidor → ticket → manifest (validado) → download → montagem (+ limite do cache, sem
  desmontar o servidor atual) → mod (só fora do modo simular) → `run\connect.json` → abrir. Etapas em `play://stage`
  (`{serverId, stage}`, com `stage` em `ticket|manifest|download|mount|mod|connect|launch`). Ticket com mais de
  240 s é renovado antes de gravar a conexão. `clear_cache` usa a mesma trava, mas o launcher não acompanha o jogo
  depois de abri-lo: limpar o cache com o jogo aberto pode apagar arquivos em uso (a UI deve avisar).
- **`run\connect.json`** (lido e apagado pelo mod; o launcher apaga após 60 s se ainda existir; sobra antiga sai na
  abertura): `{host, port, ticket, serverId, contentDir, protocol, contentReadOnly: true}`. A pasta `run\` e o arquivo
  têm DACL protegida só do usuário atual. **`contentDir` é somente leitura para o mod**: os arquivos de
  `servers\<id>` são hard links do cache verificado (compartilhados com outros servidores); escrever neles
  corromperia o cache.
- **Abrir**: o executável do jogo é aberto direto, sem argumentos e sem variáveis novas (o ticket nunca vai para
  linha de comando nem ambiente). No **modo simular** (padrão enquanto não houver mod) o jogo não é aberto e o `play`
  devolve `{kind: "simulated", preview}` com o conteúdo da conexão sem o ticket; o `connect.json` (com o ticket) é
  gravado mesmo assim e apagado em 60 s, para testar um mod/cliente à mão.

## Rede: conteúdo e ping sem proxy

Os clientes HTTP do conteúdo dos servidores (`content.rs`) e do ping (`ping.rs`) usam `.no_proxy()`: ignoram o proxy
do sistema (variáveis de ambiente e configuração do Windows). Com proxy, quem resolveria o nome do servidor seria o
proxy, e a regra de endereços (servidor de jogo só em IP público no build oficial, conferida no mesmo resolvedor que
faz a conexão) deixaria de valer. O tráfego do jogo (UDP) é direto de qualquer forma, então uma rede que só sai por
proxy não joga mesmo assim; o custo é que nela também não há ping nem download de conteúdo. O cliente do Hub (login,
lista, updates, mod) continua respeitando o proxy.

## Logs

`logs\launcher.log` na pasta de dados (rotação em 2 MiB, 5 arquivos antigos). Nada de tokens, tickets, códigos OAuth,
verifier ou chaves: tudo passa por `logging::redact`, e `tests/logging.rs` falha se um `log::*!` do código usar uma
variável com esses nomes. É o arquivo a mandar quando algo der errado.

## Login

- `login` gera PKCE (verifier de 64 chars, S256) e `state` (32 bytes), guarda o pendente por 10 min e abre o
  navegador do sistema (ShellExecuteW no Rust; só `https://`, ou `http://` de loopback em debug) em
  `{hub}/v1/auth/discord/login?client=launcher&code_challenge=…&code_challenge_method=S256&state=…`.
- O Hub devolve `cbmp://auth?code=…&state=…` (ou `error=…&state=…`). A 2ª instância repassa o argv à 1ª
  (single-instance com o recurso `deep-link`), que entrega a URL ao `on_open_url`. Só `cbmp://auth` com o `state`
  pendente é aceito (uso único); o resto é ignorado e registrado só com o código do erro. Em debug, `register_all`
  registra o esquema para o executável atual.
- Refresh token só no Credential Manager (`keyring`, serviço `CBMultiplayer`, conta `refresh_token`); access token só
  em memória, renovado quando faltam 60 s ou menos. Refresh recusado (401) limpa o cofre → `session_expired`.
- A UI recebe `auth://changed` (`{loggedIn, error}`) e chama `me`; o avatar vem como `data:` URL baixada pelo Rust só
  de `https://cdn.discordapp.com/` (a CSP não permite imagem remota).
- Teste manual do cofre real: `cargo test --test auth -- --ignored cofre_real` (usa o serviço `CBMultiplayer-teste`).

## Segurança

- A UI só tem `core:event:allow-listen`/`allow-unlisten` e os comandos do app (`capabilities/default.json`); cada
  comando novo precisa entrar em `build.rs` e na capability. `tests/config_guard.rs` lê `capabilities/**/*` (JSON e
  TOML; outro formato falha), `tauri.conf.json`, `src/lib.rs` e `Cargo.toml`, e falha com: permissão `fs:`/`shell:`/
  `http:`/`opener:`, `core:default`, `core:image*`/`core:tray*`/`core:menu*`, emitir eventos da UI, alternar devtools;
  capability remota; `assetProtocol` ligado; CSP diferente da exigida ou com `unsafe-`; plugin fora de
  deep-link/single-instance/updater/dialog registrado no código ou como dependência direta.
- `npm run check:dist -w launcher/ui` confere a UI compilada (sem script/style inline, `style=`, `on*=` ou recurso
  remoto). O `tauri dev` no Windows carrega o Vite direto e não aplica a CSP; só o build instalado a aplica.
- CSP: `default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; connect-src ipc: http://ipc.localhost`.
- Sem fontes ou conteúdo remoto na UI; textos de servidores sempre como texto (nunca `{@html}`).
