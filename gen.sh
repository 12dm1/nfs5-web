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
# instruction hooks served by runtime/pu_hooks.c
export HOOKS="${HOOKS:-0x4D2B80,0x4D3D00}"   # guest_hook sites (runtime/pu_hooks.c): network autotest only; loops yield by themselves (GUEST_BACKEDGE)
mkdir -p out gen/all
"$PY" tools/exe.py prepare "$SRC" out/PORSCHE.EXE
"$PY" tools/relocs.py out/PORSCHE.EXE
find gen/all -name '*.c' -delete
"$PY" tools/genall.py out/PORSCHE.EXE "${NSHARD:-64}" | tee out/genall.log | grep -E "functions|MISSING|traps|truncated|jtswitch"
"$PY" tools/argcount.py out/PORSCHE.EXE > out/argcount.log 2>&1; tail -1 out/argcount.log
echo "gen OK -> gen/all ($(ls gen/all/shard_*.c | wc -l | tr -d ' ') shards)"
