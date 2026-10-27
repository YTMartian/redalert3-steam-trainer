# RA3 In-Process ImGui Overlay

进程内 DLL，注入游戏后用 ImGui 菜单改 MustCode / FLAGS / 单位内存。

按 **Home / Insert / F8** 显隐菜单。

功能说明、默认热键、观战模式、战况折线和 MustCode 地址见仓库根目录 [README.md](../README.md)。这里是编译、目录和排错。

## 状态

| 项目 | 状态 |
|------|------|
| Win32 (x86) DLL | OK（`ra3_overlay_v4.dll`） |
| D3D9 Present/EndScene（MinHook） | OK |
| Home / Insert / F8 显隐 | OK |
| MustCode 注入（`mustcode_asm.build()` / `arm_mustcode.dll`） | OK |
| FLAGS 开关（勾选框）+ 危险等级（分段条） | OK |
| 单位速度/血量/摧毁/满星/复制等 | OK |
| 一键注入器 EXE | OK（`main.exe`，弹窗标题为「注入成功」或「注入失败」） |
| 战况统计（资金/电力、图标数量、协议、本局折线） | OK |
| 建造限制（按图标禁止单位/建筑，全玩家生效） | OK |
| 协议限制（三方协议图标分栏，界面已就绪） | UI 已就绪，尚未接线 |
| 摄像机（接管引擎相机、关键帧轨迹、定时事件、多配置存档；退局自动停接管） | OK（仅原版 1.12） |
| 时间控制（1x–8x 加速，写入 logicFPS / updateFPS；退局恢复原值） | OK（仅原版 1.12，实验性；只加速） |
| 动画 / Lua（动画变量跟踪 + 读写 Lua 全局） | OK（仅原版 1.12） |
| 杂项操作（血条剪影、阴影贴图/距离、光照调色盘；非对局停写） | OK（仅原版 1.12） |
| 单位操作扩展（选择范围、切换队伍、禁用鼠标输入） | OK（仅原版 1.12） |
| 顶部「调试」状态（默认关，固定高度信息框） | OK |
| 起义时刻 1.0（`ra3ep1_1.0.game`） | OK（独立 hook 表与字段偏移，部分功能见根 README 第九节） |
## 使用

1. Steam 启动项：原版填 `-runver 1.12`，起义时刻填 `-runver 1.0`，建议窗口化进遭遇战。
2. 进局后，**以管理员身份**双击：

```
overlay\bin\main.exe
```

（自动加载同目录 `ra3_overlay_v4.dll`。弹窗标题就是结果：**注入成功** 或 **注入失败**，窗口置顶；失败时正文写原因，例如没找到游戏、没有管理员权限、DLL 没载入。若启动的版本不对，会改为弹出两张图，说明在 Steam 属性的启动选项里该填什么（原版 `-runver 1.12`，起义时刻 `-runver 1.0`）。若战网客户端在运行，会提示关闭 `RA3BattleNet`，并且不会注入。）

3. 游戏内按 **Home** 打开菜单 → 点 **「注入」**（观战勾选「观战模式」）。
4. UI 约定：
   - 开关类 → 一行三个，点名称或开关
   - 危险等级 → 与名称同一行的三段选择（正常 / 高 / 最高）
   - 资金 → 「+」「-」，点名称修改每次金额
   - 一次性操作（复制/摧毁等）→ **执行**（后台异步，避免卡渲染线程）

发给别人时只拷贝这一个 exe。它会在自己旁边释出 `ra3_overlay_v4.dll`、`arm_mustcode.dll` 和名字表。`arm_mustcode.dll` 仍是装配程序，扩展名做成 dll，避免和 `main.exe` 点错。单位图标在 DLL 里，不会再写出 `unit_images`。菜单「注入」仍由覆盖层 DLL 调用旁边的 `arm_mustcode.dll`（内含 `keystone.dll`）。

日志：与 `main.exe` 同目录的 `ra3_overlay.log`。普通记录和崩溃堆栈都在这个文件里，每次重新注入会先清空。

