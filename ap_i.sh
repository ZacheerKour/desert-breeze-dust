#!/bin/bash
# ap_i.sh <tag> N : press A N times, 2s apart, on instance <tag>
cd ~/phantom-dust-re/pd-game-v2
for i in $(seq 1 $2); do echo a:200 >> pad_live_$1.txt; sleep 2; done
