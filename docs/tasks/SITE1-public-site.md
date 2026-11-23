# SITE1: the game on the public site

2026-11-23. https://nfs5-reforged.duckdns.org/ is the browser game, open without a password: `/` = a page with one
PLAY button (`web/src/play.html`), `/game` = the game (`web/src/index.html`). The old static landing is gone.

## What runs where (the VPS shared with Louditude)

| Path | Served by |
|---|---|
| `/`, `/game`, `/main.js`, `/host/*`, `/audio/*`, `/nfs5.wasm`, `/nfs5.json`, `/data-manifest`, `/data/*` | service `nfs5-web`: `node /opt/nfs5-web/web/server.mjs --public` on 127.0.0.1:9460 (COOP/COEP headers come from it) |
| `/lan`, `/lan-status` | service `nfs5-web-online` (ON1, deploy/install-online.sh), unchanged |
| `/api/*` (PC launcher: latest.json, news.json, status) | nginx static + netserver status, unchanged |
| `/download/*`, `/favicon.ico`, `/apple-touch-icon.png` | nginx files, unchanged |

`/opt/nfs5-web`: `web/` (server.mjs, online/, dist/ without source maps), `out/web/nfs5.wasm|json`, `out/PORSCHE.EXE`
(patched), `data/` (from nfs5_clean only FEData, GameData, Drivers, fe.txt, version.txt, install.txt, readme.txt:
the game stops without install.txt; the menus open the same missing files as with the whole folder), `mod/`
(out/mod), `music/` (zzzymus.viv). 576 MB, 1882 files in the manifest.

`--public`: no `/test-*` endpoints (`/test-shot` wrote files to disk), the manifest is read once at start, static
files answer 304 to their ETag. nginx gzips the wasm (19 MB -> 6 MB). The service runs as www-data with a read-only
system; `--logs /var/lib/nfs5-web` for the built-in online instance it does not serve (nginx sends `/lan` elsewhere).

## Deploy

Build in a worktree (`./gen.sh <exe>`, `./web/build.sh`, `cd web && node build.mjs`), stage
`out/deploy/pkg/{web,out}` + `out/deploy/data`, upload everything to `/tmp/nfs5-web-new` (one tar stream per
folder, the junctions need `tar -h`) with `deploy/nfs5-web.service`, `deploy/nginx-game.conf`,
`deploy/install-web.sh`, then `bash /tmp/nfs5-web-new/install-web.sh` on the VPS. nginx is changed once (backup
`nfs5-reforged.conf.bak-web`): `location /` proxies to 9460, the landing's static locations, its CSP and the site
password include are removed.

**The nfs5-reforged repository's `server/setup-web.sh` would overwrite the live nginx file** (it has neither the
game nor `/lan`): do not re-run it without merging these blocks.

## Checked

- `web/tools/live_test.mjs` against the live site: PLAY -> `/game`, `crossOriginIsolated`, the game boots, joins
  the virtual network (`online as 10.77.1.1`), no failed responses.
- Before upload, the staged data folder: a direct race at 144 fps, the menus with the same missing files as the full
  folder.
- The merged build (online + audio-input + cars-limit, branch `release`): `online_test --mode 0` PASS;
  `--mode 1` times out the same way on the unmerged online build (the host starts the race before the joiner is in).

Not checked: a person playing on the site, other browsers, several players at once on the 2-core VPS.
