#!/bin/bash
# CARS1: re-lift and rebuild the native program (and, with CHK=1, the CARCHK variant out/nfs5_chk.exe in the background).
set -e
cd "$(dirname "$0")/.."
ZIP=${ZIP:-/m/Games/nfs5_clean/nfspu_v35.zip}
./gen.sh "$ZIP" 2>&1 | grep -E "gen OK|MISSING: [1-9]"
JOBS=${JOBS:-14} ./link_native.sh > out/build.log 2>&1 || { tail -20 out/build.log; exit 1; }
tail -1 out/build.log
if [ "${CHK:-0}" = 1 ]; then
  CARCHK=1 GENOUT=gen/chk ./gen.sh "$ZIP" 2>&1 | grep -E "gen OK"
  (GEN=gen/chk NATIVE=out/native_chk EXE=out/nfs5_chk.exe JOBS=10 ./link_native.sh > out/build_chk.log 2>&1 &)
fi
