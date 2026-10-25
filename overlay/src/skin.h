#pragma once

// 皮肤 / 主题系统。
//
// 设计目标：界面里不再出现任何字面色值，所有颜色都来自 Skin 的语义槽位。
// 新增一套皮肤 = 在 skin.cpp 的 g_defs 里加一张表，不需要改界面代码。
//
// 颜色全部是 ImU32（含 alpha）：画控件时直接给 ImDrawList，做 ImGuiStyle
// 时用 Vec4A() 转换并按窗口透明度衰减。

#include <cstdint>

#include "imgui.h"
#include "paint.h"  // 复用 paint::BoxShape（形状语言在 paint 里定义，这里只引用）

namespace ui {
namespace skin {

struct Skin {
  // 窗口本体
  ImU32 win0, win1;             // 窗口背景渐变端点（上 / 下）
  ImU32 winStroke, winStroke2;  // 窗口外高光描边 / 内侧暗描边
  ImU32 sidebar0, sidebar1;     // 侧栏 / 子面板底色
  ImU32 hairline, divider;      // 极细装饰线 / 常规分割线
  ImU32 scrim, shadow;          // 压暗层 / 投影

  // 卡片与控件
  ImU32 card0, card1;  // 卡片渐变
  ImU32 cardStroke, cardInnerHi, cardInnerLo;
  ImU32 knob0, knob1;  // 滑条滑块
  ImU32 trackOff0, trackOff1, trackOn0, trackOn1;
  ImU32 segTrack, segPill, segStroke;
  ImU32 field0, field1, fieldStroke;

  // 文字
  ImU32 text, text2, text3;  // 主 / 次要 / 弱化

  // 语义强调
  ImU32 accent, accent2, accentSoft, accentGlow;
  ImU32 good, warn, bad;
  ImU32 money;  // 资金等强调数值

