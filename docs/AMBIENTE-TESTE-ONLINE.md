# Ambiente de teste online (operação)

Ambiente para testadores **sem ferramentas de desenvolvimento**: eles só instalam o launcher "CB Multiplayer (Teste)",
entram com o Discord de verdade, veem o servidor de teste e vão até o **Jogar** (modo simular) com downloads reais
pela internet. Guia do testador: [`TESTE-ONLINE.md`](TESTE-ONLINE.md).

| Peça | Onde |
|---|---|
| Hub | `https://cbmultiplayer.cntoficial.com.br` (Cloudflare → nginx da VPS → `127.0.0.1:8787`) |
| Servidor de jogo | `cb-server` em `143.95.209.35:30500` (UDP do jogo + TCP/HTTP de `/info`, manifest e arquivos), conteúdo `hello-client`, sem banco |
| Launcher | build `--features online-test` + `launcher/src-tauri/tauri.onlinetest.conf.json` ("CB Multiplayer (Teste)", identificador `br.com.castelobruxo.cbmp.teste`, pasta `%LOCALAPPDATA%\CBMultiplayer-Teste`) |
| Releases | pasta local do Hub (`HUB_STAGING=1` + `HUB_RELEASES_DIR`): launcher 0.1.0/0.1.1 e mod `mod-p1-v0.1.0`, assinados **nesta máquina** com a chave dev |
| Login | app Discord do dono (o secret só existe na VPS, num arquivo que o dono cria) |

O nome do Hub passa pelo proxy da Cloudflare (HTTPS). **UDP não passa pelo proxy da Cloudflare**, por isso o servidor
de jogo é anunciado pelo IP (o Hub lista o IP de onde vem o heartbeat). Se um dia quiser um nome para o jogo, crie um
registro **DNS only** (nuvem cinza) apontando para o IP; nada aqui depende disso.

## Por que o launcher de teste aceita o servidor em `http://IP:30500`

As regras de rede do launcher de teste online são as **mesmas do oficial** (`profile::RELAXED_NETWORK` não muda): Hub
só em `https://`, servidores só em endereços públicos. O conteúdo do `cb-server` já é HTTP puro no oficial: é
endereçado por hash e conferido no launcher (`content.rs`), e `143.95.209.35` é público. Nada foi afrouxado.

## Layout na VPS

```
/var/www/html_cbmultiplayer/            dono: usuário do deploy (código só leitura para os serviços)
  hub/              main.mjs + cli.mjs (bundle esbuild do hub/), package.json + package-lock.json, node_modules/
  hub.env           configuração do Hub (sem segredos; fonte: tools/teste-online/hub.env)
  hub-data/         cbmp-hub 700: hub.sqlite, keys/ (chaves dos tickets), test-server.json ({serverId, key}, 600)
  releases/         <tag>/<asset> servidos por /v1/launcher/update, /v1/mod/latest e /v1/dl
  server/
    bin/            cb-server + libmariadb.so* + plugins/ (pacote do workflow server-linux-dist); bin.old = anterior
    server.cfg      cbmp-server 600: gerado no deploy (contém a chave do servidor no Hub)
    data/           cbmp-server 700: bans.json
    logs/           cbmp-server 700: server.log, audit-*.jsonl, anticheat-*.jsonl
    resources/      hello-client
  download/         CB-Multiplayer-Teste-setup.exe (+ .sha256): https://cbmultiplayer.cntoficial.com.br/CB-Multiplayer-Teste-setup.exe
  acme/             webroot do certbot (renovação)
  deploy/           cópia dos arquivos de tools/teste-online/, COMMIT (repositório) e SERVER (commit + sha256 do cb-server)
/etc/cbmp-teste/discord.env   app Discord (criado pelo DONO; root:root 600) — opcional para o Hub subir
```

Fora da pasta, só isto pertence ao ambiente:

- usuários de sistema `cbmp-hub` e `cbmp-server` (`useradd --system`, sem shell, sem home, sem sudo), um por serviço:
  o `cb-server` não lê `hub-data` (chaves do Hub) e nenhum dos dois lê `/etc/cbmp-teste`;
