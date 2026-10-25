#pragma once

// 自绘基元。语义来自 HanaBi 的 paint 层：圆角填充 / 渐变、双层描边、
// 顶部高光、多层叠出来的柔和投影与辉光、以及一组排版辅助。
//
// 全部只依赖 ImDrawList，不持有状态，可以在任意 ImGui 窗口里直接调用。

#include "imgui.h"

namespace ui {
namespace paint {

// 皮肤的形状语言：圆角矩形，或切角（八角）矩形。
// 旭日/磁暴用切角，其余用圆角 —— 这是"质感差异"而不是"换色"的来源。
enum class BoxShape { Round = 0, Chamfer = 1 };

// 生成盒子轮廓点写入 out，返回点数。cap 为 out 容量（>= 48 足够）。
int BoxPath(ImVec2* out, int cap, ImVec2 a, ImVec2 b, float r, BoxShape shape, float chamfer);

void Fill(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 c);
// 垂直渐变（上 -> 下），圆角与 Fill 一致。
void FillGrad(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 top, ImU32 bot);
void Stroke(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 c, float th);
// 外亮内暗的双层描边：玻璃/卡片边缘的常见做法。
void Stroke2(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 outer, ImU32 inner, float th);

// 形状版：填充 / 渐变 / 描边，用于切角皮肤。
void FillBox(ImDrawList* dl, const ImVec2* pts, int n, ImU32 c);
void FillBoxGrad(ImDrawList* dl, const ImVec2* pts, int n, ImU32 top, ImU32 bot);
// 横向渐变（左 -> 右）。标题带这种很扁的区域用得上。
void FillBoxGradH(ImDrawList* dl, const ImVec2* pts, int n, ImU32 left, ImU32 right);
void StrokeBox(ImDrawList* dl, const ImVec2* pts, int n, ImU32 c, float th);

void TopHighlight(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 c);
// 四层叠加模拟高斯投影：sigma 是扩散半径，dy 是下移量，k 是强度系数。
void Shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 col, float dy, float sigma, float k);
void ShadowBox(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, BoxShape shape, float chamfer,
               ImU32 col, float dy, float sigma, float k);

// 一块控件底板的完整画法：投影 -> 上浅下深渐变 -> 内侧高光 -> 外层描边。
// 所有自绘控件都走这个，形状与描边宽度就随皮肤走。
struct Plate {
  ImU32 top = 0;       // 渐变上端
  ImU32 bot = 0;       // 渐变下端
  ImU32 edge = 0;      // 外层描边
  ImU32 inner = 0;     // 内侧高光描边（0 = 不画）
  ImU32 shadow = 0;    // 投影色（0 = 不画）
  float dy = 2.f;      // 投影下移
  float sigma = 3.f;   // 投影扩散
  float sk = 1.f;      // 投影强度
  float th = 1.f;      // 描边宽度
};
void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, BoxShape shape, float chamfer,
           const Plate& pl);

void Glow(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, int layers = 5);
void GlowEllipse(ImDrawList* dl, ImVec2 center, float rx, float ry, ImU32 col, int layers = 4);
void Knob(ImDrawList* dl, ImVec2 c, float radius, ImU32 c0, ImU32 c1, ImU32 shadowCol, float shadowDy,
          float shadowSigma, float k);
void Ring(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float th, int seg = 0);

void Text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const char* s);
void TextC(ImDrawList* dl, ImFont* f, float size, ImVec2 center, ImU32 c, const char* s);
void TextR(ImDrawList* dl, ImFont* f, float size, ImVec2 rightMid, ImU32 c, const char* s);
void TextV(ImDrawList* dl, ImFont* f, float size, ImVec2 leftMid, ImU32 c, const char* s);

// 字距可调的大写小标题（HanaBi 的 section label 用这个）。
void TextTracked(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const char* s,
                 float track);
float TrackedWidth(ImFont* f, float size, const char* s, float track);
ImVec2 TextSize(ImFont* f, float size, const char* s);
// 超宽时截断并补 "..."，UTF-8 安全。
void TextClip(ImDrawList* dl, ImFont* f, float size, ImVec2 leftMid, float maxW, ImU32 c,
              const char* s);

}  // namespace paint
}  // namespace ui
