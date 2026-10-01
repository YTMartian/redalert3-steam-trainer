<img src="icon.png" width="88" align="right" alt="RA3 Steam Trainer 图标">

# 红警3 Steam 版修改器（RA3 Steam Trainer）

基于 `RedAlert3_Trainer_1.12_FINAL3.exe` 逆向适配的 **《命令与征服：红色警戒3》Steam 版**独立修改器。

- 支持版本：**Steam 1.12**（游戏模块 `ra3_1.12.game`）
- 运行方式：附加到游戏逻辑进程 `ra3_1.12.game`，动态注入 17 个 hook
- 无需修改游戏目录下的任何文件（所有代码写入通过 `VirtualAllocEx` 分配的内存）

> **免责声明**：仅供单机 / 遭遇战娱乐与逆向学习使用，请勿在线上对战时使用。
> 仓库不包含任何游戏本体文件（见 §5.7）。

## 快速开始

```powershell
pip install keystone-engine
# 直接跑源码
python trainer.py
# 或自行打包成单文件 EXE
build.bat
```

**使用前必做**：Steam 库 → 右键《红色警戒3》→ 属性 → 启动选项 → 填 `-runver 1.12`。
然后在游戏里进入遭遇战，运行修改器 → 点「附加游戏」→ 勾选功能或用热键。

开发 / 改代码后请先跑离线自检：

```powershell
python selftest.py        # 地址换算 + 假内存验证指针链 + 功能接线
python verify_payload.py  # 构建一致性 + 段内跳转合法性
```

---

## 一、功能列表

### 资源类
| 功能 | 说明 |
|------|------|
| 金钱无限 | 资金锁定 |
| 电力无限 | 电力锁定 |
| 科技点无限 | 科技点锁定 |
| 全科技 | 解锁全部科技 |
| 快速建造 | 建筑/单位建造速度提升 |
| 恢复矿场 | 一次性恢复矿场资源 |

### 超武 / 建造 / 地图
| 功能 | 说明 |
|------|------|
| 超级武器 | 超级武器无冷却 |
| 禁用敌方超武 | 敌方超级武器失效 |
| 全地图 | 地图全开 |
| 敌人无法建造 | 敌方无法建造 |

### 弹药 / 危险等级
| 功能 | 说明 |
|------|------|
| 弹药无限 | 弹药无限 |
| 危险等级 | 最高 / 最低 / 正常 三态切换 |

### 单位操作（需先选中单位）
| 功能 | 说明 |
|------|------|
| 超速 ×500 | 选中单位速度 ×500 |
| 慢速 ×10 | 选中单位速度 ×10 |
| 冻结 | 选中单位停止 |
| 恢复速度 | 恢复选中单位速度 |
| 无敌 | 选中单位血量拉满（9999999，等效无敌） |
| 残血(1点) | 选中单位血量设为 1 |
| 恢复血量 | 恢复选中单位血量 |
| 满级(3星) | 在游戏进程的独立线程里调官方「加经验」接口 `0x5173F0`，循环调用直到等级升满。**星星图标 / 数值加成 / EVA 语音由引擎自己出**，全程不卡死，详见 §6 |
| 摧毁选中 | 调官方摧毁接口 `0x7DCDF0`，真正拆掉选中单位（不是只清血量） |
| 复制选中 | 调 `CreateUnit(0x6440F0)` 在**当前鼠标地图落点**生成副本；正常模式归属自己，观战模式归属原阵营 |

### 运行模式
| 模式 | 说明 |
|------|------|
| 正常模式 | 注入全部 17 个 hook；资源类功能可用；复制单位归属自己 |
| 观战模式 | 不打 hook 补丁（避免观战闪退）；速度/血量/满级/摧毁/复制仍可用；复制归属原阵营 |

---

## 二、使用方法

1. **启动游戏**：在 Steam 中启动红警3，并确保启动选项包含 `-runver 1.12`（关键！否则会附加到错误版本的游戏模块）。
   - Steam 库 → 右键《红色警戒3》→ 属性 → 启动选项 → 填入 `-runver 1.12`
2. **进入游戏主界面后**，运行 `dist\RA3_Steam_Trainer.exe`（需管理员权限，已内嵌 `--uac-admin`）。
3. 附加前先选好 **「正常模式」或「观战模式」**，再点 **「附加游戏」**，状态栏显示 `已附加：模块 0x...  MustCode 0x...` 即成功。
4. 切回游戏，进入遭遇战（或观战），通过界面勾选或全局热键开关功能。
5. **复制单位**时把鼠标移到目标地形上再按热键 / 按钮。
6. 退出前点击 **「脱离」**（或直接关闭修改器），hook 会自动还原。

---

## 三、热键对照表

| 热键 | 功能 | 热键 | 功能 |
|------|------|------|------|
| Ctrl+F1 | 金钱无限 | `-` | 超速 ×500 |
| Ctrl+F2 | 电力无限 | `=` | 慢速 ×10 |
| Ctrl+F3 | 科技点无限 | PageUp | 冻结单位 |
| Ctrl+F4 | 全科技 | PageDown | 恢复速度 |
| Ctrl+F5 | 快速建造 | `[` | 无敌 |
| Ctrl+F6 | 超级武器 | `]` | 残血(1点) |
| Ctrl+F7 | 禁用敌方超武 | `\` | 恢复血量 |
| Ctrl+F9 | 全地图 | `;` | 弹药无限 |
| Ctrl+F10 | 敌人无法建造 | `,` | 危险等级最高 |
| | | `.` | 危险等级最低 |
| | | `/` | 危险等级正常 |
| | | `'` | 恢复矿场 |
| | | `p` | 满级(3星) |
| | | `Insert` | 复制选中（到鼠标位置） |
| | | `Delete` | 摧毁选中 |

