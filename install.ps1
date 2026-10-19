<#
.SYNOPSIS
  One-command setup for the Phantom Dust PC port: your disc image -> extracted game files -> generated C -> pd_recomp.exe.

.DESCRIPTION
  Usage:  .\install.ps1 "C:\path\to\Phantom Dust (USA) (En,Ja).7z"      (or an .iso / .xiso, or a folder that already holds default.xbe)
  Or drag your disc file onto INSTALL.bat.

  Steps: 1 prerequisites, 2 tools (xdvdfs, pinned), 3 extract disc, 4 verify default.xbe, 5 python venv (capstone pinned),
         6 generate C from default.xbe, 7 configure + build, 8 finish.
  Nothing is installed silently: missing prerequisites are reported with instructions and the script stops.
  It is safe to re-run: finished steps are detected and skipped. Any failure prints "INSTALL FAILED at step: <name>".
  No game data is shipped in this repository; you must supply your own legally obtained copy.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Source,
    [switch]$SkipBuild,
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root
# A release zip ships pd-game-v2\build\pd_recomp.exe and a PREBUILT.txt marker: then only the disc is extracted and
# verified (no Python, no Visual Studio, no code generation, no build).
$Prebuilt = (Test-Path (Join-Path $Root 'PREBUILT.txt')) -and (Test-Path (Join-Path $Root 'pd-game-v2\build\pd_recomp.exe'))

# ---- pinned inputs ----------------------------------------------------------------------------------------------
$Xdvdfs = @{
    Version = '0.8.3'
    Url     = 'https://github.com/antangelo/xdvdfs/releases/download/v0.8.3/xdvdfs-windows-1cc850bf1b3487fad7ec7c9eed01d83e8fc75ba4.zip'
    ZipSha  = '008b1b3a63f1ecb00d1ed51ab154053fd0f7c41c68e973455bbe77ab989a7cc5'
    ExeSha  = 'df5f19954ef706c130c256546bf67cf7725611bdb30d1815577187007a6f907a'
}
$CapstoneVersion = '5.0.9'
# Accepted default.xbe revisions (SHA-256). Anything else is refused: the seed tables only match this binary.
$KnownXbe = @{
    'fbac4829a44f87a2399e623e2d6347c54839184efd6dce7307deb52391f9b910' = 'Phantom Dust (USA) retail, built 2004-12-26'
}
# -----------------------------------------------------------------------------------------------------------------

$script:StepName = 'startup'
$script:StepNo = 0
function Step([string]$name) {
    $script:StepNo++
    $script:StepName = $name
    Write-Host ''
    Write-Host ("== [{0}/8] {1}" -f $script:StepNo, $name) -ForegroundColor Cyan
}
function Fail([string]$msg) { throw $msg }
function Sha256([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }
function Invoke-Native([string]$what, [scriptblock]$cmd) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # PS 5.1 turns native stderr into errors otherwise
    try { & $cmd } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)" }
}

try {
    Start-Transcript -Path (Join-Path $Root 'install.log') -Force | Out-Null
} catch { }

