#!/usr/bin/env bash
# Server side of the nfs5-web site deploy. Expects in /tmp/nfs5-web-new: web/, out/, data/, mod/, music/, addons/ (any subset),
# nfs5-web.service, nginx-game.conf, nginx_headers.py. Installs /opt/nfs5-web + the nfs5-web service, then turns the
# nfs5-reforged site into the game: "location /" proxies to the service, the static landing's locations and the
# site password go; /api/, /download/, /lan stay. Re-runnable (nginx is switched once, backup .bak-web).
# CLOUD1: the folders it replaces and the old unit stay until the new service answers; if it does not, they go back
# (the site keeps running the old build). nginx_headers.py adds the proxy headers the servers' checks need, once.
set -euo pipefail
SRC=/tmp/nfs5-web-new DIR=/opt/nfs5-web CONF=/etc/nginx/sites-available/nfs5-reforged.conf UNIT=/etc/systemd/system/nfs5-web.service
command -v node >/dev/null || { echo "node is not installed" >&2; exit 1; }
sudo install -d -m 755 "$DIR"
replaced=()
for d in web out data mod music addons; do
  [ -d "$SRC/$d" ] || continue
  sudo rm -rf "$DIR/$d.new" "$DIR/$d.prev"; sudo cp -a "$SRC/$d" "$DIR/$d.new"
  [ -d "$DIR/$d" ] && sudo mv "$DIR/$d" "$DIR/$d.prev"
  sudo mv "$DIR/$d.new" "$DIR/$d"; replaced+=("$d")
done
sudo chown -R root:root "$DIR"; sudo chmod -R a+rX "$DIR"
[ -f "$UNIT" ] && sudo cp "$UNIT" "$UNIT.prev"
sudo install -m 644 "$SRC/nfs5-web.service" "$UNIT"
sudo systemctl daemon-reload
sudo systemctl enable nfs5-web >/dev/null 2>&1
sudo systemctl restart nfs5-web
ok=0
for i in $(seq 1 15); do
  sleep 1
  if [ "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:9460/)" = 200 ] && \
     curl -s http://127.0.0.1:9460/cloud/me | grep -q '"user"'; then ok=1; break; fi
done
if [ "$ok" != 1 ]; then
  echo "the new nfs5-web does not answer: putting the previous build back" >&2
  sudo journalctl -u nfs5-web -n 30 --no-pager >&2 || true
  for d in "${replaced[@]}"; do
    [ -d "$DIR/$d.prev" ] && { sudo rm -rf "$DIR/$d"; sudo mv "$DIR/$d.prev" "$DIR/$d"; }
  done
  [ -f "$UNIT.prev" ] && sudo cp "$UNIT.prev" "$UNIT"
  sudo systemctl daemon-reload; sudo systemctl restart nfs5-web
  exit 1
fi
for d in "${replaced[@]}"; do sudo rm -rf "$DIR/$d.prev"; done
echo "nfs5-web: $(systemctl is-active nfs5-web), / 200, /cloud/me answers"

if ! grep -q 'proxy_pass http://127.0.0.1:9460' "$CONF"; then
  sudo cp "$CONF" "$CONF.bak-web"
  tmp=$(mktemp)
  python3 - "$CONF" "$SRC/nginx-game.conf" > "$tmp" <<'PY'
import re, sys
s = open(sys.argv[1]).read(); game = open(sys.argv[2]).read()
s = s.replace("    include /etc/nginx/snippets/nfs5-reforged-auth.conf;\n", "    # (the site password was removed 2026-11-23: the game is open)\n")
s = s.replace("    location / { try_files $uri $uri/ =404; }\n", game, 1)
s = re.sub(r"\n    # \(an add_header in a location drops.*?\n    location ~\* \\.\(css\|js\|svg\|jpg\|png\|webp\)\$ \{.*?\n    \}\n", "\n", s, count=1, flags=re.S)
s = s.replace("""add_header Content-Security-Policy "default-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'self'; form-action 'none'" always;""",
              """add_header Content-Security-Policy "frame-ancestors 'none'; base-uri 'self'; form-action 'none'" always;""")
assert "9460" in s and "try_files $uri $uri/ =404" not in s and "css|js|svg" not in s and "nfs5-reforged-auth" not in s.split("# (the site password")[0]
sys.stdout.write(s)
PY
  sudo install -m 644 "$tmp" "$CONF"; rm "$tmp"
  if ! sudo nginx -t 2>/dev/null; then sudo cp "$CONF.bak-web" "$CONF"; echo "nginx -t failed, restored" >&2; exit 1; fi
  sudo systemctl reload nginx
  echo "nginx: site switched to the game (backup $CONF.bak-web)"
fi

# the proxy headers (X-Real-IP for / and /lan, Host for /lan), added once
if [ -f "$SRC/nginx_headers.py" ]; then
  tmp=$(mktemp)
  if python3 "$SRC/nginx_headers.py" "$CONF" > "$tmp"; then
    sudo cp "$CONF" "$CONF.bak-headers"
    sudo install -m 644 "$tmp" "$CONF"
    if ! sudo nginx -t 2>/dev/null; then sudo cp "$CONF.bak-headers" "$CONF"; echo "nginx -t failed with the headers, restored" >&2; rm "$tmp"; exit 1; fi
    sudo systemctl reload nginx
    echo "nginx: proxy headers added (backup $CONF.bak-headers)"
  fi
  rm -f "$tmp"
fi
