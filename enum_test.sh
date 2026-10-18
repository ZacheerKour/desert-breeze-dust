#!/bin/bash
# Launch like run_game.bat, press Enter at the title, report whether the game reacts.
cd ~/phantom-dust-re/pd-game-v2
export RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_VBLANK=1 RECOMP_FB_WINDOW=1 RECOMP_TRACE_BUDGET=0 RECOMP_APU_DSP_ACK=0x83050810 RECOMP_USB=1 RECOMP_USB_PADS=1 RECOMP_KEYBOARD=1 RECOMP_HW=1
for e in "$@"; do export "$e"; done
n=${N:-6}
ok=0
for k in $(seq 1 $n); do
  taskkill //F //IM pd_recomp.exe >/dev/null 2>&1; sleep 2
  (nohup build/pd_recomp.exe > run_en$k.out 2> run_en$k.err &)
  for i in $(seq 1 40); do t=$(powershell -NoProfile -Command "Get-Process pd_recomp -ErrorAction SilentlyContinue | Select-Object -ExpandProperty MainWindowTitle"); case "$t" in *"draws: 16"*) break;; esac; sleep 3; done
  sleep 8
  powershell -NoProfile -ExecutionPolicy Bypass -File ../capwin.ps1 "%USERPROFILE%\phantom-dust-re\pd-game-v2\dumps\en${k}_a.png" >/dev/null
  powershell -NoProfile -ExecutionPolicy Bypass -File ../sendmsg.ps1 13 600
  sleep 14
  powershell -NoProfile -ExecutionPolicy Bypass -File ../capwin.ps1 "%USERPROFILE%\phantom-dust-re\pd-game-v2\dumps\en${k}_b.png" >/dev/null
  r=$(python -c "
from PIL import Image,ImageChops
a=Image.open('dumps/en${k}_a.png').convert('L').resize((160,120));b=Image.open('dumps/en${k}_b.png').convert('L').resize((160,120))
import numpy as np
d=np.abs(np.array(a,int)-np.array(b,int)).mean()
print('%.1f'%d)")
  echo "run $k: image diff after Enter = $r  (usb: $(grep -ac 'USB' run_en$k.err) lines; pads: $(grep -ac 'PAD' run_en$k.err))"
done
taskkill //F //IM pd_recomp.exe >/dev/null 2>&1
