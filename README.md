# Phantom Dust PC

A native Windows port of **Phantom Dust** (original Xbox, 2004), made by static recompilation: the game's x86 executable is translated into C and linked against a Windows runtime. That runtime emulates the Xbox kernel, translates the NV2A GPU to Direct3D 11, runs the MCPX audio on XAudio2, and maps input to XInput and the keyboard. It is not an emulator, and it runs only with game files from your own disc.

> **You need your own copy of the game.** This project contains no game assets. The installer reads them from your Phantom Dust (USA) disc image. See [Legal](#legal).

**Status: playable test builds.** The opening missions, menus, hub, movies, saving and the game's own mission flow have been tested. Long play-throughs past the first missions have not been done yet, and bug reports from real play are very welcome. See [Known issues](#known-issues).

## Features
- Native Windows executable, Direct3D 11 renderer, internal resolution follows the window size.
- **60 FPS presentation.** The game logic runs at its original 30 FPS, and the port draws an interpolated in-between frame, so motion is smooth at correct game speed. It falls back to 30 automatically where a scene is too heavy.
- Borderless full screen (F11 or Alt+Enter); experimental widescreen (`RECOMP_HW_ASPECT=16:9`).
- Xbox/XInput controllers and a full keyboard layout that also works on laptops without a numpad.
- **Custom soundtracks:** the game's Music Manager works as on the Xbox, and a bundled tool turns any folder of music into a soundtrack.
- Saves (player profiles and arsenals) in a plain folder you can back up.

## Download and play
1. **Get the game files:** your own **Phantom Dust (USA)** disc image, either a `.iso`/`.xiso` or the common `Phantom Dust (USA) (En,Ja).7z`. Only the USA retail release is supported; the installer checks it.
2. **Download** the newest `PhantomDust-PC-<version>.zip` from [Releases](../../releases) and unzip it to a simple location, e.g. `C:\Games\PhantomDust`. Avoid OneDrive, Program Files and paths that need admin rights.
3. **Install:** drag your disc file onto `INSTALL.bat`. It extracts the game files into `extracted\` and verifies them. Nothing else needs to be installed.
4. **Play:** double-click `PLAY.bat`. At the title press Enter or Start, then *Create New Player*.

### Requirements
- Windows 10 or 11, 64-bit.
- A GPU with Direct3D 11 support. Laptops should be plugged in.
- About 10 GB of free disk space.

## Controls
**Controller:** any Xbox/XInput pad works, with the original layout.

**Keyboard** (the default "MMO" layout, designed for laptops):

| Key | Xbox control | In game |
|---|---|---|
| W A S D | Left stick | Move |
| Q / E, R / F | Right stick | Turn / tilt camera |
| Space | Left stick click | Jump |
| Tab or right mouse | Right trigger | Lock on |
| Shift | Left trigger | |
| Ctrl | Right stick click | |
| 1 2 3 4 | A B X Y | Skill slots (1 / left mouse also confirms) |
| Enter or Esc | Start | Pause |
| Backspace | Back / B | Cancel |
| Arrow keys | D-pad | Menus |
| Z / C | White / Black | |

To use the original keypad layout instead, remove the line `set RECOMP_KEYS=mmo` from `play_watched.bat`. That layout is: arrows = d-pad, Z X A S = A B X Y, numpad = left stick, I/J/K/L = right stick, 1/3 = triggers, Enter = Start.

## Custom soundtracks
1. Put up to 84 songs (mp3, flac, wav, ogg, m4a, aac, wma or opus) in a folder. The folder name becomes the soundtrack name.
2. Drag the folder onto `make_soundtrack.bat`. It converts the songs to the Xbox format (WMA) with the bundled ffmpeg.
3. In the game, open **Options Mode > Music Manager**, choose a stage, then your soundtrack and a song. It plays in every battle on that stage.

Running the tool again replaces the soundtrack. One custom soundtrack at a time is supported for now.

## Settings
Run `Settings.bat` (or open `settings.ini` next to `PLAY.bat` in Notepad), edit, save, and start the game again. Missing lines use the defaults.

| Line in `settings.ini` | Values (default first) | Effect |
|---|---|---|
| `fullscreen` | `0` / `1` | Start in borderless full screen |
| `window` | `auto` / `1280x960` / `1920x1080` / `2560x1440` ... | Window size in pixels (`auto` = 1280 wide) |
| `render_scale` | `auto` / `1`-`8` | Internal render resolution as a multiple of 640x480 (`auto` follows the window) |
| `aspect` | `4:3` / `16:9` | Picture aspect ratio (16:9 is experimental) |
| `interpolation` | `1` / `0` | 60 FPS in-between frames; `0` = plain 30 FPS |
| `vsync` | `1` / `0` | Wait for the monitor refresh |
| `keys` | `mmo` / `classic` | Keyboard layout (see Controls) |

F11 or Alt+Enter toggles borderless full screen at any time. The window title shows the frame rate. `FPS: 60.0 (game 30.0)` means interpolation is active.

Advanced: any `RECOMP_*` switch can still be added as a `set NAME=value` line in `play_watched.bat`; a switch set there wins over `settings.ini`.

**Typing a profile name:** on the NAME ENTRY screen the physical keyboard types directly: letters, digits and Space, Backspace deletes, Enter = OK, Esc = back (Shift for capitals). The on-screen keyboard and the pad still work.

## Saves and files
- **Profiles and arsenals:** `extracted\UDATA`. Back this folder up; updating the port never touches it.
- **Custom soundtrack:** `extracted\TDATA\FFFE0000\MUSIC`.
- **Shader cache:** `pd-game-v2\bin\shadercache`. The first minutes of play may stutter briefly while it fills.
- **Logs:** `pd-game-v2\run_user.err`. Please attach it to bug reports.

To update, unzip a new release over the old folder, or into a new folder and copy `extracted\` across. You don't need to run INSTALL.bat again.

## Known issues
- Story progress beyond the first missions has had little testing. Crashes or hangs there are the most useful reports.
- Under heavy load the controller can stop responding for a few seconds, then recover by itself.
- The busiest hub areas run at about 50 FPS instead of 60.
- Split Screen: Player 1 waits at "Please wait..." until a second controller signs in.
- Xbox Live and System Link have no servers. "Go to Xbox Live" restarts the game.
- One custom soundtrack at a time.

## Reporting bugs
Open an issue with what you were doing, where you were in the game (mission name), a screenshot, and `pd-game-v2\run_user.err`. `docs/WATCHER_PROMPT.md` (also in the release zip) is a ready-made prompt for a Claude Code session that watches a play session and writes bug reports for you.

## Building from source
For development, or if you prefer not to use the prebuilt executable.

Requirements: Visual Studio 2022 or Build Tools 2022 with *Desktop development with C++* and *C++ CMake tools for Windows*, Python 3.10+, and 7-Zip if your disc is a `.7z`. About 20 GB free.

```
git clone https://github.com/<owner>/<repo>.git
cd <repo>
INSTALL.bat "C:\path\to\Phantom Dust (USA) (En,Ja).7z"
```

The installer checks the prerequisites and never installs anything silently. Then it:
1. downloads the pinned `xdvdfs` disc tool;
2. extracts and verifies your disc;
3. creates a Python environment with `capstone`;
4. regenerates the C code from your `default.xbe` (`tools/pipeline/pd_pipeline.py`);
5. builds `pd-game-v2\build\pd_recomp.exe` with CMake and Ninja (10-30 minutes; `-Jobs N` limits parallel jobs).

It is safe to re-run, and the log is `install.log`. A release zip is made with `make_release.ps1 -Version x.y.z`.

| Path | Purpose |
|---|---|
| `install.ps1`, `INSTALL.bat` | Setup (build from source, or the extract-only mode of a release) |
| `tools/pipeline/` | `default.xbe` to generated C (`pd_pipeline.py` and the stages it runs) |
| `tools/analysis/` | Research scripts: file formats, models, textures, disassembly helpers, Ghidra scripts |
| `dev/` | Test and automation scripts used while developing (game boot/drive helpers, screenshots, soak tests) |
| `pd-game-v2/` | The game project: host entry and debug console (`src/main.c`), hand-written overrides (`src/recomp_manual.c`), seed tables, CMake |
| `xboxrecomp/` | Vendored recompiler toolkit and Xbox runtime, with this project's changes |
| `make_soundtrack.py` | Custom soundtrack builder |
| `docs/` | `RECOMP_ANALYSIS.md` (engineering log: how problems were found and fixed), file formats, notes, roadmap, test results |

## How it works
`default.xbe` is disassembled and every function is translated to C, which keeps the game's own logic exact. The generated code runs on a runtime that answers the Xbox kernel calls and services the hardware the game expects. GPU pushbuffers are executed and rendered with Direct3D 11, including the register-combiner pixel pipeline as generated shaders. The APU is emulated for audio, and the USB controller stack is driven from XInput. The 60 FPS mode records each real frame's draw calls and replays them blended halfway toward the next frame. The game itself still runs at its original 30 FPS, so nothing speeds up.

## Credits
- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet: the recompiler toolkit and Xbox runtime this port is built on (MIT).
- [xemu](https://github.com/xemu-project/xemu): the MCPX APU and NV2A register sources used by the runtime (LGPL-2.1-or-later; see `xboxrecomp/NOTICE`).
- [rePOP](https://github.com/FulGerNet/rePOP) by FulGerNet: the Direct3D 11 renderer design and vertex-program translator were ported from it (MIT). None of rePOP's generated game code is used.
- [xdvdfs](https://github.com/antangelo/xdvdfs) by antangelo (MIT), [7-Zip](https://www.7-zip.org/) by Igor Pavlov (`7zr`, LGPL), and [FFmpeg](https://ffmpeg.org/) (GPL build by gyan.dev), all bundled unmodified in releases with their licences.

## AI assistance disclosure
This port was built with substantial help from **Claude**, Anthropic's AI model, used through Claude Code. That covers the reverse engineering, the runtime and renderer code, the debugging, the tooling, and this documentation. The project owner directed the work, play-tested it, and reviewed and decided what was released. Every change was tested against the running game, and the engineering log ([`RECOMP_ANALYSIS.md`](RECOMP_ANALYSIS.md)) records the evidence. AI-written code can still contain mistakes, so please report anything that looks wrong.

## Legal
Phantom Dust is © Microsoft Corporation, published by Majesco. This project is not affiliated with or endorsed by Microsoft or Majesco, and no rights to the game are granted or implied.

- **This repository** contains only original code, the third-party code credited above, and tools. It contains no game assets and no code generated from the game.
- **Release downloads** contain the compiled port executable, which is built from code translated from the game's executable. They contain no game assets: no models, textures, audio, movies, text or scripts. Everything the game shows and plays is read at run time from the disc image you supply.
- You must own the game and use your own legally obtained copy. Do not ask for or share game files.

This project's own code is MIT licensed (`LICENSE`). Third-party components keep their own licences.
