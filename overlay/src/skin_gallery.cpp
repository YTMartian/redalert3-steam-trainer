#include "skin_gallery.h"

#include "imgui.h"
#include "paint.h"

#include <cmath>

namespace ui {
namespace skin {

int Gallery(const Skin& sk, float scale, float availW) {
  const int n = Count();
  if (n <= 0) return -1;

  auto px = [scale](float v) { return v * scale; };

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const float gap = px(8.f);
  const float avail = availW > 1.f ? availW : ImGui::GetContentRegionAvail().x;
  const int cols = (avail >= px(520.f)) ? 3 : 2;
  const float w = floorf((avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols)) -
                  0.5f;
  const float h = px(78.f);
  const float pad = px(10.f);
  const float sw = px(30.f);
  const float f_name = px(15.f);
  const float f_tag = px(10.5f);
  const float f_cap = px(11.f);

  int clicked = -1;

  for (int i = 0; i < n; ++i) {
    if (i % cols != 0) ImGui::SameLine(0.f, gap);
    const Def& d = At(i);
    const bool sel = (Current() == i);
    ImGui::PushID(d.key);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##theme", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const float r = px(9.f);
    const Skin& cs = d.s;  // 候选皮肤：卡片用它自己的材质，才能看出差别

    // 底：候选皮肤的卡片色叠在它自己的窗底上（皮肤自带的卡片色很通透，
    // 不垫底在任意背景上都漂）。
    paint::Plate pl;
    const ImU32 base = WithA(cs.win1, 242);
    pl.top = Mix(Over(cs.card0, base), cs.accent, sel ? 0.10f : (hov ? 0.06f : 0.f));
    pl.bot = Mix(Over(cs.card1, base), Darken(cs.win1, 0.30f), 0.55f);
    pl.edge = sel ? sk.accent : (hov ? Fade(sk.accent, 0.55f) : EdgeIdle(cs));
    pl.inner = Fade(cs.cardInnerHi, sel ? 0.40f : 0.16f);
    pl.shadow = cs.shadow;
    pl.dy = px(3.f);
    pl.sigma = px(7.f) * (hov ? 1.35f : 1.f);
    pl.sk = cs.shadowK;
    pl.th = sel ? px(1.6f) : cs.strokeW;
    // 卡片形状也跟着候选皮肤：旭日那张卡自己就是切角的。
    paint::Panel(dl, p0, p1, r, cs.shape, px(cs.chamfer) * 0.8f, pl);

    // 左上：该皮肤自己的强调色渐变块。形状同样跟随，切角皮肤这里就是斜角。
    const ImVec2 q0(p0.x + pad, p0.y + pad);
    const ImVec2 q1(q0.x + sw, q0.y + sw);
    if (sel) {
      paint::Glow(dl, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), sw * 0.72f, d.preview0,
                  3);
    }
    paint::Plate sp;
    sp.top = d.preview0;
    sp.bot = d.preview1;
    sp.edge = Fade(cs.cardInnerHi, 0.55f);
    sp.th = 1.f;
    paint::Panel(dl, q0, q1, px(7.f), cs.shape, px(cs.chamfer) * 0.55f, sp);

    // 右侧：中文名 + 大写标签。
    const float tx = q1.x + px(9.f);
    const float tw = p1.x - pad - tx;
    paint::TextClip(dl, font, f_name, ImVec2(tx, p0.y + pad + f_name * 0.52f), tw,
                    cs.text, d.name_cn);
    paint::TextTracked(dl, font, f_tag, ImVec2(tx, p0.y + pad + f_name + px(1.f)),
                       sel ? cs.accent2 : cs.text3, d.name_en, px(0.7f));

    // 底部：一句说明。
    paint::TextClip(dl, font, f_cap, ImVec2(p0.x + pad, p1.y - pad - f_cap * 0.30f),
                    w - pad * 2.f, Fade(cs.text2, 0.92f), d.caption);

    if (sel) {
      const ImVec2 dp(p1.x - px(11.f), p0.y + px(11.f));
      paint::Glow(dl, dp, px(5.f), sk.accentGlow, 3);
      dl->AddCircleFilled(dp, px(2.6f), sk.accent2, 8);
    }

    if (pressed) clicked = i;
    ImGui::PopID();
  }
  return clicked;
}

}  // namespace skin
}  // namespace ui
