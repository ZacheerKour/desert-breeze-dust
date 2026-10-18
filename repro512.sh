#!/bin/bash
cd ~/phantom-dust-re
for t in 1 2 3 4 5 6; do
  taskkill //F //IM pd_recomp.exe >/dev/null 2>&1; sleep 2
  bash run_game_flow.sh rp$t RECOMP_HW_FLIPDBG=1 > flow_rp$t.log 2>&1
  (cd pd-game-v2; echo "a:300" >> pad_live.txt; sleep 12)
  n=$(grep -ac "pres [0-9A-F]* 640x512" pd-game-v2/run_rp$t.err)
  echo "run $t: 512-flips=$n"
  if [ "$n" -gt 0 ]; then python ext2.py | tr '\n' ' '; echo; echo FOUND; break; fi
done
echo finished
