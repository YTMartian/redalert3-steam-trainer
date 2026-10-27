@echo off
rem Build the native injector: overlay\bin\ra3_injector.exe (Win32 / x86).
rem
rem Native replacement for the PyInstaller main.exe, step 1: find the game
rem process, unload an older overlay, inject the DLL with LoadLibraryW, then
rem show the result in a message box. Payload DLLs are embedded as resources;
rem the exe only writes them next to itself, so no temp directory is used.
rem
rem IMPORTANT: the injector embeds a copy of ra3_overlay_v4.dll and writes it
rem back next to itself on every run. Running an old injector therefore
rem DOWNGRADES a freshly built DLL. So this script rebuilds the DLL first
rem (call with "norebuild" to skip that and reuse bin as-is).
rem
rem Needs overlay\bin\arm_mustcode.dll (packed by build_injector.bat).
setlocal EnableExtensions EnableDelayedExpansion

if /i "%~1" neq "norebuild" (
  echo [native] rebuilding ra3_overlay_v4.dll first ...
  call "%~dp0build.bat"
  if errorlevel 1 exit /b 1
)

if not exist "%~dp0bin\ra3_overlay_v4.dll" (
  echo [native] missing bin\ra3_overlay_v4.dll - run overlay\build.bat first.
  exit /b 1
)
if not exist "%~dp0bin\arm_mustcode.dll" (
  echo [native] missing bin\arm_mustcode.dll - run overlay\build_injector.bat first.
  exit /b 1
)

set "VSROOT=%RA3_VS%"
if not defined VSROOT set "VSROOT=%VS%"
if not defined VSROOT (
  for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined VSROOT if exist "%%D:\visualstudio\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%%D:\visualstudio"
  )
)
if not defined VSROOT (
  for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VSROOT if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%ProgramFiles%\Microsoft Visual Studio\2022\%%E"
    if not defined VSROOT if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E"
    if not defined VSROOT if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%E\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%E"
  )
)
if not defined VSROOT (
  echo [native] Visual Studio not found.
  echo [native] Point RA3_VS at the install root and retry, e.g.
  echo [native]     set RA3_VS=H:\visualstudio
  exit /b 1
)
echo [native] Visual Studio: !VSROOT!

call "!VSROOT!\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1

set "CMAKE=!VSROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "!CMAKE!" set "CMAKE=cmake"

cd /d "%~dp0"
"%CMAKE%" -S . -B build -A Win32
if errorlevel 1 exit /b 1
"%CMAKE%" --build build --config Release --target ra3_injector
if errorlevel 1 exit /b 1

echo.
echo Built: %~dp0bin\ra3_injector.exe
echo It writes ra3_overlay_v4.dll and arm_mustcode.dll beside itself on first run.
echo No temp directory is used.
endlocal
