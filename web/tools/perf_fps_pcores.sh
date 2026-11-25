#!/usr/bin/env bash
# PERF2: fps_race.sh pinned to the P-cores (logical 0-15), stable A/B numbers
cd "$(dirname "$0")/../.."
powershell -NoProfile -Command "cmd /c 'start \"\" /affinity FFFF /wait /b \"C:\Program Files\Git\bin\bash.exe\" web/tools/perf_fps.sh $1 ${2:-X=0} ${3:-8270}'" 
