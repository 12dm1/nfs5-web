#!/bin/bash
# WEB1: the native Windows x64 program (out/nfs5_native.exe) from gen/all + runtime/.
#   ./link_native.sh            OPT=-O2 by default; OPT=-O0 for debugging
# Toolchain: llvm-mingw in deps/ (x86_64-w64-mingw32-clang, UCRT, winpthreads).
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
CC="${CC:-$(ls -d deps/llvm-mingw-*-ucrt-x86_64 2>/dev/null | tail -1)/bin/x86_64-w64-mingw32-clang}"
[ -x "$CC" ] || { echo "llvm-mingw not found in deps/ (CC=...)"; exit 1; }
[ -f gen/all/fntab.c ] && [ -f gen/all/impargs.c ] || { echo "run ./gen.sh first"; exit 1; }
OPT="${OPT:--O2}"
OUT=out/native; mkdir -p "$OUT/gen" "$OUT/rt"
export CC OPT OUT
# lifted code: rebuilt only when the C is newer than its object
ls gen/all/*.c | xargs -P "${JOBS:-16}" -I{} sh -c 'o="$OUT/gen/$(basename {} .c).o"; [ "$o" -nt "{}" ] || "$CC" $OPT -c -w -Iruntime -Igen/all "{}" -o "$o" || echo "FAILED: {}"'
RT="win32 win32_ext fileio mem heap dispatch module genexec lifecycle debug watchdog sha256 pu_stubs"
pids=""
for f in $RT; do
  "$CC" $OPT -g -c -w -include win/posix_compat.h -Iruntime -Igen/all "runtime/$f.c" -o "$OUT/rt/$f.o" & pids="$pids $!"
done
for f in runtime/win/*.c; do
  "$CC" $OPT -g -c -w -include win/posix_compat.h -Iruntime -Igen/all "$f" -o "$OUT/rt/win_$(basename "$f" .c).o" & pids="$pids $!"
done
for p in $pids; do wait "$p" || { echo "runtime compile error"; exit 1; }; done
"$CC" $OPT -g -o out/nfs5_native.exe "$OUT"/gen/*.o "$OUT"/rt/*.o -lws2_32 -lwinmm -lpsapi -lpthread -Wl,--stack,16777216 -Wl,--error-limit=0 2>&1 | tee out/link.log
echo "link OK -> out/nfs5_native.exe"
