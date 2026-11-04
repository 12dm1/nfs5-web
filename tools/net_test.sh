#!/bin/bash
# WEB2n: two headless native instances in one IP-mode race. Usage: tools/net_test.sh <work dir> [seconds] [profile dir]
# The profile dir (a real SaveData folder: 8Stig.sav, Stig1.hud, Stig2.hud, player.lst...; local files, never in git) is copied for each instance. Logs: <work dir>/{host,join}.log
W=$1; SECS=${2:-120}; SRC=${3:-M:/Games/nfspu_nglide/SaveData}
EXE=M:/Games/nfs5_clean/Porsche.exe
for r in Host Join; do
  d="$W/$r/SaveData"; rm -rf "$W/$r"; mkdir -p "$d"
  cp "$SRC"/* "$d/" 2>/dev/null; cp "$SRC/8Stig.sav" "$d/8Net$r.sav"
  cp "$SRC/Stig1.hud" "$d/Net${r}1.hud"; cp "$SRC/Stig2.hud" "$d/Net${r}2.hud"
  cp "$SRC/Stig1.hud" "$d/XXhud1XX.hud"; cp "$SRC/Stig2.hud" "$d/XXhud2XX.hud"
done
export NFS5_HEADLESS=1 NFS5_NOAUDIO=1 NETTEST_AI=1 NFSU2_WATCHDOG=0
NETTEST=host NFS5_SAVEDIR="$W/Host" NFS5_ARGS="nointromovies=1 fe_player_filename=8NetHost hostgame=1" ALARM=$SECS \
  timeout $((SECS+15)) ./out/nfs5_native.exe out/PORSCHE.EXE "$EXE" > "$W/host.log" 2>&1 &
h=$!
for i in $(seq 1 90); do grep -q "create screen" "$W/host.log" 2>/dev/null && break; sleep 1; done
grep -q "create screen" "$W/host.log" || echo "host has no create screen after 90 s"
NETTEST=join NFS5_SAVEDIR="$W/Join" NFS5_ARGS="nointromovies=1 fe_player_filename=8NetJoin joingame=127.0.0.1.9442" ALARM=$((SECS-20)) \
  timeout $((SECS)) ./out/nfs5_native.exe out/PORSCHE.EXE "$EXE" > "$W/join.log" 2>&1 &
j=$!
wait $j; wait $h
# PASS = both got the race start, the clock runs and each side has both cars moving
rc=0
for r in host join; do
  last=$(grep "sim tick" "$W/$r.log" | tail -2 | head -1)
  echo "$r: $(echo "$last" | sed 's/.*sim tick/sim tick/')"
  grep -q "race start received" "$W/$r.log" || { echo "$r: no race start"; rc=1; }
  echo "$last" | grep -Eq "sim tick [1-9][0-9]{3,} \| car slices 0:[0-9]+ 1:[0-9]+" || { echo "$r: race not running"; rc=1; }
done
[ $rc = 0 ] && echo "PASS" || echo "FAIL"
exit $rc
