#!/bin/bash
# CARS1: one headless, muted direct race (fe=0, AI drives the player) with N racers.
#   tools/cars_test.sh <work dir> <racers> [seconds=90] [track=coastal] [extra NFS5_ARGS]
# Env: BIN (default out/nfs5_native.exe), PROFILE (a SaveData folder with Stig1.hud/Stig2.hud; local, never in git),
#      CARLOG (default 5), CARCHK=1 (with a CARCHK lift), MODELS (default 993), LAPS (default 2), SEED (default 300),
#      TRAFFIC (traffic_density, default 0), TRAFFIC_CARS (extra Traffic car blocks, default 0).
# Output: <work dir>/race.log ([cars] lines), <work dir>/carchk.txt with CARCHK.
set -e
W=$1; N=${2:-8}; SECS=${3:-90}; TRACK=${4:-coastal}; EXTRA=$5
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN=${BIN:-$ROOT/out/nfs5_native.exe}
GAME=${GAME:-M:/Games/nfs5_clean}
PROFILE=${PROFILE:-M:/Games/nfspu_nglide/SaveData}
rm -rf "$W"; mkdir -p "$W/SaveData"
cp "$PROFILE/Stig1.hud" "$W/SaveData/XXhud1XX.hud"; cp "$PROFILE/Stig2.hud" "$W/SaveData/XXhud2XX.hud"
python "$ROOT/tools/cars_fe.py" "$GAME/fe.txt" "$W" --cars "$N" --track "$TRACK" --laps "${LAPS:-2}" \
       --models "${MODELS:-993}" --seed "${SEED:-300}" --traffic "${TRAFFIC:-0}" --traffic-cars "${TRAFFIC_CARS:-0}" > /dev/null
export NFS5_HEADLESS=1 NFS5_NOAUDIO=1 NFSU2_WATCHDOG=0 CARLOG=${CARLOG:-5} CARCHK_OUT="$W/carchk.txt"
cd "$ROOT"
NFS5_SAVEDIR="$W" NFS5_ARGS="fe=0 nointromovies=1 car[0].aidrive=1 $EXTRA" ALARM=$SECS \
  timeout $((SECS + 30)) "$BIN" out/PORSCHE.EXE "$GAME/Porsche.exe" > "$W/race.log" 2>&1 || echo "exit code $?"
grep "^\[cars\]" "$W/race.log" | tail -3
grep -E "TRAP|trap at|GUEST FAULT|fault|unresolved" "$W/race.log" | head -5 || true