> 速度 / 血量 / 满级 / 摧毁 / 复制需先选中单位；复制还会用到当前鼠标在地图上的落点。

> 界面上**鼠标悬停任意按钮**即可看到它的快捷键（以及该功能的简要说明）。

---

## 四、技术原理

### 4.1 进程模型
`RA3.exe` 只是启动器，真正的游戏引擎是 `Data\` 目录下的 `ra3_1.12.game`。修改器通过 Toolhelp32 快照枚举进程，优先附加 `ra3_1.12.game`。

### 4.2 Hook 机制
- 游戏主模块**无 ASLR**，固定基址 `0x400000`。
- 在游戏进程内 `VirtualAllocEx` 分配 4 段内存：
  | 段 | 大小 | 用途 |
  |----|------|------|
  | `MustCode` | 0x3000 | 主要 hook 逻辑代码 |
  | `MustCode2` | 0x1000 | 单位速度/血量操作代码 |
  | `FLAGS` | 0x100 | 各功能开关标志字节 |
  | `IDB` | 0x100 | 单位数据基址缓存 |
- 每个 hook 点在 `.text` 段用 5 字节 `jmp` 补丁跳转到 `MustCode`，执行后跳回 `_BackXXX`。
- `.text` 段默认只读，写补丁前用 `VirtualProtectEx` 临时改为 `PAGE_EXECUTE_READWRITE`，写完恢复。

### 4.3 17 个 Hook 点

| Hook | Steam VA | 原始指令 AOB | 功能 |
|------|----------|-------------|------|
| PlayerID | 0x54119B | `8b50288b4220` | 玩家 ID |
| Money | 0xA64E9E | `0378048b11` | 金钱 |
| Power | 0xA64DAD | `8b40048b8eb0030000` | 电力 |
| SCPoint | 0xA64F0C | `8b78348b4e3c` | 科技点 |
| HaveAllSC | 0xA64F55 | `f30f10472c` | 全科技 |
| FastBuild1 | 0x74D81E | `f30f2c461c` | 快速建造 |
| FastBuild2 | 0x74D77C | `d9461c89465c` | 快速建造 |
| FastBuild3 | 0x73511F | `d986bc010000` | 快速建造 |
| SuperPower | 0x729F19 | `8b9818040000` | 超级武器 |
| SuperPower2 | 0x87CB89 | `8b70508b01` | 超级武器 |
| DisableAllSP | 0x87CC16 | `8b51503b500c` | 禁用敌方超武 |
| DisableAllSP2 | 0x748A06 | `8b48503b4e20` | 禁用敌方超武 |
| Map | 0x8005BC | `f30f118560020000` | 全地图 |
| UnitAmmo | 0x569D85 | `8b0cba85c9` | 弹药无限 |
| DangerLevel | 0x877070 | `8b8178120000` | 危险等级 |
| OreMine | 0x712EB3 | `8b40082b4114` | 恢复矿场 |
| EnemyCantBuild | 0x74D920 | `0350043bd7` | 敌人无法建造 |

### 4.4 单位操作依赖的全局指针（Steam 版已重定位）

单位速度 / 血量类功能需要访问「选中单位管理器」全局指针。其内存结构：

```
管理器[+0x5C] = 当前选中单位数量
管理器[+0x50] = 选中单位链表头
   节点 + 8   → 对象
   对象 +0x138 → 单位实体
   实体 +0x374 → 单位数据（+0x200 速度控制器 / +0x33C 血量）
```

| 版本 | 全局指针 RVA |
|------|-------------|
| 原版 1.12（`RedAlert3_Trainer_1.12_FINAL3.exe`）| `0x8DB73C` |
| **Steam 1.12（本修改器使用）** | **`0x8E08DC`** |

> Steam 版重编译导致 `.data` 段布局变化，旧地址 `0x8DB73C` 在二进制中已无任何代码引用。
> 新地址 `0x8E08DC` 经运行时差分扫描定位，并已在游戏代码中交叉验证
> （存在 `mov ecx,[0xCE08DC]` → 取列表 → `mov ecx,[edi+8]` → `mov esi,[ecx+0x138]` 的完整调用链）。
> 对象内部偏移（`+0x50 / +0x5C / +0x138`）与结构完全未变。
>
> ⚠ 上表是 **RVA**：绝对地址 = 模块基址 + 该值，即 `0x400000 + 0x8E08DC = 0xCE08DC`。
> 修改器里读它必须用 `module_base + MGR_GLOBAL_RVA`，**不能**走 `va_of()`
> （`va_of` 是给函数 VA 用的）—— 这个坑见 §6.6。

---

## 五、源码与构建

### 5.1 运行依赖
```powershell
pip install keystone-engine pyinstaller
```

### 5.2 从源码构建 EXE

项目采用「开发期中间产物 → 内嵌数据」的构建链：

```powershell
# 1) 把 CE 脚本转成 keystone 汇编（生成 mustcode_body.asm / symbols.json）
python compile_mustcode.py

# 2) 汇编、计算标签偏移，并把 nop 对齐固化回 mustcode_body.asm
#    （生成 mustcode.bin / mustcode2.bin / labels.json）
python assemble.py

# 3) 再次固化对齐 + 生成 payload.py（ASM_TEXT 已含对齐填充，LABELS 与之一致）
python gen_payload.py

