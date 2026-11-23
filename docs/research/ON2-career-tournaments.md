# ON2: career tournaments, and how to run them online

Research 2026-11-13, read-only (static analysis of PORSCHE.EXE v3.5, timestamp 393C2E8F, and the stock data in
`M:/Games/nfspu_nglide/FEData/Data`). Sources: Ghidra decompile export `_mod/ghidra/export/functions`, the asm dump
`_mod/build/P1/porsche.asm`, `nfs5.trn` / `nfs5.per` / `nfs5.car` / `nfs5.trk`, `festrings.csv`, `tournament.lay`.
Nothing was run and no game file was touched. "Unknown" means it was not found, not that it does not exist.

Goal: online career tournaments (several races, points, rewards, the overall placing moves the player's career on as
in single player, free grid slots filled with career bots), over the Internet mode (NetGather) and our Node entry
server (`_mod/docs/findings/N1-internet-entry-server.md`).

## 0. Corrections to older notes

| Note | Says | Actually |
|---|---|---|
| `_mod/CLAUDE.md`, T006 | GameType 5 = "Career Tournament (transitional)" | 5 = **tournament test drive**: the `TESTDRIVE` command of the tournament screen (`005060D0`) sets 5, calls `004D5730`, and sets 4 back if that fails. A tournament race itself runs as **GameType 4**. Type 5 builds no opponents (`004EBDA0`, `004ECF00`) and records no result |
| same | 10 / 11 = Factory Driver KO / QKO | 10 = the **end-of-era prize-car race** (the hidden "550 A / 935 / GT1 Trophy" events, `+0E4 = 2`), 11 = its test drive. The screen with `TESTDRIVE` + `DECLINE` (`00506B60`) sets 10. A win in type 10 gives the car (`004EDF00` -> `PrizeCars.Car` -> `00502270`) |
| NP1 | `+3CC + diff*A4h` = AI skill scales per difficulty | `+3CC` is `+2C` of the **race** block (stride `0xA4`): a per-race skill factor (section 1.3) |
| T006 | `0x359` insufficient funds for repair, `0x35A` repair confirmation | `0x359` = string 857 "not enough credits to enter this tournament" (entry fee), `0x35A` = 858 "unfinished tournament in progress ... it will be reset" |
| NP1 | "the first win advances the year" | the first **podium** (overall placing 1-3) of an era tournament advances the year (`00503AD0`) |
| N1 per-car table | `0x226` colour, `0x227` femultiplayercarid, `0x23F` personality ... | the name column is shifted down by one row. Checked against the EXE's field table (`004B44xx..004B49xx`, entries `[id][addr][name]`): **`0x225` .COLOUR (+0x10), `0x226` .FEMULTIPLAYERCARID (+0x574), `0x23D` .DRIVER, `0x23E` .PERSONALITY, `0x23F..0x242` .SKILLSLOW/MEDIUM/FAST/DEFAULT, `0x243..0x246` .SKILLRANGE*, `0x247` .STARTINGPOS**. The ids and carInfo offsets in N1 are fine; the stage-5 text of N1 (`0x226` = car id, `0x225` = colour) was already right |

## 1. `nfs5.trn` record (35 x `0xD40`)

Loaded by `00502FF0` (factory file) or `005029F0` (from the save), saved by `00502AE0`. Table `DAT_00691E58`, count
`DAT_00691E64`. Every lookup is by id (`00503F20(id)` -> record). Shorts are signed 16-bit, ints 32-bit LE.

### 1.1 Header