### 菜单

侧栏有资源、超武/地图、弹药/危险、单位操作，以及建造限制、协议限制、战况统计、摄像机、时间控制、动画 / Lua、杂项操作、界面设置。没有单独的观战页。观战仍用「注入」旁的「观战模式」勾选。后五个面板（摄像机、时间控制、动画 / Lua、杂项操作，以及单位操作页底部的单位操作扩展）只适配原版 1.12，起义时刻下会显示未就绪并置灰。标题栏「调试」默认关；摄像机页只改镜头不改单位位置；改阴影 / 光照 / 接管相机后退出遭遇战会自动停写，避免 Present 卡死。

- **己方资金**：加减自己的钱，默认每次 10 万。
- **选中玩家资金**：列出已经进局的玩家，颜色方块加阵营名，点选后加减该玩家的钱，默认每次 1 万。
- **出场等级**：选玩家后设 0–3。0 为不设置，1–3 只影响该玩家之后生产的单位。
- **召唤基地车**：选玩家，再用盟军 / 苏联 / 帝国基地车图标决定车型，在鼠标所在地形召唤一辆。
- **召唤部队**：选玩家，把单位 / 建筑图标放进候选框（可多选），每种数量默认 1。面板分「单位 / 建筑 / 特殊单位」三块，默认展开。「特殊单位」是建造面板之外、但对象模板库里登记着的对象（将军刽子手 `JapanMechaKing`、天皇机甲 `JapanEmperorMecha`、钢铁浪人、超级要塞、磁暴坦克……）；下面的「搜索全部对象」可以按英文类型名片段从模板库里筛出任意对象（含战役 / 特殊 / Mod 单位）加进候选框。围着鼠标散开，归所选玩家。热键 `U` 使用当前设置。

这四项的玩家选择互不影响。只列出战场上存在的玩家；己方、已击败单独标注。

### 战况统计

菜单里打开 **战况统计**。只列出已经进局的玩家：观战席、空座位和中立席不显示。被击败的玩家仍留在榜上，名字旁标「已击败」，折线和累计金额保留，场上数量归零。

- 标题是一颗**玩家颜色色块**加上阵营（盟军 / 苏联 / 帝国），己方后面另标「己方」。
- 卡片上有当前资金、电力，以及场上还活着的部队、建筑（图标 + 数量）。攻击无人机、卫星扫射和帝国点防御无人机不进这张表。
- **协议**单独一行，图标加数量。悬停可看名称：冷却中显示剩余秒数，否则显示就绪。精简视图同样有这一行。
- 纳米核心展开成建筑后，统计里不再留着已经失效的核心。纳米虫群显示为「纳米虫群巢穴」。
- **本局**是一张折线图：建造、损失、消灭（部队和建筑加在一起），以及收入、支出。数量和金额各自按自己的最大值缩放，避免金额把前三条线压扁。鼠标悬停可看该时刻的时间、部队与建筑的拆分，以及收入、支出金额。
- 折线横轴从战局 0 秒画到现在。游戏只保存累计总数，不保存逐秒曲线；开始记录之前的那一段，是从开局的 0 连到当时的官方累计值，之后按实际变化画。右端始终是当前的完整统计。
- 「固定」默认关闭。不固定时，关掉主菜单会一起关掉战况窗口；勾上固定后，Home 只关主菜单。每次重新注入都会回到不固定。
- 「精简」不画折线。资金和电力在名字那一行，下面部队一行、建筑一行，仍是图标加数量。勾选会记在 `overlay_ui.ini` 里。
- 「极简」一个玩家一行，不带卡片、不画折线：左边玩家色块加名字，右边四列——资金、电力（用量/上限），中间是兵种图标加数量（只列部队，建筑不上这一栏）。图标右对齐贴着资金列，数量变化时整条不左右跳；放不下的兵种末尾打省略号，悬停看全名、建筑数量、造兵/被杀/击杀和完整兵种明细。极简和精简互斥，勾一个会退掉另一个，也记在 `overlay_ui.ini` 里。这个模式下窗口整体收紧：标题栏缩一档、内边距变小、"战况总览"标题行省掉，默认更宽更矮（580×300）。

