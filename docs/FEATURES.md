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
| Text quality, texture detail, frame interpolation (NE1 `text`, `detail`, `framerate`) | later | shader, hooks |
| Car reflection cube (R1, R2, `car_reflection.cpp`), car paint (CAR1-2, CP1, `car_paint.cpp`, `car_shading.cpp`), materials (MAT, `materials.cpp`) | later | a large port: render passes into cube faces, WGSL version of the material shader |
| MSAA (AA1) | done | 4x in `web/src/gpu/renderer.ts`, resolved before a present or a pixel read, `?msaa=1` turns it off; the PC A2C ended as the hard cutout the shader already has |
| Motion blur (MB1-2), PBR road reflections (MAT14) | later | after the cube and materials |
| Widescreen (P0-P7, `camera_hooks.cpp`, `scene_probe.cpp`) | later | camera patches; the canvas is 4:3 until then |

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
