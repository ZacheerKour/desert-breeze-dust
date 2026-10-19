#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# ap.sh N : press A N times, 2s apart
cd "$ROOT/pd-game-v2"
for i in $(seq 1 $1); do echo a:200 >> pad_live.txt; sleep 2; done
