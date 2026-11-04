# WEB2: native picture, input, sound (Windows x64)

Goal: the lifted PORSCHE.EXE (WEB1) draws its frontend in a D3D11 window through dx11z, reads keyboard and
mouse from that window, and plays sound. Status: picture + mouse/keyboard work; sound not started (see
"Progress").

## Design

- **Renderer = dx11z's own D3D11 core**, compiled into the native program: `renderer/dx11z/` is a copy of
  `_mod/wrapper/src` (our own dx11z wrapper, commit 4ddf325 of the mod repo; no game code): device, draw,
  state, texture, shaders, log. The feature modules (materials, car paint/reflection/mirror, motion blur,
  scene probe, tex anim, perf) are stubbed in `renderer/dx11z/feature_stubs.cpp` for now (they include the
  real headers so the signatures are checked); they come back with the feature port.
- **Glue `renderer/thrash_dx11.cpp`** (C ABI): `r_init/r_restore/r_setvideomode/r_setstate/r_getstate/r_window/
  r_clearwindow/r_flushwindow/r_clip/r_pageflip/r_unlockwindow/r_readrect/r_talloc..r_settexture/r_draw`.
  `runtime/thrash_hle.c` forwards the THRASH entry points to them (weak symbols: `RENDER=0 ./link_native.sh`
  builds the headless program, `NFS5_HEADLESS=1` disables the renderer at run time).
  - THRASH vertices (32 B) and texture handles (slot+1) contain no pointers: the guest's arguments are used
    as they are (`g_mem + esp`). The one guest global the draw code reads (clip codes, 0x69E0D4) goes through
    `R_GuestPtrAt`.
  - Window: own class "nfs5web", 1280x960 windowed, logical 640x480 (`out/dx11z.ini`, written at start;
    `DX11Z_INI`/`DX11Z_OUT`). Alt+F4 / close = exit.
- **Input** (host window -> guest):
  - Window messages: the frontend is driven by WM_MOUSEMOVE/WM_LBUTTON*/WM_CHAR/WM_KEY* through its window
    procedure 0x53ABA0 (bsearch over a 28-entry message table at 0x69E0E0; mouse -> 0x5728B0 clamps to the
    rect at 0x6A6430 and queues an event at 0x6B77A0). `kbd_pump` (called by the runtime's
    GetMessage/PeekMessage) moves the host window's messages into the guest queue; mouse coordinates are
    mapped from the client area to the logical screen; `GetCursorPos` = the last mapped position.
  - DirectInput keyboard: `GetDeviceState` = the window's DIK state, `GetDeviceData` = a buffered event ring
    (for the race; the frontend does not poll DirectInput).
- **Autotest hooks** (env): `R_SHOT=<frame>,...` (readback statistics on stderr + `out/shot_<n>.bmp`),
  `R_KEYS=<frame>:<DIK>,...` (held 4 frames, WM_KEY*/WM_CHAR + DirectInput), `R_CLICK=<frame>:<x>:<y>,...`
  (move 10 frames before, button held 20 frames; 3 frames is too short for the frontend), `R_MOVEONLY=1`,
  `KBDLOG=1` (message path), `IMPCOUNT=1` (every import by call count in the report), `SLEEPLOG=1`
  (SleepEx > 20 ms: sleep vs lock wait), `HOSTSAMPLE=<file>` + `tools/hostsample.sh` (host sampling profiler,
  waits attributed to the first exe frame).

## Findings

- **"6 frames" stall #1 = timer resolution.** winpthreads' `nanosleep` sleeps in whole 15.6 ms ticks unless
  some program raised the system timer resolution (measured: `nanosleep(1 ms)` = 14.9 ms, `Sleep(1)` =
  1.9 ms); every runtime wait loop with 1 ms naps ran up to 15x slow and the frontend took minutes to load.
  `timeBeginPeriod(1)` alone did not help (Windows 11 ignores it for a process without a visible window).
  Fix: `nanosleep` -> high-resolution waitable timer (`runtime/win/sleep_win.c`, via `posix_compat.h`) +
  power-throttling opt-out + `timeBeginPeriod(1)` in `main_win.c`. 1.3 ms per 1 ms nap afterwards.
- **"6 frames" stall #2 (rare) = critical-section starvation.** Main thread loops Leave/Enter on a section
  the sound thread (tid 102) waits for; the host mutex was re-taken by `trylock` before the woken waiter ran.
  Fix: the last Leave of a section with waiters releases the GIL until a waiter took it (max 2 ms,
  `cs_enter/cs_leave` in `win32_ext.c`). 0 stalls in 22 runs after it (before ~1 in 8); the thread report
  now lists sections that are held or waited for, should it come back.
- PACODE's CPU-MHz measurement (sub_00555FA0, 5 x 0.2 s QPC busy wait) runs ~7 times at start: ~7 s of
  start-up. Not changed yet (candidate: short-circuit).
- Enter/LeaveCriticalSection are served by import index (`g_fast` in `win32.c`): 23 M -> 44 k strcmp chains.
- The frontend's first screen with a fresh profile is `selectname.lay` (Create 245,344 / Remove 245,368 /
  Continue 245,392); `createplayer.lay` has the name box at 277,162 and OK at 245,392. A player created by
  script: `R_CLICK=900:300:350,1300:300:166,1800:300:398` + `R_KEYS` -> `SaveData/<name>.sav`.
- Open: scripted typing loses characters (A..F typed 10 frames apart -> name "8EF"; the leading "8" also
  appears in the real install's profiles); check with real typing before digging.
- `nfs5_clean` has no `FeData/movies` (the movies are on the CD): the main menu oval stays without video.

## Progress

- 2026-11-04: renderer links and runs (D3D11 window, frontend picture: ~87 % non-black frames, menus
  animate, ~150 fps unthrottled); both "6 frames" stalls fixed; mouse + keyboard reach the frontend (button
  hover, clicks, player creation). Next: sound (DirectSound -> WASAPI/XAudio2), then WEB2n.
