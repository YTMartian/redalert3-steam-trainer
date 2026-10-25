#pragma once

// 主窗口 / 子窗口外壳：底板 / 标题栏 / 主题胶囊 / 关闭按钮 / 拖动区。
//
// 单独成文件的原因：ImGui 自带的标题栏是 1px 直角线 + 默认字体标题，和内部
// "渐变 + 投影 + 圆角/切角" 的自绘控件不是一个语言。所以窗口改用
// NoTitleBar | NoBackground，整块外壳由这里画。
// 绘制不依赖游戏与 ui.cpp 的全局状态，离线预览工具可以直接复用同一份代码，
// 保证"预览里看到的标题栏"就是"游戏里看到的标题栏"。

#include "imgui.h"

namespace ui {
namespace shell {

// 标题栏逻辑高度；实际值 = kTitleH * scale。
constexpr float kTitleH = 38.f;

struct Hit {
  bool close = false;  // 点了关闭（右上角 ×）
};

// 拖动必须在 ImGui::Begin 之前处理。
//
// 为什么不在 Draw 里直接 SetWindowPos：窗口位置一旦在画完外壳之后才改，本帧
// 已经按旧坐标画出去的外壳/控件就留在了旧位置，下一帧窗口再跳一次——看起来
// 就是"里面的东西追着窗口跑"。这里在布局开始前就把位移吃掉，调用方随后用
// pos 做 SetNextWindowPos(pos, ImGuiCond_Always)，窗口与内容同一帧一起动。
//
// 配套要求：该窗口要用 ImGuiWindowFlags_NoMove，否则 ImGui 自己也会从空白处
// 拖窗口，和这里的位移叠加成两倍速。
//
// key：区分窗口的稳定字符串（"menu" / "stats"）。
// pos：进=初始/上一帧位置，出=本帧应当使用的位置。
void DragUpdate(const char* key, ImVec2* pos);

// 画外壳并把标题栏的交互跑掉（interactive 为 false 时只画）。
//
// a / b   ：窗口矩形，屏幕坐标。
// scale   ：ui_px 的缩放系数（0.7 ~ 3.0）。
// op      ：用户设置的窗口透明度（0.35 ~ 1.0）。
// title   ：主标题（主色，15px）。
// subtitle：副标题（弱化色），传 "" 或 nullptr 表示没有。
// show_close：false 时不画 ×（例如"固定"状态下的战况窗口）。
// drag_key：非空时把标题栏左半段登记为拖动区，供下一帧的 DragBeforeBegin 使用。
// 返回标题栏高度：正文内容要把游标下移这么多。
float Draw(ImDrawList* dl, ImVec2 a, ImVec2 b, float scale, float op, const char* title,
           const char* subtitle, bool interactive, Hit* hit, bool show_close = true,
           const char* drag_key = nullptr);

// 丢掉某个窗口的拖动状态（窗口重建、重新注入时用）。
void ResetDrag(const char* key);

}  // namespace shell
}  // namespace ui
