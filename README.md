# 红警3 Steam 版进程内菜单

《命令与征服：红色警戒3》Steam 1.12 的进程内 ImGui 菜单。DLL 注入 `ra3_1.12.game`，不改游戏目录里的文件。

> 仅供单机 / 遭遇战娱乐与逆向学习。不要在线上对战使用。仓库不含游戏本体。

## 使用

1. Steam 启动项填 `-runver 1.12`，建议窗口化进入遭遇战。
2. 以管理员身份运行 `overlay\bin\RA3_Overlay_Inject.exe`。
3. 游戏内按 **Home**（或 Insert / F8）打开菜单，点「注入」。

菜单（玩家资金、出场等级、召唤基地车）、战况、建造限制和编译步骤见 [overlay/README.md](overlay/README.md)。日志在 `overlay\bin\ra3_overlay.log`。

## 编译

```bat
cd overlay
build.bat
```

输出 `overlay\bin\ra3_overlay_v4.dll`。改完 DLL 后需要重新注入。

菜单里的「注入」会调用同目录的 `arm_mustcode.exe` 装配 MustCode。重打这个 EXE：

```bat
cd overlay
build_injector.bat
```
