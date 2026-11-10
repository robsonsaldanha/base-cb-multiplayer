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

## Dados

`%LOCALAPPDATA%\CBMultiplayer` (`settings.json`, `cache\`, `servers\`, `mods\`, `run\`, `logs\`). A raiz vem da pasta
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

## Auto-update e chave de assinatura

O updater do Tauri consulta `{hub}/v1/launcher/update/{{target}}/{{arch}}/{{current_version}}` e só aceita pacotes
assinados com a chave cuja parte pública está em `src-tauri/keys/update.pub` (embutida no binário e repetida em
`plugins.updater.pubkey` do `tauri.conf.json`; o teste `config_guard` exige que as duas sejam iguais).

> **A chave atual é de DESENVOLVIMENTO.** Foi gerada com `npx tauri signer generate` só para o esqueleto e deve ser
> substituída por uma chave nova antes do primeiro release real. A chave privada correspondente **nunca** entra no
> repositório: ela fica fora dele (na máquina de quem gerou) e, no CI, só no secret `CBMP_UPDATE_KEY` do workflow de
> release.

Para trocar a chave: `npx tauri signer generate -w <caminho fora do repo>`, copiar o `.pub` gerado para
`src-tauri/keys/update.pub`, atualizar `plugins.updater.pubkey` com o mesmo conteúdo e cadastrar a privada (e a senha)
no secret do workflow de release.

`bundle.createUpdaterArtifacts` fica `false` no `tauri.conf.json`: o build comum (local e CI) não precisa da chave
privada. Só o workflow de release liga essa opção e assina.

A checagem roda ao abrir e a cada 6 h (`update://available` para a UI); nada é instalado sozinho. `check_update`
devolve `{available, version, notes}` e `install_update` baixa (3 tentativas com espera crescente: o Hub derruba quem
fica abaixo de 32 KiB/s e não serve `Range`), confere a assinatura e instala (o instalador fecha o launcher). Pacote
anunciado fora da origem do Hub é recusado.

## Jogo, mod e Jogar

- **Pasta do jogo**: na abertura, se `gameDir` estiver vazio, procura na Steam (`SteamPath` do HKCU ou `InstallPath`
  do HKLM WOW6432Node → `steamapps\libraryfolders.vdf` → `appmanifest_990080.acf` → `steamapps\common\<installdir>`)
  e na Epic (`C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item`). `pick_game_dir` abre o seletor (plugin
  dialog, só pelo Rust). Toda pasta precisa ter `HogwartsLegacy.exe` e `Phoenix\Binaries\Win64` (senão
  `game_invalid`).
- **Mod**: `GET /v1/mod/latest?protocol=N` → zip baixado do próprio Hub (3 tentativas), tamanho e sha256 conferidos,
  assinatura minisign com a mesma chave do updater. O zip é validado inteiro antes de gravar (sem `..`, absolutos,
  `\`, `:`, links, criptografia; cada arquivo até 512 MiB declarados) e extraído só em
  `<jogo>\Phoenix\Binaries\Win64\cbmp\` (pasta do launcher, trocada inteira). `mods\installed.json` guarda versão,
  protocolo e sha256 de cada arquivo; arquivo alterado → reinstala. Nunca faz downgrade: se o Hub oferecer versão
  menor que a instalada no mesmo protocolo, a instalada fica.
- **Jogar** (`play(serverId)`, um por vez; outro pedido → `busy`; `cancel_play` interrompe): detalhe do servidor →
  ticket → manifest (validado) → download → montagem (+ limite do cache, sem desmontar o servidor atual) → mod (só
  fora do modo simular) → `run\connect.json` → abrir. Etapas em `play://stage` (`{serverId, stage}`, com `stage` em
  `ticket|manifest|download|mount|mod|connect|launch`). Ticket com mais de 240 s é renovado antes de gravar a conexão.
  `clear_cache` usa a mesma trava.
- **`run\connect.json`** (lido e apagado pelo mod; o launcher apaga após 60 s se ainda existir; sobra antiga sai na
  abertura): `{host, port, ticket, serverId, contentDir, protocol, contentReadOnly: true}`. Criado com DACL protegida
  só do usuário atual. **`contentDir` é somente leitura para o mod**: os arquivos de `servers\<id>` são hard links do
  cache verificado (compartilhados com outros servidores); escrever neles corromperia o cache.
- **Abrir**: o executável do jogo é aberto direto, sem argumentos e sem variáveis novas (o ticket nunca vai para
  linha de comando nem ambiente). No **modo simular** (padrão enquanto não houver mod) nada é aberto e o `play`
  devolve `{kind: "simulated", preview}` com o conteúdo da conexão sem o ticket.

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
