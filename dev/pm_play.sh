#!/bin/bash
# pm_play.sh <exe-base> <tag> [cmd ...]: drive a running instance one step at a time, then screenshot it.
# cmd = pad item for pad_live_<tag>.txt (e.g. a:200, lup+lleft:900, rt:300), "w<ms>" = extra wait,
#       "c:<text>" = debug console line (mem_live_<tag>.txt, e.g. "c:quest 2"). Each pad item waits for its hold + 400 ms.
# Prints the window title (FPS/draws) and writes pd-game-v2/shot_<tag>_now.png.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exe=$1; tag=$2; shift 2
cd "$ROOT/pd-game-v2"
for c in "$@"; do
  case "$c" in
    w*) sleep "$(awk "BEGIN{print ${c#w}/1000}")" ;;
    c:*) echo "${c#c:}" >> mem_live_$tag.txt; sleep 1 ;;
    *) echo "$c" >> pad_live_$tag.txt
       ms=${c##*:}; [ "$ms" = "$c" ] && ms=150
       sleep "$(awk "BEGIN{print ($ms+400)/1000}")" ;;
  esac
done
powershell -NoProfile -ExecutionPolicy Bypass -File ../dev/capwin_i.ps1 "$exe" "$(cygpath -w "$PWD/shot_${tag}_now.png")"
