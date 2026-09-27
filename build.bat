@echo off
rem ---------------------------------------------------------------
rem  IMPORTANT: keep this file 100% ASCII.
rem  cmd.exe mis-parses a .bat containing multi-byte characters while
rem  a UTF-8 code page is active, which produces
rem      '???? is not recognized as an internal or external command'
rem  Chinese output is left to the Python script being called.
rem ---------------------------------------------------------------
chcp 65001 >nul
cd /d "%~dp0"
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

rem ---- locate a Python interpreter -------------------------------------
set "PY=python"
where python >nul 2>nul || set "PY=py"
if exist "H:\Miniconda\python.exe" set "PY=H:\Miniconda\python.exe"
if exist "%LOCALAPPDATA%\Programs\Python\Python39\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python39\python.exe"

echo [1/4] checking payload.py ...
if not exist payload.py (
    echo     payload.py missing, regenerating ...
    %PY% gen_payload.py || goto :err
) else (
    echo     payload.py present
)

echo [2/4] cleaning old build ...
if exist build rmdir /s /q build
if exist dist  rmdir /s /q dist

echo [3/4] generating icon ...
if not exist icon.ico (
    %PY% make_icon.py || goto :err
) else (
    echo     icon.ico present
)

echo [4/4] PyInstaller packaging ...
%PY% -m PyInstaller --onefile --noconsole --uac-admin ^
    --name RA3_Steam_Trainer ^
    --icon icon.ico ^
    --add-data "icon.ico;." --add-data "icon.png;." ^
    --collect-all keystone trainer.py || goto :err

echo.
echo ============================================================
echo  Done: dist\RA3_Steam_Trainer.exe
echo ============================================================
pause
exit /b 0

:err
echo.
echo Packaging failed. Make sure keystone-engine and pyinstaller are installed.
pause
exit /b 1
