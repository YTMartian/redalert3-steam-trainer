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
echo   Locate the promotion code (read-only dump + static analysis)
echo  ------------------------------------------------------------
echo   Step 1: read a memory image of the game module (read-only)
echo   Step 2: offline static analysis - find code referencing
echo           [entity+0x3CC] and the writes to +0x24 / +0x10
echo  ------------------------------------------------------------
echo   Before you start: the game just needs to be running
echo   (no need to be in a skirmish); run as Administrator
echo  ============================================================
echo.


echo [1/2] dumping the game module image ...
%PY% dump_module.py
if errorlevel 1 (
    echo.
    echo [!] dump failed. If attaching failed, make sure the game is running.
    goto :end
)

echo.
echo [2/2] static analysis ...
%PY% scan_vet_code.py

:end
echo.
echo  Result file: scan_vet_code_result.txt
echo  Send it to the developer to continue.

echo.
pause