图标编进 DLL。源图在仓库根目录的 `unit_images\`，`build.bat` 打包时读它。文件名用游戏/Wiki 中文名（不含 `.png`），按阵营分子目录：

```
unit_images/
  盟军/{单位,建筑,协议}/
  苏联/{单位,建筑,协议}/
  帝国/{单位,建筑,协议}/
  中立单位建筑/     # 医院、车库、船坞、机库、油井、桥梁、矿脉等科技建筑
  战役建筑/         # 战役关卡建筑（杰斐逊、克里姆林宫、自由女神像等）
  特殊单位/         # 天空骑士、磁暴坦克等
```

协议目录里的 `.webp` 在打包时转成 PNG 再编进 DLL。中立与战役建筑图标来自 [红警3 BWiki · 中立&战役建筑](https://wiki.biligame.com/redalert3/%E4%B8%AD%E7%AB%8B%26%E6%88%98%E5%BD%B9%E5%BB%BA%E7%AD%91)。叠加层按 PNG 文件名匹配场上单位显示名。界面上的种类名会转成简体；`unit_names.txt` 里的手写名优先。

### 建造限制

侧栏 **建造限制**。上下两栏：上面是允许建造，下面是禁止建造。每栏按盟军、苏联、帝国分开展示图标，悬停显示名称。点允许栏里的图标即禁止，点禁止栏里的图标即恢复。

- 对当前对局里所有非中立玩家生效，包括 AI。名单会记住，进入下一局后自动再套一次。
- 车厂和船厂都能造的单位只显示一个图标（MCV、勘探者、激流 ACV、史普尼克勘察车、牛蛙载具、海啸坦克、采矿车 / 苏联矿车）。禁止后两边都不能造。
- 帝国纳米核心显示成展开后的建筑名和图标。统计面板仍用原来的种类名。
- 不收录开局就有的建造工厂，以及磁暴坦克、雷达碟、斩首中队、气球炸弹、点防御无人机、盟军前哨站、苏联战斗碉堡。
- 点某一座超级武器只限制这一座（纳米虫群核心会连同巢穴，超能波毁灭装置会连同它的核心）。开关「禁用超武」仍会把六座一起禁止：超时空传送仪、质子撞击炮、铁幕装置、真空内爆弹、纳米虫群核心、超能波毁灭装置。

### 协议限制

侧栏 **协议限制**，在建造限制下面。排版与建造限制同一套：上面是允许使用的协议，下面是禁止使用的协议，按盟军、苏联、帝国分栏；栏内不再按分支分组，图标从左往右平铺排开，同一协议的 1/2/3 级按等级紧挨着（换行时整族一起换行，不会把分级拆到两行）。悬停显示名称与等级，点图标即搬去另一栏。图标来自 `unit_images\<阵营>\协议\`。

目前只搭了界面：点图标只会移动位置并写日志，**还没有写进引擎**，不影响对局里的协议使用。目录覆盖三个阵营共 54 项（伞兵空降、冷冻射击、轨道抛掷、毁灭空袭、气球炸弹、天皇之怒、各阵营超级武器协议等）。

### 单位中文名（跟游戏 CSF）

游戏用 `Name:<TypeId>` 查语言包。从本机 RA3 导出官方表：

```
python overlay\tools\extract_csf_names.py --ra3 "你的RA3安装目录"
```

生成仓库根目录的 `unit_names_csf.txt`（同目录还有手写的 `unit_names.txt` 覆盖表）。`build.bat` 直接把这两份编进 `ra3_overlay_v4.dll`，`overlay\bin` 里不会留下散文件，发布时只有一个 `main.exe`。手改覆盖写在 `%LOCALAPPDATA%\RA3Overlay\unit_names.txt`（优先于 CSF）；DLL 同目录若存在 `unit_names.txt` 则优先沿用该文件。

## 编译

### Overlay DLL

需要 VS Build Tools（x86）+ CMake：

```bat
cd overlay
build.bat
```

`build.bat` 不写死盘符，按这个顺序找 Visual Studio：环境变量 `RA3_VS`（或 `VS`）→ 各盘的 `\visualstudio` →
`%ProgramFiles%` 下的 2022/2019 标准安装位置。装在别处就显式指定：

```bat
set RA3_VS=H:\visualstudio
build.bat
```

生成的 `build\CMakeCache.txt` 会记住编译器的绝对路径；VS 换盘/重装后脚本检测到缓存失效会自动重建。

输出：`overlay\bin\ra3_overlay_v4.dll`

### 注入器 / Arm EXE

需要 Python + PyInstaller + keystone-engine：

```bat
cd overlay
build_injector.bat
```

脚本会先自动跑一遍 `build.bat` 把 DLL 编出来再打包（否则打包里会带着一份旧 DLL，新加的面板在游戏里看不到）。已经单独编过 DLL、只想重打包用 `build_injector.bat norebuild`。

注入器会把 DLL、装配程序、名字表和图标打进同一个 exe。

单文件 exe 的解包目录用 `--runtime-tmpdir` 钉在 `%LOCALAPPDATA%\RA3Overlay\pyi`，不再落在 `%TEMP%\_MEIxxxxxx`：PyInstaller 引导器会自己把那棵树建出来。这样就不会因为 `%TEMP%` 被清理软件删掉 / 被重定向 / 变只读而弹 `INTERNAL ERROR: cannot create temporary directory!`（那个弹窗出现在 Python 起来之前，脚本改不了它）。

输出：

- `overlay\bin\main.exe`（带自定义图标；对外只发这一个文件）
- `overlay\bin\arm_mustcode.dll`（装配程序，扩展名是 dll，供上面的 exe 打包）

若改过 `mustcode_body.asm` / `payload.py`，先在仓库根目录：

```bat
python gen_payload.py
```

`payload.py` 里的起义时刻段（`EP1_CORE_HOOKS` + `ep1_asm_and_symbols`）是手写的，生成器会原样保留。别把它删了——`arm_mustcode.py` 顶部就 import 它。

### 原生注入器 `ra3_injector.exe`（实验，第一步）

PyInstaller 单文件版 `main.exe` 启动时要把 14 MB 解到 `_MEIxxxxxx`，临时目录不可写就弹
`INTERNAL ERROR: cannot create temporary directory!`，而且那一层在 Python 起来之前，脚本兜不住。
`ra3_injector.exe` 用 C++ 重写同一个注入器：载荷（DLL、装配程序、四张引导图）编进资源段，
运行时只把两个 DLL 写到 exe 旁边，**不使用任何临时目录**，也不需要 Python。

```bat
cd overlay
build_injector_native.bat
```

`main.exe` 的 manifest 里声明了 `requireAdministrator`：**双击就会弹 UAC，点「是」**。这台机器上普通权限
启动的进程会被放进低完整性 / 受限令牌（进程列表被过滤，只剩几十个），看不到游戏进程，所以必须提权——
这也是旧版 README 一直写「以管理员身份运行」的原因。

或者单独编这个 target（`build.bat` 默认不编它，行为不变）：

```bat
cmake --build build --config Release --target ra3_injector
```

依赖 `bin\ra3_overlay_v4.dll` 和 `bin\arm_mustcode.dll`（会被嵌进 exe，所以先跑 `build.bat` 和/或
`build_injector.bat`；DLL 内容变了会触发重新嵌入）。

第一步已对齐 Python 版的这些行为：找游戏进程（原版 1.12 / 起义时刻 1.0 / 版本不对）、战网进程拦截、
卸掉上一次注入的覆盖层、`LoadLibraryW` 远程注入、打不开进程时提权重启、DLL 释出（内容相同就跳过）、
exe 目录写不进去时退到 `%LOCALAPPDATA%\RA3Overlay\runtime`、`injector_startup.log` 里记令牌属性。

`injector_startup.log` 里还有两个字段能直接判断"为什么找不到游戏"：`integrity=`（0x2000 中 / 0x3000 高
才算正常，0x1000 = 低完整性）和 `procs=`（这个进程能看到的进程数，正常是三位数）。如果 `procs` 只有几十、
`restricted=1`，说明程序是在受限令牌 / 沙箱里被拉起来的，进程列表被过滤，看不到游戏——这种启动方式下
注入器会直接把原因写进弹窗，请在资源管理器里直接双击运行。

还没做（第二步）：版本不对时弹带两张 Steam 截图的引导窗口（现在先弹同样的文字），以及
`unit_names.txt` 的旧文件迁移。这两项都只影响提示与文件搬家，不影响注入本身。

## 布局

```
overlay/
  CMakeLists.txt
  README.md
  build.bat
  build_injector.bat
  assets/ra3_overlay_icon.ico
  bin/
    ra3_overlay_v4.dll       # 名称表/图标都编在这一个 DLL 里
                             # 图标源图在仓库根 unit_images/
    ra3_camera.ini           # 摄像机配置（运行期生成，不入库）
    main.exe   # 由 build_injector.bat 生成，*.exe 默认不入库
    arm_mustcode.dll         # 装配程序，扩展名做成 dll
                             # 名称表源文件在仓库根 unit_names*.txt，打包后 bin 里不留散文件
  src/          # dllmain, d3d9_hook, ui, input, game_api, game_features
                # camera(相机接管/轨迹/定时事件/配置), time_control(时间倍率/快进)
                # lua_bridge(Lua 桥 + 动画跟踪), render_misc(血条剪影/阴影/光照)
                # skin(皮肤语义槽位表), paint(自绘基元), skin_gallery(皮肤卡片)
  tools/skin_preview/skin_preview.cpp   # 皮肤离线预览（EXCLUDE_FROM_ALL，默认不编译）
  tools/inject.py
  tools/arm_mustcode.py
  tools/extract_csf_names.py
  third_party/imgui/
  third_party/minhook/
