@echo off
setlocal
set VS=D:\visualstudio
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1
cd /d "%~dp0"
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
