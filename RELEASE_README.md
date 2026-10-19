# Phantom Dust PC port: prebuilt release

A native Windows port of Phantom Dust (original Xbox, 2004), made by static recompilation. **No game data is included:** you need your own copy of the game, specifically the USA retail disc as an .iso or the usual `Phantom Dust (USA) (En,Ja).7z`.

## Requirements
- Windows 10 or 11, 64-bit, with a DirectX 11 GPU. A laptop should be plugged in.
- About 10 GB free disk space.
- Nothing else to install: no Visual Studio, no Python, no 7-Zip.

## Install (once, about 5 minutes)
1. Unzip this folder somewhere simple, e.g. `C:\Games\PhantomDust` (avoid OneDrive or Program Files).
2. Drag your disc file (`.7z` or `.iso`) onto **INSTALL.bat**, or run `INSTALL.bat "D:\path\to\Phantom Dust (USA) (En,Ja).7z"`. It extracts the disc into `extracted\` and checks that it is the supported USA retail version.

## Play
Double-click **pd-game-v2\play_watched.bat** (`run_game.bat` is the same without the log and debug-console files).

- **Controller:** any Xbox/XInput pad.
- **Keyboard (laptop layout):** WASD move, Q/E turn camera, R/F tilt camera, Space jump, Tab or right mouse lock on, 1 2 3 4 skill slots (1 or left mouse also confirms), Enter/Esc pause, Backspace back, arrow keys in menus, Shift left trigger, Ctrl right-stick click.
- **Window:** F11 or Alt+Enter for full screen. Keep the window in front; Windows slows down covered windows.
- **First start:** press Enter at the title, then Create New Player. Profiles are saved in `extracted\UDATA` (back that folder up).
- **Frame rate:** battles show 60 FPS (the game runs at 30 and the port draws in-between frames). The title bar shows `FPS: 60.0 (game 30.0)`. Busy hub areas may run around 50.
- **First minutes:** some stutter while shaders compile; they are cached in `pd-game-v2\bin\shadercache` after that.

## Custom soundtrack (your own battle music)
Like the Xbox dashboard's ripped soundtracks:
1. Put up to 84 songs (mp3, flac, wav, ogg, m4a, aac, wma or opus) in a folder. The folder name becomes the soundtrack name.
2. Drag that folder onto **make_soundtrack.bat** (after INSTALL.bat has been run). It converts the songs to the Xbox's format; nothing else to install.
3. In the game: **Options Mode > Music Manager**. Pick a stage (Highway, Palace, ...), then your soundtrack, then a song. That song plays in battles on that stage.

Running it again with another folder replaces the soundtrack (one soundtrack at a time for now).

## Reporting problems
Note what you were doing, take a screenshot, and keep `pd-game-v2\run_user.err`. `WATCHER_PROMPT.md` is a ready prompt for a Claude Code session that watches you play and files bugs.

## Known issues
- Not every mission has been played through to the end yet. Report anything that crashes or hangs.
- Split Screen: Player 1 waits at "Please wait..." until a second controller signs in.
- "Go to Xbox Live" restarts the game (there is no online service).
- Under heavy load the controller can stop responding for a few seconds and then recover by itself (seen once in a 1-hour test; being fixed).
- Starting a quest through the debug console after another quest can crash (the normal Battle Terminal route is fine).
