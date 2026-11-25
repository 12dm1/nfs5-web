# GLASS1: car glass reflections, race camera saved, no Resolution option

Branch `glass1` (from `release` c817179), 2026-11-25. Porsche Unleashed v3.5, English. Not merged, not deployed.

| Part | Status | Commit |
|---|---|---|
| Tools: `web/tools/race_look.mjs` (headless menu-flow race, `--react` keys/shots), `web/tools/shot_mask.py` | done | 761fab6 |
| CAM1: the race camera picked with the camera key is saved to the profile | done, checked | fb48354 |
| GLASS1: intact car glass gets the R1 cube reflection | WIP: works on the glass pages; many race windows are not on them | 2d3993c |
| RES1: the graphics options lose the Resolution row | WIP: written, not checked in the game | 412d496 |

## CAM1: race camera to the profile (runtime/pu_ext.c, runtime/pu_hooks.c)

The camera key cycles through the options' four camera slots (`Camera.1..4`, cameras.lay), and a race starts with slot 1.
When the player presses the camera key in a race (view 0, inside the race loop, not in a replay), the camera they end up
on is remembered. On the frontend's first tick after the race (hook 0x4D2B80), the cycle is rotated so that camera
becomes `Camera.1`, with the order kept. The values are written through the accessors' own set (vfn 2Ch), and the
profile is saved the game's way (00510AF0, "Saving for %s" -> `<n><name>.sav`). The cloud sync then sends that save like
any other. In a local race `carInfo[0].CAMERA` also follows the pick, so a restart keeps it; a network race does not
change it. `CAMERA_KEEP=0` turns CAM1 off, `CAMLOG=1` logs.

How it was checked: Quick Race on Redrock. The race started on camera 0, the key went to 2, and the log said `picked in the
race: camera 2; the profile's cameras 0,1,2,3 -> 2,3,0,1, profile saved`. A new browser session on the same profile then
logged `race starts with camera 2 (carInfo 2)`.

Risk: if the tab is closed before the player gets back to the menus, the pick is lost.

## GLASS1: glass reflections (runtime/pu_paint.c, runtime/web/r_web.c, web/src/gpu/renderer.ts)

- pu_paint.c builds a glass map for the glass pages (builder files 3 `glass` and 4 `intglass`). It keeps the
  see-through texels of each blit and matches them at the upload by content, frame included. A broken-glass image
  drawn over a window later no longer matches, so it keeps the stock look.
- r_web.c draws a glass page as the game does. It then queues a second pass, `fs_glass`: premultiplied (the reflection
  plus (1 - F) of what is under it), depth tested, with no depth write. The pass is drawn at the end of the view (clear,
  clip change, target change, HUD, 2D draw or frame end), so the interior the car draws after its windows still ends up
  under the glass.
- `fs_mask` keeps the game's own sphere-map pass off intact glass.
- Glass look: roughness 0.02, Schlick F0 0.04, the cube's env reflection plus the sun's spot, fogged.
- Switches: `GLASS=0` turns it off, `GLASSLOG=1` logs, `?glassdbg=1` shows intact glass in magenta, `=2` the
  reflection alone, `=3` every pixel of the pass.

Results:

| Check | GLASS=1 | GLASS=0 |
|---|---|---|
| Showroom car, menu shot: magenta (glass) pixels | 187363 | n/a |
| Showroom glass: mean luminance | 0.331 | 0.309 |
| Showroom glass: p95 luminance | 0.532 | 0.445 |
| Showroom glass: bright pixels | 2.6 % | 0.0 % |
| Showroom: ring around the glass, mean luminance | 0.380 | 0.377 |
| Frame time, default profile | 40-42 fps, work 22-25 ms | the same |
| Frame time, `?gfx=low` | 138-144 fps | the same |

In the race the glass pages match (2500 and 3364 texels a car), and the pass was seen on a nearby opponent (5347
pixels) and on the car in the mirror (20-130 pixels).

What is left: on the 356A the windows seen from outside in a race are painted into the exterior page (`sid`/`top`/`rea`/
`fro`). They are opaque dark texels that the game makes glassy with its sphere-map pass, so they keep that look. See
docs/findings/GLASS1-race-windows.md for the two ways to cover them (the cube in the game's reflection pass, or a window
mask per car). That needs a decision. The broken-glass path was not exercised (no damage test).

## RES1: Resolution row (runtime/pu_ext.c, pu_patch_read)

When `graphicsettings.lay` is read, its `Graphics.Resolutions` row is edited in place with same-length changes: the
combo box, its label and the two rectangles at y 228/230 get `state=hidden`. The rows under it (WideScreen, CarDetail,
CarChrome, SpecialFX, ViewDistance) each move up 18 px. `RES_OPTION=1` keeps the row. `pu_patch_read` now picks the
layout by file name; the old `optiontabs.lay` Controllers-tab hide is unchanged.

Not checked yet. Next steps:
1. `node web/tools/race_look.mjs --port 8137 --profile <dir> --steps "OPTIONS,wait:3,dumpall"`, then click the graphics tab
   (the third tab by x) and `dumpall` again.
2. Expect the log line `[options] graphics options: the Resolution row hidden (4 controls), the rows under it moved up 18
   (16 controls)`, no control at y 228 with the Resolutions accessor, and WideScreen at 228.
3. If the built-in ComboBoxSlider ignores `state=hidden`, move its `pos=` off screen instead (same length) and hide only
   the label and rectangles.
4. Check that the keyboard and pad focus (web/src/focus-page.ts skips flags & 0x30) never lands on the hidden box.

## How to check in a real browser

- Camera: Quick Race, any car and track (Redrock). Press C in the race until the chase camera, finish or quit to the
  menu, then start another race: it starts in the chase camera. Reload the page: the next race still starts there.
- Glass: open a car in the showroom (main menu background, dealer). The windows should show moving sky and sun
  reflections that are stronger at grazing angles, with the interior still visible. In a race, look at an opponent
  beside you with the chase camera. The 356A's own windows keep the old look (see above).
- Resolution: Options -> Graphics. Not checked yet (see RES1).
