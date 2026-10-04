# RA3 In-Process ImGui Overlay

进程内 DLL，注入游戏后用 ImGui 菜单改 MustCode / FLAGS / 单位内存。

按 **Home / Insert / F8** 显隐菜单。

## 状态

| 项目 | 状态 |
|------|------|
| Win32 (x86) DLL | OK（`ra3_overlay_v4.dll`） |
| D3D9 Present/EndScene（MinHook） | OK |
| Home / Insert / F8 显隐 | OK |
| MustCode 注入（`mustcode_asm.build()` / `arm_mustcode.exe`） | OK |
| FLAGS 开关（勾选框）+ 危险等级（分段条） | OK |
| 单位速度/血量/摧毁/满星/复制等 | OK |
| 一键注入器 EXE | OK（`RA3_Overlay_Inject.exe`，弹窗标题为「注入成功」或「注入失败」） |
| 战况统计（资金/电力、图标数量、本局折线） | OK |
| 建造限制（按图标禁止单位/建筑，全玩家生效） | OK |
## 使用

1. Steam 启动项：`-runver 1.12`，建议窗口化进遭遇战。
2. 进局后，**以管理员身份**双击：

```
overlay\bin\RA3_Overlay_Inject.exe
```

（自动加载同目录 `ra3_overlay_v4.dll`。弹窗标题就是结果：**注入成功** 或 **注入失败**，窗口置顶；失败时正文写原因，例如没找到游戏、没有管理员权限、DLL 没载入。）

3. 游戏内按 **Home** 打开菜单 → 点 **「注入」**（观战勾选「观战模式」）。
4. UI 约定：
   - 开关类 → 一行三个，点名称或开关
   - 危险等级 → 与名称同一行的三段选择（正常 / 高 / 最高）
   - 资金 → 「+」「-」，点名称修改每次金额
   - 一次性操作（复制/摧毁等）→ **执行**（后台异步，避免卡渲染线程）

同目录还需有 `arm_mustcode.exe`（菜单「注入」时由 DLL 调用；内含 `keystone.dll`，用 `mustcode_asm.py` 装配 MustCode）。

日志：与 `RA3_Overlay_Inject.exe` 同目录的 `ra3_overlay.log`。普通记录和崩溃堆栈都在这个文件里，每次重新注入会先清空。

### 菜单

侧栏有资源、超武/地图、弹药/危险、单位操作、战场、情报，以及建造限制、战况统计、界面设置。没有单独的观战页。观战仍用「注入」旁的「观战模式」勾选。

- **己方资金**：加减自己的钱，默认每次 10 万。
- **选中玩家资金**：列出已经进局的玩家，颜色方块加阵营名，点选后加减该玩家的钱，默认每次 1 万。
- **出场等级**：选玩家后设 0–3。0 为不设置，1–3 只影响该玩家之后生产的单位。
- **召唤基地车**：选玩家，再用盟军 / 苏联 / 帝国基地车图标决定车型，在鼠标所在地形召唤一辆。

这三项的玩家选择互不影响。只列出战场上存在的玩家；己方、已击败单独标注。

### 战况统计

菜单里打开 **战况统计**。只列出已经进局的玩家：观战席、空座位和中立席不显示。被击败的玩家仍留在榜上，名字旁标「已击败」，折线和累计金额保留，场上数量归零。

- 标题是一颗**玩家颜色色块**加上阵营（盟军 / 苏联 / 帝国），己方后面另标「己方」。
- 卡片上有当前资金、电力，以及场上还活着的部队、建筑（图标 + 数量）。
- **本局**是一张折线图：建造、损失、消灭（部队和建筑加在一起），以及收入、支出。数量和金额各自按自己的最大值缩放，避免金额把前三条线压扁。鼠标悬停可看该时刻的时间、部队与建筑的拆分，以及收入、支出金额。
- 折线横轴从战局 0 秒画到现在。游戏只保存累计总数，不保存逐秒曲线；开始记录之前的那一段，是从开局的 0 连到当时的官方累计值，之后按实际变化画。右端始终是当前的完整统计。
- 「固定」默认关闭。不固定时，关掉主菜单会一起关掉战况窗口；勾上固定后，Home 只关主菜单。

图标放在与 DLL 同目录的 `unit_images\`（仓库根目录的 `unit_images\` 也会被找到；`build.bat` 会复制过去）。文件名用游戏/Wiki 中文名（不含 `.png`），按阵营分子目录：

```
unit_images/
  盟军/{单位,建筑}/
  苏联/{单位,建筑}/
  帝国/{单位,建筑}/
  中立单位建筑/     # 医院、车库、船坞、机库、油井、桥梁、矿脉等科技建筑
  战役建筑/         # 战役关卡建筑（杰斐逊、克里姆林宫、自由女神像等）
  特殊单位/         # 天空骑士、磁暴坦克等
```

中立与战役建筑图标来自 [红警3 BWiki · 中立&战役建筑](https://wiki.biligame.com/redalert3/%E4%B8%AD%E7%AB%8B%26%E6%88%98%E5%BD%B9%E5%BB%BA%E7%AD%91)。叠加层按 PNG 文件名匹配场上单位显示名。界面上的种类名会转成简体；`unit_names.txt` 里的手写名优先。

### 建造限制

侧栏 **建造限制**。上下两栏：上面是允许建造，下面是禁止建造。每栏按盟军、苏联、帝国分开展示图标，悬停显示名称。点允许栏里的图标即禁止，点禁止栏里的图标即恢复。

- 对当前对局里所有非中立玩家生效，包括 AI。名单会记住，进入下一局后自动再套一次。
- 车厂和船厂都能造的单位只显示一个图标（MCV、勘探者、激流 ACV、史普尼克勘察车、牛蛙载具、海啸坦克、采矿车 / 苏联矿车）。禁止后两边都不能造。
- 帝国纳米核心显示成展开后的建筑名和图标。统计面板仍用原来的种类名。
- 不收录开局就有的建造工厂，以及磁暴坦克、雷达碟、斩首中队、气球炸弹、点防御无人机、盟军前哨站、苏联战斗碉堡。
- 点某一座超级武器只限制这一座（纳米虫群核心会连同巢穴，超能波毁灭装置会连同它的核心）。开关「禁用超武」仍会把六座一起禁止：超时空传送仪、质子撞击炮、铁幕装置、真空内爆弹、纳米虫群核心、超能波毁灭装置。

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
    unit_images/             # 单位/建筑图标（build.bat 从仓库根 unit_images 复制）
                             # 盟军|苏联|帝国/{单位,建筑}、中立单位建筑、战役建筑、特殊单位
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
| 复制失败 | 鼠标不在地形上；先选中单位；观战时没有本地玩家归属 |
| 菜单不出现 | Steam Overlay 冲突；看 log 是否有 `hooked Present` |
| 旧 DLL 占文件锁 | 关掉游戏后重编；输出名为 `ra3_overlay_v4.dll` |
