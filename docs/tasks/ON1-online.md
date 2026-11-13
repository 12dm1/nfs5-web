# ON1: Online mode (instead of network play)

Goal (task of 2026-11-13): the main menu's network play becomes **Online**: one click to a lobby with the players and
their games, races of several modes (quick race with or without bots, career tournaments: track after track, points,
prizes, the placing moves the career on). Reference: github.com/th0mk/nfsu2-web (virtual LAN over a WebSocket).
Decisions of the user: the lobby stays inside the game's menus; IP/LAN screens hidden (code kept); bots fill the
free grid slots of a tournament, a player who leaves gets zeros; work on branch `online` (worktree
`M:/Games/nfs5-web-online`), commits there only.

Research: `docs/research/ON1-nfsu2-web-network.md` (how nfsu2-web does it), `docs/research/ON2-career-tournaments.md`
(the game's tournament code and data, server vs local career).

## Status

| Part | State |
|---|---|
| Browser network stack (Winsock on a virtual network, WebSocket switch) | works |
| The game's Internet mode against our own NetGather server, in the browser | works: login, server ping, lobby, chat, games |
| **Online** button (main menu) -> lobby | works: about 7 s, the login / server screens flash by |
| PvP race between two browsers | **PASS** (`online_test.mjs --mode 0 --env NETTEST_UI=1`, 288cf63) |
| Race modes in the create screen: Quick race / Quick race + bots / Career tournament | in the MODE combo; PvE with 2 browsers + 6 bots **PASS** |
| Career tournament, server side | event from `nfs5.trn`, its opponents as bots, track + laps sent to the host, points / money / standings in the game's chat; the whole 2-race event **PASS** (709a94a) |
| Career tournament, local career effects | stage 2: points, race prize, bonus, final prize, event progress / year / era by the stock code, profile saved; **PASS** in the 2-race test (cash seen growing) |
| Online server without the game files (`web/online/standalone.mjs`), page `?lan=wss://host/lan` | smoke PASS locally and public |
| Deployment to nfs5-reforged.duckdns.org | **live** since 2026-11-13: `deploy/` (service + nginx `/lan`); PvE race of 2 headless browsers through `wss://nfs5-reforged.duckdns.org/lan` **PASS** (seen in the server's log) |

## How it fits together

```
game (wasm)                         page                       server (web/server.mjs)
 Winsock calls                       web/src/net/lan.ts          /lan WebSocket -> web/online/vnet.mjs (switch)
 runtime/web/winsock_web.c  <-ring-> one WebSocket   <-frames->    10.77.1.x per tab, ICMP echo answered
   UDP / ICMP.DLL echo                                             UDP :9442 / 10.77.0.1 -> netgather.mjs (N1 server)
   control line UDP :9443  ............................................ :9443 -> netgather control() (tournaments)
```

- `runtime/web/winsock_web.c`: Winsock 1.1 for the game on a mailbox ring laid out as nfsu2-web's `net_proto.h`
  (TX 128 / RX 256 slots of 1536 B). TCP connect is refused (the game only needs UDP); `gethostbyname` gives the own
  name the tab's address and every other name 10.77.0.1. `ICMP.DLL` (IcmpCreateFile / IcmpSendEcho) for the server
  list's ping.
- `web/online/netgather.mjs`: `_mod/netserver/server.js` (the PC mod's N1 entry + session host) without its UDP
  socket, so the same code serves browsers (through the switch) and PC players (`--udp <port>`).
- Registry `Primary Address` reads `10.77.0.1.9442` in the browser (`win32_ext.c`): the login (`0048AC90`) resolves only
  the numeric `a.b.c.d.port` form; with a host name the join went to a stack-garbage address.
- The Castanet update check is skipped (`patches/exe_v35.txt` 004A56EA).

### The Online button

`MULTIPLAYER` (string 12403) reads "online"; its command `SETTYPEMULTIPLAYER` (00510B10, override in `nettest.c`) arms
`online_auto`, which runs in the frontend frame: `SETMODENET` + `accountlogin`, account = `NFS5_ACCOUNT` or a random
`webNNNNNN`, password `x`, `SIGNON`, wait for status 0x0E, the server row (after its ping), `ENTER` + `udpip`. Back from
the lobby (server list again) it goes `GOBACK` to the main menu. The persona shown in the lobby is the profile name.

### Race modes and bots (PC mod N1 / NET1 ports)

- MODE combo (setup record 0x14) = 0 Quick race, 1 Quick race + bots, 2 Career tournament: row table at 006C1F40
  (texts by the 004E06D0 override), 20 reads of COMMSPOSITIONAL moved to 00657404 and the car-state send gate
  (`patches/exe_v35.txt`, from `race_modes.cpp`).
- `runtime/pu_net.c`: the game's creator sends the car state (BE) of every car nobody feeds for 1.5 s (the server's bots,
  driven by its AI); the receive side uses the full car array (0048F665); the car the server announced for a bot slot
  is kept over the network race build (004B4B80 records, 0048D6A0 at 004B5051).

### Career tournaments (stage 1, server-owned)

`web/online/tournament.mjs` reads `nfs5.trn` (35 events, ON2 section 1). Online events: visible, without traffic
(network races set none), known tracks: 27 of 35. MODE = career on the create screen:

1. the server picks the event (`#<id>` in the game name, else #1 "Classic era: Normandie, Cote d'Azur"), fills the grid
   with bots that drive the event's opponent cars;
2. it sends the creator `race k/N laps=L reverse=R track=<name>`; the creator's runtime (`nettest.c career_tick`) sets
   `Track1.Track` / `Track1.Laps` on the create screen (the joiners get it from the host as for any change);
3. after the race the creator reports `result slot:place:time ...` (005E77A0 + car index * 258h, car index -> slot from
   the member list);
4. points = 9 - place, race prize + win bonus (x0.5 2nd, x0.25 3rd), final prize per overall place, order by points,
   money, time (rally events: time) as 00503660; race and table lines go to the game's chat; then the next race.

A player who leaves keeps the points so far and gets nothing more; a bot takes the free slot.

## Stage 2: the career of each player

After each race the server sends every human `career id=<event> race=<r> of=<n> fee=<fee> results p:t p:t ...` in that
player's own numbering (entry 0 = the player, the others by slot; a car that did not finish gets the places after the
finishers). `runtime/nettest.c career_apply` runs the stock pieces of 004EDF00 type 4 (ON2 4.1 route B) on the profile's
tournament table (`00691E58`):

- race 1: `00503E40(id)` clears the event's old results, the entry fee is paid (`005012E0(-fee)`; not enough cash = not
  applied);
- `005DB768/76C` = race / event, `00503960(entry, time, place, 1)` for every entry (points 9 - place, prize, bonus),
  cash += `00505740()` + `005057A0()`; after the last race (`005056D0() == 0`) also the final prize
  `00505770(00505710())`;
- `00503A50`, `00503AD0(id)`: next race, or the event done: club +1, era event on the podium: year step, all era events
  done = the next era;
- the offline career's current event / race (`00691E6C/7C`) is put back unless the era moved; the profile is saved
  (`004DEDA0(00501270())`, "Saving for ...").

Rules of the prototype: only events of a club or of an era the profile has reached (`+004 <= 00691E50`); only when the
client had the event from its first race. Not done yet: a separate online profile (ON2 recommends one, so single-player
careers are never mixed with online results), the owned-car check (any car may start), damage / mileage of the owned
car, the prize-car race after an era, server-side checks.

## Known problems / workarounds

- **A network race's HUD asks for a font with no name** (`.\gamedata\dashhud\` + ""), the DTEX driver reports "Invalid
  shape type" (fontdtex.c line 300) in a "terminate execution?" box. The HLE answers No (`win32.c`) and the race runs.
  Which HUD element it is: not found yet (the name is not in the EXE; the HUD files `hud.psh` / `font.pfn`).
- The driver picture: the loading screen of a network race draws every player's `pic16.fsh` (file sharing); without
  it the loop at 004A51C0 ran on garbage. `gimme.dll!ConvertFile` is now ported from the PC CF1 (`pu_imports.c`).
- The games list's MODE column read "THIS IS A BUG" (record +0x35 = the old positional value): now the race mode.
- A network race never ends by itself (00417010): every player leaves through the race menu (the tests do it 5 s
  after the finish, `nettest_race`). A real player needs to know that: open point for the design.
- Old test hosts: `nettest.c` counted bots as players; it waits for the server's `humans N` line now.

## Deployment

The online server needs no game files from the player, only `nfs5.car` (bot cars) and `nfs5.trn` (tournaments), copied
to the host by the deploy (never into git). On the VPS of nfs5-reforged.duckdns.org (SSH alias `louditude`):

- `deploy/nfs5-web-online.service`: `node /opt/nfs5-web-online/online/standalone.mjs --port 9450` on the loopback, logs
  in `/var/lib/nfs5-web-online`; the PC game server (`nfs5-netserver`, UDP / TCP 9443) is a separate service, untouched;
- `deploy/nginx-lan.conf`: `location = /lan` (WebSocket proxy) and `/lan-status` (JSON: players, games, tournaments),
  inserted by `deploy/install-online.sh` into the live `nfs5-reforged.conf`. **The nfs5-reforged repo's own copy of that
  file does not have it**: re-running its `setup-web.sh` drops `/lan` until `install-online.sh` runs again;
- upload: `web/online/*.mjs`, the two data files, the service and the snippet to `/tmp/nfs5-web-online`, then
  `bash install-online.sh` there.

A player: `run_web_online.bat` (= `run_web.bat` with `?lan=wss://nfs5-reforged.duckdns.org/lan`), main menu -> Online.
The page code (`web/src`) is bundled by `node web/build.mjs` (run_web*.bat do it; `web/build.sh` builds only the wasm):
a test run after a page change without it runs the old bundle (the first `--lan` runs went to the local server).

## Tests

```
node web/tools/online_smoke.mjs ws://127.0.0.1:8078/lan     # switch + NetGather without a game
node web/tools/online_test.mjs --mode 0 --env NETTEST_UI=1  # 2 browsers, Online button, PvP race -> PASS
node web/tools/online_test.mjs --mode 1 --env NETTEST_UI=1  # PvE: 2 players + 6 bots
node web/tools/online_test.mjs --mode 2 --env NETTEST_UI=1 --seconds 1200  # career: the whole event (2 races) + career_apply
node web/online/standalone.mjs --port 9450 --data M:/Games/nfs5_clean      # the online server alone
node web/tools/online_test.mjs ... --lan ws://127.0.0.1:9450/lan             # the games against it
```
Every game's log lines carry its tag (`[H]`, `[J1]`); the server's log: `out/online/online-*.log`.
