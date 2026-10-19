#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# From the Clinic start (after run_game_flow_err.sh ready, pause menu open): resume, walk to the seated NPC and talk to him.
cd "$ROOT/pd-game-v2"
mvs(){ for s in "$@"; do echo "$s" >> pad_live.txt; ms=${s#*:}; sleep $(( ms/1000 + 2 )); done; }
echo "a:300" >> pad_live.txt; sleep 8
mvs "lup+lright:2500" "ldown:3000" "lleft:2500" "ldown:2000" "lup:1500" "lright:2500" "lleft:2500" "lup+lright:700" "lup:3000" "lleft:1500" "a:300" "lright:500" "a:300" "lleft:700" "a:300" "lright:900" "a:300" "lleft:700" "lup:400" "a:300" "lright:600" "a:300" "lleft:250" "a:300"
echo walked $(date +%T)
