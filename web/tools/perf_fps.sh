#!/usr/bin/env bash
# PERF2: fps only (no profiler): web/tools/perf_fps.sh <tag> [env] [port] [jsflags]
cd "$(dirname "$0")/../.."
S=$(node -e "console.log(encodeURIComponent('player.lst,5îïðïðî.sav,îïðïðî1.hud,îïðïðî2.hud'))")
ENV="PERFSTAT=1,NETTEST=quickrace,NETTEST_AI=1,NETTEST_OPP=${OPP:-7},NETTEST_PICK=Track1.Track%3D${TRACK:-azur}${2:+,$2}"
timeout 400 node web/tools/prof_race.mjs --noprof $DBG ${4:+--jsflags "$4"} --port ${3:-8250} --width ${W:-3840} --height ${H:-1906} --url "/?log=1&seed=$S&env=$ENV" \
  --at "interp\] measured" --delay 4 --seconds ${SECS:-12} --log out/fps_$1.log > out/fps_$1.txt 2>&1
echo "$1 $(grep '\[perf game\]' out/fps_$1.log | tail -${NL:-4} | grep -o '[0-9.]* fps, per frame: work [0-9.]*' | sed 's/, per frame: work/\//' | paste -sd'|')"
