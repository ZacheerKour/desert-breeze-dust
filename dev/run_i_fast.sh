#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# run_i.sh <exe-base> <tag> [ENV=val ...]: boot flow (title -> Load Profile -> hub, pause menu open) for an independent instance.
# Own pad/console files: pad_live_<tag>.txt, mem_live_<tag>.txt; capture with capwin_i.ps1 <exe-base> <png>
exe=$1; tag=$2; shift; shift
cd "$ROOT/pd-game-v2"; : > pad_live_$tag.txt; : > mem_live_$tag.txt; rm -f sample.flag
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_PAD_LIVE=pad_live_$tag.txt RECOMP_MEM_LIVE=mem_live_$tag.txt RECOMP_HW=1
for kv in "$@"; do export "$kv"; done
(nohup bash -c "build/$exe.exe > run_$tag.out 2> run_$tag.err; echo \$? > exit_$tag.txt" &)
for i in $(seq 1 120); do t=$(powershell -NoProfile -Command "Get-Process $exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle"); case "$t" in *"draws: 16"*) break;; esac; sleep 4; done
sleep 15
for b in "start:2500" "start:2500" "down:200" "a:1500" "a:1500" "a:1500"; do echo "$b" >> pad_live_$tag.txt; sleep ${PD_STEP:-14}; done
for k in 1 2 3 4 5 6 7 8; do echo "start:2500" >> pad_live_$tag.txt; sleep 8; grep -a -q "pausemenu_en.ssb" run_$tag.err && break; done
sleep 6
echo ready $(date +%T)
