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

## Builds: oficial, "Teste local" e "Teste" (online)

Há três perfis de build, com a **mesma** CSP, capabilities, plugins e chave pública (`tests/config_guard.rs` confere
os três):

| | Oficial | Teste local (`--features local-test`) | Teste online (`--features online-test`) |
|---|---|---|---|
| Nome / identificador | CB Multiplayer / `br.com.castelobruxo.cbmp` | CB Multiplayer (Teste local) / `br.com.castelobruxo.cbmp.localtest` | CB Multiplayer (Teste) / `br.com.castelobruxo.cbmp.teste` |
| Executável | `CBMultiplayer.exe` | `CBMultiplayerTesteLocal.exe` | `CBMultiplayerTeste.exe` |
| Hub | `settings::DEFAULT_HUB_URL` (fixo) | `http://127.0.0.1:8787` (fixo) | `https://cbmultiplayer.cntoficial.com.br` (fixo) |
| Servidores | só endereços públicos | aceita loopback e rede privada | só endereços públicos (regras do oficial) |
| Updater | endpoint do Hub em `tauri.conf.json` | Hub local (`tauri.localtest.conf.json`, http liberado) | Hub de teste (`tauri.onlinetest.conf.json`, só https) |
| Dados | `%LOCALAPPDATA%\CBMultiplayer` | `%LOCALAPPDATA%\CBMultiplayer-TesteLocal` | `%LOCALAPPDATA%\CBMultiplayer-Teste` |
| Cofre (Credential Manager) | serviço `CBMultiplayer` | serviço `CBMultiplayer (Teste local)` | serviço `CBMultiplayer (Teste)` |

O perfil "Teste" é o do ambiente de teste online (`docs/AMBIENTE-TESTE-ONLINE.md`); `local-test` e `online-test` não
podem ser ligados juntos (`compile_error!`). Build: `node tools/teste-online.mjs --build-releases` (assina com a chave
dev) ou, sem assinar, `npm run tauri -w launcher/ui -- build --ci --features online-test --config
../src-tauri/tauri.onlinetest.conf.json --config '{"bundle":{"createUpdaterArtifacts":false}}' -- --locked`.

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
perfil de teste (`127.0.0.1:8787`, `Teste local`, `localtest`, `TesteLocal`, e do teste online:
`cbmultiplayer.cntoficial`, `CBMultiplayer-Teste`, `CBMultiplayer (Teste)`, `cbmp.teste`).

Os dois builds registram o esquema `cbmp://`: com os dois instalados, o login cai no que foi instalado por último.
Mantenha só um instalado (ou reinstale o que vai usar). Isso fica em `docs/PENDENCIAS.md`.

**Pasta de dados de cada build**: `run\connect.json` e `mods\` ficam na pasta de dados do build (tabela acima). O mod
não conhece esses nomes: o launcher grava o **ponteiro** `Win64\cbmp\cbmp-run.json` com o caminho do `run\` do build
que abriu o jogo (veja "Jogo, mod e Jogar").

## Desenvolvimento com Hub local (`tools/dev-env.mjs`)

Sobe, só nesta máquina, um Hub de teste (`HUB_DEV_LOGIN=1`, `HUB_ALLOW_PRIVATE_SERVERS=1`, dados em
`%TEMP%\cbmp-dev-env`, releases locais em `HUB_RELEASES_DIR`) e um `cb-server` em modo Hub com o `hello-client`,
cadastrado como **"Castelo Bruxo (teste)"** em destaque:

```bash
node tools/dev-env.mjs --build-releases   # launcher "Teste local" 0.1.0 e 0.1.1 + mod de teste, assinados com a chave dev
node tools/dev-env.mjs --build-releases --mod-zip dist/cbmp-mod.zip   # idem, com o mod real 0.2.0 (mod-p1-v0.2.0)
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
- **Kit para testar em outro PC** (sem Git/Rust/Visual Studio/chave): `node tools/make-test-kit.mjs` gera
  `dist\cbmp-kit-teste-sub4.zip` com o Hub (dependências já instaladas), `packages/contracts`, o `dev-env`, o
  `cb-server` com as DLLs e o runtime do VC++ ao lado, o `hello-client`, a chave **pública** e os releases de
  teste prontos. Recusa empacotar releases de outro código do launcher (carimbo) ou `cb-server` mais velho que o
  código, confere as assinaturas com `keys/update.pub` e varre o kit atrás de chave privada, token, `.env` e dados
  do Hub. Dentro do kit (há `kit.json` na raiz) o `dev-env` entra em **modo kit**: usa `releases\` e
  `bin\cb-server\` como estão, nunca compila nem procura a chave (`--build-releases`/`--key`/`--force` são
  recusados), guarda os dados em `<kit>\dados` e confere o sha256 de cada arquivo antes de subir. `tools/kit-smoke.mjs`
  (`CONFERIR.cmd` no kit) confere por HTTP o Hub, o destaque, o update 0.1.0 → 0.1.1 e o mod (assinaturas) e o
  manifest do `cb-server`. Roteiro: `docs/TESTES-EM-CASA-SUB4.md` (vai no kit como `LEIA-ME.md`).

O launcher em `tauri dev` (debug) também aceita o Hub local: `CBMP_HUB_URL=http://127.0.0.1:8787` (e
`CBMP_DATA_DIR` para outra pasta de dados).

