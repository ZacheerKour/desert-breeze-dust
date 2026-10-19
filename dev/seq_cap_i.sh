#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; exe=$1; d=$2; n=$3; iv=$4
for i in $(seq 1 $n); do
  f=$(printf "%s/s%03d.png" "$d" $i)
  powershell -NoProfile -ExecutionPolicy Bypass -File "$ROOT/capwin_i.ps1" "$exe" "$(cygpath -w "$f")" >/dev/null 2>&1
  echo "$i $(date +%T) $(powershell -NoProfile -Command "(Get-Process $exe).MainWindowTitle")" >> "$d/titles.txt"
  sleep $iv
done
