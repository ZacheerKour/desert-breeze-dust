#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# usage: mv.sh "lup:1500" "a:300" ... ; then screenshot
cd "$ROOT/pd-game-v2"
for s in "$@"; do echo "$s" >> pad_live.txt; ms=${s#*:}; sleep $(( ms/1000 + 2 )); done
powershell -NoProfile -ExecutionPolicy Bypass -File ../dev/capwin_ab1.ps1 "$(cygpath -w "$ROOT/shot_now.png")"