# 4) 一致性校验（trainer 精简 build 的输出必须与 LABELS 严格对应）
python verify_payload.py

# 5) PyInstaller 打包（输出 dist\RA3_Steam_Trainer.exe）
build.bat
```

> `payload.py` 是 `gen_payload.py` 自动生成的内嵌数据文件，**请勿手改**。
> 修改 hook 逻辑时，改 `mustcode_raw.txt`（CE 原始脚本）后重跑上述 1→3 步。
>
> **务必注意**：任何会改变 MC / MC2 段内指令长度的改动（新增指令、改偏移等）
> 都会破坏「标签名 == 段内偏移」这一跨段引用依赖的不变量。
> `gen_payload.py` 会自动用 `nop` 补齐，但如果某个块被撑得超过它到下一个
> 对齐边界的余量，会在第 2/3 步直接报错（`已超出对齐边界`），
> 此时需要调整 `assemble.py` 里的 `MC_ALIGN_TARGETS` / `MC2_ALIGN_TARGETS`。

### 5.3 核心源码文件

| 文件 | 作用 |
|------|------|
| `trainer.py` | 修改器主程序：GUI + 热键 + 进程附加 + 注入 |
| `payload.py` | 自动生成的内嵌数据（ASM_TEXT / SYMBOLS / LABELS / CORE_HOOKS） |
| `compile_mustcode.py` | CE 脚本 → keystone 汇编转换器（含 32 位立即数对齐） |
| `assemble.py` | 汇编器：标签方案 + 段内/跨段/绝对地址解析 + 段内标签 nop 对齐 |
| `gen_payload.py` | 由中间产物重新生成 `payload.py`（含对齐固化） |
| `verify_payload.py` | 校验 trainer 精简 build 与 `LABELS` 严格一致、跳转目标合法 |
| `test_tooltip.py` | GUI 悬停提示的自动化验证（文本/位置/绘制/销毁）+ 截图 |
| `make_icon.py` | **生成图标**：绘制并输出多尺寸 `icon.ico` + `icon.png` + 预览图（改设计改这里后重跑） |
| `icon.ico` / `icon.png` | 图标资源（`icon.png` 供 `iconphoto` 与 README 预览用） |
| `core_hooks.py` | 17 个 hook 定义（名称/VA/AOB/jmp 目标偏移） |
| `hook_map.py` | 原版 → Steam 版 hook 地址映射表 |
| `build.bat` | 一键打包脚本 |
| `diagnose.py` | 诊断脚本（进程/模块/字节校验） |
| `diag_labels.py` | 标签对齐诊断（检查行/指令一一对应与标签偏移） |
| `diag_unit.py` | 单位指针链**只读**诊断（不写内存，用于定位偏移） |
| `find_veterancy.py` | 单位星级(veterancy)字段**只读**差分定位工具 |
| `定位单位星级.bat` | 上述工具的图形化启动脚本 |
| `rank_probe.py` | 星级组件**交互式**探测（读组件布局 + 可选写入试验 + 自动还原） |
| `探测星级字段.bat` | 上述工具的图形化启动脚本 |
| `rank_probe2.py` | 星级晋升机制**交互式**验证 v4（① 调一次官方接口看等级是否升到顶 ② 循环调到不再上升 ③ 对照说明：纯写内存无效。全脚本**不写内存**，含指针链逐步校验） |
| `验证星级机制.bat` | 上述工具的图形化启动脚本 |
| `selftest.py` | **离线自检**（不需要游戏）：地址换算 / 假内存验证指针链 / 功能接线 / 防误调回归 |
| `dump_module.py` | **只读** dump 游戏模块内存镜像，供离线静态分析 |
| `scan_vet_code.py` | 离线静态分析：找引用 `[实体+0x3CC]` 的代码及其中的字段写操作 |
| `定位星级代码.bat` | 上述两个工具的组合启动脚本 |
| `dasm.py` | 按 VA 反汇编 `ra3_image.bin`（`dasm.py 0x5173F0:0x140`） |
| `vtable_dump.py` | 打印指定虚表全部项并反汇编其中一项（`vtable_dump.py 0xC35354 20`） |
| `dasm_bin.py` / `dasm_seg.py` | 反汇编 `mustcode*.bin` / trainer 运行时真正写进游戏的 MC、MC2 段 |
| `asm_bisect.py` | 逐段二分汇编 `mustcode_body.asm`，定位无法汇编的行 |
| `find_unit_mgr.py` | 单位管理器指针自动定位工具（全内存差分扫描，无需 Cheat Engine） |
| `定位单位管理器.bat` | 上述工具的图形化启动脚本 |

### 5.4 逆向辅助产物（开发期，保留备查）
`parse_trainer.py`、`aob_search.py`、`aob_variants.py`、`disasm_candidates.py`、`cluster_analysis.py`、`delta_predict.py`、`verify_hooks.py`、`verify_asm.py` 等，用于从 `RedAlert3_Trainer_1.12_FINAL3.exe` 逆向定位 hook 与交叉验证。

定位 Steam 版内部函数/字段时新增的通用工具：`disasm_va.py`（按 VA 反汇编游戏二进制）、
`scan_field.py`（统计某结构偏移的访问点）、`xref2.py`（字符串→指针表→代码的二级交叉引用）、
`find_string_xref.py`（字符串搜索 + 引用定位）。

### 5.5 自检与回归测试

| 脚本 | 作用 | 是否需要游戏 |
|------|------|------------|
| `selftest.py` | **离线自检**（见下）：地址换算、假内存验证指针链、功能接线、防误调回归 | 否 |
| `ascii_bats.py` | 重新生成所有 `.bat`（保证 100% ASCII，见下方说明）。幂等，可随时重跑 | 否 |
| `verify_payload.py` | 构建一致性：trainer 精简 build 与 `assemble.py` 输出逐字节相同、`LABELS` 与固化标签一致、Capstone 段内非法跳转必须为 0 | 否 |
| `test_tooltip.py` | GUI 悬停提示的自动化验证（文本 / 位置 / 绘制 / 销毁）+ 截图 | 否 |
| `test_icon.py` | 图标端到端验证：ICO 各尺寸帧、`WM_GETICON` 回读窗口真实图标句柄、`iconphoto` 兜底路径、EXE 内嵌图标 | 否（需桌面） |
| `test_readme.py` | README 结构自检：代码围栏成对、`5.x` 编号连续、正文引用的文件与图片都真实存在 | 否 |
| `diag_unit.py` | 单位指针链**只读**诊断（不写内存），用于定位偏移 | 是 |
| `rank_probe2.py` | 星级晋升机制**交互式**验证（详见 §5.3、§6） | 是 |

> `selftest.py` 与 `verify_payload.py` 覆盖了历史上真实踩过的坑
> （地址 VA/RVA 语义混用、跨段跳转错位、误调会挂游戏的函数），
> 改完代码务必先跑这两个。

> **`.bat` 文件必须保持 100% ASCII**（这也是被真实踩过的坑）：cmd.exe 在
> `chcp 65001` 下解析含多字节字符的 `.bat` 时会按字节切分 `echo` 行，
> 把中文碎片当命令执行，报
> `'????' is not recognized as an internal or external command` 并可能中断。
> 所以所有 `.bat` 只保留 ASCII，中文提示一律交给被调用的 Python 脚本
> （它们自己设了 `PYTHONUTF8` / `PYTHONIOENCODING`）。
> 要改批处理内容，请编辑 `ascii_bats.py` 后重跑它，**不要直接手改 `.bat`**。

### 5.6 图标

![图标预览](icon_preview.png)

设计呼应两件事：**红警系列的红星**，以及本修改器的招牌功能**「满级(3 星)」**
——三颗五角星按军衔式三角排布。配色直接取自 GUI 主题
（底板 `#1e1e26→#121216`、描边与主星 `#e04a3f`）。

