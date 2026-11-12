# Features of the PC wrapper: what the browser build takes

Source: `M:\Games\nfspu_nglide\_mod\wrapper\src`. A feature is ported when a person has accepted it in the browser.
Status: **done** = in the build and checked by a script, **next** = the order below, **later** = wanted, not started,
**no** = not needed in the browser.

## Converted High Stakes tracks

| Feature (PC source) | Verdict | How |
|---|---|---|
| Roster, sound archives (`extended_content.cpp`, push repoints) | done | no code: the data layer gives `nfs5ext.trk`, `trackext.viv`, `zzzwzzz_mod.viv` the stock names (`tools/make_mod_layer.py`) |
| Track names (HS8, `H_GetString`) | done | override 004E06D0, `runtime/pu_ext.c` |
| Respawn guard (HS8, `H_SliceFree`) | done | override 0040F480 |
| Walls + object collisions, stock impact (HS16-19, HZ5, HZ7, `hs_corridor.cpp`) | done | override 00498FF0, `runtime/ext/hs_walls.cpp`; the crash recorder is left out |
| AI on converted tracks (AI1, AI2, AI4-6, `ai_tuning.cpp`, stuck reset of `race_trace.cpp`) | done | instruction hooks, `runtime/ext/hs_ai.cpp`; the [ai] values are constants; lap times not compared with the PC yet |
| Animated textures (HZ4, `tex_anim.cpp`, `.tan`) | done | in `r_web.c`, names from an override of 004677D0; tables load, the look is not checked |
| Animated objects, smackables (HZ2-3) | check | data-driven; look in a race whether anything is missing |
| Sound arena overrun (HS15) | done | fixed in the data on the PC (loops start at 0, banks up to 28000 samples), the layer brings them; `audio_guard.cpp` is diagnostics only |
| Sky horizon, sky-only strip (HZ1, SKY1, G9) | check | mostly data; the renderer's cutout code came with dx11z |

## Picture

| Feature | Verdict | How |
|---|---|---|
| Base renderer, fog, cutouts, guard band, UI rects, LOD bias, flares (G1-G11) | done | `r_web.c`, `renderer.ts` |
| Car LOD: LOD1 in races (L1, L3 `car_lod.cpp`) | done | hooks 0043A3B0, 0043C431, override 0043E120, size reference patch; the look is not checked |
| HUD scaling (NE1 `hud_scaling`) | done | 61 size reads patched to a virtual size in patches/exe_v35.txt, mode hooks, layout override 0041E170, font scale around the text loop; the digital-gauge option is left out |
| Draw distance, distant wheels (NE1 `detail`) | done | x2 and at least 20000 as on the PC: override 0044F1A0, far plane, object limits and wheel table in patches/exe_v35.txt; aquatica 2000 draws a frame instead of 450, 70 fps instead of 80 in headless Chrome |
| Text quality (NE1 `text`) | done | runtime/pu_text.c: glyph sheets enlarged 2x or 4x by the window's height with smooth edges, overrides 00571A10, 005722C0, 00572230 and hook 005725A5; `?env=TEXT=0` off |
| Texture detail (NE1 `detail`) | done | detection 0044E370 returns tier 3, patches/exe_v35.txt |
| Adaptive resolution, live window resize | done | the race's mode at the start: the window's real pixels (720-2160 lines), shape 4:3-32:9; afterwards the page draws the game's screen at the window's pixels whatever its size (`renderer.ts` fitTarget, menus too; new targets once the size has held 200 ms), and a window of another shape changes the race's screen mid-race at the same height once it has held 16 simulation steps (r_web.c r_reshape, pu_ext.c: the views' camera rectangles 00538DA0 made again, 00619784/005DEAC8; a camera's record goes with it, 00538D80). The HUD keeps the layout of the race start: each draw of its two passes (00425950, 00425810) moves by the third of the screen it sits on, the views placed from the layout every frame (mirror, map: 00538DA0 from 0046744C) the same way; `?rh=<lines>` caps, `?ws=0` 4:3 |
| Frame interpolation (NE1 `framerate`) | done | runtime/pu_interp.c: the display's rate instead of 64 frames, blended cars and cameras, the simulation never sees them; 100-143 fps in headless Chrome, camera motion unevenness 1.5-3 % instead of 10 %; `?env=INTERP=0` off |
| CPU speed (compat `fast_cpu_speed`) | done | both measuring functions return a constant, patches/exe_v35.txt |
| Car reflection cube (R1, `scene_probe.cpp`) | done | runtime/pu_cube.c at hook 00468AA6: the game's own view, sky, traversal and car pass into one face of a 256 px HDR cube per frame (the PC draws six every second frame), mip chain, emissive gain and sun mask as on the PC; `?env=CUBE=0` off, `?cubelog=1` face stats |
| Car paint for every car (CP1, CAR1-2, `car_paint.cpp`, `car_shading.cpp`) | done | runtime/pu_paint.c: the page builder 00443F60 / tint blit 00444130 give a paint map per car page (R paint, G shine kept), the light layer 00446530 puts camera-space normals into the specular dword; `fs_car` in `renderer.ts` is the PC `mat_shade_car` with the `[car_paint]` values of dx11z.ini (flakes, clear coat, cube reflection, sun glow); the game's own reflection pass skips painted texels. `?env=PAINT=0` off, `?paintdbg=1..6` debug views. In races and menus (not in `fe=0` direct races, which stay 640x480) |
| Showroom cube (FE1, `scene_probe.cpp`) | done | runtime/pu_cube.c at hooks 004D2FEA, 004D3091 and override 004D15E0: one face a menu frame from the car's spot with only that frame's location models; paint in the menus too, with the light from above and `env_hdr_menu`; checked on the Quick Race screen only |
| Track and road materials (MAT, `materials.cpp`) | later | material packs |
| MSAA (AA1) | done | 4x in `web/src/gpu/renderer.ts`, resolved before a present or a pixel read, `?msaa=1` turns it off; the PC A2C ended as the hard cutout the shader already has |
| PBR road reflections (MAT14) | later | after materials |
| Motion blur (MB1-2) | not needed | the user's decision |
| Exact game timer (`enhanced` qpc_timer, SilentPatch) | done | overrides of the timer thread and the one-shot callback in `runtime/pu_ext.c`: 128 Hz by the clock, no lost ticks, a steady 64 simulation steps a second; the race loop waits for the tick instead of spinning; `PERFSTAT=1` logs the frame and loop timings |
| Widescreen (P0-P7, `camera_hooks.cpp`) | done | Hor+ camera, tree footprint and sky extent hooks in `runtime/pu_ext.c` (`pu_cam_*`); the race mode is 960 lines as wide as the window (`NFS5_RACE_W` from `web/src/main.ts`, 4:3 to 32:9, `?ws=0` keeps 4:3); the menus stay 4:3, the canvas element takes the mode's shape |