- `/etc/systemd/system/cbmp-hub-teste.service` e `cbmp-server-teste.service`: `Restart=on-failure`, `MemoryMax`
  300M/400M, `UMask=0077` e sandbox (`TemporaryFileSystem=/var/www:ro` com `BindReadOnlyPaths` só do próprio código: os
  outros projetos de `/var/www` nem aparecem; `InaccessiblePaths=/etc/cbmp-teste`; `ProtectSystem=strict`,
  `ProtectHome`, `PrivateTmp`, `PrivateDevices`, `PrivateUsers`, `ProtectProc=invisible`, `ProtectKernel*`,
  `ProtectControlGroups`, `RestrictAddressFamilies`, `RestrictNamespaces`, `LockPersonality`,
  `CapabilityBoundingSet=` vazio, `SystemCallFilter=@system-service` menos privilegiados; `MemoryDenyWriteExecute` só
  no `cb-server`, porque o JIT do Node precisa de páginas W+X). Nota do `systemd-analyze security`: veja o relatório;
- `/etc/nginx/sites-available/cbmultiplayer.cntoficial.com.br` + link em `sites-enabled/`;
- certificado Let's Encrypt `cbmultiplayer.cntoficial.com.br` (`certbot --webroot`, renovação pelo `certbot.timer` da
  VPS com `--deploy-hook "systemctl reload nginx"`);
- regras do ufw `30500/udp` e `30500/tcp` (comentário `CB Multiplayer teste`).

Portas: Hub `127.0.0.1:8787` (só local, atrás do nginx); `cb-server` `0.0.0.0:30500` UDP e `[::]:30500` TCP; painel admin
do `cb-server` desligado (sem `admin.admins`).

## IP do visitante (Cloudflare + nginx + Hub)

O nginx inclui `snippets/cloudflare-real-ip.conf` (já existente na VPS): `$remote_addr` vira o `CF-Connecting-IP`, mas
só quando a conexão vem de um IP da Cloudflare. O site manda `X-Forwarded-For: $remote_addr` (substitui, não
acrescenta) e o Hub confia só em `127.0.0.1` (`HUB_TRUST_PROXY=127.0.0.1`). Resultado: limites por IP do Hub
(120/min, 2 downloads por IP, heartbeats) valem por visitante, e um `X-Forwarded-For` forjado pelo cliente é
descartado pelo nginx. O heartbeat do `cb-server` também sai pela Cloudflare (`hub.url` https), então o Hub vê o IP
público da VPS (`143.95.209.35`), faz o probe em `http://143.95.209.35:30500/info` e anuncia esse endereço.

## Hub em modo staging (`HUB_STAGING=1`)

Modo novo do Hub para um Hub de teste **público** (`hub/src/config.ts`):

- permite `HUB_RELEASES_DIR` sem dev-login (antes só com `HUB_DEV_LOGIN=1`, preso à máquina local);
- sobe sem `DISCORD_CLIENT_ID`/`DISCORD_CLIENT_SECRET`: `/v1/auth/discord/login` responde `503 login_unavailable`
  sem client id; com client id e sem secret, redireciona ao Discord e o callback responde `503 login_unavailable`
  antes de trocar o código; o resto do Hub funciona;
- recusado junto com `HUB_DEV_LOGIN=1`, com `NODE_ENV=production` e com `HUB_PUBLIC_URL` que não seja https;
- `HUB_RELEASES_DIR` continua exclusivo com `HUB_GITHUB_TOKEN`. O dev-login fica **desligado** (`/v1/auth/dev-login` = 404).

Releases locais em vez de GitHub Releases: o Hub só lê Releases do GitHub com `HUB_GITHUB_TOKEN` (mesmo para repo
público) e publicar Releases é decisão do dono. A pasta local dispensa token e publicação, e a segurança não muda: o
launcher confere a assinatura (chave pública embutida) de todo update e mod; o Hub nunca vê a chave privada.

## Servidor de teste no Hub