几个工程要点：

- 图标由 `make_icon.py` **代码生成**（只依赖 Pillow），不手工绘制。
  生成的 `icon.ico` 是二进制，**不要手改**，改设计请改脚本后重跑。
- **每个尺寸都是独立绘制的**（16/20/24/32/40/48/64/128/256），不是从
  256px 缩出来的。做法是先在该尺寸的 8 倍画布上绘制做超采样抗锯齿，
  再 LANCZOS 降采样。
- 小尺寸单独调参：笔触宽度设下限（`max(1.0, …)`）、16/24px 加大辅星并
  去掉外发光 —— 否则细节被采样吃掉后三颗星会糊成一团。
- **圆角外是透明的**（不是填深色），否则在浅色标题栏上会露出一个黑方块。
  降采样时先按 alpha 预乘再除回来，避免半透明边缘混进透明区的黑色而
  出现一圈暗边。`make_icon.py` 末尾会断言四角透明、中心不透明。
- `trainer.apply_icon()` 同时设 `iconbitmap`（.ico，影响标题栏与任务栏）
  和 `iconphoto`（.png，兜底），并**吞掉所有异常** —— 图标不是功能，
  绝不能因为它缺失就让修改器起不来。
- 资源定位走 `_resource_dir()`：源码运行取脚本目录，PyInstaller onefile
  取 `sys._MEIPASS`，两种方式都能找到图标。

```powershell
python make_icon.py    # 重新生成 icon.ico / icon.png / icon_preview.png
python test_icon.py    # 回读 WM_GETICON 验证窗口真的带上了图标，
                       # 并校验 EXE 内嵌图标的 bpp/尺寸
```

> `icon_preview.png` 是给 README 用的预览图（棋盘底，方便看清圆角透明），
> 会一起提交；它由 `make_icon.py` 生成，同样不要手改。

### 5.7 仓库内容说明

本仓库只包含**自己编写的代码、文档与少量小型数据文件**。
以下内容被 `.gitignore` 排除，不会上传：

| 排除项 | 原因 |
|--------|------|
| `ra3_1.12.game` / `RA3.exe` / `ra3_image.bin` / `image.bin` / `tail.bin` | 游戏本体二进制，**版权归 EA**，不可分发 |
| `RedAlert3_Trainer_1.12_FINAL3.exe` | 第三方修改器原版，版权归其作者 |
| `dist/` / `build/` / `__pycache__/` / `*.exe` | 构建产物与打包 EXE，可自行 `build.bat` 生成 |
| `_find_mouse*.py` 等临时逆向脚本 | 本地探针，不纳入版本库 |
| `scan_vet_code_result.txt`（1.3MB）等大型分析 dump | 体积大且可从游戏二进制离线重新生成 |

因此 clone 后若要跑离线静态分析工具（`dasm.py` / `scan_vet_code.py` 等），
需要自己把 `ra3_1.12.game` 复制到项目根目录，并用 `dump_module.py` 生成 `ra3_image.bin`。
只跑修改器本身则**不需要**这些文件——`payload.py` 已内嵌全部注入数据。

仓库布局（只列主要文件）：

