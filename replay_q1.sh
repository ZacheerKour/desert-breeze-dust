#!/bin/bash
# from the hub with the pause menu open: start quest 1 and replay the tutorial dialogue up to free roam in the plaza
cd ~/phantom-dust-re/pd-game-v2
echo a:300 >> pad_live.txt; sleep 4; echo "quest 1" >> live.txt; sleep 45
bash ../ap.sh 5; sleep 8; bash ../ap.sh 14
echo lthumb:300 >> pad_live.txt; sleep 3; bash ../ap.sh 8
echo rt:300 >> pad_live.txt; sleep 3; bash ../ap.sh 3
echo rt:300 >> pad_live.txt; sleep 3; bash ../ap.sh 12
