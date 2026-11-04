#!/bin/bash
# Names the hot spots of a HOSTSAMPLE=<file> run (runtime/win/sampler.c):
#   tools/hostsample.sh <file> [n]      top n (default 40), grouped by thread + function
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYM="$(ls -d "$ROOT"/deps/llvm-mingw-*-ucrt-x86_64 | tail -1)/bin/llvm-symbolizer"
N="${2:-40}"
total=$(head -1 "$1" | awk '{print $2}')
grep -v '^#' "$1" | while read n t m o; do
  if [ "${m%%-*}" = exe ]; then f=$("$SYM" --obj="$ROOT/out/nfs5_native.exe" $(printf "0x%x" $((o + 0x140000000 - 1))) | head -1); [ "$m" != exe ] && f="(wait)$f"; else f="$m+$o"; fi
  echo "$n $t $f"
done | awk -v t="$total" '{k=$2" "$3; s[k]+=$1} END{for(k in s) printf "%7d %5.1f%%  %s\n", s[k], 100*s[k]/t, k}' | sort -rn | head -"$N"
