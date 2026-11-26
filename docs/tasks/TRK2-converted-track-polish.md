# TRK2 — converted High Stakes tracks: polish list (2026-11-26)

User list of 2026-11-26. Status per item.

| # | Item | Status |
|---|------|--------|
| 1 | Sound drops ~100 ms in races, about once a minute (PC + Android) | in work |
| 2 | Sandbox profile "AllCarsAndTracks" (all cars, full tuning, all tracks, no career), opened by registering; online name = account name | design agreed, not started |
| 3 | Seams on gradient textures (Redrock rock-to-sand): edge texels blend with the opposite edge (wrap) | plan (a): clamp for polygons whose UVs stay inside one tile |
| 4 | Converted tracks in High Stakes order | done (data, ids unchanged; _mod 157496c) |
| 5 | Sensible track records (all 19 copied Monaco1's) | in work: times from the AI6 lap tables |
| 6 | Blurry/pixelated NFS4 horizon: smooth x4 upscale + light blur | in work |
| 7 | Horizon bottom vs sky dome colour (sea on Atlantica/Dolphin shows light) | in work: extend the strip's bottom colour, dome bottom = same colour |
| 8 | Huge draw distance: good on Coastal, bad on Snowy Ridge (far track pieces float) | **TODO, waiting for the user**: per-track draw-distance table; the user will name the tracks that go back to the default distance |
| 9 | Aquatica lamp rays opaque | done (`<additive>` bitmaps -> soft alpha, _mod 84f3378) |

## 8 — draw distance per track (waiting)

Today: `DRAW_X` x2 / at least 20000 for every track (override 0044F1A0, patches/exe_v35.txt).
Plan: a table track -> multiplier (1 = stock distance), read where `DRAW_X` is applied.
Tracks to set back to default: _the user will list them_ (first report: Snowy Ridge).
