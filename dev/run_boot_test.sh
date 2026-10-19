#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# pd-dz0: cold boot -> title -> Load Profile -> hub -> (quest 1 -> battle loaded). Reports OK/CRASH per run.
# usage: run_boot_test.sh <tag> [RECOMP_X=Y ...]    (exe: build/pd_ab1.exe)   env LOADCPU=1 starts a CPU burner
tag=$1; shift
cd "$ROOT/pd-game-v2"; : > pad_live.txt; : > live.txt; rm -f sample.flag exit_$tag.txt
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_PAD_LIVE=pad_live.txt RECOMP_HW=1 RECOMP_FN_WATCH=5 RECOMP_MEM_LIVE=live.txt
for kv in "$@"; do export "$kv"; done
t0=$(date +%s)
(nohup bash -c "build/pd_ab1.exe > run_$tag.out 2> run_$tag.err; echo \$? > exit_$tag.txt" &)
alive() { [ ! -f exit_$tag.txt ]; }
sleep 6; PID=$(powershell -NoProfile -Command "Get-Process pd_ab1 -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Id")
for i in $(seq 1 80); do
  alive || break
  t=$(powershell -NoProfile -Command "Get-Process pd_ab1 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle")
  case "$t" in *"draws: 16"*) break;; esac; sleep 5
done
sleep 12
for b in "start:2500" "start:2500" "down:200" "a:1500" "a:1500" "a:1500"; do alive || break; echo "$b" >> pad_live.txt; sleep 22; done
for k in 1 2 3 4 5 6 7 8; do alive || break; echo "start:2500" >> pad_live.txt; sleep 15; grep -a -q "pausemenu_en.ssb" run_$tag.err && break; done
alive && sleep 10
hub=0; grep -a -q "pausemenu_en.ssb" run_$tag.err && hub=1
if alive && [ $hub = 1 ]; then echo "a:300" >> pad_live.txt; sleep 4; powershell -NoProfile -ExecutionPolicy Bypass -File ../dev/capwin_ab1.ps1 "$(cygpath -w "$ROOT/shot_boot_$tag.png")" > /dev/null; echo "quest 1" >> live.txt; sleep 50; fi
q=0; grep -a -q "stage torn down; starting quest" run_$tag.err && q=1
if alive; then
  title=$(powershell -NoProfile -Command "Get-Process pd_ab1 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle")
  r=OK
else r="CRASH exit=$(cat exit_$tag.txt)"; title=""; fi
echo "RESULT $tag $r hub=$hub quest_started=$q secs=$(( $(date +%s)-t0 )) title='$title'"
grep -a "CRASH\|access violation\|\[VEH" run_$tag.err | head -3
alive && [ -n "$PID" ] && taskkill //PID $PID //F > /dev/null 2>&1
exit 0
