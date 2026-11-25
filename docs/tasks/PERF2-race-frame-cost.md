# PERF2: where the race's frame time goes (desktop)

Opened 2026-11-25. Branch `release` (worktree `nfs5-web-release`). In progress (round 1 done 2026-11-25, see Results).

## Results, round 1 (2026-11-25, headless, no user runs)

Bench: Quick Race Côte d'Azur (Coastal; `NETTEST_PICK` matches "azur"), 8 cars, AI drives the player, canvas 3840x1906, `PERFSTAT=1` work ms a frame over 25 s from
4 s after the race's start (`web/tools/perf_fps_pcores.sh <tag> [env]`, pinned to the P-cores; the numbers below are
game-thread work per frame).

| change | P-cores | E-cores |
|---|---|---|
| before (all cars drew the garage model) | ~8.7-9.2 ms | ~16.8 ms |
| garage model only within 25 m of the camera (`CARLOD_NEAR`, 0 = old) | ~7.7 | ~13.3 |
| + cube faces draw cars 2 tiers coarser (`CUBE_CARLOD`, face 1.35 -> 1.1 ms) | ~7.7 | |
| + THRASH calls: futex lock instead of the pthread mutex, clock check every 8th call, settexture fast path | ~6.9 | |
| normals into the specular dword (`sub_00446530` hook) as one 3x3, no double normalisation | own cost 1.7x -> 0.1x the game's light layer | |
| + lifter `X87OPT=1` (gen.sh default now: x87 stack slots as constant-index locals within a block) | ~6.1-6.3 (A/B 3 rounds: 6.6 -> 6.1) | |

`X87OPT` was checked with AI races (3 laps Côte d'Azur, 8 cars, `NETTEST_TRACE=1`: every racer's progress / speed /
finish state every 10 s): 3 races each build, mean progress at 110 s 3112-3240 (old) vs 3216-3278 (X87OPT), no
traps; the race is not deterministic run to run, so this is a sanity check, not a bit-exact one. **Every worktree has to
regenerate its lifted C** (`./gen.sh`, or `X87OPT=1 GENOUT=gen/all python tools/genall.py ...` with gen.sh's
environment) to get it; `web/build.sh` takes `GEN=<dir> WEBOUT=<dir>` for side-by-side builds.

Findings:

- **The profiler doubles the frame cost** at a 250 us interval: use `--interval 2000` (prof_race.mjs) or the fps-only
  scripts for A/B; the profile's shares stay right.
- **P-cores vs E-cores (i7-14700KF)**: the same race is ~2.4x slower on an E-core (60 fps vs 140). The user's runs
  (a cube face 3.1-3.8 ms vs 1.1-1.6 headless) match the game thread on an E-core. Unpinned runs flip between both
  as the machine's load changes, so A/B runs are pinned.
- All cars drew the garage model at every distance (the game's top race tier holds to 120 m+): ~2 ms a frame.
- Guest lock: the main thread waits ~6 % of the time, mostly for the sound thread's own mixing (serialised by the
  lock); hand-off overhead ~1.5 %; `GIL_HOLD_MS` (hold the lock longer) gave nothing and was removed.
- What is left (2000 us profile, P-cores): cars `0043C830` 43 % (EAGL: `00543AE0` 7 % self, `00489CC0` 5 %,
  `005B0730` 5 % per-vertex transform + projection with a call per vertex, `00449390` 4 %, light layer `00446530_orig`
  4 %), cube 13 %, our draw path (`draw_any`, `flush_verts`, `submit`, `cs_apply`) 8 %, guest lock 8 %.
  Next: C versions of the EAGL kernels (verified against the lifted code on the same input), then our draw path.

## The problem

A 2000 game on an RTX 3060 runs at 37-70 fps in a race in the browser (the user's PC, 2026-11-25), where it should
hold the display's rate (147 Hz) with room to spare. Custom mid-poly car models make it much worse (the user: "huge
stalls"), so the cost grows with the vertex count, not only with the track.

**The stalls with mid-poly models are the original game's own**: the user sees them in the stock PC game too (no
wrapper, no browser). So the root is in the game's code (its per-vertex CPU work on cars), which the browser port
inherits and makes slower (lifted x86 in wasm); our own additions only add to it.

The user's run (local dev server, `?log=1`, one cube face a frame):

- canvas 3840x1906, display ~147 Hz, race 37-70 fps (`[fps]` every 2 s), 800-2100 draws and 25-125 k vertices a
  `[fps]` segment, 1460 textures;
