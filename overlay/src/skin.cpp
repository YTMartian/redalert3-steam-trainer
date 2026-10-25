#include "skin.h"

#include <cmath>
#include <cstring>

namespace ui {
namespace skin {
namespace {

// 十六进制 0xRRGGBB + alpha，皮肤表里最好读的写法。
constexpr ImU32 C(unsigned hex, int a = 255) {
  return IM_COL32(static_cast<int>((hex >> 16) & 0xFFu), static_cast<int>((hex >> 8) & 0xFFu),
                  static_cast<int>(hex & 0xFFu), a);
}

inline int A(ImU32 c) { return static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFF); }
inline int R(ImU32 c) { return static_cast<int>((c >> IM_COL32_R_SHIFT) & 0xFF); }
inline int G(ImU32 c) { return static_cast<int>((c >> IM_COL32_G_SHIFT) & 0xFF); }
inline int B(ImU32 c) { return static_cast<int>((c >> IM_COL32_B_SHIFT) & 0xFF); }

float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// ---------------------------------------------------------------------------
// 1. 曜石 OBSIDIAN — 默认深色，最接近改版前的观感，作为回归基线。
// ---------------------------------------------------------------------------
Skin MakeObsidian() {
  Skin s;
  s.win0 = C(0x171B21);
  s.win1 = C(0x101317);
  s.winStroke = C(0xFFFFFF, 30);
  s.winStroke2 = C(0x000000, 90);
  s.sidebar0 = C(0x13161B, 228);
  s.sidebar1 = C(0x0C0E11, 238);
  s.hairline = C(0x4DA3FF, 26);
  s.divider = C(0xFFFFFF, 16);
  s.scrim = C(0x000000, 74);
  s.shadow = C(0x000000, 205);

  s.card0 = C(0xFFFFFF, 16);
  s.card1 = C(0xFFFFFF, 8);
  s.cardStroke = C(0xFFFFFF, 16);
  s.cardInnerHi = C(0xFFFFFF, 28);
  s.cardInnerLo = C(0x000000, 60);
  s.knob0 = C(0xFFFFFF);
  s.knob1 = C(0xE9EDF3);
  s.trackOff0 = C(0xFFFFFF, 30);
  s.trackOff1 = C(0xFFFFFF, 18);
  s.trackOn0 = C(0x4DA3FF);
  s.trackOn1 = C(0x2E7BE0);
  s.segTrack = C(0x000000, 70);
  s.segPill = C(0xFFFFFF, 26);
  s.segStroke = C(0xFFFFFF, 28);
  s.field0 = C(0xFFFFFF, 12);
  s.field1 = C(0xFFFFFF, 6);
  s.fieldStroke = C(0xFFFFFF, 20);

  s.text = C(0xF5F6F8);
  s.text2 = C(0x9BA3AF);
  s.text3 = C(0x5E6672);
  s.accent = C(0x4DA3FF);
  s.accent2 = C(0x9BE0FF);
  s.accentSoft = C(0x4DA3FF, 32);
  s.accentGlow = C(0x4DA3FF, 110);
  s.good = C(0x3DDC97);
  s.warn = C(0xFFB74D);
  s.bad = C(0xFF6B6B);
  s.money = C(0xFFD86B);

  s.radius = 20.f;
  s.cardRadius = 16.f;
  s.shadowK = 1.15f;
  s.light = 0.f;
  s.strokeW = 1.f;
  s.dark = true;
  return s;
}

// ---------------------------------------------------------------------------
// 2. 云母 MICA — 亮色。游戏画面本身偏亮，所以透明度和描边都要更克制。
// ---------------------------------------------------------------------------
Skin MakeMica() {
  Skin s;
  s.win0 = C(0xFBFCFE);
  s.win1 = C(0xF1F3F8);
  s.winStroke = C(0xFFFFFF, 235);
  s.winStroke2 = C(0x1E283C, 40);
  s.sidebar0 = C(0xF7F9FC, 236);
  s.sidebar1 = C(0xEDF0F6, 242);
  s.hairline = C(0x1E283C, 26);
  s.divider = C(0x1E283C, 24);
  s.scrim = C(0x1E283C, 16);
  s.shadow = C(0x1E283C, 84);

  s.card0 = C(0xFFFFFF);
  s.card1 = C(0xFBFCFE, 250);
  s.cardStroke = C(0x1E283C, 24);
  s.cardInnerHi = C(0xFFFFFF, 245);
  s.cardInnerLo = C(0x1E283C, 18);
  s.knob0 = C(0xFFFFFF);
  s.knob1 = C(0xF2F5FA);
  s.trackOff0 = C(0x1E283C, 40);
  s.trackOff1 = C(0x1E283C, 26);
  s.trackOn0 = C(0x0A84FF);
  s.trackOn1 = C(0x3FA0FF);
  s.segTrack = C(0x1E283C, 26);
  s.segPill = C(0xFFFFFF);
  s.segStroke = C(0x1E283C, 30);
  s.field0 = C(0x1E283C, 14);
  s.field1 = C(0x1E283C, 7);
  s.fieldStroke = C(0x1E283C, 24);

  s.text = C(0x111A2B);
  s.text2 = C(0x5B6678);
  s.text3 = C(0x98A2B3);
  s.accent = C(0x0A84FF);
  // 亮色皮肤上 accent2 会被当作"实心强调色"直接压在白底上画文字和细条，
  // 所以这里必须是 accent 的加深版，而不是暗色皮肤那种提亮版。
  s.accent2 = C(0x0A6BE0);
  s.accentSoft = C(0x0A84FF, 30);
  s.accentGlow = C(0x0A84FF, 90);
  s.good = C(0x0F8F63);
  s.warn = C(0xB26A00);
  s.bad = C(0xC62A31);
  s.money = C(0x8F5E00);

  s.radius = 20.f;
  s.cardRadius = 16.f;
  s.shadowK = 1.25f;
  s.light = 1.f;
  s.strokeW = 1.f;
  s.dark = false;
  return s;
}

// ---------------------------------------------------------------------------
// 3. 深潮 TIDE — 盟军：冷蓝青，理性、界面感强。
// ---------------------------------------------------------------------------
Skin MakeTide() {
  Skin s;
  s.win0 = C(0x0E1A24);
  s.win1 = C(0x08121A);
  s.winStroke = C(0x8FE3FF, 34);
  s.winStroke2 = C(0x000000, 96);
  s.sidebar0 = C(0x0B1620, 230);
  s.sidebar1 = C(0x071018, 240);
  s.hairline = C(0x35C8FF, 30);
  s.divider = C(0x8FE3FF, 14);
  s.scrim = C(0x000000, 80);
  s.shadow = C(0x00060C, 210);

  s.card0 = C(0x9FE6FF, 18);
  s.card1 = C(0x9FE6FF, 6);
  s.cardStroke = C(0x6FD4FF, 22);
  s.cardInnerHi = C(0xBFF0FF, 30);
  s.cardInnerLo = C(0x000000, 64);
  s.knob0 = C(0xF2FBFF);
  s.knob1 = C(0xD6EEFF);
  s.trackOff0 = C(0x9FE6FF, 30);
  s.trackOff1 = C(0x9FE6FF, 16);
  s.trackOn0 = C(0x22B8F0);
  s.trackOn1 = C(0x6BE0FF);
  s.segTrack = C(0x02121C, 90);
  s.segPill = C(0x8FE3FF, 26);
  s.segStroke = C(0x9FE6FF, 26);
  s.field0 = C(0x9FE6FF, 12);
  s.field1 = C(0x9FE6FF, 5);
  s.fieldStroke = C(0x9FE6FF, 20);

  s.text = C(0xEAF9FF);
  s.text2 = C(0x92B6C8);
  s.text3 = C(0x5A7C8E);
  s.accent = C(0x2FC2F5);
  s.accent2 = C(0x9BF0FF);
  s.accentSoft = C(0x2FC2F5, 34);
  s.accentGlow = C(0x2FC2F5, 120);
  s.good = C(0x3BE0B0);
  s.warn = C(0xFFC15A);
  s.bad = C(0xFF6E6E);
  s.money = C(0xFFD86B);

  s.radius = 18.f;
  s.cardRadius = 14.f;
  s.shadowK = 1.2f;
  s.light = 0.f;
  s.strokeW = 1.f;
  s.dark = true;
  return s;
}

// ---------------------------------------------------------------------------
// 4. 赤铁 CRIMSON — 苏联：赤红、硬朗，圆角刻意收紧。
// ---------------------------------------------------------------------------
Skin MakeCrimson() {
  Skin s;
  s.win0 = C(0x1C1211);
  s.win1 = C(0x120B0A);
  s.winStroke = C(0xFFB199, 30);
  s.winStroke2 = C(0x000000, 96);
  s.sidebar0 = C(0x170F0E, 230);
  s.sidebar1 = C(0x0E0908, 240);
  s.hairline = C(0xFF7A5C, 30);
  s.divider = C(0xFFB199, 14);
  s.scrim = C(0x000000, 78);
  s.shadow = C(0x0A0302, 210);

  s.card0 = C(0xFFD3C4, 16);
  s.card1 = C(0xFFD3C4, 6);
  s.cardStroke = C(0xFF8F70, 22);
  s.cardInnerHi = C(0xFFE3D8, 28);
  s.cardInnerLo = C(0x000000, 62);
  s.knob0 = C(0xFFF3EF);
  s.knob1 = C(0xF2DCD5);
  s.trackOff0 = C(0xFFD3C4, 26);
  s.trackOff1 = C(0xFFD3C4, 14);
  s.trackOn0 = C(0xE8503A);
  s.trackOn1 = C(0xFF8A5C);
  s.segTrack = C(0x0A0403, 90);
  s.segPill = C(0xFFB199, 24);
  s.segStroke = C(0xFFD3C4, 22);
  s.field0 = C(0xFFD3C4, 12);
  s.field1 = C(0xFFD3C4, 5);
  s.fieldStroke = C(0xFFD3C4, 18);

  s.text = C(0xFDF0EC);
  s.text2 = C(0xC49A90);
  s.text3 = C(0x8A655D);
  s.accent = C(0xFF6A4A);
  s.accent2 = C(0xFFC08A);
  s.accentSoft = C(0xFF6A4A, 36);
  s.accentGlow = C(0xFF6A4A, 120);
  s.good = C(0x4FD69C);
  s.warn = C(0xFFC15A);
  s.bad = C(0xFF5A5A);
  s.money = C(0xFFD86B);

  s.radius = 12.f;
  s.cardRadius = 9.f;
  s.shadowK = 1.05f;
  s.light = 0.f;
  s.strokeW = 1.f;
  s.dark = true;
  return s;
}

// ---------------------------------------------------------------------------
// 5. 旭日 AURUM — 升阳：墨金 + 切角（圆角收到最小），几何感最强。
// ---------------------------------------------------------------------------
Skin MakeAurum() {
  Skin s;
  s.win0 = C(0x1A1610);
  s.win1 = C(0x100D09);
  s.winStroke = C(0xFFD79A, 34);
  s.winStroke2 = C(0x000000, 96);
  s.sidebar0 = C(0x16120C, 232);
  s.sidebar1 = C(0x0D0A07, 242);
  s.hairline = C(0xFFC24A, 34);
  s.divider = C(0xFFD79A, 16);
  s.scrim = C(0x000000, 78);
  s.shadow = C(0x0A0703, 205);

  s.card0 = C(0xFFCF7A, 16);
  s.card1 = C(0xFFCF7A, 6);
  s.cardStroke = C(0xF0B44A, 24);
  s.cardInnerHi = C(0xFFE6B8, 30);
  s.cardInnerLo = C(0x000000, 60);
  s.knob0 = C(0xFFF8E8);
  s.knob1 = C(0xF0E2C4);
  s.trackOff0 = C(0xFFCF7A, 26);
  s.trackOff1 = C(0xFFCF7A, 14);
  s.trackOn0 = C(0xE8A63C);
  s.trackOn1 = C(0xFFD67A);
  s.segTrack = C(0x0B0805, 92);
  s.segPill = C(0xFFD79A, 24);
  s.segStroke = C(0xFFCF7A, 22);
  s.field0 = C(0xFFCF7A, 12);
  s.field1 = C(0xFFCF7A, 5);
  s.fieldStroke = C(0xFFCF7A, 18);

  s.text = C(0xFBF1DC);
  s.text2 = C(0xC0AA84);
  s.text3 = C(0x877759);
  s.accent = C(0xFFC24A);
  s.accent2 = C(0xFFE8AE);
  s.accentSoft = C(0xFFC24A, 34);
  s.accentGlow = C(0xFFC24A, 120);
  s.good = C(0x5BD9A0);
  s.warn = C(0xFF9F45);
  s.bad = C(0xFF6B5A);
  s.money = C(0xFFE28A);

  s.radius = 6.f;
  s.cardRadius = 4.f;
  s.shadowK = 1.f;
  s.light = 0.f;
  s.strokeW = 1.f;
  s.dark = true;
  // 升阳的视觉语言是切角装甲板，不是圆角。这是六套皮肤里唯一改形状的一套。
  s.shape = paint::BoxShape::Chamfer;
  s.chamfer = 7.f;
  return s;
}

// ---------------------------------------------------------------------------
// 6. 磁暴 TESLA — 彩蛋：紫电，冷紫 + 亮紫描边。
// ---------------------------------------------------------------------------
Skin MakeTesla() {
  Skin s;
  s.win0 = C(0x151021);
  s.win1 = C(0x0C0814);
  s.winStroke = C(0xC6A8FF, 32);
  s.winStroke2 = C(0x000000, 96);
  s.sidebar0 = C(0x110D1B, 232);
  s.sidebar1 = C(0x090612, 242);
  s.hairline = C(0xB47BFF, 34);
  s.divider = C(0xC6A8FF, 14);
  s.scrim = C(0x000000, 80);
  s.shadow = C(0x05020A, 210);

  s.card0 = C(0xC9A8FF, 18);
  s.card1 = C(0xC9A8FF, 6);
  s.cardStroke = C(0xB47BFF, 22);
  s.cardInnerHi = C(0xE2D0FF, 30);
  s.cardInnerLo = C(0x000000, 62);
  s.knob0 = C(0xF8F2FF);
  s.knob1 = C(0xE4D6FF);
  s.trackOff0 = C(0xC9A8FF, 28);
  s.trackOff1 = C(0xC9A8FF, 15);
  s.trackOn0 = C(0x9A5CFF);
  s.trackOn1 = C(0xD0A8FF);
  s.segTrack = C(0x06030D, 92);
  s.segPill = C(0xC6A8FF, 24);
  s.segStroke = C(0xC9A8FF, 22);
  s.field0 = C(0xC9A8FF, 12);
  s.field1 = C(0xC9A8FF, 5);
  s.fieldStroke = C(0xC9A8FF, 18);

  s.text = C(0xF3EEFF);
  s.text2 = C(0xAFA0C8);
  s.text3 = C(0x736790);
  s.accent = C(0xB47BFF);
  s.accent2 = C(0xE6C8FF);
  s.accentSoft = C(0xB47BFF, 36);
  s.accentGlow = C(0xB47BFF, 120);
  s.good = C(0x5BE0B0);
  s.warn = C(0xFFB44D);
  s.bad = C(0xFF6A8A);
  s.money = C(0xFFD86B);

  s.radius = 16.f;
  s.cardRadius = 12.f;
  s.shadowK = 1.15f;
  s.light = 0.f;
  s.strokeW = 1.f;
  s.dark = true;
  return s;
}

Skin MixSkin(const Skin& a, const Skin& b, float t) {
  if (t <= 0.f) return a;
  if (t >= 1.f) return b;
  Skin r = a;
#define SKIN_MIX(f) r.f = Mix(a.f, b.f, t)
  SKIN_MIX(win0); SKIN_MIX(win1); SKIN_MIX(winStroke); SKIN_MIX(winStroke2);
  SKIN_MIX(sidebar0); SKIN_MIX(sidebar1); SKIN_MIX(hairline); SKIN_MIX(divider);
  SKIN_MIX(scrim); SKIN_MIX(shadow);
  SKIN_MIX(card0); SKIN_MIX(card1); SKIN_MIX(cardStroke); SKIN_MIX(cardInnerHi);
  SKIN_MIX(cardInnerLo);
  SKIN_MIX(knob0); SKIN_MIX(knob1);
  SKIN_MIX(trackOff0); SKIN_MIX(trackOff1); SKIN_MIX(trackOn0); SKIN_MIX(trackOn1);
  SKIN_MIX(segTrack); SKIN_MIX(segPill); SKIN_MIX(segStroke);
  SKIN_MIX(field0); SKIN_MIX(field1); SKIN_MIX(fieldStroke);
  SKIN_MIX(text); SKIN_MIX(text2); SKIN_MIX(text3);
  SKIN_MIX(accent); SKIN_MIX(accent2); SKIN_MIX(accentSoft); SKIN_MIX(accentGlow);
  SKIN_MIX(good); SKIN_MIX(warn); SKIN_MIX(bad); SKIN_MIX(money);
#undef SKIN_MIX
  r.radius = a.radius + (b.radius - a.radius) * t;
  r.cardRadius = a.cardRadius + (b.cardRadius - a.cardRadius) * t;
  r.shadowK = a.shadowK + (b.shadowK - a.shadowK) * t;
  r.light = a.light + (b.light - a.light) * t;
  r.strokeW = a.strokeW + (b.strokeW - a.strokeW) * t;
  // 形状是离散的，不能插值：过渡到一半时切换，肉眼看不出跳变。
  r.shape = t < 0.5f ? a.shape : b.shape;
  r.chamfer = a.chamfer + (b.chamfer - a.chamfer) * t;
  r.dark = t < 0.5f ? a.dark : b.dark;
  return r;
}

const Def g_defs[] = {
    {u8"曜石", "OBSIDIAN", "BASE / DARK", "obsidian", u8"默认深色，最接近改版前观感",
     MakeObsidian(), C(0x4DA3FF), C(0x9BE0FF)},
    {u8"云母", "MICA", "BASE / LIGHT", "mica", u8"亮色纸感，需要更高的窗口不透明度",
     MakeMica(), C(0x0A84FF), C(0x5AC8FA)},
    {u8"深潮", "TIDE", "ALLIED", "tide", u8"盟军冷蓝青，理性界面感",
     MakeTide(), C(0x2FC2F5), C(0x9BF0FF)},
    {u8"赤铁", "CRIMSON", "SOVIET", "crimson", u8"赤红硬朗，圆角收紧",
     MakeCrimson(), C(0xFF6A4A), C(0xFFC08A)},
    {u8"旭日", "AURUM", "EMPIRE", "aurum", u8"墨金切角，几何感最强",
     MakeAurum(), C(0xFFC24A), C(0xFFE8AE)},
    {u8"磁暴", "TESLA", "BONUS", "tesla", u8"紫电彩蛋，冷紫高光",
     MakeTesla(), C(0xB47BFF), C(0xE6C8FF)},
};

constexpr int kCount = static_cast<int>(sizeof(g_defs) / sizeof(g_defs[0]));

int g_current = 0;      // 目标皮肤
Skin g_from;            // 过渡起点
float g_t = 1.f;        // 0..1
float g_dur = 0.f;
Skin g_live;            // 当前帧使用的实时皮肤
bool g_live_init = false;
float g_fade = 1.f;     // 全局淡出系数
bool g_fade_dirty = false;

void RefreshLive() {
  const float k = Clamp01(g_t);
  const Skin& to = g_defs[(g_current < 0 || g_current >= kCount) ? 0 : g_current].s;
  if (k >= 1.f) {
    g_live = to;
  } else {
    // smoothstep：起止平缓，中段快，切换观感更像原生过渡。
    const float e = k * k * (3.f - 2.f * k);
    g_live = MixSkin(g_from, to, e);
  }
  if (g_fade < 0.999f) {
    // 淡出：只压 alpha，不改 RGB —— 否则过渡中会先"变灰"再消失。
    Skin& r = g_live;
    const float f = g_fade;
#define SKIN_FADE(field) r.field = Fade(r.field, f)
    SKIN_FADE(win0); SKIN_FADE(win1); SKIN_FADE(winStroke); SKIN_FADE(winStroke2);
    SKIN_FADE(sidebar0); SKIN_FADE(sidebar1); SKIN_FADE(hairline); SKIN_FADE(divider);
    SKIN_FADE(scrim); SKIN_FADE(shadow);
    SKIN_FADE(card0); SKIN_FADE(card1); SKIN_FADE(cardStroke); SKIN_FADE(cardInnerHi);
    SKIN_FADE(cardInnerLo); SKIN_FADE(knob0); SKIN_FADE(knob1);
    SKIN_FADE(trackOff0); SKIN_FADE(trackOff1); SKIN_FADE(trackOn0); SKIN_FADE(trackOn1);
    SKIN_FADE(segTrack); SKIN_FADE(segPill); SKIN_FADE(segStroke);
    SKIN_FADE(field0); SKIN_FADE(field1); SKIN_FADE(fieldStroke);
    SKIN_FADE(text); SKIN_FADE(text2); SKIN_FADE(text3);
    SKIN_FADE(accent); SKIN_FADE(accent2); SKIN_FADE(accentSoft); SKIN_FADE(accentGlow);
    SKIN_FADE(good); SKIN_FADE(warn); SKIN_FADE(bad); SKIN_FADE(money);
#undef SKIN_FADE
  }
  g_fade_dirty = false;
  g_live_init = true;
}

ImVec4 ToVec4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImVec4 ScaleAlpha(ImVec4 v, float mul) {
  const float a = v.w * mul;
  v.w = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
  return v;
}

}  // namespace

int Count() { return kCount; }

const Def& At(int i) { return g_defs[(i < 0 || i >= kCount) ? 0 : i]; }

int IndexOfKey(const char* key) {
  if (!key || !key[0]) return -1;
  for (int i = 0; i < kCount; ++i) {
    if (std::strcmp(g_defs[i].key, key) == 0) return i;
  }
  return -1;
}

const char* KeyAt(int i) { return g_defs[(i < 0 || i >= kCount) ? 0 : i].key; }

int Current() { return g_current; }

const Skin& Active() {
  if (!g_live_init || g_fade_dirty) RefreshLive();
  return g_live;
}

void SetCurrent(int i) {
  g_current = (i < 0 || i >= kCount) ? 0 : i;
  g_from = g_defs[g_current].s;
  g_t = 1.f;
  g_dur = 0.f;
  RefreshLive();
}

void Select(int i, float seconds) {
  g_current = (i < 0 || i >= kCount) ? 0 : i;
  if (seconds <= 0.f) {
    SetCurrent(g_current);
    return;
  }
  g_from = Active();
  g_t = 0.f;
  g_dur = seconds;
}

void Update(float dt) {
  if (g_t >= 1.f) return;
  if (g_dur <= 0.0001f) {
    g_t = 1.f;
    RefreshLive();
    return;
  }
  g_t += (dt > 0.f ? dt : 0.f) / g_dur;
  if (g_t > 1.f) g_t = 1.f;
  RefreshLive();
}

bool Transitioning() { return g_t < 1.f; }

bool SetFade(float k) {
  const float v = Clamp01(k);
  if (v == g_fade) return false;
  g_fade = v;
  g_fade_dirty = true;
  return true;
}

float FadeLevel() { return g_fade; }

float MinOpacity(int i) {
  const bool light = At(i).s.dark == false;
  return light ? 0.72f : 0.35f;
}

void BuildStyle(ImGuiStyle& s, const Skin& sk, float opacity) {
  const float op = Clamp01(opacity);

  s = ImGuiStyle();
  s.WindowPadding = ImVec2(12, 12);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(10, 8);
  s.ItemInnerSpacing = ImVec2(8, 5);
  s.WindowRounding = sk.radius * 0.85f;
  s.ChildRounding = sk.cardRadius * 0.80f;
  s.FrameRounding = sk.cardRadius * 0.50f;
  s.PopupRounding = sk.cardRadius * 0.86f;
  s.ScrollbarRounding = sk.cardRadius * 0.60f;
  s.GrabRounding = sk.cardRadius * 0.42f;
  s.TabRounding = sk.cardRadius * 0.50f;
  s.WindowBorderSize = sk.strokeW;
  s.ChildBorderSize = sk.strokeW;
  s.FrameBorderSize = 0.0f;
  s.PopupBorderSize = sk.strokeW;
  s.ScrollbarSize = 12.0f;
  s.GrabMinSize = 10.0f;

  ImVec4* c = s.Colors;

  // 窗口本体：alpha 留在这里是"设计 alpha"，整体透明度由
  // SetNextWindowBgAlpha(opacity) 再乘一次，避免两处重复衰减。
  c[ImGuiCol_Text] = Vec4(sk.text);
  c[ImGuiCol_TextDisabled] = Vec4(sk.text3);
  c[ImGuiCol_WindowBg] = Vec4(sk.win0);
  c[ImGuiCol_ChildBg] = ScaleAlpha(Vec4(sk.sidebar0), op);
  c[ImGuiCol_PopupBg] = Vec4(TipBg(sk));  // 提示框必须"深框浅字/浅框深字"成对，见 TipBg()
  c[ImGuiCol_Border] = Vec4(WithA(sk.winStroke2, A(sk.winStroke2)));
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

  c[ImGuiCol_FrameBg] = Vec4(sk.field0);
  c[ImGuiCol_FrameBgHovered] = Vec4(Lighten(sk.field0, sk.dark ? 0.10f : 0.04f));
  c[ImGuiCol_FrameBgActive] = Vec4(sk.accentSoft);

  c[ImGuiCol_TitleBg] = Vec4(sk.win1);
  c[ImGuiCol_TitleBgActive] = Vec4(Mix(sk.win1, sk.accent, 0.34f));
  c[ImGuiCol_TitleBgCollapsed] = Vec4(Fade(sk.win1, 0.85f));
  c[ImGuiCol_MenuBarBg] = Vec4(sk.sidebar1);

  c[ImGuiCol_ScrollbarBg] = Vec4(WithA(sk.scrim, A(sk.scrim)));
  c[ImGuiCol_ScrollbarGrab] = Vec4(Mix(sk.text3, sk.accent, 0.25f));
  c[ImGuiCol_ScrollbarGrabHovered] = Vec4(sk.accent);
  c[ImGuiCol_ScrollbarGrabActive] = Vec4(sk.accent2);

  c[ImGuiCol_CheckMark] = Vec4(sk.accent2);
  c[ImGuiCol_SliderGrab] = Vec4(sk.accent);
  c[ImGuiCol_SliderGrabActive] = Vec4(sk.accent2);

  c[ImGuiCol_Button] = Vec4(Mix(sk.accent, sk.win0, 0.52f));
  c[ImGuiCol_ButtonHovered] = Vec4(Mix(sk.accent, sk.win0, 0.22f));
  c[ImGuiCol_ButtonActive] = Vec4(Darken(sk.accent, 0.16f));

  c[ImGuiCol_Header] = Vec4(Fade(sk.accent, 0.34f));
  c[ImGuiCol_HeaderHovered] = Vec4(Fade(sk.accent, 0.55f));
  c[ImGuiCol_HeaderActive] = Vec4(sk.accent);

  c[ImGuiCol_Separator] = Vec4(sk.divider);
  c[ImGuiCol_SeparatorHovered] = Vec4(Fade(sk.accent, 0.80f));
  c[ImGuiCol_SeparatorActive] = Vec4(sk.accent);

  c[ImGuiCol_ResizeGrip] = Vec4(Fade(sk.accent, 0.28f));
  c[ImGuiCol_ResizeGripHovered] = Vec4(Fade(sk.accent, 0.60f));
  c[ImGuiCol_ResizeGripActive] = Vec4(sk.accent);

  c[ImGuiCol_Tab] = Vec4(Fade(sk.win1, 0.92f));
  c[ImGuiCol_TabHovered] = Vec4(Fade(sk.accent, 0.78f));
  c[ImGuiCol_TabSelected] = Vec4(Mix(sk.win1, sk.accent, 0.42f));
  c[ImGuiCol_TabSelectedOverline] = Vec4(sk.accent2);
  c[ImGuiCol_TabDimmed] = Vec4(Fade(sk.win1, 0.70f));
  c[ImGuiCol_TabDimmedSelected] = Vec4(Mix(sk.win1, sk.accent, 0.26f));

  c[ImGuiCol_PlotLines] = Vec4(sk.accent2);
  c[ImGuiCol_PlotLinesHovered] = Vec4(sk.accent);
  c[ImGuiCol_PlotHistogram] = Vec4(sk.accent);
  c[ImGuiCol_PlotHistogramHovered] = Vec4(sk.accent2);

  c[ImGuiCol_TableHeaderBg] = Vec4(Fade(sk.accent, 0.22f));
  c[ImGuiCol_TableBorderStrong] = Vec4(sk.divider);
  c[ImGuiCol_TableBorderLight] = Vec4(Fade(sk.divider, 0.55f));
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = Vec4(Fade(sk.card0, 0.55f));

  c[ImGuiCol_TextSelectedBg] = Vec4(Fade(sk.accent, 0.38f));
  c[ImGuiCol_TextLink] = Vec4(sk.accent2);
  c[ImGuiCol_DragDropTarget] = Vec4(sk.accent2);
  c[ImGuiCol_NavCursor] = Vec4(sk.accent);
  c[ImGuiCol_ModalWindowDimBg] = Vec4(WithA(sk.scrim, A(sk.scrim) > 150 ? A(sk.scrim) : 150));
  c[ImGuiCol_InputTextCursor] = Vec4(sk.accent2);
  c[ImGuiCol_TreeLines] = Vec4(sk.divider);
}

ImU32 Fade(ImU32 c, float k) {
  const float kk = Clamp01(k);
  return WithA(c, static_cast<int>(static_cast<float>(A(c)) * kk + 0.5f));
}

ImU32 WithA(ImU32 c, int a) {
  const int aa = a < 0 ? 0 : (a > 255 ? 255 : a);
  return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(aa) << IM_COL32_A_SHIFT);
}

