# PERF2: where the race's frame time goes (desktop)

Opened 2026-11-25. Branch `release` (worktree `nfs5-web-release`). Not started.

## The problem

A 2000 game on an RTX 3060 runs at 37-70 fps in a race in the browser (the user's PC, 2026-11-25), where it should
hold the display's rate (147 Hz) with room to spare. Custom mid-poly car models make it much worse (the user: "huge
stalls"), so the cost grows with the vertex count, not only with the track.

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

1. **Our own per-vertex work on car draws** (runtime/web/r_web.c): `car_clip_normals` looks up every clipped corner
   in all the model's vertices (`for j < orig`), O(clipped x vertices) per draw: harmless at 186 vertices, quadratic
   on a mid-poly model. `pv_same`, `cs_page` / `cs_apply`, the `g_pv_uv` / `g_pv_xy` copies, `conv` per vertex,
   `glass_log` / `GDUMP` checks per draw. pu_paint.c `sub_00446530` (the normals into the specular dword) per vertex.
2. **The game's CPU T&L** on cars (`0043C830` and below): per-vertex transform/light/clip in lifted code. Find the
   hot functions (a sampling profile of the game worker), then decide: an override in C, or let the GPU do it (send
   the model-space vertices and the matrix instead of the game's screen-space ones).
3. **Draw-call and state cost**: one `C_DRAW` per game primitive batch; `emit_state` builds a 64-float uniform block
   and compares it on every draw; `paint_uniform` reads the camera on every painted draw.
4. **GPU at 4K**: `fs_car` (flakes, clear coat, cube), MSAA 4x, the glass pass. Compare the same race at 640x480
   and at 3840x1906: if the frame time does not change, the GPU is not the limit.
5. **The cube**: a face is the whole scene's traversal once more (`pu_cube.c`); ~1 ms a face on the test PC.

## How to measure

- `?log=1&env=PERFSTAT=1` on the local dev server: the game's log goes to the server's stdout (not the browser's
  console): `[perf game]` work / wait segment / wait present per frame every 5 s; `[perf page]` the page's side.
- `?env=CUBELOG=1` logs fps, the present wait and a cube face's cost every 2 s.
- Knobs for A/B: `CUBE=0`, `CARLOD=0`, `DRAW_X=1`, `PAINT=0`, `GLASS=0`, `?msaa=1` (off), `CUBE_FACES=<n>`.
- Chrome DevTools Performance on the game worker (wasm function names, if the build keeps a name section) for the
  hot functions; or a sampling counter in the runtime.
- Test on one or two tracks only, with a mid-poly custom car and with stock cars, menu-flow race (fe=0 races stay
  640x480 and have no paint).

## Related

- `CUBE2` (2026-11-25, commits 9722369..): the cube's faces a frame by the game thread's spare time, OFF by default
  (one face a frame as in production): `?env=CUBE_ADAPT=1` turns it on. Turn it back on once the frame cost is fixed:
  with spare time it refreshes the cube several times as often.
- PERF1-8 of the PC wrapper (memory: PERF1 profiler, split-lock fixes) were about the DX11 driver, not this.