`node cli.mjs ensure-server` (fonte `hub/src/cli.ts`), rodado pelo deploy na VPS, cadastra **"CB Multiplayer – Servidor
de teste"** em destaque no nome de um usuário de sistema (Discord ID `0`, que o Discord nunca emite), sem precisar de
login. É idempotente: reaproveita o servidor e a chave de `server/data/hub-server.json`; se a chave sumir, gera outra
(a antiga deixa de valer). A chave nunca é impressa; vai só para `server/server.cfg` (600). Como o dono do servidor é
o usuário de sistema, ele não aparece no painel de ninguém; mudanças passam pelo deploy.

## cb-server Linux

Workflow `.github/workflows/server-linux-dist.yml` (push na `main`/`teste-online` que mexe no nativo, ou manual):
imagem `manylinux_2_28` (AlmaLinux 8, glibc 2.28) com `gcc-toolset-14` (o código usa `std::print`, GCC 14), Release
sem sanitizers, `-static-libstdc++ -static-libgcc` e `-lanl`, `libmariadb.so` (LGPL, dinâmica) ao lado com RPATH
`$ORIGIN`. O job **falha** se o binário ou a libmariadb exigirem glibc acima de 2.35 (a da VPS, Ubuntu 22.04) ou se a
libstdc++ ficar dinâmica (hoje a maior exigida é `GLIBC_2.27`). Artefato `cb-server-linux-x64`
(`cb-server-linux-x64.tar.gz`). Imagens com glibc mais nova não servem: a libstdc++ estática do GCC 14 do Debian 12
usa `arc4random` (`GLIBC_2.36`).

O `cb-server` usa o OpenSSL estático do vcpkg; a unit aponta `SSL_CERT_FILE`/`SSL_CERT_DIR` para as CAs do sistema
(TLS até o Hub).

## Deploy e atualização

Tudo pela máquina de desenvolvimento (Windows, Git Bash ou PowerShell), com chave SSH já autorizada na VPS:

```bash
export CBMP_TESTE_SSH='<usuario>@143.95.209.35'
export CBMP_TESTE_SSH_PORT='<porta-ssh>'              # padrão 22
# 1) releases assinadas (local; a chave dev nunca sai desta máquina) + dist/CB-Multiplayer-Teste-setup.exe
node tools/teste-online.mjs --build-releases
# 2) cb-server Linux do CI (último run verde do workflow server-linux-dist). O resumo do run mostra o commit e o
#    sha256 do pacote; os dois são conferidos aqui e de novo na VPS antes de instalar.
gh run download <run-id> -n cb-server-linux-x64 -D "$TEMP/cbmp-srv"
# 3) envia e instala (Hub, hello-client, releases, cb-server, units, nginx)
node tools/teste-online.mjs --deploy --server-tar "$TEMP/cbmp-srv/cb-server-linux-x64.tar.gz" \
  --server-sha256 <sha256 do resumo> --server-commit <commit do resumo>
# primeira vez apenas: certificado e firewall
node tools/teste-online.mjs --cert
node tools/teste-online.mjs --firewall
# validação pela internet
node tools/teste-online.mjs --check
```

- Só o Hub mudou: `--deploy --no-releases` (sem `--server-tar` o `cb-server` instalado continua).
- Nova versão do launcher para os testadores: suba a versão em `LAUNCHER_VERSIONS` de `tools/teste-online.mjs`
  (ex. `0.1.2`), `--build-releases`, `--deploy`. O launcher instalado oferece a atualização sozinho.
- O deploy roda `tools/teste-online/remote-install.sh install` na VPS:
  - cria os usuários de serviço (se faltarem) e acerta donos e modos;
  - `npm ci --omit=dev --ignore-scripts` do Hub com um `package-lock.json` gerado do lock do repositório (mesmas
    versões e hashes; nenhum script de pacote roda; o binário nativo do better-sqlite3 já vem no pacote);
  - confere sha256 e commit do pacote do `cb-server` antes de extrair (nada do pacote roda como o usuário do deploy) e
    troca o binário (o anterior fica em `bin.old`; se o novo não sobe, volta sozinho para o anterior);
  - instala as units, reinicia os serviços, cadastra o servidor (`ensure-server`, rodado como `cbmp-hub`), gera o
    `server.cfg` (só do `cbmp-server`);
  - aplica o site nginx: recusa se outro site usa o nome; faz backup, `nginx -t` e, se falhar, restaura o arquivo e o
    link anteriores (ou remove na primeira instalação) e não recarrega. Teste: `tools/test/teste-online-nginx.test.mjs`
    (precisa de links simbólicos; no Windows é pulado, roda no Linux).

