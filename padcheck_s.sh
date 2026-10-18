#!/bin/bash
# padcheck.sh <log> <cmd>... : send each pad command, wait, verify the game consumed it (PAD: live line count grows)
log=$1; shift
cd ~/phantom-dust-re/pd-game-v2
for c in "$@"; do
  n0=$(grep -a -c "PAD: live" $log)
  echo "$c" >> pad_live_s.txt
  ms=${c#*:}; sleep $(( ms/1000 + 3 ))
  n1=$(grep -a -c "PAD: live" $log)
  if [ "$n1" -le "$n0" ]; then echo "PAD DEAD at $(date +%T) after cmd '$c' (live lines $n0 -> $n1)"; exit 1; fi
done
echo "pad ok $(date +%T)"
