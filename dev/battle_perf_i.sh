#!/bin/bash
# battle_perf_i.sh <tag>: from a booted run_i.sh instance (hub, pause menu open) close the menu, start quest 1 with the
# console hook (needs RECOMP_FN_WATCH=5 in the run), skip the intro dialogue and tutorial prompts, then move/cast for
# ~2 min. Needs RECOMP_PERF=1; stats afterwards: python pd-game-v2/perfstat.py pd-game-v2/run_<tag>.err 230
# Writes pd-game-v2/battle_done_<tag>.txt when finished (worker p, 2026-10-18).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag=$1
cd "$ROOT/pd-game-v2"
p() { echo "$1" >> pad_live_$tag.txt; sleep $2; }
rm -f battle_done_$tag.txt
p a:300 6
echo "quest 1" >> mem_live_$tag.txt
sleep 50
for i in $(seq 1 14); do p a:200 3; done
for i in $(seq 1 30); do p a:200 2.5; done
for k in $(seq 1 10); do p a:300 3; p lthumb:300 2; p rt:300 3; done
for k in 1 2 3; do p a:300 3; done
for k in $(seq 1 12); do p lup:900 2.5; p lleft:800 2.5; p lright:800 2.5; p rt:300 2.5; done
echo done > battle_done_$tag.txt
