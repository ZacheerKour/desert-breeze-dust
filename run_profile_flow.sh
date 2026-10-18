#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Usage: run_profile_flow.sh <tag> [extra env assignments...]
# Boots pd_recomp with a fresh profile, drives title -> name entry -> accept via the live pad file.
tag=$1; shift
cd "$ROOT"
rm -rf bak-profile-20261015/last_$tag; mkdir -p bak-profile-20261015
[ -d extracted/UDATA/4d530046/A78E1D952BDA ] && mv extracted/UDATA/4d530046/A78E1D952BDA bak-profile-20261015/last_$tag
cd pd-game-v2; : > pad_live.txt
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_PAD_LIVE=pad_live.txt
for kv in "$@"; do export "$kv"; done
(nohup build/pd_recomp.exe > run_$tag.out 2> run_$tag.err &)
for i in $(seq 1 40); do t=$(powershell -NoProfile -Command "Get-Process pd_recomp -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle"); case "$t" in *"draws: 16"*) break;; esac; sleep 15; done
sleep 10; echo "start:2500" >> pad_live.txt; sleep 45
echo "down:1500" >> pad_live.txt; sleep 25
echo "a:1500" >> pad_live.txt; sleep 45
echo "start:2500" >> pad_live.txt; sleep 15
echo "start:2500" >> pad_live.txt; sleep 50
echo done $(date +%T)
