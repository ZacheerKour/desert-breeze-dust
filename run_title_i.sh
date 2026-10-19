#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# run_title_i.sh <exe-base> <tag> [ENV=val ...]: boot to the title (no input) for an independent instance; same env as run_i.sh.
exe=$1; tag=$2; shift; shift
cd "$ROOT/pd-game-v2"; : > pad_live_$tag.txt; : > mem_live_$tag.txt
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_PAD_LIVE=pad_live_$tag.txt RECOMP_MEM_LIVE=mem_live_$tag.txt RECOMP_HW=1
for kv in "$@"; do export "$kv"; done
(nohup bash -c "build/$exe.exe > run_$tag.out 2> run_$tag.err; echo \$? > exit_$tag.txt" &)
for i in $(seq 1 40); do t=$(powershell -NoProfile -Command "Get-Process $exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle"); case "$t" in *"draws: 16"*) break;; esac; sleep 5; done
echo title $(date +%T)
