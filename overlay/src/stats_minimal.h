// 战况统计「极简模式」：一个玩家一行的排版。
//
// 单独成文件的原因：这套排版不碰游戏数据，只吃 POD，所以离线预览工具
// （tools/skin_preview --stats）能直接编译同一个实现，导出截图核对行高、
// 列对齐、图标排布、窄窗口下的截断。放在 ui.cpp 里就只能进游戏肉眼看了。
#pragma once

#include <cstdint>

#include "imgui.h"

namespace stats_minimal {

// 一行里的兵种图标。名字只用于图标查表和悬停，不影响排版。
struct UnitIcon {
  const char* key = "";
  const char* name = "";
  int count = 0;
};

struct Row {
  const char* name = "";
  bool is_local = false;
  bool defeated = false;
  bool has_color = false;
  uint8_t color_r = 210, color_g = 214, color_b = 220;
  bool has_money = false;
  uint32_t money = 0;
  bool has_power = false;
  uint32_t power_used = 0;
  uint32_t power_total = 0;
  int unit_total = 0;
  int building_total = 0;
  const UnitIcon* units = nullptr;
  int unit_count = 0;
};

// 四列数字的宽度取"当前所有行"的最大值，所以名字长短不一也不会错位。
struct Cols {
  float name = 0.f;
  float name_off = 0.f;  // 从左边界到名字列起点的偏移（色块 + 内边距）
  float money = 0.f;
  float power = 0.f;
  float unit = 0.f;
  float build = 0.f;
  float gap = 0.f;
  float Total() const { return name + money + power + unit + build + gap * 5.f; }
};

// 图标来源。stats_minimal 不链接 unit_icons（预览工具没有它），
// 由调用方注入：游戏里接 unit_icons::get，预览里返回空 -> 画占位块。
using IconFn = ImTextureID (*)(const char* type_key, const char* disp_name, bool is_building);
void SetIconProvider(IconFn fn);

// 整个兵种格子的画法。游戏里注入精简模式那份 draw_compact_icon_ex，
// 极简和精简的格子因此像素一致；预览不注入，走本文件里的保底画法。
using ChipFn = void (*)(ImDrawList* dl, ImVec2 p0, float cell, const UnitIcon& u, ImU32 border,
                        bool defeated);
void SetChipRenderer(ChipFn fn);

// 图标倍率（设置项，默认 1.5）：极简模式自己算格子边长，所以由调用方把
// 当前倍率推进来，和标准 / 精简视图用同一个值。
void SetChipScale(float scale);
float ChipScale();

// px: 高 DPI / UI 缩放下的像素换算（调用方传 ui_px，预览传自己的缩放）。
void Measure(Cols* c, const Row* rows, int n, float avail, float px);

// 表头一行（玩家 / 资金 / 电力 / 建筑 / 兵种）+ 下沿分割线。
void DrawHeader(const Cols& c, float px);

// 画一行，返回这一行的屏幕矩形，方便调用方挂 tooltip。
// icons_only: 极简模式只要兵种图标（不带建筑），表头也据此少一列。
void DrawRow(const Row& r, int index, const Cols& c, float px, ImVec2* out_min,
             ImVec2* out_max);

// 一行里兵种图标占的宽度（给调用方判断窗口要多宽才不挤）。
float IconStripWidth(const Row& r, float px);

}  // namespace stats_minimal
