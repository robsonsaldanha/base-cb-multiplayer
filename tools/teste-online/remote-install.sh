#!/usr/bin/env bash
# Instalação do ambiente de TESTE online na VPS (roda NA VPS, como o usuário dono da pasta, com `sudo -n` só para
# systemd/nginx/certbot/ufw). Chamado por `node tools/teste-online.mjs`; docs/AMBIENTE-TESTE-ONLINE.md.
#
#   remote-install.sh install <pasta-staging>   instala/atualiza Hub, cb-server, releases, units e site nginx
#   remote-install.sh cert                      emite o certificado (certbot --webroot) e liga o HTTPS
#   remote-install.sh firewall                  libera 30500/udp e 30500/tcp (comentário "CB Multiplayer teste")
#   remote-install.sh status                    estado dos serviços e saúde local
#   remote-install.sh uninstall --yes           remove TUDO deste ambiente (units, site, certificado, regras, pasta)
#
# Só mexe no que é deste ambiente: /var/www/html_cbmultiplayer, cbmp-*-teste.service, o site nginx
# cbmultiplayer.cntoficial.com.br, o certificado desse nome e as duas regras do ufw. Nunca imprime segredos.
set -euo pipefail

ROOT=/var/www/html_cbmultiplayer
HOST=cbmultiplayer.cntoficial.com.br
SITE=/etc/nginx/sites-available/$HOST
SITE_LINK=/etc/nginx/sites-enabled/$HOST
UNITS=(cbmp-hub-teste.service cbmp-server-teste.service)
SERVER_NAME='CB Multiplayer – Servidor de teste'
SERVER_DESC='Servidor de teste do CB Multiplayer: conteúdo de exemplo (hello-client) para testar lista, login, download e Jogar (modo simular).'
ME=$(id -un)

log() { printf '[teste-online] %s\n' "$*"; }
die() { printf '[teste-online] ERRO: %s\n' "$*" >&2; exit 1; }

nginx_apply() { # $1 = arquivo de site a instalar
  # Outro site já responde por este nome? (o nosso arquivo é o único permitido)
  local others
  others=$(sudo -n grep -RlE "server_name[^;]*\b${HOST//./\\.}\b" /etc/nginx/sites-enabled /etc/nginx/conf.d 2>/dev/null | grep -v "^$SITE_LINK\$" || true)
  [ -z "$others" ] || die "outro site nginx já usa $HOST: $others"
  sudo -n install -m 644 -o root -g root "$1" "$SITE"
  [ -L "$SITE_LINK" ] || sudo -n ln -s "$SITE" "$SITE_LINK"
  sudo -n nginx -t
  sudo -n systemctl reload nginx
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
  mkdir -p "$ROOT"/{hub,hub-data,releases,acme,server/bin,server/data,server/logs,server/resources}
  chmod 700 "$ROOT/hub-data" "$ROOT/server/data"

  # --- Hub (bundle esbuild + better-sqlite3)
  install -m 644 "$S/hub/main.mjs" "$S/hub/cli.mjs" "$S/hub/package.json" "$ROOT/hub/"
  (cd "$ROOT/hub" && npm install --omit=dev --no-audit --no-fund --loglevel=error >/dev/null)
  install -m 640 "$S/hub.env" "$ROOT/hub.env"

  # --- releases assinadas (launcher + mod de teste)
  if [ -d "$S/releases" ]; then
    rsync -a --delete "$S/releases/" "$ROOT/releases/"
  fi

  # --- instalador dos testadores (https://<host>/CB-Multiplayer-Teste-setup.exe)
  if [ -f "$S/download/CB-Multiplayer-Teste-setup.exe" ]; then
    mkdir -p "$ROOT/download"
    install -m 644 "$S/download/"* "$ROOT/download/"
  fi

  # --- cb-server (pacote do workflow server-linux-dist)
  if [ -f "$S/cb-server-linux-x64.tar.gz" ]; then
    rm -rf "$ROOT/server/bin.new"
    mkdir -p "$ROOT/server/bin.new"
    tar -xzf "$S/cb-server-linux-x64.tar.gz" -C "$ROOT/server/bin.new" --strip-components=1
    "$ROOT/server/bin.new/cb-server" --help >/dev/null
    rm -rf "$ROOT/server/bin.old"
    [ -d "$ROOT/server/bin" ] && mv "$ROOT/server/bin" "$ROOT/server/bin.old"
    mv "$ROOT/server/bin.new" "$ROOT/server/bin"
  fi
  HAVE_SERVER=1
  [ -x "$ROOT/server/bin/cb-server" ] || { HAVE_SERVER=0; log "AVISO: cb-server ausente em $ROOT/server/bin (passe --server-tar); só o Hub sobe"; }
  rsync -a --delete "$S/resources/" "$ROOT/server/resources/"

  # --- units (com o usuário que roda o deploy)
  for u in "${UNITS[@]}"; do
    sed "s/@USER@/$ME/g" "$S/teste-online/$u" > "$S/$u.final"
    sudo -n install -m 644 -o root -g root "$S/$u.final" "/etc/systemd/system/$u"
  done
  sudo -n systemctl daemon-reload
  sudo -n systemctl enable "${UNITS[@]}" >/dev/null 2>&1

  # --- Hub no ar e servidor cadastrado (estado com a chave só aqui, modo 600)
  sudo -n systemctl restart cbmp-hub-teste.service
  wait_health
  (cd "$ROOT/hub" && set -a && . "$ROOT/hub.env" && set +a && node cli.mjs ensure-server --name "$SERVER_NAME" \
    --description "$SERVER_DESC" --tag teste --locale pt-BR --featured --state "$ROOT/server/data/hub-server.json")
  umask 077
  node -e '
    const fs = require("fs");
    const [tpl, state, out] = process.argv.slice(1);
    const s = JSON.parse(fs.readFileSync(state, "utf8"));
    const cfg = JSON.parse(fs.readFileSync(tpl, "utf8"));
    cfg.hub.serverId = s.serverId; cfg.hub.key = s.key;
    fs.writeFileSync(out + ".tmp", JSON.stringify(cfg, null, 2) + "\n", { mode: 0o600 });
    fs.renameSync(out + ".tmp", out);
  ' "$S/teste-online/server.cfg.json" "$ROOT/server/data/hub-server.json" "$ROOT/server/server.cfg"
  umask 022
  chmod 600 "$ROOT/server/server.cfg" "$ROOT/server/data/hub-server.json"
  if [ "$HAVE_SERVER" = 1 ]; then sudo -n systemctl restart cbmp-server-teste.service; fi

  # --- nginx: HTTPS se o certificado já existe; senão só o desafio ACME (rode "cert" depois)
  if sudo -n test -f "/etc/letsencrypt/live/$HOST/fullchain.pem"; then
    nginx_apply "$S/teste-online/nginx.conf"
  else
    nginx_apply "$S/teste-online/nginx-bootstrap.conf"
    log "sem certificado ainda: rode 'remote-install.sh cert'"
  fi
  mkdir -p "$ROOT/deploy"
  cp "$S/teste-online/"* "$ROOT/deploy/"
  [ -f "$S/COMMIT" ] && cp "$S/COMMIT" "$ROOT/deploy/COMMIT"
  log "instalado (commit $(cat "$ROOT/deploy/COMMIT" 2>/dev/null || echo '?'))"
}

