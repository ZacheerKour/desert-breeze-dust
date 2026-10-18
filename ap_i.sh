#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# ap_i.sh <tag> N : press A N times, 2s apart, on instance <tag>
cd "$ROOT/pd-game-v2"
for i in $(seq 1 $2); do echo a:200 >> pad_live_$1.txt; sleep 2; done