| Off | Type | Meaning | Evidence |
|---|---|---|---|
| `+000` | i32 | tournament id (1..59, sparse) | all lookups |
| `+004` | i32 | group: `0x209/20A/20B` (521-523) = Classic/Golden/Modern **era events**, `0x20C/20D/20E` = Classic/Golden/Modern **club events** | `00502BA0` builds one id list per group |
| `+008` | i16 | start year of an era event (1950/1970/1989), 0 for club | data |
| `+00A..+01F` | i16[] | Classic `{4,9,16,18,25}`, Golden `{6,11,12,13,29,30,34}`, Modern none. **Unknown, no reader found** | data |
| `+020` | i32 | string id: short name ("356") | `Tournament.Tournament(SHORT)` |
| `+024` | i32 | string id: long name ("356 Challenge") | data |
| `+028` | i32 | string id: description | `(DESCRIPTION)` at `00506659` |
| `+02C` | i32 | string id: car restriction text ("356 models only") | `(CARRESTRICTION)` at `00506735` |
| `+030` | i16[85] | allowed car ids | `00504A70(tid, car)` |
| `+0DA` | i16 | allowed car count | same |
| `+0DC` | i32 | entry fee (DM), paid at the first race | `00503430`, `004D5730` |
| `+0E0` | i16 | traffic level 0..3 -> `{0,2,4,8}` cars (`DAT_005DB438`) | `004FC0B0` case 4 |
| `+0E2` | i16 | 1 = club event | `00504B00` |
| `+0E4` | i16 | 2 = hidden (the three prize-car trophies), skipped in lists and era checks | `00502BA0`, `00503AD0` |
| `+0E6` | i16 | AI difficulty 0..2, sent as setup record `0x12` (other modes read `Game.Difficutly`) | `004EB64F` |
| `+0E8` | u8 | ranking: 0 = by points, 1 = by total time (**rally**; setup record `0xD6` TOURNAMENTRALLY) | `005038EB`, `004EC4E0` |
| `+0E9` | u8 | 1 on #20/24/25/30/34 (club events with traffic). **Unknown, no reader found** | data |
| `+0EC..+108` | i32[8] | **final prize per overall place 1..8** (`00503480(tid, place)` reads `+0E8 + place*4`) | `004EDF00` |
| `+10C` | i16 | prize car id of a hidden trophy (168 550 A, 169 935, 170 GT1); leftovers in other records | `005048D0` |
| `+114` | i16 | opponent count (7 in all records) | `004EBDA0` |
| `+118 + k*58h` | 7 x `0x58` | opponents, 1.2 | `004EBDA0` |
| `+380` | u8 | 1 = use the fixed traffic list below instead of `+0E0` | `004FC0B0`, `004ECF00` |
| `+382` | i16[7] | traffic car ids (EAS* / Eden* records, kind 1) | same |
| `+39C` | i16 | race count, clamped to 10 at load | `00502BA0` |
| `+3A0 + r*A4h` | 10 x `0xA4` | races, 1.3 | many |
| `+A08..+D3B` | | unused (zero) | data |
| `+D3C` | i16 | **times completed** (era: podium finishes; club: full runs) | `(TIMESCOMPLETED)` `00506929`, `00503AD0` |

### 1.2 Opponent (`0x58`)

| Off | Meaning |
|---|---|
| `+00` | personality id -> `nfs5.per` (name `+04` string id 18000+, tier `+08`, skills `+0C/+14/+1C/+24`, ranges `+10/+18/+20/+28`) |
| `+02` | car id (`nfs5.car`) |
| `+04` / `+06` | part count / part ids (tuned build, OS1 `004EE3B0` + `004ED880`) |
| `+34` | exterior colour (ARGB, stored byte-swapped) |
| `+44` | interior colour |
| `+50` | stripe colour index (passed as stripe 1 and 2) |
| `+52` | race number / package (-1 = none) |
| `+38..+40`, `+48..+4C`, `+54` | unknown (not read by `004EBDA0`) |

### 1.3 Race block (`0xA4`, at `+3A0 + r*0xA4`)

Definition part (from the file) and result part (written during play, saved in the profile):

