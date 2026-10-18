# Phantom Dust PC port (static recompilation)

A work-in-progress Windows port of **Phantom Dust** (Microsoft Game Studios / Majesco, original Xbox, 2004) built by
statically recompiling the game's x86 executable into C and linking it against a Windows runtime (Xbox kernel emulation,
NV2A graphics translated to Direct3D 11, audio on XAudio2, input via XInput/keyboard). It is not an emulator and it is
not finished.

**This repository contains no game data and no code generated from the game.** You need your own legally obtained
copy of the USA retail disc. The installer extracts your disc locally and generates and builds everything on your PC.

## Requirements

- Windows 10 or 11, 64-bit. A GPU with Direct3D 11 support.
- About 20 GB of free disk space (disc image 4.3 GB, extracted game 4 GB, generated sources, build output).
- [Python](https://www.python.org/downloads/) 3.10 or newer.
- Visual Studio 2022 or Build Tools 2022 with the *Desktop development with C++* workload and the *C++ CMake tools for
  Windows* component (this provides CMake and Ninja).
- [7-Zip](https://www.7-zip.org/), only if your disc is a `.7z` archive.
- Internet access during setup (downloads the pinned `xdvdfs` tool and the `capstone` Python package).
- Your disc image: **Phantom Dust (USA)** `default.xbe` with SHA-256
  `fbac4829a44f87a2399e623e2d6347c54839184efd6dce7307deb52391f9b910`. Other regions and revisions are refused.

The installer checks all of this and tells you what is missing; it never installs anything silently.

## Install

```
git clone <this repository>
cd <clone folder>
INSTALL.bat "C:\path\to\Phantom Dust (USA) (En,Ja).7z"
```

You can also drag the disc file onto `INSTALL.bat`, or call `powershell -File install.ps1 "<path>"`. The source may be a
`.7z` containing an ISO, an `.iso`/`.xiso`, or a folder that already holds an extracted disc (`default.xbe` at its top).

The installer does the following, stopping with `INSTALL FAILED at step: <name>` on any error:

1. checks prerequisites;
2. downloads `xdvdfs` 0.8.3 into `tools\` and verifies its SHA-256;
3. extracts the disc into `extracted\` (skipped when already done);
4. verifies `default.xbe` against the accepted SHA-256;
5. creates `.venv-recomp` with a pinned `capstone`;
6. runs the recompiler pipeline (`pd_pipeline.py`) to generate C into `pd-game-v2\src\recomp\gen` (a few minutes);
7. configures and builds with CMake + Ninja (10-30 minutes; `-Jobs N` limits parallel compile jobs, default 4);
8. reports where the game is.

It is safe to re-run; finished steps are skipped. The log is `install.log`. `.\install.ps1 -SkipBuild` stops after code
generation.

## Run

```
pd-game-v2\run_game.bat
```

Boot to the title takes about 20 seconds. The game reads its data from `extracted\`, so do not move it.

### Controls

| Key | Action |
|---|---|
| Enter | Start |
| Z | A (confirm) |
| X | B (cancel) |
| Arrow keys | D-pad |

Xbox-style USB/XInput pads are also supported. Alt+Enter or F11 toggles borderless fullscreen; Alt+F4 or closing the
window quits.

### Useful switches

`run_game.bat` already sets the defaults the port needs (`RECOMP_HW=1` for the GPU path and so on). Set these before
launching to change behaviour:

| Variable | Effect |
|---|---|
| `RECOMP_HW_SCALE=n` | Internal render scale 1-8 (default: follows window size) |
| `RECOMP_HW_ASPECT=16:9` | Widen the picture (default 4:3) |
| `RECOMP_HW_VSYNC=0` | Disable vsync |
| `RECOMP_HW_NOGLOW=1` | Skip the glow effect pass |
| `RECOMP_HW_NOCULL=1` | Ignore the cull state (debugging) |
| `RECOMP_WATCHDOG_SECS=n` | Report a hang after n seconds without progress |

Console output (stderr) is large; redirect it: `run_game.bat 2> run_err.txt`.

Custom soundtrack: `make_soundtrack.py` (see the script header).

## Status

Working: boots, intro movies, title, profile create/load, main menu, the Clinic hub (free roam, NPC dialogue and
cutscenes), pad and keyboard input, audio, glow/stencil/decals, window-driven render scale.

First battle: reached through a developer debug hook (`quest <id>` command, see `RECOMP_ANALYSIS.md`), not yet through
the normal Mission Terminal flow.

Known issues (tracked in `.beads/issues.jsonl`):

- Battles run at about 2.5-6 FPS (hub: 30 FPS) (pd-pc7).
- White glyph block in the top-left of battle frames; stale sky rectangle in some cutscenes (pd-js7, pd-42q).
- Normal story progression to a battle, arsenal/Lab, battle audio, skills, AI and win/lose are untested (pd-ab1, pd-i0w).
- Split-screen waits for player 2 sign-in (pd-srj); title-screen audio stutter until a profile loads (pd-089).
- Save round-trips across restarts are not yet verified; a damaged test profile exists on the developer machine (pd-5tj, pd-lu8).
- Remaining Options screens and most movies untested; frame pacing dips (pd-4y6, pd-55f, pd-m2k).
- No long-run stability testing yet (pd-ecu).

Do not expect a finished, polished port.

## Layout

| Path | Purpose |
|---|---|
| `install.ps1`, `INSTALL.bat` | One-command setup |
| `pd_pipeline.py` | default.xbe to generated C (the stages `xbe_parser`, `disasm`, `extend`, `func_id`, `abi_analysis`, `recomp`) |
| `pd-game-v2/` | The game project: `src/main.c` (host entry, debug consoles), `src/recomp_manual.c` (hand-written overrides), seed tables, CMake |
| `xboxrecomp/` | Vendored toolkit (lifter and runtime libraries) with local patches |
| `RECOMP_ANALYSIS.md`, `NOTES.md` | Engineering log and findings |

## Licensing

- This repository's own code: MIT (see `LICENSE`).
- `xboxrecomp/` is the [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit, MIT, except the
  xemu-derived APU and NV2A register sources which are LGPL-2.1-or-later (texts in `xboxrecomp/LICENSES/`, file list in
  its `NOTICE`). Local changes follow the same terms.
- The Direct3D 11 hardware renderer, vertex-program-to-HLSL translator and some executor hooks were ported from the
  [rePOP](https://github.com/FulGerNet/rePOP) project (MIT). No rePOP-generated game code is used.
- `xdvdfs` (MIT, antangelo) is downloaded at install time and not redistributed here.
- **No rights to Phantom Dust are granted or implied.** Phantom Dust is the property of its respective owners. This
  project ships no game assets, executables or code generated from them; you must supply your own copy.