- `[perf page]` js per frame 0.8-1.4 ms on the page's thread, longest submit 0.1 ms, 4-120 gaps over 25 ms per 5 s;
- `[interp]` 64 simulation steps a second, render delay 1-4 steps.

The headless test PC (640x480, Coastal, AI drives, 8 cars) holds 144 fps with 3.5-4 ms of game work a frame, a
cube face ~1 ms of game-thread time.

## What is known (PERF-M1, phones, 2026-11-24)

Measured on a desktop core with a phone's viewport, Coastal 8 cars, `PERFSTAT=1` (`[perf game]` work ms/frame):
~12.5 ms a frame; the simulation 0.23 ms a step; the car draw `0043C830` ~63 %, the cube ~18 %, the guest lock
wait ~10 %. Knobs: `CARLOD=0` -4.7 ms, `DRAW_X=1` -6.2 ms, `CUBE=0` -5.5 ms, all three 3.6 ms
(`web/src/quality.ts` profile `low`).

So the cars are most of it, and the game does their transform, lighting and clipping on the CPU (EAGL: the light
layer `00446530`, the clipper appending corners past the model's vertices), all of it lifted x86 running as wasm.

## Suspects to measure first

1. **The game's CPU T&L on cars** (`0043C830` and below): per-vertex transform / light / clip in the game's own code,
   which stalls on mid-poly models even in the stock PC game. Find the hot functions (a sampling profile of the game
   worker; count calls and vertices per function), then decide: a C override of the hot loop (the per-vertex math
   written for wasm SIMD), or let the GPU do it (send the model-space vertices and the matrix instead of the game's
   screen-space ones).
2. **Our own per-vertex work on car draws** (runtime/web/r_web.c): `car_clip_normals` looks up every clipped corner
   in all the model's vertices (`for j < orig`), O(clipped x vertices) per draw: harmless at 186 vertices, quadratic
   on a mid-poly model (it adds to the game's own cost, it is not the root). `pv_same`, `cs_page` / `cs_apply`, the `g_pv_uv` / `g_pv_xy` copies, `conv` per vertex,
   `glass_log` / `GDUMP` checks per draw. pu_paint.c `sub_00446530` (the normals into the specular dword) per vertex.
3. **Draw-call and state cost**: one `C_DRAW` per game primitive batch; `emit_state` builds a 64-float uniform block
   and compares it on every draw; `paint_uniform` reads the camera on every painted draw.
4. **GPU at 4K**: `fs_car` (flakes, clear coat, cube), MSAA 4x, the glass pass. Compare the same race at 640x480
   and at 3840x1906: if the frame time does not change, the GPU is not the limit.
5. **The cube**: a face is the whole scene's traversal once more (`pu_cube.c`); ~1 ms a face on the test PC.

## How to measure

- `?log=1&env=PERFSTAT=1` on the local dev server: the game's log goes to the server's stdout (not the browser's
  console): `[perf game]` work / wait segment / wait present per frame every 5 s; `[perf page]` the page's side.
- `?env=CUBELOG=1` logs fps, the present wait and a cube face's cost every 2 s.
- Knobs for A/B: `CUBE=0`, `CARLOD=0`, `CARLOD_NEAR=<m>`, `CUBE_CARLOD=<tiers>`, `DRAW_X=1`, `PAINT=0`, `GLASS=0`,
  `?msaa=1` (off), `CUBE_FACES=<n>`; diagnostics `LODLOG=1` (car LOD index / distance), `GILREP=1` (guest lock per
  thread every 1500 frames).
- Headless tools: `web/tools/perf_fps.sh` (fps only), `perf_fps_pcores.sh` / `perf_fps_ecores.sh` (pinned),
  `perf_prof.sh` (sampling profile, `IV=2000 FOCUS=fn CALLERS=fn`), all on `web/tools/prof_race.mjs`
  (`--at <log regex> --delay s`: the window from the race's start; `--noprof`).
- Chrome DevTools Performance on the game worker (wasm function names, if the build keeps a name section) for the
  hot functions; or a sampling counter in the runtime.
- Test on one or two tracks only, with a mid-poly custom car and with stock cars, menu-flow race (fe=0 races stay
  640x480 and have no paint).

## Related

- `CUBE2` (2026-11-25, commits 9722369..): the cube's faces a frame by the game thread's spare time, OFF by default
  (one face a frame as in production): `?env=CUBE_ADAPT=1` turns it on. Turn it back on once the frame cost is fixed:
  with spare time it refreshes the cube several times as often.
- PERF1-8 of the PC wrapper (memory: PERF1 profiler, split-lock fixes) were about the DX11 driver, not this.
