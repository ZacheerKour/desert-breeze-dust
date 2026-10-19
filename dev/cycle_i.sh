#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# cycle_i.sh <tag> <n> : n x (mission card A -> wait -> pause -> Give Up -> OK -> failed -> RETRY)
cd "$ROOT/pd-game-v2"
tag=$1
p() { echo "$1" >> pad_live_$tag.txt; sleep $2; }
for i in $(seq 1 $2); do
  p a:300 25; p start:300 4; p down:200 2; p a:300 3
  p left:200 2; p a:300 12; p a:300 6; p a:300 25
  echo "cycle $i done $(date +%T) anomalies=$(grep -a -c 'OHCIDIAG\] ====\|CRASH' run_$tag.err)"
done
