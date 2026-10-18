#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Boot with two pads + GPU path, title -> profile load, stop at the Main Menu.
tag=$1; shift
cd "$ROOT/pd-game-v2"; : > pad_live.txt; rm -f sample.flag
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=2 RECOMP_PAD_LIVE=pad_live.txt RECOMP_HW=1
for kv in "$@"; do export "$kv"; done
(nohup build/pd_recomp.exe > run_$tag.out 2> run_$tag.err &)
for i in $(seq 1 40); do t=$(powershell -NoProfile -Command "Get-Process pd_recomp -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle"); case "$t" in *"draws: 16"*) break;; esac; sleep 15; done
sleep 15
for b in "start:2500" "start:2500" "a:1500"; do echo "$b" >> pad_live.txt; sleep 30; done
echo ready $(date +%T)