```
redalert3-steam-trainer/
├─ trainer.py              修改器主程序（GUI + 热键 + 附加 + 注入）
├─ payload.py              自动生成的内嵌注入数据（勿手改）
├─ core_hooks.py           17 个 hook 定义
├─ mustcode_raw.txt        ★ hook 逻辑源码（CE 脚本语法），改逻辑从这里改
├─ mustcode_body.asm       compile_mustcode.py 生成的 keystone 汇编
├─ assemble.py             ┐
├─ compile_mustcode.py     │ 构建链
├─ gen_payload.py          │
├─ verify_payload.py       ┘
├─ selftest.py             离线自检
├─ ascii_bats.py           重新生成所有 .bat（保持 ASCII）
├─ build.bat               一键打包
├─ make_icon.py            图标生成器（输出 icon.ico / icon.png）
├─ icon.ico / icon.png     图标资源
├─ README.md
│
├─ 工具：进程/字段定位
│   find_unit_mgr.py + 定位单位管理器.bat
│   find_veterancy.py + 定位单位星级.bat
│   diag_unit.py + 诊断单位偏移.bat
│
├─ 工具：星级机制验证
│   rank_probe.py  + 探测星级字段.bat
│   rank_probe2.py + 验证星级机制.bat
│
└─ 工具：离线静态分析
    dump_module.py + scan_vet_code.py + 定位星级代码.bat
    dasm.py / dasm_bin.py / dasm_seg.py / vtable_dump.py / xref2.py ...
```

---

## 六、关键适配问题与修复记录

### 1. 基址双重相加
- **现象**：`PlayerID` hook 校验读到错误字节。
- **原因**：`CORE_HOOKS` 里已是绝对 VA（含 `0x400000` 基址），代码又加了一次 `module_base`。
- **修复**：新增 `va_of()`，用 `module_base + (va - 0x400000)` 计算实际地址。

### 2. 代码段写入失败
- **现象**：`playerID hook 写入失败`。
- **原因**：`.text` 页默认 `PAGE_EXECUTE_READ`，直接 `WriteProcessMemory` 被拒。
- **修复**：`write_code()` 先 `VirtualProtectEx` 改可写，写完恢复保护。

### 3. 进入遭遇战闪退（关键）
- **现象**：能附加，但进入遭遇战游戏立即退出。
- **原因**：CE 5.5 与 Keystone 的**立即数编码长度不同**。CE 5.5 对 `add [mem], 0x1`、`cmp [mem], 0` 这类显式 32 位立即数保留 7 字节（`81 /digit id`），而 Keystone 会优化成 4 字节（`83 /digit ib`）。每处差 3 字节，导致后续 19 处硬编码短跳转（`db 74/75/eb`）跳到指令中间，执行到垃圾字节而崩溃。
- **修复**：在 `compile_mustcode.py` 中手动编码这 15 处 ALU 指令为 32 位立即数形式，对齐 CE 5.5 布局。
- **验证**：修复后汇编 + Capstone 反汇编控制流校验，段内无效跳转 **19 → 0**。

### 4. 单位操作功能崩溃（选中单位管理器全局指针失效）
- **现象**：启用单位速度 / 血量功能后，选中单位即导致游戏退出。
- **原因**：Steam 版是**重新编译的二进制**（hook 偏移与原版各不相同，如弹药 hook 原版 `0x128735` / Steam `0x569D85`，且差值不是常数）。`.data` 段布局随之变化，原全局指针 `0x8DB73C` 在新二进制中**已无任何代码引用**。
- **定位**：不用 Cheat Engine，改用自动差分扫描（`find_unit_mgr.py`）——依次选中 4/3/2/1 个单位，每轮扫描全进程可写内存中「值 = 选中数量」的 dword，取交集得到「选中数量」字段，再反查指向该对象的全局指针。结果：
  ```
  全局指针 0x00CE08DC (RVA 0x8E08DC) [.data] -> 管理器 0x04608340  数量偏移 +0x5C
      mgr+0x50 = 0x0F5E5768
  ```
- **交叉验证**：在游戏代码中找到 `mov ecx,[0xCE08DC]` → 取选中列表 → `mov ecx,[edi+8]` → `mov esi,[ecx+0x138]`，与原版遍历逻辑完全一致，确认偏移 `+0x50 / +0x5C / +0x138` 未变。
- **修复**：`mustcode_raw.txt` 中 12 处 `8DB73C` → `8E08DC`；同步更新 `core_hooks.py` / `hook_map.py` / `verify_hooks.py`。
- **验证**：
  - 新旧两个版本汇编产物长度完全一致（1480 / 1223 字节），标签偏移全部相同；
  - 逐字节对比差异**仅 12 处**，均为该指针的 4 字节立即数（`3cb7cd` → `dc08ce`），说明未发生任何指令长度变化；
  - Capstone 控制流校验：段内无效跳转 **0 个**；
  - 重新打包后在 `PYZ` 中确认 `payload` 含 `MOD+0x8E08DC` 且无 `0x8DB73C` 残留；
  - 只读诊断（`diag_unit.py`）实测指针链全部有效：
    `实体+0x374 → +0x200 → [Q] → R+0x8 = 50.0`（速度倍率）、
    `实体+0x33C → +4/+0xC/+0x10 = 160.0`（血量）。

> 注意：以上验证只证明了**偏移正确**，并不能解释为何仍然闪退。真正原因见下一节。

### 5. 跨段跳转目标错位（单位速度/血量功能闪退的真正原因）

