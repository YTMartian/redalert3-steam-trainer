# -*- coding: utf-8 -*-
"""把项目里所有 .bat 重写成 100% ASCII 版本。

背景：cmd.exe 在 UTF-8 代码页（chcp 65001）下解析**含多字节字符的 .bat**
时会按字节切分 echo 行，把中文碎片当成命令执行，于是出现
    '???? is not recognized as an internal or external command'
并在某些行崩溃。解决办法：.bat 只保留 ASCII，
中文提示全部交给被调用的 Python 脚本（它们自己设了 PYTHONUTF8 / PYTHONIOENCODING）。

本脚本幂等：随时可重跑，会覆盖生成以下文件。
"""
import io
import os

HEADER = """@echo off
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
if exist "H:\\Miniconda\\python.exe" set "PY=H:\\Miniconda\\python.exe"
if exist "%LOCALAPPDATA%\\Programs\\Python\\Python39\\python.exe" set "PY=%LOCALAPPDATA%\\Programs\\Python\\Python39\\python.exe"
"""

FOOTER = """
echo.
pause
"""

BATS = {
    # 文件名 -> (批处理体, Python 脚本名，用于报错提示)
    '定位单位星级.bat': ("""
echo.
echo  ============================================================
echo   Locate the unit veterancy field (READ-ONLY)
echo  ------------------------------------------------------------
echo   It never writes to game memory, so it cannot crash the game.
echo.
echo   Before you start:
echo     1) be inside a skirmish game
echo     2) select exactly ONE unit and keep it selected
echo     3) run this script as Administrator
echo.
echo   How it works:
echo     first run  -> saves snapshot A
echo     level the unit up in game (kill enemies / grab a crate)
echo     run again  -> compares automatically
echo  ============================================================
echo.
""", 'find_veterancy.py'),

    '探测星级字段.bat': ("""
echo.
echo  ============================================================
echo   Veterancy field probe (read, plus optional write test)
echo  ------------------------------------------------------------
echo   Before you start:
echo     1) be inside a skirmish game
echo     2) select exactly ONE unit and keep it selected
echo     3) the trainer has already attached to the game
echo     4) run this script as Administrator
echo.
echo   Flow: read --^> click "Promote" in the trainer --^> read again
echo         --^> it diffs automatically. Afterwards you may write a
echo         large candidate experience value to see what changes.
echo  ============================================================
echo.
""", 'rank_probe.py'),

    '验证星级机制.bat': ("""
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
""", 'rank_probe2.py'),

    '诊断单位偏移.bat': ("""
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
""", 'diag_unit.py'),

    '定位单位管理器.bat': ("""
echo.
echo  ============================================================
echo   Selection-manager locator (no Cheat Engine needed)
echo  ------------------------------------------------------------
echo   Before you start:
echo     1) be inside a skirmish game
echo     2) have at least 4 selectable units built
echo     3) run this script as Administrator
echo  ------------------------------------------------------------
echo   Flow: follow the prompts and select 4 units, press Enter,
echo         then 3 units, Enter, then 2, then 1.
echo  ============================================================
echo.
""", 'find_unit_mgr.py'),

    '定位星级代码.bat': ("""
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
""", 'dump_module.py/scan_vet_code.py'),
}

# 这些脚本要额外跑一步/多步，单独处理
EXTRA_BODY = {
    '定位星级代码.bat': """
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
""",
}

TEMPLATE_RUN = """
%%PY%% %(script)s
if errorlevel 1 (
    echo.
    echo [NOTE] If Python was not found, run it manually, e.g.:
    echo        python %(script)s
)
"""


def main():
    written = []
    for name, (body, script) in BATS.items():
        if name in EXTRA_BODY:
            run = '\n' + EXTRA_BODY[name]
        else:
            run = TEMPLATE_RUN % {'script': script}
        text = HEADER + body + run + FOOTER
        data = text.replace('\r\n', '\n').encode('ascii')      # 关键：必须是 ASCII
        data = data.replace(b'\n', b'\r\n')                    # .bat 用 CRLF
        io.open(name, 'wb').write(data)
        written.append((name, len(data)))

    # build.bat 也一样处理
    build = HEADER + """
echo [1/3] checking payload.py ...
if not exist payload.py (
    echo     payload.py missing, regenerating ...
    %PY% gen_payload.py || goto :err
) else (
    echo     payload.py present
)

echo [2/3] cleaning old build ...
if exist build rmdir /s /q build
if exist dist  rmdir /s /q dist

echo [3/3] PyInstaller packaging ...
%PY% -m PyInstaller --onefile --noconsole --uac-admin --name RA3_Steam_Trainer --collect-all keystone trainer.py || goto :err

echo.
echo ============================================================
echo  Done: dist\\RA3_Steam_Trainer.exe
echo ============================================================
pause
exit /b 0

:err
echo.
echo Packaging failed. Make sure keystone-engine and pyinstaller are installed.
pause
exit /b 1
"""
    data = build.replace('\r\n', '\n').encode('ascii').replace(b'\n', b'\r\n')
    io.open('build.bat', 'wb').write(data)
    written.append(('build.bat', len(data)))

    for n, sz in sorted(written):
        b = io.open(n, 'rb').read()
        b.decode('ascii')                     # 断言：必须能按 ASCII 解码
        print('%-26s %5d bytes  ASCII OK' % (n, sz))


if __name__ == '__main__':
    main()
