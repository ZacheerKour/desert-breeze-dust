@echo off
set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%BT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >NUL 2>&1
dumpbin /symbols "%~1" | findstr /i "External" | findstr /v "UNDEF"
