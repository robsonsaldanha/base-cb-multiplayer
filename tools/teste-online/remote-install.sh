#!/usr/bin/env bash
# Instalação do ambiente de TESTE online na VPS (roda NA VPS, como o usuário dono da pasta, com `sudo -n` só para
# usuários de serviço/systemd/nginx/certbot/ufw/donos de pastas). Chamado por `node tools/teste-online.mjs`;
# docs/AMBIENTE-TESTE-ONLINE.md.
#
#   remote-install.sh install <pasta-staging>   instala/atualiza Hub, cb-server, releases, units e site nginx
#   remote-install.sh cert                      emite o certificado (certbot --webroot) e liga o HTTPS
#   remote-install.sh firewall                  libera 30500/udp e 30500/tcp (comentário "CB Multiplayer teste")
#   remote-install.sh status                    estado dos serviços e saúde local
#   remote-install.sh uninstall --yes           remove TUDO deste ambiente (units, site, certificado, regras, pasta,
#                                               usuários de serviço)
#
# Só mexe no que é deste ambiente: /var/www/html_cbmultiplayer, os usuários de sistema cbmp-hub e cbmp-server,
# cbmp-*-teste.service, o site nginx cbmultiplayer.cntoficial.com.br, o certificado desse nome e as duas regras do
# ufw com o comentário "CB Multiplayer teste". Nunca imprime segredos.
set -euo pipefail

ROOT=${CBMP_ROOT:-/var/www/html_cbmultiplayer}
HOST=cbmultiplayer.cntoficial.com.br
# Sobreponíveis só para o teste automatizado de nginx_apply (tools/test/teste-online-nginx.test.mjs).
NGINX_AVAILABLE=${CBMP_NGINX_AVAILABLE:-/etc/nginx/sites-available}
NGINX_ENABLED=${CBMP_NGINX_ENABLED:-/etc/nginx/sites-enabled}
NGINX_CONFD=${CBMP_NGINX_CONFD:-/etc/nginx/conf.d}
NGINX_OWNER=${CBMP_NGINX_OWNER-root}
SITE=$NGINX_AVAILABLE/$HOST
SITE_LINK=$NGINX_ENABLED/$HOST
UNITS=(cbmp-hub-teste.service cbmp-server-teste.service)
HUB_USER=cbmp-hub
SRV_USER=cbmp-server
UFW_COMMENT='CB Multiplayer teste'
SERVER_NAME='CB Multiplayer – Servidor de teste'
SERVER_DESC='Servidor de teste do CB Multiplayer: conteúdo de exemplo (hello-client) para testar lista, login, download e Jogar (modo simular).'

