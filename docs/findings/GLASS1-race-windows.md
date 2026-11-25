# GLASS1: where a car's windows come from in a race

Found 2026-11-25 while giving intact car glass the R1 cube reflection (docs/tasks/GLASS1-glass-camera-res.md).
Porsche Unleashed v3.5, English.

## The three sources of window pixels

A car's `.tpg` lists its texture pages. Two of them are glass:

| tpg page | file | race build | content |
|---|---|---|---|
| tpage5 | `glass.fsh` (`window=1`) | 64x64, 32 bit (uploaded as a 4444 copy) | tinted glass at alpha 128 (2500 texels on the 356A) + opaque frame |
| tpage6 | `intglass.fsh` (`global=2`: `GameData/CarModel/INTGLASS.FSH`) | 64x64, 32 bit | the race uses image `glat`: 3364 texels at alpha 0 (clear) + 732 opaque frame; the menus use `glai`: the same at alpha 32..47 (a light tint) |

The builder (00443F60 / 00444130) gives them file indexes 3 and 4. In the showroom the pages are 4444 and 128x128 / 64x64.

What is actually seen from outside in a race (chase camera, Redrock, the 356A as the player car, CARLOD on so the closest
tier is the garage model LOD 1):

- **intglass (tpage6) polygons** are the windows' inner faces. Their race image is fully clear and the draw has an alpha
  test (aref 0.04), so they draw nothing and write no depth. From outside only the far side's inner faces pass the
  culling. With the GLASS1 pass on them and the depth test off, 6300 pixels of the 356A turn magenta; with the depth
  test on, none do: something opaque lies in front.
- **the exterior page (tpage1)**: `?paintdbg=1` paints paint pages green / magenta. All 6294 of those pixels come out
  green, so they are the exterior page's non-paint texels. On this car the windows the player sees are painted into
  the exterior page's side / top / rear / front views (`sid`, `top`, `rea`, `fro`): opaque, dark (luminance 17 of 255
  in the stock render), untinted (source alpha 255, the same zone as tyres, seams, grille and trim). The game makes
  them look like glass with its own sphere-map reflection pass (R3DSpecularTextureLayer, 00445520), which r_web.c
  draws as it always did on non-paint texels.
- **the glass page (tpage5) polygons**: on the 356A one to three triangles a frame. Other cars use it more: the
  opponent beside the player gave 5347 glass-pass pixels in one shot, the mirror's car 20 to 130.

So on such cars GLASS1 shows the cube reflection only where the model uses the glass pages; the painted windows keep the
game's sphere-map look.

## Ways to cover the painted windows (not done, needs a decision)

1. **The game's own reflection pass with the cube** (what the PC wrapper's R2 v3 does): `fs_mask` draws the sphere-map
   pass off the paint already; it could sample the R1 cube in the reflection direction instead of the sphere map for
   every non-paint texel, keeping the game's per-vertex strength. Windows, chrome and trim would all reflect the live
   cube. Broader than glass, and it changes how broken glass looks (it gets the same pass in the stock game).
2. **A window mask per car**: mark the window texels of each exterior page by hand or with the PC car editor's
   material library (CE2: GLS layer), shipped as data and applied like the paint map's G channel. Exact, but needs one
   mask per car and a format.
3. A colour heuristic (dark untinted texels in `sid`/`top`/`rea`/`fro`) was rejected: it cannot tell windows from
   tyres, seams and grilles.

## Tools used

- `GLASSLOG=1`: builder files, glass page builds (alpha histograms), uploads matched to builds, slot list, glass page draws
  with screen boxes.
- `GDUMP=<frame>`: every draw of a frame with texture, paint kind and first vertex; with GLASS1 also each deferred glass
  pass (texture, depth compare, cull, clip).
- `?glassdbg=1` intact glass magenta; `?glassdbg=3` every glass-pass pixel magenta (no glass mask), for depth checks.
- `?paintdbg=1` paint pages green (no paint) / magenta (paint).
- `web/tools/shot_mask.py` on `globalThis.stats.shotNext` shots (`--react "...|<s>:SHOT"` in `web/tools/race_look.mjs`).