## Testes

- `cargo test` (em `launcher/src-tauri`, também com `--features local-test`): unidade, guarda de configuração,
  Hub/`cb-server` falsos (axum) e os testes da assinatura de release (`examples/release_sign.rs`, `test = true`:
  o que o núcleo do assinador grava é conferido com as funções do próprio launcher).
- `cargo test` em `launcher/release-signer`: o assinador do workflow de release (crate próprio, veja "Publicar um
  release").
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
privada. O workflow de release também compila assim (sem a chave) e assina depois, num job separado (veja "Publicar
um release"); só o `dev-env --build-releases` (local, com a chave dev) liga essa opção e deixa o `tauri build` assinar.

A checagem roda ao abrir e a cada 6 h (`update://available` para a UI); nada é instalado sozinho. `check_update`
devolve `{available, version, notes}` e `install_update` baixa (3 tentativas com espera crescente: o Hub derruba quem
fica abaixo de 32 KiB/s e não serve `Range`), confere a assinatura e instala (o instalador fecha o launcher). Pacote
anunciado fora da origem do Hub é recusado. A versão também precisa estar assinada: `requireSignedVersion: true` no
`tauri.conf.json` (o comentário confiável do `.sig` traz `version:X.Y.Z`, gravado pelo `launcher/release-signer` no
release, ou pelo `tauri build` no `dev-env`) e o launcher confere de novo que a versão assinada é a anunciada. Assim um Hub
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

**A chave nunca fica perto de código de terceiros.** O build do instalador roda pacotes npm e os build scripts e
proc-macros de ~400 crates; qualquer um deles poderia ler o ambiente (ou deixar um processo vivo no runner até o fim
do job). Por isso o workflow tem cinco jobs, e só um vê a chave:

| job | secrets | o que roda |
|---|---|---|
| `build` | nenhum | testes (UI, launcher, assinador), `check-pubkey` e o build do instalador **sem assinatura** (`createUpdaterArtifacts=false`) |
| `signer` | nenhum | runner limpo; compila só `launcher/release-signer` (`minisign`/`base64`/`sha2`/`semver`, versões exatas e `Cargo.lock` próprio, `--locked`, sem cache) |
| `sign` | `CBMP_UPDATE_KEY` (environment `release`) | runner limpo, **sem checkout, npm, cargo nem ações de terceiros**: baixa o binário do assinador e o que vai ser assinado e roda só o assinador, com a chave apenas no ambiente desse passo |
| `verify` | nenhum | confere o que foi assinado com as funções do launcher (`examples/release_sign.rs verify-*`) |
| `publish` | nenhum (`contents: write`) | só `gh`: cria a tag e o Release (nada com `dry_run`) |

O assinador embute a mesma `keys/update.pub` do launcher, recusa a chave dev antes de ler a privada, confere a própria
assinatura com a pública antes de gravar e nunca sobrescreve arquivos. O núcleo (`examples/common/signing.rs`) não
pode usar a biblioteca do launcher nem outras dependências: o crate do assinador o inclui pelo caminho. A aprovação do
environment acontece antes do job `sign`, depois do build e dos testes.

- `kind=launcher`, `version=X.Y.Z`: roda os testes e faz o build oficial com `version`, sem chave; o job `sign` grava
  o `.sig` no formato do updater do Tauri (`timestamp:<unix>	file:CBMultiplayer_X.Y.Z_x64-setup.exe	version:X.Y.Z`
  no comentário confiável); o `verify` confere a assinatura com `keys/update.pub` e `version:X.Y.Z` (mesma função do
  launcher); o `publish` cria o Release `launcher-vX.Y.Z` com `CBMultiplayer_X.Y.Z_x64-setup.exe` e o `.sig` (sem
  espaço no nome: o GitHub troca espaços em nomes de asset).
- `kind=mod`, `version=X.Y.Z`, `protocol=N`, `mod_run_id=<id da execução que gerou o artefato "cbmp-mod">`: baixa o
  zip (no job `sign`), assina com o comentário confiável exato `cbmp-mod protocol=N version=X.Y.Z sha256=<hex>`, confere e cria o
  Release `mod-pN-vX.Y.Z` com `cbmp-mod-pN-vX.Y.Z.zip`, `.zip.sig` e `.zip.sha256`. O artefato `cbmp-mod` vem do
  `mod.yml` (veja "Publicar o mod" abaixo).
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
assinatura que não confere. Assinar fica em `launcher/release-signer` (núcleo em `src-tauri/examples/common/signing.rs`)
e conferir em `examples/release_sign.rs` (`examples/common/verify.rs`); os dois são testados no `cargo test` com
pares gerados no teste. O `examples/dev_sign_mod.rs` (mod de teste do dev-env) usa o mesmo código
(`examples/common/`).

## Publicar o mod

O mod (`cb-mp` + UE4SS) é um zip assinado, `cbmp-mod-p<protocolo>-v<versão>.zip`, no layout 2 (`cbmp-mod.json`,
`loader/dwmapi.dll`, `ue4ss/...`, `licenses/`, `LEIA-ME.txt`; detalhes e build em `client/README.md`). Versão atual:
mod `0.2.0`, protocolo `1`, tag `mod-p1-v0.2.0`.

- **`mod.yml` real** (`.github/workflows/mod.yml`; contrato em "Publicar um release"): roda em `push` na `main` (caminhos
  `client/**`, `proto/**`, `tools/package-mod.ps1`, `tools/check-mod-zip.mjs` e o próprio arquivo) e por
  `workflow_dispatch` sem entradas, em `windows-2025`. Compila o UE4SS (commit fixo) e o mod, monta o zip
  (`tools/package-mod.ps1`), confere (`tools/check-mod-zip.mjs`) e envia o artefato `cbmp-mod` com exatamente um `.zip`.
- **Segredo `UE4SS_SUBMODULES_TOKEN`**: o UE4SS tem um submódulo privado (`UEPseudo`, só para contas GitHub ligadas à
  Epic Games). O dono do repositório precisa criar esse segredo (token só de leitura). **Sem ele o job termina verde,
  sem artefato e com um aviso**, e o `release.yml` não tem o que assinar: o release oficial do mod **não sai**. O build
  real do `mod.yml` ainda **não foi verificado** num runner (só o caminho do Visual Studio 2026 local rodou; o
  `tools/build.ps1` escolhe o gerador do VS 2022 17.13+ no `windows-2025`, mas esse caminho nunca rodou).
- **Release oficial**: com uma execução verde do `mod.yml` na `main`, rode o `release.yml` com `kind=mod`, `version=0.2.0`,
  `protocol=1` e `mod_run_id=<id da execução>` (environment `release`, chave de produção).
- **Release de teste (chave dev)**: `node tools/teste-online.mjs --build-releases --mod-zip dist/cbmp-mod.zip` (o zip vem
  do artefato do `mod.yml` por `gh run download`, ou do build local: `tools\build.ps1 -Target mod` e
  `tools\package-mod.ps1`); confere o zip (`checkModZip`), assina com `examples/dev_sign_mod.rs --zip` e gera
  `mod-p1-v0.2.0`; depois `--deploy` e `--check` (`docs/AMBIENTE-TESTE-ONLINE.md`). O ambiente local
  (`tools/dev-env.mjs --build-releases --mod-zip <zip>`) faz o mesmo para o Hub local.

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
  `GetNamedSecurityInfoW`) para ser usado sozinho. O launcher da Epic costuma deixar esses caminhos graváveis por
  outras contas: se **uma única** pasta válida falha nessa regra, ela vira **sugestão** (`game::Detection::Candidate`;
  o log diz qual verificação falhou: manifest, pasta do jogo, executável ou Binaries). Duas ou mais pastas distintas,
  ou mais de 1000 `.item`, viram `Detection::Ambiguous`: nenhuma sugestão (um `.item` plantado não toma o lugar do
  verdadeiro) e a UI pede o seletor. Caminho com caractere de controle ou de direção/formatação do Unicode é
  descartado. `detect_game` devolve a sugestão sem gravá-la; `use_detected_game(dir)` (uma confirmação por vez)
  procura de novo, só aceita se a única candidata ainda é exatamente `dir` (instalação confiável não passa por aqui),
  pede confirmação num diálogo nativo (fora do alcance da UI; o texto não atribui a pasta a ninguém) e grava pelo
  mesmo caminho do seletor (`validate_game_dir` + `set_game_dir`). A UI nunca define `gameDir`.
- **Mod**: `GET /v1/mod/latest?protocol=N` → zip baixado do próprio Hub (3 tentativas), tamanho e sha256 conferidos,
  assinatura minisign com a mesma chave do updater. O comentário confiável da assinatura tem de ser exatamente
  `cbmp-mod protocol=<N> version=<X.Y.Z> sha256=<hex>` e bater com o protocolo pedido, a versão anunciada e o zip
  (senão `mod_signature_invalid`); a versão registrada e comparada é a assinada. O zip é validado inteiro antes de
  gravar (sem `..`, absolutos, `\`, `:`, links, criptografia; cada arquivo até 512 MiB declarados) e instalado de
  forma atômica em `<jogo>\Phoenix\Binaries\Win64\cbmp\atual\` (layout 2, veja abaixo): extrai em `atual.staging`, relê
  os hashes e só então troca (`atual` → `atual.old`, `atual.staging` → `atual`, `atual.old` apagado); qualquer falha
  antes da troca deixa o mod atual intacto, e sobras de uma queda são resolvidas na próxima vez.
  `mods\installed.json` guarda versão, protocolo, pasta do jogo, sha256 de cada arquivo, o carregador e os backups; o
  zip assinado e o `.sig` ficam em `mods\package\` para reparo: arquivo alterado → reinstala do pacote local
  (assinatura conferida de novo) ou baixa de novo. Nunca faz downgrade: se o Hub oferecer versão menor que a instalada
  no mesmo protocolo, a instalada fica (`mod_altered` se ela estiver alterada e não houver pacote local para
  reparar). O pacote precisa de `cbmp-mod.json` com `layout 2`, `version` e `protocol` iguais aos assinados e
  `minLauncher` ≤ versão do launcher (senão `mod_invalid` ou `mod_incompatible`: "Este mod precisa de uma versão mais
  nova do launcher. Atualize e tente de novo."). Zip antigo (layout 1, arquivos direto em `cbmp\`) migra: os arquivos
  listados no registro antigo saem antes da primeira instalação em `atual\`.
- **Layout na pasta do jogo** (`Win64` = `<jogo>\Phoenix\Binaries\Win64`):

  | Onde | O quê |
  |---|---|
  | `Win64\cbmp\atual\` | o zip inteiro (troca atômica; anterior em `atual.old` só durante a troca) |
  | `Win64\cbmp\cbmp-run.json` | ponteiro do launcher para o mod, regravado a cada Jogar |
  | `Win64\cbmp\logs\`, `crash\`, `cb-mp.json` | do mod e do jogador; o launcher nunca grava nem apaga ali (a remoção os mantém) |
  | `Win64\dwmapi.dll` | cópia de `atual\loader\dwmapi.dll`, só se ausente ou já nossa (sha256 atual ou do histórico do registro) |
  | `Win64\override.txt` | uma linha, `cbmp\atual\ue4ss` (aponta o carregador para a pasta do UE4SS dentro de `atual`) |
  | `Win64\cbmp-backup\` (+ `cbmp-backup.json`) | arquivos do jogador movidos com confirmação; o manifesto viaja com a pasta |

  A pasta `Win64\ue4ss` do próprio jogador (outro UE4SS) nunca é tocada.
- **Ativação conferida em todo Jogar** (depois de instalar): hash do `Win64\dwmapi.dll` e conteúdo do `override.txt`.
  **Conflito** (`mod_conflict`): `dwmapi.dll` ou `override.txt` que não são nossos (ou `xinput1_3.dll` junto de
  `UE4SS.dll`, um UE4SS antigo: sem saída automática, o erro volta e o jogador remove à mão). A UI explica e o Rust
  confirma num diálogo nativo ("Encontramos outro carregador de mods (UE4SS) na pasta do jogo… Seus arquivos não são
  apagados: ficam em Phoenix\Binaries\Win64\cbmp-backup e voltam quando você remover o mod…", botões "Usar o
  carregador do CB Multiplayer" e "Cancelar"). Aceito, os dois arquivos são **movidos** (rename no mesmo volume; nome
  repetido ganha `.1`, `.2`…), nunca apagados nem sobrescritos, com o registro gravado **antes** de mover.
- **Remover mod do jogo** (Configurações; jogo fechado, senão `game_running`): apaga `dwmapi.dll` e `override.txt`
  **se forem os nossos**, devolve o que está em `cbmp-backup\` (só para destino vazio), apaga `cbmp\atual*` e o
  ponteiro; mantém `cbmp\logs\`, `crash\`, `cb-mp.json`, o pacote em `mods\package\` e a versão mínima no registro (o
  anti-downgrade não zera). Configurações mostra o estado ("Não instalado", "Instalado X.Y.Z", "Conflito com outro
  carregador de mods") e os botões "Remover mod do jogo" e "Abrir pasta de logs do mod" (abre `Win64\cbmp\logs`;
  `mod_logs_missing` se ainda não existe).
- **Erros novos do mod** (código → mensagem): `mod_conflict` → "Já existe outro carregador de mods (UE4SS) na pasta do
  jogo."; `mod_incompatible` (acima); `mod_activation_failed` → "Não foi possível ativar o mod na pasta do jogo.";
  `game_running` → "O Hogwarts Legacy está aberto. Feche o jogo e tente de novo." (Jogar e Remover recusam com
  `HogwartsLegacy.exe` rodando); `mod_folder_protected` (abaixo); `mod_folder_unreachable` → "A pasta do jogo <pasta> não
  está acessível agora. O mod continua registrado lá e sai na próxima vez."; `mod_folder_present` (Esquecer pasta numa
  pasta que ainda existe); e o aviso não bloqueante `mod_old_folder` (evento `mod://warning`): "O mod do CB
  Multiplayer continua na pasta anterior do jogo: <pastas>…".
- **Sem elevação, nunca**: o launcher não pede administrador nem muda permissões de nenhuma pasta do jogo. Antes de
  baixar o mod ele confere se dá para gravar em `Phoenix\Binaries\Win64` (cria e apaga um arquivo de teste); sem
  permissão (ex.: Epic em `C:\Program Files`), instalar, ativar e remover param em `mod_folder_protected` ("…instale o
  jogo numa pasta em que o seu usuário possa gravar…"). O jogo aberto é conferido antes (`game_running`), para um
  `dwmapi.dll` em uso não parecer falta de permissão. A instalação elevada foi retirada do subprojeto 5a (motivo e
  direção futura em `docs/PENDENCIAS.md`, "Instalação em pasta protegida"); `config_guard` garante que não há caminho
  de elevação no código.
- **Junções**: a pasta escolhida pode passar por junções/links (biblioteca da Steam movida); o launcher a resolve uma
  vez para o caminho final e trabalha nele (um registro do mod gravado antes pelo caminho da junção passa ao caminho
  final, sem tirar e pôr o mod de novo). Dentro de `cbmp\` e `cbmp-backup\` links/junções são recusados (nada é
  gravado, movido ou devolvido através deles), como nas entradas do zip.
- **Backups do jogador e pastas anteriores**: arquivos de outro carregador (`dwmapi.dll`, `override.txt`) vão para
  `Win64\cbmp-backup\` com um manifesto `cbmp-backup.json` dentro da própria pasta do jogo (viaja com ela se a pasta for
  movida). Pasta anterior que não existe mais e tinha backups fica registrada; nas Configurações aparece "Esquecer
  pasta", com uma confirmação que lista os arquivos do jogador que estavam lá (o comando só aceita, texto igual, uma
  pasta do registro; outro caminho vindo da UI nunca chega ao disco). O aviso de pasta anterior
  (`mod://warning`) aparece no máximo uma vez por execução do launcher.
- **Jogar** (`play(serverId)`, um por vez; outro pedido → `busy`; `cancel_play` interrompe, inclusive pedidos ao Hub
  em andamento): detalhe do servidor → ticket → manifest (validado) → download → montagem (+ limite do cache, sem
  desmontar o servidor atual) → mod (só fora do modo simular) → `run\connect.json` → abrir. Etapas em `play://stage`
  (`{serverId, stage}`, com `stage` em `ticket|manifest|download|mount|mod|connect|launch`). Ticket com mais de
  30 s (`play::TICKET_RENEW_AFTER_SECS`) é renovado antes de gravar a conexão, para o ticket gravado ter pelo menos 270 s
  de validade (o arquivo dura `CONNECT_TTL` = 240 s). Antes do `connect.json`, o `host` do servidor é resolvido e
  conferido pela regra de endereços do build e gravado como IP literal. `game_running` vem antes de qualquer
  mudança no mod. `clear_cache` usa a mesma trava, mas o launcher não acompanha o jogo
  depois de abri-lo: limpar o cache com o jogo aberto pode apagar arquivos em uso (a UI deve avisar).
- **`run\connect.json`** (lido e apagado pelo mod; o launcher apaga após `CONNECT_TTL` = 240 s se ainda existir, e o
  ponteiro junto, se ainda for o dele; sobra antiga sai na abertura):
  `{host, port, ticket, serverId, contentDir, protocol, contentReadOnly: true, launchId}`. A pasta `run\` e o arquivo
  têm DACL protegida só do usuário atual. O prazo de 240 s cobre os dois processos que o jogo abre (cerca de 33 s de
  diferença; o mod só resgata o arquivo depois de até 45 s).
- **Ponteiro `Win64\cbmp\cbmp-run.json`** (sem segredo; gravado depois do `connect.json`, por arquivo temporário +
  rename, a cada Jogar): `{ "v": 1, "runDir": "<absoluto>", "launchId": "<32 hex>", "writtenAt": <Unix s>,
  "launcher": "0.2.0" }`, até 4 KiB. É o que ativa o mod: ele procura o arquivo subindo no máximo 5 pastas a partir do
  `main.dll`, e só age se o ponteiro tem no máximo 600 s (futuro: 120 s) e o `launchId` bate com o do `connect.json`.
  Sem ponteiro válido (jogo aberto pela Steam/Epic sem o launcher) o mod fica **inerte**. O `launchId` aleatório
  impede que um ponteiro velho case com o arquivo de outro Jogar. **`contentDir` é somente leitura para o mod**: os arquivos de
  `servers\<id>` são hard links do cache verificado (compartilhados com outros servidores); escrever neles
  corromperia o cache.
- **Abrir**: o executável do jogo é aberto direto, sem argumentos e sem variáveis novas (o ticket nunca vai para
  linha de comando nem ambiente). No **modo simular** (Configurações → Avançado; **desligado por padrão** desde o
  0.2.0, e o valor salvo por versões anteriores é desligado uma vez) o jogo não é aberto e o `play` devolve
  `{kind: "simulated", preview}` com o conteúdo da conexão sem o ticket; o mod **não** é instalado nem ativado, e o
  `connect.json` (com o ticket) é gravado mesmo assim e apagado em `CONNECT_TTL`, para testar um mod/cliente à mão.

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
variável com esses nomes. É o arquivo a mandar quando algo der errado. Os logs do **mod** são outros: ficam na pasta do
jogo, `Win64\cbmp\logs\cb-mp-<aaaammdd-hhmmss>-<pid>.log` (um por processo; ficam os 10 mais novos), com os `.dmp` de
falha em `Win64\cbmp\crash\` e o `UE4SS.log` dentro de `Win64\cbmp\atual\ue4ss\`; o botão "Abrir pasta de logs do
mod" abre a primeira. Roteiro de coleta: `docs/TESTE-NO-JOGO-5A.md`.

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
