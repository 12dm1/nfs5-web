#!/usr/bin/env bash
# Deploys the site's code (web/: server.mjs, cloud/, online/, dist/) and the online server's (online/*.mjs) to the VPS,
# without the game data (data/, mod/, music/, addons/ and out/ stay as they are there).
#   deploy/push-web.sh <ssh host> [--site-only|--online-only]
# The site first (install-web.sh: rolls back when the new service does not answer; adds the nginx proxy headers once),
# then the online server (install-online.sh). The ssh host is an argument: no server address lives in this repository.
set -euo pipefail
HOST=${1:?usage: deploy/push-web.sh <ssh host> [--site-only|--online-only]}
WHAT=${2:-all}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

(cd "$ROOT/web" && node build.mjs)

if [ "$WHAT" != --online-only ]; then
  mkdir -p "$STAGE/site/web"
  cp -r "$ROOT/web/server.mjs" "$ROOT/web/cloud" "$ROOT/web/online" "$ROOT/web/dist" "$STAGE/site/web/"
  rm -f "$STAGE/site/web/dist/"*.map
  cp "$ROOT/deploy/nfs5-web.service" "$ROOT/deploy/nginx-game.conf" "$ROOT/deploy/nginx_headers.py" "$ROOT/deploy/install-web.sh" "$STAGE/site/"
  tar -C "$STAGE/site" -czf - . | ssh "$HOST" 'rm -rf /tmp/nfs5-web-new && mkdir -p /tmp/nfs5-web-new && tar -C /tmp/nfs5-web-new -xzf -'
  ssh "$HOST" 'bash /tmp/nfs5-web-new/install-web.sh'
fi
if [ "$WHAT" != --site-only ]; then
  mkdir -p "$STAGE/online/online"
  cp "$ROOT/web/online/"*.mjs "$STAGE/online/online/"
  cp "$ROOT/deploy/nfs5-web-online.service" "$ROOT/deploy/nginx-lan.conf" "$ROOT/deploy/nginx_headers.py" "$ROOT/deploy/install-online.sh" "$STAGE/online/"
  tar -C "$STAGE/online" -czf - . | ssh "$HOST" 'rm -rf /tmp/nfs5-web-online && mkdir -p /tmp/nfs5-web-online && tar -C /tmp/nfs5-web-online -xzf -'
  ssh "$HOST" 'bash /tmp/nfs5-web-online/install-online.sh'
fi
echo "deployed ($WHAT)"
