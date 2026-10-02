@echo off
setlocal
set VS=D:\visualstudio
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1
cd /d "%~dp0"
"%CMAKE%" -S . -B build -A Win32
if errorlevel 1 exit /b 1
"%CMAKE%" --build build --config Release
if errorlevel 1 exit /b 1
echo.
echo Built: %~dp0bin\ra3_overlay.dll
endlocal
