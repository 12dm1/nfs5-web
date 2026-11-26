#!/bin/bash
# Build the game module for the browser: out/web/nfs5.wasm + nfs5.json. Needs gen/all (./gen.sh) and wasi-sdk in deps/.
#   web/build.sh    lifted shards (rebuilt when newer) + runtime + link
# After nfsu2-web's web/build.sh: wasi-sdk (clang + wasm-ld), target wasm32-wasip1-threads, run by our own host in
# the page (web/src/host), no Emscripten.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-x86_64-windows 2>/dev/null | tail -1)}"
[ -x "$WASI/bin/clang.exe" ] || { echo "wasi-sdk not found in deps/"; exit 1; }
CC="$WASI/bin/clang.exe"
J="${J:-16}"
OPT="${OPT:--O2}"
GEN="${GEN:-gen/all}"; OUT="${WEBOUT:-out/web}"; mkdir -p "$OUT/obj" "$OUT/rt"
# The guest owns [0, GUEST_END) of linear memory (runtime/cpu.h); the runtime's data starts there.
GUEST_END=671088640   # 0x28000000
INITIAL_MB="${INITIAL_MB:-1024}"
MIN_MB="${MIN_MB:-704}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
# -fno-delete-null-pointer-checks: guest address 0 is real wasm memory (cpu.h)
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -w -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -I$GEN"
SHARDFLAGS="-DNFSU2_GUEST_OBJ"
export CC CFLAGS SHARDFLAGS OUT
ls $GEN/shard_*.c $GEN/fntab.c $GEN/impargs.c | xargs -P "$J" -I{} sh -c 'o="$OUT/obj/$(basename {} .c).o"; [ "$o" -nt "{}" ] || "$CC" $CFLAGS $SHARDFLAGS -c "{}" -o "$o" || echo "FAILED {}"'
RT="win32 win32_ext fileio mem heap dispatch module genexec lifecycle debug watchdog sha256 res modheap pu_stubs pu_hooks nettest pu_com pu_imports thrash_hle pu_ext pu_interp pu_cube pu_paint pu_text pu_cars pu_net"
pids=""; fail=0
for f in $RT; do "$CC" $CFLAGS -c "runtime/$f.c" -o "$OUT/rt/$f.o" & pids="$pids $!"; done
for f in runtime/ext/*.cpp; do "${CC%clang.exe}clang++.exe" -std=c++17 $TARGET $OPT $FEAT -fno-exceptions -fno-rtti -fno-threadsafe-statics -w -c "$f" -o "$OUT/rt/ext_$(basename "$f" .cpp).o" & pids="$pids $!"; done
for f in runtime/web/*.c; do b=$(basename "$f" .c)
  case "$b" in thread_exit) fl="$TARGET $OPT $FEAT" ;; *) fl="$CFLAGS" ;; esac
  "$CC" $fl -c "$f" -o "$OUT/rt/web_$b.o" & pids="$pids $!"
done
for p in $pids; do wait "$p" || fail=1; done
[ $fail = 0 ] || { echo "runtime compile failed"; exit 1; }
ls "$OUT"/obj/*.o "$OUT"/rt/*.o > "$OUT/objs.rsp"
rm -f "$OUT/nfs5.wasm"
"$CC" $TARGET $OPT $FEAT -o "$OUT/nfs5.wasm" @"$OUT/objs.rsp" \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=$(( MIN_MB * 1048576 )),--global-base=$GUEST_END \
  -lc++ -lc++abi -Wl,-z,stack-size=$(( 8 * 1048576 )) -Wl,--no-stack-first \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks > "$OUT/link.log" 2>&1 || true
[ -f "$OUT/nfs5.wasm" ] || { echo "link FAILED: $(grep -c 'undefined symbol' "$OUT/link.log") undefined"; grep -E "error" "$OUT/link.log" | head -60 | cut -c1-150; exit 1; }
SIZE=$(wc -c < "$OUT/nfs5.wasm" | tr -d ' ')
printf '{"pages":%d,"maxPages":65536,"guestEnd":%d,"size":%d}\n' $(( INITIAL_MB * 16 )) $GUEST_END $SIZE > "$OUT/nfs5.json"
echo "link OK: $OUT/nfs5.wasm ($(( SIZE / 1048576 )) MB)"
