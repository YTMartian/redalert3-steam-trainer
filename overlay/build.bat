@echo off
rem Build ra3_overlay_v4.dll (Win32 / x86 only).
rem
rem No drive letter is hardcoded. Visual Studio is located in this order:
rem   1. env RA3_VS   (or VS)  - explicit override, wins always
rem   2. <drive>:\visualstudio
rem   3. %ProgramFiles%/%ProgramFiles(x86)% standard 2022/2019 layouts
setlocal EnableExtensions EnableDelayedExpansion

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
  echo [build] Visual Studio not found.
  echo [build] Point RA3_VS at the install root and retry, e.g.
  echo [build]     set RA3_VS=H:\visualstudio
  exit /b 1
)
echo [build] Visual Studio: !VSROOT!

call "!VSROOT!\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1

rem Prefer the CMake that ships with VS (it knows the current VS generator).
set "CMAKE=!VSROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "!CMAKE!" set "CMAKE=cmake"

cd /d "%~dp0"

rem A CMake cache stores absolute compiler/linker/cmake paths from the machine
rem that created it. After moving or reinstalling VS those paths dangle and the
rem configure step fails, so drop the cache and let CMake regenerate it.
if exist "build\CMakeCache.txt" (
  set "CACHED_CMAKE="
  for /f "usebackq tokens=2 delims==" %%P in (`findstr /C:"CMAKE_COMMAND:INTERNAL=" "build\CMakeCache.txt"`) do set "CACHED_CMAKE=%%P"
  if defined CACHED_CMAKE if not exist "!CACHED_CMAKE!" (
    echo [build] stale CMake cache ^(missing: !CACHED_CMAKE!^) - regenerating
    del /q "build\CMakeCache.txt" 2>nul
    rmdir /s /q "build\CMakeFiles" 2>nul
  )
)

python "%~dp0tools\pack_unit_images.py" "%~dp0..\unit_images" "%~dp0build"
if errorlevel 1 exit /b 1
"%CMAKE%" -S . -B build -A Win32
if errorlevel 1 exit /b 1
"%CMAKE%" --build build --config Release
if errorlevel 1 exit /b 1
rem Keep name tables beside the DLL for portable installs.
if exist "%~dp0..\unit_names_csf.txt" copy /Y "%~dp0..\unit_names_csf.txt" "%~dp0bin\unit_names_csf.txt" >nul
if exist "%~dp0..\unit_names.txt" copy /Y "%~dp0..\unit_names.txt" "%~dp0bin\unit_names.txt" >nul
if exist "%~dp0bin\unit_names_csf.txt" if not exist "%~dp0..\unit_names_csf.txt" copy /Y "%~dp0bin\unit_names_csf.txt" "%~dp0..\unit_names_csf.txt" >nul
echo.
echo Built: %~dp0bin\ra3_overlay_v4.dll
echo Name tables: %~dp0bin\unit_names*.txt
echo Icons: packed into the DLL from ..\unit_images
endlocal
