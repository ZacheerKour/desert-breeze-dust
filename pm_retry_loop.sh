#!/bin/bash
# pm_retry_loop.sh <tag> <n>: in a mission: clear dialogue, pause -> Give Up -> OK -> MISSION FAILED -> A (retry), n times.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; cd "$ROOT/pd-game-v2"; tag=$1
p() { echo "$1" >> pad_live_$tag.txt; sleep $2; }
for i in $(seq 1 $2); do
  p rt:250 2; for k in $(seq 1 10); do p a:200 2; done; p rt:250 2; for k in $(seq 1 6); do p a:200 2; done
  p start:300 3; p down:200 1; p a:300 2; p left:200 1; p a:300 10
  for k in 1 2 3; do p a:200 4; done
  n=$(grep -a -c "battlestart_en.ssb" run_$tag.err); c=$(grep -a -c "\[CRASH\]" run_$tag.err)
  alive=$(powershell -NoProfile -Command "if (Get-Process pd_pm -ErrorAction SilentlyContinue) {1} else {0}")
  echo "cycle $i $(date +%T) battlestarts=$n crashes=$c alive=$alive"
  [ "$alive" = 0 ] && break
done
