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

echo [1/4] ensure payload.py ...
if not exist "%REPO%\payload.py" (
  "%PY%" "%REPO%\gen_payload.py" || goto :err
)

echo [2/4] packaging arm_mustcode.dll ...
"%PY%" -m PyInstaller --noconfirm --onefile --noconsole --name arm_mustcode --icon "%ICON%" --distpath "%REPO%\overlay\bin" --workpath "%REPO%\overlay\build\pyi_arm" --specpath "%REPO%\overlay\build" --hidden-import keystone --hidden-import keystone.keystone --hidden-import keystone.keystone_const --hidden-import payload --hidden-import mustcode_asm --collect-submodules keystone --add-binary "%KS_DLL%;keystone" --add-data "%REPO%\payload.py;." --add-data "%REPO%\mustcode_asm.py;." "%REPO%\overlay\tools\arm_mustcode.py"
if errorlevel 1 goto :err
move /Y "%REPO%\overlay\bin\arm_mustcode.exe" "%REPO%\overlay\bin\arm_mustcode.dll" >nul
if errorlevel 1 goto :err

if not exist "%REPO%\overlay\bin\ra3_overlay_v4.dll" (
  echo Missing overlay\bin\ra3_overlay_v4.dll. Run overlay\build.bat first.
  goto :err
)
if exist "%REPO%\unit_names_csf.txt" copy /Y "%REPO%\unit_names_csf.txt" "%REPO%\overlay\bin\unit_names_csf.txt" >nul
if exist "%REPO%\unit_names.txt" copy /Y "%REPO%\unit_names.txt" "%REPO%\overlay\bin\unit_names.txt" >nul
if not exist "%REPO%\overlay\bin\unit_names_csf.txt" (
  echo Missing overlay\bin\unit_names_csf.txt. Run overlay\build.bat first.
  goto :err
)

echo [3/4] packaging main.exe ...
set ADD_NAMES=
if exist "%REPO%\overlay\bin\unit_names.txt" set ADD_NAMES=--add-data "%REPO%\overlay\bin\unit_names.txt;."
"%PY%" -m PyInstaller --noconfirm --onefile --noconsole --uac-admin --name main --icon "%ICON%" --distpath "%REPO%\overlay\bin" --workpath "%REPO%\overlay\build\pyi_inject" --specpath "%REPO%\overlay\build" --add-binary "%REPO%\overlay\bin\ra3_overlay_v4.dll;." --add-binary "%REPO%\overlay\bin\arm_mustcode.dll;." --add-data "%REPO%\overlay\bin\unit_names_csf.txt;." %ADD_NAMES% --add-data "%REPO%\v1.12_change_method;v1.12_change_method" "%REPO%\overlay\tools\inject.py"
if errorlevel 1 goto :err

echo [4/4] done
echo.
echo Done:
echo   overlay\bin\main.exe
echo Ship that exe alone. On launch it unpacks the DLL, arm_mustcode.dll
echo and unit_names*.txt next to itself. arm_mustcode.dll is the helper
echo program with a dll name so it is not the file to double-click.
echo Unit icons stay inside the DLL.
exit /b 0

:err
echo BUILD FAILED
exit /b 1
