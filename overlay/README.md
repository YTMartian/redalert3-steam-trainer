# RA3 In-Process ImGui Overlay

与 Python 修改器（`trainer.py` / `RA3_Steam_Trainer.exe`）**并行、二选一**：本 DLL 注入游戏进程，用 ImGui 菜单操作同一套 MustCode / FLAGS / 单位内存逻辑。

按 **Home / Insert / F8** 显隐菜单。

## 状态

| 项目 | 状态 |
|------|------|
| Win32 (x86) DLL | OK（`ra3_overlay_v4.dll`） |
| D3D9 Present/EndScene（MinHook） | OK |
| Home / Insert / F8 显隐 | OK |
| MustCode 注入（`trainer.build()` / `arm_mustcode.exe`） | OK |
| FLAGS 开关（勾选框）+ 危险等级（单选） | OK |
| 单位速度/血量/摧毁/满星/复制/观战赠送等 | OK |
| 一键注入器 EXE | OK（`RA3_Overlay_Inject.exe`，弹窗标题为「注入成功」或「注入失败」） |
| 战况统计（资金/电力、图标数量、本局折线） | OK |
| 与 Python MustCode **同时**注入 | **禁止** |

## 使用

1. Steam 启动项：`-runver 1.12`，建议窗口化进遭遇战。
2. **不要**再开 `RA3_Steam_Trainer.exe`。
3. 进局后，**以管理员身份**双击：

```
overlay\bin\RA3_Overlay_Inject.exe
```

（自动加载同目录 `ra3_overlay_v4.dll`。弹窗标题就是结果：**注入成功** 或 **注入失败**，窗口置顶；失败时正文写原因，例如没找到游戏、没有管理员权限、DLL 没载入。）

4. 游戏内按 **Home** 打开菜单 → 点 **「注入」**（观战勾选「观战模式」）。
5. UI 约定：
   - 开关类 → **勾选框**
   - 危险等级 → **单选**
   - 一次性操作（复制/摧毁等）→ **按钮**（后台异步执行，避免卡渲染线程）

同目录还需有 `arm_mustcode.exe`（菜单「注入」时由 DLL 调用；内含 `keystone.dll`，用与 `trainer.py` 相同的 `build()` 装配 MustCode）。

日志：`%TEMP%\ra3_overlay.log`

### 战况统计

菜单里打开 **战况统计**。只列出已经进局的玩家：观战席、空座位和中立席不显示。

- 标题是一颗**玩家颜色色块**加上阵营（盟军 / 苏联 / 帝国），己方后面另标「己方」。
- 卡片上有当前资金、电力，以及场上还活着的部队、建筑（图标 + 数量）。
- **本局**是一张折线图，三条线为建造、损失、摧毁（部队和建筑加在一起）。鼠标悬停可看该时刻的时间，以及部队、建筑各自的数字。收入和支出在图下方。
- 折线从本次注入之后开始记。中途注入时，左端是当时已经累计的数量，不是 0。
- 「固定」默认关闭。不固定时，关掉主菜单会一起关掉战况窗口；勾上固定后，Home 只关主菜单。

图标放在与 DLL 同目录的 `unit_images\`（仓库根目录的 `unit_images\` 也会被找到；`build.bat` 会复制过去）。

### 单位中文名（跟游戏 CSF）

游戏用 `Name:<TypeId>` 查语言包。从本机 RA3 导出官方表：

```
python overlay\tools\extract_csf_names.py --ra3 "你的RA3安装目录"
```

生成 `overlay\bin\unit_names_csf.txt`（与 DLL 同目录，换机拷贝 bin 即可）。手动覆盖写在同目录 `unit_names.txt`（优先于 CSF）。

## 编译

### Overlay DLL

需要 VS Build Tools（x86）+ CMake：

```bat
cd overlay
build.bat
```

输出：`overlay\bin\ra3_overlay_v4.dll`

### 注入器 / Arm EXE

需要 Python + PyInstaller + keystone-engine：

```bat
cd overlay
build_injector.bat
```

输出：

- `overlay\bin\RA3_Overlay_Inject.exe`（带自定义图标，管理员权限）
- `overlay\bin\arm_mustcode.exe`（打包 `keystone.dll`）

若改过 `mustcode_body.asm` / `payload.py`，先在仓库根目录：

```bat
python gen_payload.py
```

## 布局

```
overlay/
  CMakeLists.txt
  README.md
  build.bat
  build_injector.bat
  assets/ra3_overlay_icon.ico
  bin/
    ra3_overlay_v4.dll
    unit_names_csf.txt       # CSF 官方名称表（与 DLL 同目录）
    unit_names.txt           # 手动覆盖（与 DLL 同目录）
    unit_images/             # 单位、建筑图标（build.bat 从仓库根目录复制）
    RA3_Overlay_Inject.exe   # 由 build_injector.bat 生成，*.exe 默认不入库
    arm_mustcode.exe
  src/          # dllmain, d3d9_hook, ui, input, game_api, game_features
  tools/inject.py
  tools/arm_mustcode.py
  tools/extract_csf_names.py
  third_party/imgui/
  third_party/minhook/
```

## 排错

| 现象 | 可能原因 |
|------|----------|
| 注入弹窗写「注入失败」 | 先看正文：游戏没开、没管理员权限，或 DLL 被安全软件隔离 |
| 注入失败 / probe failed | 不是 1.12，或已有其它 MustCode 改写了 PlayerID 处字节 |
| 开关无效 | 未点「注入」；或观战模式卸掉了资源 hook |
| 菜单「注入」失败找不到 arm | `arm_mustcode.exe` 未放在 `overlay\bin` |
| arm 报 keystone 动态库失败 | 重新跑 `build_injector.bat`（需打包 `keystone.dll`） |
| 复制/观战赠送失败 | 鼠标不在地形上；先选中单位；观战无本地归属时用「观战赠送」 |
| 菜单不出现 | Steam Overlay 冲突；看 log 是否有 `hooked Present` |
| 旧 DLL 占文件锁 | 关掉游戏后重编；输出名为 `ra3_overlay_v4.dll` |
