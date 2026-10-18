@echo off
rem Scratch helper: MSVC x64 environment + CMake/Ninja from VS Build Tools 2022.
set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
call "%BT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cl 2>&1 | findstr /i "version"
cmake --version | findstr /i version
cd /d "%~dp0"
if "%1"=="configure" cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=/MAP -DCMAKE_C_FLAGS=/DRECOMP_ABI_CHECK
if "%1"=="build" cmake --build build -- -k 0