```

## 皮肤

界面配色不再写字面值：所有颜色都来自 `src/skin.cpp` 里的一套语义槽位（`Skin`），
换皮肤只是换一张表。当前 6 套：

| 键名 | 名称 | 定位 |
|------|------|------|
| `obsidian` | 曜石 | 默认深色，回归基线 |
| `mica` | 云母 | 亮色，透明度下限自动抬到 72% |
| `tide` | 深潮 | 盟军冷蓝青 |
| `crimson` | 赤铁 | 苏联赤红，圆角收紧 |
| `aurum` | 旭日 | 升阳墨金切角 |
| `tesla` | 磁暴 | 紫电彩蛋 |

### 材质，不只是换色

六套皮肤除了配色，还各有一套几何语言，自绘控件都跟着走：

| 维度 | 字段 | 说明 |
|------|------|------|
| 圆角基准 | `radius` / `cardRadius` | 赤铁 12/9 比曜石 20/16 明显更硬 |
| 形状 | `shape` / `chamfer` | `BoxShape::Round` 圆角矩形，`BoxShape::Chamfer` 切角八角（旭日） |
| 投影 | `shadowK` | 云母投影更重（亮底需要更实的分离感） |
| 描边 | `strokeW` | 全局线宽系数 |

`shape` 是离散值，过渡时在 50% 处切换，肉眼不会看到跳变。

### 控件底板

所有自绘控件都走同一条路径：`ui.cpp` 的 `DrawPlate()` → `paint::Panel()`：

```
投影（4 层叠加模拟高斯）
  → 上浅下深渐变（按顶点着色，圆角/切角都能渐变）
  → 顶部高光（切角形状改画一条上沿细亮线）
  → 外层描边
