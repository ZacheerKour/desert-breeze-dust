#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# usage: st.sh name input... ; presses inputs (each "btn:ms"), waits, screenshots to shot_e_<name>.png
n=$1; shift; cd "$ROOT/pd-game-v2"
for s in "$@"; do echo "$s" >> pad_live.txt; ms=${s#*:}; sleep $(( ms/1000 + 3 )); done
sleep 2; powershell -NoProfile -ExecutionPolicy Bypass -File ../capwin_ab1.ps1 "$(cygpath -w "$ROOT/shot_e_$n.png")" >/dev/null
