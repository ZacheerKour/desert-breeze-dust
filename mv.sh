#!/bin/bash
# usage: mv.sh "lup:1500" "a:300" ... ; then screenshot
cd ~/phantom-dust-re/pd-game-v2
for s in "$@"; do echo "$s" >> pad_live.txt; ms=${s#*:}; sleep $(( ms/1000 + 2 )); done
powershell -NoProfile -ExecutionPolicy Bypass -File ../capwin.ps1 "%USERPROFILE%\phantom-dust-re\shot_now.png"