- **现象**：更换全局指针后仍然闪退（且 `超速` 之外的功能一触即崩）。
- **原因**：`MustCode` / `MustCode2` 是**两个独立的内存段**，跨段引用无法用标签表示，汇编器只能把它写成**绝对地址**：

  | 引用位置 | 源码写法 | 实际汇编结果 | 期望目标 |
  |----------|----------|--------------|----------|
  | MC 段调度器 | `je MC2+0x100` | `je 0x<mc2_base+0x100>` | 慢速块起始 |
  | MC2 段调用 | `call MC+0x1120` | `call 0x<mc_base+0x1120>` | `GetMouseXYZinMap` |

  这套写法依赖原 CE 脚本的**隐式约定：「标签名 == 该标签在段内的实际偏移」**。
  但实测该不变量**并不成立**——各块长度并不整齐：

  ```
  MC2 标签   实际偏移   名字期望        MC 标签    实际偏移   名字期望
  mc2_9b     0x096      0x9B            mc_29      0x1E       0x29
  mc2_100    0xA6       0x100   <-- 错位  mc_600     0x433      0x600
  mc2_200    0x15F      0x200             mc_1120    0x587      0x1120  <-- 错位
  mc2_c00    0x4F9      0xC00
  ```

  于是 `cmd=2`（慢速）实际跳到 `mc2_base+0x100`，那落在慢速块**内部**（+0x5A 处），
  从指令中间开始解码执行，几字节后必然非法访存 → **闪退**；
  `GetMeBase / WeNeedBack / CopyForMe` 里的 `call MC+0x1120` 同理跳到 MC 段代码中间。

- **修复**：
  1. `assemble.py` 新增 `align_pair()`：在每个块起始标签前插入 `nop`，
     使其实际偏移等于名字里的偏移，重新建立该不变量（MC 31 个 / MC2 12 个目标）。
  2. `gen_payload.py` 改为先用 `aligned_body_text()` 把对齐结果**固化回**
     `mustcode_body.asm`，再据此生成 `LABELS`。
     这一步是必须的——`trainer.py` 内置的是**精简版 build（不含 capstone、不做对齐）**，
     它直接消费 `mustcode_body.asm` 文本 + 固化的 `LABELS`，只有把填充写进文本才能对齐。
  3. 顺带加固：所有指针解引用补判空，并**跳过**（而非中止）无效单位。
     原脚本对 `实体+0x374`（速度组件，实际是个 `std::vector`，可能为空）与
     `[Q]`（向量首元素）**完全没有判空**，选中建筑物/特殊单位时会读野指针。
     由于插入点都位于硬编码短跳转（`db 74/75/eb`）之前，跳转距离不受影响。

- **验证**（`verify_payload.py`）：
  - `trainer.build()` 与 `assemble` 的对齐 build 输出**逐字节相同**（MC 4614 / MC2 3103 字节）；
  - `payload.LABELS` 与重算结果完全一致；
  - 固化标签与名字 100% 对齐（`mc_29=0x29`、`mc_600=0x600`、`mc_700=0x700`、`mc_1120=0x1120`、
    `mc2_100=0x100` … `mc2_c00=0xC00`）；
  - Capstone 控制流校验：MC 3542 条 / MC2 2168 条指令，段内非法跳转 **0 个**；
  - 反汇编确认 MC2 的 13 个调度入口全部以 `mov esi,[0xce08dc]` 正确起始，
    `+0xA00 / +0xB00 / +0xC00` 均正确 `call 0x<mc_base+0x1120>`。

> 另有一次「按 `p` 卡死」的教训：hook 里**不能**同步调游戏函数（自死锁），
> 详见 §6.3。

### 6. 「单位升级」的 Steam 版实现

原脚本里还有一批「内部函数调用」（`hook_map.py` 的 `FUNCS`）。早期只有零售版 RVA，
`MustCode` 里的 `call` 若原样汇编会跳到错误地址。目前 **GUI 已改走 Python `call_remote`**，
并完成了下列 Steam 重定位（升级仍走独立路径，见下文「星级满级」）：

| 符号 | 零售版 RVA | Steam VA | 用途 |
|------|-----------|----------|------|
| `SelectUnitLevelUp` | `0x35C200` | （不用） | 满级改调 `0x5173F0`，不经此符号 |
| `DestroySelectUnit` | `0x39EA50` | **`0x7DCDF0`** | 摧毁选中 |
| `GetUnitData2` | `0x3E4230` | （不用） | 复制时直接读实体 `+0x4` 模板 |
| `CreateUnit` | `0x205240` | **`0x6440F0`** | 复制选中 |
| `GetMouseXYZinMap` | `0x1ED4A0` | **`0x62C500`** | 屏幕像素 → 地图世界坐标（复制落点） |

`MustCode+800` / `MustCode+900` 等残留 cmd 入口仍标注警告，GUI 不会走那条路径。

**星级「满级」**（GUI 按钮「满级(3星)」，热键 `p`）——
不再经过 hook / `cmd=8`，由修改器在游戏进程的**独立线程**里调用官方接口
`0x5173F0(实体, 经验)` 并循环调到等级不再上升。原因与实测见 §6.3 / §6.4 / §6.5。

**摧毁 / 复制**——同样用独立线程 `call_remote`：摧毁调 `0x7DCDF0`；复制先用 Win32
取鼠标客户区坐标，再调 `0x62C500` 投影到地形，最后调 `0x6440F0` 在落点生成。
观战模式下不打资源 hook，这两项与速度/血量/满级一样仍可用。

##### 6.1 定位过程

