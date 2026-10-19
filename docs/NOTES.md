# Phantom Dust (OG Xbox, USA) - RE notes

Goal: native PC port. Started 2026-10-14.

## Layout
- `tools/` jdk-21, ghidra 12.1.4, xemu 0.8.136, xdvdfs 0.8.3 (official releases)
- `_downloads/` archive + ISO (user-supplied legal dump, do not redistribute)
- `extracted/` xdvdfs unpack of the ISO (2062 files, 4.05 GB)
- `xbeinfo.py` minimal XBE header parser (`python -I xbeinfo.py extracted/default.xbe`)

## default.xbe (verified from header)
- Title `PhantomDust`, title id 0x4D530046, retail-key encoded entry/thunk
- Base 0x10000, image 0x3F0080, 17 sections, entry 0x145B19, kernel thunk 0x25DC60
- `.text` 0x11000, 0x1A916C bytes (~1.7 MB game code)
- Statically linked XDK libs as separate sections: D3D, DSOUND, XNET, XONLINE, XMV (video), WMADEC, XGRPH, XPP, DOLBY
  - Port implication: D3D8 fixed/pixel-shader calls and DSOUND calls must be shimmed or reimplemented; XMV movies (2.9 GB, 60 files) need a decoder or conversion.

## Asset inventory (by extension, unconfirmed formats)
| ext | files | MB | note |
|---|---|---|---|
| .xmv | 60 | 2915 | Xbox Media Video, `Movie/` |
| .alr | 353 | 465 | unknown, likely audio/anim |
| .stx | 105 | 391 | `BGM/` music |
| .mk | 93 | 116 | unknown, paired with .ak |
| .ak | 93 | 57 | unknown |
| .e | 8 | 57 | `Effect/` |
| .ssb | 599 | 14 | unknown, `com/`/`Event/` |
| .bin | 83 | 13 | `Sound/` |
| .cad/.cam | 18/18 | 11 | likely camera/anim |
| .qdt | 5 | 0.7 | `Questdata/` |
| .xpr | 1 | 6 | standard XDK packed resource (textures) |
| .xwb/.xsb | 1/1 | 0.2 | XACT wave/sound bank |
| .ddx .ttf .csv | misc | | font + misc |

Format meanings above are guesses from names/dirs; none are verified yet.

## Not yet done
- Ghidra auto-analysis on `default.xbe` (loader installed, see below)
- xemu needs user-supplied BIOS + HDD image for dynamic analysis
- Nothing decompiled yet
- `pd_loader_core` targets the 2017 Windows remaster, not this version

## Ghidra setup (done)
- XBE loader: XboxDev/ghidra-xbe build-202610100001 (built for Ghidra 12.1.4). Installed to `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions\ghidra-xbe`. Extracting it into the Ghidra install dir was NOT picked up ("No load spec found").
- Project `ghidra-projects/PhantomDust`: `default.xbe` imported and auto-analyzed (see results below). Kernel imports link to `xboxkrnl.exe`, not found in project (expected).
- Launch GUI: `tools\ghidra\ghidra_12.1.4_PUBLIC\ghidraRun.bat` with `tools\jdk\jdk-21.0.12.1+1` as JAVA_HOME.

## Ghidra auto-analysis results (2026-10-14, headless, default analyzers)
- 8166 functions; 663 named (all from XDK library signatures / FID, none are game code); 71 thunks
- 1240 defined strings; 151 `xboxkrnl.exe` imports (the OS surface a PC port must shim)
- Named functions are XDK libs: D3DDevice (58), XOnline (46), XNet (27), DirectSound/MCPX, XMVDecoder (9), CRT/EH
- Entry 0x145B19 is inside `.text` (0x11000-0x1BA17C); game logic is the ~7.5k unnamed functions there
- `.data` is 1.4 MB, `.rdata` 231 KB: tables likely live there
- Logs: `analysis.log`, `stats.log`; script `ghidra-scripts/PdStats.java`