  // 材质几何
  float radius = 20.f;      // 外层圆角基准
  float cardRadius = 16.f;  // 卡片圆角基准
  float shadowK = 1.f;      // 投影强度系数
  float light = 0.f;        // 0 = 暗色皮肤，1 = 亮色皮肤
  float strokeW = 1.f;      // 描边基准宽度
  bool dark = true;
  // 形状语言：圆角矩形 / 切角（八角）矩形。离散值，过渡时在中点切换。
  paint::BoxShape shape = paint::BoxShape::Round;
  float chamfer = 7.f;  // 切角尺寸（仅 Chamfer 生效）
};

// 开关：暗色皮肤里"高光叠层"要更克制，亮色皮肤要更强才看得出层次。
inline bool IsLight(const Skin& s) { return s.light > 0.5f; }

struct Def {
  const char* name_cn;
  const char* name_en;
  const char* tag;      // 阵营 / 类型标签
  const char* key;      // 写进 ini 的稳定键名
  const char* caption;  // 主题卡上的一句描述
  Skin s;
  ImU32 preview0, preview1;  // 主题卡上的预览渐变色
};

int Count();
const Def& At(int i);
int IndexOfKey(const char* key);  // 找不到返回 -1
const char* KeyAt(int i);

int Current();
const Skin& Active();  // 过渡中的实时皮肤

// 整窗淡入淡出的全局系数：1 = 正常，0 = 完全透明。
//
// 为什么不逐控件传 alpha：自绘控件取色一律走 Active()，把淡出乘在
// 皮肤槽位上，几十个控件就自动一起淡，不用每个都记得乘。
// 返回 true 表示值变了（调用方据此决定是否重建 ImGuiStyle / 重画）。
bool SetFade(float k);
float FadeLevel();

// 立即切换（读 ini 恢复时用，不播过渡）。
void SetCurrent(int i);
// 带过渡切换；seconds <= 0 等价于 SetCurrent。
void Select(int i, float seconds = 0.26f);
// 每帧推进过渡，dt 为帧时间。没有过渡时是空操作。
void Update(float dt);
bool Transitioning();

// 亮色皮肤在游戏画面上更难读，透明度下限更高。
float MinOpacity(int i);

// 把皮肤写进 ImGuiStyle（会先重置为默认 geometry，只写颜色 + 圆角，
// 不调用 ScaleAllSizes —— 缩放由 ui.cpp 统一处理）。
void BuildStyle(ImGuiStyle& s, const Skin& sk, float opacity);

// ---- 语义合成面 ----
// 光有槽位还不够：同一个"行底色"在暗色皮肤里是"白 6% 叠窗底"，
// 亮色皮肤里却是"纯白"，直接写字面值必然错。这里把这类合成集中一处。
//
// 实心面：卡片色 over 窗底，得到比窗口高一档的实心块。
ImU32 PanelBg(const Skin& s);
ImU32 PanelBgAlt(const Skin& s);
// 凹槽面：输入框 / 滑条轨道 / 内嵌区域（比面板更暗一层）。
ImU32 FieldBg(const Skin& s);
// 角标底：快捷键、数量小块的底。
ImU32 ChipBg(const Skin& s);
// 细线描边。cardStroke 在暗色皮肤里只有 ~6% 白，叠到窗底上和面板同色、
// 边框会消失，所以按 accent 少量着色，保证看得见且跟随主题。
ImU32 EdgeIdle(const Skin& s);
ImU32 EdgeHot(const Skin& s);

// ---- 悬浮提示（tooltip）----
// 提示框不能直接拿 card1 顶 alpha：很多皮肤的 card1 是 6% 透明的高光色
// （深潮就是个近白的淡青），把 alpha 强行写到 242 会得到"近白实心框"，
// 再配深色皮肤那套浅色正文，就变成浅字压浅框、完全看不清。
// 这里统一用 PanelBg 合成后的实色，暗色皮肤再压暗一档，保证和正文对比度够。
ImU32 TipBg(const Skin& s);
ImU32 TipTextDim(const Skin& s);  // 提示框里的次要文字（TextDisabled）
// 在提示框（BeginTooltip / SetTooltip）前后各调一次，把配色压成高对比的一套。
void PushTipStyle();
void PopTipStyle();

// ---- 颜色工具 ----
ImU32 Fade(ImU32 c, float k);      // 按比例缩放 alpha
ImU32 WithA(ImU32 c, int a);       // 替换 alpha
ImU32 Lighten(ImU32 c, float k);   // 向白色插值
ImU32 Darken(ImU32 c, float k);    // 向黑色插值
ImU32 Mix(ImU32 a, ImU32 b, float t);
// 按 fg 自身 alpha 把 fg 叠在 bg 上（over-composite）。叠半透明卡片色时必须用它，
// 否则 Mix 会把 RGB 一起插值，暗底上的浅卡片会被算成灰白。
ImU32 Over(ImU32 fg, ImU32 bg);
ImVec4 Vec4(ImU32 c);
ImVec4 Vec4A(ImU32 c, float mul);

// ---- 电力供需要不要报警 ----
// 判断"电够不够"只在 skin 层算一次，标准 / 精简 / 极简三个视图都调这里，
// 免得各写一份阈值、出现"总电力小于使用电力却还是绿的"这种分歧。
enum class PowerLevel {
  None,     // 0/0：没有电网，不上色
  Deficit,  // 使用 > 总量（或贴到 95%）：红
  Tight,    // 75%~95%：黄
  Ok,       // 余量充足：绿
};
inline PowerLevel PowerLevelOf(uint32_t used, uint32_t total) {
  if (total == 0 && used == 0) return PowerLevel::None;
  if (used > total) return PowerLevel::Deficit;  // 总电力 < 使用电力，供不应求
  if (total == 0) return PowerLevel::Deficit;    // 有消耗但没有产能，同样是赤字
  const float frac = (float)used / (float)total;
  if (frac >= 0.95f) return PowerLevel::Deficit;
  if (frac >= 0.75f) return PowerLevel::Tight;
  return PowerLevel::Ok;
}
inline ImU32 PowerColor(const Skin& s, PowerLevel lv) {
  switch (lv) {
    case PowerLevel::Deficit: return s.bad;
    case PowerLevel::Tight: return s.warn;
    case PowerLevel::Ok: return s.good;
    default: return s.text3;
  }
}

}  // namespace skin
}  // namespace ui
