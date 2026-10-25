#pragma once

// 皮肤选择画廊。被游戏内设置页和离线预览工具共用，保证两边看到的是同一份
// 绘制代码，不会各画一套。
//
// 调用前需要已经处在某个 ImGui 窗口/子窗口里：卡片会从当前光标位置开始，
// 按可用宽度自动排成 2 或 3 列。

#include "skin.h"

namespace ui {
namespace skin {

// 返回被点击的皮肤下标，没点返回 -1。
//   scale   界面缩放（等同于 ui_px 的系数）
//   availW  可用宽度；传 <= 0 时内部自取内容区宽度
int Gallery(const Skin& sk, float scale, float availW = 0.f);

}  // namespace skin
}  // namespace ui
