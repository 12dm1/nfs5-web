# Plan

Goal: the finished game in the browser, on a password-protected site modelled on nfsu2.xyz: Porsche Unleashed
(English v3.5) with the NFS5 Reforged features, the converted High Stakes tracks and online races.

The PC mod (`M:\Games\nfspu_nglide\_mod`, wrapper `dx11z`) stays the source of the features and of the
reverse-engineering notes; this repository takes code from it feature by feature.

## Step 1 — the game runs

| Task | Goal | Done when |
|---|---|---|
| WEB1 native boot | lifted C + runtime (Win32, files, threads, guest lock, dispatcher; from nfsu2-web where it is generic) as a Windows x64 program, THRASH stubbed | the frontend's game logic runs to the main menu (log only) |
| WEB2 native picture | the same program with dx11z as its THRASH renderer (D3D11, native) | menus and a race on screen, race_test-style autotest passes |
| WEB2n network smoke test (IP mode PASS, docs/tasks/WEB2n-network.md) | the game's IP mode through the runtime's Winsock | two native instances in one race; N1 server with the online step |
| WEB3 browser (works on one PC, docs/tasks/WEB3-browser.md) | wasi-sdk build; dx11z's renderer on WebGPU (frame records in shared memory, encoder on the page); DirectSound -> AudioWorklet ring; DirectInput 5 -> keyboard/Gamepad API; files via I/O worker + OPFS cache; COOP/COEP server with a password | a race in Chrome |

The native build stays for good (debugging, autotests), as the Mac build does in nfsu2-web.

## Step 2 — our features and the NFS4 tracks

- A feature list of the PC wrapper with a verdict each: port / not needed in the browser (PERF5-8 driver split
  locks, IN1 DirectInput enumeration, CF1 Windows compatibility, device recreation, ...) / later.
- A patch manifest per feature from the run-time patch census (`_mod/web/tools/patch_census.py`, race +
  frontend): call-site redirects and plain byte edits -> `patches/` before lifting, detours -> OVERRIDE /
  HOOK_SITE into runtime C, moved tables -> lift-time symbolic operands, vtable pointers -> host thunks.
- Generated adapters for hooks (arguments from the guest stack, thiscall ecx, eax result) and for the wrapper's
  calls into game code.
- The HS track converter runs on the player's own NFS4 files; its output is a `/mod` data layer next to `/data`.

## Step 3 — finish them

Acceptance per feature in the browser (the PC mod's acceptance list as the checklist).

## Step 4 — online

Decided 2026-11-05: the nfsu2-web way (github.com/th0mk/nfsu2-web, revision in `_mod/web/upstream/REVISION`).
**Started 2026-11-13 on branch `online` (ON1, `docs/tasks/ON1-online.md`)**: the virtual network, the Internet mode
against our NetGather server (`web/online`), the Online button, PvP / PvE races between browsers and the first stage of
career tournaments (server-owned standings) work; the per-player career effects and the deployment are next.

- A virtual LAN: the runtime's Winsock answers the game's sockets with a virtual stack (their `runtime/web/net_lan.c`):
  every browser gets a 10.77.x.x address, UDP / TCP / ICMP go through a mailbox ring in wasm memory (their
  `runtime/web/net_proto.h`) to the page and over one WebSocket to a switch in our web server (their
  `web/server-rs/crates/lan`), which forwards frames by virtual address. TCP over the switch needs no retransmission.
- The game's own code stays unchanged: its IP mode works between browsers as on a LAN, race traffic goes player to
  player through the switch (a star around the game's creator); the server never parses it.
- The Internet mode: our N1 server (`_mod/netserver/server.js`, the NetGather session host) sits on the virtual LAN as
  the EA entry host, as their `crates/online` stands in for EA's servers at 10.77.0.1. PC players over real UDP as a
  later option.
- Hosting behind HTTPS (their `web/deploy`: Docker, Caddy or Cloudflare).
- Not in scope: hot pursuit (dropped 2026-11-05) and the police parts of other features.

## Rules

- Nothing from the game goes into git: no EXE/DLL, no game files, no lifted C, no compiled module, no converted
  tracks. `.gitignore` covers `gen/`, `out/`, `deps/`, `data/`.
- The site, the module and the data are behind the password.
