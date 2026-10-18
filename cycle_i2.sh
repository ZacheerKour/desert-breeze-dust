#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# cycle_i2.sh <tag> <n> : load-transition stress for a fresh profile where quest 1 starts with the tutorial prompts
# (pause is blocked while an Information prompt is up, so cycle_i.sh never reached Give Up on the new PC).
# Starts at the mission card ("Press A"); a 3rd arg (any) skips the card press on cycle 1 (already in battle). Per cycle: A -> intro dialogue -> tutorial (jump=lthumb, lock-on=rt twice, A on chevrons)
# -> Pause -> Give Up -> OK -> MISSION FAILED -> A -> GAME OVER menu (RETRY) -> A. Success = a new quest001.ssb load in the log.
cd "$ROOT/pd-game-v2"
tag=$1
p() { echo "$1" >> pad_live_$tag.txt; sleep $2; }
loads() { grep -a -c "quest001.ssb" run_$tag.err; }
for i in $(seq 1 $2); do
  l0=$(loads)
  [ -z "$3" -o $i -gt 1 ] && p a:300 30
  for k in $(seq 1 14); do p a:300 3; p lthumb:300 2; p rt:300 3; done
  for att in 1 2 3; do
    p start:300 4; p down:200 2; p a:300 3; p left:200 2; p a:300 14
    p a:300 8; p a:300 30
    [ "$(loads)" -gt "$l0" ] && break
  done
  echo "cycle $i done $(date +%T) quest001_loads $l0 -> $(loads) attempts=$att anomalies=$(grep -a -c 'OHCIDIAG\] ====\|CRASH\|OHCIDEAD' run_$tag.err) $(grep -a OHCIGATE run_$tag.err | tail -1 | sed 's/.*begins/begins/')"
done