cert() {
  sudo -n certbot certonly --webroot -w "$ROOT/acme" -d "$HOST" --cert-name "$HOST" \
    --non-interactive --agree-tos --keep-until-expiring --deploy-hook "systemctl reload nginx"
  nginx_apply "$ROOT/deploy/nginx.conf"
}

firewall() {
  sudo -n ufw allow 30500/udp comment 'CB Multiplayer teste'
  sudo -n ufw allow 30500/tcp comment 'CB Multiplayer teste'
}

status() {
  for u in "${UNITS[@]}"; do printf '%s: %s\n' "$u" "$(systemctl is-active "$u" || true)"; done
  curl -fsS -m 3 http://127.0.0.1:8787/health && echo
  curl -fsS -m 3 http://127.0.0.1:30500/info && echo
  sudo -n ss -lunpt 2>/dev/null | grep -E ':(8787|30500)\b' || true
}

uninstall() {
  [ "${1:-}" = "--yes" ] || die "confirme com: remote-install.sh uninstall --yes (apaga dados do Hub de teste e o certificado)"
  sudo -n systemctl disable --now "${UNITS[@]}" 2>/dev/null || true
  for u in "${UNITS[@]}"; do sudo -n rm -f "/etc/systemd/system/$u"; done
  sudo -n systemctl daemon-reload
  sudo -n rm -f "$SITE_LINK" "$SITE"
  sudo -n nginx -t && sudo -n systemctl reload nginx
  sudo -n certbot delete --cert-name "$HOST" --non-interactive || true
  sudo -n ufw delete allow 30500/udp || true
  sudo -n ufw delete allow 30500/tcp || true
  rm -rf "$ROOT"
  log "removido. /etc/cbmp-teste/discord.env (do dono) ficou: apague com 'sudo rm -r /etc/cbmp-teste' se quiser."
}

case "${1:-}" in
  install) install_all "${2:?pasta de staging}" ;;
  cert) cert ;;
  firewall) firewall ;;
  status) status ;;
  uninstall) uninstall "${2:-}" ;;
  *) sed -n '2,12p' "$0"; exit 2 ;;
esac
