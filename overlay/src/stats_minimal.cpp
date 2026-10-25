#include "stats_minimal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "skin.h"

// skin 在 ui::skin 里；这里起个别名，正文就能照常写 skin::Fade / skin::Active。
namespace skin = ui::skin;

namespace stats_minimal {
namespace {

const skin::Skin& SK() { return skin::Active(); }

IconFn g_icon_fn = nullptr;
ChipFn g_chip_fn = nullptr;
// 图标倍率：默认 1.5（原尺寸偏小）。和 ui.cpp 的设置项同步。
float g_chip_scale = 1.5f;

void group_uint(char* out, size_t n, uint32_t v) {
  char raw[24];
  std::snprintf(raw, sizeof(raw), "%u", v);
  const int len = (int)std::strlen(raw);
  int o = 0;
  for (int i = 0; i < len && o < (int)n - 1; ++i) {
    if (i > 0 && (len - i) % 3 == 0) out[o++] = ',';
    out[o++] = raw[i];
  }
  out[o] = 0;
}

void money_text(char* out, size_t n, const Row& r) {
  if (r.has_money) group_uint(out, n, r.money);
  else std::snprintf(out, n, u8"—");
}

void power_text(char* out, size_t n, const Row& r) {
  if (!r.has_power) {
    std::snprintf(out, n, u8"—");
    return;
  }
  char used[24], total[24];
  group_uint(used, sizeof(used), r.power_used);
  group_uint(total, sizeof(total), r.power_total);
  std::snprintf(out, n, "%s/%s", used, total);
}

void row_label(char* out, size_t n, const Row& r) {
  const char* title = (r.name && r.name[0]) ? r.name : u8"玩家";
  std::snprintf(out, n, "%s%s%s", title, r.is_local ? u8" ·己" : "",
                r.defeated ? u8" ·败" : "");
}

ImU32 accent_of(const Row& r) {
  if (r.has_color) return IM_COL32(r.color_r, r.color_g, r.color_b, 255);
  return r.is_local ? SK().money : SK().text2;
}

// 图标格子边长：跟随字体行高和设置里的倍率，缩放时一起长大。
inline float cell_px(float lh) {
  const float s = (g_chip_scale > 0.05f) ? g_chip_scale : 1.f;
  return (std::max)(14.f, lh * 1.32f) * s;
}

// 一个兵种格子：游戏中由 ui.cpp 注入精简模式那份画法（像素一致）；
// 预览 / 未注入时用这里的保底画法（图标 + 数量角标）。
void draw_chip(ImDrawList* dl, ImVec2 p0, float cell, const UnitIcon& u, ImU32 border,
               bool defeated) {
  if (g_chip_fn) {
    g_chip_fn(dl, p0, cell, u, border, defeated);
    return;
  }
  const ImVec2 p1(p0.x + cell, p0.y + cell);
  const float round = cell * 0.22f;
  // 底色用实心面板色（和标准视图的图标格子一致）：图标本身是彩色纹理，
  // 底再半透明的话在暗底上整片糊掉。描边给足，格子边界才立得住。
  dl->AddRectFilled(p0, p1, skin::PanelBg(SK()), round);
  dl->AddRect(p0, p1, skin::Fade(skin::EdgeIdle(SK()), 0.95f), round, 0, 1.f);

  const float inset = cell * 0.12f;
  ImTextureID tex = 0;
  if (g_icon_fn && u.key) tex = g_icon_fn(u.key, u.name, false);
  if (tex) {
    dl->AddImage(tex, ImVec2(p0.x + inset, p0.y + inset),
                 ImVec2(p1.x - inset, p1.y - inset));
  } else if (u.name && u.name[0]) {
    // 预览 / 查表失败：用兵种名首字顶一下，位置和真图标一致。
    const unsigned char c = (unsigned char)u.name[0];
    int step = 1;
    if ((c & 0xE0) == 0xC0) step = 2;
    else if ((c & 0xF0) == 0xE0) step = 3;
    else if ((c & 0xF8) == 0xF0) step = 4;
    char one[8] = {};
    std::memcpy(one, u.name, (size_t)(std::min)(step, 7));
    ImFont* f = ImGui::GetFont();
    const float fsz = cell * 0.62f;
    const ImVec2 ts = f->CalcTextSizeA(fsz, FLT_MAX, 0.f, one);
    dl->AddText(f, fsz, ImVec2(p0.x + (cell - ts.x) * 0.5f, p0.y + (cell - ts.y) * 0.5f - inset),
                SK().text, one);
  }

  // 数量角标：贴在右下角，字比正文小一号，别把图标盖住。
  char cnt[16];
  std::snprintf(cnt, sizeof(cnt), "%d", u.count);
  // 兜底画法的数量角标也跟格子一起放大。
  ImFont* font = ImGui::GetFont();
  const float csz = (std::max)(9.f, (std::min)(cell * 0.36f, ImGui::GetFontSize() * 1.4f));
  const ImVec2 cts = font->CalcTextSizeA(csz, FLT_MAX, 0.f, cnt);
  const float bx1 = p1.x + 1.f;
  const float by1 = p1.y + 1.f;
  const float bx0 = bx1 - cts.x - cell * 0.20f;
  const float by0 = by1 - cts.y - cell * 0.06f;
  dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), skin::ChipBg(SK()), cell * 0.14f);
  dl->AddText(font, csz, ImVec2(bx0 + cell * 0.10f, by0 - cell * 0.02f),
              u.count > 0 ? SK().money : skin::Fade(SK().text3, 0.9f), cnt);
}

}  // namespace

