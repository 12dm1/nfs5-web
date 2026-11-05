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
ls gen/all/*.c | xargs -P "${JOBS:-16}" -I{} sh -c 'o="$OUT/gen/$(basename {} .c).o"; [ "$o" -nt "{}" ] || "$CC" $OPT -g1 -c -w -Iruntime -Igen/all "{}" -o "$o" || echo "FAILED: {}"'
RT="win32 win32_ext fileio mem heap dispatch module genexec lifecycle debug watchdog sha256 res modheap pu_stubs pu_hooks nettest pu_com pu_imports thrash_hle pu_ext pu_interp pu_cube pu_paint"
pids=""
for f in $RT; do
  "$CC" $OPT -g -c -w -include win/posix_compat.h -Iruntime -Igen/all "runtime/$f.c" -o "$OUT/rt/$f.o" & pids="$pids $!"
done
for f in runtime/ext/*.cpp; do "${CC%clang}clang++" -std=c++17 $OPT -g -fno-exceptions -fno-rtti -w -c "$f" -o "$OUT/rt/ext_$(basename "$f" .cpp).o" & pids="$pids $!"; done
for f in runtime/win/*.c; do
  "$CC" $OPT -g -c -w -include win/posix_compat.h -Iruntime -Igen/all "$f" -o "$OUT/rt/win_$(basename "$f" .c).o" & pids="$pids $!"
done
for p in $pids; do wait "$p" || { echo "runtime compile error"; exit 1; }; done
# WEB2: the THRASH renderer (dx11z core, C++); RENDER=0 builds the headless program
CXX="${CC%clang}clang++"
LIBS="-lws2_32 -lwinmm -lpsapi -lpthread -lole32"
rm -f "$OUT"/rt/r_*.o
if [ "${RENDER:-1}" != 0 ]; then
  mkdir -p "$OUT/r"; pids=""
  for f in renderer/*.cpp renderer/dx11z/*.cpp; do
    o="$OUT/r/$(basename "$f" .cpp).o"
    if [ ! "$o" -nt "$f" ] || [ -n "$(find renderer -name '*.h' -newer "$o" 2>/dev/null | head -1)" ]; then
      "$CXX" -std=c++17 $OPT -g1 -c -w -Irenderer "$f" -o "$o" & pids="$pids $!"
    fi
  done
  for p in $pids; do wait "$p" || { echo "renderer compile error"; exit 1; }; done
  cp "$OUT"/r/*.o "$OUT/rt/" 2>/dev/null; for o in "$OUT"/r/*.o; do mv "$OUT/rt/$(basename "$o")" "$OUT/rt/r_$(basename "$o")"; done
  LIBS="$LIBS -ld3d11 -ldxgi -ld3dcompiler -luser32 -lgdi32 -static"   # libc++/libunwind inside the exe
  CC="$CXX"
fi
"$CC" $OPT -g -o out/nfs5_native.exe "$OUT"/gen/*.o "$OUT"/rt/*.o $LIBS -Wl,--stack,16777216 -Wl,--error-limit=0 2>&1 | tee out/link.log
echo "link OK -> out/nfs5_native.exe"
