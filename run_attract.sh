#!/bin/bash
# Boot to the title and leave it alone for $WAIT seconds so attract-mode movies cycle video modes.
tag=$1; shift
cd ~/phantom-dust-re/pd-game-v2; : > pad_live.txt
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_PAD_LIVE=pad_live.txt RECOMP_HW=1
for kv in "$@"; do export "$kv"; done
(nohup build/pd_recomp.exe > run_$tag.out 2> run_$tag.err &)
sleep ${WAIT:-420}
echo ready $(date +%T)
