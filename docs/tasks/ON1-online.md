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
| Race modes in the create screen: Quick race / Quick race + bots / Career tournament | in the MODE combo; PvE bots drive (7 bots + host seen racing), two-player PvE test to redo |
| Career tournament, server side | event from `nfs5.trn`, its opponents as bots, track + laps sent to the host, points / money / standings in the game's chat; first test running |
| Career tournament, local career effects (cash, year, era, prize cars) | not started (stage 2, below) |
| Deployment to nfs5-reforged.duckdns.org | not started |

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

## Stage 2: the career of each player (not started)

ON2 section 4 recommends a hybrid: a separate online profile (a normal `.sav`, master copy on the server), the stock
code applies the server's results. Per client: `DAT_00691E6C` = the event, 004EDE20 hook so the race counts as type 4,
the results array reordered so the local car is entry 0, `005DB768/76C` set (`00503940`); then 004EDF00 / 00503AD0 pay
the prizes, count the podium, move the year and era. Open points from ON2: remote use of 0x14B parts, 0x247 starting
grid for other slots, the tournament result screen in a type-3 race.

## Known problems / workarounds

- **A network race's HUD asks for a font with no name** (`.\gamedata\dashhud\` + ""), the DTEX driver reports "Invalid
  shape type" (fontdtex.c line 300) in a "terminate execution?" box. The HLE answers No (`win32.c`) and the race runs.
  Which HUD element it is: not found yet (the name is not in the EXE; the HUD files `hud.psh` / `font.pfn`).
- The driver picture: the loading screen of a network race draws every player's `pic16.fsh` (file sharing); without
  it the loop at 004A51C0 ran on garbage. `gimme.dll!ConvertFile` is now ported from the PC CF1 (`pu_imports.c`).
- The game name shows as "THIS IS A BUG" in the games list (a string id the list asks for); cosmetic, open.
- Old test hosts: `nettest.c` counted bots as players; it waits for the server's `humans N` line now.

## Tests

```
node web/tools/online_smoke.mjs ws://127.0.0.1:8078/lan     # switch + NetGather without a game
node web/tools/online_test.mjs --mode 0 --env NETTEST_UI=1  # 2 browsers, Online button, PvP race -> PASS
node web/tools/online_test.mjs --mode 1 --env NETTEST_UI=1  # PvE: 2 players + 6 bots
node web/tools/online_test.mjs --mode 2 --env NETTEST_UI=1 --seconds 960   # career: the whole event (2 races)
```
Every game's log lines carry its tag (`[H]`, `[J1]`); the server's log: `out/online/online-*.log`.
