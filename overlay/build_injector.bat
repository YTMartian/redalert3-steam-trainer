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

echo [3/4] packaging main.exe ...
rem 单文件（onefile）：分发只需要这一个 exe，启动时把 Python 运行库、DLL 和
rem arm_mustcode 解到一个 _MEIxxxxxx 目录。这一步是 PyInstaller 的 C 引导器做的
rem （5.8 的 pyi_create_temp_path）：先用 GetTempPathW 取目录，再 _wtempnam 拼名、
rem CreateDirectoryW 建目录，重试 5 次都失败就弹
rem "INTERNAL ERROR: cannot create temporary directory!"。此时我们的 Python 代码
rem 还来不及跑（injector_startup.log 不会出现），所以排查/兜底都只能在打包这层做。
rem 两个已知触发条件（都实测复现过）：%TEMP% 指向的目录不存在（清理软件/存储感知
rem 把 Temp 目录整个删了，Windows 还没重建），或 %TEMP% 下建目录被拒（权限、只读、
rem 安全软件拦截）。引导器只建 _MEI 最后一层，从不创建 %TEMP% 本身。
rem 因此这里把解包父目录钉死到 %LOCALAPPDATA%\RA3Overlay\pyi：5.8 的
rem pyi_build_temp_folder 会自己逐级 CreateDirectoryW 建好这棵树，再把它塞进 TMP
rem 交给 _wtempnam，不再依赖系统临时目录有没有被删/重定向/变只读。
rem 注：批处理里 % 要写两次（%%）才是字面 %，环境变量由引导器运行时自己展开。
rem 不用 --uac-admin：先以普通权限解包（一定落在当前用户可写的本地目录里），
rem 需要提权时由 inject.py 自己按需弹 UAC。
"%PY%" -m PyInstaller --clean --noconfirm --onefile --noconsole --noupx --name main --runtime-tmpdir "%%LOCALAPPDATA%%\RA3Overlay\pyi" --icon "%ICON%" --distpath "%REPO%\overlay\bin" --workpath "%REPO%\overlay\build\pyi_inject" --specpath "%REPO%\overlay\build" --add-binary "%REPO%\overlay\bin\ra3_overlay_v4.dll;." --add-binary "%REPO%\overlay\bin\arm_mustcode.dll;." --add-data "%REPO%\v1.12_change_method;v1.12_change_method" --add-data "%REPO%\uprising_v1.0_change_method;uprising_v1.0_change_method" "%REPO%\overlay\tools\inject.py"
if errorlevel 1 goto :err

echo [4/4] done
echo.
echo Done:
echo   overlay\bin\main.exe
echo Ship that exe alone. On launch it unpacks the DLL and arm_mustcode.dll
echo next to itself. arm_mustcode.dll is the helper program with a dll name
echo so it is not the file to double-click.
echo Unit icons and the unit name tables stay inside ra3_overlay_v4.dll.
exit /b 0

:err
echo BUILD FAILED
exit /b 1