| 步骤 | 工具 | 结论 |
| --- | --- | --- |
| 1 | `find_veterancy.py` 只读差分 | 单位升一级时 `[[实体+0x3CC]+0x24]` 递增，该字段就是等级索引 |
| 2 | `rank_probe.py` 交互式写入 | **写入成功但游戏星级不变** —— 说明 `+0x24` 只是缓存，不是权威数据 |
| 3 | `scan_vet_code.py` 离线静态扫描 | 找出 125 条引用 `[reg+0x3CC]` 的指令、分布在 91 个函数里，并标出其中写 `+0x24`/`+0x10` 的函数 |
| 4 | `dasm.py` 逐个反汇编候选函数 | 还原出完整晋升链路（下表） |
| 5 | `rank_probe2.py` v4 实测 | 确定唯一可用做法（独立线程调 `0x5173F0`）与等级范围 `0~4`，见 §6.4 |

##### 6.2 反汇编还原出的官方晋升链路（Steam 1.12）

```
0x5173F0(entity, int xp)            __cdecl      ← 官方「给单位加经验」接口
  └ 0x781EB0(tracker, xp*经验倍率, 1,1,1, 0)     __thiscall
      └ 0x71B290(tracker, 1)                     __thiscall  按经验重算星级
          · 0x524520 / 0x5331F0 取该单位的等级定义表
          · 0xA1B500(rd) 取该等级所需经验，与当前经验逐个比较
          · 达到就调用 tracker 虚表 +0x10 = 0x79A8B0(rd, notify)
0x79A8B0(rd, notify) = ExperienceTrackerObject::SetRank
          · 派发晋升事件、播 EVA 语音（经 [实体+0xF4]）
          · [tracker+0x24] = 等级序号，然后 call 0x765090 重算加成
          · 等级 > 1 且 [tracker+0x20] == 0 → 播星星特效 0x877A80 并置 +0x20 = 1
```

字段布局（对象类型名由运行时读到的**虚表 `0x00C35354`** 与它邻近的
`"ExperienceTrackerObject"` 字符串确认；该虚表的 RTTI 已被编译掉，
`[vt-4]` 不是 COL 指针，所以只能靠邻近字符串佐证）：

| 偏移 | 含义 |
| --- | --- |
| `tracker+0x08` | 当前等级的「定义键」（`0x5331F0` 用它查下一级） |
| `tracker+0x0C` | 当前经验（float） |
| `tracker+0x10` | 升到下一级所需经验（阈值，会随等级变化） |
| `tracker+0x1C` | 经验倍率（float），加经验时乘以它 |
| `tracker+0x20` | 复合字段，**只有最低字节**是「等级特效已播放」标志 |
| `tracker+0x24` | 当前等级索引 **0~4**（实测 1 = 最低一级、4 = 该单位等级的顶；**只是缓存**） |
| `tracker+0x28` | 等级上限（实测为 0 = 无特殊限制；>0 且已达上限时引擎会直接跳过加经验） |
| `tracker+0x2C` | 加成对象 `sub` |
| `tracker+0x38` | 单位实体（== `[对象+0x138]`，已与运行时内存互相印证） |

加成对象 `sub = [tracker+0x2C]`：

| 偏移 | 含义 |
| --- | --- |
| `sub+0x04` | 等级持有者指针 |
| `sub+0x08` | **当前加成倍率**（真正影响数值的就是它） |
| `sub+0x0C` | 已应用等级 |
| `sub+0x10` | 每级倍率表 `vector<float>` |

`0x765090(sub)` 的算法：

```
index = [sub+0x04]->[+0x24] - [sub+0x0C]
index <= 0   → 倍率 = 倍率表[0]
index >  0   → 倍率 = 倍率表[index]（越界取最后一项）
```

> ⚠ `0x517490` / `0x781CA0` 会把 `sub+0x0C` 写成当前星级，使 `index` 归零、
> 倍率被重置为 `表[0]` —— 它们是「清空加成」的函数，**升星时绝不能再调用**
> （早期的实现踩过这个坑）。

##### 6.3 血泪教训：为什么**不能**在 hook 里同步调游戏函数

第一版 `MustCode2+700` 的写法是「在 hook 里直接 `call 0x5173F0(实体, 100万经验)`」，
结果 **一按 `p` 游戏就彻底卡死不动**。原因经反汇编确认：

1. 命令是从 `MustCode+600` 分发的，而 `MustCode+600` 是被 **`0x6CFDFE`**（函数
   `0x6CFBD0`，每帧的玩家更新）里同步调用的。
2. `0x6CFBD0` 自己不抢锁就直接读写玩家列表 → 说明**它的调用者已经持有那把全局锁**。
3. 而晋升链路里的 `0x781EB0` 一进来就：
   ```
   push 0xC23988 / mov ecx,0xCDDF20 / call 0x9C9880   ; 0x9C9880 = 加锁
   ...                                                ; 0x9C9930 = 解锁
   ```
   同一线程重复抢一把非递归锁 → **自死锁** → 主线程永久阻塞 → 游戏画面静止。

佐证：现有 17 个 hook 的功能（金钱、电力、速度、血量……）**全部都是纯内存读写，
没有任何一个去 `call` 游戏函数**，所以它们从不卡死。`0x9C9880` 在全模块里有
1253 处调用点，`0xCDDF20` 是被 2476 条指令引用的核心锁 —— 在 hook 里碰它必死。

##### 6.4 实测四轮，把各种做法全试了一遍

