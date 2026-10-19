# Quest sweep (worker q, 2026-10-19)

Method: one process, `quest N` console hook; between quests the `hub` console command (stage end + town) then `quest N` (battle->battle chaining crashed in the HUD draw). 15 s settle with `a` presses, screenshot a, 55 s walk/lock/cast input, screenshot b. A chain crash is retried once as the first quest after a fresh boot. Run was PAUSED at quest 55 (see docs/RECOMP_ANALYSIS.md): ids 56-145, 150, 151, 245, 0-2 are NOT swept yet. FPS = game FPS (min/max of 1.6 s samples, machine shared with the PM's builds).

| id | result | first frame (s) | FPS | max draws | notes |
|---|---|---|---|---|---|
| 3 | ok | 7 | 26.6-31.0 | 1053 |  |
| 4 | ok | 5 | 26.0-30.0 | 1086 |  |
| 5 | ok | 5 | 29.5-30.5 | 930 |  |
| 6 | ok | 5 | 29.0-31.0 | 996 |  |
| 7 | ok | 5 | 29.0-31.0 | 1005 |  |
| 8 | ok | 5 | 24.6-30.0 | 1116 |  |
| 9 | ok | 5 | 29.0-31.0 | 1260 |  |
| 10 | ok | 5 | 28.0-31.0 | 1299 |  |
| 11 | ok | 5 | 26.0-30.0 | 1138 |  |
| 12 | ok | 5 | 29.0-31.0 | 1154 |  |
| 13 | ok | 5 | 18.0-30.0 | 1231 |  |
| 14 | ok | 5 | 29.0-31.0 | 1444 |  |
| 15 | ok (retry) | 8 | 26.0-31.0 | 932 | chain start crashed (load-timeout); loads fine on fresh boot |
| 16 | ok | 5 | 11.0-31.0 | 1032 |  |
| 17 | ok (retry) | 5 | 20.0-30.0 | 1243 | chain start crashed (crash); loads fine on fresh boot |
| 18 | load-timeout | - | - | - |  |
| 19 | ok | 8 | 28.0-31.0 | 1107 |  |
| 20 | ok | 8 | 29.0-31.0 | 941 |  |
| 21 | ok (retry) | 8 | 29.0-31.0 | 234 | chain start crashed (crash); loads fine on fresh boot |
| 22 | ok (retry) | 8 | 22.0-30.0 | 1971 | chain start crashed (crash); loads fine on fresh boot |
| 23 | ok | 8 | 22.0-31.0 | 2296 |  |
| 24 | ok (retry) | 7 | 28.0-31.0 | 881 | chain start crashed (crash); loads fine on fresh boot |
| 25 | ok (retry) | 8 | 1.0-31.0 | 847 | chain start crashed (crash); loads fine on fresh boot |
| 26 | ok | 7 | 27.0-31.0 | 935 |  |
| 27 | ok | 5 | 29.0-31.0 | 629 |  |
| 28 | ok (retry) | 8 | 29.0-31.0 | 1130 | chain start crashed (crash); loads fine on fresh boot |
| 29 | ok | 5 | 29.0-31.0 | 965 |  |
| 30 | ok | 5 | 25.0-31.0 | 1009 |  |
| 31 | ok | 5 | 29.0-31.0 | 1023 |  |
| 32 | ok | 5 | 19.0-30.0 | 1218 |  |
| 33 | ok | 5 | 29.0-31.0 | 1166 |  |
| 34 | ok | 7 | 26.0-31.0 | 1129 |  |
| 35 | ok | 5 | 29.0-31.0 | 1145 |  |
| 36 | ok | 5 | 18.0-29.0 | 861 |  |
| 37 | ok | 8 | 29.0-31.0 | 1483 |  |
| 38 | ok (retry) | 5 | 23.6-30.0 | 864 | chain start crashed (crash); loads fine on fresh boot |
| 39 | ok (retry) | 5 | 29.0-30.5 | 1167 | chain start crashed (crash); loads fine on fresh boot |
| 40 | ok (retry) | 5 | 21.0-30.0 | 1197 | chain start crashed (crash); loads fine on fresh boot |
| 41 | ok | 5 | 29.0-30.5 | 1222 |  |
| 42 | ok | 8 | 29.0-31.0 | 1248 |  |
| 43 | ok | 5 | 29.0-31.0 | 716 |  |
| 44 | ok | 5 | 29.0-31.0 | 997 |  |
| 45 | ok (retry) | 5 | 25.0-30.0 | 1049 | chain start crashed (crash); loads fine on fresh boot |
| 46 | ok (retry) | 8 | 25.0-30.0 | 1109 | chain start crashed (crash); loads fine on fresh boot |
| 47 | ok | 5 | 29.0-31.0 | 919 |  |
| 48 | ok | 5 | 27.0-30.0 | 891 |  |
| 49 | ok (retry) | 5 | 27.0-31.0 | 1000 | chain start crashed (crash); loads fine on fresh boot |
| 50 | ok | 5 | 29.0-31.0 | 929 |  |
| 51 | ok | 5 | 23.0-30.0 | 1096 |  |
| 52 | ok | 5 | 28.0-31.0 | 1067 |  |
| 53 | ok | 5 | 27.0-30.0 | 1018 |  |
| 54 | ok (retry) | 5 | 29.0-31.0 | 994 | chain start crashed (crash); loads fine on fresh boot |
| 55 | crash | - | - | - | [CRASH] Access violation at RIP=0x7FF61ACC5CF1, fault addr=0xFC2B425C  |

Notes
- 18: record stage field 0, no `com/st00`, game shows its own "problem with the disc" screen (black, 11 draws); not a playable battle. Same on a fresh boot.
- 21: stage 24, only 127-234 draws, black void with a white fog band (shot_qs_21_a/b.png): suspected missing geometry (not confirmed against hardware).
- 17 (stage 13), 20 (stage 23), 21, 22 ... use stage ids up to 26; ids 0-2, 150, 151, 245 and 56-145 not swept.
- Crash signature in chain mode (13 of ~66 starts): AV in the game thread (entry 0x14596A) at sub_00157780/sub_0007B670/sub_00012E70 reading garbage (see docs/RECOMP_ANALYSIS.md).
- Quest ids map 1:1 to `Event/en/questNNN.ssb` (battle scripts); record at [0x39E754]+id*0x520, stage at +0x504.
