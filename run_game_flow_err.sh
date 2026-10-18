#!/bin/bash
# Boot -> title -> Load Profile -> Yes -> Main Menu -> Scenario -> skip movies, until gameplay. Needs a saved profile.
tag=$1; shift
cd ~/phantom-dust-re/pd-game-v2; : > pad_live.txt; rm -f sample.flag
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_PAD_LIVE=pad_live.txt RECOMP_HW=1
for kv in "$@"; do export "$kv"; done
(nohup build/pd_recomp.exe > run_$tag.out 2> run_$tag.err &)
for i in $(seq 1 40); do t=$(powershell -NoProfile -Command "Get-Process pd_recomp -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle"); case "$t" in *"draws: 16"*) break;; esac; sleep 15; done
sleep 15
for b in "start:2500" "start:2500" "down:200" "a:1500" "a:1500" "a:1500"; do echo "$b" >> pad_live.txt; sleep 40; done
for k in 1 2 3 4 5 6 7 8; do echo "start:2500" >> pad_live.txt; sleep 25; grep -a -q "pausemenu_en.ssb" run_$tag.err && break; done
sleep 30
echo ready $(date +%T)
