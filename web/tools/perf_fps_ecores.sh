#!/usr/bin/env bash
# PERF2: fps_race.sh pinned to the E-cores (logical 16-27 on the i7-14700KF), the user's game thread's likely place
cd "$(dirname "$0")/../.."
powershell -NoProfile -Command "cmd /c 'start \"\" /affinity FFF0000 /wait /b \"C:\Program Files\Git\bin\bash.exe\" web/tools/perf_fps.sh $1 ${2:-X=0} ${3:-8270}'" 
