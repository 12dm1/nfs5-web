# CLOUD1: accounts and cloud saves (with a security review)

Asked 2026-11-23: as in nfsu2-web, keep a player's career on our server so every device has the same profiles.
On PLAY: signed in on the site, go on; not signed in, offer to create an account quickly (login + password) and tell
the player to keep the password, since the career is tied to it. The game then gets the account's saves, and every save
the game writes goes to the server under that login.

Decisions (the user, 2026-11-23):

| question | decision |
|---|---|
| account kind | login + password (not nfsu2-web's keyphrase) |
| playing without an account | stays ("Play without an account", saves only in that browser) |
| both devices changed the saves | the newer one wins; the other is kept and can be downloaded |
| login = online name | yes |
| password recovery | none for now (no e-mail) |

## What the player sees

* **PLAY page** (`web/src/play.html`, `play.ts`). Under PLAY: "Signed in as X, cloud save 5 min ago, Account", or "Cloud
  saves: not signed in, Sign in or create an account, Saves on this device".
* **PLAY without an account** opens the dialog. First a big *Play without an account* button (it has the focus, so no
  phone keyboard pops up) with *Don't ask again on this device* under it; then, after "or", the account form:
  *Create account* / *Sign in* tabs, login (3-15 letters, digits, `_`, `-`; also the online name), password (8+)
  twice, the warning that there is no recovery. Opened from *Sign in or create an account* instead, the dialog has
  no guest button (2026-11-23: it was a small link under the form, and visitors had to read the form to find it).
* **After creating an account**: "Account ready", a *Save the login and password as a file* button (a text file), Play.
  Chrome and Edge are asked to save the password (`PasswordCredential`); the others see a normal login form
  (`autocomplete=username/new-password`).
* **Account dialog**: the cloud save (profiles, when, which device) with Download; *Older cloud versions* (the server's
  backups) with Download; *Saves on this device* with Download and *Restore from a file*; *Replaced on this device*
  (local backups) with Download; change password; sign out; delete account. Every download is a zip with a
  `SaveData/` folder (unpacked into a game folder it is the PC game's layout). Guests get the device part only.
* **In the game**: a line at the top for a few seconds when something happened (a newer career came from another
  device, these saves replaced the cloud's, the server is unreachable, this device was signed out).

## Sync (`web/src/cloud/sync.ts`)

What syncs: the files of the save folder's `SaveData` (career profiles `.sav`/`.hud`, `player.lst`, `nfs5.trk`
records). The rest of the save folder stays on the device: `registry.txt` (this device's settings), the key bindings
(`FEData/controls`), logs.

Each browser keeps its sync state in OPFS `cloud/state.json`: the account, the cloud revision its saves came from, and
their content hash (SHA-256 over the files sorted by name; `contentHash` in `zip.ts` and `zipcheck.mjs` compute the
same, the tests check they agree).

Before the game starts (`beforeGame`, while the page holds the Web Lock `nfs5-game`, so a second tab with the game
never syncs and nothing writes the saves while a game runs):

| saves here | cloud | what happens |
|---|---|---|
| same content as the cloud | | nothing |
| unchanged since the last sync | changed | the cloud's come here |
| changed | unchanged | these are sent |
| changed | changed | **the newer wins**: compared by when the saves were last changed (the newest file's time, sent as `X-Made-At` on the server's clock), not by upload time. Cloud's newer: they come here, these go to a local backup. These newer: sent with `X-Overwrite`, the cloud's becomes a cloud backup |
| no profile | has profiles | the cloud's come here (an empty save never replaces a career) |
| none | none | nothing |

While the game runs: the I/O worker's `saved`/`removed` for a `SaveData/` file sends the saves 3 s later (one upload
for a burst of writes). A 412 then means another device sent meanwhile: the save made just now is the newer one, it
replaces the cloud's (kept as a backup) and the player is told. A failed upload is retried (5 s, 15 s, 45 s ... 5 min);
the saves are safe on the device anyway, and the next game start sends them. A 401 (password changed elsewhere) stops
syncing in that tab and says so. When the game exits or the page closes, a waiting upload is sent at once.

## Server (`web/cloud/cloud.mjs`, `zipcheck.mjs`, in `web/server.mjs`)

| request | what |
|---|---|
| `GET /cloud/me` | `{ user, save, backups, now }` or `{ user: null }` |
| `POST /cloud/register` `{user,password,device}` | new account, signed in |
| `POST /cloud/login` | signed in (a new session) |
| `POST /cloud/logout` | this session ends |
| `POST /cloud/password` `{old,password}` | new password; every other session ends |
| `POST /cloud/delete` `{password}` | the account and its saves are deleted |
| `GET /cloud/save[?b=<backup>]` | the zip (attachment) |
| `PUT /cloud/save` | a zip; `If-Match: "<rev>"` / `If-None-Match: *` / `X-Overwrite: 1`; `X-Device`, `X-Made-At` |
| `GET /cloud/taken?u=` | whether a name is an account's |

Storage under `--cloud` (live: `/var/lib/nfs5-web/cloud`, the service's own user, folders 0700, files 0600, every
write a temporary file + rename, one operation at a time per account):

```
accounts/<key>.json   { v:1, name, created, pw, sessions[{h, created, used, dev}], save{rev,at,made,size,hash,dev,profiles}, backups[] }
saves/<key>/cur.zip   b-<id>.zip
```

`key` = the first 40 hex digits of SHA-256(lower-case name): no file name ever comes from what a client typed.
Backups: the replaced version is kept when it came from another device, had more profiles, or the last backup is
older than 20 h; at most 5. Accounts that never stored a save and were not used for 30 days are removed by the daily
sweep (the name is free again); sessions unused for a year expire.

The online server (`web/online/vnet.mjs`) asks the site server on its loopback-only `--internal-port` (9461):
`/whoami` (the account of the WebSocket upgrade's cookie) and `/taken?u=`. A signed-in player plays online under the
account name whatever the game profile is called; a guest who types an account's name plays as `<name>-guest`.

## Security review (2026-11-23)

Scope: the site server (`server.mjs`), the new cloud code, the online server (`web/online`), the pages, the nginx
site and the VPS as far as it concerns them. Method: reading the code, a crash probe of the live site's server
code (run against a local copy), a WebSocket fuzzer for the online server, nfsu2-web's own audit notes (its
`ONL-*`, `PAGE-*`, `PRIV-*`, `DEP-*` findings) as a checklist, then `web/tools/cloud_test.mjs` (106 checks) and
`web/tools/cloud_browser_test.mjs` (two browsers, the real game).

### Found and fixed

| id | where | what | fix |
|---|---|---|---|
| SEC-1 | `server.mjs` (live) | **One request killed the site**: `/%E0%A4%A` (`decodeURIComponent` URIError), `/data/x?b=-5`, `Range: bytes=999999999-` or `bytes=500-100` (stream RangeError) threw out of the handler and the Node process exited; systemd restarts it in 3 s, so a loop of them keeps the site down | the handler runs in try/catch (400/500 for that request); block numbers must be non-negative integers; a Range past the end or reversed is 416; read streams carry an error handler; request/header timeouts |
| SEC-2 | `main.ts` (live) | **A link could change a player's game and wipe a save**: `/game?seed=<file>` wrote the (404) answer of `/test-seed` as an empty file over that save; `env=`, `save=`, `lan=wss://elsewhere` (the online traffic to another server), `log=` too | test parameters are taken only from a development server (`/test-dev.json` answers `{"dev":true}`; the public one `{"dev":false}`, so no 404 in the players' console) |
| SEC-3 | pages (live) | no Content-Security-Policy on the game (nginx's `add_header` in `location /` drops the server block's), so nothing stopped framing (clickjacking a future account page) or a script from elsewhere | `server.mjs` sends CSP (`default-src 'self'`, `script-src 'self' 'wasm-unsafe-eval'`, `connect-src 'self' wss://host`, `frame-ancestors 'none'`, `base-uri 'none'`, `object-src 'none'`), `X-Frame-Options: DENY`, HSTS; play.html's inline script moved to `play.js`. The game, its workers, sound worklet, wasm and the online link run under it (browser test: no violation) |
| SEC-4 | online (live) | `/lan` accepted WebSockets from any site's page and any number of connections; a 1 MB frame limit per connection (memory); a ping over 125 bytes got a malformed pong | Origin must be the site (`Host`, or `--origin`); at most 16 connections per address (X-Real-IP from nginx) and 500 in all; frames over 8 KB close the connection; long pings get no answer |
| SEC-5 | online (live) | a player's name went into the lobby and logs as typed (control characters, `~`) and anyone could take anyone's name | names cleaned; signed-in = account name; guests cannot use an account's name |
| SEC-6 | VPS | the site's service ran as `www-data`, the user the VPS's PHP sites run as: a hole in any of them could read or change the accounts | `DynamicUser=yes` (a user of its own), state 0700, UMask 0077, more systemd sandboxing |
| SEC-7 | nginx | no `X-Real-IP` to the node servers: every client looked like 127.0.0.1 | `nginx_headers.py` adds it (and `Host` for `/lan`), once, with `nginx -t` and a backup. Until it is in, the per-address limits are off rather than one limit for everybody |

### How the account system is protected

* **Passwords**: scrypt N=2^15 r=8 p=1 (32 MB, ~0.1 s), a random salt each, NFKC-normalised; at most 2 at once and 32
  waiting (then 503), so a login flood cannot exhaust memory (`MemoryMax=600M`) or CPU. 8..128 characters, not the
  login, not one repeated character, not one of the most used passwords. Never logged, never sent back.
* **Guessing**: 15 password checks per address per 15 min, counted *before* the check (40 parallel requests still get
  only 15 checks: tested); 50 logins per account per hour from all addresses; an unknown login costs the same scrypt
  as a wrong password and gets the same answer.
* **Sessions**: 32 random bytes in an `HttpOnly; SameSite=Strict` cookie, `__Host-` prefixed and `Secure` on the public
  site; the account keeps only SHA-256 of it (compared with `timingSafeEqual`); 20 per account; a year unused; a
  password change ends all others; logout ends this one.
* **CSRF**: every change needs the `X-NFS5: 1` header (a page of another site can only send it after a CORS preflight,
  which is never answered) and, when the browser sends one, the site's own `Origin`; JSON bodies only; logout is POST.
  `SameSite=Strict` would stop it alone; these are the second and third locks.
* **Uploads**: at most 960 KB (under nginx's 1 MB); the zip is checked completely before anything is stored: names
  one level deep without `\ / : * ? " < > |` or control characters, no duplicates in any case, stored/deflated only,
  no encryption, every entry inflated to its declared size at most (a zip bomb stops there) and its CRC checked, 64
  files, 1 MB each, 8 MB in all; an empty save is refused. 120 uploads and 300 downloads an hour per account; all
  saves together at most `--cloud-quota-mb` (2 GB), then 507.
* **New accounts**: 5 an hour and 20 a day per address, 500 a day on the server, 100,000 in all. Reserved names
  (admin, server, guest, `*-guest` ...).
* **Memory**: every rate-limit map is pruned each minute and cleared past 50,000 keys; request bodies are capped
  before they are read whole; JSON bodies 4 KB.
* **Logs**: at most 20 lines a minute per address; addresses with the last byte dropped (as nginx's log); names, never
  passwords.
* **Pages**: every server or player text is put in as `textContent`; downloads are blobs the page made.

### Not done / residual risk

* **No password recovery** (the user's decision): a forgotten password is a lost cloud career. The saves stay on the
  devices that played, and *Saves on this device* downloads them. A manual reset by the admin is possible on the
  server (replace `pw` in the account file with a new hash), with no proof of who asks.
* **No second factor, no CAPTCHA**. A botnet with many addresses can still register 20 accounts a day per address,
  bounded by 500 a day and the disk quota. A proof-of-work on registration is the next step if that happens.
* **Saves are not encrypted on the server** (nfsu2-web encrypts with the keyphrase). Whoever gets root on the VPS
  reads the saves and the password hashes (scrypt: slow to crack, but weak passwords fall). Saves are not secret;
  the hashes are what matters.
* **Backups of the server**: the accounts live only on the VPS disk. A disk loss loses every account; a daily copy of
  `/var/lib/nfs5-web/cloud` off the VPS is advised.
* **A player's own save is not checked for cheats**: anyone can upload a modified career for themselves. It reaches
  nobody else (no sharing), and online races do not trust it.
* **The game's file names (found on the way, not fixed here)**: the game names profile files with bytes >= 0x80
  (latin-1); the I/O worker decodes paths as UTF-8, so in the browser every such byte becomes U+FFFD. Two profiles
  whose names have the same length map to the **same** file name, and one overwrites the other's saves (also without
  cloud saves). Cloud saves carry the names exactly as stored, so they neither fix nor worsen it; a downloaded zip
  therefore has U+FFFD names for profiles (fine for restoring in the browser, not usable in the PC game as is).
  Fix: decode/encode paths as latin-1 in `io-worker.ts` plus a one-time rename of the U+FFFD files (their real names
  are in `player.lst` / inside the `.sav`); separate task.
* **Other services on the VPS** (not part of nfs5-web, not changed): `linktest` (UDP 3110, "temporary", running as
  `ubuntu`, a sudo user) should be stopped if no longer needed; the PC game server `nfs5-netserver` (9443 tcp/udp)
  runs as its own user; SSH has passwords off; unattended-upgrades is on; fail2ban is not installed (not needed with
  key-only SSH).

## Tests

```
node web/tools/cloud_test.mjs             # 106 checks, ~40 s: accounts, CSRF, sessions, saves, hostile zips, limits,
                                          # the old crash requests, the public server's cookie/headers, online names
node web/tools/cloud_browser_test.mjs     # ~8 min, headless Chrome + the game: two devices, register/sign in/sync,
                                          # newer-wins both ways, backups, account dialog, downloads, no CSP violation
```

## Deploy

`deploy/install-web.sh` (site: `web/` with `cloud/`, the unit, nginx headers; rolls back when the new service does not
answer) and `deploy/install-online.sh` (online server: `online/*.mjs`, the unit with `--accounts` and `--origin`).
Order: the site first (it opens 9461), then the online server. `deploy/push-web.sh <ssh host>` stages and runs both.
After it: `curl https://<site>/cloud/me` answers `{"user":null,...}`, `web/tools/live_test.mjs` passes.