ImU32 Lighten(ImU32 c, float k) {
  const float kk = Clamp01(k);
  auto up = [kk](int v) { return static_cast<int>(static_cast<float>(v) + (255.f - static_cast<float>(v)) * kk + 0.5f); };
  return IM_COL32(up(R(c)), up(G(c)), up(B(c)), A(c));
}

ImU32 Darken(ImU32 c, float k) {
  const float kk = Clamp01(k);
  auto dn = [kk](int v) { return static_cast<int>(static_cast<float>(v) * (1.f - kk) + 0.5f); };
  return IM_COL32(dn(R(c)), dn(G(c)), dn(B(c)), A(c));
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  if (t <= 0.f) return a;
  if (t >= 1.f) return b;
  auto lerp = [t](int x, int y) { return static_cast<int>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f); };
  return IM_COL32(lerp(R(a), R(b)), lerp(G(a), G(b)), lerp(B(a), B(b)), lerp(A(a), A(b)));
}

ImU32 Over(ImU32 fg, ImU32 bg) {
  const int fa = A(fg);
  if (fa >= 255) return fg;
  if (fa <= 0) return bg;
  auto blend = [fa](int f, int b) {
    return static_cast<int>((static_cast<float>(f) * static_cast<float>(fa) +
                             static_cast<float>(b) * static_cast<float>(255 - fa)) /
                                255.f +
                            0.5f);
  };
  const int ba = A(bg);
  const int out_a = fa + ba * (255 - fa) / 255;
  return IM_COL32(blend(R(fg), R(bg)), blend(G(fg), G(bg)), blend(B(fg), B(bg)),
                  out_a > 255 ? 255 : out_a);
}

