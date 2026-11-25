#!/bin/bash
# Regenerate everything derived from the game's EXE: patched copy, relocations, lifted C.
#   ./gen.sh <Porsche.exe | nfspu_v35.zip>        (default: $NFS5_EXE)
# Output: out/PORSCHE.EXE (patched copy), out/relocs.txt, gen/all/*.c (never committed).
# Lift options (environment, as in nfsu2-web): JTSWITCH=1 by default (MSVC switch
# tables become C switches), NSHARD=64.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
SRC="${1:-$NFS5_EXE}"
[ -f "$SRC" ] || { echo "usage: ./gen.sh path/to/Porsche.exe (v3.5, English) or nfspu_v35.zip"; exit 1; }
PY="${PY:-python}"
export JTSWITCH="${JTSWITCH:-1}"
# PERF2: x87 register allocation within a block (tools/lift.py X87Region): ST(n) become constant-index locals instead of
# c->st[(c->top+n)&7]; the race's frame ~7 % cheaper, same results (AI races traced against X87OPT=0, 2026-11-25)
export X87OPT="${X87OPT:-1}"
# instruction hooks served by runtime/pu_hooks.c
export HOOKS="${HOOKS:-0x5725A5,0x4D2FEA,0x4D3091,0x468AA6,0x45D6F6,0x45D6FB,0x45D25C,0x45D261,0x415C7F,0x4D2B80,0x4D3D00,0x41562E,0x4B6CB9,0x40B6FD,0x40BA91,0x40BAF0,0x40C865,0x40C8DA,0x41041E,0x43A3B0,0x43C431,0x413800,0x40F740,0x44E324,0x44EC58,0x4CBBDE,0x4CBBF9,0x41A495,0x41A4AE,0x46700C,0x467011,0x467162,0x467167,0x539133,0x47FB6F,0x47FB74,0x47F642,0x45B3BE,0x45BD7E,0x45B3C3,0x45BD83,0x415C6E,0x4B4DBB,0x4B6AC3,0x413E8B,0x414002,0x414699,0x413DD1,0x414E1C}"   # guest_hook sites (runtime/pu_hooks.c): network autotest only; loops yield by themselves (GUEST_BACKEDGE)
# lifted functions replaced in runtime/pu_ext.c (the original stays as sub_x_orig)
export OVERRIDE="${OVERRIDE:-0x4E06D0,0x40F480,0x498FF0,0x4677D0,0x43E120,0x4980D0,0x4152E0,0x4F4FA0,0x4F5820,0x4F5720,0x4F57A0,0x41E170,0x44F1A0,0x564EB0,0x565270,0x414FF0,0x443F60,0x444130,0x446530,0x4D15E0,0x571A10,0x5722C0,0x572230,0x538DA0,0x425950,0x425810,0x538D80,0x55D4D0,0x4A0DF0,0x4B4B80,0x48D6A0,0x510B10,0x4F5850,0x4E3160,0x445B10,0x435460,0x445520,0x460290,0x460460,0x451710,0x488B50}"
G="${GENOUT:-gen/all}"; export GENOUT="$G"
mkdir -p out "$G"
"$PY" tools/exe.py prepare "$SRC" out/PORSCHE.EXE
"$PY" tools/relocs.py out/PORSCHE.EXE
find "$G" -name '*.c' -delete
"$PY" tools/genall.py out/PORSCHE.EXE "${NSHARD:-64}" | tee out/genall.log | grep -E "functions|MISSING|traps|truncated|jtswitch"
IMPOUT="$G/impargs.c" "$PY" tools/argcount.py out/PORSCHE.EXE > out/argcount.log 2>&1; tail -1 out/argcount.log
echo "gen OK -> $G ($(ls "$G"/shard_*.c | wc -l | tr -d ' ') shards)"
