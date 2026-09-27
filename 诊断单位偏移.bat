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
echo   Unit offset diagnostics (READ-ONLY)
echo  ------------------------------------------------------------
echo   It never writes to game memory, so it cannot crash the game.
echo.
echo   Before you start:
echo     1) be inside a skirmish game
echo     2) select ONE unit
echo     3) run this script as Administrator
echo  ============================================================
echo.

%PY% diag_unit.py
if errorlevel 1 (
    echo.
    echo [NOTE] If Python was not found, run it manually, e.g.:
    echo        python diag_unit.py
)

echo.
pause
