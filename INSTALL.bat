@echo off
rem Phantom Dust PC port installer. Drag your disc image (.7z / .iso / .xiso) onto this file, or run:  INSTALL.bat "path\to\disc"
setlocal
cd /d "%~dp0"
set "SRC=%~1"
if "%SRC%"=="" (
  echo Drag your Phantom Dust disc file (.7z, .iso or .xiso^) onto INSTALL.bat, or enter its full path.
  set /p "SRC=Path: "
)
if defined SRC set "SRC=%SRC:"=%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Source "%SRC%"
set RC=%ERRORLEVEL%
echo.
if not "%RC%"=="0" (echo Installer failed ^(exit code %RC%^). See install.log.) else (echo Done.)
pause
exit /b %RC%
