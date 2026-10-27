@echo off
rem Build injector EXE + arm_mustcode EXE into overlay\bin
chcp 65001 >nul
setlocal EnableExtensions
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

cd /d "%~dp0.."
set "REPO=%CD%"

rem Always rebuild the overlay DLL first, then embed it. Otherwise main.exe
rem would keep shipping a stale ra3_overlay_v4.dll (new UI panels would be
rem missing at runtime even though the DLL on disk is up to date).
if /i "%~1" neq "norebuild" (
  echo [0/4] rebuilding ra3_overlay_v4.dll ...
  call "%~dp0build.bat" || goto :err
)

set "PY=python"
where python >nul 2>nul || set "PY=py"
if exist "%LOCALAPPDATA%\Programs\Python\Python39\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python39\python.exe"

set "ICON=%REPO%\overlay\assets\ra3_overlay_icon.ico"
set "KS_DLL=%REPO%\overlay\build\keystone.dll"
"%PY%" -c "import keystone,os,shutil; src=os.path.join(os.path.dirname(keystone.__file__),'keystone.dll'); dst=r'%KS_DLL%'; os.makedirs(os.path.dirname(dst),exist_ok=True); shutil.copy2(src,dst); print(dst)"
if not exist "%KS_DLL%" (
  echo keystone.dll copy failed
  goto :err
)
if not exist "%ICON%" (
  echo icon not found: %ICON%
  goto :err
)

echo [1/4] ensure payload.py ...
if not exist "%REPO%\payload.py" (
  "%PY%" "%REPO%\gen_payload.py" || goto :err
)

echo [2/4] packaging arm_mustcode.dll ...
"%PY%" -m PyInstaller --clean --noconfirm --onefile --noconsole --noupx --name arm_mustcode --icon "%ICON%" --distpath "%REPO%\overlay\bin" --workpath "%REPO%\overlay\build\pyi_arm" --specpath "%REPO%\overlay\build" --hidden-import keystone --hidden-import keystone.keystone --hidden-import keystone.keystone_const --hidden-import payload --hidden-import mustcode_asm --collect-submodules keystone --add-binary "%KS_DLL%;keystone" --add-data "%REPO%\payload.py;." --add-data "%REPO%\mustcode_asm.py;." "%REPO%\overlay\tools\arm_mustcode.py"
if errorlevel 1 goto :err
move /Y "%REPO%\overlay\bin\arm_mustcode.exe" "%REPO%\overlay\bin\arm_mustcode.dll" >nul
if errorlevel 1 goto :err

if not exist "%REPO%\overlay\bin\ra3_overlay_v4.dll" (
  echo Missing overlay\bin\ra3_overlay_v4.dll. Run overlay\build.bat first.
  goto :err
)

echo [3/4] packaging main.exe (native) ...
rem main.exe 现在是原生注入器（overlay\build_injector_native.bat 产出的那份）：
rem 载荷编在资源段里，启动时只把两个 DLL 写到 exe 旁边，不需要临时目录，也就不会
rem 再弹 PyInstaller 那句 "INTERNAL ERROR: cannot create temporary directory!"。
rem 这里只是转调它；norebuild = DLL 上面已经编过了，别重复跑 build.bat。
call "%~dp0build_injector_native.bat" norebuild
if errorlevel 1 goto :err

echo [4/4] done
echo.
echo Done:
echo   overlay\bin\main.exe        (native injector, needs admin - UAC)
echo   overlay\bin\arm_mustcode.dll (MustCode assembler, loaded by the overlay DLL)
echo Ship main.exe alone. It writes the two DLLs beside itself on first run;
echo no temp directory, no Python, no unpacking.
exit /b 0

:err
echo BUILD FAILED
exit /b 1
