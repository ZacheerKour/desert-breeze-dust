# Phantom Dust PC port: install and play on a fresh Windows PC

## Easiest: the prebuilt release (no compiler, about 5 minutes)
1. You need access to the private repo (the owner adds your GitHub account under Settings > Collaborators) and your own Phantom Dust (USA) disc image (`.iso` or `Phantom Dust (USA) (En,Ja).7z`).
2. Download the newest `PhantomDust-PC-<version>.zip` from the repo's **Releases** page (github.com/ZacheerKour/desert-breeze-dust/releases), and unzip it somewhere simple like `C:\Games`.
3. Drag your disc file onto `INSTALL.bat`.
4. Play: double-click `pd-game-v2\play_watched.bat`. Controls and known issues are in the zip's README.

The rest of this page is the build-from-source route (for development).

Status and known issues: see "Known issues" at the end (kept current by the PM).

## 1. Install the tools (one time, about 30-45 minutes, mostly downloads)
Open **PowerShell** (Start menu, type "PowerShell") and run these one at a time. Accept the UAC prompts.

```
winget install --id Git.Git -e
winget install --id GitHub.cli -e
winget install --id Python.Python.3.13 -e
winget install --id 7zip.7zip -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.CMake.Project"
```
Close and reopen PowerShell afterwards so the new programs are on the PATH.

## 2. Get the code
```
gh auth login
```
Pick GitHub.com, HTTPS, "Login with a web browser", and sign in as the account that owns the private repo.
```
mkdir C:\recomp
cd C:\recomp
gh repo clone ZacheerKour/desert-breeze-dust phantom-dust-re
```
Use exactly `C:\recomp\phantom-dust-re` (no spaces in the path; some test scripts need that).

## 3. Build the game from your own disc
Copy your disc image `Phantom Dust (USA) (En,Ja).7z` (or the .iso) to the PC, e.g. into Downloads. Then:
```
cd C:\recomp\phantom-dust-re
.\INSTALL.bat "$env:USERPROFILE\Downloads\Phantom Dust (USA) (En,Ja).7z"
```
(Or drag the .7z onto INSTALL.bat in Explorer.) It checks the tools, extracts the disc, verifies it is the USA retail disc, generates the C code and builds `pd-game-v2\build\pd_recomp.exe`. First build: 10-30 minutes. It is safe to re-run. If it stops, the last lines say which step failed and what to install.

## 4. Play
Double-click `C:\recomp\phantom-dust-re\pd-game-v2\play_watched.bat`.
- Keyboard ("MMO" layout, for laptops without a numpad): **WASD** move, **Q/E** turn camera, **R/F** tilt camera, **Space** jump, **Tab** or **right mouse** lock on, **1 2 3 4** skill slots (A B X Y; **1** or **left mouse** also confirms), **Enter/Esc** pause, **Backspace** back, **arrows** in menus, **Shift** left trigger, **Ctrl** right-stick click.
- An Xbox/XInput controller also works and is the more comfortable option.
- First start: at the title press Enter, then Create New Player (or load a profile).
- The game window: F11 or Alt+Enter toggles full screen. Keep the window in front: a covered window is throttled.
- `play_watched.bat` also writes `pd-game-v2\run_user.err` (log) and reads `pd-game-v2\mem_live_user.txt` (debug console), so a Claude session can watch and help. `run_game.bat` is the same without that.

## 5. Updating later
```
cd C:\recomp\phantom-dust-re
git pull
.\INSTALL.bat "$env:USERPROFILE\Downloads\Phantom Dust (USA) (En,Ja).7z"
```
The re-run only rebuilds what changed. Your profiles live in `extracted\UDATA` and are never touched by updates; back that folder up now and then.

## 6. Watch-along prompt for a Claude Code session on that PC
Open Claude Code in `C:\recomp\phantom-dust-re` and paste the text in `WATCHER_PROMPT.md`.

## Frame rate
The game itself runs at 30 frames per second, as on the Xbox. The PC port draws an in-between frame for each pair of real frames, so battles show **60 FPS** at normal game speed. The window title shows `FPS: 60.0 (game 30.0)` when that is happening. In the busiest parts of the hub the game cannot hold its own 30, and the port then falls back to plain 30. `RECOMP_INTERP=0` (set in the .bat before the exe line) turns the in-between frames off. A laptop should be plugged in, and the game window kept in front.

## Known issues
(filled in by the PM at hand-off)
