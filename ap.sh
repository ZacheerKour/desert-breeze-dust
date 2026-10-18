#!/bin/bash
# ap.sh N : press A N times, 2s apart
cd ~/phantom-dust-re/pd-game-v2
for i in $(seq 1 $1); do echo a:200 >> pad_live.txt; sleep 2; done
