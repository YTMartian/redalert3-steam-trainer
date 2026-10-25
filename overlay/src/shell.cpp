#include "shell.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <unordered_map>

#include "paint.h"
#include "skin.h"

namespace ui {
namespace shell {
namespace {

// 关闭按钮的悬停量。多个窗口各有一个 ×，所以要按 ID 分开存；
// 和 ui.cpp 的 AnimTo 同一个思路，只是这里不需要帧淘汰（外壳窗口就两三个）。
std::unordered_map<ImGuiID, float> g_close_hv;

// 每个窗口的拖动状态。area 是上一帧登记的标题栏可拖区域（屏幕坐标），
// 拖动起手要靠它判断"鼠标是不是按在标题栏上"。
struct DragState {
  ImVec2 pos{};        // 最近一次已知的窗口位置
  ImVec2 area0{};      // 标题栏可拖区域
  ImVec2 area1{};
  ImVec2 size{};       // 窗口尺寸，用于把窗口夹在屏幕内
  bool have_pos = false;
  bool area_valid = false;
  bool active = false;
};
std::unordered_map<std::string, DragState> g_drag;

// 小胶囊：不做投影（标题栏一屏只有一个，但投影会盖住顶部高光）。
void Chip(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, paint::BoxShape shape, float cham, ImU32 top,
          ImU32 bot, ImU32 edge, float th) {
  paint::Plate pl;
  pl.top = top;
  pl.bot = bot;
  pl.edge = edge;
  pl.th = th;
  paint::Panel(dl, a, b, r, shape, cham, pl);
}

}  // namespace

void ResetDrag(const char* key) {
  if (!key || !key[0]) return;
  g_drag.erase(key);
}

void DragUpdate(const char* key, ImVec2* pos) {
  if (!key || !key[0] || !pos) return;
  DragState& st = g_drag[key];
  ImGuiIO& io = ImGui::GetIO();
  if (!st.have_pos) {
    st.pos = *pos;
    st.have_pos = true;
  }

  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (st.active && !down) st.active = false;

  // 起拖：上一帧鼠标在标题栏区域按下。不能每帧都用 IsMouseDown 判断，
  // 那样手滑出标题栏就断了。
  if (!st.active && st.area_valid && down && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      io.MousePos.x >= st.area0.x && io.MousePos.x <= st.area1.x &&
      io.MousePos.y >= st.area0.y && io.MousePos.y <= st.area1.y) {
    st.active = true;
  }

  if (st.active) {
    // 位移在本帧布局之前吃掉，窗口和内容一起动。
    st.pos.x += io.MouseDelta.x;
    st.pos.y += io.MouseDelta.y;
  }

  // 夹在屏幕内：至少留一条标题栏在画面里，不然拖出边界就再也抓不回来。
  // 不只在拖动时夹——改分辨率 / 缩放后，上一次记住的位置也可能已经在画面外。
  const float keep = 80.f;
  const float w = st.size.x > 1.f ? st.size.x : 200.f;
  if (io.DisplaySize.x > 1.f) {
    const float lo = -(std::max)(keep, w - keep);
    const float hi = (std::max)(keep, io.DisplaySize.x - keep);
    st.pos.x = std::clamp(st.pos.x, lo, hi);
  }
  if (io.DisplaySize.y > 1.f) {
    st.pos.y = std::clamp(st.pos.y, 0.f, (std::max)(0.f, io.DisplaySize.y - keep));
  }

  *pos = st.pos;
}

float Draw(ImDrawList* dl, ImVec2 a, ImVec2 b, float scale, float op, const char* title,
           const char* subtitle, bool interactive, Hit* hit, bool show_close,
           const char* drag_key) {
  if (!dl) return 0.f;
  if (hit) *hit = Hit{};

  const skin::Skin& sk = skin::Active();
  const float s = (scale > 0.01f) ? scale : 1.f;
  auto px = [s](float v) { return v * s; };
  const float r = px(sk.radius * 0.85f);
  const float cham = px(sk.chamfer);
  const float th = px(kTitleH);
  const float o = std::clamp(op, 0.f, 1.f);
  const float edge_a = (std::max)(o, 0.6f);  // 外框不跟着压到看不见
  const paint::BoxShape shape = sk.shape;

  // 这里原来画了一圈 4 层同心圆角矩形当外投影，叠在游戏画面上会看到窗口边缘
  // 有几道由深到浅的暗带（每层一个填充矩形、半径逐层放大 0.3/0.6/1.05/1.7 倍）。
  // 灰阶在 3D 场景上尤其脏，索性直接去掉：窗口靠底板和描边立住就够了。

  // 底板：和控件共用同一套 4 层画法（渐变 -> 高光 -> 内外描边）。
  paint::Plate pl;
  pl.top = skin::Fade(sk.win0, o);
  pl.bot = skin::Fade(sk.win1, o);
  pl.edge = skin::Fade(sk.winStroke, edge_a);
  pl.inner = skin::Fade(sk.winStroke2, edge_a);
  pl.th = sk.strokeW * 1.1f;
  paint::Panel(dl, a, b, r, shape, cham, pl);

  // 标题带：左浓右淡的横向主题色，透明度压得很低，只让标题栏"带一点阵营色"。
  {
    ImVec2 band[64];
    const int bn = paint::BoxPath(band, 64, a, ImVec2(b.x, a.y + th), r, shape, cham);
    if (bn >= 3) {
      paint::FillBoxGradH(dl, band, bn, skin::Fade(sk.accent, 0.15f * o),
                          skin::WithA(sk.accent2, 0));
    }
  }

  // 顶端 2px 主题高光。夹在圆角/切角内侧画，不压到轮廓线。
  {
    const float inset = r * 0.8f + px(3.f);
    dl->PushClipRect(ImVec2(a.x + inset, a.y), ImVec2(b.x - inset, a.y + px(2.f)), true);
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y), ImVec2(b.x, a.y + px(2.f)),
                                skin::Fade(sk.accent, 0.9f * o), skin::Fade(sk.accent2, 0.9f * o),
                                skin::WithA(sk.accent2, 0), skin::WithA(sk.accent, 0));
    dl->PopClipRect();
  }

