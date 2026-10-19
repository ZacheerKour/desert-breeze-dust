#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# padsoak_i.sh <tag> <minutes>: random walking + A presses on instance <tag>; abort when the game stops polling the pad.
tag=$1; mins=$2; log=run_$tag.err
end=$(( $(date +%s) + mins*60 ))
moves=(lup:900 ldown:700 lleft:600 lright:600 a:300 lup+lleft:800 lup+lright:800 rleft:500 rright:500 a:300 rthumb:300)
cd "$ROOT/pd-game-v2"
n=0
while [ $(date +%s) -lt $end ]; do
  m=${moves[$((RANDOM % ${#moves[@]}))]}
  n0=$(grep -a -c "PAD: live" $log)
  echo "$m" >> pad_live_$tag.txt
  ms=${m#*:}; sleep $(( ms/1000 + 3 ))
  n1=$(grep -a -c "PAD: live" $log)
  n=$((n+1))
  if [ "$n1" -le "$n0" ]; then echo "PAD DEAD at $(date +%T) after cmd '$m' #$n (live lines $n0 -> $n1)"; exit 1; fi
done
echo "soak done, pad alive $(date +%T), $n commands"
