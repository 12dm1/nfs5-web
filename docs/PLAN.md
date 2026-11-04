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
| WEB3 browser | wasi-sdk build; dx11z's renderer on WebGPU (frame records in shared memory, encoder on the page); DirectSound -> AudioWorklet ring; DirectInput 5 -> keyboard/Gamepad API; files via I/O worker + OPFS cache; COOP/COEP server with a password | a race in Chrome |

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

The game's IP mode over a WebSocket bridge to the N1 server (lobby, PvP / PvE / pursuit modes), browser and PC
players in one lobby.

## Rules

- Nothing from the game goes into git: no EXE/DLL, no game files, no lifted C, no compiled module, no converted
  tracks. `.gitignore` covers `gen/`, `out/`, `deps/`, `data/`.
- The site, the module and the data are behind the password.
