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

echo.
echo  ============================================================
echo   Veterancy probe v4 (calls the game's own functions)
echo  ------------------------------------------------------------
echo   Before you start:
echo     1) be inside a skirmish game
echo     2) select exactly ONE unit (multi-select gets messy)
echo     3) the trainer has already attached to the game
echo.
echo   Step 1: call the official add-experience entry once and check
echo           whether the level rises to the cap.
echo   Step 2: keep calling until the level stops rising.
echo   Step 3: contrast note - writing the level field does nothing.
echo.
echo   This script never writes to memory.
echo   The game may stutter for 1-2 seconds - that is normal.
echo   At every prompt press Enter to continue, or q to stop.
echo  ============================================================
echo.

%PY% rank_probe2.py
if errorlevel 1 (
    echo.
    echo [NOTE] If Python was not found, run it manually, e.g.:
    echo        python rank_probe2.py
)

echo.
pause