## Logs e estado

```bash
ssh -p <porta-ssh> <usuario>@143.95.209.35
journalctl -u cbmp-hub-teste -n 200 --no-pager         # Hub (JSON do pino; nunca tokens/segredos)
journalctl -u cbmp-server-teste -n 200 --no-pager      # cb-server (também em server/logs/server.log)
bash /var/www/html_cbmultiplayer/deploy/remote-install.sh status
sudo systemctl restart cbmp-hub-teste cbmp-server-teste
```

O Hub em `HUB_LOG_LEVEL=info` registra cada pedido (método, URL, IP real). Em `/v1/auth/*` a query é trocada por
`?[redigido]` no log (o `code`/`state` do OAuth nunca é gravado). O journal guarda os IPs dos testadores (dado
pessoal) pelo tempo de retenção do journald da VPS; apague o histórico do serviço ao fim dos testes se precisar.

## Login com Discord (dono)

1. Discord Developer Portal → **New Application** (ex. "CB Multiplayer Teste") → **OAuth2**.
2. Em **Redirects**, adicione exatamente: `https://cbmultiplayer.cntoficial.com.br/v1/auth/discord/callback`.
3. Copie o **Client ID** e gere/copie o **Client Secret**.
4. Na VPS, crie o arquivo (o secret é digitado sem aparecer na tela e não vai para o histórico):

   ```bash
   sudo install -d -m 700 -o root -g root /etc/cbmp-teste
   read -r -p 'Client ID: ' CID; read -r -s -p 'Client Secret: ' CSEC; echo
   sudo install -m 600 -o root -g root /dev/null /etc/cbmp-teste/discord.env
   printf 'DISCORD_CLIENT_ID=%s\nDISCORD_CLIENT_SECRET=%s\n' "$CID" "$CSEC" | sudo tee /etc/cbmp-teste/discord.env >/dev/null
   unset CID CSEC
   sudo systemctl restart cbmp-hub-teste
   ```

   Opcional: para usar a rota de destaque como admin da plataforma, acrescente `HUB_PLATFORM_ADMINS=<seu Discord ID>`
   no mesmo arquivo. O arquivo é lido pelo systemd (como root) antes de iniciar o Hub; o processo do Hub não enxerga
   `/etc/cbmp-teste`. Sem o arquivo o Hub sobe normalmente.
5. Confira: `node tools/teste-online.mjs --check` (o passo de login passa a mostrar o `client_id`).

## Parar e remover

- Parar: `sudo systemctl stop cbmp-server-teste cbmp-hub-teste` (e `disable` para não voltar no boot).
- Remover tudo (units, site nginx, certificado, as regras do ufw com o comentário `CB Multiplayer teste`, a pasta
  inteira, inclusive o banco do Hub de teste, e os usuários `cbmp-hub`/`cbmp-server`):

  ```bash
  bash /var/www/html_cbmultiplayer/deploy/remote-install.sh uninstall --yes
  sudo rm -r /etc/cbmp-teste     # o arquivo do app Discord, se quiser
  ```

  E no Discord Developer Portal, apague o app de teste (ou o redirect).

## Limites conhecidos

- O e2e do launcher (`launcher/src-tauri/tests/e2e.rs`) usa dev-login e não roda contra este Hub (dev-login nunca é
  ligado aqui). O `--check` cobre o mesmo caminho sem login: Hub, lista, servidor online, `/info`, manifest, download
  e hash de cada arquivo, mod, update e redirect do login.
- Os três builds (oficial/"Teste local"/"Teste") registram o esquema `cbmp://`: com mais de um instalado, o login cai
  no último instalado.
- Releases de teste assinadas com a chave **dev**: quando o par de produção entrar em `keys/update.pub`, o launcher de
  teste também passa a recusar essas releases (gere um par de teste ou embuta a pública dev só neste perfil).
