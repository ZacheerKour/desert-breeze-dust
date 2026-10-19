@echo off
set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%BT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /w audit_narrow_gen.c /Fe:audit_narrow_gen.exe >audit_narrow_cl.log 2>&1
if errorlevel 1 ( type audit_narrow_cl.log & exit /b 1 )
"%~dp0audit_narrow_gen.exe"
