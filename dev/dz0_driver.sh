#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# pd-dz0: alternating cold boots under CPU load. usage: dz0_driver.sh <load_threads> <tag-prefix> cfg1 cfg2 ...   cfg = N (no lock) | L (RECOMP_GUEST_LOCK=1)
nload=$1; pre=$2; shift 2
cd "$ROOT"
i=0
for cfg in "$@"; do
  i=$((i+1)); tag=${pre}${i}${cfg}
  pids=""
  for k in $(seq 1 $nload); do python -c "
while True: pass" & pids="$pids $!"; done
  extra=""; [ "$cfg" = "L" ] && extra="RECOMP_GUEST_LOCK=1"
  bash "$(dirname "$0")/run_boot_test.sh" $tag $extra | tee -a dz0_results.txt
  for p in $pids; do kill $p 2>/dev/null; done
  sleep 5
done
echo "DRIVER DONE" >> dz0_results.txt
