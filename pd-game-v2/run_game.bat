@echo off
rem Launches the recompiled Phantom Dust build (pd_recomp.exe) with the runtime switches found so far.
rem STATE (2026-10-16): plays at 30 FPS on the GPU path. Boot ~20 s warm (first run after clearing the effect cache is slower), title -> Load Profile -> Main Menu -> Scenario Mode. Keys (window focused): Enter=Start, Z=A, X=B, arrows=dpad. Audio flowing (unverified by ear); XMV movies now play (attract movie after ~60 s idle at the title).
rem Run from this folder: the game data path is ..\extracted (your own extracted disc).
rem Console output (stderr) is large; redirect it if you want to read it:   run_game.bat 2> run_err.txt
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
set RECOMP_HW=1
rem glow now works (self-sample shadow copy); RECOMP_HW_NOGLOW=1 skips it
rem Optional diagnostics (uncomment): set RECOMP_WATCHDOG_SECS=20   set RECOMP_SAMPLE=1   set RECOMP_KERNEL_LOG_BUDGET=100000
build\pd_recomp.exe