## Game

| Feature | Verdict | How |
|---|---|---|
| Career additions, club events, bonus garage (NP1 `new_profile.cpp`, race mode part done) | later | hooks + layout files |
| Similar opponents in Quick Race (OS1 `opponent_similar.cpp`) | done | overrides of the accessor, `runtime/ext/opp_similar.cpp`, counts in patches/exe_v35.txt; the police part comes with the pursuit |
| Eight racers (`race_cars8.cpp`) | later | cop cars dropped with the pursuit |
| Hot pursuit (PUR1-14: `cop_ai`, `pursuit_rules`, `pursuit_radio`, `pursuit_fx`, `hot_pursuit_menu`) | dropped | not ported (decision 2026-11-05) |
| Car-car collision fix (PHYS1 `collision_probe.cpp`) | done | normal gate: patches/exe_v35.txt; remote car velocity: override 004980D0; the race and the native IP race run, the feel of a hit is not checked |
| Music after focus loss (MUS1 `music_focus.cpp`) | check | byte patch; test by hiding the tab |
| Gamepad (GP1) | done | Gamepad API, standard mapping only: menus in `web/src/input.ts` (stick = cursor, A click, B/Start Esc, d-pad arrows, right stick wheel), race merged into the packet at 00413800, impact rumble from 0040F740; checked with a scripted pad (`browser_test.mjs --fakepad`), a real pad and the rumble are not |

## Network

| Feature | Verdict | How |
|---|---|---|
| IP race in the runtime (WEB2n) | done (native) | browser: WebSocket relay |
| N1 entry server, race modes, host bots, net pursuit (`net_redirect`, `race_modes`, `net_host_bots`) | later | step 4, the nfsu2-web virtual LAN (docs/PLAN.md); no net pursuit |

## Not needed in the browser

PERF5-8 (driver split locks, device recreation), PERF1-4 and HITCH1 profilers, IN1 DirectInput enumeration, CF1 Windows
compatibility fixes (`compat_fixes.cpp`: check the gameplay ones one by one), MAC1 Wine bundle, all `*_probe.cpp` and
`*_trace.cpp` diagnostics, `net_autotest.cpp` (`runtime/nettest.c` has its job), editors (run on the PC, their output is data).

## Converted tracks checked in Chrome

A 60-100 s AI race without errors: aquatica, dolphin, gt1, hometown, rockypas, snowy, summit, uk. Not run yet (the batch's own
fault, Chrome exit code 21, profile in use): atlantic, country, empire, france, germany, gt2, gt3, hills, lostcany, park, redrock.
Build the layer with `python tools/make_mod_layer.py --pc <PC mod install>`.
