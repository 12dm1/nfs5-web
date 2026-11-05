# WEB3: the game in the browser

Goal: menus, sound and a race in Chrome, from the same lifted code as the native build.
Status: **works on one PC** (`run_web.bat`, http://127.0.0.1:8077/). Checked in headless Chrome on an nvidia card by
scripts, except what is listed under "Not checked".

## How it is built

- `web/build.sh`: wasi-sdk (`wasm32-wasip1-threads`), `out/web/nfs5.wasm` (18 MB) + `nfs5.json`. `g_mem` is the macro 0, a
  guest address is a linear address; the guest owns `[0, 0x28000000)`, the runtime sits above it.
- Page host from nfsu2-web (`web/src/host/`): game threads as workers on one shared memory, an I/O worker for files (blocks
  over HTTP with an OPFS cache, saves in OPFS). `nfsu2_register(kind, ptr, size)` hands the mailboxes to the page.
- `runtime/web/r_web.c` + `web/src/gpu/renderer.ts`: dx11z's base path in C, draws go through a ring of 3 segments (commands,
  48-byte vertices, uploads) to WebGPU. Uploads run before a segment's draws; using a texture that is updated closes the
  segment. Offscreen rgba8 + depth24plus at 1280x960, blitted on present; one present per display frame, a timer drains
  upload-only segments (and everything in a hidden tab).
- `runtime/web/input_web.c` + `web/src/input.ts`: keyboard and mouse mailbox (DIK table, window messages).
- `runtime/web/audio_web.c` + `web/src/audio/worklet.ts`: 48 kHz float ring, target 50 ms; starts on the first click or key.

## Findings

- A directory `open()` must fail under WASI as on Windows, else `fe=0` crashes in `sub_0056e2a8` (`.\gamedata\dashhud\`).
- `fe=0` on a fresh save: "FONT - Invalid shape type" = no HUD files; tests seed them (`seed=XXhud1XX.hud,XXhud2XX.hud`).
  The normal flow creates them with the player.
- The race's video mode (`00467FC0` at 0x41562E and 0x4B6CB9, hook `race_mode` in `runtime/pu_hooks.c`) is forced to
  1280x960x32; a new profile asks for 640x480x16, where the LOD selector picks low-poly cars close by (PC mod NP1). The
  frontend is 32 bits too, so the race flag is `bpp == 32 && w > 640`.
- PACODE's CPU speed measure takes 7 s at start: `00555FA0` returns 600000000 at once (`patches/exe_v35.txt`).
- Lens flares (`Graphics.LensFlare`, 0x657D7C, off in a new profile) read 4x4 pixels under the sun, once for the reference,
  then every 200 ms. `r_readrect`: the first read (or the first after 1 s without one) waits for the GPU (4-8 ms), the others
  return the answer to the previous read, so the flare reacts 200 ms late and the game does not stall. Checked by forcing
  the variable: the values follow the sun's position and go dark when something covers it.
- The test's own `timeout` looks like a crash: Playwright reports "Target crashed" and Crashpad writes one dump per process,
  all at the same address (`chrome.dll+0x5e69950`). Leave the script enough time before suspecting the game.
- `nfs5_clean` has no `GameData/Music` (an optional install part); the server fills it from `out/save/gamedata/music`
  (`--music`).

## Checked

- Frontend at the display rate (144 fps), the text crisp, the menu button crop nearly equal to the native frame.
- Input: mouse positions reach the guest; typing + clicks create a player (`player.lst`, `8Bob.sav`, `Bob1.hud`, `Bob2.hud`
  in OPFS), and it is there on the next start.
- Menus by mouse: Continue, Single Player, Quick Race, Play, the race at 1280x960x32, ~80 fps.
- Keyboard: with ArrowUp held the frames change (mean difference 13 of 255 over 2 s), without it the car stands (0.6).
- Exit, Yes: `ExitProcess(0)`, the page shows "The game has ended".
- Sound: the ring advances at 48000 frames/s, the menu music and the race have energy in the ring (rms 0.12-0.16 in the menu).

## Not checked

- By a person: how it looks, how it sounds, how it drives. All tests are muted and look at numbers and ASCII frames.
- The end of a race and the way back to the menu.
- Other GPUs and browsers (Firefox and Safari WebGPU), another PC over the network (needs `--password`; publishing the game's
  files anywhere is the user's decision).
- Why the race is ~80 fps when the frontend reaches 144. It is above 60, so not urgent.

## Later

Winsock in the browser (weak stubs now, a WebSocket relay with the online step), Gamepad API (with the feature port of GP1).
