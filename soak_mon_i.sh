#!/bin/bash
# soak_mon_i.sh <exe-base> <tag> <minutes>: every 5 min append process stats and log counters to pd-game-v2/soak_<tag>.log
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; exe=$1; tag=$2; mins=$3
cd "$ROOT/pd-game-v2"; : > soak_$tag.log
for i in $(seq 0 $((mins/5))); do
  s=$(powershell -NoProfile -Command "\$p=Get-Process $exe -ErrorAction SilentlyContinue; if(\$p){ '{0} ws={1:N0}MB priv={2:N0}MB handles={3} threads={4} title={5}' -f (Get-Date -Format HH:mm:ss),(\$p.WorkingSet64/1MB),(\$p.PrivateMemorySize64/1MB),\$p.HandleCount,\$p.Threads.Count,\$p.MainWindowTitle } else {'PROCESS GONE'}")
  e=run_$tag.err
  c1=$(grep -ac "\[CRASH\]" $e); c2=$(grep -ac "OHCIDEAD\|OHCIDIAG" $e); c3=$(grep -ac "\[CPULOCK-GIVEUP" $e); c4=$(grep -ac "\[LONGWAIT\]" $e)
  p=$(grep -a "\[PERF\] presents" $e | tail -1 | sed -E 's/.*presents ([0-9]+) \(([0-9.]+)\/s\) flips ([0-9]+) in .*\(([0-9.]+) fps\).*/pres=\2 flips=\4/')
  echo "T+$((i*5))min $s | $p | CRASH=$c1 OHCI=$c2 GIVEUP=$c3 LONGWAIT=$c4" >> soak_$tag.log
  sleep 300
done
