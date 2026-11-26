# TRK2 — converted High Stakes tracks: polish list (2026-11-26)

User list of 2026-11-26. Status per item.

| # | Item | Status |
|---|------|--------|
| 1 | Sound drops ~100 ms in races, about once a minute (PC + Android) | done e0a5f45: the worklet's stream margin holds the longest write gap of the last 10-20 s (headless race: underruns 4/min -> 0, margin ~20 -> ~36 ms) |
| 2 | Sandbox profile "AllCarsAndTracks" (all cars, full tuning, all tracks, no career), opened by registering; online name = account name | design agreed, not started |
| 3 | Seams on gradient textures (Redrock rock-to-sand): edge texels blend with the opposite edge (wrap) | done (_mod aad1b0d): every NFS4 polygon maps a whole texture, UVs pulled in by half a texel; all 19 .crp rebuilt (reproduced byte-exact first), backup _mod/backups/TRK2-uv-20261127 |
| 4 | Converted tracks in High Stakes order | done (data, ids unchanged; _mod 157496c) |
| 5 | Sensible track records (all 19 copied Monaco1's) | done (_mod 5222549): 1/100 s (empty = 359999, 004FAE20); place 1 = 0.92 x AI6 Gt1 bot lap, names/cars/gaps from a stock ladder each |
| 6 | Blurry/pixelated NFS4 horizon: smooth x4 upscale + light blur | done (_mod 3945e49): horz 1024, bicubic premultiplied + gaussian 1 px |
| 7 | Horizon bottom vs sky dome colour (sea on Atlantica/Dolphin shows light) | done (_mod 3945e49): band down to the NFS4 strip foot (<= 45 deg below the eye), last panel row carried down; tracks without panels (Atlantica, Aquatica, Raceway 1/2) get NFS4's earth gradient; backup _mod/backups/TRK2-sky-20261127 |
| 8 | Huge draw distance: good on Coastal, bad on Snowy Ridge (far track pieces float) | **TODO, waiting for the user**: per-track draw-distance table; the user will name the tracks that go back to the default distance |
| 9 | Aquatica lamp rays opaque | done (`<additive>` bitmaps -> soft alpha, _mod 84f3378) |

## 8 — draw distance per track (waiting)

Today: `DRAW_X` x2 / at least 20000 for every track (override 0044F1A0, patches/exe_v35.txt).
Plan: a table track -> multiplier (1 = stock distance), read where `DRAW_X` is applied.
Tracks to set back to default: _the user will list them_ (first report: Snowy Ridge).