void SetIconProvider(IconFn fn) { g_icon_fn = fn; }
void SetChipRenderer(ChipFn fn) { g_chip_fn = fn; }
void SetChipScale(float scale) {
  if (scale < 0.5f) scale = 0.5f;
  if (scale > 3.f) scale = 3.f;
  g_chip_scale = scale;
}
float ChipScale() { return g_chip_scale; }

float IconStripWidth(const Row& r, float px) {
  const float cell = (std::max)(cell_px(ImGui::GetTextLineHeight()), px * 28.f *
                                    ((g_chip_scale > 0.05f) ? g_chip_scale : 1.f));
  const float gap = px * 3.f;
  int n = 0;
  for (int i = 0; i < r.unit_count; ++i) {
    if (r.units[i].count > 0) ++n;
  }
  return n > 0 ? (float)n * (cell + gap) : 0.f;
}

void Measure(Cols* c, const Row* rows, int n, float avail, float px) {
  if (!c) return;
  c->name = c->money = c->power = c->unit = c->build = 0.f;
  c->gap = px * 6.f;
  char a[32], b[48], lab[128];
  float name_max = ImGui::CalcTextSize(u8"玩家").x;
  // 兵种图标不设表头：图标本身就说明是什么，排在电力列右边。
  c->unit = 0.f;
  const float cell = (std::max)(cell_px(ImGui::GetTextLineHeight()), px * 28.f *
                                    ((g_chip_scale > 0.05f) ? g_chip_scale : 1.f));
  const float igap = px * 3.f;
  float strip_max = 0.f;
  for (int i = 0; i < n; ++i) {
    const Row& r = rows[i];
    money_text(a, sizeof(a), r);
    power_text(b, sizeof(b), r);
    row_label(lab, sizeof(lab), r);
    name_max = (std::max)(name_max, ImGui::CalcTextSize(lab).x);
    c->money = (std::max)(c->money, ImGui::CalcTextSize(a).x);
    c->power = (std::max)(c->power, ImGui::CalcTextSize(b).x);
    int total = 0;
    for (int k = 0; k < r.unit_count; ++k) {
      if (r.units[k].count > 0) ++total;
    }
    if (total > 0) {
      strip_max = (std::max)(strip_max, (float)total * (cell + igap) - igap);
    }
  }
  // 列宽分配顺序：先保图标（极简模式的主角），再保数字，名字最后让位，
  // 最少留 4 个字，超出的部分走省略号。
  const float dot_w = (std::max)(6.f, ImGui::GetTextLineHeight() * 0.55f);
  const float name_x_off = px * 5.f + dot_w + px * 6.f;
  c->name_off = name_x_off;
  const float min_name = ImGui::CalcTextSize(u8"玩家名").x;
  const float rest = c->money + c->power + strip_max + c->gap * 4.f;
  const float room = avail - name_x_off - rest - px * 4.f;
  c->name = (std::min)(name_max, (std::max)(min_name, room));
  // 窗口极窄时再压列间距，压到 3px 为止。
  while (c->gap > px * 3.f && name_x_off + c->name + rest - c->gap > avail) {
    c->gap -= px;
  }
}