| Off | Type | Meaning |
|---|---|---|
| `+00` | i16 | track id (`nfs5.trk` +0: 8 Côte d'Azur, 9 Schwarzwald, 10 Pyrénées, 11 Alps, 12 Autobahn, 13 Auvergne, 14 Normandie, 16 Zone Industrielle, 18/25/26/29/30 Monte Carlo 1-5, 34 Corsica) |
| `+02` | i16 | laps (0 -> 1, point-to-point) |
| `+04` | u8 | reverse |
| `+05` | u8 | mirrored |
| `+08..+24` | i32[8] | race prize for place 1..8 (`+04 + place*4`) |
| `+28` | i32 | "safe driving" bonus for a win; 2nd gets x0.5, 3rd x0.25 (`005B23F0`, `005B2468`) |
| `+2C` | f32 | **AI skill factor of this race** (0.45..1.0): multiplies the personality's four skills |
| `+30` | u8 | result: race done |
| `+32 + s*2` | i16[8] | result: points of slot s (= 9 - place) |
| `+44 + s*4` | i32[8] | result: race time of slot s (unit not checked) |
| `+64 + s*4` | i32[8] | result: bonus paid to slot s |
| `+84 + s*4` | i32[8] | result: race prize paid to slot s |

Slot 0 is always the player; slots 1..7 are the record's opponents.

### 1.4 What the profile stores (`Tournament` section)

`00502AE0` / `005029F0`, in this order, all u32:

| # | Global | Meaning |
|---|---|---|
| 1 | `DAT_005DB764` | career year |
| 2 | `DAT_00691E7C` | next race index in the current tournament |
| 3 | `DAT_00691E6C` | current tournament (table index) |
| 4 | `DAT_00691E50` | current era group `0x209..0x20B` |
| 5 | `DAT_005DB768` | race index being played (-1 when none) |
| 6 | `DAT_005DB76C` | tournament index being played (-1 when none) |
| 7 | `DAT_00691E54` | 1 = Modern era finished (career done) |
| 8 | `DAT_00691E74` | table size, then the **whole table** with all results and `+D3C` counters |

Cash (`DAT_00691D90`, get `005012D0`, add `005012E0`), the owned cars (`DAT_00691DFC`) and the used market are in other
sections. The fixed format of this section is what makes server-side reading and rewriting of the tournament state
possible (section 4).

## 2. The tournament flow

| Step | What happens | Where |
|---|---|---|
| Screen | Tournament screen (`tournament.lay`, ctor `00505B50`, which also prepares the forfeit dialog `0x338` "Are you sure you want to forfeit..."). Tabs `0x1D6..0x1D9` (era / Classic, Golden, Modern club) -> `00505010(tab)` selects the first tournament of the group with `+D3C == 0`. `Tournament.Tournament` (FETournyAccessor) sets `DAT_00691E6C`; `Tournament.RemainingTracks` (list `DAT_00691E1C`, filled by `005041B0`) picks the race | `00505010`, `00505570` |
| Buttons | `ID_PRACTICERUN`, `ID_REPAIRBUTTON`, `ID_CHOOSETOURNEYCAR`, `ID_FORFEITBUTTON` (only after race 1), `TESTDRIVE` (GameType 5), `FORFEITTOURNY` -> `00503E40` clears this tournament's results (fee lost) | `00505D50` |
| Start checks | GameType 4: the car (`OwnedCars.Car`) must be in `+030` (else screen `0x32D`). If another tournament is half done (`00504B60`), dialog `0x35A`; yes -> `00504BD0` clears it. Before race 1: cash >= `+0DC` (else `0x359`), then `005012E0(-fee)`. `00503940` copies the race / tournament index into `005DB768/76C` | `004D5730` |
| Race setup | `004ED5D0` writes the FE->race record stream. GameType 4 adds: track `004ECDF0` (`0x10F` name, `0x1C`, `0x1D` p2p, `0x11` laps, `0x23` reverse, `0x22` mirror), difficulty `0x12 = +0E6`, game type `0x30`, the tournament block `004EC4E0` (below), cars `004ECF00`: `0x27` humans = 1, `0x28` AI = 7, per slot `0x247` STARTINGPOS = **current standing** (`00503660(tid, slot)`; before race 1 the player starts 8th, opponent k at k), opponents `004EBDA0`, traffic (`+380` list or `+0E0` level; total cars capped at 9) | |
| Opponents | per k: car + parts + colours (`004EE3B0`, `004ED880`), name = personality string, `004EEDA0` (slot records incl. `0x226`, `0x225`, `0x14B` parts), `0x23E` = slot number, `0x23F..0x242` = personality skill x race `+2C`, `0x243..0x246` = ranges | `004EBDA0` |
| Tournament records | `0xA8..0xAF` race points 8..1, `0xB0..0xB7` total points per slot, `0xD6` rally flag, `0xD7..0xDE` total time per slot, `0xBC` race index, `0xBD..0xC4` race prizes 1..8, `0xCE..0xD5` final prizes 1..8, `0xC6..0xCD` earnings per slot, `0xDF..0xE6` race winnings, `0xE9..0xF0` bonuses, `0xF3` races done, `0xC5`/`0xB8` safe-driving bonus, `0xB9` last race, `0xBA` end of era next, `0xBB` era (race globals `006576D8..00657804`; the in-race result screens show the standings from these) | `004EC4E0` |
| Results in | `004EDD00` after the race: `004EDE20` (`DAT_00690AD4` = race game type from `006573E8`, `DAT_00690ADC` = the owned car, `DAT_00690AD8` = local car index), `004EDD40` (**career only**: the 10 damage zones and the part wear go into the owned car), `004EDF00`, `004F3E40` (car lists), `004F7DB0(year)` (used market) | |
| Points / money | `004EDF00`, type 4: for every car slot `00503960(slot, time, place, flag)` writes `+30/+32/+44/+64/+84`; the bonus is paid only when the per-car byte at `005E778C + slot*0x258` is 0 (its writer is not identified; the name suggests "no crash"). Results array: place `005E77A0 + slot*0x258`, time `+4`, damage `005E7764 + slot*600`. Owned car: `+69C` wins++ on a win, `+698` mileage += 3000. Cash += race prize + bonus; after the last race also the final prize `+0E8 + place*4` | |
| Standings | `00503660(tid, slot)`: sums points, times and money of the done races and bubble-sorts (`00502880`): points desc, money desc, time asc; rally: time asc, money desc. Returns the slot's place 1..8 | |
| Progress | `00503AD0(tid)` after every race: not all races done -> `DAT_00691E7C` = next race. All done: **club** -> `+D3C`++, results cleared, nothing else. **Era** -> place (slot 0) > 3: results cleared, nothing gained (the fee is due again). Place <= 3: `+D3C`++; first time -> year steps through `005B8670` (1950 1956 1960 1965 1969 1970 1973 1975 1978 1982 1988 1989 1993 1996 1997 1999 2000). If every visible era event now has `+D3C > 0`: era `20A`/`20B` (from `20B`: `DAT_00691E54 = 1`), `DAT_00691E81 = 1`, current = first open event of the new era. Results cleared | |
| End of era | record `0xBA` -> race global `006577DC`; back in the FE `004D3420` sees it and `00505800()` (`DAT_00691E81`) and runs `VictoryRoom` + `endofera`; the prize-car race (type 10, hidden event of that era, car `+10C`); a win adds the car (`00502270`) | |
| Unlocks | new era = new event list + the dealer list by year (`004F3E40`, `+1F8 <= year <= +202`) + the used market for the year. Track unlocks tied to tournaments: **none found** | |
| Damage / repair | career forces damage on; it enters the race through `0x14C` .PARTDAMAGE / `0x14D` .DAMAGEZONES and comes back through `004EDD40`. Repair: `ID_REPAIRBUTTON` / repair screen (`Parts_RepairShoppingList`, costs cash). Damage does not block a start in `004D5730` | |
| Abort | `DAT_00657E34` set (left the race, inferred) with type 4: only `00503A50` + `00503AD0`; the race is not marked done and can be run again | `004EDF00` |
| Cheats | `DAT_00657E20` = win the tournament (`00504370`), `DAT_00657E21` = win the era (`00504450`); `DAT_00657E24 = 0x65..0x67` at table load completes eras (origin not traced) | |

The profile file is written by `004DE9B0` (`%s%s.sav`, loader `004DEDA0`); exactly when it runs after a race was not
traced.

## 3. Can a network race reproduce a tournament race?

The single-player race setup is the same `[id][slot][value]` record stream that NetGather sends as BC records, so most of
it can be copied. What the network setup (`004ED5D0` with type 3) does today, and what the host / server would add:

| Tournament race needs | Single player | Network race today | Gap |
|---|---|---|---|
| track, laps, direction, mirror | `0x10F`, `0x11`, `0x23`, `0x22` from the race block | `Track1.Track/Laps/Direction` (session record `+22` laps, `+24` direction, `+26` track id; NET0) | none: the server picks them from `+3A0 + r*A4` |
| grid size | 1 human + 7 AI | up to 8 members; PvE bots = server members (N1 stage 5) | none: bots = the first 8 - H opponents of the record (or chosen by the server) |
| bot car, colours, number | `0x226`, `0x225`, `0x24D`, `0x24E` | the server already sends `0x226`, `0x141`, `0x225` for bots | add the record's exact car and colours |
| bot tuning | `0x14B` .PARTINDEX from opponent `+04/+06` | not sent | send `0x14B`; whether a remote PC applies it is **open** (NP1 risk) |
| bot driver | `0x23E` personality, `0x23F..0x242` skill x race `+2C`, `0x243..0x246` ranges | bots run the host's AI with default skills | send them; only the host's AI reads them |
| difficulty | `0x12 = +0E6` | Game.Difficulty of the host (inferred) | host sets it |
| starting grid | `0x247` = standing | `0x247` is a per-slot record; whether the network build honours a value received for another slot is **untested** | server sends `0x247` for every slot |
| traffic | `+0E0` / `+380` | **none**: `004FC0B0` case 3 never sets a traffic count | patch needed, and traffic sync over the network is untested; keep tournaments without traffic online at first (5 of 35 have traffic: club #20, #24, #25, #30, #34) |
| tournament result screen | `0xA8..0xF3` + game type 4 | not sent; type 3 | game setting records (`< 0x10E`), so the server can send them; whether the race shows them with `0x30 = 3` is **unknown** |
| player car | owned car in `+030`, with its own parts and damage | any car of `zMergedList` (owned first, NP1 bonus garage too) | the server checks `0x226` against `+030`; the client has to offer only owned career cars (`004F4DF0`) |
| results | `005E77A0` per car index | `B7 a,b` per member (race time, best lap, 1/64 s) reaches the server; the local array is filled too | the server computes the order from B7 (or each client reports its array) |
| damage persists | `004EDD40` when type 4 | not copied | part of the write-back (section 4) |

Verdict: a host can emulate the grid of any tournament race without traffic. The data the bots need is all in the
record and fits the existing PvE path. Open points to test: remote parts (`0x14B`), `0x247` for other slots, and the
result screen in a type-3 race.

## 4. Server vs local career

### 4.1 What must change in the local game after an online race

The game does all the bookkeeping for slot 0 = "me". Each client therefore needs the race results numbered from its own
point of view: me = slot 0, the other humans and the bots = slots 1..7 (fixed for the whole tournament, chosen by the
server). Two ways to apply them:

| Route | How | Notes |
|---|---|---|
| A. Let the stock code run | hook `004EDE20`: online tournament race -> `DAT_00690AD4 = 4`; before `004EDF00`, reorder `005E77A0` (and the damage at `005E7764`) so the local car is entry 0, or fill it from the server's list; set `005DB768/76C` (`00503940`) | `004EDD40`, `004EDF00`, `00503AD0`, the cash, mileage, wins, year, era and prize logic all run unchanged. Least code |
| B. Call the pieces | `00503960(slot, time, place, flag)` x8, `005012E0(prize + bonus)` (`00505740`, `005057A0`), last race: `005012E0(00505770(00505710()))`, `00503A50`, `00503AD0(tid)`, `004F3E40`, `004F7DB0(year)`; damage: `004EDD40` logic | same effect, more control (e.g. server-chosen money) |

Before race 1: `DAT_00691E6C` = the tournament, `00503E40` (clean results), `005012E0(-fee)`. Every client must own an
allowed car and, for era events, be in that era (`+004 == DAT_00691E50`) unless the server decides otherwise.

### 4.2 Options

| | Server profile | Local save | Hybrid (recommended) |
|---|---|---|---|
| Storage | server keeps cash, garage (parts, damage, mileage), year, era, tournaments; game gets a generated `.sav` or patched memory | each PC's normal `.sav` | a separate **online profile** per account: a normal `.sav` whose master copy is on the server; download at login, upload after every save |
| Points / standings | server | local only (other humans unknown) | **server-authoritative**, sent to every client as per-slot results (4.1) |
| Rewards | server must reimplement prizes, prize cars, dealer, repair, used market | stock code | stock code, driven by server results (route A) |
| Anti-cheat | strong | none | server re-reads the `Tournament` section (fixed format, 1.4) on upload and compares it with its own standings; cash is in the property-store region (format partly known) |
| Cross-device / browser | yes | no | yes (the web build keeps files in browser storage anyway) |
| Work | very large (whole economy) | small, but no shared standings | moderate: section parser on the server, results message, route-A hook |

**Recommendation: hybrid.** The game needs a real profile to run its career screens, so keep the career in a
`.sav`, but (1) make it a dedicated online profile, so single-player careers are never mixed with online results;
(2) let the server own the tournament session: grid, bots from `nfs5.trn`, points, standings, which race is next, and
the final placing, using the same rules as `00503660` / `00503AD0` (section 2), and hand each client its own slot
view; (3) apply the effects locally through the stock functions (route A); (4) keep the `.sav` on the server as
the copy of record and check its `Tournament` section on upload. A full server profile is only worth it if the
economy (cash, cars) should become server-only later. The format knowledge it would need starts with 1.4.

## 5. Unknowns

- `+00A..+01F`, `+0E9`, opponent `+38..+40/+48..+4C/+54`: no reader found.
- The writer of the safe-driving flag at `005E778C`; the race-time unit in `005E77A0 + 4`.
- When `004DE9B0` saves the profile after a race.
- In a type-3 race: remote use of `0x14B` parts, `0x247` for other slots, display of the `0xA8..0xF3` results.
- Network traffic (count and sync).
- `DAT_00657E24` (era auto-complete at load): origin.