| 做法 | 结果 |
| --- | --- |
| ① 纯内存写 `[tracker+0x24] = 3` | **游戏里毫无变化** —— 证实 `+0x24` 只是等级缓存 |
| ② 在 hook 里同步 `call 0x5173F0` | **游戏彻底卡死**（自死锁，见 §6.3） |
| ③ 单独 `call 0x71B290(tracker,1)`「重算星级」 | **游戏直接挂掉**（绕过官方入口后等级链状态不一致） |
| ④ 在**独立线程**里 `call 0x5173F0(实体, 经验)` | ✅ **真的升级了**：`+0x24` 上升、`+0x20` 标志置 1、`+0x08` 等级定义键切换，且不卡死 |

④ 还测出三个关键细节：

- **一次调用就会跨多级**。只给 5000 经验就把 `+0x24` 从 1 直接推到 4（封顶），
  所以循环调用只是为了确保一次点到顶，不必关心每级阈值。
  早期「一次只涨一级」的判断是错的 —— 那是因为当时 `+0x24` 已被假写入污染成 3，
  看起来只涨了一级。
- **等级索引范围是 `0~4`**（不是 0~3）：1 = 最低一级、4 = 该单位等级的顶。
  `+0x20` 是复合字段，只有最低字节是「等级特效已播放」标志。
- 该单位的 `sub+0x10`「每级倍率表」只有 1 项 `1.0`，所以**数值上**看不出差异，
  但**星级图标确实变了**。

> **踩坑提醒**：不要再用「写 `+0x24`」来试效果。它会污染真实的等级计数
> （实测把它写成 3 后再调官方接口，等级从 3 只涨到 4，看起来像「一次只涨一级」，
> 误导了对晋升步长的判断）。`rank_probe2.py` v4 已完全不含写内存操作。

##### 6.5 现在的实现：独立线程 + 官方入口，循环晋升

`MustCode2+700` 已停用（改成一个只清命令字的空块，仅保留标签以免动 MC 调度器），
`cmd=8` 不再被任何界面触发。晋升完全由修改器侧完成
（`trainer.GameProcess.rank_up_via_engine`）：

```python
ents = self.selected_entities()          # [基址+0xCE08DC]（RVA 0x8E08DC）→ +0x50 → +8 → +0x138
for ent in ents:                         # 对每个选中单位
    tracker = read_u32(ent + 0x3CC)
    prev = read_u32(tracker + 0x24)
    for _ in range(max_calls=8):         # 到封顶为止
        call_remote(FN_ADD_XP, args=(ent, 200000), timeout=4000)
        now = read_u32(tracker + 0x24)
        if now <= prev:
            break                        # 到该单位等级链的顶
        prev = now
```

工程细节：

- 调用跑在**修改器侧的工作线程**里（`do_engine` → `threading.Thread`），
  否则会挡住热键轮询和界面。
- `call_remote` 的调用桩是共享内存，多线程同时写会把机器码写坏 →
  加 `threading.Lock` 串行化；并且**一旦 `WaitForSingleObject` 超时就丢弃这块桩**
  （远端线程可能还在跑，复用会被踩坏）。
- 任一指针为空、调用超时、等级不升，都会给出可读的状态栏提示，
  而不是静默失败。

##### 6.6 坑：函数地址是 VA，但这个全局指针是 **RVA**

「没读到选中单位」的真凶是地址语义混用：

| 常量 | 写法来源 | 语义 | 绝对地址 |
| --- | --- | --- | --- |
| `FN_ADD_XP = 0x5173F0` | MustCode 里 `call ra3_1.12.game+1173F0` | **VA**（已含 0x400000） | 0x5173F0 |
| `MGR_GLOBAL_RVA = 0x8E08DC` | MustCode 里 `mov esi,[ra3_1.12.game+8E08DC]` | **RVA**（偏移） | **0xCE08DC** |

`va_of(va) = module_base + (va - 0x400000)` 是给 **VA** 用的；把 RVA 丢进去会得到
`0x8E08DC` 而不是 `0xCE08DC`，整整少了一个 0x400000 —— 于是读到一片无关内存，
`selected_entities()` 返回空列表，界面就报「没读到选中单位」。

修复：数据指针一律用 `self.module_base + MGR_GLOBAL_RVA`，
并加了离线自检 `selftest.py`（`assert mgr_addr() == 0xCE08DC` + 用假内存验证
`selected_entities()` 的链表遍历与空指针安全），防止这类错误回归。

> **验证方法**：`验证星级机制.bat`（`rank_probe2.py` v4，交互式，3 步）：
> ① 调一次官方接口看等级是否上升并回游戏确认星星 → ② 反复调直到等级不再上升
> → ③ 对照说明证明纯写内存没用。每一步之间停下来让你回游戏肉眼确认。

### 7. 仍未启用的功能

改单位 ID（原 cmd=9）等仍依赖未重定位的零售版逻辑，目前未暴露到 GUI。
摧毁 / 复制 / 满级已通过 `call_remote` 在 Steam 版可用（见上表与功能列表）。

---

## 七、注意事项

- **必须在启动选项加 `-runver 1.12`**，否则游戏模块不是 `ra3_1.12.game`，附加会失败。
- **仅支持 Steam 1.12 版本**，其他版本（如 1.13）基址与 AOB 不同。
- 修改器需**管理员权限**运行（`OpenProcess` 需要）。
- 若启动打包脚本时提示 `PermissionError`，先关闭仍在运行的旧版 `RA3_Steam_Trainer.exe`。
- 本修改器仅用于单机 / 遭遇战娱乐，请勿在线上对战中作弊。