```

- `DrawChip()`：小角标用，跳过投影（一屏几十个，投影会白烧 4 层多边形）。
- `AnimTo(id, target)`：悬停 / 按下 / 开关的过渡量，用控件 ID 存 0..1，
  所以开关是"滑过去"、卡片是"渐亮"，而不是硬切。600 帧没再出现的条目会被淘汰。

- 入口：菜单 → 界面设置 → 皮肤卡片。点选带 0.26 秒交叉淡入，并立刻写回 `overlay_ui.ini` 的 `theme=`。
- 新增一套：在 `skin.cpp` 的 `MakeXxx()` 里填 `Skin`（颜色 + radius/cardRadius/shadowK/light/dark），
  再加进 `g_defs[]` 一行即可，界面代码不用动。
- 绘制基元在 `src/paint.cpp`（圆角渐变、双层描边、顶部高光、多层投影、辉光、排版辅助），
  皮肤画廊在 `src/skin_gallery.cpp`，两者都不依赖游戏接口。

### 设置持久化

界面设置（`theme=` / `scale=` / `opacity=` / 热键 / 杂项等）写在 DLL 同目录的
`overlay_ui.ini`，启动时读回。

| 细节 | 做法 |
|------|------|
| 切主题 | 点选后**立刻**落盘，不等 0.6 秒的批量保存定时器，切完马上 Esc / 退游戏也不会丢 |
| 写入方式 | 先写 `overlay_ui.ini.tmp` 再 `MoveFileEx` 替换，进程被杀不会留下半截文件 |
| 只读目录 | DLL 在 Program Files 这类目录写不进去时，自动退到 `%LOCALAPPDATA%\RA3Overlay\overlay_ui.ini`（首次会把旧文件带过去），并在设置页底部显示实际路径 |
| 旧版本 | 早期把 ini 写在仓库根目录，DLL 旁边没有时会自动搬过来 |

### 界面缩放 / 透明度

| 项 | 默认 | 范围 | 说明 |
|----|------|------|------|
| 界面缩放 | 1.00 | 0.70 ~ 3.00 | 同时缩放控件、间距、字体与窗口尺寸 |
| 透明度 | 66% | 该皮肤下限 ~ 100% | 亮色皮肤（云母）下限自动抬到 72% |

放大到 3 倍时有两个坑，代码里已经处理：

1. **字体不能靠位图拉伸**。ImGui 1.92+ 支持按最终像素尺寸动态栅格化字形，
   所以倍数交给 `style.FontScaleMain`，不用旧的 `io.FontGlobalScale`（那条路放大就糊）。
2. **固定像素的容器要跟着放大**：侧栏宽度、按钮宽度、信息框高度都过 `ui_px()`；
   窗口默认尺寸按缩放放大后还会夹进当前分辨率（否则 3 倍时窗口比屏幕大、标题栏跑出画面），
   顶部那一行按钮放不下时逐项换行，不会把「隐藏」「调试」裁到窗口外。

### 语义取色层

`ui.cpp` 里已经没有任何字面色值（`IM_COL32(...)` / `ImVec4(...)` 常量）。需要颜色时用
`ui.cpp` 顶部那层薄封装：

| 取色 | 含义 |
|------|------|
| `SK().text` / `text2` / `text3` | 主 / 次 / 弱化文字 |
| `SK().accent` / `accent2` | 强调色 / 强调色的"实心可用"版本（标题、选中描边） |
| `SK().good` / `warn` / `bad` / `money` | 语义状态色 |
| `PanelBg()` / `PanelBgAlt()` | 行、卡片底（卡片色叠在窗底上） |
| `FieldBg()` / `ChipBg()` | 输入框·滑条轨道 / 快捷键·数量角标底 |
| `EdgeIdle()` / `EdgeHot()` | 细线描边（常态 / 高亮） |

三个要点：

1. **不要用 `Mix()` 叠半透明卡片色**。`Mix` 会连 alpha 一起插值，暗底上的"白 6%"会被算成
   RGB 中间值、变成灰白。叠面用 `skin::Over(fg, bg)`。
2. **只有玩家色是数据驱动的**，仍取 `p.color_r/g/b`（经济卡、座位选择、编队色块），
   不要再往表里加"玩家 N 色"。
3. **亮色皮肤的 `accent2` 是 accent 的加深版**，不是提亮版——它会被直接压在白底上当文字画。

### 主窗口外壳（标题栏 / 外框 / 淡入淡出）

ImGui 自带的标题栏是 1px 直角线 + 默认字体标题，和内部"渐变 + 投影 + 圆角/切角"的
自绘控件完全不是一个语言。所以主菜单改用 `ImGuiWindowFlags_NoTitleBar | NoBackground`，
外壳整块自己画：

```
src/shell.cpp   ui::shell::Draw(dl, a, b, scale, opacity, interactive, hit)
```

一条链画完：底板 4 层（渐变 + 内侧高光 + 内外描边）→ 标题带横向主题色（`paint::FillBoxGradH`）
→ 顶端 2px 主题高光 → 标题带下沿渐隐分割线 → 左侧主题竖条 + 标题 → 右侧当前主题胶囊 +
关闭按钮（悬停渐亮）。

> 外投影已去掉。原来是在背景层叠 4 层同心圆角矩形模拟高斯（半径逐层 ×0.3/0.6/1.05/1.7，
> alpha 0.52/0.30/0.17/0.09），叠在 3D 画面上能看到窗口边缘几道由深到浅的暗带。
> 窗口现在只靠底板渐变 + 描边立住，边缘是干净的一刀切。

| 行为 | 实现 |
|------|------|
| 拖窗 | `shell::DragUpdate()` 在 `ImGui::Begin` **之前**吃掉位移，窗口和内容同一帧动 |
| 关闭 | 右上角 ×，等同 Esc / 「隐藏」 |
| 提示当前皮肤 | 标题栏右侧胶囊直接显示当前主题名，不用开设置页确认 |
| 形状跟随皮肤 | 底板、标题带、胶囊、关闭按钮全部用同一套 `BoxPath`，旭日整窗是切角 |

拖动的时序很关键。早先的写法是画外壳时拿 `Hit::drag_delta` 再 `SetWindowPos`——那一帧
外壳已经按旧坐标画完、内容却按新坐标排，下一帧窗口再跳一次，观感就是"里面的控件追着
窗口跑"。现在拆成两步：

1. `shell::DragUpdate(key, &pos)`：`Begin` 之前判断鼠标是否按在上一帧登记的标题栏区域，
   是则把 `io.MouseDelta` 加进 `pos` 并夹在屏幕内（至少留 80px 标题栏，拖出界抓得回来）。
2. `SetNextWindowPos(pos, ImGuiCond_Always)` + 窗口带 `ImGuiWindowFlags_NoMove`：
   位移只此一处来源，ImGui 不会自己再从空白处拖一次（否则两倍速）。

标题栏区域在 `shell::Draw` 里用 `InvisibleButton` 登记，同时也挡住这块区域，防止 ImGui
把它当成"空白处拖动"。

**淡入淡出**：菜单隐藏不是瞬间消失，`ui.cpp` 里一个 0.16 秒的指数逼近量同时压两处：

1. `skin::SetFade(k)` —— 把淡出系数乘进 `skin::Active()` 的所有颜色槽位，
   几十个自绘控件不用各自记得乘 alpha，一起淡。
2. `ImGui::PushStyleVar(ImGuiStyleVar_Alpha, k)` —— ImGui 原生控件（文字/按钮/子窗口）。

淡到 0 才停止 `Begin()`，所以"关菜单"是一段过渡而不是一帧闪没。

外壳代码单独成 `src/shell.cpp`，不依赖游戏和 `ui.cpp` 的全局状态，离线预览工具直接复用
同一份代码（下面 `--shell`），保证"预览里看到的标题栏"就是"游戏里看到的标题栏"。

### 配色自检

换皮肤最容易翻车的不是难看，而是亮底白字、暗底描边和面底同色。这类问题到游戏里才发现太贵，
所以有离线自检。这个预览工具用 `EXCLUDE_FROM_ALL`，**默认编译不产出**（发布目录里不会多
一个开发工具），要先用的时候手动编一次：

```
cmake --build build --config Release --target ra3_skin_preview
```

编出来的 exe 落在 `bin\preview`。然后：

```
overlay\bin\preview\ra3_skin_preview.exe --audit
```

逐套皮肤先报几何（形状 / 圆角 / 切角），再打印文字/底色配对的实际对比度（WCAG）和
描边与面底的亮度差，全部达标退出码 0，有问题退出码 2。新增皮肤后先跑这个。

离线预览工具（不进 DLL）：

```
overlay\bin\preview\ra3_skin_preview.exe            # 1~6 切换，F1 默认，Esc 退出
overlay\bin\preview\ra3_skin_preview.exe --theme=4  # 直接定在某套皮肤，便于截图
overlay\bin\preview\ra3_skin_preview.exe --shell    # 只画主窗口外壳，核对标题栏/外框
overlay\bin\preview\ra3_skin_preview.exe --audit    # 只跑配色自检，不开窗口
```

截图核对：`capture_preview.ps1` 走 PrintWindow，对 D3D 窗口经常拿到 DWM 里的旧帧，
所以更可靠的是让预览工具自己从后备缓冲导出：

```
overlay\bin\preview\ra3_skin_preview.exe --theme=4 --shell --shot=out.bmp
```

渲染三帧后直接 `GetRenderTargetData` 存 BMP 并退出（32 位 BMP，`--shot` 是给脚本和
截图核对用的）。`--shell` 只画主窗口外壳（标题栏 / 外框 / 主题胶囊 / ×），
用来核对"游戏里那层壳"和"内部控件"是不是同一套语言。

`build.bat` 会同时构建 DLL 和这个预览工具；它只链接 skin/paint/skin_gallery + ImGui，
不需要游戏在跑。

## 排错

| 现象 | 可能原因 |
|------|----------|
| 注入弹窗写「注入失败」 | 先看正文：游戏没开、没管理员权限，或 DLL 被安全软件隔离 |
| 注入失败 / probe failed | 不是 1.12（原版）或不是 1.0（起义时刻），或已有其它 MustCode 改写了 PlayerID 处字节 |
| 开关无效 | 未点「注入」；或观战模式卸掉了资源 hook |
| 菜单「注入」失败找不到 arm | 重新运行注入器，让它释出 `arm_mustcode.dll`。安全软件可能隔离释出的文件 |
| arm 报 keystone 动态库失败 | 重新跑 `build_injector.bat`（需打包 `keystone.dll`） |
| 复制失败 | 鼠标不在地形上；先选中单位；观战时没有本地玩家归属 |
| 菜单不出现 | Steam Overlay 冲突；看 log 是否有 `hooked Present` |
| 旧 DLL 占文件锁 | 关掉游戏后重编；输出名为 `ra3_overlay_v4.dll` |
| 退出遭遇战，或接着进任务时崩溃 | 完全退出游戏后重新运行注入器。日志应有 `player slot guard sites=2` |
| 摄像机页写「hook 已就绪，等待游戏更新相机…」 | 还没进对局，或进局后没动过视角；移动一下镜头再回来 |
| 摄像机页写「相机 hook 安装失败」 | 重新注入；确认注入的是当前编出来的那份 DLL |
| 摄像机 XYZ 改了单位却不动 | 摄像机只改镜头；复制单位用「单位操作」 |
| 时间控制 / 动画 Lua 写「未读到」 | 只支持原版 1.12，且要进入对局；仍失败就把 log 里 `time:` / `lua:` 开头的行发出来 |
| 退出遭遇战卡死，音效一直循环 | 用当前 `build_injector.bat` 打出来的 `main.exe`；log 应有 `battle left` 行 |
| 主窗口随状态文字跳动 | 关掉标题旁「调试」 |
