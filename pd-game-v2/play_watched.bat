@echo off
rem Like run_game.bat, but the PM/Claude can watch: stderr goes to run_user.err and the debug console reads mem_live_user.txt
rem (e.g. "quest 1" from the hub). Keyboard (window focused): Enter=Start, Z=A, X=B, A=X, S=Y, 1/3=L/R trigger,
rem numpad 8/2/4/6 = left stick (NumLock on), I/K/J/L = right stick, arrows = d-pad. A connected Xbox/XInput pad also works.
cd /d "%~dp0"
set RECOMP_AC97_READY=1
set RECOMP_PB_EXEC=1
set RECOMP_VBLANK=1
set RECOMP_FB_WINDOW=1
set RECOMP_TRACE_BUDGET=0
set RECOMP_APU_DSP_ACK=0x83050810
set RECOMP_USB=1
set RECOMP_USB_PADS=1
set RECOMP_KEYBOARD=1
rem MMO layout for keyboards without a numpad: WASD move, Q/E R/F camera, Space jump, Tab or right mouse = lock on,
rem 1-4 = A B X Y (skills; 1 or left mouse also confirms), Enter/Esc = Start, Backspace = back, arrows = menus.
rem Remove this line for the original layout listed above.
set RECOMP_KEYS=mmo
set RECOMP_HW=1
type nul > mem_live_user.txt
type nul > pad_live_user.txt
set RECOMP_MEM_LIVE=mem_live_user.txt
set RECOMP_PAD_LIVE=pad_live_user.txt
rem pd_play.exe = a copy of build\pd_recomp.exe, so a rebuild during play cannot collide with the running game
copy /y build\pd_recomp.exe build\pd_play.exe > nul
build\pd_play.exe > run_user.out 2> run_user.err
