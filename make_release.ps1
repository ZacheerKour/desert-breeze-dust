<#
.SYNOPSIS
  Packages a prebuilt release zip: the built exe plus everything a player needs to install from their own disc,
  without Python or Visual Studio.  Usage:  .\make_release.ps1 -Version 0.1.0
  Output: dist\PhantomDust-PC-<version>.zip (no game data inside; the player supplies their disc to INSTALL.bat).
#>
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$exe = Join-Path $Root 'pd-game-v2\build\pd_recomp.exe'
if (-not (Test-Path $exe)) { throw 'pd-game-v2\build\pd_recomp.exe not built' }
$dirty = git status --porcelain --untracked-files=no
if ($dirty) { throw "working tree has uncommitted changes; commit first so the release matches a commit:`n$dirty" }
$commit = (git rev-parse --short HEAD).Trim()

# The exe's only non-Windows dependency (dumpbin /dependents): the VC++ runtime, redistributable by Microsoft.
$redist = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT\vcruntime140.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $redist) { throw 'vcruntime140.dll not found in the VS 2022 redist folder' }

$name = "PhantomDust-PC-$Version"
$stage = Join-Path $Root "dist\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\pd-game-v2\build", "$stage\tools\xdvdfs", "$stage\tools\7zr" | Out-Null

Copy-Item $exe "$stage\pd-game-v2\build\"
Copy-Item $redist.FullName "$stage\pd-game-v2\build\"
foreach ($f in 'settings.ini', 'Settings.bat', 'INSTALL.bat', 'install.ps1', 'LICENSE') { Copy-Item (Join-Path $Root $f) $stage }
# these two live in docs\ and dev\ in the repository but stay at the zip root (WATCHER_PROMPT.md refers to capwin_i.ps1 there)
Copy-Item (Join-Path $Root 'docs\WATCHER_PROMPT.md') $stage
Copy-Item (Join-Path $Root 'dev\capwin_i.ps1') $stage
foreach ($f in 'play_watched.bat', 'run_game.bat') { Copy-Item (Join-Path $Root "pd-game-v2\$f") "$stage\pd-game-v2\" }
Copy-Item (Join-Path $Root 'README.md') "$stage\README.md"
Copy-Item (Join-Path $Root 'PLAY.bat') $stage
# Licence texts of code compiled into the exe: xboxrecomp (MIT) and its xemu-derived parts (LGPL-2.1-or-later).
New-Item -ItemType Directory -Force "$stage\licenses\xboxrecomp" | Out-Null
foreach ($f in 'LICENSE', 'NOTICE') { Copy-Item (Join-Path $Root "xboxrecomp\$f") "$stage\licenses\xboxrecomp\" }
Copy-Item (Join-Path $Root 'xboxrecomp\LICENSES') "$stage\licenses\xboxrecomp\" -Recurse

# Disc tools, bundled so the install needs no downloads: xdvdfs (MIT, the installer's pinned build) and 7zr (LGPL, .7z only).
$xd = Join-Path $Root 'tools\xdvdfs'
if (-not (Test-Path "$xd\xdvdfs.exe")) { throw 'tools\xdvdfs\xdvdfs.exe missing: run install.ps1 once first' }
Copy-Item "$xd\*" "$stage\tools\xdvdfs\" -Recurse
$sz = Join-Path $Root '_downloads\7zr.exe'
if (-not (Test-Path $sz)) {
    New-Item -ItemType Directory -Force (Split-Path $sz) | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri 'https://www.7-zip.org/a/7zr.exe' -OutFile $sz -UseBasicParsing
}
Copy-Item $sz "$stage\tools\7zr\7zr.exe"
Set-Content -Path "$stage\tools\7zr\LICENSE.txt" -Encoding ascii -Value '7zr.exe is the standalone 7-Zip console extractor for .7z (7-Zip by Igor Pavlov, GNU LGPL). Source: https://www.7-zip.org/'

# Custom soundtrack tool (Options > Music Manager): frozen make_soundtrack.py + ffmpeg (GPL build, license and README kept).
$st = Join-Path $Root 'dist\st\make_soundtrack.exe'
if (-not (Test-Path $st) -or (Get-Item $st).LastWriteTime -lt (Get-Item (Join-Path $Root 'make_soundtrack.py')).LastWriteTime) {
    $vpy = Join-Path $Root '.venv-recomp\Scripts\python.exe'
    & $vpy -m pip install -q --disable-pip-version-check pyinstaller
    & $vpy -m PyInstaller --onefile --clean --noconfirm --distpath (Join-Path $Root 'dist\st') --workpath (Join-Path $Root 'dist\st_build') --specpath (Join-Path $Root 'dist\st_build') --name make_soundtrack (Join-Path $Root 'make_soundtrack.py') | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $st)) { throw 'PyInstaller build of make_soundtrack.exe failed' }
}
Copy-Item $st $stage
Copy-Item (Join-Path $Root 'make_soundtrack.bat') $stage
$ff = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg.Essentials*\*\bin\ffmpeg.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $ff) { throw 'ffmpeg not found: winget install --id Gyan.FFmpeg.Essentials' }
New-Item -ItemType Directory -Force "$stage\tools\ffmpeg" | Out-Null
Copy-Item $ff.FullName "$stage\tools\ffmpeg\"
foreach ($f in 'LICENSE', 'README.txt') { $p = Join-Path $ff.Directory.Parent.FullName $f; if (Test-Path $p) { Copy-Item $p "$stage\tools\ffmpeg\" } }

$exeSha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$stage\PREBUILT.txt" -Encoding ascii -Value @(
    "Phantom Dust PC port, prebuilt release $Version",
    "commit $commit",
    "pd_recomp.exe sha256 $exeSha",
    'No game data is included. INSTALL.bat extracts and verifies your own Phantom Dust (USA) disc.')

$zip = Join-Path $Root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Write-Host ("{0}  ({1:N1} MB)  commit {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $commit)
