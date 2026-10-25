#include "paint.h"

#include "skin.h"

#include <cmath>

namespace ui {
namespace paint {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kMaxPts = 64;

using skin::Lighten;
using skin::Mix;
using skin::WithA;

inline int AlphaOf(ImU32 c) { return static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFF); }

// 采样圆角矩形轮廓，交给 fill/stroke 变换。回调为每个顶点调用一次。
template <typename F>
void RoundRect(ImVec2 a, ImVec2 b, float r, F&& emit) {
  const float w = b.x - a.x, h = b.y - a.y;
  if (r < 0.f) r = 0.f;
  const float max_r = (w < h ? w : h) * 0.5f;
  if (r > max_r) r = max_r;
  if (r <= 0.01f) {
    emit(ImVec2(a.x, a.y));
    emit(ImVec2(b.x, a.y));
    emit(ImVec2(b.x, b.y));
    emit(ImVec2(a.x, b.y));
    return;
  }
  int seg = static_cast<int>(r * 0.75f) + 3;
  if (seg < 4) seg = 4;
  if (seg > 10) seg = 10;
  const ImVec2 cs[4] = {ImVec2(b.x - r, a.y + r), ImVec2(b.x - r, b.y - r),
                        ImVec2(a.x + r, b.y - r), ImVec2(a.x + r, a.y + r)};
  const float base[4] = {-kPi * 0.5f, 0.f, kPi * 0.5f, kPi};
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i <= seg; ++i) {
      const float ang = base[c] + (kPi * 0.5f) * (static_cast<float>(i) / static_cast<float>(seg));
      emit(ImVec2(cs[c].x + std::cos(ang) * r, cs[c].y + std::sin(ang) * r));
    }
  }
}

// UTF-8 单字符字节数。
inline int CharLen(unsigned char ch) {
  return ch < 0x80 ? 1 : (ch >> 5) == 0x6 ? 2 : (ch >> 4) == 0xE ? 3 : 4;
}

}  // namespace

void Fill(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 c) {
  if (!dl || AlphaOf(c) == 0) return;
  dl->AddRectFilled(a, b, c, r);
}

void FillGrad(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 top, ImU32 bot) {
  if (!dl) return;
  const bool ta = AlphaOf(top) != 0;
  const bool ba = AlphaOf(bot) != 0;
  if (!ta && !ba) return;
  if (top == bot) {
    dl->AddRectFilled(a, b, top, r);
    return;
  }

  ImVec2 pts[kMaxPts];
  int count = 0;
  RoundRect(a, b, r, [&](ImVec2 p) {
    if (count < kMaxPts) pts[count++] = p;
  });
  if (count < 3) return;

  const int first = dl->VtxBuffer.Size;
  dl->AddConvexPolyFilled(pts, count, IM_COL32_WHITE);
  const float height = b.y - a.y;
  for (int i = first; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float k = height > 0.f ? (v.pos.y - a.y) / height : 0.f;
    const ImU32 color = Mix(top, bot, k);
    // 抗锯齿顶点自带边缘 alpha，插值后要乘回去，否则圆角边缘发硬。
    const int va = static_cast<int>((v.col >> IM_COL32_A_SHIFT) & 0xFF);
    v.col = WithA(color, static_cast<int>(static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFF) * va / 255));
  }
}

void Stroke(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 c, float th) {
  if (!dl || AlphaOf(c) == 0 || th <= 0.f) return;
  dl->AddRect(a, b, c, r, ImDrawFlags_RoundCornersAll, th);
}

void Stroke2(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 outer, ImU32 inner, float th) {
  if (!dl || th <= 0.f) return;
  Stroke(dl, a, b, r, outer, th);
  if (AlphaOf(inner) == 0) return;
  Stroke(dl, ImVec2(a.x + th, a.y + th), ImVec2(b.x - th, b.y - th), r > th ? r - th : 0.f, inner,
         th);
}

void TopHighlight(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 c) {
  if (!dl) return;
  const int a0 = AlphaOf(c);
  if (a0 == 0) return;
  const float x0 = a.x + r;
  const float x1 = b.x - r;
  if (x1 - x0 < 2.f) return;

  const float y = a.y + 0.5f;
  dl->AddRectFilled(ImVec2(x0 + 8.f, y), ImVec2(x1 - 8.f, y + 1.f), c);
  dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + 8.f, y + 1.f), WithA(c, 0), c, c,
                              WithA(c, 0));
  dl->AddRectFilledMultiColor(ImVec2(x1 - 8.f, y), ImVec2(x1, y + 1.f), c, WithA(c, 0), WithA(c, 0),
                              c);
}

void Shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 col, float dy, float sigma, float k) {
  if (!dl) return;
  const int a0 = AlphaOf(col);
  if (a0 == 0 || k <= 0.f) return;

  static const float kOff[4] = {0.30f, 0.62f, 1.05f, 1.70f};
  static const float kAlpha[4] = {0.52f, 0.30f, 0.17f, 0.09f};
  for (int i = 0; i < 4; ++i) {
    const float g = sigma * kOff[i] * k;
    const float shift = dy * (0.45f + 0.35f * static_cast<float>(i));
    const ImVec2 mn(a.x - g, a.y - g + shift);
    const ImVec2 mx(b.x + g, b.y + g + shift);
    dl->AddRectFilled(mn, mx, WithA(col, static_cast<int>(static_cast<float>(a0) * kAlpha[i] + .5f)),
                      r + g * 0.92f);
  }
}

int BoxPath(ImVec2* out, int cap, ImVec2 a, ImVec2 b, float r, BoxShape shape, float chamfer) {
  if (!out || cap < 3) return 0;
  const float w = b.x - a.x;
  const float h = b.y - a.y;
  if (w <= 0.5f || h <= 0.5f) return 0;
  int n = 0;
  auto push = [&](float x, float y) {
    if (n < cap) out[n++] = ImVec2(x, y);
  };

  if (shape == BoxShape::Chamfer) {
    float c = chamfer;
    const float lim = (w < h ? w : h) * 0.5f;
    if (c > lim) c = lim;
    if (c <= 0.6f) {
      push(a.x, a.y);
      push(b.x, a.y);
      push(b.x, b.y);
      push(a.x, b.y);
      return n;
    }
    // 八角：四条边各留一段直边，四个角切掉。
    push(a.x + c, a.y);
    push(b.x - c, a.y);
    push(b.x, a.y + c);
    push(b.x, b.y - c);
    push(b.x - c, b.y);
    push(a.x + c, b.y);
    push(a.x, b.y - c);
    push(a.x, a.y + c);
    return n;
  }

  float rr = r < 0.f ? 0.f : r;
  const float lim = (w < h ? w : h) * 0.5f;
  if (rr > lim) rr = lim;
  if (rr <= 0.5f) {
    push(a.x, a.y);
    push(b.x, a.y);
    push(b.x, b.y);
    push(a.x, b.y);
    return n;
  }
  int seg = static_cast<int>(rr * 0.75f) + 3;
  if (seg < 4) seg = 4;
  if (seg > 10) seg = 10;
  const ImVec2 cs[4] = {ImVec2(b.x - rr, a.y + rr), ImVec2(b.x - rr, b.y - rr),
                        ImVec2(a.x + rr, b.y - rr), ImVec2(a.x + rr, a.y + rr)};
  const float base[4] = {-kPi * 0.5f, 0.f, kPi * 0.5f, kPi};
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i <= seg; ++i) {
      const float ang =
          base[c] + (kPi * 0.5f) * (static_cast<float>(i) / static_cast<float>(seg));
      push(cs[c].x + std::cos(ang) * rr, cs[c].y + std::sin(ang) * rr);
    }
  }
  return n;
}

void FillBox(ImDrawList* dl, const ImVec2* pts, int n, ImU32 c) {
  if (!dl || !pts || n < 3 || AlphaOf(c) == 0) return;
  dl->AddConvexPolyFilled(pts, n, c);
}

void FillBoxGrad(ImDrawList* dl, const ImVec2* pts, int n, ImU32 top, ImU32 bot) {
  if (!dl || !pts || n < 3) return;
  const bool ta = AlphaOf(top) != 0;
  const bool ba = AlphaOf(bot) != 0;
  if (!ta && !ba) return;
  if (top == bot) {
    dl->AddConvexPolyFilled(pts, n, top);
    return;
  }
  float y0 = pts[0].y, y1 = pts[0].y;
  for (int i = 1; i < n; ++i) {
    if (pts[i].y < y0) y0 = pts[i].y;
    if (pts[i].y > y1) y1 = pts[i].y;
  }
  const int first = dl->VtxBuffer.Size;
  dl->AddConvexPolyFilled(pts, n, IM_COL32_WHITE);
  const float span = y1 - y0;
  for (int i = first; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float k = span > 0.f ? (v.pos.y - y0) / span : 0.f;
    const ImU32 color = Mix(top, bot, k);
    // 抗锯齿顶点自带边缘 alpha，插值后要乘回去，否则边缘发硬。
    const int va = static_cast<int>((v.col >> IM_COL32_A_SHIFT) & 0xFF);
    v.col = WithA(color, static_cast<int>(
                              static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFF) * va / 255));
  }
}

