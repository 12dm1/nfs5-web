# CARS1: more cars per race

Branch `cars-limit`, 2026-11-13. Question: can the per-race car limit of nfs5-web (PORSCHE.EXE v3.5, lifted to C)
be raised, and how far?

**Verdict: yes, in single player, up to 16 cars in total.** A direct race (`fe=0`) runs with 12 and 16 racers, or
with 8 racers + 8 traffic cars. All cars spawn on the grid, drive and are ranked, and the race finishes to the
results screen. These are headless, muted native runs on coastal and alps (section 4). Menu races, replays with more
than 9 cars, the results list beyond 8th place, and network races are not done (section 5).

## 1. Where the limit comes from

The engine has no single "max cars" constant. The 9 (player + opponents + cops + traffic, TR1 finding on the PC
mod) is the size of a set of static arrays. The car objects (`Car_tObj`, 0x1254 bytes, FUN_00411130) are on the
heap and are not limited. The frontend clamps the count to fit those arrays (FUN_004ECF00, section 3).

How the bounds were found:
- `tools/car_census.py`: every data operand of every lifted function in 0x5B2000..0x6E3000, with its base and index
  registers (`out/car_census.tsv`). The tables next to the car counters (`5E8B58`, `5E8AE8`, `658080`) and the
  indexed operands whose index is a car index came from it.
- Crash-driven: run 12 racers, fix the fault, repeat (four crashes: HUD opponent list, HUD per-racer data and roster,
  AI state pool, engine sound player channels, results widget).
- `CARCHK=1` lift (`tools/lift.py`, `runtime/pu_cars.c`): every indexed memory operand goes through
  `chk_ea(ea, base, extent, ip)`. The extent is the distance to the next address in the census. The run records the
  largest offset per instruction, and any access to the old place of a moved table. `tools/carchk_diff.py` compares an
  8-car run with a 12- or 16-car run and lists the instructions that reach further with more cars. It caught the
  finish table (0x608B38) that overflowed silently.

## 2. The bounds (research table)

Element counts are stock; "moved" means relocated at lift time (`runtime/pu_cars.h`) to the `.reloc` section
0x6C3000..0x6E3000. The game never reads its own relocations, and the loader now leaves that section zero.

| Address | What | Element | Stock count | Indexed by | CARS1 |
|---|---|---|---|---|---|
| 0x65808C | carInfo (setup hand-off, `car[N].key` from fe.txt; ends the setup block 6573E8) | 0x580 | 9 | car slot | moved -> 0x6C3000, 16 |
| 0x5E8AC4 | cop list (count 5E8ABC) | 4 | 9 | cop index | moved, 16 |
| 0x5E8AEC | AI-driven cars (count 5E8E28) | 4 | 9 | AI index; also `[0x12+i]` = L_CARS[i] (engine sound 004A93C0) | moved, 16 + 3 FIX operands |
| 0x5E8B10 | pursuable cars (count 5E8AB8) | 4 | 9 | | moved, 16 |
| 0x5E8B34 | every car (count 5E8B58) | 4 | 9 | car index | moved, 16 |
| 0x5E8B60 | traffic (count 5E8E24) | 4 | 9 | | moved, 16 |
| 0x5E8B88, 0x5E8BAC | car lists (copies) | 4 | 9 | | moved, 16 |
| 0x5E8BD4 | AI racers (count 5E8BF8) | 4 | 9 | | moved, 16 |
| 0x5E8C00 | humans (count 5E8BFC) | 4 | 9 | | moved, 16 |
| 0x5E8E2C | racers (count 5E8AE8) | 4 | 9 | place / racer | moved, 16 |
| 0x5E75A0 | race roster (004EDED0, 004B68F0) | 0x258 | 9 | racer | moved, 16 (KEEP 004068C4: end of 5E7550[20]) |
| 0x60C9C8 | HUD per racer (00426A80, 00427660) | 4 | 8 | racer | moved, 16 |
| 0x5E6774 | AI high-level object per car | 4 | 9 | car id (+0x520) | moved, 16 |
| 0x60FDA8 | minimap blip clock (00433110) | 4 | 9 | car | moved, 16 |
| 0x690AB0 | frontend race list (004EDF00) | 4 | 9 | racer | moved, 16 |
| 0x5E6E38 / 0x5E7484 | AI state pool + in-use flags (alloc 0040B530; loops 0040B570, 0040B610, 0040CC10, 0040DB20, 0040DF70) | 0xA0 / 4 | 10 | pool slot | moved, 24; 6 end-pointer compares remapped (CARS_END) |
| 0x608B38 | finish order (00416310, 00415FC0, 00416AC0) | 0x10 | 8 | finish place | moved, 16 (found by CARCHK) |
| 0x6563C8 | engine sound load (004AA050) | 4 | 8 | car id | moved, 32; clear count 004A876F 8 -> 32; 7 KEEPs (end of channel table 656180) |
| 0x4B5873 | setup parser: `car[N].` accepted for N < 9 | | 9 | | patched to 16 |
| heap, FUN_0042A790 (vtable 5B2DBC) | HUD opponent list widget: names 8 x 0x19 at +0x183 in 600 bytes | 0x19 | 8 | racer | object 1000 bytes, 16 names at +0x258 |
| heap, FUN_0042BCD0 (vtable 5B2E60) | race results widget: per-racer flags +0x128[8], place names +0x19C[8], table of 8 rows (FUN_0042E8B0, 0x7B0 bytes, cells inline) | | 8 | racer / place | object 0x200 bytes, flags [16] at +0x1BC; places 9+ not listed |
| 0x5E9A20 | replay buffer: holds the setup block (0xF89 dwords incl. old carInfo) | | 9 slots | | slots 0..8 copied in/out at their old offset, 9..15 in a shadow at 0x6CC200 (in memory only) |
| 0x6573E8 | setup block memset (004B4DA0, 004B6A50) | | 9 slots | | hook clears the moved carInfo too |
| 0x4ECF98, 0x4ED00D | frontend: player + opponents <= 8 | | 8 | | **not changed** |
| 0x4ED389 | frontend: player + opponents + cops + traffic <= 9, traffic cut first | | 9 | | **not changed** |
| 0x5CC480 | AI purgatory budget (5 single player, 1 network) | | | | not changed (traffic recycling, not a car limit) |
| car+0x520 | object ids, shared with smackables (`& 0x7F`, 0x5E8C24[128], 0x628FC0[128]) | | 128 | | not changed: 16 cars + up to 112 smackables |
| 0x656180 | sound channel table, 73 fixed channel ids | 8 | 73 | channel id | not changed: ids stay < 73 with 16 cars (CARCHK) |
| 0x656528, 0x656B00 | sound bank cache [32], sound command ring [64] | | | | not changed: capped by their own loops |
| 0x60AB78 | HUD draw list (count 60C7EC) | 0x24 | 202 | | not changed: 43 entries with 16 cars |
| 0x6BC3E0 | polygon clipper vertices | 0x20 | 11 | vertex | unrelated to cars |
| 0x606AF0, 0x607AF0, 0x608AF4, 0x608B14 | input per human player | | 2 | human | not affected |
| FUN_004B4DA0 (game type 3) | network setup: humans in slots 0..7, AI records remapped by `-8 + nhumans`, slot mask byte e5, NUMCARS = NUMCARS - 8 + nhumans | | 8 | | **not changed** |
| 0048BB50, 00491BE9, slot mask +0xCC0 | network game: 8 members, mask is a byte (NET1) | | 8 | | **not changed** |