ImVec4 Vec4(ImU32 c) { return ToVec4(c); }

// ---- 语义合成面 ----
ImU32 PanelBg(const Skin& s) { return Over(s.card0, WithA(s.win1, 236)); }
ImU32 PanelBgAlt(const Skin& s) { return Over(s.card1, WithA(s.win1, 240)); }
ImU32 FieldBg(const Skin& s) { return Over(s.field0, WithA(s.win1, 244)); }
ImU32 ChipBg(const Skin& s) {
  return Over(s.dark ? WithA(IM_COL32(0, 0, 0, 255), 190) : s.field1, WithA(s.win1, 240));
}
ImU32 EdgeIdle(const Skin& s) { return Over(Fade(s.accent, s.dark ? 0.30f : 0.34f), s.win1); }
ImU32 EdgeHot(const Skin& s) { return Fade(s.accent, 0.80f); }

// ---- 悬浮提示（tooltip）----
// 只改 alpha 不管 RGB 是这里过去最大的坑：深潮的 card1 是 9FE6FF/6%，
// 顶到 242 之后就是一块近白的实心板，而正文是 EAF9FF（也是近白）——
// 浅字压浅框，鼠标一悬停就什么都看不见。所以提示框一律走"合成后的实色"：
//   暗色皮肤 = 卡片色叠窗底再压暗一档 → 深框浅字
//   亮色皮肤 = 卡片色叠窗底           → 浅框深字
ImU32 TipBg(const Skin& s) {
  ImU32 c = Over(s.card0, WithA(s.win1, 250));
  if (s.dark) c = Darken(c, 0.24f);
  return WithA(c, 255);  // 提示框单独浮在游戏画面上，必须不透明才读得清
}
ImU32 TipTextDim(const Skin& s) {
  // 提示里的"次要行"（快捷键、数量说明）如果直接用 text3，暗色皮肤下
  // 只有 ~35% 亮度，叠在提示框上仍然发虚；这里往主文字方向提一档。
  return Mix(s.text2, s.text, 0.35f);
}

void PushTipStyle() {
  const Skin& s = Active();
  ImGui::PushStyleColor(ImGuiCol_PopupBg, Vec4(TipBg(s)));
  ImGui::PushStyleColor(ImGuiCol_Text, Vec4(WithA(s.text, 255)));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, Vec4(WithA(TipTextDim(s), 255)));
  ImGui::PushStyleColor(ImGuiCol_Border, Vec4(WithA(EdgeIdle(s), 160)));
  ImGui::PushStyleColor(ImGuiCol_Separator, Vec4(s.divider));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, s.cardRadius * 0.55f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
}
void PopTipStyle() {
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(5);
}

ImVec4 Vec4A(ImU32 c, float mul) { return ScaleAlpha(ToVec4(c), mul); }

}  // namespace skin
}  // namespace ui
