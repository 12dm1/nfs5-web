#!/bin/bash
# Compile the lifted C (gen/all) — a compile check until the runtime exists.
#   ./build.sh            native x64 objects  -> out/obj/
#   TARGET=wasm ./build.sh wasm32 objects     -> out/wobj/
# Compiler: clang (CC=...), by default the one that ships with Visual Studio 18.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
CC="${CC:-/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang.exe}"
[ -x "$CC" ] || CC=clang
[ -d gen/all ] && ls gen/all/shard_*.c >/dev/null 2>&1 || { echo "run ./gen.sh first"; exit 1; }
if [ "${TARGET:-native}" = wasm ]; then
  OUT=out/wobj
  FLAGS="--target=wasm32 -pthread -matomics -O3 -msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -fno-delete-null-pointer-checks -DNFSU2_GUEST_OBJ -Iruntime/wasm_inc"
else
  OUT=out/obj
  FLAGS="-O2"
fi
mkdir -p "$OUT"
export CC FLAGS OUT
ls gen/all/*.c | xargs -P "${JOBS:-16}" -I{} sh -c '"$CC" $FLAGS -c -w -Iruntime -Igen/all "{}" -o "$OUT/$(basename {} .c).o" || echo "FAILED: {}"'
echo "build OK -> $OUT ($(ls "$OUT"/*.o | wc -l | tr -d ' ') objects, $(du -ch "$OUT"/*.o | tail -1 | cut -f1))"
