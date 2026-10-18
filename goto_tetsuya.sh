#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# from spawn in the Battle Terminal (profile h): walks to Tetsuya next to the orange monitors (same inputs as run n6)
cd "$ROOT/pd-game-v2"
echo a:300 >> pad_live.txt; sleep 4
for s in "lup+lleft:1200" "rthumb:300" "ldown:1500" "ldown:2500" "lright:900" "rthumb:300"; do echo "$s" >> pad_live.txt; ms=${s#*:}; sleep $(( ms/1000 + 3 )); done
