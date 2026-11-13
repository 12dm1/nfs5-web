#!/usr/bin/env bash
# Server side of the nfs5-web online deploy (docs/tasks/ON1-online.md "Deployment"). Expects in /tmp/nfs5-web-online:
# online/*.mjs, data/nfs5.car, data/nfs5.trn, nfs5-web-online.service, nginx-lan.conf.
# Touches only /opt/nfs5-web-online, the nfs5-web-online service and the /lan block of nfs5-reforged.conf. Re-runnable.
set -euo pipefail
SRC=/tmp/nfs5-web-online DIR=/opt/nfs5-web-online CONF=/etc/nginx/sites-available/nfs5-reforged.conf
command -v node >/dev/null || { echo "node is not installed" >&2; exit 1; }
sudo install -d -m 755 "$DIR/online" "$DIR/data/FEData/Data"
sudo install -m 644 "$SRC"/online/*.mjs "$DIR/online/"
sudo install -m 644 "$SRC/data/nfs5.car" "$SRC/data/nfs5.trn" "$DIR/data/FEData/Data/"
sudo install -m 644 "$SRC/nfs5-web-online.service" /etc/systemd/system/nfs5-web-online.service
sudo systemctl daemon-reload
sudo systemctl enable nfs5-web-online >/dev/null 2>&1
sudo systemctl restart nfs5-web-online

# nginx: insert the /lan locations once, before the last "}" of the HTTPS server block (the file's last line)
if ! grep -q 'location = /lan ' "$CONF"; then
  sudo cp "$CONF" "$CONF.bak-online"
  tmp=$(mktemp)
  head -n -1 "$CONF" > "$tmp"; cat "$SRC/nginx-lan.conf" >> "$tmp"; tail -n 1 "$CONF" >> "$tmp"
  if [ "$(tail -n 1 "$CONF" | tr -d '[:space:]')" != "}" ]; then echo "unexpected end of $CONF, nginx not changed" >&2; rm "$tmp"; exit 1; fi
  sudo install -m 644 "$tmp" "$CONF"; rm "$tmp"
  if ! sudo nginx -t 2>/dev/null; then sudo cp "$CONF.bak-online" "$CONF"; echo "nginx -t failed, restored" >&2; exit 1; fi
  sudo systemctl reload nginx
fi
sleep 1
echo "service: $(systemctl is-active nfs5-web-online)"
echo "status: $(curl -s --max-time 3 http://127.0.0.1:9450/status)"