log() { printf '[teste-online] %s\n' "$*"; }
die() { printf '[teste-online] ERRO: %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------------------------------------------------ nginx

# Instala o site e testa; se o `nginx -t` falhar, devolve o estado anterior (arquivo e link) e confere que a config
# compartilhada voltou a passar. Nunca recarrega com config inválida. $1 = arquivo de site ('' = remover o site).
nginx_apply() {
  local new=$1 others bak had_link=0 had_file=0
  if [ -n "$new" ]; then
    others=$(sudo -n grep -RlE "server_name[^;]*\b${HOST//./\\.}\b" "$NGINX_ENABLED" "$NGINX_CONFD" 2>/dev/null | grep -v "^$SITE_LINK\$" || true)
    [ -z "$others" ] || die "outro site nginx já usa $HOST: $others"
  fi
  bak=$(mktemp)
  if sudo -n test -f "$SITE"; then had_file=1; sudo -n cat "$SITE" > "$bak"; fi
  if sudo -n test -L "$SITE_LINK"; then had_link=1; fi

  local own=()
  [ -n "$NGINX_OWNER" ] && own=(-o "$NGINX_OWNER" -g "$NGINX_OWNER")
  if [ -n "$new" ]; then
    sudo -n install -m 644 "${own[@]}" "$new" "$SITE"
    [ "$had_link" = 1 ] || sudo -n ln -s "$SITE" "$SITE_LINK"
  else
    sudo -n rm -f "$SITE_LINK" "$SITE"
  fi

  if ! sudo -n nginx -t >/dev/null 2>&1; then
    log "nginx -t falhou com o site novo; desfazendo"
    if [ "$had_file" = 1 ]; then sudo -n install -m 644 "${own[@]}" "$bak" "$SITE"; else sudo -n rm -f "$SITE"; fi
    if [ "$had_link" = 1 ]; then [ -L "$SITE_LINK" ] || sudo -n ln -s "$SITE" "$SITE_LINK"; else sudo -n rm -f "$SITE_LINK"; fi
    rm -f "$bak"
    sudo -n nginx -t >/dev/null 2>&1 || die "nginx -t continua falhando DEPOIS de desfazer: o problema não é deste site (nada recarregado)"
    die "site do $HOST recusado pelo nginx -t; estado anterior restaurado, nada recarregado"
  fi
  rm -f "$bak"
  sudo -n systemctl reload nginx
}

# ----------------------------------------------------------------------------------------------- usuários e pastas

ensure_user() { # $1 usuário, $2 descrição
  if ! getent passwd "$1" >/dev/null; then
    sudo -n useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin \
      --comment "$2" "$1"
    log "usuário de sistema $1 criado"
  fi
}

wait_health() {
  for _ in $(seq 1 30); do
    if curl -fsS -m 2 http://127.0.0.1:8787/health >/dev/null 2>&1; then return 0; fi
    sleep 1
  done
  die "Hub não respondeu em http://127.0.0.1:8787/health (journalctl -u cbmp-hub-teste)"
}

install_all() {
  local S=$1
  [ -d "$S" ] || die "pasta de staging inexistente: $S"
  ensure_user "$HUB_USER" 'CB Multiplayer teste (Hub)'
  ensure_user "$SRV_USER" 'CB Multiplayer teste (cb-server)'

  # Código e releases: do usuário do deploy, só leitura para os serviços.
  mkdir -p "$ROOT"/{hub,releases,acme,download,deploy,server/bin,server/resources}
  sudo -n mkdir -p "$ROOT/hub-data" "$ROOT/server/data" "$ROOT/server/logs"
  # Dados: cada serviço só no seu (700). O cb-server não lê hub-data e o Hub não lê server/.
  sudo -n chown -R "$HUB_USER:$HUB_USER" "$ROOT/hub-data"
  sudo -n chown -R "$SRV_USER:$SRV_USER" "$ROOT/server/data" "$ROOT/server/logs"
  sudo -n chmod 700 "$ROOT/hub-data" "$ROOT/server/data" "$ROOT/server/logs"

  # --- Hub: bundle esbuild + better-sqlite3 do lockfile (versão e integridade do package-lock do repositório).
  # --ignore-scripts: nenhum script de pacote roda; o binário nativo vem pronto dentro do pacote (prebuilds/).
  install -m 644 "$S/hub/main.mjs" "$S/hub/cli.mjs" "$S/hub/package.json" "$S/hub/package-lock.json" "$ROOT/hub/"
  (cd "$ROOT/hub" && npm ci --omit=dev --ignore-scripts --no-audit --no-fund --loglevel=error >/dev/null)
  [ -f "$ROOT/hub/node_modules/better-sqlite3/prebuilds/linux-x64.node" ] || die "better-sqlite3 sem o binário linux-x64"
  install -m 644 "$S/hub.env" "$ROOT/hub.env"

  # --- releases assinadas (launcher + mod de teste) e instalador dos testadores
  [ -d "$S/releases" ] && rsync -a --delete --chmod=D755,F644 "$S/releases/" "$ROOT/releases/"
  if [ -f "$S/download/CB-Multiplayer-Teste-setup.exe" ]; then install -m 644 "$S/download/"* "$ROOT/download/"; fi

  # --- cb-server: sha256 e commit conferidos ANTES de extrair; nada do pacote é executado pelo usuário do deploy.
  if [ -f "$S/cb-server-linux-x64.tar.gz" ]; then
    local want got commit
    want=$(cat "$S/SERVER_SHA256"); got=$(sha256sum "$S/cb-server-linux-x64.tar.gz" | cut -d' ' -f1)
    [ "$want" = "$got" ] || die "sha256 do pacote do cb-server não confere (esperado $want, recebido $got)"
    commit=$(tar -xzOf "$S/cb-server-linux-x64.tar.gz" cb-server-linux-x64/COMMIT | tr -d '[:space:]')
    [ "$commit" = "$(cat "$S/SERVER_COMMIT")" ] || die "COMMIT do pacote ($commit) difere do esperado ($(cat "$S/SERVER_COMMIT"))"
    rm -rf "$ROOT/server/bin.new"; mkdir -p "$ROOT/server/bin.new"
    tar -xzf "$S/cb-server-linux-x64.tar.gz" -C "$ROOT/server/bin.new" --strip-components=1 --no-same-owner
    chmod -R go-w "$ROOT/server/bin.new"
    rm -rf "$ROOT/server/bin.old"
    [ -d "$ROOT/server/bin" ] && mv "$ROOT/server/bin" "$ROOT/server/bin.old"
    mv "$ROOT/server/bin.new" "$ROOT/server/bin"
    printf '%s %s\n' "$commit" "$got" > "$ROOT/deploy/SERVER"
    log "cb-server do commit $commit (sha256 $got)"
  fi
  local have_server=1
  [ -x "$ROOT/server/bin/cb-server" ] || { have_server=0; log "AVISO: cb-server ausente (passe --server-tar); só o Hub sobe"; }
  rsync -a --delete --chmod=D755,F644 "$S/resources/" "$ROOT/server/resources/"

  # --- units
  for u in "${UNITS[@]}"; do sudo -n install -m 644 -o root -g root "$S/teste-online/$u" "/etc/systemd/system/$u"; done
  sudo -n systemctl daemon-reload
  sudo -n systemctl enable "${UNITS[@]}" >/dev/null 2>&1

  # --- Hub no ar; servidor cadastrado pelo próprio usuário do Hub (estado com a chave em hub-data, 600)
  sudo -n systemctl restart cbmp-hub-teste.service
  wait_health
  local state="$ROOT/hub-data/test-server.json"
  if sudo -n test -f "$ROOT/server/data/hub-server.json" && ! sudo -n test -f "$state"; then
    sudo -n install -o "$HUB_USER" -g "$HUB_USER" -m 600 "$ROOT/server/data/hub-server.json" "$state"
  fi
  sudo -n rm -f "$ROOT/server/data/hub-server.json"
  (cd "$ROOT/hub" && sudo -n -u "$HUB_USER" env HUB_DATA_DIR="$ROOT/hub-data" node cli.mjs ensure-server \
    --name "$SERVER_NAME" --description "$SERVER_DESC" --tag teste --locale pt-BR --featured --state "$state")
  # server.cfg (com a chave) só do cbmp-server, 600; a chave passa só por pipes.
  [ -e "$ROOT/server/server.cfg" ] || sudo -n install -o "$SRV_USER" -g "$SRV_USER" -m 600 /dev/null "$ROOT/server/server.cfg"
  sudo -n chown "$SRV_USER:$SRV_USER" "$ROOT/server/server.cfg"; sudo -n chmod 600 "$ROOT/server/server.cfg"
  sudo -n cat "$state" | node -e '
    const fs = require("fs");
    const s = JSON.parse(fs.readFileSync(0, "utf8"));
    const cfg = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
    cfg.hub.serverId = s.serverId; cfg.hub.key = s.key;
    process.stdout.write(JSON.stringify(cfg, null, 2) + "\n");
  ' "$S/teste-online/server.cfg.json" | sudo -n tee "$ROOT/server/server.cfg" >/dev/null

  if [ "$have_server" = 1 ]; then
    sudo -n systemctl restart cbmp-server-teste.service
    sleep 3
    if ! systemctl is-active --quiet cbmp-server-teste.service && [ -d "$ROOT/server/bin.old" ] && [ -f "$S/cb-server-linux-x64.tar.gz" ]; then
      log "cb-server novo não ficou de pé; voltando para o anterior"
      mv "$ROOT/server/bin" "$ROOT/server/bin.bad"; mv "$ROOT/server/bin.old" "$ROOT/server/bin"
      sudo -n systemctl restart cbmp-server-teste.service
      die "cb-server novo recusado (journalctl -u cbmp-server-teste); o anterior foi restaurado"
    fi
  fi

  # --- nginx: HTTPS se o certificado já existe; senão só o desafio ACME (rode "cert" depois)
  if sudo -n test -f "/etc/letsencrypt/live/$HOST/fullchain.pem"; then
    nginx_apply "$S/teste-online/nginx.conf"
  else
    nginx_apply "$S/teste-online/nginx-bootstrap.conf"
    log "sem certificado ainda: rode 'remote-install.sh cert'"
  fi
  cp "$S/teste-online/"* "$ROOT/deploy/"
  [ -f "$S/COMMIT" ] && cp "$S/COMMIT" "$ROOT/deploy/COMMIT"
  log "instalado (repositório $(cat "$ROOT/deploy/COMMIT" 2>/dev/null || echo '?'); cb-server $(cut -d' ' -f1 "$ROOT/deploy/SERVER" 2>/dev/null || echo '?'))"
}

cert() {
  sudo -n certbot certonly --webroot -w "$ROOT/acme" -d "$HOST" --cert-name "$HOST" \
    --non-interactive --agree-tos --keep-until-expiring --deploy-hook "systemctl reload nginx"
  nginx_apply "$ROOT/deploy/nginx.conf"
}

firewall() {
  sudo -n ufw allow 30500/udp comment "$UFW_COMMENT"
  sudo -n ufw allow 30500/tcp comment "$UFW_COMMENT"
}

# Apaga só as regras com o nosso comentário (do número maior para o menor, para não deslocar os índices).
firewall_remove() {
  local nums
  nums=$(sudo -n ufw status numbered | grep -F "# $UFW_COMMENT" | sed -E 's/^\[ *([0-9]+)\].*/\1/' | sort -rn)
  for n in $nums; do sudo -n ufw --force delete "$n" >/dev/null; done
}

status() {
  for u in "${UNITS[@]}"; do printf '%s: %s (%s)\n' "$u" "$(systemctl is-active "$u" || true)" "$(systemctl show -p User --value "$u")"; done
  curl -fsS -m 3 http://127.0.0.1:8787/health && echo
  curl -fsS -m 3 http://127.0.0.1:30500/info && echo
  sudo -n ss -lunpt 2>/dev/null | grep -E ':(8787|30500)\b' || true
}

uninstall() {
  [ "${1:-}" = "--yes" ] || die "confirme com: remote-install.sh uninstall --yes (apaga dados do Hub de teste e o certificado)"
  sudo -n systemctl disable --now "${UNITS[@]}" 2>/dev/null || true
  for u in "${UNITS[@]}"; do sudo -n rm -f "/etc/systemd/system/$u"; done
  sudo -n systemctl daemon-reload
  nginx_apply ''
  sudo -n certbot delete --cert-name "$HOST" --non-interactive || true
  firewall_remove
  sudo -n rm -rf "$ROOT"
  for u in "$HUB_USER" "$SRV_USER"; do
    if getent passwd "$u" >/dev/null; then sudo -n userdel "$u"; fi
    if getent group "$u" >/dev/null; then sudo -n groupdel "$u" 2>/dev/null || true; fi
  done
  log "removido. /etc/cbmp-teste/discord.env (do dono) ficou: apague com 'sudo rm -r /etc/cbmp-teste' se quiser."
}

case "${1:-}" in
  install) install_all "${2:?pasta de staging}" ;;
  cert) cert ;;
  firewall) firewall ;;
  status) status ;;
  uninstall) uninstall "${2:-}" ;;
  nginx-apply) nginx_apply "${2-}" ;;   # interno: usado pelo teste automatizado
  *) sed -n '2,15p' "$0"; exit 2 ;;
esac
