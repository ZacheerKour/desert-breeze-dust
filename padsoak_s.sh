#!/bin/bash
# padsoak.sh <log> <minutes>: random walking + A presses, abort on dead pad
log=$1; mins=$2
end=$(( $(date +%s) + mins*60 ))
moves=(lup:900 ldown:700 lleft:600 lright:600 a:300 lup+lleft:800 lup+lright:800 rleft:500 rright:500 a:300 rthumb:300)
while [ $(date +%s) -lt $end ]; do
  m=${moves[$((RANDOM % ${#moves[@]}))]}
  bash padcheck_s.sh $log $m > /tmp/pc.out || { cat /tmp/pc.out; exit 1; }
done
echo "soak done, pad alive $(date +%T)"
