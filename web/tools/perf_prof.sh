#!/usr/bin/env bash
# PERF2 bench: Quick Race Coastal, 8 cars, AI drives, 3840x1906; profile 12 s from 4 s after the race's start
# usage: web/tools/perf_prof.sh <tag> [extra env, comma list] [port]
cd "$(dirname "$0")/../.."
S=$(node -e "console.log(encodeURIComponent('player.lst,5îïðïðî.sav,îïðïðî1.hud,îïðïðî2.hud'))")
ENV="PERFSTAT=1,NETTEST=quickrace,NETTEST_AI=1,NETTEST_OPP=${OPP:-7},NETTEST_PICK=Track1.Track%3D${TRACK:-coastal}${2:+,$2}"
timeout 400 node web/tools/prof_race.mjs --port ${3:-8190} --width ${W:-3840} --height ${H:-1906} --url "/?log=1&seed=$S&env=$ENV" \
  --at "interp\] measured" --delay 4 --seconds ${SECS:-12} --interval ${IV:-250}  --top ${TOP:-0} ${FOCUS:+--focus $FOCUS} ${CALLERS:+--callers $CALLERS} --out out/bench_$1.json --log out/bench_$1.log > out/bench_$1.txt 2>&1
node -e "
const r=JSON.parse(require('fs').readFileSync('out/bench_$1.json'))[0];
const L=require('fs').readFileSync('out/bench_$1.log','utf8').split('\n').filter(l=>l.includes('[perf game]')).slice(-3).map(l=>l.match(/([0-9.]+) fps, per frame: work ([0-9.]+)/)).filter(Boolean).map(m=>m[1]+' fps/'+m[2]+' ms');
const w=['sub_0043c830','pu_cube_capture','sub_00446530','gil_acquire','guest_yield_end','draw_any','r_pageflip'];
console.log('$1', 'busy', (r.busy/1000).toFixed(2)+'s', L.join(' | '), '::', w.map(x=>x.replace('sub_','')+' '+(r.total[x]||0).toFixed(0)).join(', '));"
