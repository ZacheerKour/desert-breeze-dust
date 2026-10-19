#!/bin/bash
# soak_drive_i.sh <tag> <end-epoch>: from the Game Over menu (RETRY highlighted) cycle: 3 missions (retry, tutorial panels, battle, Give Up,
# Mission Failed, retry), then Game Over -> hub, hub random walk + pause menu, End Game -> main menu -> Scenario re-entry -> hub walk,
# then back to Tetsuya for the next round if time remains. Logs phase marks to soak_<tag>.phases.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; tag=$1; end=$2
cd "$ROOT/pd-game-v2"
P=pad_live_$tag.txt
pad(){ echo "$1" >> $P; ms=${1##*:}; sleep "$(awk "BEGIN{print ($ms+500)/1000}")"; }
mark(){ echo "$(date +%T) $*" >> soak_$tag.phases; }
dirs=(lup lleft lright ldown lup+lleft lup+lright)
walk(){ n=$1; for i in $(seq 1 $n); do d=${dirs[$((RANDOM%6))]}; pad "$d:$((400+RANDOM%1600))"; [ $((RANDOM%4)) = 0 ] && pad "rthumb:300"; done; }
mission(){  # starts from RETRY/confirm
  mark mission-start
  pad a:300; sleep 28
  pad a:200; sleep 3; pad a:200; sleep 3
  for k in 1 2 3; do pad rt:300; sleep 2; for i in 1 2 3 4 5 6; do pad a:200; sleep 1.5; done; done
  for k in $(seq 1 10); do d=${dirs[$((RANDOM%6))]}; pad "$d:$((500+RANDOM%900))"; pad rt:300; pad a:200; done
  pad start:300; sleep 3; pad down:200; sleep 1; pad a:300; sleep 3; pad lleft:200; sleep 1; pad a:300; sleep 14
  mark mission-failed; pad a:300; sleep 22
}
mark soak-driver-start
while [ $(date +%s) -lt $end ]; do mission; done
mark soak-driver-end