void DrawHeader(const Cols& c, float px) {
  const float lh = ImGui::GetTextLineHeight();
  const float avail = ImGui::GetContentRegionAvail().x;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + avail, p0.y + lh);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::Dummy(ImVec2(avail, lh));

  // 表头和行同序：玩家 → 资金 → 电力，全部左起。
  const ImU32 head = skin::Fade(SK().text3, 0.90f);
  dl->AddText(ImVec2(p0.x + px * 5.f, p0.y), head, u8"玩家");
  float x = p0.x + c.name_off + c.name + c.gap;
  auto cell = [&](const char* text, float w) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(x + w - ts.x, p0.y), head, text);
    x += w + c.gap;
  };
  cell(u8"资金", c.money);
  cell(u8"电力", c.power);
  dl->AddRectFilledMultiColor(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y + 1.f),
                              skin::WithA(SK().divider, 0), skin::WithA(SK().divider, 0),
                              skin::Fade(SK().divider, 0.7f), skin::Fade(SK().divider, 0.7f));
}

void DrawRow(const Row& r, int index, const Cols& c, float px, ImVec2* out_min,
             ImVec2* out_max) {
  const float lh = ImGui::GetTextLineHeight();
  // 格子尺寸跟 UI 缩放走（px 就是 ui_px(1)）：比精简模式的 32 略小，一行能多放几个。
  const float cell = (std::max)(cell_px(lh), px * 28.f *
                                    ((g_chip_scale > 0.05f) ? g_chip_scale : 1.f));
  const float igap = px * 3.f;
  // 行高由图标决定（图标比一行字高），字在格子里垂直居中。
  const float row_h = (std::max)(lh + px * 3.f, cell + px * 3.f);
  const float avail = ImGui::GetContentRegionAvail().x;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + avail, p0.y + row_h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (out_min) *out_min = p0;
  if (out_max) *out_max = p1;

  // 斑马行底只给偶数行一点点，够区分行又不抢图标。
  if ((index & 1) == 0) {
    dl->AddRectFilled(p0, p1, skin::Fade(SK().cardInnerHi, 0.055f), px * 3.f);
  }
  const ImU32 accent = accent_of(r);
  if (r.is_local) dl->AddRectFilled(p0, ImVec2(p0.x + px * 2.f, p1.y), accent, px);

  const float cy = p0.y + row_h * 0.5f;

  // 一行从左到右依次排：玩家名 → 资金 → 电力 → 兵种图标，全部左起，
  // 每列宽度按本帧最大值统一，所以上下行的数字和图标能对齐。
  char money_s[32], power_s[48], label[128];
  money_text(money_s, sizeof(money_s), r);
  power_text(power_s, sizeof(power_s), r);
  row_label(label, sizeof(label), r);
  ImU32 power_col = skin::Fade(SK().text3, 0.92f);
  if (r.has_power) {
    // 供需判定统一走 skin::PowerLevelOf：使用 > 总量（或贴顶）一定是红。
    power_col = skin::PowerColor(SK(), skin::PowerLevelOf(r.power_used, r.power_total));
  }
  const ImU32 dim = skin::Fade(SK().text2, 0.95f);

  // 左侧：色块 + 名字（列宽由 Measure 统一分配，超出部分直接裁掉，不加省略号）。
  const float sw = (std::max)(6.f, lh * 0.55f);
  const float dot_x = p0.x + px * 5.f;
  dl->AddRectFilled(ImVec2(dot_x, cy - sw * 0.5f), ImVec2(dot_x + sw, cy + sw * 0.5f),
                    accent, px * 1.5f);
  const float name_x = p0.x + c.name_off;
  const ImU32 name_col = r.defeated ? skin::Fade(SK().text3, 0.92f) : SK().text;
  if (ImGui::CalcTextSize(label).x <= c.name) {
    dl->AddText(ImVec2(name_x, cy - lh * 0.5f), name_col, label);
  } else {
    // 用裁剪区截断：字形在列边界处干脆断掉，不出现"…"。
    dl->PushClipRect(ImVec2(name_x, cy - lh), ImVec2(name_x + c.name, cy + lh), true);
    dl->AddText(ImVec2(name_x, cy - lh * 0.5f), name_col, label);
    dl->PopClipRect();
  }

  // 资金列：在列内右对齐，位数不同的行也能对整齐。
  float x = name_x + c.name + c.gap;
  {
    const ImVec2 ts = ImGui::CalcTextSize(money_s);
    dl->AddText(ImVec2(x + c.money - ts.x, cy - ts.y * 0.5f),
                r.has_money ? SK().money : dim, money_s);
    x += c.money + c.gap;
  }
  // 电力列。
  {
    const ImVec2 ts = ImGui::CalcTextSize(power_s);
    dl->AddText(ImVec2(x + c.power - ts.x, cy - ts.y * 0.5f), power_col, power_s);
    x += c.power + c.gap;
  }

  // 兵种区：紧跟电力列，从左往右依次排，只有图标格子。
  int total = 0;
  for (int i = 0; i < r.unit_count; ++i) {
    if (r.units[i].count > 0) ++total;
  }
  char empty_s[24];
  if (r.unit_total > 0) std::snprintf(empty_s, sizeof(empty_s), u8"×%d", r.unit_total);
  else std::snprintf(empty_s, sizeof(empty_s), u8"—");
  const float strip_left = x;
  const float right = p1.x - px * 4.f;
  const float chip_y = cy - cell * 0.5f;
  float ix = strip_left;
  int drawn = 0;
  bool clipped = false;
  // 格子的描边颜色跟玩家色走，和精简模式保持一致（自己用金钱色高亮）。
  const ImU32 chip_border =
      r.is_local ? skin::Fade(SK().money, 0.70f) : skin::Fade(accent, 0.72f);
  for (int i = 0; i < r.unit_count; ++i) {
    const UnitIcon& u = r.units[i];
    if (u.count <= 0) continue;
    const float need = cell + (drawn + 1 >= total ? 0.f : igap);
    if (ix + need > right) {
      clipped = true;
      break;
    }
    draw_chip(dl, ImVec2(ix, chip_y), cell, u, chip_border, r.defeated);
    ix += cell + igap;
    ++drawn;
  }
  if (clipped && drawn < total) {
    // 放不下的兵种直接不画（不加省略号，保持干净）。
  }
  static const bool trace_rows = std::getenv("RA3_STATS_TRACE") != nullptr;
  if (trace_rows) {
    std::fprintf(stderr,
                 "row %d name=%s avail=%.1f cell=%.1f total=%d drawn=%d "
                 "strip=%.1f right=%.1f clip=%d\n",
                 index, r.name, avail, cell, total, drawn, strip_left, right,
                 clipped ? 1 : 0);
  }
  if (total == 0) {
    // 没有兵种条目：给总数或短横，绝不整段留白（否则看起来就是"没显示"）。
    dl->AddText(ImVec2(strip_left, cy - lh * 0.5f), dim, empty_s);
  }

  ImGui::Dummy(ImVec2(avail, row_h));
}

}  // namespace stats_minimal