try {
    # ---------------------------------------------------------------------------------------------------------
    Step 'prerequisites'
    $missing = @()
    if ($Prebuilt) { Write-Host 'Prebuilt release: no compiler or Python needed.' }
    # Python 3.10+
    $py = $null
    foreach ($cand in @(@('py', '-3'), @('python'), @('python3'))) {
        $exe = Get-Command $cand[0] -ErrorAction SilentlyContinue
        if (-not $exe) { continue }
        $extra = @(); if ($cand.Count -gt 1) { $extra = $cand[1..($cand.Count - 1)] }
        $v = $null
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try { $v = & $exe.Source @extra -c "import sys;print('%d.%d' % sys.version_info[:2])" 2>$null } catch { } finally { $ErrorActionPreference = $old }
        if ($LASTEXITCODE -eq 0 -and $v -match '^(\d+)\.(\d+)$' -and ([int]$Matches[1] -gt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -ge 10))) {
            $py = @($exe.Source) + $extra
            Write-Host "Python $v : $($exe.Source)"
            break
        }
    }
    if (-not $py -and -not $Prebuilt) { $missing +="Python 3.10 or newer (python.org installer; tick 'Add python.exe to PATH', or install the 'py' launcher)." }
    # Visual Studio 2022 C++ tools + CMake + Ninja
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = $null
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if ($Prebuilt) {
        # nothing to build
    } elseif ($vsRoot) {
        $vsRoot = ($vsRoot | Select-Object -First 1)
        Write-Host "Visual Studio 2022: $vsRoot"
        $cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        $ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (-not (Test-Path $cmake) -or -not (Test-Path $ninja)) {
            $missing += "Visual Studio 2022 component 'C++ CMake tools for Windows' (provides CMake and Ninja). Open Visual Studio Installer > Modify > tick it."
        }
        $env:PD_VS_ROOT = $vsRoot
    } else {
        $missing += "Visual Studio 2022 or Build Tools 2022 with the 'Desktop development with C++' workload and the 'C++ CMake tools for Windows' component (https://visualstudio.microsoft.com/downloads/ > Tools for Visual Studio > Build Tools)."
    }
    # a Windows SDK ships with that workload; xdvdfs/7-Zip are checked in the steps that need them
    if ($missing.Count -gt 0) {
        Write-Host 'Missing prerequisites (nothing was installed):' -ForegroundColor Yellow
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
        Fail 'prerequisites missing; install them and re-run install.ps1'
    }
    $drive = (Get-Item $Root).PSDrive
    if ($drive.Free -lt 20GB) { Write-Warning ("Only {0:N1} GB free on {1}: the install needs about 20 GB (disc image copy, extracted game, build)." -f ($drive.Free / 1GB), $drive.Name) }

    # ---------------------------------------------------------------------------------------------------------
    Step 'tools (xdvdfs)'
    $xdvdfsExe = Join-Path $Root 'tools\xdvdfs\xdvdfs.exe'
    if ((Test-Path $xdvdfsExe) -and ((Sha256 $xdvdfsExe) -eq $Xdvdfs.ExeSha)) {
        Write-Host "xdvdfs $($Xdvdfs.Version) already present and verified."
    } else {
        $dl = Join-Path $Root '_downloads'
        New-Item -ItemType Directory -Force $dl | Out-Null
        $zip = Join-Path $dl 'xdvdfs.zip'
        if (-not (Test-Path $zip) -or (Sha256 $zip) -ne $Xdvdfs.ZipSha) {
            Write-Host "Downloading xdvdfs $($Xdvdfs.Version) (MIT) from github.com/antangelo/xdvdfs ..."
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -Uri $Xdvdfs.Url -OutFile $zip -UseBasicParsing
        }
        if ((Sha256 $zip) -ne $Xdvdfs.ZipSha) { Remove-Item $zip -Force; Fail 'xdvdfs download failed its SHA-256 check (file deleted)' }
        New-Item -ItemType Directory -Force (Split-Path $xdvdfsExe) | Out-Null
        Expand-Archive -LiteralPath $zip -DestinationPath (Split-Path $xdvdfsExe) -Force
        if ((Sha256 $xdvdfsExe) -ne $Xdvdfs.ExeSha) { Fail 'extracted xdvdfs.exe does not match the pinned SHA-256' }
        Write-Host 'xdvdfs installed into tools\xdvdfs (SHA-256 verified).'
    }

    # ---------------------------------------------------------------------------------------------------------
    Step 'extract disc'
    $ext = Join-Path $Root 'extracted'
    $xbe = Join-Path $ext 'default.xbe'
    $haveGood = (Test-Path $xbe) -and $KnownXbe.ContainsKey((Sha256 $xbe))
    if ($haveGood) {
        Write-Host 'extracted\default.xbe already present with a known SHA-256; skipping extraction.'
    } else {
        if (-not $Source) { Fail 'no disc given. Run:  .\install.ps1 "<path to your Phantom Dust .iso / .7z / extracted folder>"' }
        if (-not (Test-Path -LiteralPath $Source)) { Fail "source not found: $Source" }
        if ((Test-Path $xbe)) { Fail 'extracted\default.xbe exists but is not a known retail build; move extracted\ away (it is never overwritten) and re-run' }
        $Source = (Resolve-Path -LiteralPath $Source).Path
        $tmp = $null
        $iso = $null
        if (Test-Path -LiteralPath $Source -PathType Container) {
            if (-not (Test-Path (Join-Path $Source 'default.xbe'))) { Fail "folder has no default.xbe: $Source" }
            Write-Host "Copying extracted game folder $Source -> extracted\ ..."
            New-Item -ItemType Directory -Force $ext | Out-Null
            Invoke-Native 'robocopy' { robocopy $Source $ext /E /NFL /NDL /NJH /NJS /NP | Out-Null; if ($LASTEXITCODE -lt 8) { $global:LASTEXITCODE = 0 } }
        } else {
            $extn = [IO.Path]::GetExtension($Source).ToLower()
            if ($extn -eq '.7z') {
                $sz = $null
                foreach ($c in @((Get-Command 7z -ErrorAction SilentlyContinue).Source, "$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe", (Join-Path $Root 'tools\7zr\7zr.exe'))) {
                    if ($c -and (Test-Path $c)) { $sz = $c; break }
                }
                if (-not $sz) { Fail 'your disc is a .7z but 7-Zip was not found. Install 7-Zip from https://www.7-zip.org/ and re-run (or extract the .7z yourself and pass the .iso)' }
                $tmp = Join-Path $Root '_downloads\iso-tmp'
                if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
                New-Item -ItemType Directory -Force $tmp | Out-Null
                Write-Host "Unpacking $Source with $sz (about 4.3 GB; this takes a few minutes) ..."
                Invoke-Native '7-Zip' { & $sz x -y "-o$tmp" -- $Source '*.iso' '*.xiso' | Out-Null }
                $iso = Get-ChildItem -Path $tmp -Recurse -File | Where-Object { $_.Extension -in '.iso', '.xiso' } | Select-Object -First 1
                if (-not $iso) { Fail 'no .iso found inside the 7z archive' }
                $iso = $iso.FullName
            } elseif ($extn -in '.iso', '.xiso') {
                $iso = $Source
            } else {
                Fail "unsupported source type '$extn' (use .iso, .xiso, .7z or a folder containing default.xbe)"
            }
            Write-Host "Extracting $iso with xdvdfs -> extracted\ ..."
            New-Item -ItemType Directory -Force $ext | Out-Null
            Invoke-Native 'xdvdfs unpack' { & $xdvdfsExe unpack $iso $ext }
            if ($tmp -and (Test-Path $tmp)) { Remove-Item -Recurse -Force $tmp }
        }
    }

    # ---------------------------------------------------------------------------------------------------------
    Step 'verify default.xbe'
    if (-not (Test-Path $xbe)) { Fail 'extracted\default.xbe is missing after extraction (is this really a Phantom Dust disc?)' }
    $h = Sha256 $xbe
    if ($KnownXbe.ContainsKey($h)) {
        Write-Host "default.xbe OK: $($KnownXbe[$h])  (sha256 $h)"
    } else {
        Fail "default.xbe SHA-256 $h is not a supported revision. Only the USA retail disc is supported (known: $($KnownXbe.Keys -join ', '))."
    }

    # ---------------------------------------------------------------------------------------------------------
    if ($Prebuilt) {
        Step 'finish'
        Write-Host ''
        Write-Host 'Install complete. Start the game by double-clicking:  PLAY.bat' -ForegroundColor Green
        Write-Host 'Keys (laptop layout): WASD move, Space jump, Tab lock on, 1-4 skills, Enter pause. A controller also works.'
        try { Stop-Transcript | Out-Null } catch { }
        exit 0
    }
    Step 'python venv'
    $venv = Join-Path $Root '.venv-recomp'
    $vpy = Join-Path $venv 'Scripts\python.exe'
    if (-not (Test-Path $vpy)) {
        Write-Host 'Creating .venv-recomp ...'
        $pyExe = $py[0]; $pyArgs = @(); if ($py.Count -gt 1) { $pyArgs = $py[1..($py.Count - 1)] }
        Invoke-Native 'venv creation' { & $pyExe @pyArgs -m venv $venv }
    }
    $have = ''
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $have = & $vpy -c "import capstone;print(capstone.__version__)" 2>$null } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0 -or $have -ne $CapstoneVersion) {
        Write-Host "Installing capstone==$CapstoneVersion into the venv ..."
        Invoke-Native 'pip install capstone' { & $vpy -m pip install --disable-pip-version-check "capstone==$CapstoneVersion" }
    } else {
        Write-Host "capstone $have already installed."
    }

    # ---------------------------------------------------------------------------------------------------------
    Step 'generate C code (several minutes)'
    $work = Join-Path $Root 'recomp-analysis'
    Invoke-Native 'pd_pipeline.py' { & $vpy (Join-Path $Root 'pd_pipeline.py') --xbe $xbe --work $work --if-stale }

    # ---------------------------------------------------------------------------------------------------------
    Step 'configure + build'
    if ($SkipBuild) {
        Write-Host '-SkipBuild given; skipping.'
    } else {
        $proj = Join-Path $Root 'pd-game-v2'
        $env:PD_JOBS = [string]$Jobs
        if (-not (Test-Path (Join-Path $proj 'build\CMakeCache.txt'))) {
            Invoke-Native 'cmake configure' { cmd /c "`"$proj\build_env.bat`" configure" }
        }
        Write-Host "Building with up to $Jobs parallel jobs (the generated code is large: expect 10-30 minutes) ..."
        Invoke-Native 'cmake build' { cmd /c "`"$proj\build_env.bat`" build" }
        $exe = Join-Path $proj 'build\pd_recomp.exe'
        if (-not (Test-Path $exe)) { Fail 'build finished but pd_recomp.exe was not produced' }
        Write-Host ("Built {0} ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB))
    }

    # ---------------------------------------------------------------------------------------------------------
    Step 'finish'
    if (-not $SkipBuild) {
        Write-Host ''
        Write-Host 'Install complete. Start the game with:  pd-game-v2\run_game.bat' -ForegroundColor Green
        Write-Host 'Keys: Enter=Start, Z=A, X=B, arrow keys=D-pad (a gamepad also works).'
    } else {
        Write-Host 'Generation done; build skipped.'
    }
}
catch {
    Write-Host ''
    Write-Host ("INSTALL FAILED at step: {0}" -f $script:StepName) -ForegroundColor Red
    Write-Host ("Reason: {0}" -f $_.Exception.Message) -ForegroundColor Red
    try { Stop-Transcript | Out-Null } catch { }
    exit 1
}
try { Stop-Transcript | Out-Null } catch { }
exit 0