Other 7/8/9 constants in the census (00422954, 00425A80, 00434FA0, 00464054, 004A4A70, 00426C90) compare the
game type (0x6573E8 == 7/8/9), not a car count.

## 3. What changed (code only, no game data)

- `runtime/pu_cars.h`: the moves (X), KEEP instructions (operand stays: the end of the previous table), END
  instructions (an operand equal to an old table's end becomes the new end), FIX operands (cross-table indexing:
  operand -> new table + delta). `CARS_MAX` = 16.
- `tools/split_reloc.py` (was a stub): at lift time an operand is moved only if an EXE HIGHLOW fixup inside the
  instruction holds it and it lies in a moved table's old range (half-open). `python tools/split_reloc.py --check`
  lists, per move, the references to its end or below its start that may need a KEEP/END. `CARS=0 ./gen.sh` lifts
  the stock layout.
- `patches/exe_v35.txt`: the parser bound, the HUD opponent list widget, the results widget, the sound clear count.
- `runtime/pu_cars.c` + hooks in `runtime/pu_hooks.c` / `gen.sh` HOOKS: setup reset clears the moved carInfo
  (004B4DBB, 004B6AC3); replay copies carry it (413E8B, 414002, 414699 out; 413DD1 in). `CARLOG=<s>` monitor:
  every `<s>` sim seconds, per car `index:flags/slice/kmh/place` on stderr. `CARCHK` runtime.
- Runtime and ext code that read the tables by address now use the new names (`pu_interp.c` interpolation captures 16
  cars + 16 cops, `pu_ext.c`, `nettest.c`, `ext/hs_ai.cpp` with `CARS_MAX` stuck timers).
- `runtime/mem.c`: the `.reloc` section is left zero.
- Build: `gen.sh` takes `GENOUT`, `link_native.sh` takes `GEN`/`NATIVE`/`EXE` (CARCHK side by side), `web/build.sh`
  and `link_native.sh` link `pu_cars`.
