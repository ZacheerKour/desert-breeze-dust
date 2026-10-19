#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; tag=$1; end=$2; cd "$ROOT/pd-game-v2"; P=pad_live_$tag.txt
pad(){ echo "$1" >> $P; ms=${1##*:}; sleep "$(awk "BEGIN{print ($ms+500)/1000}")"; }
dirs=(lup lleft lright ldown lup+lleft lup+lright)
echo "$(date +%T) hubwalk-start" >> soak_$tag.phases
while [ $(date +%s) -lt $end ]; do
  for i in 1 2 3 4 5 6; do pad "${dirs[$((RANDOM%6))]}:$((400+RANDOM%1800))"; done
  pad start:300; sleep 2; pad b:200; sleep 1; pad start:300; sleep 2; pad a:300; sleep 1
done
echo "$(date +%T) hubwalk-end" >> soak_$tag.phases