void FillBoxGradH(ImDrawList* dl, const ImVec2* pts, int n, ImU32 left, ImU32 right) {
  if (!dl || !pts || n < 3) return;
  const bool la = AlphaOf(left) != 0;
  const bool ra = AlphaOf(right) != 0;
  if (!la && !ra) return;
  if (left == right) {
    dl->AddConvexPolyFilled(pts, n, left);
    return;
  }
  float x0 = pts[0].x, x1 = pts[0].x;
  for (int i = 1; i < n; ++i) {
    if (pts[i].x < x0) x0 = pts[i].x;
    if (pts[i].x > x1) x1 = pts[i].x;
  }
  const int first = dl->VtxBuffer.Size;
  dl->AddConvexPolyFilled(pts, n, IM_COL32_WHITE);
  const float span = x1 - x0;
  for (int i = first; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float k = span > 0.f ? (v.pos.x - x0) / span : 0.f;
    v.col = Mix(left, right, k);
  }
}

void StrokeBox(ImDrawList* dl, const ImVec2* pts, int n, ImU32 c, float th) {  if (!dl || !pts || n < 3 || AlphaOf(c) == 0 || th <= 0.f) return;
  dl->AddPolyline(pts, n, c, ImDrawFlags_Closed, th);
}

void ShadowBox(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, BoxShape shape, float chamfer,
               ImU32 col, float dy, float sigma, float k) {
  if (shape == BoxShape::Round) {
    Shadow(dl, a, b, r, col, dy, sigma, k);
    return;
  }
  if (!dl) return;
  const int a0 = AlphaOf(col);
  if (a0 == 0 || k <= 0.f) return;
  static const float kOff[4] = {0.30f, 0.62f, 1.05f, 1.70f};
  static const float kAlpha[4] = {0.52f, 0.30f, 0.17f, 0.09f};
  ImVec2 pts[kMaxPts];
  for (int i = 0; i < 4; ++i) {
    const float g = sigma * kOff[i] * k;
    const float shift = dy * (0.45f + 0.35f * static_cast<float>(i));
    const int n = BoxPath(pts, kMaxPts, ImVec2(a.x - g, a.y - g + shift),
                          ImVec2(b.x + g, b.y + g + shift), 0.f, BoxShape::Chamfer,
                          chamfer + g * 0.55f);
    if (n >= 3) {
      FillBox(dl, pts, n, WithA(col, static_cast<int>(static_cast<float>(a0) * kAlpha[i] + .5f)));
    }
  }
}

void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, BoxShape shape, float chamfer,
           const Plate& pl) {
  if (!dl) return;
  ImVec2 pts[kMaxPts];
  const int n = BoxPath(pts, kMaxPts, a, b, r, shape, chamfer);
  if (n < 3) return;

  if (pl.shadow && AlphaOf(pl.shadow) > 0) {
    ShadowBox(dl, a, b, r, shape, chamfer, pl.shadow, pl.dy, pl.sigma, pl.sk);
  }
  FillBoxGrad(dl, pts, n, pl.top, pl.bot);
  if (shape == BoxShape::Round && pl.inner && AlphaOf(pl.inner) > 0) {
    TopHighlight(dl, a, b, r, pl.inner);
  } else if (pl.inner && AlphaOf(pl.inner) > 0) {
    // 切角形状没有顶部直线段可放高光，改成沿上沿的一条细亮线。
    dl->AddLine(ImVec2(a.x + chamfer, a.y + 0.5f), ImVec2(b.x - chamfer, a.y + 0.5f), pl.inner,
                1.f);
  }
  if (pl.edge && AlphaOf(pl.edge) > 0) {
    StrokeBox(dl, pts, n, pl.edge, pl.th);
  }
}

void Glow(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, int layers) {
  if (!dl) return;
  const int a0 = AlphaOf(col);
  if (a0 == 0 || layers <= 0) return;
  for (int i = layers; i >= 1; --i) {
    const float t = static_cast<float>(i) / static_cast<float>(layers);
    const float rad = radius * (0.55f + 0.45f * t) + 1.f;
    const ImU32 c = WithA(col, static_cast<int>(static_cast<float>(a0) * (0.16f + 0.14f * (1.f - t)) + .5f));
    dl->AddCircleFilled(center, rad, c, 0);
  }
}

void GlowEllipse(ImDrawList* dl, ImVec2 center, float rx, float ry, ImU32 col, int layers) {
  if (!dl) return;
  const int a0 = AlphaOf(col);
  if (a0 == 0 || layers <= 0) return;
  for (int i = layers; i >= 1; --i) {
    const float t = static_cast<float>(i) / static_cast<float>(layers);
    const float s = 0.72f + 0.42f * t;
    const ImU32 c = WithA(col, static_cast<int>(static_cast<float>(a0) * (0.13f + 0.16f * (1.f - t)) + .5f));
    dl->AddEllipseFilled(center, ImVec2(rx * s, ry * s), c, 0.f, 0);
  }
}

