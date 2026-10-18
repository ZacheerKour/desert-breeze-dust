@echo off
rem MSVC x64 environment + CMake/Ninja from VS 2022 Build Tools (or any VS 2022 edition).
rem Usage: build_env.bat configure|build     Optional env: PD_JOBS=<n> caps parallel compile jobs; PD_VS_ROOT=<install dir> overrides discovery.
set "BT=%PD_VS_ROOT%"
if not defined BT set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%BT%\VC\Auxiliary\Build\vcvarsall.bat" (
  set "BT="
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "BT=%%i"
)
if not defined BT (
  echo ERROR: Visual Studio 2022 with the "Desktop development with C++" workload was not found.
  exit /b 1
)
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
call "%BT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cl 2>&1 | findstr /i "version"
cmake --version | findstr /i version
cd /d "%~dp0"
if "%1"=="configure" cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=/MAP -DCMAKE_C_FLAGS=/DRECOMP_ABI_CHECK || exit /b 1
if "%1"=="build" (
  if defined PD_JOBS (
    cmake --build build -- -j %PD_JOBS% -k 0
  ) else (
    cmake --build build -- -k 0
  )
  if errorlevel 1 exit /b 1
)
