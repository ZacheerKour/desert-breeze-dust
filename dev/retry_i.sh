#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# retry_i.sh <tag> : from the pause menu "Give Up" confirm dialog (Cancel selected): OK -> MISSION FAILED -> GAME OVER menu -> RETRY
cd "$ROOT/pd-game-v2"
p() { echo "$1" >> pad_live_$tag.txt; sleep $2; }
tag=$1
p left:200 2; p a:300 12; p a:300 6; p a:300 25
