@echo off
rem Build injector EXE + arm_mustcode EXE into overlay\bin
chcp 65001 >nul
setlocal EnableExtensions
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

cd /d "%~dp0.."
set "REPO=%CD%"

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

echo [1/3] ensure payload.py ...
if not exist "%REPO%\payload.py" (
  "%PY%" "%REPO%\gen_payload.py" || goto :err
)

echo [2/3] packaging RA3_Overlay_Inject.exe ...
"%PY%" -m PyInstaller --noconfirm --onefile --noconsole --uac-admin --name RA3_Overlay_Inject --icon "%ICON%" --distpath "%REPO%\overlay\bin" --workpath "%REPO%\overlay\build\pyi_inject" --specpath "%REPO%\overlay\build" "%REPO%\overlay\tools\inject.py"
if errorlevel 1 goto :err

echo [3/3] packaging arm_mustcode.exe ...
"%PY%" -m PyInstaller --noconfirm --onefile --noconsole --name arm_mustcode --icon "%ICON%" --distpath "%REPO%\overlay\bin" --workpath "%REPO%\overlay\build\pyi_arm" --specpath "%REPO%\overlay\build" --hidden-import keystone --hidden-import keystone.keystone --hidden-import keystone.keystone_const --hidden-import payload --hidden-import trainer --collect-submodules keystone --add-binary "%KS_DLL%;keystone" --add-data "%REPO%\payload.py;." --add-data "%REPO%\trainer.py;." "%REPO%\overlay\tools\arm_mustcode.py"
if errorlevel 1 goto :err

echo.
echo Done:
echo   overlay\bin\RA3_Overlay_Inject.exe
echo   overlay\bin\arm_mustcode.exe
echo Keep them beside ra3_overlay_v4.dll
exit /b 0

:err
echo BUILD FAILED
exit /b 1
