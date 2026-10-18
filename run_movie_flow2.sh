#!/bin/bash
# Boot -> title -> Load Profile -> Yes -> Main Menu -> Scenario -> skip movies, until gameplay. Needs a saved profile.
tag=$1; shift
cd ~/phantom-dust-re/pd-game-v2; : > pad_live.txt; rm -f sample.flag
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_PAD_LIVE=pad_live.txt
for kv in "$@"; do export "$kv"; done
(nohup build/pd_recomp.exe > run_$tag.out 2> run_$tag.err &)
for i in $(seq 1 40); do t=$(powershell -NoProfile -Command "Get-Process pd_recomp -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle"); case "$t" in *"draws: 16"*) break;; esac; sleep 15; done
sleep 15
c=0
for b in "start:2500" "start:2500" "a:1500" "a:1500" "a:1500"; do
  echo "$b" >> pad_live.txt
  for q in 1 2 3 4 5 6 7 8; do
    sleep 5; c=$((c+1))
    powershell -NoProfile -ExecutionPolicy Bypass -File ../capwin.ps1 "%USERPROFILE%\phantom-dust-re\pd-game-v2\dumps\mvs_$c.png" | tail -1
  done
done
echo moviephase $(date +%T)
