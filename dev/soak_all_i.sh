#!/bin/bash
# soak_all_i.sh <tag> <minutes>: run_i.sh boot (leaves the pause menu OPEN), close it with A on "Back to Game", then padsoak_i.sh.
# Needed because padsoak_i.sh's random stick-down + A inside the open pause menu selects "Go to Xbox Live" and exits the game.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag=$1; mins=$2
"$ROOT/dev/run_i.sh" pd_recomp $tag
cd "$ROOT/pd-game-v2"
echo "a:300" >> pad_live_$tag.txt; sleep 8
powershell -NoProfile -ExecutionPolicy Bypass -File ../dev/capwin_i.ps1 pd_recomp shot_${tag}_soakstart.png
"$ROOT/dev/padsoak_i.sh" $tag $mins
