#!/usr/bin/env bash
# Server side of the nfs5-web site deploy. Expects in /tmp/nfs5-web-new: web/, out/, data/, mod/, music/, addons/ (any subset),
# nfs5-web.service, nginx-game.conf. Installs /opt/nfs5-web + the nfs5-web service, then turns the
# nfs5-reforged site into the game: "location /" proxies to the service, the static landing's locations and the
# site password go; /api/, /download/, /lan stay. Re-runnable (nginx is changed once, backup .bak-web).
set -euo pipefail
SRC=/tmp/nfs5-web-new DIR=/opt/nfs5-web CONF=/etc/nginx/sites-available/nfs5-reforged.conf
command -v node >/dev/null || { echo "node is not installed" >&2; exit 1; }
sudo install -d -m 755 "$DIR"
for d in web out data mod music addons; do
  [ -d "$SRC/$d" ] || continue
  sudo rm -rf "$DIR/$d.new"; sudo cp -a "$SRC/$d" "$DIR/$d.new"
  sudo rm -rf "$DIR/$d.old"; [ -d "$DIR/$d" ] && sudo mv "$DIR/$d" "$DIR/$d.old"
  sudo mv "$DIR/$d.new" "$DIR/$d"; sudo rm -rf "$DIR/$d.old"
done
sudo chown -R root:root "$DIR"; sudo chmod -R a+rX "$DIR"
sudo install -m 644 "$SRC/nfs5-web.service" /etc/systemd/system/nfs5-web.service
sudo systemctl daemon-reload
sudo systemctl enable nfs5-web >/dev/null 2>&1
sudo systemctl restart nfs5-web
sleep 2
systemctl is-active nfs5-web
curl -s -o /dev/null -w "local / %{http_code}\n" http://127.0.0.1:9460/

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