  // 标题带下沿：中间实、两端淡出的分割线，比一条硬边干净。
  {
    const float y = a.y + th;
    const float in = px(12.f);
    dl->AddRectFilledMultiColor(ImVec2(a.x + in, y), ImVec2(b.x - in, y + px(1.f)),
                                skin::WithA(sk.divider, 0), skin::WithA(sk.divider, 0),
                                skin::Fade(sk.divider, edge_a), skin::Fade(sk.divider, edge_a));
  }

  // 右侧：关闭按钮（悬停渐亮，按下即关窗）。
  const float bh = px(24.f);
  const float by = a.y + (th - bh) * 0.5f;
  const float close_x = b.x - px(12.f) - bh;
  const float right_edge = show_close ? close_x : b.x - px(12.f);

  bool hovered = false;
  if (show_close && interactive) {
    ImGui::SetCursorScreenPos(ImVec2(close_x, by));
    ImGui::PushID("##shell_close");
    if (ImGui::InvisibleButton("##btn", ImVec2(bh, bh)) && hit) hit->close = true;
    hovered = ImGui::IsItemHovered();
    ImGui::PopID();
  }
  {
    const ImGuiID aid = ImGui::GetID("##shell_close_anim");
    float& hv = g_close_hv[aid];
    const float dt = (std::max)(0.f, (std::min)(0.1f, ImGui::GetIO().DeltaTime));
    hv += ((hovered ? 1.f : 0.f) - hv) * (1.f - std::exp(-18.f * dt));
    if (show_close) {
      const ImVec2 q0(close_x, by);
      const ImVec2 q1(close_x + bh, by + bh);
      if (hv > 0.01f) {
        const ImU32 tint = sk.bad;
        Chip(dl, q0, q1, px(bh * 0.36f), shape, cham,
             skin::Over(skin::Fade(tint, 0.30f * hv), skin::Fade(sk.win1, o)),
             skin::Over(skin::Fade(tint, 0.16f * hv), skin::Fade(sk.win1, o)),
             skin::Fade(tint, 0.60f * hv), sk.strokeW);
      }
      const ImU32 glyph = skin::Mix(skin::Fade(sk.text2, 1.f), skin::Fade(sk.text, 1.f), hv);
      const ImVec2 c(close_x + bh * 0.5f, by + bh * 0.5f);
      const float g = px(4.5f);
      const float gw = px(1.6f);
      dl->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), glyph, gw);
      dl->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x - g, c.y + g), glyph, gw);
    }
  }

  // 主题胶囊：标题栏上直接看出当前是哪套皮肤，不用开设置页确认。
  {
    ImFont* f = ImGui::GetFont();
    const float fs = px(10.f);
    const char* name = skin::At(skin::Current()).name_cn;
    const ImVec2 sz = f->CalcTextSizeA(fs, FLT_MAX, 0.f, name);
    const float ch = px(20.f);
    const ImVec2 c1(right_edge - px(8.f), a.y + (th - ch) * 0.5f);
    const ImVec2 c0(c1.x - (sz.x + px(16.f)), c1.y + ch);
    Chip(dl, ImVec2(c0.x, c1.y), c1, px(ch * 0.5f), shape, cham,
         skin::Over(skin::Fade(sk.accent, 0.14f * o), skin::Fade(sk.win1, o)),
         skin::Over(skin::Fade(sk.accent, 0.07f * o), skin::Fade(sk.win1, o)),
         skin::Fade(sk.accent, 0.35f), sk.strokeW);
    dl->AddText(f, fs, ImVec2(c0.x + px(8.f), c1.y + (ch - sz.y) * 0.5f),
                skin::Fade(sk.accent2, 0.95f), name);
  }

  // 标题：左侧一道主题竖条 + 主标题（副标题用弱化色）。
  {
    const float pad = px(16.f);
    const float cy = a.y + th * 0.5f;
    const ImVec2 s0(a.x + pad, cy - px(8.f));
    dl->AddRectFilled(s0, ImVec2(s0.x + px(3.f), s0.y + px(16.f)), skin::Fade(sk.accent, 0.95f),
                      px(2.f));
    ImFont* f = ImGui::GetFont();
    const float tx = s0.x + px(11.f);
    const float ts = px(15.f);
    const char* t1 = (title && title[0]) ? title : "";
    const char* t2 = (subtitle && subtitle[0]) ? subtitle : "";
    const ImVec2 sz1 = f->CalcTextSizeA(ts, FLT_MAX, 0.f, t1);
    dl->AddText(f, ts, ImVec2(tx, cy - sz1.y * 0.5f), skin::Fade(sk.text, 1.f), t1);
    if (t2[0]) {
      dl->AddText(f, ts, ImVec2(tx + sz1.x, cy - sz1.y * 0.5f), skin::Fade(sk.text3, 1.f), t2);
    }
  }

  // 拖动区：只覆盖标题栏左半段，避开右侧按钮，点按钮不会顺手拖窗。
  // 这里只登记区域（下一帧 DragBeforeBegin 用），不移动窗口——见 shell.h 里
  // 关于"内容追着窗口跑"的说明。
  if (interactive && drag_key && drag_key[0]) {
    const ImVec2 d0(a.x + px(6.f), a.y + px(5.f));
    const float drag_w = (std::max)(px(40.f), right_edge - a.x - px(22.f));
    const ImVec2 d1(d0.x + drag_w, d0.y + th - px(10.f));
    ImGui::SetCursorScreenPos(d0);
    // 一个空按钮：吃掉这块区域的点击，免得 ImGui 自己把窗口当成"从空白处拖动"
    // 再移一遍（那样会和 DragBeforeBegin 叠加成两倍速）。
    ImGui::InvisibleButton("##shell_drag", ImVec2(drag_w, th - px(10.f)));
    DragState& st = g_drag[drag_key];
    st.area0 = d0;
    st.area1 = d1;
    st.area_valid = true;
    st.size = ImVec2(b.x - a.x, b.y - a.y);
    st.pos = a;
    st.have_pos = true;
  }

  return th;
}

}  // namespace shell
}  // namespace ui