- Test tools: `tools/cars_fe.py` (fe.txt with N racers and K traffic cars from the game's fe.txt),
  `tools/cars_test.sh`, `tools/cars_rebuild.sh`, `tools/carchk_diff.py`, `tools/setup_keys.py` (setup key table
  0x5D1E40 -> `out/setup_keys.txt`).

## 4. Tests

All native, headless, muted: `NFS5_HEADLESS=1 NFS5_NOAUDIO=1`, `fe=0 nointromovies=1 car[0].aidrive=1`, a scratch
save dir with the generated fe.txt (overlay over the game folder). Launches at least 3 s apart, one at a time.

```
bash tools/cars_rebuild.sh                                    # gen + link (CHK=1 also builds out/nfs5_chk.exe)
bash tools/cars_test.sh <dir> 12 150 coastal                  # 12 racers, 150 s
bash tools/cars_test.sh <dir> 16 120 coastal                  # 16 racers
LAPS=1 CARLOG=10 bash tools/cars_test.sh <dir> 16 280 coastal # 16 racers, one lap to the results screen
TRAFFIC_CARS=8 CARLOG=10 bash tools/cars_test.sh <dir> 8 120 coastal   # 8 racers + 8 traffic = 16
MODELS=993,959,911,356b,550 CARLOG=10 bash tools/cars_test.sh <dir> 12 120 alps
BIN=out/nfs5_chk.exe bash tools/cars_test.sh <dir> 8 90 coastal   # CARCHK baseline, then 16; then
python tools/carchk_diff.py <dir8>/carchk.txt <dir16>/carchk.txt
```

| Run | Result |
|---|---|
| coastal, 8 racers | all 8 drive, 100-130 km/h at 60 s; the trace is identical across the normal and the CARCHK lift (deterministic) |
| coastal, 12 racers, 150 s | 12 on the grid (slices 100..67), all drive 100-200 km/h at 145 s, no fault |
| coastal, 16 racers, 120 s | 16 on the grid (112..67), all drive at 85 s and 110 s (170-200 km/h on straights), no fault |
| coastal, 16 racers, 1 lap | places 1..16 tracked all race; the player finishes at ~180 s, results screen runs 100 s more until the alarm, no fault (before the results widget fix: fault in 0042C210 at the finish) |
| coastal, 8 racers + 8 traffic | 16 cars; traffic (flags 0x12) drives 20-50 km/h and is recycled; racers ranked 1..8; no fault |
| alps, 12 racers, 5 models, 120 s | all 12 drive, places update, no fault |
| CARCHK 8 vs 16 | nothing reaches past a table except the capped sound tables, the 202-entry HUD draw list and the clipper (section 2); no access to an old moved table |

Not tested: sound (muted; the sound tables were checked by CARCHK only, audio code runs guest-side but is not
heard), the browser build in a browser (the wasm module was only compiled), long races of several laps, cops.

## 5. What is left

1. **Menu races.** The frontend clamps player + opponents to 8 and the total to 9 (0x4ECF98, 0x4ED00D, 0x4ED389),
   and the opponent option range comes from the FE data. Raising the clamps is three byte patches. The option range
   needs a code hook (the FE data files are game data and stay stock). Also the OS1 opponent accessor (004F4E60).
2. **Results list beyond 8th.** The results table has 8 rows inline in a generic 0x7B0-byte widget (FUN_0042E8B0,
   cells at +4, fields at +0x784, row count 8 also in the constructor loop). 16 rows means re-laying out that class
   in all its methods, or a second table. Places 9+ are now simply not listed.
3. **Replays with more than 9 cars.** The replay file carries the setup block with 9 carInfo slots (file size
   0x1CE28). Slots 9..15 survive only in memory (the shadow at 0x6CC200), so the replay right after the race works and
   a saved replay from a 12+ car race does not. A file format extension is needed.
4. **Limit guard.** `numcars` in fe.txt above 16 is not clamped. The parser only fills car blocks 0..15, but the count
   itself is taken as is and would overrun the 16-slot tables. A clamp after setup parsing is a small hook.
5. **Network.** Not done, and the 8 is structural there: FUN_004B4DA0 lays out humans in slots 0..7 and remaps AI
   records with `-8 + nhumans`; the game has 8 members (0048BB50, 00491BE9) and the slot mask (+0xCC0, e5) is one
   byte. More than 8 humans online is not realistic without rewriting the session layer. More than 8 *AI*
   opponents in an online race (host-run bots, NET/PvE style) looks possible: the remap would have to use the new
   16 slots, and the host would have to sync the extra cars. The planned online path (nfsu2-web virtual LAN, PLAN
   step 4) inherits this.
6. **Heap objects.** Inline per-car arrays in heap objects are found only by faults or by reading the class. Two were
   found (the two HUD widgets). The CARCHK lift does not see them, because their base is a register, not a data
   address.
7. **Performance.** 16 cars on coastal ran at the same headless pace; not measured in the browser.

## 6. Risks

- Lift-time moves depend on the relocation fixups. An operand that names a table without a fixup (computed
  addresses), or a table indexed through the base of a neighbour (the 0x12/0x13 sound case), is not moved by itself.
  Every known case is listed in pu_cars.h. CARCHK covers only the paths that ran.
- The moved tables sit in the `.reloc` section. Anything that later wants to read the relocations from guest memory
  would break; nothing does today.
- Stock-size races take the same code paths with new addresses. The 8-car trace is deterministic across lifts, but
  there is no stock-vs-new trace comparison (the baseline exe has no monitor).