void Knob(ImDrawList* dl, ImVec2 c, float radius, ImU32 c0, ImU32 c1, ImU32 shadowCol, float shadowDy,
          float shadowSigma, float k) {
  if (!dl) return;
  Shadow(dl, ImVec2(c.x - radius, c.y - radius), ImVec2(c.x + radius, c.y + radius), radius,
         shadowCol, shadowDy, shadowSigma, k);
  dl->AddCircleFilled(ImVec2(c.x, c.y - 0.35f), radius, c0, 0);
  dl->AddCircleFilled(ImVec2(c.x, c.y + radius * 0.42f), radius * 0.96f, WithA(c1, 40), 0);
  float pth = radius * 0.16f;
  if (pth < 0.6f) pth = 0.6f;
  // 高光不写死白色：亮色皮肤的 knob 本身就是白的，用白色会消失；
  // 从 knob 自身提亮一档，暗色皮肤上是白弧、亮色皮肤上是极淡的自身色。
  dl->PathArcTo(ImVec2(c.x, c.y - radius * 0.04f), radius * 0.78f, -kPi * 0.88f, -kPi * 0.30f, 10);
  dl->PathStroke(WithA(Lighten(c0, 0.35f), 170), 0, pth);
}

void Ring(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float th, int seg) {
  if (!dl || AlphaOf(col) == 0) return;
  dl->AddCircle(c, r, col, seg, th);
}

ImVec2 TextSize(ImFont* f, float size, const char* s) {
  if (!f || !s) return ImVec2(0, 0);
  return f->CalcTextSizeA(size, FLT_MAX, 0.f, s);
}

void Text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const char* s) {
  if (!dl || !f || !s || !s[0]) return;
  dl->AddText(f, size, pos, c, s);
}

void TextC(ImDrawList* dl, ImFont* f, float size, ImVec2 center, ImU32 c, const char* s) {
  if (!dl || !f || !s) return;
  const ImVec2 ts = TextSize(f, size, s);
  dl->AddText(f, size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), c, s);
}

void TextR(ImDrawList* dl, ImFont* f, float size, ImVec2 rightMid, ImU32 c, const char* s) {
  if (!dl || !f || !s) return;
  const ImVec2 ts = TextSize(f, size, s);
  dl->AddText(f, size, ImVec2(rightMid.x - ts.x, rightMid.y - ts.y * 0.5f), c, s);
}

void TextV(ImDrawList* dl, ImFont* f, float size, ImVec2 leftMid, ImU32 c, const char* s) {
  if (!dl || !f || !s) return;
  const ImVec2 ts = TextSize(f, size, s);
  dl->AddText(f, size, ImVec2(leftMid.x, leftMid.y - ts.y * 0.5f), c, s);
}

void TextTracked(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const char* s,
                 float track) {
  if (!dl || !f || !s) return;
  float x = pos.x;
  const char* p = s;
  while (*p) {
    const int len = CharLen(static_cast<unsigned char>(*p));
    const char* next = p + len;
    const float gw = f->CalcTextSizeA(size, FLT_MAX, 0.f, p, next).x;
    dl->AddText(f, size, ImVec2(x, pos.y), c, p, next);
    x += gw + track;
    p = next;
  }
}

float TrackedWidth(ImFont* f, float size, const char* s, float track) {
  if (!f || !s) return 0.f;
  float x = 0.f;
  int n = 0;
  const char* p = s;
  while (*p) {
    const int len = CharLen(static_cast<unsigned char>(*p));
    x += f->CalcTextSizeA(size, FLT_MAX, 0.f, p, p + len).x;
    p += len;
    ++n;
  }
  if (n > 1) x += track * static_cast<float>(n - 1);
  return x;
}

void TextClip(ImDrawList* dl, ImFont* f, float size, ImVec2 leftMid, float maxW, ImU32 c,
              const char* s) {
  if (!dl || !f || !s) return;
  const ImVec2 ts = TextSize(f, size, s);
  if (maxW <= 0.f) return;
  if (ts.x <= maxW) {
    dl->AddText(f, size, ImVec2(leftMid.x, leftMid.y - ts.y * 0.5f), c, s);
    return;
  }

  const char* p = s;
  float x = 0.f;
  const char* cut = s;
  while (*p) {
    const int len = CharLen(static_cast<unsigned char>(*p));
    const float gw = f->CalcTextSizeA(size, FLT_MAX, 0.f, p, p + len).x;
    if (x + gw > maxW - 8.f) break;
    x += gw;
    p += len;
    cut = p;
  }
  dl->AddText(f, size, ImVec2(leftMid.x, leftMid.y - ts.y * 0.5f), c, s, cut);
  dl->AddText(f, size, ImVec2(leftMid.x + x, leftMid.y - ts.y * 0.5f), c, "...");
}

}  // namespace paint
}  // namespace ui
