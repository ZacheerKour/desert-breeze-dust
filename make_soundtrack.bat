@echo off
rem Custom soundtrack for Phantom Dust (Options > Music Manager), like the Xbox dashboard's ripped soundtracks.
rem Drag a folder of music (mp3, flac, wav, ogg, m4a, aac, wma, opus; up to 84 songs) onto this file.
rem The folder name becomes the soundtrack name. Running it again replaces the soundtrack.
setlocal
cd /d "%~dp0"
if "%~1"=="" (
  echo Drag a folder of music files onto make_soundtrack.bat, or run:  make_soundtrack.bat "C:\path\to\music"
  pause
  exit /b 1
)
if exist "%~dp0make_soundtrack.exe" (
  "%~dp0make_soundtrack.exe" "%~1"
) else (
  py -3 "%~dp0make_soundtrack.py" "%~1"
)
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (echo Done. In the game: Options ^> Music Manager, pick the soundtrack.) else (echo Failed ^(exit code %RC%^).)
pause
exit /b %RC%
