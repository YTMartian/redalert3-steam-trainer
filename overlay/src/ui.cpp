#include "ui.h"
#include "input.h"
#include "game_api.h"
#include "paint.h"
#include "shell.h"
#include "skin.h"
#include "skin_gallery.h"
#include "stats_minimal.h"
#include "unit_icons.h"

#include <Windows.h>
#include <d3d9.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "imgui_plot.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {
namespace {

bool g_inited = false;
HWND g_hwnd = nullptr;
int g_group = 0;
bool g_show_settings = false;
bool g_show_build_lock = false;
bool g_show_protocol_lock = false;
bool g_show_camera = false;
bool g_show_time = false;
bool g_show_lua = false;
bool g_show_misc = false;
bool g_stats_open = false;
bool g_stats_pinned = false;
bool g_stats_compact = false;
bool g_stats_minimal = false;
std::string g_last_msg =
    u8"先启动游戏并进局，再运行修改器。菜单内点「注入」。Home 显隐主菜单。";
bool g_spectate_inject = false;
float g_sync_timer = 0.f;
volatile LONG g_injecting = 0;
volatile LONG g_engine_busy = 0;

// ---- 悬浮提示的统一入口 ----
// 提示框的底色 / 文字 / 描边必须成对换（见 skin::TipBg）：只换底色不换字色，
// 或者反过来，都会出现"浅字压浅框"。所有 tooltip 一律走下面三个函数。
void TipBegin() {
  skin::PushTipStyle();
  ImGui::BeginTooltip();
}
void TipEnd() {
  ImGui::EndTooltip();
  skin::PopTipStyle();
}
void Tip(const char* fmt, ...) {
  if (!fmt || !fmt[0]) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  skin::PushTipStyle();
  ImGui::SetTooltip("%s", buf);
  skin::PopTipStyle();
}

// UI appearance (persisted). Defaults match the previous large layout.
constexpr float kUiScaleMin = 0.70f;
constexpr float kUiScaleMax = 3.00f;  // 上限给"高分辨率下想放大看"留余量
constexpr float kUiOpacityDefault = 0.66f;
// 战况统计里的兵种 / 建筑图标倍率：默认 1.5 倍（原尺寸偏小，图标里的兵看不清）。
constexpr float kStatsIconScaleMin = 0.75f;
constexpr float kStatsIconScaleMax = 2.50f;
constexpr float kStatsIconScaleDefault = 1.50f;

float g_stats_icon_scale = kStatsIconScaleDefault;

float g_ui_scale = 1.00f;                        // kUiScaleMin ~ kUiScaleMax
float g_ui_opacity = kUiOpacityDefault;          // 0.35 ~ 1.00
// 主窗口整窗淡入淡出（0 = 完全不画，1 = 正常）。放在这里是因为 draw() 开头的
// 提前返回要先看它：菜单刚隐藏的那一帧还得继续画，否则淡出会被掐掉。
static float s_menu_fade = 0.f;
// ScaleAllSizes / FontGlobalScale do not affect hardcoded Image sizes.
static float ui_px(float v) {
  const float s = (g_ui_scale > 0.01f) ? g_ui_scale : 1.f;
  return v * s;
}

// 按钮宽度必须按文字算，不能写死像素：界面放大时字体和 FramePadding 会一起变大
// （skin 里 ScaleAllSizes + FontScaleMain），写死的 52 / 72 就会把「执行」「召唤」
// 这类中文标签裁掉，看起来像字偏右、显示不全。
static float button_width(const char* label, float slack = 6.f) {
  // ImGui 只画 "##" 之前的可见部分（"−##m" 这类），宽度也按那部分量。
  std::string shown(label ? label : "");
  const size_t hash = shown.find("##");
  if (hash != std::string::npos) shown.resize(hash);
  const float text = ImGui::CalcTextSize(shown.c_str()).x;
  return text + ImGui::GetStyle().FramePadding.x * 2.f + ui_px(slack);
}
bool g_unit_inspect_open = true;
bool g_show_debug_status = false;  // 顶部调试状态，默认关闭，避免长日志撑开窗口
char g_ui_settings_path[MAX_PATH] = {};
bool g_settings_dirty = false;
float g_settings_save_timer = 0.f;

// ---------------------------------------------------------------------------
// P1 语义取色层。
// 界面代码里不再出现任何字面色值：需要颜色时从这里取，最终都落到皮肤槽位。
// 这样换皮肤只改 skin.cpp 一张表，ui.cpp 一行都不用动。
// ---------------------------------------------------------------------------
inline const skin::Skin& SK() { return skin::Active(); }
inline ImVec4 CV(ImU32 c) { return skin::Vec4(c); }
inline ImVec4 CVA(ImU32 c, float mul) { return skin::Vec4A(c, mul); }

// 实心底：把（通常半透明的）卡片色叠在窗底上，得到比窗口高一档的面。
// 具体合成规则在 skin.cpp，这里只是绑定到"当前皮肤"的薄封装。
inline ImU32 PanelBg() { return skin::PanelBg(SK()); }
inline ImU32 PanelBgAlt() { return skin::PanelBgAlt(SK()); }
// 凹槽 / 输入框 / 滑条轨道底。
inline ImU32 FieldBg() { return skin::FieldBg(SK()); }
// 角标、快捷键小块的底：比面再压暗一层。
inline ImU32 ChipBg() { return skin::ChipBg(SK()); }

// 标题层级
inline ImU32 HeadingCol() { return SK().accent2; }
inline ImU32 SubHeadCol() { return SK().text2; }

// 描边
inline ImU32 EdgeIdle() { return skin::EdgeIdle(SK()); }
inline ImU32 EdgeHot() { return skin::EdgeHot(SK()); }

// 形状语言：圆角矩形 or 切角。所有自绘控件都走这两个取值，不写字面圆角。
inline paint::BoxShape ShapeOf(const skin::Skin& s) { return s.shape; }
inline float ChamferOf(const skin::Skin& s) { return ui_px(s.chamfer); }
inline float CardR(const skin::Skin& s) { return ui_px(s.cardRadius); }

// 悬停 / 按下的过渡量。
//
// ImGui 没有给自绘控件留动画状态，这里用控件 ID 存一个 0..1 的时间量：
// 悬停时控件是"渐亮"而不是"硬跳"，切换皮肤时也是同一条错觉链的一部分。
// 键是 ImGuiID（含窗口与 ID 栈），不会串到别的控件上。
float AnimTo(ImGuiID id, bool target, float speed = 14.f);

// 一块控件底板。皮肤的形状、描边宽度、投影强度都从这里进，
// 别处只管给颜色，不碰几何。
struct PlateSpec {
  ImU32 top = 0;
  ImU32 bot = 0;
  ImU32 edge = 0;
  ImU32 inner = 0;
  ImU32 shadow = 0;
  float dy = 2.f;
  float sigma = 3.f;
  float sk = 1.f;
  float th = 1.f;
};

inline void DrawPlate(ImDrawList* dl, const ImVec2& a, const ImVec2& b, float r,
                      const PlateSpec& sp) {
  const skin::Skin& s = SK();
  paint::Plate pl;
  pl.top = sp.top;
  pl.bot = sp.bot;
  pl.edge = sp.edge;
  pl.inner = sp.inner;
  pl.shadow = sp.shadow;
  pl.dy = sp.dy;
  pl.sigma = sp.sigma;
  pl.sk = sp.sk * s.shadowK;
  pl.th = sp.th * s.strokeW;
  paint::Panel(dl, a, b, r, ShapeOf(s), ChamferOf(s), pl);
}

// 小角标 / 小色块：不做投影（一屏几十个，投影会白烧 4 层多边形），
// 只填 + 描边，但形状仍然跟皮肤走。
inline void DrawChip(ImDrawList* dl, const ImVec2& a, const ImVec2& b, float r, ImU32 top,
                     ImU32 bot, ImU32 edge, float th = 1.f) {
  PlateSpec sp;
  sp.top = top;
  sp.bot = bot;
  sp.edge = edge;
  sp.th = th;
  DrawPlate(dl, a, b, r, sp);
}

// 过渡量实现。用帧号做惰性清理：一个控件不再出现后，几百帧后自动淘汰，
// 不会随开关 UI 页反复累积。
struct AnimEntry {
  float v = 0.f;
  int frame = 0;
};

float AnimTo(ImGuiID id, bool target, float speed) {
  static std::unordered_map<ImGuiID, AnimEntry> tbl;
  static int frame = 0;
  static int last_gc = 0;
  ++frame;
  if (frame - last_gc > 600) {
    last_gc = frame;
    for (auto it = tbl.begin(); it != tbl.end();) {
      if (frame - it->second.frame > 600) {
        it = tbl.erase(it);
      } else {
        ++it;
      }
    }
  }
  AnimEntry& e = tbl[id];
  e.frame = frame;
  const float dest = target ? 1.f : 0.f;
  const float dt = ImGui::GetIO().DeltaTime;
  const float k = 1.f - std::exp(-speed * (dt > 0.f ? dt : 0.016f));
  e.v += (dest - e.v) * k;
  if (e.v < 0.002f) e.v = 0.f;
  if (e.v > 0.998f) e.v = 1.f;
  return e.v;
}

// ---------------------------------------------------------------------------
// 杂项 / 真无敌 / 空投的持久化设置（写进 overlay_ui.ini）。
static int g_misc_shadow_size = 0;     // 0 = 没设置过
static float g_misc_shadow_dist = 0.f; // 0 = 没设置过
static bool g_misc_hide_hp = false;
static bool g_misc_hide_sil = false;
static bool g_misc_take = false;
static bool g_misc_shadow_take = false;
static bool g_misc_light_lock = true;
static int g_crate_kind_persist = 0;
static int g_crate_count_persist = 1;
static bool g_misc_restore_pending = false;

struct AsyncResult {
  bool done = false;
  bool ok = false;
  std::string msg;
};
AsyncResult g_async_result;
CRITICAL_SECTION g_async_cs;
bool g_async_cs_ready = false;

DWORD WINAPI inject_worker(LPVOID param) {
  const bool spectate = param != nullptr;
  bool ok = game_api::inject_full(spectate);
  std::string msg =
      ok ? u8"注入成功，可以使用修改功能。"
         : (std::string(u8"注入失败：") + game_api::status_text());
  game_api::beep(ok ? "on" : "error");
  if (g_async_cs_ready) EnterCriticalSection(&g_async_cs);
  g_async_result.ok = ok;
  g_async_result.msg = msg;
  g_async_result.done = true;
  if (g_async_cs_ready) LeaveCriticalSection(&g_async_cs);
  InterlockedExchange(&g_injecting, 0);
  return 0;
}

struct EngineJob {
  char key[64];
};

DWORD WINAPI engine_worker(LPVOID param) {
  auto* job = static_cast<EngineJob*>(param);
  std::string msg;
  bool ok = false;
  if (job && job->key[0]) {
    // Clone/spawn needs cursor on terrain; hide menu briefly so GetMouseXyz works.
    const bool need_terrain =
        std::strcmp(job->key, "unit_clone") == 0 ||
        std::strcmp(job->key, "spawn_unit") == 0 ||
        std::strcmp(job->key, "clone_multi") == 0 ||
        std::strcmp(job->key, "ore_convoy") == 0 ||
        std::strcmp(job->key, "spawn_mcv") == 0 ||
        std::strcmp(job->key, "summon_troops") == 0 ||
        std::strcmp(job->key, "airdrop") == 0;
    if (need_terrain) {
      input::set_menu_visible(false);
      Sleep(280);
    }
    ok = game_api::run_engine(job->key, &msg);
  } else {
    msg = u8"内部错误：空功能键";
  }
  game_api::beep(ok ? "click" : "error");
  if (g_async_cs_ready) EnterCriticalSection(&g_async_cs);
  g_async_result.ok = ok;
  g_async_result.msg = msg;
  g_async_result.done = true;
  if (g_async_cs_ready) LeaveCriticalSection(&g_async_cs);
  delete job;
  InterlockedExchange(&g_engine_busy, 0);
  return 0;
}

bool start_engine_async(const char* key) {
  if (!key || !key[0]) return false;
  if (InterlockedCompareExchange(&g_engine_busy, 1, 0) != 0) return false;
  auto* job = new EngineJob{};
  std::snprintf(job->key, sizeof(job->key), "%s", key);
  if (g_async_cs_ready) EnterCriticalSection(&g_async_cs);
  g_async_result.done = false;
  if (g_async_cs_ready) LeaveCriticalSection(&g_async_cs);
  HANDLE th = CreateThread(nullptr, 0, engine_worker, job, 0, nullptr);
  if (!th) {
    delete job;
    InterlockedExchange(&g_engine_busy, 0);
    return false;
  }
  CloseHandle(th);
  return true;
}

// 这个目录能不能落盘。DLL 被放在 Program Files 之类的只读目录时，
// ofstream 会静默失败，表现就是"切了皮肤/改了透明度，重启又变回去"。
static bool path_writable(const wchar_t* file) {
  if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
    return _waccess(file, 2) == 0;  // 2 = W_OK
  }
  // 文件还不存在：在目录里建一个 DELETE_ON_CLOSE 的探针文件试一下，
  // 不留垃圾，也不会在用户没改设置时凭空造出一个空 ini。
  wchar_t dir[MAX_PATH] = {};
  wcsncpy_s(dir, file, _TRUNCATE);
  wchar_t* slash = wcsrchr(dir, L'\\');
  if (!slash) return false;
  *slash = 0;
  wchar_t probe[MAX_PATH] = {};
  _snwprintf_s(probe, _TRUNCATE, L"%s\\.ra3_ui_probe", dir);
  HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

bool resolve_ui_settings_path(char* out, size_t out_len) {
  if (!out || out_len < 8) return false;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&resolve_ui_settings_path),
                          &self) ||
      !self) {
    return false;
  }
  wchar_t wpath[MAX_PATH] = {};
  if (!GetModuleFileNameW(self, wpath, MAX_PATH)) return false;
  wchar_t* slash = wcsrchr(wpath, L'\\');
  if (!slash) return false;
  *slash = 0;
  wchar_t beside[MAX_PATH] = {};
  _snwprintf_s(beside, _TRUNCATE, L"%s\\overlay_ui.ini", wpath);
  // Older builds wrote the ini three levels above the DLL (the repo root).
  // Bring that file next to the DLL once, so a copied bin folder keeps working.
  if (GetFileAttributesW(beside) == INVALID_FILE_ATTRIBUTES) {
    wchar_t legacy_dir[MAX_PATH] = {};
    wcsncpy_s(legacy_dir, wpath, _TRUNCATE);
    bool have_legacy = true;
    for (int i = 0; i < 2; ++i) {
      wchar_t* up = wcsrchr(legacy_dir, L'\\');
      if (!up) {
        have_legacy = false;
        break;
      }
      *up = 0;
    }
    if (have_legacy) {
      wchar_t legacy[MAX_PATH] = {};
      _snwprintf_s(legacy, _TRUNCATE, L"%s\\overlay_ui.ini", legacy_dir);
      if (GetFileAttributesW(legacy) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(legacy, beside, TRUE);
    }
  }
  // 紧挨 DLL 写不进去（只读安装目录）时退到 %LOCALAPPDATA%\RA3Overlay。
  // 两者都存在时优先读 DLL 旁边，保持"绿色版"习惯。
  if (!path_writable(beside)) {
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      wchar_t dir[MAX_PATH] = {};
      _snwprintf_s(dir, _TRUNCATE, L"%s\\RA3Overlay", base);
      CreateDirectoryW(dir, nullptr);
      wchar_t alt[MAX_PATH] = {};
      _snwprintf_s(alt, _TRUNCATE, L"%s\\overlay_ui.ini", dir);
      // 首次回退时把旧位置的文件带过去，用户感知不到换了地方。
      if (GetFileAttributesW(alt) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(beside) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(beside, alt, FALSE);
      }
      wcsncpy_s(beside, alt, _TRUNCATE);
    }
  }
  return WideCharToMultiByte(CP_UTF8, 0, beside, -1, out, (int)out_len, nullptr,
                             nullptr) > 0;
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// g_ui_settings_path 是 UTF-8（WideCharToMultiByte(CP_UTF8) 出来的）。Windows 上
// std::fstream 的窄字符构造函数按 ANSI 代码页解析路径，装在中文目录（或用户
// 目录带中文）里会直接打开失败——设置就永远写不进去。统一转宽字符再开。
static std::wstring utf8_to_wide(const char* s) {
  if (!s || !*s) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
  w.resize(static_cast<size_t>(n - 1));  // 去掉结尾的 '\0'
  return w;
}

void load_ui_settings() {
  if (!g_ui_settings_path[0]) {
    resolve_ui_settings_path(g_ui_settings_path, sizeof(g_ui_settings_path));
  }
  if (!g_ui_settings_path[0]) return;
  std::ifstream f(utf8_to_wide(g_ui_settings_path).c_str());
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string val = line.substr(eq + 1);
    if (key == "scale") {
      g_ui_scale = clampf((float)std::atof(val.c_str()), kUiScaleMin, kUiScaleMax);
    } else if (key == "opacity") {
      g_ui_opacity = clampf((float)std::atof(val.c_str()), 0.35f, 1.00f);
    } else if (key == "stats_icon_scale") {
      g_stats_icon_scale = clampf((float)std::atof(val.c_str()), kStatsIconScaleMin,
                                  kStatsIconScaleMax);
    } else if (key == "theme") {
      const int idx = skin::IndexOfKey(val.c_str());
      if (idx >= 0) skin::SetCurrent(idx);
    } else if (key == "unit_inspect_open") {
      g_unit_inspect_open = (val == "1" || val == "true" || val == "yes");
    } else if (key == "stats_pinned") {
      // 固定是可选项，启动时一律不固定。
      g_stats_pinned = false;
    } else if (key == "stats_compact") {
      g_stats_compact = (val == "1" || val == "true" || val == "yes");
    } else if (key == "stats_minimal") {
      g_stats_minimal = (val == "1" || val == "true" || val == "yes");
      // 两档互斥：老配置文件里两个都为 1 时，以极简为准。
      if (g_stats_minimal) g_stats_compact = false;
    } else if (key == "money_self_step") {
      game_api::set_money_self_step((int)std::atoi(val.c_str()));
    } else if (key == "money_sel_step") {
      game_api::set_money_sel_step((int)std::atoi(val.c_str()));
    } else if (key == "mcv_faction") {
      game_api::set_mcv_faction((int)std::atoi(val.c_str()));
    } else if (key == "misc.shadow_size") {
      g_misc_shadow_size = (int)std::atoi(val.c_str());
      g_misc_restore_pending = true;
    } else if (key == "misc.shadow_dist") {
      g_misc_shadow_dist = (float)std::atof(val.c_str());
      g_misc_restore_pending = true;
    } else if (key == "misc.hide_hp") {
      g_misc_hide_hp = (val == "1" || val == "true" || val == "yes");
      g_misc_restore_pending = true;
    } else if (key == "misc.hide_sil") {
      g_misc_hide_sil = (val == "1" || val == "true" || val == "yes");
      g_misc_restore_pending = true;
    } else if (key == "misc.take") {
      g_misc_take = (val == "1" || val == "true" || val == "yes");
      g_misc_restore_pending = true;
    } else if (key == "misc.shadow_take") {
      g_misc_shadow_take = (val == "1" || val == "true" || val == "yes");
      g_misc_restore_pending = true;
    } else if (key == "misc.light_lock") {
      g_misc_light_lock = (val == "1" || val == "true" || val == "yes");
      g_misc_restore_pending = true;
    } else if (key == "crate.kind") {
      g_crate_kind_persist = (int)std::atoi(val.c_str());
    } else if (key == "crate.count") {
      g_crate_count_persist = (int)std::atoi(val.c_str());
      if (g_crate_count_persist < 1) g_crate_count_persist = 1;
      if (g_crate_count_persist > 20) g_crate_count_persist = 20;
    } else if (key.compare(0, 3, "hk.") == 0) {
      game_api::apply_hotkey_text(key.c_str() + 3, val.c_str());
    }
  }
  // 皮肤可能比保存时的更亮，恢复时把透明度抬到该皮肤的可读下限。
  g_ui_opacity = clampf(g_ui_opacity, skin::MinOpacity(skin::Current()), 1.00f);
}

void save_ui_settings() {
  if (!g_ui_settings_path[0]) {
    resolve_ui_settings_path(g_ui_settings_path, sizeof(g_ui_settings_path));
  }
  if (!g_ui_settings_path[0]) return;
  // 先写 .tmp 再原子替换。切完主题立刻退游戏、或进程被强行结束时，
  // 直接 trunc 原文件容易留下半截 ini（读回来 theme= 只剩半行）。
  char tmp_path[MAX_PATH] = {};
  _snprintf_s(tmp_path, _TRUNCATE, "%s.tmp", g_ui_settings_path);
  const std::wstring wtmp = utf8_to_wide(tmp_path);
  const std::wstring wdst = utf8_to_wide(g_ui_settings_path);
  std::ofstream f(wtmp.c_str(), std::ios::trunc);
  if (!f) return;
  f << "# RA3 Overlay UI settings\n";
  f << "scale=" << g_ui_scale << "\n";
  f << "opacity=" << g_ui_opacity << "\n";
  f << "stats_icon_scale=" << g_stats_icon_scale << "\n";
  f << "theme=" << skin::KeyAt(skin::Current()) << "\n";
  f << "unit_inspect_open=" << (g_unit_inspect_open ? 1 : 0) << "\n";
  f << "stats_pinned=" << (g_stats_pinned ? 1 : 0) << "\n";
  f << "stats_compact=" << (g_stats_compact ? 1 : 0) << "\n";
  f << "stats_minimal=" << (g_stats_minimal ? 1 : 0) << "\n";
  f << "money_self_step=" << game_api::money_self_step() << "\n";
  f << "money_sel_step=" << game_api::money_sel_step() << "\n";
  f << "mcv_faction=" << game_api::mcv_faction() << "\n";
  f << "misc.shadow_size=" << g_misc_shadow_size << "\n";
  f << "misc.shadow_dist=" << g_misc_shadow_dist << "\n";
  f << "misc.hide_hp=" << (g_misc_hide_hp ? 1 : 0) << "\n";
  f << "misc.hide_sil=" << (g_misc_hide_sil ? 1 : 0) << "\n";
  f << "misc.take=" << (g_misc_take ? 1 : 0) << "\n";
  f << "misc.shadow_take=" << (g_misc_shadow_take ? 1 : 0) << "\n";
  f << "misc.light_lock=" << (g_misc_light_lock ? 1 : 0) << "\n";
  f << "crate.kind=" << g_crate_kind_persist << "\n";
  f << "crate.count=" << g_crate_count_persist << "\n";
  const int hk_n = game_api::hotkey_slot_count();
  for (int i = 0; i < hk_n; ++i) {
    char combo[48] = {};
    game_api::format_hotkey_slot(i, combo, sizeof(combo));
    if (!combo[0]) continue;
    f << "hk." << game_api::hotkey_slot_id(i) << "=" << combo << "\n";
  }
  f.close();
  if (!MoveFileExW(wtmp.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(wtmp.c_str());
  }
  g_settings_dirty = false;
  g_settings_save_timer = 0.f;
}

void apply_ra3_theme() {
  // 颜色与圆角全部来自当前皮肤（skin.cpp 的语义槽位表）。BuildStyle 每次
  // 都从 ImGuiStyle() 默认值重建，只在末尾缩放一次，所以拖动缩放滑块不会
  // 在脏 style 上反复累乘、把窗口宽度顶飞。
  ImGuiStyle& s = ImGui::GetStyle();
  skin::BuildStyle(s, skin::Active(), g_ui_opacity);

  if (g_ui_scale > 0.01f && g_ui_scale != 1.0f) {
    s.ScaleAllSizes(g_ui_scale);
  }
  // 字体放大：ImGui 1.92+ 的字体是按"最终像素尺寸"动态栅格化的，把倍数交给
  // FontScaleMain，字形会以 18 * scale 重新生成。3 倍下文字依然锐利。
  // 旧的 io.FontGlobalScale 是 1.92 之前的遗留通道，只做位图拉伸，放大就糊。
  s.FontScaleMain = (g_ui_scale > 0.01f) ? g_ui_scale : 1.f;
  ImGui::GetIO().FontGlobalScale = 1.f;
}

void mark_settings_dirty() {
  g_settings_dirty = true;
  g_settings_save_timer = 0.6f;
  apply_ra3_theme();
}

// 窗口默认 / 最小尺寸：跟着界面缩放走，并夹到当前分辨率内。
// 3 倍缩放时 900x720 会变成 2700x2160，比屏幕还大，标题栏会被推到画面外、
// 拖都拖不回来。所以这里把尺寸收到屏幕内（留一圈边距），内容超出时窗口滚动。
static ImVec2 scaled_window_size(float w, float h) {
  ImGuiIO& io = ImGui::GetIO();
  const float margin = ui_px(56.f);
  float sw = ui_px(w);
  float sh = ui_px(h);
  if (io.DisplaySize.x > 1.f) {
    const float cap = (std::max)(ui_px(360.f), io.DisplaySize.x - margin);
    if (sw > cap) sw = cap;
  }
  if (io.DisplaySize.y > 1.f) {
    const float cap = (std::max)(ui_px(280.f), io.DisplaySize.y - margin);
    if (sh > cap) sh = cap;
  }
  return ImVec2(sw, sh);
}

// ---------------------------------------------------------------------------
// 皮肤选择：卡片绘制在 skin_gallery.cpp 里，和离线预览工具共用同一份代码。
// 点一下立刻切换（带 0.26 秒过渡），并写回 overlay_ui.ini。
// ---------------------------------------------------------------------------
static void draw_theme_gallery() {
  const int pick = skin::Gallery(skin::Active(), ui_px(1.f));
  if (pick < 0 || pick == skin::Current()) return;
  skin::Select(pick);
  if (g_ui_opacity < skin::MinOpacity(pick)) g_ui_opacity = skin::MinOpacity(pick);
  mark_settings_dirty();
  // 主题是"点一下就该记住"的设置：立刻落盘，不等 0.6 秒的批量保存定时器，
  // 否则切完马上 Esc 关菜单 / 退游戏，这次选择会丢。
  save_ui_settings();
  g_last_msg = std::string(u8"皮肤已切换：") + skin::At(pick).name_cn;
}

// 分区标题：小号大写英文 + 中文标题 + 一条渐隐分割线。
// 设置页里裸 ImGui 文字和自绘控件混在一起会显脏，这里统一一下。
static void section_header(const char* caps, const char* cn) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  ImGui::Dummy(ImVec2(0.f, ui_px(26.f)));
  const ImVec2 p = ImGui::GetItemRectMin();
  paint::TextTracked(dl, font, ui_px(10.f), ImVec2(p.x, p.y + ui_px(2.f)),
                     skin::Fade(SK().text3, 0.95f), caps, ui_px(1.f));
  paint::Text(dl, font, ui_px(15.f), ImVec2(p.x, p.y + ui_px(15.f)), SK().accent2, cn);
  const float w = ImGui::GetContentRegionAvail().x;
  const float y = p.y + ui_px(37.f);
  dl->AddRectFilledMultiColor(ImVec2(p.x, y), ImVec2(p.x + w, y + 1.f),
                              skin::Fade(SK().divider, 0.9f), skin::WithA(SK().divider, 0), 0,
                              skin::Fade(SK().divider, 0.9f));
  ImGui::SetCursorScreenPos(ImVec2(p.x, y + ui_px(8.f)));
}

void draw_settings_panel() {
  const skin::Skin& sk = skin::Active();
  ImGui::TextColored(skin::Vec4(sk.accent2), u8"界面设置");
  ImGui::TextDisabled(u8"界面和快捷键自动保存到和 DLL 同一目录的 overlay_ui.ini");
  ImGui::Separator();
  ImGui::Spacing();

  section_header("THEME", u8"皮肤");
  ImGui::TextDisabled(u8"共 %d 套；点选即时生效（0.26 秒过渡）并自动保存", skin::Count());
  ImGui::Spacing();
  draw_theme_gallery();
  ImGui::Spacing();

  section_header("DISPLAY", u8"显示");
  float scale = g_ui_scale;
  ImGui::TextUnformatted(u8"界面缩放");
  ImGui::SetNextItemWidth(-1.f);
  if (ImGui::SliderFloat("##ui_scale", &scale, kUiScaleMin, kUiScaleMax, "%.2f")) {
    g_ui_scale = clampf(scale, kUiScaleMin, kUiScaleMax);
    mark_settings_dirty();
  }
  ImGui::TextDisabled(u8"同时影响字体与控件尺寸（默认 1.00，上限 %.1f）", kUiScaleMax);

  ImGui::Spacing();
  ImGui::TextUnformatted(u8"透明度");
  const float op_min = skin::MinOpacity(skin::Current());
  float opacity_pct = g_ui_opacity * 100.f;
  ImGui::SetNextItemWidth(-1.f);
  if (ImGui::SliderFloat("##ui_opacity", &opacity_pct, op_min * 100.f, 100.f, "%.0f%%")) {
    g_ui_opacity = clampf(opacity_pct / 100.f, op_min, 1.00f);
    mark_settings_dirty();
  }
  ImGui::TextDisabled(u8"窗口背景不透明度（默认 %d%%；亮色皮肤下限 %d%%）",
                      (int)(kUiOpacityDefault * 100.f + 0.5f), (int)(op_min * 100.f + 0.5f));

  ImGui::Spacing();
  ImGui::TextUnformatted(u8"战况统计图标大小");
  float icon_scale = g_stats_icon_scale;
  ImGui::SetNextItemWidth(-1.f);
  if (ImGui::SliderFloat("##stats_icon_scale", &icon_scale, kStatsIconScaleMin,
                         kStatsIconScaleMax, "%.2fx")) {
    g_stats_icon_scale = clampf(icon_scale, kStatsIconScaleMin, kStatsIconScaleMax);
    mark_settings_dirty();
  }
  ImGui::TextDisabled(u8"兵种 / 建筑图标的放大倍数（默认 %.2fx）", kStatsIconScaleDefault);

  ImGui::Spacing();
  section_header("RESET", u8"重置");
  if (ImGui::Button(u8"恢复默认", ImVec2(ui_px(140.f), 0))) {
    g_ui_scale = 1.00f;
    g_ui_opacity = kUiOpacityDefault;
    g_stats_icon_scale = kStatsIconScaleDefault;
    g_unit_inspect_open = true;
    skin::Select(0);
    mark_settings_dirty();
    save_ui_settings();
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"立即保存", ImVec2(ui_px(140.f), 0))) {
    save_ui_settings();
    g_last_msg = u8"界面设置已保存";
  }
  if (g_ui_settings_path[0]) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", g_ui_settings_path);
  }
}

void format_uint_grouped(char* out, size_t out_len, uint32_t v) {
  if (!out || out_len < 2) return;
  char raw[16];
  std::snprintf(raw, sizeof(raw), "%u", v);
  const int n = (int)std::strlen(raw);
  int o = 0;
  for (int i = 0; i < n && o + 1 < (int)out_len; ++i) {
    const int left = n - i;
    if (i > 0 && left % 3 == 0) out[o++] = ',';
    out[o++] = raw[i];
  }
  out[o] = 0;
}

struct TrendPt {
  float t = 0.f;
  // 0..2 units built/lost/destroyed, 3..5 buildings, 6 earned, 7 spent
  uint32_t v[8] = {};
};
struct TrendTrack {
  uint32_t player = 0;
  // True when the first span is not sampled: t=0 is all zeros, and the next
  // point is the official running total at the moment we started watching.
  bool bridged = false;
  std::vector<TrendPt> pts;
};
static std::vector<TrendTrack> g_trends;
static float g_trend_clock = -1.f;

static void reset_trends() {
  g_trends.clear();
  g_trend_clock = -1.f;
}

static void sample_trends(const game_api::MatchEconomy& st) {
  if (!st.valid || st.player_count <= 0) {
    reset_trends();
    return;
  }
  float t = st.match_seconds;
  const bool game_clock = t >= 0.f && t < 6.f * 3600.f;
  if (!game_clock) {
    const float now = (float)ImGui::GetTime();
    if (g_trend_clock < 0.f) g_trend_clock = now;
    t = now - g_trend_clock;
  } else if (g_trend_clock >= 0.f && t + 2.f < g_trend_clock) {
    reset_trends();
  }
  if (game_clock) g_trend_clock = t;

  for (int i = (int)g_trends.size() - 1; i >= 0; --i) {
    bool keep = false;
    for (int p = 0; p < st.player_count; ++p) {
      if (st.players[p].player == g_trends[i].player) {
        keep = true;
        break;
      }
    }
    if (!keep) g_trends.erase(g_trends.begin() + i);
  }

  for (int pi = 0; pi < st.player_count; ++pi) {
    const auto& pe = st.players[pi];
    if (!pe.has_score || !pe.player) continue;
    TrendTrack* tr = nullptr;
    for (auto& c : g_trends) {
      if (c.player == pe.player) {
        tr = &c;
        break;
      }
    }
    if (!tr) {
      g_trends.push_back(TrendTrack{});
      tr = &g_trends.back();
      tr->player = pe.player;
    }
    const uint32_t v[8] = {pe.units_built,     pe.units_lost,        pe.units_destroyed,
                           pe.buildings_built, pe.buildings_lost,    pe.buildings_destroyed,
                           pe.money_earned,    pe.money_spent};
    // ScoreKeeper only stores match totals, not a per-second history. A loaded
    // battle already has those totals, so anchor the chart at 0:00 and let the
    // first segment run from zero up to the official numbers.
    if (tr->pts.empty() && t > 1.f) {
      TrendPt origin;
      origin.t = 0.f;
      tr->pts.push_back(origin);
      tr->bridged = true;
    }
    if (!tr->pts.empty()) {
      TrendPt& last = tr->pts.back();
      bool same = true;
      for (int k = 0; k < 8; ++k) {
        if (last.v[k] != v[k]) same = false;
      }
      if (same && t <= last.t + 1.f) continue;
      if (!same && t <= last.t + 0.25f) {
        for (int k = 0; k < 8; ++k) last.v[k] = v[k];
        last.t = t;
        continue;
      }
    }
    TrendPt pt;
    pt.t = t;
    for (int k = 0; k < 8; ++k) pt.v[k] = v[k];
    tr->pts.push_back(pt);
    if (tr->pts.size() > 1800) {
      std::vector<TrendPt> kept;
      kept.reserve(tr->pts.size() / 2 + 2);
      kept.push_back(tr->pts.front());
      for (size_t i = 1; i + 1 < tr->pts.size(); i += 2) kept.push_back(tr->pts[i]);
      kept.push_back(tr->pts.back());
      tr->pts.swap(kept);
    }
  }
}

static const TrendTrack* trend_for(uint32_t player) {
  for (const auto& c : g_trends) {
    if (c.player == player) return &c;
  }
  return nullptr;
}

static uint32_t trend_total(const TrendPt& pt, int series) {
  if (series < 3) return pt.v[series] + pt.v[series + 3];
  return pt.v[series + 3];  // 3 → earned, 4 → spent
}

static void draw_trend_legend(const char* const* names, const ImU32* cols, int count) {
  for (int s = 0; s < count; ++s) {
    if (s) ImGui::SameLine(0.f, 12.f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 3.f), ImVec2(p.x + 10.f, p.y + h - 2.f),
                                              cols[s], 2.f);
    ImGui::Dummy(ImVec2(10.f, h));
    ImGui::SameLine(0.f, 4.f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cols[s]), "%s", names[s]);
  }
}

static int trend_hover_index(int count) {
  if (count <= 0 || !ImGui::IsItemHovered()) return -1;
  if (count == 1) return 0;
  const ImVec2 a = ImGui::GetItemRectMin();
  const ImVec2 b = ImGui::GetItemRectMax();
  const ImVec2 pad = ImGui::GetStyle().FramePadding;
  const float inner_w = std::max(1.f, (b.x - a.x) - pad.x * 2.f);
  float t = (ImGui::GetIO().MousePos.x - (a.x + pad.x)) / inner_w;
  if (t < 0.f) t = 0.f;
  if (t > 0.9999f) t = 0.9999f;
  int idx = (int)(t * (float)(count - 1));
  if (idx < 0) idx = 0;
  if (idx >= count) idx = count - 1;
  return idx;
}

static void draw_trend_chart(const char* title, const TrendTrack* tr) {
  const char* names[5] = {u8"建造", u8"损失", u8"消灭", u8"收入", u8"支出"};
  const ImU32 cols[5] = {
      SK().good, SK().bad, SK().warn, SK().accent2, skin::Fade(SK().text2, 0.95f),
  };

  ImGui::TextColored(CV(SubHeadCol()), "%s", title);
  ImGui::SameLine(0.f, 14.f);
  draw_trend_legend(names, cols, 3);
  draw_trend_legend(names + 3, cols + 3, 2);

  const int src_n = tr ? (int)tr->pts.size() : 0;
  const int n = src_n == 1 ? 2 : src_n;
  std::vector<float> xs((size_t)n);
  std::vector<float> series[5];
  for (int s = 0; s < 5; ++s) series[s].assign((size_t)n, 0.f);
  float raw[5] = {};
  float count_max = 1.f;
  float money_max = 1.f;
  for (int i = 0; i < n; ++i) {
    const TrendPt& pt = tr->pts[src_n == 1 ? 0 : i];
    xs[(size_t)i] = (src_n == 1 && i == 1) ? pt.t + 1.f : pt.t;
    for (int s = 0; s < 5; ++s) {
      raw[s] = (float)trend_total(pt, s);
      series[s][(size_t)i] = raw[s];
    }
    count_max = std::max(count_max, std::max(raw[0], std::max(raw[1], raw[2])));
    money_max = std::max(money_max, std::max(raw[3], raw[4]));
  }
  if (count_max < 4.f) count_max = 4.f;
  if (money_max < 4.f) money_max = 4.f;
  // One axis cannot show counts and money together, so each group fills the
  // chart against its own peak. The tooltip still shows the real numbers.
  for (int i = 0; i < n; ++i) {
    for (int s = 0; s < 3; ++s) series[s][(size_t)i] /= count_max;
    for (int s = 3; s < 5; ++s) series[s][(size_t)i] /= money_max;
  }
  const float* ys[5] = {series[0].data(), series[1].data(), series[2].data(), series[3].data(),
                        series[4].data()};

  ImGui::PlotConfig conf;
  conf.values.xs = n > 0 ? xs.data() : nullptr;
  conf.values.ys_list = n > 0 ? ys : nullptr;
  conf.values.ys_count = 5;
  conf.values.count = n;
  conf.values.colors = cols;
  conf.scale.min = 0.f;
  conf.scale.max = 1.f;
  conf.tooltip.show = false;
  conf.grid_y.show = true;
  conf.grid_y.size = 0.25f;
  conf.grid_y.subticks = 1;
  conf.grid_x.show = false;
  conf.frame_size = ImVec2(std::max(8.f, ImGui::GetContentRegionAvail().x), 118.f);
  conf.line_thickness = 2.f;
  conf.skip_small_lines = true;
  ImGui::Plot("##trend", conf);

  const int idx = trend_hover_index(src_n);
  if (idx < 0 || !tr) return;
  const TrendPt& pt = tr->pts[idx];
  int sec = (int)(pt.t + 0.5f);
  if (sec < 0) sec = 0;
  TipBegin();
  ImGui::Text("%d:%02d", sec / 60, sec % 60);
  for (int s = 0; s < 3; ++s) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cols[s]), u8"%s  %u", names[s],
                       trend_total(pt, s));
    ImGui::TextDisabled(u8"    部队 %u    建筑 %u", pt.v[s], pt.v[s + 3]);
  }
  for (int s = 3; s < 5; ++s) {
    char num[32];
    format_uint_grouped(num, sizeof(num), trend_total(pt, s));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cols[s]), u8"%s  %s", names[s], num);
  }
  TipEnd();
}

void draw_player_econ_card(const game_api::PlayerEconomy& p, int index) {
  // 玩家色来自游戏数据，保留；未着色时回落到皮肤语义色。
  ImVec4 accent(p.color_r / 255.f, p.color_g / 255.f, p.color_b / 255.f, 1.f);
  if (!p.has_color) {
    accent = p.is_local ? CV(SK().money) : CV(SK().text2);
  }
  const ImVec4 panel = CV(PanelBg());
  const ImVec4 money_col = CV(SK().money);

  ImGui::PushID((int)p.player_id ? (int)p.player_id : index);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.55f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 10.f));
  ImGui::BeginChild("##econ_card", ImVec2(-1.f, 0.f),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

  // Accent strip + title row
  const ImVec2 row0 = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(row0.x - 8.f, row0.y - 6.f),
                    ImVec2(row0.x - 4.f, row0.y + ImGui::GetTextLineHeight() + 4.f),
                    ImGui::GetColorU32(accent), 2.f);

  const char* title = p.name[0] ? p.name : u8"玩家";
  const float sw = ImGui::GetTextLineHeight();
  const ImVec2 sw0 = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(sw0, ImVec2(sw0.x + sw, sw0.y + sw), ImGui::GetColorU32(accent), 3.f);
  dl->AddRect(sw0, ImVec2(sw0.x + sw, sw0.y + sw), skin::Fade(SK().cardInnerHi, 0.28f), 3.f, 0, 1.f);
  ImGui::Dummy(ImVec2(sw, sw));
  ImGui::SameLine(0.f, 8.f);
  ImGui::TextColored(CV(SK().text), "%s", title);
  if (p.is_local) {
    ImGui::SameLine();
    ImGui::TextDisabled(u8"己方");
  }
  if (p.defeated) {
    ImGui::SameLine();
    ImGui::TextDisabled(u8"已击败");
  }

  ImGui::Spacing();

  // Money
  ImGui::BeginGroup();
  ImGui::TextDisabled(u8"资金");
  if (p.has_money) {
    char money_s[32];
    format_uint_grouped(money_s, sizeof(money_s), p.money);
    ImGui::PushStyleColor(ImGuiCol_Text, money_col);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted(money_s);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("—");
  }
  ImGui::EndGroup();

  ImGui::SameLine(0.f, 28.f);

  // Power
  ImGui::BeginGroup();
  ImGui::TextDisabled(u8"电力");
  if (p.has_power) {
    char used_s[24], total_s[24], ratio_s[48];
    format_uint_grouped(used_s, sizeof(used_s), p.power_used);
    format_uint_grouped(total_s, sizeof(total_s), p.power_total);
    std::snprintf(ratio_s, sizeof(ratio_s), "%s / %s", used_s, total_s);

    // 使用 > 总量（含 0 产能但有消耗）一律红；数值和进度条同一个判定。
    const skin::PowerLevel lv = skin::PowerLevelOf(p.power_used, p.power_total);
    float frac = (p.power_total > 0) ? (float)p.power_used / (float)p.power_total : 1.f;
    if (frac > 1.f) frac = 1.f;
    const ImVec4 bar = CV(skin::PowerColor(SK(), lv));
    const ImVec4 ratio_col = (lv == skin::PowerLevel::Deficit)
                                 ? CV(skin::PowerColor(SK(), lv))
                                 : CV(skin::Fade(SK().accent2, 0.96f));

    ImGui::TextColored(ratio_col, "%s", ratio_s);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar);
    ImGui::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x, 8.f), "");
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("—");
  }
  ImGui::EndGroup();

  if (p.has_score) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(accent.x, accent.y, accent.z, 0.35f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    draw_trend_chart(u8"本局", trend_for(p.player));
  }

  if (p.roster_count > 0 || p.unit_total || p.building_total || p.protocol_count > 0) {
    ImGui::Dummy(ImVec2(0.f, 4.f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(accent.x, accent.y, accent.z, 0.28f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.f, 3.f));

    const float icon_sz = ui_px(42.f) * g_stats_icon_scale;
    const float gap = ui_px(4.f) * g_stats_icon_scale;
    const float pad = ui_px(2.f);
    const float cell_w = icon_sz + pad * 2.f;
    const float cell_h = icon_sz + pad * 2.f;
    const float round = ui_px(5.f);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    auto draw_roster_section = [&](const char* title, bool buildings, ImU32 accent_u32,
                                   bool trailing_gap) {
      int n = 0;
      for (int i = 0; i < p.roster_count; ++i) {
        if (p.roster[i].is_building == buildings && p.roster[i].count > 0) ++n;
      }
      if (n <= 0) return;

      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, ui_px(3.f)));
      ImGui::TextColored(CV(SubHeadCol()), "%s", title);
      ImGui::SameLine(0.f, ui_px(6.f));
      ImGui::TextDisabled("%d", buildings ? p.building_total : p.unit_total);

      const float avail = ImGui::GetContentRegionAvail().x;
      float used_x = 0.f;
      bool row_started = false;

      for (int i = 0; i < p.roster_count; ++i) {
        const auto& r = p.roster[i];
        if (r.is_building != buildings || r.count <= 0) continue;

        if (row_started && used_x + cell_w + gap > avail) {
          ImGui::NewLine();
          used_x = 0.f;
          row_started = false;
        }
        if (row_started) ImGui::SameLine(0.f, gap);

        ImGui::PushID(r.key[0] ? r.key : r.name);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 chip1 = ImVec2(p0.x + cell_w, p0.y + cell_h);

        DrawChip(draw, p0, chip1, round, skin::Over(skin::Fade(SK().win0, 0.86f), SK().win1),
                 skin::Over(skin::Fade(SK().win0, 0.70f), SK().win1), accent_u32, 1.f);

        IDirect3DTexture9* tex = unit_icons::get(r.key, r.name, r.is_building);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
        if (tex) {
          ImGui::Image((ImTextureID)tex, ImVec2(icon_sz, icon_sz));
        } else {
          ImGui::Dummy(ImVec2(icon_sz, icon_sz));
          draw->AddRectFilled(ImVec2(p0.x + pad, p0.y + pad),
                              ImVec2(p0.x + pad + icon_sz, p0.y + pad + icon_sz),
                              EdgeIdle(), ui_px(3.f));
          const char* short_l = buildings ? u8"建" : u8"兵";
          const ImVec2 sts = ImGui::CalcTextSize(short_l);
          draw->AddText(ImVec2(p0.x + pad + (icon_sz - sts.x) * 0.5f,
                               p0.y + pad + (icon_sz - sts.y) * 0.5f),
                        SK().text2, short_l);
        }
        if (ImGui::IsMouseHoveringRect(p0, chip1)) {
          TipBegin();
          ImGui::TextUnformatted(r.name[0] ? r.name : r.key);
          ImGui::TextDisabled("%s  × %d", buildings ? u8"建筑" : u8"部队", r.count);
          TipEnd();
        }

        char cnt[16];
        std::snprintf(cnt, sizeof(cnt), "%d", r.count);
        const ImVec2 cts = ImGui::CalcTextSize(cnt);
        const float bx1 = chip1.x - pad;
        const float by1 = chip1.y - pad;
        const float bx0 = bx1 - cts.x - 6.f;
        const float by0 = by1 - cts.y - 2.f;
        draw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), ChipBg(),
                            ui_px(3.f));
        draw->AddText(ImVec2(bx0 + 3.f, by0 + 1.f), SK().money, cnt);

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y));
        ImGui::Dummy(ImVec2(cell_w, cell_h));
        ImGui::PopID();

        used_x += cell_w + gap;
        row_started = true;
      }
      ImGui::PopStyleVar();
      if (trailing_gap) ImGui::Dummy(ImVec2(0.f, ui_px(5.f)));
    };

    const ImU32 unit_accent =
        skin::Fade(p.is_local ? SK().money : SK().accent, 0.50f);
    const ImU32 bld_accent =
        skin::Fade(p.is_local ? SK().money : SK().accent, 0.58f);
    const bool has_bld = p.building_total > 0;
    const bool has_proto = p.protocol_count > 0;
    draw_roster_section(u8"部队", false, unit_accent, has_bld || has_proto);
    draw_roster_section(u8"建筑", true, bld_accent, has_proto);
    if (p.protocol_count > 0) {
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, ui_px(3.f)));
      ImGui::TextColored(CV(SubHeadCol()), u8"协议");
      ImGui::SameLine(0.f, ui_px(6.f));
      ImGui::TextDisabled("%d", p.protocol_count);
      const float avail = ImGui::GetContentRegionAvail().x;
      float used_x = 0.f;
      bool row_started = false;
      const ImU32 proto_accent = skin::Fade(SK().good, 0.70f);
      for (int i = 0; i < p.protocol_count; ++i) {
        const auto& pr = p.protocols[i];
        if (row_started && used_x + cell_w + gap > avail) {
          ImGui::NewLine();
          used_x = 0.f;
          row_started = false;
        }
        if (row_started) ImGui::SameLine(0.f, gap);
        ImGui::PushID(pr.key[0] ? pr.key : pr.name);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 chip1(p0.x + cell_w, p0.y + cell_h);
        DrawChip(draw, p0, chip1, round, skin::Over(skin::Fade(SK().good, 0.10f), SK().win1),
                 skin::Over(skin::Fade(SK().good, 0.06f), SK().win1), proto_accent, 1.f);
        IDirect3DTexture9* tex = unit_icons::get(pr.key, pr.name, false);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
        if (tex) ImGui::Image((ImTextureID)tex, ImVec2(icon_sz, icon_sz));
        else ImGui::Dummy(ImVec2(icon_sz, icon_sz));
        if (ImGui::IsMouseHoveringRect(p0, chip1)) {
          TipBegin();
          ImGui::TextUnformatted(pr.name[0] ? pr.name : pr.key);
          if (pr.remain_sec > 0) ImGui::TextDisabled(u8"剩余 %d 秒", pr.remain_sec);
          else ImGui::TextDisabled(u8"就绪");
          TipEnd();
        }
        if (pr.remain_sec > 0) {
          char clk[16];
          if (pr.remain_sec < 60) std::snprintf(clk, sizeof(clk), "%ds", pr.remain_sec);
          else std::snprintf(clk, sizeof(clk), "%d:%02d", pr.remain_sec / 60, pr.remain_sec % 60);
          const ImVec2 cts = ImGui::CalcTextSize(clk);
          const float bx1 = chip1.x - pad;
          const float by1 = chip1.y - pad;
          const float bx0 = bx1 - cts.x - 6.f;
          const float by0 = by1 - cts.y - 2.f;
          draw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), ChipBg(),
                              ui_px(3.f));
          draw->AddText(ImVec2(bx0 + 3.f, by0 + 1.f), SK().money, clk);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y));
        ImGui::Dummy(ImVec2(cell_w, cell_h));
        ImGui::PopID();
        used_x += cell_w + gap;
        row_started = true;
      }
      ImGui::PopStyleVar();
    }
  }

  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGui::PopID();
  ImGui::Spacing();
}

void text_ellipsis(const char* s, float max_w) {
  if (!s) s = "";
  if (max_w < 8.f || ImGui::CalcTextSize(s).x <= max_w) {
    ImGui::TextUnformatted(s);
    return;
  }
  const char* ell = u8"…";
  const float ell_w = ImGui::CalcTextSize(ell).x;
  const char* end = s + std::strlen(s);
  const char* cut = s;
  for (const char* p = s; p < end;) {
    const unsigned char c = (unsigned char)*p;
    int step = 1;
    if ((c & 0xE0) == 0xC0) step = 2;
    else if ((c & 0xF0) == 0xE0) step = 3;
    else if ((c & 0xF8) == 0xF0) step = 4;
    if (p + step > end) break;
    if (ImGui::CalcTextSize(s, p + step).x + ell_w > max_w) break;
    p += step;
    cut = p;
  }
  char buf[160];
  const size_t n = (size_t)(cut - s);
  if (n + 4 >= sizeof(buf)) {
    ImGui::TextUnformatted(s);
    return;
  }
  std::memcpy(buf, s, n);
  std::memcpy(buf + n, ell, 3);
  buf[n + 3] = 0;
  ImGui::TextUnformatted(buf);
  if (ImGui::IsItemHovered()) Tip("%s", s);
}

// Draw one compact chip at an absolute screen position (no ImGui layout side effects).
// 极简模式复用同一份画法（draw_minimal_unit_chip），所以两种模式下的兵种格子
// 必须像素一致——想改外观只改这里。
void draw_compact_icon_ex(ImDrawList* draw, const ImVec2& p0, const char* key, const char* name,
                          int count, float cell, float icon_sz, ImU32 border, bool building,
                          bool defeated, bool show_tip = true) {
  const ImVec2 chip1(p0.x + cell, p0.y + cell);
  const ImU32 fill = building ? skin::Over(skin::Fade(SK().money, 0.13f), SK().win1) : PanelBg();
  const float round = ui_px(SK().cardRadius * 0.30f);
  DrawChip(draw, p0, chip1, round, fill, skin::Mix(fill, SK().win1, 0.35f), border, 1.f);

  const float inset = (cell - icon_sz) * 0.5f;
  IDirect3DTexture9* tex = unit_icons::get(key, name, building);
  const ImU32 tint = defeated ? skin::Fade(SK().text3, 0.88f) : IM_COL32(255, 255, 255, 255);
  if (tex) {
    draw->AddImage((ImTextureID)tex, ImVec2(p0.x + inset, p0.y + inset),
                   ImVec2(p0.x + inset + icon_sz, p0.y + inset + icon_sz), ImVec2(0, 0),
                   ImVec2(1, 1), tint);
  } else {
    draw->AddRectFilled(ImVec2(p0.x + inset, p0.y + inset),
                        ImVec2(p0.x + inset + icon_sz, p0.y + inset + icon_sz),
                        EdgeIdle(), ui_px(3.f));
    const char* short_l = building ? u8"建" : u8"兵";
    const ImVec2 sts = ImGui::CalcTextSize(short_l);
    draw->AddText(ImVec2(p0.x + inset + (icon_sz - sts.x) * 0.5f,
                         p0.y + inset + (icon_sz - sts.y) * 0.5f),
                  SK().text2, short_l);
  }
  if (show_tip && ImGui::IsMouseHoveringRect(p0, chip1)) {
    TipBegin();
    ImGui::TextUnformatted(name && name[0] ? name : (key ? key : ""));
    ImGui::TextDisabled("%s  × %d", building ? u8"建筑" : u8"部队", count);
    TipEnd();
  }

  char cnt[16];
  std::snprintf(cnt, sizeof(cnt), "%d", count);
  ImFont* font = ImGui::GetFont();
  // 数量角标跟着格子走：图标放大 1.5 倍，数字也放大，否则角标会小得看不清。
  const float csz = (std::max)(10.f, (std::min)(cell * 0.40f, ImGui::GetFontSize() * 1.5f));
  const ImVec2 cts = font->CalcTextSizeA(csz, FLT_MAX, 0.f, cnt);
  const float bx1 = chip1.x - 1.f;
  const float by1 = chip1.y - 1.f;
  const float bx0 = bx1 - cts.x - cell * 0.14f;
  const float by0 = by1 - cts.y - cell * 0.05f;
  draw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), ChipBg(), ui_px(3.f));
  draw->AddText(font, csz, ImVec2(bx0 + cell * 0.07f, by0), SK().money, cnt);
}

void draw_compact_icon_at(ImDrawList* draw, const ImVec2& p0, const game_api::RosterType& r,
                          float cell, float icon_sz, ImU32 border, bool building, bool defeated) {
  draw_compact_icon_ex(draw, p0, r.key, r.name, r.count, cell, icon_sz, border, building, defeated);
}

void draw_compact_icon_row(const game_api::PlayerEconomy& p, bool buildings, bool defeated) {
  // 图标尺寸走"界面缩放 × 战况统计图标倍率"（设置里可调，默认 1.5 倍）。
  const float icon_sz = ui_px(32.f) * g_stats_icon_scale;
  const float cell = ui_px(34.f) * g_stats_icon_scale;
  const float gap = ui_px(3.f);
  const float row_gap = ui_px(2.f);  // space after this row before the next
  const float label_w = ui_px(56.f);

  const game_api::RosterType* items[game_api::kMaxRosterTypes];
  int n = 0;
  for (int i = 0; i < p.roster_count; ++i) {
    if (p.roster[i].count <= 0 || p.roster[i].is_building != buildings) continue;
    items[n++] = &p.roster[i];
  }

  const float avail = ImGui::GetContentRegionAvail().x;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 row0 = ImGui::GetCursorScreenPos();
  const int total = buildings ? p.building_total : p.unit_total;
  char head[32];
  if (n <= 0) std::snprintf(head, sizeof(head), "%s —", buildings ? u8"建筑" : u8"部队");
  else std::snprintf(head, sizeof(head), "%s %d", buildings ? u8"建筑" : u8"部队", total);

  const ImVec2 ts = ImGui::CalcTextSize(head);
  if (n <= 0) {
    draw->AddText(ImVec2(row0.x, row0.y), skin::Fade(SK().text2, 0.95f), head);
    // Grow layout via Dummy only — do not SetCursor beyond content first.
    ImGui::Dummy(ImVec2(avail, ts.y + row_gap));
    return;
  }

  const ImU32 border = buildings
                           ? skin::Fade(SK().money, buildings ? 0.78f : 0.72f)
                           : (p.is_local ? skin::Fade(SK().money, 0.70f)
                                         : skin::Fade(SK().accent, 0.72f));
  const float icons_x = row0.x + label_w;
  const float wrap_w = (std::max)(cell, avail - label_w);

  float x = 0.f;
  float y = 0.f;
  int rows = 1;
  for (int i = 0; i < n; ++i) {
    if (x > 0.f && x + cell > wrap_w) {
      x = 0.f;
      y += cell + gap;
      ++rows;
    }
    const ImVec2 p0(icons_x + x, row0.y + y);
    draw_compact_icon_at(draw, p0, *items[i], cell, icon_sz, border, buildings, defeated);
    x += cell + gap;
  }

  const float block_h = (float)rows * cell + (float)(rows - 1) * gap;
  draw->AddText(ImVec2(row0.x, row0.y + (block_h - ts.y) * 0.5f), skin::Fade(SK().text2, 0.95f),
                head);
  // Reserve the drawn block with Dummy so AutoResize / parent bounds grow.
  ImGui::Dummy(ImVec2(avail, block_h + row_gap));
}

void draw_compact_player(const game_api::PlayerEconomy& p, int index) {
  ImVec4 accent(p.color_r / 255.f, p.color_g / 255.f, p.color_b / 255.f, 1.f);
  if (!p.has_color) {
    accent = p.is_local ? CV(SK().money) : CV(SK().text2);
  }
  const ImVec4 money_col = CV(SK().money);
  const ImVec4 muted = CV(skin::Fade(SK().text3, 0.92f));
  const ImVec4 name_col = p.defeated ? muted : CV(SK().text);

  char label[96];
  const char* title = p.name[0] ? p.name : u8"玩家";
  std::snprintf(label, sizeof(label), "%s%s%s", title, p.is_local ? u8" ·己" : "",
                p.defeated ? u8" ·败" : "");
  char money_s[32];
  if (p.has_money) format_uint_grouped(money_s, sizeof(money_s), p.money);
  else std::snprintf(money_s, sizeof(money_s), u8"—");
  char power_s[48];
  float frac = 0.f;
  ImVec4 power_col = muted;
  if (p.has_power) {
    char used_s[24], total_s[24];
    format_uint_grouped(used_s, sizeof(used_s), p.power_used);
    format_uint_grouped(total_s, sizeof(total_s), p.power_total);
    std::snprintf(power_s, sizeof(power_s), "%s/%s", used_s, total_s);
    // 使用 > 总量（含 0 产能但有消耗）一律红：bar 和数值都用同一判定。
    const skin::PowerLevel lv = skin::PowerLevelOf(p.power_used, p.power_total);
    frac = (p.power_total > 0) ? (float)p.power_used / (float)p.power_total : 1.f;
    if (frac > 1.f) frac = 1.f;
    power_col = CV(skin::PowerColor(SK(), lv));
  } else {
    std::snprintf(power_s, sizeof(power_s), u8"—");
  }

  const float avail = ImGui::GetContentRegionAvail().x;
  const float right_w = 8.f + ImGui::CalcTextSize(u8"资金").x + 4.f + ImGui::CalcTextSize(money_s).x +
                        10.f + ImGui::CalcTextSize(u8"电力").x + 4.f + ImGui::CalcTextSize(power_s).x;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::PushID(index + 1);
  ImGui::BeginGroup();

  const float sw = ImGui::GetTextLineHeight();
  const ImVec2 sw0 = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(sw0, ImVec2(sw0.x + sw, sw0.y + sw), ImGui::GetColorU32(accent), 2.f);
  ImGui::Dummy(ImVec2(sw, sw));
  ImGui::SameLine(0.f, 4.f);
  const float name_w = (std::max)(28.f, avail - sw - 8.f - right_w);
  ImGui::PushStyleColor(ImGuiCol_Text, name_col);
  text_ellipsis(label, name_w);
  ImGui::PopStyleColor();
  ImGui::SameLine(0.f, 8.f);
  ImGui::TextDisabled(u8"资金");
  ImGui::SameLine(0.f, 4.f);
  ImGui::TextColored(p.has_money ? money_col : muted, "%s", money_s);
  ImGui::SameLine(0.f, 10.f);
  ImGui::TextDisabled(u8"电力");
  ImGui::SameLine(0.f, 4.f);
  ImGui::TextColored(power_col, "%s", power_s);

  // Tiny gap under the header, then pack 部队 / 建筑 tight.
  ImGui::Dummy(ImVec2(0.f, ui_px(2.f)));
  draw_compact_icon_row(p, false, p.defeated);
  draw_compact_icon_row(p, true, p.defeated);
  if (p.protocol_count > 0) {
    const float icon_sz = ui_px(32.f);
    const float cell = ui_px(34.f);
    const float gap = ui_px(3.f);
    const float row_gap = ui_px(2.f);
    const float label_w = ui_px(56.f);
    const float avail_p = ImGui::GetContentRegionAvail().x;
    ImDrawList* pdraw = ImGui::GetWindowDrawList();
    const ImVec2 row0 = ImGui::GetCursorScreenPos();
    char head[32];
    std::snprintf(head, sizeof(head), u8"协议 %d", p.protocol_count);
    const ImVec2 ts = ImGui::CalcTextSize(head);
    const float icons_x = row0.x + label_w;
    const float wrap_w = (std::max)(cell, avail_p - label_w);
    float x = 0.f;
    float y = 0.f;
    int rows = 1;
    const ImU32 border = skin::Fade(SK().good, 0.74f);
    for (int i = 0; i < p.protocol_count; ++i) {
      const auto& pr = p.protocols[i];
      if (x > 0.f && x + cell > wrap_w) {
        x = 0.f;
        y += cell + gap;
        ++rows;
      }
      const ImVec2 p0(icons_x + x, row0.y + y);
      const ImVec2 chip1(p0.x + cell, p0.y + cell);
      DrawChip(pdraw, p0, chip1, ui_px(SK().cardRadius * 0.30f),
               skin::Over(skin::Fade(SK().good, 0.12f), SK().win1),
               skin::Over(skin::Fade(SK().good, 0.07f), SK().win1), border, 1.f);
      const float inset = (cell - icon_sz) * 0.5f;
      IDirect3DTexture9* tex = unit_icons::get(pr.key, pr.name, false);
      const ImU32 tint = p.defeated ? skin::Fade(SK().text3, 0.88f) : IM_COL32(255, 255, 255, 255);
      if (tex) {
        pdraw->AddImage((ImTextureID)tex, ImVec2(p0.x + inset, p0.y + inset),
                        ImVec2(p0.x + inset + icon_sz, p0.y + inset + icon_sz), ImVec2(0, 0),
                        ImVec2(1, 1), tint);
      }
      if (ImGui::IsMouseHoveringRect(p0, chip1)) {
        TipBegin();
        ImGui::TextUnformatted(pr.name[0] ? pr.name : pr.key);
        if (pr.remain_sec > 0) ImGui::TextDisabled(u8"剩余 %d 秒", pr.remain_sec);
        else ImGui::TextDisabled(u8"就绪");
        TipEnd();
      }
      if (pr.remain_sec > 0) {
        char clk[16];
        if (pr.remain_sec < 60) std::snprintf(clk, sizeof(clk), "%ds", pr.remain_sec);
        else std::snprintf(clk, sizeof(clk), "%d:%02d", pr.remain_sec / 60, pr.remain_sec % 60);
        ImFont* font = ImGui::GetFont();
        const float csz = (std::max)(10.f, ImGui::GetFontSize() * 0.72f);
        const ImVec2 cts = font->CalcTextSizeA(csz, FLT_MAX, 0.f, clk);
        const float bx1 = chip1.x - 1.f;
        const float by1 = chip1.y - 1.f;
        const float bx0 = bx1 - cts.x - 5.f;
        const float by0 = by1 - cts.y - 1.f;
        pdraw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), ChipBg(),
                             ui_px(3.f));
        pdraw->AddText(font, csz, ImVec2(bx0 + 2.5f, by0), SK().money, clk);
      }
      x += cell + gap;
    }
    const float block_h = (float)rows * cell + (float)(rows - 1) * gap;
    pdraw->AddText(ImVec2(row0.x, row0.y + (block_h - ts.y) * 0.5f), skin::Fade(SK().good, 0.95f),
                   head);
    ImGui::Dummy(ImVec2(avail_p, block_h + row_gap));
  }

  ImGui::EndGroup();
  const ImVec2 r0 = ImGui::GetItemRectMin();
  const ImVec2 r1 = ImGui::GetItemRectMax();
  ImGui::PopID();

  dl->ChannelsSetCurrent(0);
  // 玩家卡底：斑马纹行底，形状跟皮肤（切角皮肤下卡片是斜角）。
  {
    const ImU32 top = (index & 1) ? skin::Fade(SK().cardInnerLo, 0.60f)
                                  : skin::Fade(SK().cardInnerHi, 0.05f);
    const ImU32 bot = (index & 1) ? skin::Fade(SK().cardInnerLo, 0.35f) : skin::WithA(top, 0);
    DrawChip(dl, ImVec2(origin.x, r0.y), ImVec2(origin.x + avail, r1.y),
             ui_px(SK().cardRadius * 0.26f), top, bot, skin::Fade(EdgeIdle(), 0.55f), 1.f);
  }
  dl->AddRectFilled(ImVec2(origin.x, r0.y), ImVec2(origin.x + 3.f, r1.y), ImGui::GetColorU32(accent),
                    2.f);
  dl->ChannelsMerge();
}

void draw_stats_compact(const game_api::MatchEconomy& st) {
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 3.f));
  for (int i = 0; i < st.player_count; ++i) {
    draw_compact_player(st.players[i], i);
  }
  ImGui::PopStyleVar();
}

// ---------------- 极简模式：一个玩家一行 ----------------
//
// 排版（列宽、右对齐、窄窗口截断）在 stats_minimal.cpp 里：那里不碰游戏数据，
// 离线预览能渲染同一份实现做像素核对。这里只做数据适配 + 悬停明细。
void draw_stats_minimal(const game_api::MatchEconomy& st) {
  static std::vector<stats_minimal::Row> rows;
  // 兵种列表每行一段，单独放一个池子：Row 里只存指针，扩容时不会悬空。
  static std::vector<stats_minimal::UnitIcon> units;
  // 图标倍率是设置项：每帧同步一次，改滑块立刻见效。
  stats_minimal::SetChipScale(g_stats_icon_scale);
  rows.clear();
  units.clear();
  rows.reserve((size_t)st.player_count);

  size_t unit_hint = 0;
  for (int i = 0; i < st.player_count; ++i) unit_hint += (size_t)st.players[i].roster_count;
  units.reserve(unit_hint);

  for (int i = 0; i < st.player_count; ++i) {
    const auto& p = st.players[i];
    stats_minimal::Row r;
    r.name = p.name;
    r.is_local = p.is_local;
    r.defeated = p.defeated;
    r.has_color = p.has_color;
    r.color_r = p.color_r;
    r.color_g = p.color_g;
    r.color_b = p.color_b;
    r.has_money = p.has_money;
    r.money = p.money;
    r.has_power = p.has_power;
    r.power_used = p.power_used;
    r.power_total = p.power_total;
    r.unit_total = p.unit_total;
    r.building_total = p.building_total;
    // 极简只上兵种图标，建筑不上（建筑数量放悬停里看）。
    const size_t first = units.size();
    for (int k = 0; k < p.roster_count; ++k) {
      const auto& rt = p.roster[k];
      if (rt.is_building || rt.count <= 0) continue;
      stats_minimal::UnitIcon u;
      u.key = rt.key;
      u.name = rt.name;
      u.count = rt.count;
      units.push_back(u);
    }
    r.unit_count = (int)(units.size() - first);
    rows.push_back(r);
  }
  // 指针在全部 push_back 之后再取：中途扩容会让先取的地址失效。
  size_t cur = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    rows[i].units = rows[i].unit_count > 0 ? &units[cur] : nullptr;
    cur += (size_t)rows[i].unit_count;
  }

  // 行距压到 0：行高已经在 DrawRow 里算好，再叠一层 ItemSpacing 就不"紧密"了。
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 0.f));
  const float px = ui_px(1.f);
  const float avail = ImGui::GetContentRegionAvail().x;

  // 诊断：极简模式的兵种图标"有没有东西可画"取决于 roster 是否被扫描填上。
  // 每 2 秒记一行，roster=N units=M 能直接区分"数据空"和"排版挤掉"；
  // 后面再跟每条 roster 的 名字:数量:建筑位，一次就能看清是分类错了还是根本没数据。
  {
    static float s_t = 0.f;
    s_t += ImGui::GetIO().DeltaTime;
    if (s_t >= 2.f) {
      s_t = 0.f;
      char buf[1024];
      int o = std::snprintf(buf, sizeof(buf), "minimal: avail=%.0f win=%.0f players=%d |", avail,
                            ImGui::GetWindowSize().x, st.player_count);
      for (int i = 0; i < st.player_count; ++i) {
        o += std::snprintf(buf + o, sizeof(buf) - (size_t)o, " [%d roster=%d units=%d tot=%d]", i,
                           st.players[i].roster_count, rows[i].unit_count,
                           st.players[i].unit_total);
        if (i >= 2) continue;
        for (int k = 0; k < st.players[i].roster_count && o < (int)sizeof(buf) - 40; ++k) {
          const auto& rt = st.players[i].roster[k];
          o += std::snprintf(buf + o, sizeof(buf) - (size_t)o, " %s:%d:b%d",
                             rt.name[0] ? rt.name : rt.key, rt.count, rt.is_building ? 1 : 0);
        }
      }
      game_api::log("%s", buf);
    }
  }
  stats_minimal::Cols cols;
  stats_minimal::Measure(&cols, rows.data(), (int)rows.size(), avail, px);
  stats_minimal::DrawHeader(cols, px);

  for (int i = 0; i < (int)rows.size(); ++i) {
    ImVec2 r0{}, r1{};
    stats_minimal::DrawRow(rows[i], i, cols, px, &r0, &r1);
    if (!ImGui::IsMouseHoveringRect(r0, r1)) continue;

    // 一行塞不下的信息放悬停：兵力明细和比分。表格本体只留资金、电力、兵种图标。
    const auto& p = st.players[i];
    char money_s[32], power_s[48];
    if (p.has_money) format_uint_grouped(money_s, sizeof(money_s), p.money);
    else std::snprintf(money_s, sizeof(money_s), u8"—");
    if (p.has_power) {
      char used[24], total[24];
      format_uint_grouped(used, sizeof(used), p.power_used);
      format_uint_grouped(total, sizeof(total), p.power_total);
      std::snprintf(power_s, sizeof(power_s), "%s/%s", used, total);
    } else {
      std::snprintf(power_s, sizeof(power_s), u8"—");
    }
    TipBegin();
    ImGui::TextUnformatted(p.name[0] ? p.name : u8"玩家");
    ImGui::TextDisabled(u8"资金 %s   电力 %s", money_s, power_s);
    ImGui::TextDisabled(u8"部队 %d   建筑 %d", p.unit_total, p.building_total);
    if (p.has_score) {
      ImGui::TextDisabled(u8"造兵 %u  被杀 %u  击杀 %u", p.units_built,
                          p.units_lost, p.units_destroyed);
    }
    for (int k = 0; k < p.roster_count; ++k) {
      const auto& rt = p.roster[k];
      if (rt.count <= 0) continue;
      ImGui::TextDisabled("%s%s x%d", rt.name[0] ? rt.name : rt.key,
                          rt.is_building ? u8"（建筑）" : "", rt.count);
    }
    TipEnd();
  }
  ImGui::PopStyleVar();
}

void draw_stats_panel() {
  static game_api::MatchEconomy st{};
  // Wall-clock gate — ImGui::GetTime() advances by the hitch duration, so after a
  // slow collect the next frame was immediately due again (death spiral ~1.5s/frame).
  // On open: poll immediately once to enqueue the full player-list scan. While
  // seeding, protocol rebuild is skipped (~40ms hitch gone); 1s refresh is enough
  // to show counts as Present classifies. Steady state stays at 2s.
  static DWORD last_poll_tick = 0;
  static DWORD last_res_tick = 0;
  static bool was_seeding = false;
  const DWORD now_tick = GetTickCount();
  const bool seeding = game_api::roster_seed_pending();
  if (seeding && !was_seeding) last_poll_tick = 0;
  was_seeding = seeding;
  // Full collect (roster / protocols / seats): slow. Money / power: ~10Hz.
  const int poll_ms = seeding ? 1000 : 2000;
  if (last_poll_tick == 0 || (int32_t)(now_tick - last_poll_tick) >= poll_ms) {
    LARGE_INTEGER freq{}, t0{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    game_api::collect_match_economy(&st);
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    const double ms = (freq.QuadPart > 0)
                          ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
                          : 0.0;
    if (ms >= 20.0) {
      game_api::log("perf: collect_match_economy=%.1fms players=%d seed=%d", ms, st.player_count,
                    seeding ? 1 : 0);
    }
    last_poll_tick = now_tick;
    last_res_tick = now_tick;
  } else if (st.valid && (last_res_tick == 0 || (int32_t)(now_tick - last_res_tick) >= 100)) {
    game_api::refresh_match_resources(&st);
    last_res_tick = now_tick;
  }
  sample_trends(st);

  // 极简模式把"战况总览"这一整行省掉：一屏就那几行玩家，标题只是白占高度。
  // 观战标记和方数并到表头下面那一行里由 stats_minimal 的列宽自然带出来。
  if (!g_stats_minimal) {
    ImGui::TextColored(CV(SK().accent2), u8"战况总览");
    if (st.spectator) {
      ImGui::SameLine();
      ImGui::TextColored(CV(SK().money), u8"观战");
    }
    if (st.valid) {
      char count_s[32];
      std::snprintf(count_s, sizeof(count_s), u8"%d 方", st.player_count);
      const float count_w = ImGui::CalcTextSize(count_s).x;
      ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - count_w);
      ImGui::TextDisabled("%s", count_s);
    }
    ImGui::PushStyleColor(ImGuiCol_Separator, CVA(SK().accent, 0.55f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    if (!g_stats_compact) ImGui::Spacing();
  }

  if (!st.valid) {
    ImGui::TextWrapped(u8"%s", st.note[0] ? st.note : u8"暂无数据：请先进入对局。");
    return;
  }

  if (g_stats_minimal) {
    draw_stats_minimal(st);
    return;
  }
  if (g_stats_compact) {
    draw_stats_compact(st);
    return;
  }

  for (int i = 0; i < st.player_count; ++i) {
    draw_player_econ_card(st.players[i], i);
  }
}

void draw_stats_window() {
  if (!g_stats_open) {
    shell::ResetDrag("stats");
    return;
  }
  if (!input::menu_visible() && !g_stats_pinned) {
    g_stats_open = false;
    shell::ResetDrag("stats");
    return;
  }

  // 极简模式要放兵种图标，横向要宽一点；但一屏只有几个玩家，纵向可以矮很多。
  // 宽度随图标倍率一起长：图标放大后 580 一行只放得下两三个。
  const ImVec2 st_size =
      g_stats_minimal
          ? scaled_window_size(420.f + 160.f * g_stats_icon_scale,
                               200.f + 100.f * g_stats_icon_scale)
          : scaled_window_size(420.f, 640.f);
  // 默认贴右边；高缩放下 950 可能已经在屏幕外，按当前窗口尺寸反算。
  // 位置在 Begin 之前定好（理由同主窗口）：拖动位移先吃掉，内容不会落后一帧。
  static ImVec2 s_stats_pos(-1.f, -1.f);
  if (s_stats_pos.x < 0.f) {
    s_stats_pos.x = (std::max)(ui_px(12.f), ImGui::GetIO().DisplaySize.x - st_size.x - ui_px(20.f));
    s_stats_pos.y = ui_px(28.f);
  }
  // 切模式时窗口尺寸会变：记住上一帧的尺寸，变了就强制设一次，并把它拉回屏幕内
  // （否则贴着右边缘的窗口换成宽版会整块探出屏幕）。
  static ImVec2 s_last_size(-1.f, -1.f);
  const bool size_changed = (s_last_size.x != st_size.x || s_last_size.y != st_size.y);
  if (size_changed && s_last_size.x > 0.f) {
    const float max_x = ImGui::GetIO().DisplaySize.x - st_size.x - ui_px(20.f);
    s_stats_pos.x = (std::max)(ui_px(12.f), (std::min)(s_stats_pos.x, max_x));
  }
  s_last_size = st_size;
  shell::DragUpdate("stats", &s_stats_pos);
  ImGui::SetNextWindowPos(s_stats_pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(st_size, size_changed ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2((std::min)(ui_px(320.f), st_size.x), (std::min)(ui_px(280.f), st_size.y)),
      ImVec2(FLT_MAX, FLT_MAX));
  ImGui::SetNextWindowBgAlpha(g_ui_opacity);

  // Pinned: no close button — must uncheck 固定 to hide.
  // 和主菜单同一套外壳：去掉 ImGui 的直角标题条与矩形底色，标题栏自己画。
  // NoMove 见 shell.h：位移统一由 DragUpdate 处理，避免两处叠加。
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoMove;
  bool open = g_stats_open;
  if (g_stats_pinned) {
    flags |= ImGuiWindowFlags_NoCollapse;
  }

  // 极简模式整套内边距收紧：省下来的高度正好多显示两三个玩家。
  const ImVec2 pad_normal = ImGui::GetStyle().WindowPadding;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      g_stats_minimal ? ImVec2(ui_px(8.f), ui_px(6.f)) : pad_normal);

  if (!ImGui::Begin(u8"战况统计", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }

  // 排查用：每 2 秒记一次滚动区实况。滚轮没反应到底是"没东西可滚"、
  // "窗口没被 hover"、还是"滚了又被钳回 0"，这几项能直接分辨。
  {
    static float s_diag_t = 0.f;
    s_diag_t += ImGui::GetIO().DeltaTime;
    if (s_diag_t >= 2.f) {
      s_diag_t = 0.f;
      const ImVec2 wp = ImGui::GetWindowPos();
      const ImVec2 ws = ImGui::GetWindowSize();
      game_api::log(
          "stats: pos=(%.0f,%.0f) size=(%.0f,%.0f) scrollmax=(%.0f,%.0f) scroll=(%.0f,%.0f) "
          "hovered=%d want_mouse=%d wheel=%.0f",
          wp.x, wp.y, ws.x, ws.y, ImGui::GetScrollMaxX(), ImGui::GetScrollMaxY(),
          ImGui::GetScrollX(), ImGui::GetScrollY(),
          ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ? 1 : 0,
          ImGui::GetIO().WantCaptureMouse ? 1 : 0, ImGui::GetIO().MouseWheel);
    }
  }

  {
    shell::Hit sh;
    const ImVec2 content0 = ImGui::GetCursorScreenPos();
    const ImVec2 wp = ImGui::GetWindowPos();
    // 极简模式下标题栏也缩一档：窗体和内容一起变紧凑，不然只剩内容挤、标题还是原尺寸。
    const float shell_scale = g_stats_minimal ? g_ui_scale * 0.80f : g_ui_scale;
    const float shell_h = shell::Draw(ImGui::GetWindowDrawList(), wp,
                                      ImVec2(wp.x + ImGui::GetWindowSize().x,
                                             wp.y + ImGui::GetWindowSize().y),
                                      shell_scale, g_ui_opacity, u8"战况统计",
                                      g_stats_minimal ? u8" · 极简" : u8" · 实时", true, &sh,
                                      !g_stats_pinned, "stats");
    ImGui::SetCursorScreenPos(
        ImVec2(content0.x, wp.y + shell_h + ui_px(g_stats_minimal ? 3.f : 6.f)));
    if (sh.close) open = false;
  }

  if (ImGui::Checkbox(u8"固定", &g_stats_pinned)) {
    mark_settings_dirty();
    g_settings_save_timer = 0.2f;
    if (g_stats_pinned) {
      g_stats_open = true;
    } else if (!input::menu_visible()) {
      g_stats_open = false;
      ImGui::End();
      ImGui::PopStyleVar();
      return;
    }
  }
  ImGui::SameLine();
  // 精简 / 极简是两档显示密度，互斥：勾一个自动退另一个。
  if (ImGui::Checkbox(u8"精简", &g_stats_compact)) {
    if (g_stats_compact) g_stats_minimal = false;
    mark_settings_dirty();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox(u8"极简", &g_stats_minimal)) {
    if (g_stats_minimal) g_stats_compact = false;
    mark_settings_dirty();
  }
  // 极简模式不再解释一遍自己：那一行提示只在标准 / 精简下有用，留着白占一行高度。
  if (!g_stats_minimal) {
    ImGui::TextDisabled(g_stats_compact ? u8"图标、资金、电力"
                                        : (g_stats_pinned ? u8"主菜单隐藏后仍显示"
                                                          : u8"取消固定后可关闭本窗口"));
  }

  if (!g_stats_minimal) ImGui::Spacing();
  // 列表放进独立滚动区。
  //
  // 之前依赖根窗口的隐式滚动：内容尺寸由 CursorMaxPos 推算，而统计面板大量用
  // 绝对坐标 + Dummy 预留高度画格子，只要有一处算少，ScrollMax.y 就是 0 ——
  // 滚轮再怎么滚也没东西可滚。子窗口的滚动区是显式容器，内容超出就一定滚，
  // 而且标题行（固定 / 精简）留在滚动区上面，不会跟着一起滚走。
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
  ImGui::BeginChild("##stats_body", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
  const float body_y0 = ImGui::GetScrollY();
  draw_stats_panel();
  // 兜底滚动：鼠标明明在列表里，但 ImGui 把这一格滚轮派给了别人（hover 被抢）
  // 时，自己吃下来。只在"ImGui 自己没滚成"的情况下生效，不会滚两倍。
  {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.f && !io.WantTextInput && ImGui::GetScrollY() == body_y0) {
      const ImVec2 wp0 = ImGui::GetWindowPos();
      const ImVec2 ws0 = ImGui::GetWindowSize();
      if (ImGui::IsMouseHoveringRect(wp0, ImVec2(wp0.x + ws0.x, wp0.y + ws0.y))) {
        float step = 5.f * ImGui::GetFontSize();
        const float max_step = ImGui::GetWindowHeight() * 0.67f;
        if (step > max_step) step = max_step;
        ImGui::SetScrollY(body_y0 - io.MouseWheel * step);
      }
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::End();
  ImGui::PopStyleVar();

  if (!g_stats_pinned && !open) g_stats_open = false;
}

}  // namespace

bool init(IDirect3DDevice9* device, void* hwnd) {
  if (g_inited) return true;
  if (!device || !hwnd) return false;
  g_hwnd = static_cast<HWND>(hwnd);

  load_ui_settings();

  // 极简模式的行图标走 unit_icons 的纹理缓存；stats_minimal 自己不链接它
  // （预览工具没有游戏纹理），所以在这里注入一次。
  stats_minimal::SetIconProvider(
      [](const char* key, const char* name, bool building) -> ImTextureID {
        return (ImTextureID)unit_icons::get(key, name, building);
      });
  // 极简模式一个格子的画法直接用精简模式那份：同一个函数，两种模式长得一样。
  stats_minimal::SetChipRenderer(
      [](ImDrawList* dl, ImVec2 p0, float cell, const stats_minimal::UnitIcon& u,
         ImU32 border, bool defeated) {
        draw_compact_icon_ex(dl, p0, u.key, u.name, u.count, cell, cell * 0.94f, border,
                             /*building*/ false, defeated, /*show_tip*/ false);
      });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  ImFontConfig cfg;
  cfg.OversampleH = 1;  // 2x oversample doubles atlas fill cost for CJK
  cfg.PixelSnapH = true;
  const char* font_candidates[] = {
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\msyh.ttf",
      "C:\\Windows\\Fonts\\simhei.ttf",
      "C:\\Windows\\Fonts\\simsun.ttc",
  };
  bool font_ok = false;
  const char* font_used = "(default)";
  for (const char* path : font_candidates) {
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
      // ChineseFull builds a huge atlas and freezes the first open for seconds.
      // Common glyphs cover the UI; missing rare chars fall back to '?'.
      ImFont* f = io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg,
                                               io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
      font_ok = (f != nullptr);
      font_used = font_ok ? path : "(load failed)";
      break;
    }
  }
  if (!font_ok) io.Fonts->AddFontDefault();
  // 中文字全部显示成 '?' 时，第一件事就是看这一行：字体文件没加载上，
  // ImGui 会退回内置 ASCII 字体，所有汉字都会变成替换字符。
  game_api::log("font: %s ok=%d scale=%.2f fonts=%d", font_used, font_ok ? 1 : 0,
                g_ui_scale, io.Fonts->Fonts.Size);

  apply_ra3_theme();

  if (!ImGui_ImplWin32_Init(g_hwnd)) {
    ImGui::DestroyContext();
    return false;
  }
  if (!ImGui_ImplDX9_Init(device)) {
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return false;
  }

  unit_icons::init(device);

  g_inited = true;
  if (!g_async_cs_ready) {
    InitializeCriticalSection(&g_async_cs);
    g_async_cs_ready = true;
  }
  game_api::log("ImGui UI ready scale=%.2f opacity=%.2f build=%s %s", g_ui_scale,
                g_ui_opacity, __DATE__, __TIME__);
  return true;
}

void shutdown() {
  if (!g_inited) return;
  unit_icons::shutdown();
  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_inited = false;
  g_hwnd = nullptr;
}

void begin_frame() {
  if (!g_inited) return;
  unit_icons::begin_frame();
  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
}

static int g_tile_col = 0;
static int g_tile_cols = 3;
static float g_tile_w = 0.f;

static void end_chip_flow() { g_tile_col = 0; }

// HUD card. Consecutive cards fill a 2- or 3-column grid.
// active lights the card (toggles and the selected danger level).
static bool draw_hud_tile(const char* id, const char* title, const char* hotkey, bool active,
                          bool enabled) {
  const float gap = 8.f;
  if (g_tile_col == 0) {
    const float avail = ImGui::GetContentRegionAvail().x;
    g_tile_cols = avail >= 480.f ? 3 : 2;
    g_tile_w = (avail - gap * (float)(g_tile_cols - 1)) / (float)g_tile_cols;
    if (g_tile_w < 48.f) g_tile_w = avail;
  } else {
    ImGui::SameLine(0.f, gap);
  }
  const float h = ImGui::GetFrameHeight() * 2.15f;
  ImGui::PushID(id);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 sz(g_tile_w, h);
  const bool pressed = ImGui::InvisibleButton("##hud", sz);
  const bool hov = enabled && ImGui::IsItemHovered();
  const bool held = enabled && ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
  const float rnd = ui_px(SK().cardRadius * 0.62f);
  // 悬停/按下的过渡量：控件是渐亮而不是硬跳。
  const ImGuiID tile_id = ImGui::GetItemID();
  const float hv = AnimTo(tile_id, hov, 16.f);
  const float av = AnimTo(tile_id + 1u, held, 26.f);

  ImU32 top;
  ImU32 bot;
  ImU32 edge;
  ImU32 bar;
  if (!enabled) {
    top = skin::WithA(skin::Over(SK().card1, SK().win1), 150);
    bot = top;
    edge = skin::Fade(SK().text3, 0.45f);
    bar = skin::Fade(SK().text3, 0.32f);
  } else if (active) {
    top = skin::Mix(SK().win1, SK().accent, 0.50f + 0.14f * hv + 0.12f * av);
    bot = skin::Mix(SK().win1, SK().accent, 0.62f + 0.14f * hv + 0.12f * av);
    edge = SK().accent2;
    bar = SK().accent2;
  } else {
    top = skin::Mix(PanelBg(), SK().accent, 0.14f * hv + 0.16f * av);
    bot = skin::Mix(PanelBg(), skin::Darken(SK().win1, 0.35f), 0.55f);
    edge = skin::Mix(EdgeIdle(), SK().accent, 0.55f * hv);
    bar = skin::Fade(SK().accent, 0.55f + 0.30f * hv);
  }

  // 整块底板走 paint::Panel：投影、渐变、顶部高光、描边一次画完，
  // 形状（圆角 / 切角）与描边宽度由皮肤决定。
  PlateSpec ps;
  ps.top = top;
  ps.bot = bot;
  ps.edge = edge;
  ps.inner = skin::Fade(SK().cardInnerHi, active ? 0.16f : 0.08f);
  ps.shadow = SK().shadow;
  ps.dy = ui_px(2.f) + ui_px(2.f) * av;
  ps.sigma = ui_px(4.f);
  ps.sk = 1.f + 0.6f * hv;
  ps.th = (hov || active) ? 1.6f : 1.f;
  DrawPlate(dl, p0, p1, rnd, ps);
  dl->AddRectFilled(ImVec2(p0.x + 2.f, p0.y + 9.f), ImVec2(p0.x + 5.f, p1.y - 9.f), bar, 2.f);

  dl->PushClipRect(ImVec2(p0.x + 12.f, p0.y + 3.f), ImVec2(p1.x - 8.f, p1.y - 3.f), true);
  const ImVec2 ts = ImGui::CalcTextSize(title ? title : "");
  const bool has_key = hotkey && hotkey[0];
  const float ty = p0.y + (has_key ? 7.f : (sz.y - ts.y) * 0.5f);
  dl->AddText(ImVec2(p0.x + 14.f, ty),
              enabled ? SK().text : skin::Fade(SK().text3, 0.90f),
              title ? title : "");
  if (has_key) {
    const ImVec2 bs = ImGui::CalcTextSize(hotkey);
    const ImVec2 b0(p1.x - bs.x - 14.f, p1.y - bs.y - 7.f);
    dl->AddRectFilled(ImVec2(b0.x - 6.f, b0.y - 2.f), ImVec2(p1.x - 8.f, b0.y + bs.y + 2.f),
                      ChipBg(), 4.f);
    dl->AddText(b0, enabled ? skin::Fade(SK().accent2, 0.94f) : skin::Fade(SK().text3, 0.70f),
                hotkey);
  }
  if (active) {
    const ImVec2 dot(p1.x - 12.f, p0.y + 12.f);
    paint::Glow(dl, dot, ui_px(4.f), SK().accentGlow, 3);
    dl->AddCircleFilled(dot, 3.5f, SK().accent2);
  }
  dl->PopClipRect();

  g_tile_col = (g_tile_col + 1) % g_tile_cols;
  ImGui::PopID();
  return enabled && pressed;
}

static bool draw_nav_item(const char* id, const char* label, bool selected) {
  ImGui::PushID(id);
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = ImGui::GetFrameHeight() + 2.f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p1(p0.x + w, p0.y + h);
  const float rnd = ui_px(SK().cardRadius * 0.46f);
  const float hv = AnimTo(ImGui::GetItemID(), hov || selected, 18.f);
  if (hv > 0.01f) {
    PlateSpec ps;
    if (selected) {
      ps.top = skin::Mix(SK().win1, SK().accent, 0.34f);
      ps.bot = skin::Mix(SK().win1, SK().accent, 0.46f);
      ps.edge = EdgeHot();
      ps.inner = skin::Fade(SK().accent2, 0.22f);
      ps.th = 1.2f;
    } else {
      ps.top = skin::Fade(SK().cardInnerHi, 0.10f * hv);
      ps.bot = skin::Fade(SK().cardInnerLo, 0.30f * hv);
      ps.edge = skin::Fade(EdgeIdle(), hv);
    }
    ps.shadow = selected ? SK().shadow : 0;
    ps.dy = ui_px(1.f);
    ps.sigma = ui_px(2.5f);
    ps.sk = 1.f;
    DrawPlate(dl, p0, p1, rnd, ps);
  }
  if (selected) {
    // 左侧指示条：选中态的唯一"硬"元素，切角皮肤下同样保持竖直。
    dl->AddRectFilled(ImVec2(p0.x + 1.f, p0.y + 5.f), ImVec2(p0.x + 4.f, p1.y - 5.f),
                      SK().accent2, 2.f);
  }
  const ImVec2 ts = ImGui::CalcTextSize(label ? label : "");
  dl->AddText(ImVec2(p0.x + 12.f, p0.y + (h - ts.y) * 0.5f),
              selected ? SK().text : skin::Mix(skin::Fade(SK().text2, 0.94f), SK().text, hv),
              label ? label : "");
  ImGui::PopID();
  return pressed;
}

static int g_capture_slot = -1;
static bool g_capture_ready = false;

static const char* danger_slot_label(const char* id) {
  if (!id) return "";
  if (std::strcmp(id, "danger_max") == 0) return u8"高";
  if (std::strcmp(id, "danger_min") == 0) return u8"最高";
  if (std::strcmp(id, "danger_norm") == 0) return u8"正常";
  return id;
}

static void begin_hotkey_capture(int slot) {
  g_capture_slot = slot;
  g_capture_ready = false;
  game_api::set_hotkey_capture(true);
  g_last_msg = u8"请按下新的快捷键，Esc 取消";
}

static void poll_hotkey_capture() {
  if (g_capture_slot < 0) {
    game_api::set_hotkey_capture(false);
    return;
  }
  game_api::set_hotkey_capture(true);
  bool any = false;
  int found = 0;
  for (int vk = 8; vk < 256; ++vk) {
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
        vk == VK_XBUTTON2) {
      continue;
    }
    if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
    any = true;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT ||
        vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
        vk == VK_RMENU) {
      continue;
    }
    found = vk;
    break;
  }
  if (!g_capture_ready) {
    if (!any) g_capture_ready = true;
    return;
  }
  if (!found) return;
  const int slot = g_capture_slot;
  g_capture_slot = -1;
  g_capture_ready = false;
  game_api::set_hotkey_capture(false);
  if (found == VK_ESCAPE) {
    g_last_msg = u8"已取消修改快捷键";
    return;
  }
  std::string err;
  if (!game_api::set_hotkey_slot(slot, found, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                                 (GetAsyncKeyState(VK_MENU) & 0x8000) != 0,
                                 (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, &err)) {
    g_last_msg = err.empty() ? std::string(u8"快捷键没有改成") : err;
    game_api::beep("error");
    return;
  }
  char shown[48] = {};
  game_api::format_hotkey_slot(slot, shown, sizeof(shown));
  g_last_msg = std::string(u8"快捷键已保存 ") + shown;
  mark_settings_dirty();
  game_api::beep("click");
}

static bool draw_gear_button(const char* id, const char* tip, bool* right_click) {
  ImGui::PushID(id);
  const float s = ImGui::GetFrameHeight();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##gear", ImVec2(s, s));
  if (right_click) *right_click = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p0.x + s * 0.5f, p0.y + s * 0.5f);
  const float r = s * 0.22f;
  const ImU32 col = hov ? SK().accent2 : skin::Fade(SK().text2, 0.92f);
  if (hov) paint::Glow(dl, c, s * 0.40f, SK().accentGlow, 3);
  dl->AddCircle(c, r, col, 16, 1.7f);
  dl->AddCircleFilled(c, r * 0.42f, col, 12);
  for (int i = 0; i < 8; ++i) {
    const float a = (float)i * 0.78539816f;
    const float cs = std::cos(a);
    const float sn = std::sin(a);
    dl->AddLine(ImVec2(c.x + cs * (r * 0.72f), c.y + sn * (r * 0.72f)),
                ImVec2(c.x + cs * (r + s * 0.16f), c.y + sn * (r + s * 0.16f)), col, 2.2f);
  }
  if (hov && tip && tip[0]) Tip("%s", tip);
  ImGui::PopID();
  return pressed;
}

static bool draw_switch(const char* id, bool on, bool enabled) {
  ImGui::PushID(id);
  const float h = ImGui::GetFrameHeight() * 0.86f;
  const float w = h * 1.9f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##sw", ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImGuiID sid = ImGui::GetItemID();
  const float hv = AnimTo(sid, hov, 18.f);
  // 开/关也要过渡：滑块滑过去、轨道渐亮，而不是瞬间跳到另一侧。
  const float tv = AnimTo(sid + 1u, on, 20.f);
  const ImVec2 p1(p0.x + w, p0.y + h);

  PlateSpec ps;
  if (!enabled) {
    ps.top = skin::WithA(skin::Over(SK().card1, SK().win1), 150);
    ps.bot = ps.top;
    ps.edge = skin::Fade(SK().text3, 0.40f);
  } else {
    const ImU32 off = skin::WithA(skin::Over(SK().card0, SK().win1), 235);
    const ImU32 on_c = skin::Mix(SK().win1, SK().accent, 0.62f + 0.14f * hv);
    ps.top = skin::Mix(off, on_c, tv);
    ps.bot = skin::Mix(skin::Darken(off, 0.25f), skin::Mix(on_c, SK().accent, 0.35f), tv);
    ps.edge = skin::Mix(skin::Fade(SK().text3, 0.55f), skin::Fade(SK().accent2, 0.90f), tv);
    ps.inner = skin::Fade(SK().cardInnerHi, 0.10f + 0.10f * tv);
  }
  ps.shadow = skin::Fade(SK().shadow, 0.55f);
  ps.dy = ui_px(1.f);
  ps.sigma = ui_px(2.f);
  ps.th = 1.3f;
  // 轨道用胶囊圆角；切角皮肤下 paint 会改用八角形，看得到棱角。
  DrawPlate(dl, p0, p1, h * 0.5f, ps);

  const float pad = 3.f;
  const float kr = (h - pad * 2.f) * 0.5f;
  const float kx = p0.x + pad + kr + (w - pad * 2.f - kr * 2.f) * tv;
  const ImVec2 kc(kx, p0.y + h * 0.5f);
  const ImU32 knob = !enabled ? skin::Fade(SK().text3, 0.85f)
                              : skin::Mix(skin::Fade(SK().knob1, 0.92f), SK().knob0, tv);
  if (enabled && tv > 0.02f) {
    paint::Glow(dl, kc, kr * 1.4f, skin::Fade(SK().accentGlow, 0.55f * tv), 3);
  }
  paint::Knob(dl, kc, kr, knob, skin::Darken(knob, 0.22f), SK().shadow, ui_px(1.f),
              ui_px(2.2f), SK().shadowK * 0.9f);
  if (hov) Tip("%s", !enabled ? u8"请先注入" : on ? u8"点击关闭" : u8"点击开启");
  ImGui::PopID();
  return enabled && pressed;
}

static void draw_feature_gear(const char* feature) {
  int slots[4] = {};
  const int n = game_api::hotkey_slots_for_feature(feature, slots, 4);
  if (n <= 0) return;
  char tip[160] = {};
  if (n == 1 && g_capture_slot == slots[0]) {
    std::snprintf(tip, sizeof(tip), u8"请按下新的快捷键，Esc 取消");
  } else if (n == 1) {
    char combo[48] = {};
    game_api::format_hotkey_slot(slots[0], combo, sizeof(combo));
    std::snprintf(tip, sizeof(tip), u8"修改快捷键\n当前 %s\n右键恢复默认",
                  combo[0] ? combo : u8"未设置");
  } else {
    std::snprintf(tip, sizeof(tip), u8"修改快捷键");
  }
  bool right = false;
  if (draw_gear_button(feature, tip, &right)) {
    if (n == 1) begin_hotkey_capture(slots[0]);
    else ImGui::OpenPopup("##hkpop");
  }
  if (n == 1 && right) {
    if (game_api::reset_hotkey_slot(slots[0])) {
      char combo[48] = {};
      game_api::format_hotkey_slot(slots[0], combo, sizeof(combo));
      g_last_msg = std::string(u8"已恢复默认快捷键 ") + combo;
      mark_settings_dirty();
    }
  }
  if (ImGui::BeginPopup("##hkpop")) {
    ImGui::TextUnformatted(u8"修改快捷键");
    for (int i = 0; i < n; ++i) {
      const char* id = game_api::hotkey_slot_id(slots[i]);
      char combo[48] = {};
      game_api::format_hotkey_slot(slots[i], combo, sizeof(combo));
      char line[96] = {};
      std::snprintf(line, sizeof(line), "%s    %s", danger_slot_label(id),
                    combo[0] ? combo : u8"未设置");
      if (ImGui::Button(line, ImVec2(button_width(line), 0))) {
        begin_hotkey_capture(slots[i]);
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

static void run_feature_action(const game_api::FeatureInfo* f) {
  if (!f) return;
  if (std::strcmp(f->type, "pulse") == 0) {
    std::string msg;
    const bool ok = game_api::pulse_feature(f->key, &msg);
    g_last_msg = msg;
    game_api::beep(ok ? "click" : "error");
    return;
  }
  if (std::strcmp(f->type, "engine") == 0) {
    if (start_engine_async(f->key)) {
      g_last_msg = u8"正在执行…（请把鼠标移到地形上）";
    } else {
      g_last_msg = u8"上一次操作仍在进行中";
      game_api::beep("error");
    }
    return;
  }
  if (std::strcmp(f->type, "money") == 0) {
    std::string msg;
    const bool ok = game_api::adjust_local_money(game_api::money_self_step(), &msg);
    g_last_msg = msg;
    game_api::beep(ok ? "click" : "error");
  }
}

static void draw_hotkey_row(const game_api::FeatureInfo* f, bool armed, bool as_switch,
                            bool action_enabled) {
  end_chip_flow();
  ImGui::PushID(f->key);
  const float gear = ImGui::GetFrameHeight();
  const float gap = 8.f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float action_w = as_switch ? gear * 1.64f : button_width(u8"执行");
  const float name_w = std::max(24.f, avail - gear - action_w - gap * 2.f);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float row_h = gear;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  {
    PlateSpec ps;
    ps.top = PanelBg();
    ps.bot = skin::Mix(PanelBg(), SK().win1, 0.40f);
    ps.edge = EdgeIdle();
    ps.inner = skin::Fade(SK().cardInnerHi, 0.05f);
    ps.th = 1.f;
    DrawPlate(dl, origin, ImVec2(origin.x + avail, origin.y + row_h),
              ui_px(SK().cardRadius * 0.5f), ps);
  }

  const ImVec2 ts = ImGui::CalcTextSize(f->label);
  dl->PushClipRect(ImVec2(origin.x + 4.f, origin.y), ImVec2(origin.x + name_w - 4.f, origin.y + row_h),
                   true);
  dl->AddText(ImVec2(origin.x + 12.f, origin.y + (row_h - ts.y) * 0.5f), SK().text,
              f->label);
  dl->PopClipRect();
  ImGui::Dummy(ImVec2(name_w, row_h));
  ImGui::SameLine(0.f, gap);
  draw_feature_gear(f->key);
  ImGui::SameLine(0.f, gap);
  if (as_switch) {
    const bool on = game_api::feature_enabled(f->key);
    if (draw_switch("sw", on, armed)) {
      std::string msg;
      const bool ok = game_api::toggle_feature(f->key, !on, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? (!on ? "on" : "off") : "error");
    }
  } else {
    if (!action_enabled) ImGui::BeginDisabled();
    if (ImGui::Button(u8"执行", ImVec2(action_w, 0))) run_feature_action(f);
    if (!action_enabled) ImGui::EndDisabled();
  }
  ImGui::PopID();
}

static bool feature_needs_hooks(const char* key) {
  if (!key) return false;
  return std::strcmp(key, "protocol_ready") == 0 || std::strcmp(key, "unit_skill_ready") == 0 ||
         std::strcmp(key, "disable_protocol") == 0 || std::strcmp(key, "unit_clone") == 0 ||
         std::strcmp(key, "spawn_unit") == 0 || std::strcmp(key, "clone_multi") == 0 ||
         std::strcmp(key, "ore_convoy") == 0 ||
         std::strcmp(key, "full_buff") == 0 || std::strcmp(key, "spawn_mcv") == 0 ||
         std::strcmp(key, "summon_troops") == 0;
}

static int g_pair_col = 0;
static int g_pair_cols = 2;
static float g_pair_w = 0.f;

static void end_pair_flow() { g_pair_col = 0; }

static void begin_grid_cell(int cols) {
  const float gap = 6.f;
  if (g_pair_col == 0) {
    const float avail = ImGui::GetContentRegionAvail().x;
    int use = cols < 1 ? 1 : cols;
    // 一格至少要放得下：名字（4 个中文字）＋ 齿轮 ＋「执行」按钮，放不下就降列数。
    // 这些宽度都跟着界面缩放走（字体 + FramePadding 都会放大），所以 1.5x / 2x 时
    // 列数会自动减少，而不是把最后一列挤到窗口外、把按钮里的字裁掉。
    const float min_name = ImGui::CalcTextSize(u8"伤害×5").x;
    const float min_cell =
        min_name + ImGui::GetFrameHeight() + button_width(u8"执行") + ui_px(18.f);
    while (use > 1 && avail < min_cell * (float)use + gap * (float)(use - 1)) --use;
    g_pair_cols = use;
    g_pair_w = use <= 1 ? avail : (avail - gap * (float)(use - 1)) / (float)use;
  } else {
    ImGui::SameLine(0.f, gap);
  }
}

static void end_grid_cell() {
  g_pair_col = g_pair_cols <= 1 ? 0 : (g_pair_col + 1) % g_pair_cols;
}

static void draw_compact_cell(const game_api::FeatureInfo* f, int cols) {
  if (!f) return;
  const bool as_switch = std::strcmp(f->type, "toggle") == 0;
  begin_grid_cell(cols);

  const bool armed = game_api::hooks_armed();
  const bool busy = InterlockedCompareExchange(&g_engine_busy, 0, 0) != 0;
  const bool action_ok = as_switch ? armed
                                   : ((!(feature_needs_hooks(f->key) && !armed)) && !busy);
  int slots[1] = {};
  const bool has_hk = game_api::hotkey_slots_for_feature(f->key, slots, 1) > 0;
  const float row_h = ImGui::GetFrameHeight();
  const float sw_h = row_h * 0.86f;
  const float action_w = as_switch ? sw_h * 1.9f : button_width(u8"执行");
  const float gear_w = has_hk ? row_h : 0.f;
  const float inner = 4.f;
  const float name_w =
      std::max(20.f, g_pair_w - gear_w - action_w - inner * (has_hk ? 2.f : 1.f));

  ImGui::PushID(f->key);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool on = as_switch && game_api::feature_enabled(f->key);
  {
    // 行底板：开着的行整块染 accent，未开的行只给一层极淡的高光。
    PlateSpec ps;
    const float r = ui_px(SK().cardRadius * 0.38f);
    ps.top = on ? skin::Mix(PanelBg(), SK().accent, 0.30f) : PanelBg();
    ps.bot = on ? skin::Mix(PanelBg(), SK().accent, 0.20f) : skin::Mix(PanelBg(), SK().win1, 0.35f);
    ps.edge = on ? skin::Mix(EdgeIdle(), SK().accent, 0.72f) : EdgeIdle();
    ps.inner = skin::Fade(SK().cardInnerHi, on ? 0.10f : 0.05f);
    ps.th = 1.f;
    DrawPlate(dl, origin, ImVec2(origin.x + g_pair_w, origin.y + row_h), r, ps);
  }

  const bool name_click = ImGui::InvisibleButton("##name", ImVec2(name_w, row_h));
  if (ImGui::IsItemHovered()) {
    const char* hk = game_api::hotkey_hint(f->key);
    if (hk && hk[0]) Tip("%s\n%s", f->label, hk);
    else Tip("%s", f->label);
  }
  const ImVec2 ts = ImGui::CalcTextSize(f->label);
  dl->PushClipRect(ImVec2(origin.x + 2.f, origin.y), ImVec2(origin.x + name_w - 2.f, origin.y + row_h),
                   true);
  dl->AddText(ImVec2(origin.x + 8.f, origin.y + (row_h - ts.y) * 0.5f),
              action_ok ? SK().text : skin::Fade(SK().text3, 0.90f), f->label);
  dl->PopClipRect();
  if (has_hk) {
    ImGui::SameLine(0.f, inner);
    draw_feature_gear(f->key);
  }
  ImGui::SameLine(0.f, inner);
  if (as_switch) {
    if (draw_switch("sw", on, armed) || (name_click && armed)) {
      std::string msg;
      const bool ok = game_api::toggle_feature(f->key, !on, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? (!on ? "on" : "off") : "error");
    }
  } else {
    if (!action_ok) ImGui::BeginDisabled();
    const bool run = ImGui::Button(u8"执行", ImVec2(action_w, row_h)) || (name_click && action_ok);
    if (!action_ok) ImGui::EndDisabled();
    if (run) run_feature_action(f);
  }
  ImGui::PopID();

  end_grid_cell();
}

static void draw_money_cell(const game_api::FeatureInfo* f, int cols) {
  if (!f) return;
  const bool sel = std::strcmp(f->type, "money_sel") == 0;
  begin_grid_cell(cols);
  int step = sel ? game_api::money_sel_step() : game_api::money_self_step();
  int slots[1] = {};
  const bool has_hk = game_api::hotkey_slots_for_feature(f->key, slots, 1) > 0;
  const float row_h = ImGui::GetFrameHeight();
  const float btn_w = button_width("+");
  const float gear_w = has_hk ? row_h : 0.f;
  const float inner = 4.f;
  const float name_w =
      std::max(20.f, g_pair_w - gear_w - btn_w * 2.f - 2.f - inner * (has_hk ? 2.f : 1.f));

  ImGui::PushID(f->key);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  {
    PlateSpec ps;
    const float r = ui_px(SK().cardRadius * 0.38f);
    ps.top = PanelBg();
    ps.bot = skin::Mix(PanelBg(), SK().win1, 0.40f);
    ps.edge = EdgeIdle();
    ps.inner = skin::Fade(SK().cardInnerHi, 0.05f);
    ps.th = 1.f;
    DrawPlate(dl, origin, ImVec2(origin.x + g_pair_w, origin.y + row_h), r, ps);
  }

  const bool name_click = ImGui::InvisibleButton("##name", ImVec2(name_w, row_h));
  const bool name_right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  if (ImGui::IsItemHovered()) {
    const char* hk = game_api::hotkey_hint(f->key);
    if (sel) {
      if (hk && hk[0]) {
        Tip(u8"%s\n每次 %d\n%s\n点击名称修改每次金额", f->label, step, hk);
      } else {
        Tip(u8"%s\n每次 %d\n点击名称修改每次金额", f->label, step);
      }
    } else if (hk && hk[0]) {
      Tip(u8"%s\n每次 %d\n%s\n点击名称修改每次金额", f->label, step, hk);
    } else {
      Tip(u8"%s\n每次 %d\n点击名称修改每次金额", f->label, step);
    }
  }
  if (name_click || name_right) ImGui::OpenPopup("##step");
  const ImVec2 ts = ImGui::CalcTextSize(f->label);
  dl->PushClipRect(ImVec2(origin.x + 2.f, origin.y), ImVec2(origin.x + name_w - 2.f, origin.y + row_h),
                   true);
  dl->AddText(ImVec2(origin.x + 8.f, origin.y + (row_h - ts.y) * 0.5f), SK().text,
              f->label);
  dl->PopClipRect();
  if (ImGui::BeginPopup("##step")) {
    ImGui::TextUnformatted(u8"每次金额");
    ImGui::SetNextItemWidth(150.f);
    if (ImGui::InputInt("##amt", &step, 10000, 100000)) {
      if (sel) game_api::set_money_sel_step(step);
      else game_api::set_money_self_step(step);
      mark_settings_dirty();
    }
    ImGui::EndPopup();
  }
  if (has_hk) {
    ImGui::SameLine(0.f, inner);
    draw_feature_gear(f->key);
  }
  ImGui::SameLine(0.f, inner);
  if (ImGui::Button("+", ImVec2(btn_w, row_h))) {
    std::string msg;
    const bool ok = sel ? game_api::adjust_selected_player_money(game_api::money_sel_step(), &msg)
                        : game_api::adjust_local_money(game_api::money_self_step(), &msg);
    g_last_msg = msg;
    game_api::beep(ok ? "click" : "error");
  }
  if (ImGui::IsItemHovered()) {
    Tip(u8"+ %d", sel ? game_api::money_sel_step() : game_api::money_self_step());
  }
  ImGui::SameLine(0.f, 2.f);
  if (ImGui::Button("-", ImVec2(btn_w, row_h))) {
    std::string msg;
    const int d = sel ? game_api::money_sel_step() : game_api::money_self_step();
    const bool ok = sel ? game_api::adjust_selected_player_money(-d, &msg)
                        : game_api::adjust_local_money(-d, &msg);
    g_last_msg = msg;
    game_api::beep(ok ? "click" : "error");
  }
  if (ImGui::IsItemHovered()) {
    Tip(u8"- %d", sel ? game_api::money_sel_step() : game_api::money_self_step());
  }
  ImGui::PopID();
  end_grid_cell();
}

static bool draw_mcv_choice(const char* id, const char* type_id, const char* tip, bool selected) {
  const float s = ui_px(56.f);
  ImGui::PushID(id);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##mcv", ImVec2(s, s));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float hv = AnimTo(ImGui::GetItemID(), hov, 18.f);
  const float sel = selected ? 1.f : 0.f;
  const ImVec2 p1(p0.x + s, p0.y + s);
  {
    PlateSpec ps;
    ps.top = skin::Mix(PanelBg(), SK().accent, 0.30f * sel + 0.10f * hv);
    ps.bot = skin::Mix(PanelBg(), SK().win1, 0.30f - 0.20f * sel);
    ps.edge = skin::Mix(EdgeIdle(), skin::Mix(SK().accent, SK().accent2, sel),
                        0.62f * hv + 0.38f * sel);
    ps.inner = skin::Fade(SK().cardInnerHi, selected ? 0.12f : 0.06f);
    ps.shadow = selected ? skin::Fade(SK().shadow, 0.7f) : 0;
    ps.dy = ui_px(1.f);
    ps.sigma = ui_px(2.5f);
    ps.th = selected ? 1.8f : 1.f;
    DrawPlate(dl, p0, p1, ui_px(SK().cardRadius * 0.48f), ps);
  }
  IDirect3DTexture9* tex = unit_icons::get(type_id, "MCV", false);
  if (tex) {
    dl->AddImage((ImTextureID)tex, ImVec2(p0.x + 4.f, p0.y + 4.f), ImVec2(p0.x + s - 4.f, p0.y + s - 4.f));
  }
  if (selected) {
    const ImVec2 dot(p0.x + s - 10.f, p0.y + 10.f);
    paint::Glow(dl, dot, 4.f, SK().accentGlow, 3);
    dl->AddCircleFilled(dot, 3.5f, SK().accent2);
  }
  if (hov) Tip("%s", tip);
  ImGui::PopID();
  return pressed;
}

static int draw_danger_slider(int level, bool enabled, float width) {
  if (level < 0 || level > 2) level = 0;
  static const char* names[3] = {u8"正常", u8"高", u8"最高"};
  const float h = ImGui::GetFrameHeight();
  const float w = std::max(48.f, width);

  ImGui::PushID("danger_slider");
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##sl", ImVec2(w, h));
  const bool hot = enabled && (ImGui::IsItemActive() || ImGui::IsItemClicked());
  int picked = level;
  if (hot && w > 8.f) {
    float t = (ImGui::GetIO().MousePos.x - origin.x) / w;
    if (t < 0.f) t = 0.f;
    if (t > 0.999f) t = 0.999f;
    picked = (int)(t * 3.f);
    if (picked < 0) picked = 0;
    if (picked > 2) picked = 2;
  }
  if (ImGui::IsItemHovered()) {
    Tip(u8"拖动选择：正常 / 高 / 最高");
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rnd = h * 0.45f;
  const float seg = w / 3.f;
  ImU32 accent = skin::Fade(SK().text3, 0.62f);
  if (enabled) {
    accent = picked == 2 ? SK().bad : picked == 1 ? SK().warn : SK().accent2;
  }
  // 轨道：整条凹槽。形状随皮肤（圆角 / 切角），描边用当前档位的语义色。
  {
    PlateSpec ps;
    ps.top = FieldBg();
    ps.bot = skin::Mix(FieldBg(), SK().win1, 0.35f);
    ps.edge = enabled ? skin::Fade(accent, 0.90f) : skin::Fade(SK().text3, 0.55f);
    ps.inner = skin::Fade(SK().cardInnerLo, 0.55f);
    ps.th = 1.2f;
    DrawPlate(dl, origin, ImVec2(origin.x + w, origin.y + h), rnd, ps);
  }
  // 选中段：内缩一圈单独画，避免用 ImDrawFlags 圆角标志
  //（切角形状下那套标志无效，会露出直角）。
  {
    const float inset = 2.f;
    const float xL = origin.x + seg * (float)picked + inset;
    const float xR = xL + seg - inset * 2.f;
    PlateSpec ps;
    ps.top = skin::Fade(accent, enabled ? 0.42f : 0.22f);
    ps.bot = skin::Fade(accent, enabled ? 0.26f : 0.13f);
    ps.edge = skin::Fade(accent, enabled ? 0.80f : 0.30f);
    ps.th = 1.f;
    DrawPlate(dl, ImVec2(xL, origin.y + inset), ImVec2(xR, origin.y + h - inset),
              std::max(0.f, rnd - inset), ps);
  }
  for (int i = 0; i < 3; ++i) {
    const ImVec2 ts = ImGui::CalcTextSize(names[i]);
    const float tx = origin.x + seg * ((float)i + 0.5f) - ts.x * 0.5f;
    const ImU32 tc = (enabled && i == picked) ? SK().text : skin::Fade(SK().text2, 0.84f);
    dl->AddText(ImVec2(tx, origin.y + (h - ts.y) * 0.5f), tc, names[i]);
  }
  ImGui::PopID();
  return picked;
}

static int draw_rank_slider(int level, bool* dragging) {
  if (level < 0 || level > 3) level = 0;
  static const char* names[4] = {u8"不设置", u8"一级", u8"二级", u8"三级"};
  const float w = std::max(48.f, ImGui::GetContentRegionAvail().x);
  const float text_h = ImGui::GetTextLineHeight();
  const float grab_r = 9.f;
  const float track_y = grab_r + 2.f;
  const float h = track_y + grab_r + 6.f + text_h;
  const float pad = grab_r + 4.f;

  ImGui::PushID("spawn_rank_slider");
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##sl", ImVec2(w, h));
  const bool hot = ImGui::IsItemActive() || ImGui::IsItemClicked();
  if (dragging) *dragging = ImGui::IsItemActive();
  int picked = level;
  if (hot && w > pad * 2.f) {
    const float t = (ImGui::GetIO().MousePos.x - (origin.x + pad)) / (w - pad * 2.f);
    const float clamped = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    picked = (int)(clamped * 3.f + 0.5f);
    if (picked < 0) picked = 0;
    if (picked > 3) picked = 3;
  }
  if (ImGui::IsItemHovered()) {
    Tip(u8"拖动选择出场等级：不设置 / 一级 / 二级 / 三级");
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = origin.y + track_y;
  const float x0 = origin.x + pad;
  const float x1 = origin.x + w - pad;
  const float span = std::max(1.f, x1 - x0);
  const float gx = x0 + span * ((float)picked / 3.f);
  ImU32 accent = skin::Fade(SK().text3, 0.88f);
  if (picked == 1) accent = SK().accent2;
  else if (picked == 2) accent = SK().warn;
  else if (picked == 3) accent = SK().bad;
  const float track_h = 7.f;
  {
    // 轨道凹槽 + 已选区间的亮条。用 plate 而不是 AddRectFilled，
    // 切角皮肤下轨道两端也是斜的，不会突然冒出直角。
    PlateSpec ps;
    ps.top = FieldBg();
    ps.bot = skin::Mix(FieldBg(), SK().win1, 0.40f);
    ps.edge = skin::Fade(EdgeIdle(), 0.80f);
    ps.inner = skin::Fade(SK().cardInnerLo, 0.45f);
    ps.th = 1.f;
    DrawPlate(dl, ImVec2(x0, cy - track_h * 0.5f), ImVec2(x1, cy + track_h * 0.5f),
              track_h * 0.5f, ps);

    if (gx > x0 + 1.f) {
      PlateSpec f;
      f.top = skin::Fade(accent, 0.85f);
      f.bot = skin::Darken(accent, 0.25f);
      f.edge = skin::Fade(accent, 0.55f);
      f.th = 0.8f;
      DrawPlate(dl, ImVec2(x0, cy - track_h * 0.5f), ImVec2(gx, cy + track_h * 0.5f),
                track_h * 0.5f, f);
    }
  }
  for (int i = 0; i < 4; ++i) {
    const float tx = x0 + span * ((float)i / 3.f);
    const bool lit = i <= picked;
    dl->AddCircleFilled(ImVec2(tx, cy), i == picked ? 3.2f : 2.2f,
                        lit ? accent : skin::Fade(SK().text3, 0.55f));
    const ImVec2 ts = ImGui::CalcTextSize(names[i]);
    const ImU32 tc = i == picked ? accent : skin::Fade(SK().text2, 0.84f);
    dl->AddText(ImVec2(tx - ts.x * 0.5f, origin.y + track_y + grab_r + 4.f), tc, names[i]);
  }
  paint::Shadow(dl, ImVec2(gx - grab_r, cy - grab_r), ImVec2(gx + grab_r, cy + grab_r), grab_r,
                SK().shadow, 1.5f, 3.f, SK().shadowK);
  dl->AddCircleFilled(ImVec2(gx, cy), grab_r, ChipBg());
  dl->AddCircleFilled(ImVec2(gx, cy), grab_r - 3.2f, accent);
  dl->AddCircle(ImVec2(gx, cy), grab_r, skin::Fade(SK().cardInnerHi, 0.90f), 0, 1.4f);
  ImGui::PopID();
  return picked;
}

static uint32_t g_rank_player_ui = 0;
static uint32_t g_mcv_player_ui = 0;
static uint32_t g_money_player_ui = 0;
static uint32_t g_summon_player_ui = 0;
static int g_summon_count = 1;
static std::vector<std::string> g_summon_sel;

static bool summon_is_picked(const char* id) {
  if (!id) return false;
  for (const auto& s : g_summon_sel) {
    if (s == id) return true;
  }
  return false;
}

static void summon_add(const char* id) {
  if (!id || !id[0] || summon_is_picked(id)) return;
  if (g_summon_sel.size() < 64) g_summon_sel.push_back(id);
}

static bool draw_summon_icon(const char* tip, const char* icon_id, const char* icon_name,
                             bool removable) {
  const float kIcon = ui_px(40.f);
  const float inset = ui_px(2.f);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##ico", ImVec2(kIcon, kIcon));
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  {
    PlateSpec ps;
    const float hvk = AnimTo(ImGui::GetItemID(), hovered, 18.f);
    ps.top = skin::Mix(PanelBg(), SK().money, 0.08f * hvk);
    ps.bot = skin::Mix(PanelBg(), SK().win1, 0.30f);
    ps.edge = skin::Mix(EdgeIdle(), SK().money, hvk);
    ps.inner = skin::Fade(SK().cardInnerHi, 0.06f);
    ps.th = hovered ? 1.8f : 1.f;
    DrawPlate(draw, p0, ImVec2(p0.x + kIcon, p0.y + kIcon), ui_px(SK().cardRadius * 0.30f), ps);
  }
  IDirect3DTexture9* tex = nullptr;
  if (icon_id && icon_id[0]) tex = unit_icons::get(icon_id, icon_name, false);
  if (tex) {
    draw->AddImage((ImTextureID)tex, ImVec2(p0.x + inset, p0.y + inset),
                   ImVec2(p0.x + kIcon - inset, p0.y + kIcon - inset));
  } else {
    // 战役 / 特殊单位没有打包图标：用名字占位，否则只剩一个空方块认不出。
    char label[24] = {};
    const char* src = (tip && tip[0]) ? tip : (icon_id ? icon_id : "?");
    size_t taken = 0;
    if ((unsigned char)src[0] < 0x80) {
      for (size_t i = 0; src[i] && i < 6; ++i) label[taken++] = src[i];  // 英文类型名
    } else {
      int glyphs = 0;
      for (size_t i = 0; src[i] && glyphs < 3;) {                       // 中文名最多 3 个字
        const unsigned char c = (unsigned char)src[i];
        size_t step = (c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : ((c >> 3) == 30 ? 4 : 1));
        if (taken + step > sizeof(label) - 1) break;
        std::memcpy(label + taken, src + i, step);
        taken += step;
        i += step;
        ++glyphs;
      }
    }
    label[taken] = 0;
    const ImVec2 ts = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(p0.x + (kIcon - ts.x) * 0.5f, p0.y + (kIcon - ts.y) * 0.5f),
                  hovered ? SK().money : skin::Fade(SK().text, 0.88f), label);
  }
  if (hovered && tip && tip[0]) {
    TipBegin();
    ImGui::TextUnformatted(tip);
    if (icon_id && icon_id[0] && tip != icon_id) ImGui::TextDisabled("%s", icon_id);
    if (removable) ImGui::TextDisabled(u8"点击拿掉");
    TipEnd();
  }
  return clicked;
}

static int build_faction_rank_ui(const char* id);
static const char* build_faction_label(int rank);
static bool is_visible_rep(const game_api::BuildLockEntry* cat, int count, int index);

struct SummonExtra {
  const char* id;
  const char* name;
};

// 「特殊单位」：建造面板里没有、但对象模板库里登记着的对象。
// 引擎的 CreateUnit 对它们一样有效（Ra3-Copilot 就是靠模板库按哈希找出来召唤的），
// 这里按类型名找模板（find_template_by_id 走的也是同一张表）。
static const SummonExtra kSummonExtra[] = {
    // 帝国战役 / 起义时刻
    {"JapanMechaKing", u8"将军刽子手"},
    {"JapanEmperorMecha", u8"天皇芳朗（红鬼王）"},
    {"JapanEmperorMecha02", u8"天皇机甲·改"},
    {"JapanSentinelVehicle", u8"钢铁浪人"},
    {"JapanGigaFortress_Land", u8"超级要塞（陆）"},
    {"JapanFortressShip", u8"超级要塞（海）"},
    {"JapanCommandoTech1_Complete", u8"欧米茄百合子（完整）"},
    {"JapanAntiVehicleVehicleTech3_Movie", u8"鬼王（过场）"},
    {"JapanRadarShip", u8"帝国雷达舰"},
    {"JapanBalloonBombBalloon", u8"气球炸弹"},
    // 苏联 / 盟军的非建造对象
    {"SovietAntiVehicleVehicleTech2", u8"磁暴坦克"},
};static bool summon_is_extra(const char* id) {
  if (!id) return false;
  for (const auto& e : kSummonExtra) {
    if (std::strcmp(id, e.id) == 0) return true;
  }
  return false;
}

static const game_api::BuildLockEntry* summon_extra_entry(const std::string& id) {
  static game_api::BuildLockEntry row;
  for (const auto& e : kSummonExtra) {
    if (id != e.id) continue;
    row = {};
    std::snprintf(row.type_id, sizeof(row.type_id), "%s", e.id);
    std::snprintf(row.name, sizeof(row.name), "%s", e.name);
    std::snprintf(row.icon_id, sizeof(row.icon_id), "%s", e.id);
    std::snprintf(row.icon_name, sizeof(row.icon_name), "%s", e.name);
    return &row;
  }
  return nullptr;
}

static const game_api::BuildLockEntry* summon_lookup(const game_api::BuildLockEntry* cat, int count,
                                                     const std::string& id) {
  if (cat) {
    for (int i = 0; i < count; ++i) {
      if (id == cat[i].type_id) return &cat[i];
    }
  }
  return summon_extra_entry(id);
}

static void draw_summon_candidate_box(const game_api::BuildLockEntry* cat, int count) {
  // 尺寸跟着界面缩放走，否则缩放调大后图标会比这里算的宽度还宽，右边被裁掉。
  const float kIcon = ui_px(40.f);
  const float kGap = ui_px(4.f);
  const float kPad = ui_px(8.f);

  ImGui::TextDisabled(u8"候选 %d", (int)g_summon_sel.size());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float full_w = ImGui::GetContentRegionAvail().x;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);

  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(full_w, kPad));
  ImGui::Indent(kPad);
  int remove_at = -1;
  if (g_summon_sel.empty()) {
    ImGui::TextDisabled(u8"点击下方部队加入，再点这里的图标可拿掉");
  } else {
    const float avail = ImGui::GetContentRegionAvail().x - kPad - ImGui::GetStyle().ScrollbarSize;
    float used = 0.f;
    bool started = false;
    for (size_t n = 0; n < g_summon_sel.size(); ++n) {
      const auto* e = summon_lookup(cat, count, g_summon_sel[n]);
      const char* zh = e == nullptr ? game_api::type_display_name(g_summon_sel[n].c_str())
                                    : nullptr;
      const char* tip = (e && e->name[0]) ? e->name
                                         : ((zh && zh[0]) ? zh : g_summon_sel[n].c_str());
      const char* icon_id = e ? (e->icon_id[0] ? e->icon_id : e->type_id) : "";
      const char* icon_name = e ? (e->icon_name[0] ? e->icon_name : e->name) : "";
      if (started && used + kIcon + kGap > avail) {
        used = 0.f;
        started = false;
      }
      if (started) ImGui::SameLine(0.f, kGap);
      ImGui::PushID((int)n + 40000);
      if (draw_summon_icon(tip, icon_id, icon_name, true)) remove_at = (int)n;
      ImGui::PopID();
      used += kIcon + kGap;
      started = true;
    }
  }
  ImGui::Unindent(kPad);
  ImGui::Dummy(ImVec2(1.f, kPad));
  ImGui::EndGroup();
  const ImVec2 r0 = ImGui::GetItemRectMin();
  const ImVec2 r1 = ImGui::GetItemRectMax();

  dl->ChannelsSetCurrent(0);
  {
    // 候选槽：内嵌的"料盘"，比普通行再凹一层（渐变反向 + 无投影）。
    PlateSpec ps;
    ps.top = skin::Mix(PanelBgAlt(), SK().win1, 0.45f);
    ps.bot = skin::Mix(PanelBgAlt(), SK().win1, 0.25f);
    ps.edge = skin::Fade(EdgeIdle(), 0.85f);
    ps.inner = skin::Fade(SK().cardInnerLo, 0.55f);
    ps.th = 1.2f;
    DrawPlate(dl, r0, ImVec2(r0.x + full_w, r1.y), ui_px(SK().cardRadius * 0.42f), ps);
  }
  dl->ChannelsMerge();
  if (remove_at >= 0 && remove_at < (int)g_summon_sel.size()) {
    g_summon_sel.erase(g_summon_sel.begin() + remove_at);
  }
  ImGui::Spacing();
}

// 某阵营 + 单位/建筑 这一组里有没有可点的图标。
static bool summon_row_has(const game_api::BuildLockEntry* cat, int count, int fac,
                           bool buildings) {
  for (int i = 0; i < count; ++i) {
    const auto& e = cat[i];
    if (e.building != buildings) continue;
    if (summon_is_extra(e.type_id)) continue;
    if (build_faction_rank_ui(e.type_id) != fac) continue;
    if (!is_visible_rep(cat, count, i)) continue;
    return true;
  }
  return false;
}

// 召唤图标一行：按阵营 + 单位/建筑筛出条目，按可用宽度换行。
// 尺寸统一走 ui_px：以前这里写死 40/4，界面缩放调大后最右边那列会被面板裁掉。
static void draw_summon_icon_row(const game_api::BuildLockEntry* cat, int count, int fac,
                                 bool buildings, int id_base) {
  const float kIcon = ui_px(40.f);
  const float kGap = ui_px(4.f);
  // 预留右侧滚动条 + 一点余量：内容一多面板就会出现竖向滚动条，可绘制宽度
  // 随之变窄，不预留的话最右边那列会被面板边缘裁掉。
  const float avail =
      ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - kGap;
  float used = 0.f;
  bool started = false;
  for (int i = 0; i < count; ++i) {
    const auto& e = cat[i];
    if (e.building != buildings) continue;
    if (summon_is_extra(e.type_id)) continue;
    if (build_faction_rank_ui(e.type_id) != fac) continue;
    if (!is_visible_rep(cat, count, i)) continue;
    if (started && used + kIcon + kGap > avail) {
      used = 0.f;
      started = false;
    }
    if (started) ImGui::SameLine(0.f, kGap);
    const char* icon_id = e.icon_id[0] ? e.icon_id : e.type_id;
    const char* icon_name = e.icon_name[0] ? e.icon_name : e.name;
    ImGui::PushID(i + id_base);
    if (draw_summon_icon(e.name[0] ? e.name : e.type_id, icon_id, icon_name, false)) {
      summon_add(e.type_id);
    }
    ImGui::PopID();
    used += kIcon + kGap;
    started = true;
  }
}

// 子串匹配（忽略 ASCII 大小写）：搜索对象类型名用。
static bool summon_substr_ci(const char* s, const char* q) {
  if (!q || !q[0]) return true;
  if (!s) return false;
  for (const char* p = s; *p; ++p) {
    const char* a = p;
    const char* b = q;
    while (*a && *b) {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb) break;
      ++a;
      ++b;
    }
    if (!*b) return true;
  }
  return false;
}

// 「按名字搜索」：直接列对象模板库里的类型名（含战役 / 特殊 / Mod 单位）。
// 找到后点一下就能加进候选框，和上面点图标是同一条召唤路径。
static void draw_summon_search() {
  static char s_query[64] = {};
  ImGui::SeparatorText(u8"搜索全部对象（战役 / 特殊 / Mod）");
  const std::vector<std::string>& all = game_api::object_type_names();
  if (all.empty()) {
    ImGui::TextDisabled(u8"还没读到对象模板库（进对局后再试）。");
    ImGui::SameLine();
    if (ImGui::SmallButton(u8"重新扫描")) game_api::refresh_object_type_names();
    return;
  }
  char head[96];
  std::snprintf(head, sizeof(head), u8"模板库共 %d 个对象类型", (int)all.size());
  ImGui::TextDisabled("%s", head);
  ImGui::SameLine();
  if (ImGui::SmallButton(u8"重新扫描##summon_rescan")) game_api::refresh_object_type_names();
  ImGui::SetNextItemWidth(-1.f);
  ImGui::InputTextWithHint("##summon_search",
                           u8"类型名或中文名片段，例如 Mecha / Oni / 将军 / 鬼王", s_query,
                           sizeof(s_query));
  if (strlen(s_query) < 2) {
    ImGui::TextDisabled(u8"输入 2 个字符以上开始筛选（模板库里包含建造面板之外的对象）。");
    return;
  }
  int shown = 0;
  for (const std::string& id : all) {
    const char* zh = game_api::type_display_name(id.c_str());
    if (!summon_substr_ci(id.c_str(), s_query) &&
        !(zh && zh[0] && summon_substr_ci(zh, s_query))) {
      continue;
    }
    char label[192];
    if (zh && zh[0]) {
      std::snprintf(label, sizeof(label), "%s  ·  %s##summon_hit", zh, id.c_str());
    } else {
      std::snprintf(label, sizeof(label), "%s##summon_hit", id.c_str());
    }
    if (ImGui::Selectable(label)) summon_add(id.c_str());
    ImGui::SetItemTooltip(u8"点击加入上方候选框");
    if (++shown >= 24) {
      ImGui::TextDisabled(u8"只列了前 %d 条，输入得更具体一些能缩小范围。", shown);
      break;
    }
  }
  if (shown == 0) ImGui::TextDisabled(u8"模板库里没有匹配的名字。");
}

static void draw_summon_unit_icons() {
  int count = 0;
  const game_api::BuildLockEntry* cat = game_api::build_lock_catalog(&count);
  draw_summon_candidate_box(cat, count);
  if (count <= 0 || !cat) {
    ImGui::TextDisabled(u8"还没从建造面板读到部队；下面的特殊 / 战役单位和搜索仍然可用。");
  }
  bool any_building = false;
  for (int i = 0; i < count && cat; ++i) {
    if (cat[i].building) {
      any_building = true;
      break;
    }
  }
  if (count > 0 && cat) {
    ImGui::SeparatorText(u8"单位");
    for (int fac = 0; fac < 4; ++fac) {
      if (!summon_row_has(cat, count, fac, false)) continue;
      ImGui::TextDisabled("%s", build_faction_label(fac));
      draw_summon_icon_row(cat, count, fac, false, 30000);
      ImGui::Spacing();
    }
    if (any_building) {
      ImGui::SeparatorText(u8"建筑");
      for (int fac = 0; fac < 4; ++fac) {
        if (!summon_row_has(cat, count, fac, true)) continue;
        ImGui::TextDisabled("%s", build_faction_label(fac));
        draw_summon_icon_row(cat, count, fac, true, 60000);
        ImGui::Spacing();
      }
    }
  }
  ImGui::SeparatorText(u8"特殊单位");
  {
    const float kIcon = ui_px(40.f);
    const float kGap = ui_px(4.f);
    const float avail =
        ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - kGap;
    const bool list_ready = game_api::object_type_list_ready();
    float used = 0.f;
    bool started = false;
    int absent = 0;
    for (int i = 0; i < (int)(sizeof(kSummonExtra) / sizeof(kSummonExtra[0])); ++i) {
      const auto& extra = kSummonExtra[i];
      const bool has = !list_ready || game_api::object_type_exists(extra.id);
      if (started && used + kIcon + kGap > avail) {
        used = 0.f;
        started = false;
      }
      if (started) ImGui::SameLine(0.f, kGap);
      ImGui::PushID(i + 50000);
      if (has) {
        if (draw_summon_icon(extra.name, extra.id, extra.name, false)) summon_add(extra.id);
      } else {
        ++absent;
        // 本版本模板库里没有这个对象：画成暗块，点不动，只留提示。
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ico_missing", ImVec2(kIcon, kIcon));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        {
          // 本版本没有的对象：不投影、不描亮，只留一个压暗的空位。
          PlateSpec ps;
          ps.top = skin::Fade(PanelBg(), 0.62f);
          ps.bot = ps.top;
          ps.edge = skin::Fade(EdgeIdle(), 0.55f);
          ps.th = 1.f;
          DrawPlate(draw, p0, ImVec2(p0.x + kIcon, p0.y + kIcon),
                    ui_px(SK().cardRadius * 0.30f), ps);
        }
        char label[24] = {};
        size_t taken = 0;
        for (size_t k = 0; extra.name[k] && taken < sizeof(label) - 1;) {
          const unsigned char c = (unsigned char)extra.name[k];
          const size_t step =
              (c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : ((c >> 3) == 30 ? 4 : 1));
          if (taken + step > sizeof(label) - 1) break;
          std::memcpy(label + taken, extra.name + k, step);
          taken += step;
          k += step;
        }
        label[taken] = 0;
        const ImVec2 ts = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(p0.x + (kIcon - ts.x) * 0.5f, p0.y + (kIcon - ts.y) * 0.5f),
                      skin::Fade(SK().text3, 0.90f), label);
        if (ImGui::IsItemHovered()) {
          TipBegin();
          ImGui::TextUnformatted(extra.name);
          ImGui::TextDisabled("%s", extra.id);
          ImGui::TextDisabled(u8"本版本的对象模板库里没有这个对象");
          TipEnd();
        }
      }
      ImGui::PopID();
      used += kIcon + kGap;
      started = true;
    }
    if (absent > 0) {
      ImGui::TextDisabled(u8"其中 %d 个在本版本不存在（暗格）。用下面的搜索可以查当前版本实际有的对象。",
                          absent);
    }
    if (ImGui::SmallButton(u8"把可用性写进日志##summon_diag")) {
      game_api::log("special units: factory list = %d names",
                    (int)game_api::object_type_names().size());
      for (const auto& extra : kSummonExtra) {
        game_api::log("special %-32s %s", extra.id,
                      game_api::object_type_exists(extra.id) ? "PRESENT" : "absent");
      }
    }
    ImGui::SetItemTooltip(u8"写进 ra3_overlay.log，出问题时可据此判断是版本没有还是引擎不造");
  }
  draw_summon_search();
  ImGui::Spacing();
}

static uint32_t draw_player_picker(const char* scope, uint32_t* selected) {
  static int cached_frame = -1;
  static int cached_n = 0;
  static game_api::BattlePlayer cached_seats[16];
  const int frame = ImGui::GetFrameCount();
  if (frame != cached_frame) {
    cached_frame = frame;
    cached_n = game_api::list_battle_players(cached_seats, 16);
  }
  game_api::BattlePlayer* seats = cached_seats;
  const int n = cached_n;
  if (n <= 0) {
    ImGui::TextDisabled(u8"尚未进入战局，没有可选玩家");
    if (selected) *selected = 0;
    return 0;
  }
  if (!selected) return 0;
  bool still = false;
  for (int i = 0; i < n; ++i) {
    if (seats[i].player == *selected) still = true;
  }
  if (!still) {
    *selected = seats[0].player;
    for (int i = 0; i < n; ++i) {
      if (seats[i].is_local) *selected = seats[i].player;
    }
  }
  for (int i = 0; i < n; ++i) {
    const auto& s = seats[i];
    char label[96];
    if (s.is_local && s.defeated) {
      std::snprintf(label, sizeof(label), u8"%s 己方 已击败", s.name[0] ? s.name : u8"玩家");
    } else if (s.is_local) {
      std::snprintf(label, sizeof(label), u8"%s 己方", s.name[0] ? s.name : u8"玩家");
    } else if (s.defeated) {
      std::snprintf(label, sizeof(label), u8"%s 已击败", s.name[0] ? s.name : u8"玩家");
    } else {
      std::snprintf(label, sizeof(label), "%s", s.name[0] ? s.name : u8"玩家");
    }
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float w = ts.x + 28.f;
    if (i > 0 && ImGui::GetContentRegionAvail().x > w + 8.f) ImGui::SameLine(0.f, 6.f);
    ImGui::PushID(scope);
    ImGui::PushID(i);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const bool pressed = ImGui::InvisibleButton("##seat", ImVec2(w, h));
    const bool on = s.player == *selected;
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hv = AnimTo(ImGui::GetItemID(), hov, 18.f);
    const float selk = on ? 1.f : 0.f;
    {
      PlateSpec ps;
      ps.top = skin::Mix(PanelBg(), SK().accent, 0.30f * selk + 0.08f * hv);
      ps.bot = skin::Mix(PanelBg(), SK().win1, 0.32f - 0.22f * selk);
      ps.edge = skin::Mix(EdgeIdle(), skin::Mix(SK().accent, SK().accent2, selk),
                          0.62f * hv + 0.38f * selk);
      ps.inner = skin::Fade(SK().cardInnerHi, on ? 0.10f : 0.05f);
      ps.th = on ? 1.6f : 1.f;
      DrawPlate(dl, p0, ImVec2(p0.x + w, p0.y + h), ui_px(SK().cardRadius * 0.34f), ps);
    }
    const ImU32 dot = s.has_color ? IM_COL32(s.color_r, s.color_g, s.color_b, 255)
                                  : (s.is_local ? SK().money : skin::Fade(SK().text2, 0.95f));
    dl->AddRectFilled(ImVec2(p0.x + 6.f, p0.y + (h - 12.f) * 0.5f),
                      ImVec2(p0.x + 18.f, p0.y + (h + 12.f) * 0.5f), dot, 2.f);
    dl->AddText(ImVec2(p0.x + 22.f, p0.y + (h - ts.y) * 0.5f), SK().text, label);
    ImGui::PopID();
    ImGui::PopID();
    if (pressed) *selected = s.player;
  }
  return *selected;
}

static void draw_feature_row(const game_api::FeatureInfo* f) {
  if (!f) return;
  const bool armed = game_api::hooks_armed();
  const char* hk = game_api::hotkey_hint(f->key);

  if (std::strcmp(f->type, "money_sel") == 0) {
    end_chip_flow();
    ImGui::PushID(f->key);
    ImGui::TextUnformatted(f->label);
    {
      int slots[1] = {};
      if (game_api::hotkey_slots_for_feature(f->key, slots, 1) > 0) {
        ImGui::SameLine();
        draw_feature_gear(f->key);
      }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(u8"选择玩家后加减该玩家的资金。");
    ImGui::PopStyleColor();
    const uint32_t who = draw_player_picker("money", &g_money_player_ui);
    game_api::set_money_player(who);
    int step = game_api::money_sel_step();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::InputInt(u8"每次##step", &step, 10000, 100000)) {
      game_api::set_money_sel_step(step);
      mark_settings_dirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(button_width("+"), 0))) {
      std::string msg;
      const bool ok = game_api::adjust_selected_player_money(game_api::money_sel_step(), &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    ImGui::SameLine();
    if (ImGui::Button("-", ImVec2(button_width("-"), 0))) {
      std::string msg;
      const bool ok = game_api::adjust_selected_player_money(-game_api::money_sel_step(), &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    ImGui::PopID();
    return;
  }

  if (std::strcmp(f->type, "money") == 0) {
    end_chip_flow();
    const bool sel = false;
    ImGui::PushID(f->key);
    int step = sel ? game_api::money_sel_step() : game_api::money_self_step();
    ImGui::TextUnformatted(f->label);
    {
      int slots[1] = {};
      if (game_api::hotkey_slots_for_feature(f->key, slots, 1) > 0) {
        ImGui::SameLine();
        draw_feature_gear(f->key);
      }
    }
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::InputInt(u8"每次##step", &step, 10000, 100000)) {
      if (sel) game_api::set_money_sel_step(step);
      else game_api::set_money_self_step(step);
      mark_settings_dirty();
    }
    char add_l[48], sub_l[48];
    std::snprintf(add_l, sizeof(add_l), u8"+ %d", sel ? game_api::money_sel_step()
                                                      : game_api::money_self_step());
    std::snprintf(sub_l, sizeof(sub_l), u8"- %d", sel ? game_api::money_sel_step()
                                                      : game_api::money_self_step());
    if (ImGui::Button(add_l, ImVec2(ui_px(140.f), 0))) {
      std::string msg;
      bool ok = sel ? game_api::adjust_selected_player_money(game_api::money_sel_step(), &msg)
                    : game_api::adjust_local_money(game_api::money_self_step(), &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    ImGui::SameLine();
    if (ImGui::Button(sub_l, ImVec2(ui_px(140.f), 0))) {
      std::string msg;
      const int d = sel ? game_api::money_sel_step() : game_api::money_self_step();
      bool ok = sel ? game_api::adjust_selected_player_money(-d, &msg)
                    : game_api::adjust_local_money(-d, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    ImGui::PopID();
    return;
  }

  if (std::strcmp(f->type, "toggle") == 0) {
    int slots[1] = {};
    if (game_api::hotkey_slots_for_feature(f->key, slots, 1) > 0) {
      draw_hotkey_row(f, armed, true, armed);
      return;
    }
    bool on = game_api::feature_enabled(f->key);
    if (!armed) ImGui::BeginDisabled();
    if (draw_hud_tile(f->key, f->label, hk, on, armed)) {
      on = !on;
      std::string msg;
      bool ok = game_api::toggle_feature(f->key, on, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? (on ? "on" : "off") : "error");
    }
    if (!armed) ImGui::EndDisabled();
    return;
  }

  if (std::strcmp(f->type, "pulse") == 0) {
    int slots[1] = {};
    if (game_api::hotkey_slots_for_feature(f->key, slots, 1) > 0) {
      draw_hotkey_row(f, armed, false, armed);
      return;
    }
    if (!armed) ImGui::BeginDisabled();
    if (draw_hud_tile(f->key, f->label, hk, false, armed)) {
      run_feature_action(f);
    }
    if (!armed) ImGui::EndDisabled();
    return;
  }

  if (std::strcmp(f->type, "danger") == 0) {
    end_chip_flow();
    ImGui::PushID(f->key);
    const float row_h = ImGui::GetFrameHeight();
    const float gap = 8.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts = ImGui::CalcTextSize(f->label);
    const float name_w = ts.x + 20.f;
    const float gear_w = row_h;
    const float slider_w = std::max(120.f, avail - name_w - gear_w - gap * 2.f - 8.f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
      PlateSpec ps;
      ps.top = PanelBg();
      ps.bot = skin::Mix(PanelBg(), SK().win1, 0.40f);
      ps.edge = EdgeIdle();
      ps.inner = skin::Fade(SK().cardInnerHi, 0.05f);
      ps.th = 1.f;
      DrawPlate(dl, origin, ImVec2(origin.x + avail, origin.y + row_h),
                ui_px(SK().cardRadius * 0.5f), ps);
    }
    dl->AddText(ImVec2(origin.x + 12.f, origin.y + (row_h - ts.y) * 0.5f),
                SK().text, f->label);
    ImGui::Dummy(ImVec2(name_w, row_h));
    ImGui::SameLine(0.f, gap);
    draw_feature_gear(f->key);
    ImGui::SameLine(0.f, gap);
    int level = (int)game_api::get_flag(0x13);
    if (level < 0 || level > 2) level = 0;
    const int picked = draw_danger_slider(level, armed, slider_w);
    ImGui::PopID();
    if (armed && picked != level) {
      std::string msg;
      bool ok = game_api::set_danger(picked, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    return;
  }

  if (std::strcmp(f->type, "spawn_rank") == 0) {
    end_chip_flow();
    ImGui::TextUnformatted(f->label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(u8"选择玩家后拖动滑条，设置该玩家之后生产单位的出场等级。0 为不设置，最高 3 级。");
    ImGui::PopStyleColor();
    const uint32_t who = draw_player_picker("rank", &g_rank_player_ui);
    game_api::set_rank_player(who);
    const int live = who ? game_api::player_spawn_rank(who) : -1;
    static int shown = 0;
    static uint32_t shown_for = 0;
    static bool was_drag = false;
    if (!was_drag && who != shown_for) {
      shown_for = who;
      if (live >= 0) shown = live;
    } else if (!was_drag && live >= 0) {
      shown = live;
    }
    bool dragging = false;
    const int picked = draw_rank_slider(who ? shown : 0, &dragging);
    was_drag = dragging;
    if (!who) {
      ImGui::TextDisabled(u8"进入战局后会列出全部玩家");
    } else if (live == -2) {
      ImGui::TextDisabled(u8"还没读到出场等级数据");
    }
    if (picked != shown) {
      if (!who || live < 0) {
        g_last_msg = !who ? u8"请先进入战局" : u8"找不到出场等级数据（请先进入战局）";
        game_api::beep("error");
      } else {
        std::string msg;
        const bool ok = game_api::set_spawn_rank(picked, &msg);
        g_last_msg = msg;
        game_api::beep(ok ? "click" : "error");
        if (ok) shown = picked;
      }
    }
    return;
  }

  if (std::strcmp(f->type, "engine") == 0 && std::strcmp(f->key, "spawn_mcv") == 0) {
    end_chip_flow();
    const bool busy = InterlockedCompareExchange(&g_engine_busy, 0, 0) != 0;
    const bool action_ok = armed && !busy;
    ImGui::PushID(f->key);
    ImGui::TextUnformatted(f->label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(u8"选择玩家和基地车阵营后召唤一辆，归该玩家。鼠标要放在地形上。");
    ImGui::PopStyleColor();
    const uint32_t who = draw_player_picker("mcv", &g_mcv_player_ui);
    game_api::set_mcv_player(who);
    const int fac = game_api::mcv_faction();
    const char* type_ids[3] = {"AlliedMCV", "SovietMCV", "JapanMCV"};
    const char* tips[3] = {u8"盟军基地车", u8"苏联基地车", u8"帝国基地车"};
    int picked = fac;
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine(0.f, 8.f);
      char id[16];
      std::snprintf(id, sizeof(id), "fac%d", i);
      if (draw_mcv_choice(id, type_ids[i], tips[i], fac == i)) picked = i;
    }
    if (picked != fac) {
      game_api::set_mcv_faction(picked);
      mark_settings_dirty();
    }
    ImGui::SameLine(0.f, 12.f);
    const float summon_y = ImGui::GetFrameHeight();
    if (!action_ok) ImGui::BeginDisabled();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (56.f - summon_y) * 0.5f);
    if (ImGui::Button(u8"召唤", ImVec2(button_width(u8"召唤"), summon_y))) run_feature_action(f);
    if (!action_ok) ImGui::EndDisabled();
    ImGui::PopID();
    return;
  }

  if (std::strcmp(f->type, "engine") == 0 && std::strcmp(f->key, "summon_troops") == 0) {
    end_chip_flow();
    const bool busy = InterlockedCompareExchange(&g_engine_busy, 0, 0) != 0;
    const bool action_ok = armed && !busy;
    ImGui::PushID(f->key);
    ImGui::TextUnformatted(f->label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        u8"点下方单位 / 建筑放进候选框，再点候选里的图标可拿掉。数量是每种各召唤几个。出现在鼠标处，归所选玩家。快捷键按当前这些设置召唤。");
    ImGui::PopStyleColor();
    const uint32_t who = draw_player_picker("summon", &g_summon_player_ui);
    int count = g_summon_count;
    ImGui::SetNextItemWidth(120.f);
    if (ImGui::InputInt(u8"每种数量", &count, 1, 5)) {
      if (count < 1) count = 1;
      if (count > 20) count = 20;
      g_summon_count = count;
    }
    ImGui::SameLine();
    float lift = game_api::summon_extra_lift();
    ImGui::SetNextItemWidth(140.f);
    if (ImGui::InputFloat(u8"高度##summon_lift", &lift, 100.f, 500.f, "%.0f")) {
      game_api::set_summon_extra_lift(lift);
    }
    ImGui::SetItemTooltip(
        u8"生成点在鼠标落点基础上再抬高多少（0 = 贴地）。\n"
        u8"气球炸弹这类会自己再抬 2000，否则一落地就炸。");
    bool visible_fix = game_api::summon_visible_fix();
    if (ImGui::Checkbox(u8"召唤后立刻可见##summon_visible", &visible_fix)) {
      game_api::set_summon_visible_fix(visible_fix);
    }
    ImGui::SetItemTooltip(
        u8"引擎新建的对象带「隐形 / 未入世」标志，小地图透明、看起来像没召唤出来。\n"
        u8"勾着就清掉这几位（默认，推荐）。");
    bool hold = game_api::air_hold_enabled();
    if (ImGui::Checkbox(u8"抬高后停在空中##summon_hold", &hold)) {
      game_api::set_air_hold_enabled(hold);
    }
    ImGui::SetItemTooltip(
        u8"气球炸弹这类「落地即爆」的对象，光抬高只会变成从高处掉下来炸。\n"
        u8"勾着就每帧把它们按回召唤高度，真的停在空中。");
    if (hold && game_api::air_hold_count() > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled(u8"托着 %d 个", game_api::air_hold_count());
      ImGui::SameLine();
      if (ImGui::SmallButton(u8"放手##summon_release")) {
        game_api::air_hold_release_all();
        g_last_msg = u8"已放手，空中单位开始下落";
      }
      ImGui::SetItemTooltip(u8"清掉托举，让它们按自己的逻辑下落 / 引爆");
    }
    ImGui::SameLine();
    if (!action_ok) ImGui::BeginDisabled();
    if (ImGui::Button(u8"召唤", ImVec2(button_width(u8"召唤"), 0))) {
      std::vector<const char*> ids;
      ids.reserve(g_summon_sel.size());
      for (const auto& s : g_summon_sel) ids.push_back(s.c_str());
      std::string msg;
      if (!game_api::summon_prepare(who, g_summon_count, ids.data(), (int)ids.size(), &msg)) {
        g_last_msg = msg;
        game_api::beep("error");
      } else if (!start_engine_async(f->key)) {
        g_last_msg = u8"上一次操作仍在进行中";
        game_api::beep("error");
      } else {
        g_last_msg = u8"正在召唤…（请把鼠标移到地形上）";
      }
    }
    if (!action_ok) ImGui::EndDisabled();
    ImGui::SameLine();
    draw_feature_gear(f->key);
    {
      std::vector<const char*> live;
      live.reserve(g_summon_sel.size());
      for (const auto& s : g_summon_sel) live.push_back(s.c_str());
      game_api::summon_prepare(who, g_summon_count, live.data(), (int)live.size(), nullptr);
    }
    // 默认展开：图标纹理按每帧 3 张的预算解码，展开不会卡住游戏
    // （以前写死折叠是怕一帧解几百张图）。用户手动折叠后保持折叠。
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader(u8"选择召唤单位 / 建筑##summon_pick")) {
      draw_summon_unit_icons();
    }
    ImGui::PopID();
    return;
  }

  if (std::strcmp(f->key, "invincible") == 0) {
    end_chip_flow();
    const bool busy = InterlockedCompareExchange(&g_engine_busy, 0, 0) != 0;
    const bool action_ok = (!(feature_needs_hooks(f->key) && !armed)) && !busy;
    ImGui::PushID(f->key);
    if (!action_ok) ImGui::BeginDisabled();
    if (ImGui::Button(u8"无敌", ImVec2(button_width(u8"无敌"), 0))) {
      std::string msg;
      const bool ok = game_api::invincible_apply(true, 0.f, 0, &msg);
      if (!msg.empty()) g_last_msg = msg;
      game_api::beep(ok ? "on" : "error");
    }
    ImGui::SameLine(0.f, 10.f);
    if (ImGui::Button(u8"解除无敌", ImVec2(button_width(u8"解除无敌"), 0))) {
      std::string msg;
      game_api::invincible_apply(false, 0.f, 0, &msg);
      if (!msg.empty()) g_last_msg = msg;
      game_api::beep("off");
    }
    if (!action_ok) ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(game_api::invincible_status());
    ImGui::PopStyleColor();
    ImGui::PopID();
    return;
  }

  if (std::strcmp(f->key, "airdrop") == 0) {
    end_chip_flow();
    const bool busy = InterlockedCompareExchange(&g_engine_busy, 0, 0) != 0;
    const bool action_ok = (!(feature_needs_hooks(f->key) && !armed)) && !busy;
    ImGui::PushID(f->key);
    ImGui::TextUnformatted(f->label);

    const int kinds = game_api::crate_kind_count();
    int sel = game_api::crate_kind_sel();
    if (sel < 0 || sel >= kinds) sel = 0;
    int count = game_api::crate_sel_count();
    if (count < 1) count = 1;
    if (count > 20) count = 20;

    // 紧凑布局：一行放完「种类 / 数量 / 空投」。
    // 面板太窄时（< 300px）自动拆成两行，避免控件被挤出面板外看不见。
    const float avail = std::max(140.f, ImGui::GetContentRegionAvail().x);
    const float drop_w = 62.f;
    const float cnt_w = 96.f;
    const float gap = 8.f;
    const bool one_row = avail >= 300.f;
    const float kind_w = one_row ? std::max(90.f, avail - drop_w - cnt_w - gap * 2.f) : avail;

    ImGui::SetNextItemWidth(kind_w);
    if (ImGui::BeginCombo(u8"##crate_kind",
                          kinds > 0 ? game_api::crate_kind_label(sel) : u8"（未就绪）")) {
      for (int i = 0; i < kinds; ++i) {
        char label[160];
        std::snprintf(label, sizeof(label), "%s##crate%d", game_api::crate_kind_label(i), i);
        if (ImGui::Selectable(label, i == sel)) {
          sel = i;
          g_crate_kind_persist = i;
          mark_settings_dirty();
          game_api::crate_select(sel, count);
          game_api::beep("click");
        }
        if (ImGui::IsItemHovered()) Tip("%s", game_api::crate_kind_name(i));
      }
      ImGui::EndCombo();
    }
    if (one_row) ImGui::SameLine(0.f, gap);   // 窄面板时不并排，自动换行

    ImGui::SetNextItemWidth(one_row ? cnt_w : std::max(80.f, avail - drop_w - gap));
    if (ImGui::SliderInt(u8"##crate_count", &count, 1, 20, "%d")) {
      g_crate_count_persist = count;
      mark_settings_dirty();
      game_api::crate_select(sel, count);
    }
    if (ImGui::IsItemHovered()) Tip(u8"空投个数（1~20）");
    ImGui::SameLine(0.f, gap);
    if (!action_ok) ImGui::BeginDisabled();
    if (ImGui::Button(u8"空投", ImVec2(drop_w, 0))) run_feature_action(f);
    if (!action_ok) ImGui::EndDisabled();
    ImGui::SetItemTooltip(u8"鼠标先指到战场地形上再点");

    ImGui::PopID();
    return;
  }

  if (std::strcmp(f->type, "engine") == 0) {
    const bool busy = InterlockedCompareExchange(&g_engine_busy, 0, 0) != 0;
    const bool action_ok = (!(feature_needs_hooks(f->key) && !armed)) && !busy;
    int slots[1] = {};
    if (game_api::hotkey_slots_for_feature(f->key, slots, 1) > 0) {
      draw_hotkey_row(f, armed, false, action_ok);
      return;
    }
    if (!action_ok) ImGui::BeginDisabled();
    if (draw_hud_tile(f->key, f->label, hk, false, action_ok)) {
      // Must not call CreateUnit while blocked inside Present — run async.
      if (start_engine_async(f->key)) {
        g_last_msg = u8"正在执行…（请把鼠标移到地形上）";
      } else {
        g_last_msg = u8"上一次操作仍在进行中";
        game_api::beep("error");
      }
    }
    if (!action_ok) ImGui::EndDisabled();
  }
}

static int build_faction_rank_ui(const char* id) {
  if (!id) return 9;
  if (_strnicmp(id, "Allied", 6) == 0) return 0;
  if (_strnicmp(id, "Soviet", 6) == 0) return 1;
  if (_strnicmp(id, "Japan", 5) == 0) return 2;
  return 3;
}

static const char* build_faction_label(int rank) {
  if (rank == 0) return u8"盟军";
  if (rank == 1) return u8"苏联";
  if (rank == 2) return u8"帝国";
  return u8"其他";
}

static bool is_visible_rep(const game_api::BuildLockEntry* cat, int count, int index) {
  const auto& e = cat[index];
  if (!e.group[0]) return true;
  for (int i = 0; i < count; ++i) {
    if (i == index) continue;
    if (std::strcmp(cat[i].group, e.group) != 0) continue;
    if (cat[i].group_pref < e.group_pref || (cat[i].group_pref == e.group_pref && i < index)) {
      return false;
    }
  }
  return true;
}

static bool shown_banned(const game_api::BuildLockEntry* cat, int count, int index) {
  const auto& e = cat[index];
  if (!e.group[0]) return e.banned;
  for (int i = 0; i < count; ++i) {
    if (std::strcmp(cat[i].group, e.group) != 0) continue;
    if (cat[i].banned) return true;
  }
  return false;
}

static void draw_lock_faction_icons(const game_api::BuildLockEntry* cat, int count,
                                    bool want_banned) {
  const float kIcon = ui_px(40.f);
  const float kGap = ui_px(4.f);
  const float inset = ui_px(2.f);
  ImDrawList* draw = ImGui::GetWindowDrawList();

  for (int fac = 0; fac < 3; ++fac) {
    ImGui::TextDisabled("%s", build_faction_label(fac));
    const float avail = ImGui::GetContentRegionAvail().x;
    float used = 0.f;
    bool started = false;
    bool any = false;
    for (int i = 0; i < count; ++i) {
      const auto& e = cat[i];
      if (build_faction_rank_ui(e.type_id) != fac) continue;
      if (!is_visible_rep(cat, count, i)) continue;
      const bool banned = shown_banned(cat, count, i);
      if (banned != want_banned) continue;

      if (started && used + kIcon + kGap > avail) {
        used = 0.f;
        started = false;
      }
      if (started) ImGui::SameLine(0.f, kGap);

      ImGui::PushID(want_banned ? i + 10000 : i);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("##ico", ImVec2(kIcon, kIcon));
      const bool hovered = ImGui::IsItemHovered();
      const bool clicked = ImGui::IsItemClicked();
      const float hvk = AnimTo(ImGui::GetItemID(), hovered, 18.f);
      {
        PlateSpec ps;
        ps.top = skin::Mix(want_banned ? skin::Over(skin::Fade(SK().bad, 0.26f), SK().win1)
                                       : PanelBg(),
                           SK().money, 0.06f * hvk);
        ps.bot = skin::Mix(want_banned ? skin::Over(skin::Fade(SK().bad, 0.16f), SK().win1)
                                       : PanelBg(),
                           SK().win1, 0.30f);
        ps.edge = skin::Mix(want_banned ? skin::Fade(SK().bad, 0.92f) : EdgeIdle(), SK().money,
                            hvk);
        ps.inner = skin::Fade(SK().cardInnerHi, 0.06f);
        ps.th = hovered ? 1.8f : 1.f;
        DrawPlate(draw, p0, ImVec2(p0.x + kIcon, p0.y + kIcon),
                  ui_px(SK().cardRadius * 0.30f), ps);
      }
      const char* icon_id = e.icon_id[0] ? e.icon_id : e.type_id;
      const char* icon_name = e.icon_name[0] ? e.icon_name : e.name;
      IDirect3DTexture9* tex = unit_icons::get(icon_id, icon_name, e.building);
      if (tex) {
        draw->AddImage((ImTextureID)tex, ImVec2(p0.x + inset, p0.y + inset),
                       ImVec2(p0.x + kIcon - inset, p0.y + kIcon - inset));
      } else {
        const char* mark = e.building ? u8"建" : u8"兵";
        const ImVec2 ts = ImGui::CalcTextSize(mark);
        draw->AddText(ImVec2(p0.x + (kIcon - ts.x) * 0.5f, p0.y + (kIcon - ts.y) * 0.5f),
                      skin::Fade(SK().text, 0.92f), mark);
      }
      if (hovered) {
        TipBegin();
        ImGui::TextUnformatted(e.name[0] ? e.name : e.type_id);
        if (e.paired) ImGui::TextDisabled(u8"车厂和船厂一起限制");
        TipEnd();
      }
      if (clicked) {
        std::string msg;
        if (game_api::build_lock_set(e.type_id, !want_banned, &msg)) {
          g_last_msg = msg;
          game_api::beep("click");
        } else {
          g_last_msg = msg;
          game_api::beep("error");
        }
      }
      ImGui::PopID();
      used += kIcon + kGap;
      started = true;
      any = true;
    }
    if (!any) ImGui::TextDisabled(u8"—");
    ImGui::Spacing();
  }
}

static void draw_build_lock_panel() {
  ImGui::TextColored(CV(HeadingCol()), u8"建造限制");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"对局内所有玩家生效");

  int count = 0;
  const game_api::BuildLockEntry* cat = game_api::build_lock_catalog(&count);
  const float pane = ImGui::GetContentRegionAvail().y * 0.5f - 4.f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, CVA(SK().good, 0.10f));
  ImGui::BeginChild("##build_allow", ImVec2(0, pane), ImGuiChildFlags_Borders);
  ImGui::TextColored(CV(SK().good), u8"允许");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"点击图标后移到下方");
  if (count <= 0) {
    ImGui::TextWrapped(u8"还没读到可建造的单位。进入对局后再打开这一页。");
  } else {
    draw_lock_faction_icons(cat, count, false);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::PushStyleColor(ImGuiCol_ChildBg, CVA(SK().bad, 0.10f));
  ImGui::BeginChild("##build_deny", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::TextColored(CV(SK().bad), u8"禁止");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"点击图标后恢复建造");
  if (count <= 0) {
    ImGui::TextDisabled(u8"—");
  } else {
    draw_lock_faction_icons(cat, count, true);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// 协议限制：与「建造限制」同一套板子（上允许 / 下禁止，点图标来回搬）。
// 图标来自 unit_images\<阵营>\协议\。一个阵营的协议平铺排开，不画分类标题，
// 只保证同一协议的 1/2/3 级挨在一起（family 相同者算一族，整族一起换行）。
// 目前只维护界面状态，还没有写进引擎（game_api::protocol_lock_set）。
// ---------------------------------------------------------------------------
static void draw_protocol_faction_icons(const game_api::ProtocolLockEntry* cat, int count,
                                        bool want_banned) {
  const float kIcon = ui_px(40.f);
  const float kGap = ui_px(4.f);
  const float inset = ui_px(2.f);
  ImDrawList* draw = ImGui::GetWindowDrawList();

  for (int fac = 0; fac < 3; ++fac) {
    ImGui::TextDisabled("%s", build_faction_label(fac));
    const float avail = ImGui::GetContentRegionAvail().x;
    float used = 0.f;
    bool started = false;
    bool any = false;
    for (int i = 0; i < count;) {
      const auto& first = cat[i];
      if (first.faction != fac || first.banned != want_banned) {
        ++i;
        continue;
      }

      // 同族 = 同一协议的 1/2/3 级：整族一起算宽度，先换行再画，
      // 避免一个协议的分级被拆到两行。
      int j = i;
      int members = 0;
      while (j < count && cat[j].faction == fac && cat[j].banned == want_banned &&
             std::strcmp(cat[j].family, first.family) == 0) {
        ++members;
        ++j;
      }
      const float fam_w = members * kIcon + (members - 1) * kGap;
      if (started && used + kGap + fam_w > avail) {
        used = 0.f;
        started = false;
      }

      for (int k = i; k < j; ++k) {
        const auto& e = cat[k];
        if (started) ImGui::SameLine(0.f, kGap);

        ImGui::PushID(want_banned ? k + 10000 : k);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ico", ImVec2(kIcon, kIcon));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const float hvk = AnimTo(ImGui::GetItemID(), hovered, 18.f);
        {
          PlateSpec ps;
          ps.top = skin::Mix(want_banned ? skin::Over(skin::Fade(SK().bad, 0.26f), SK().win1)
                                         : PanelBg(),
                             SK().money, 0.06f * hvk);
          ps.bot = skin::Mix(want_banned ? skin::Over(skin::Fade(SK().bad, 0.16f), SK().win1)
                                         : PanelBg(),
                             SK().win1, 0.30f);
          ps.edge = skin::Mix(want_banned ? skin::Fade(SK().bad, 0.92f)
                                          : (e.ultimate ? skin::Fade(SK().accent2, 0.80f)
                                                        : EdgeIdle()),
                              SK().money, hvk);
          ps.inner = skin::Fade(SK().cardInnerHi, 0.06f);
          ps.th = hovered ? 1.8f : 1.f;
          DrawPlate(draw, p0, ImVec2(p0.x + kIcon, p0.y + kIcon),
                    ui_px(SK().cardRadius * 0.30f), ps);
        }
        IDirect3DTexture9* tex =
            unit_icons::get(e.icon_id, e.icon_name, false);
        if (tex) {
          draw->AddImage((ImTextureID)tex, ImVec2(p0.x + inset, p0.y + inset),
                         ImVec2(p0.x + kIcon - inset, p0.y + kIcon - inset));
        } else {
          const char* mark = u8"协";
          const ImVec2 ts = ImGui::CalcTextSize(mark);
          draw->AddText(ImVec2(p0.x + (kIcon - ts.x) * 0.5f, p0.y + (kIcon - ts.y) * 0.5f),
                        skin::Fade(SK().text, 0.92f), mark);
        }
        if (hovered) {
          TipBegin();
          ImGui::TextUnformatted(e.name[0] ? e.name : e.type_id);
          if (e.tier > 0) ImGui::TextDisabled(u8"第 %d 级协议", e.tier);
          if (e.ultimate) ImGui::TextDisabled(u8"超级武器协议");
          ImGui::TextDisabled(want_banned ? u8"点击恢复使用"
                                          : u8"点击禁止使用");
          TipEnd();
        }
        if (clicked) {
          std::string msg;
          if (game_api::protocol_lock_set(e.type_id, !want_banned, &msg)) {
            g_last_msg = msg;
            game_api::beep("click");
          } else {
            g_last_msg = msg;
            game_api::beep("error");
          }
        }
        ImGui::PopID();
        used += kIcon + kGap;
        started = true;
        any = true;
      }
      i = j;
    }
    if (!any) ImGui::TextDisabled(u8"—");
    ImGui::Spacing();
  }
}

static void draw_protocol_lock_panel() {
  ImGui::TextColored(CV(HeadingCol()), u8"协议限制");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"限制本局三方协议的使用");

  int count = 0;
  const game_api::ProtocolLockEntry* cat = game_api::protocol_lock_catalog(&count);
  int banned = 0;
  for (int i = 0; i < count; ++i) {
    if (cat[i].banned) ++banned;
  }
  ImGui::TextDisabled(u8"已禁止 %d / %d · 界面已就绪，接线前不影响游戏", banned, count);

  const float pane = ImGui::GetContentRegionAvail().y * 0.5f - 4.f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, CVA(SK().good, 0.10f));
  ImGui::BeginChild("##proto_allow", ImVec2(0, pane), ImGuiChildFlags_Borders);
  ImGui::TextColored(CV(SK().good), u8"允许");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"点击图标后移到下方");
  if (count <= 0) {
    ImGui::TextDisabled(u8"—");
  } else {
    draw_protocol_faction_icons(cat, count, false);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::PushStyleColor(ImGuiCol_ChildBg, CVA(SK().bad, 0.10f));
  ImGui::BeginChild("##proto_deny", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::TextColored(CV(SK().bad), u8"禁止");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"点击图标后恢复使用");
  if (count <= 0) {
    ImGui::TextDisabled(u8"—");
  } else {
    draw_protocol_faction_icons(cat, count, true);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

constexpr float kRad2Deg = 57.2957795f;
constexpr float kDeg2Rad = 0.0174532925f;

float deg_of_rad(float rad) {
  float d = rad * kRad2Deg;
  while (d > 180.f) d -= 360.f;
  while (d <= -180.f) d += 360.f;
  return d;
}

// 摄像机页面（CameraBridge 的「摄像机操作」）
void draw_camera_panel() {
  static game_api::CameraState s_edit;
  static bool s_edit_ok = false;
  static bool s_follow = true;
  static float s_step_pos = 50.f;
  static float s_step_ang = 5.f;
  static float s_step_dist = 50.f;
  static char s_new_label[32] = {};
  static char s_cfg_name[64] = {};
  static char s_cfg_note[512] = {};
  static int s_note_cfg = -1;

  game_api::CameraState live{};
  const bool have_live = game_api::camera_read(&live);
  const bool takeover = game_api::camera_takeover();
  if (takeover) {
    game_api::CameraState lv{};
    if (game_api::camera_live(&lv)) {
      s_edit = lv;
      s_edit_ok = true;
    }
  } else if (s_follow && have_live) {
    s_edit = live;
    s_edit_ok = true;
  }
  if (!s_edit_ok && have_live) {
    s_edit = live;
    s_edit_ok = true;
  }

  auto push_edit = [&]() {
    if (!game_api::camera_takeover()) game_api::camera_set_takeover(true);
    game_api::camera_set_live(&s_edit);
    game_api::camera_apply(&s_edit);
  };

  ImGui::TextColored(CV(HeadingCol()), u8"摄像机");
  ImGui::SameLine();
  const int hook_state = game_api::camera_hook_state();
  if (hook_state == 1 && have_live) {
    ImGui::TextDisabled(u8"已接管引擎相机 (View %08X)", live.view);
  } else if (hook_state == 1) {
    ImGui::TextDisabled(u8"hook 已就绪，等待游戏更新相机…");
  } else if (hook_state < 0) {
    ImGui::TextColored(CV(SK().bad), u8"相机 hook 安装失败，请重新注入");
  } else {
    ImGui::TextDisabled(u8"未就绪（仅零售版 1.12 支持）");
  }

  if (ImGui::BeginTabBar("##cam_tabs", ImGuiTabBarFlags_None)) {
    // ------------------------------- 当前坐标 -------------------------------
    if (ImGui::BeginTabItem(u8"当前坐标")) {
      ImGui::TextDisabled(u8"这里改的是镜头位置，不是单位位置。复制/移动单位请用「单位操作」。");
      if (!have_live && hook_state == 1) {
        ImGui::TextWrapped(u8"还没有读到相机。先进入对局并移动一下视角（或者等一帧），再回到这里。");
      }
      if (have_live) {
        ImGui::TextDisabled(u8"游戏当前：X %.1f  Y %.1f  Z %.1f   偏航 %.2f°  俯仰 %.2f°  距离 %.1f",
                            live.x, live.y, live.z, deg_of_rad(live.yaw),
                            live.pitch * kRad2Deg, live.dist);
      }

      ImGui::Spacing();
      bool tk = takeover;
      if (ImGui::Checkbox(u8"接管摄像机（持续锁定）", &tk)) {
        game_api::camera_set_takeover(tk);
        game_api::beep(tk ? "on" : "off");
      }
      if (ImGui::IsItemHovered()) {
        Tip(u8"打开后引擎每次设置相机都会被改成这里的目标值；关掉恢复游戏自己控制。"
                          u8"退出遭遇战时会自动关闭，避免卡死。");
      }
      ImGui::SameLine();
      ImGui::Checkbox(u8"跟随当前", &s_follow);
      ImGui::SameLine();
      ImGui::TextDisabled(u8"(关掉「接管」时数值会跟着游戏走)");

      ImGui::Separator();
      ImGui::TextUnformatted(u8"坐标");
      ImGui::SetNextItemWidth(110.f);
      if (ImGui::InputFloat("X##camx", &s_edit.x, 0.f, 0.f, "%.2f")) push_edit();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.f);
      if (ImGui::InputFloat("Y##camy", &s_edit.y, 0.f, 0.f, "%.2f")) push_edit();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.f);
      if (ImGui::InputFloat("Z##camz", &s_edit.z, 0.f, 0.f, "%.2f")) push_edit();

      ImGui::TextUnformatted(u8"旋转");
      float yaw_d = deg_of_rad(s_edit.yaw);
      float pitch_d = s_edit.pitch * kRad2Deg;
      float roll_d = s_edit.roll * kRad2Deg;
      ImGui::SetNextItemWidth(110.f);
      if (ImGui::InputFloat(u8"偏航##camyaw", &yaw_d, 0.f, 0.f, "%.2f")) {
        s_edit.yaw = yaw_d * kDeg2Rad;
        push_edit();
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.f);
      if (ImGui::InputFloat(u8"俯仰##campitch", &pitch_d, 0.f, 0.f, "%.2f")) {
        s_edit.pitch = pitch_d * kDeg2Rad;
        push_edit();
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.f);
      if (ImGui::InputFloat(u8"滚转##camroll", &roll_d, 0.f, 0.f, "%.2f")) {
        s_edit.roll = roll_d * kDeg2Rad;
        push_edit();
      }

      ImGui::TextUnformatted(u8"距离（缩放）");
      ImGui::SetNextItemWidth(110.f);
      if (ImGui::InputFloat("##camdist", &s_edit.dist, 0.f, 0.f, "%.1f")) push_edit();

      ImGui::Spacing();
      if (ImGui::Button(u8"把当前值应用到游戏", ImVec2(button_width(u8"把当前值应用到游戏"), 0))) {
        push_edit();
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"读取游戏当前", ImVec2(button_width(u8"读取游戏当前"), 0))) {
        if (have_live) {
          s_edit = live;
          s_follow = true;
        }
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"停止接管", ImVec2(ui_px(90.f), 0))) {
        game_api::camera_set_takeover(false);
        game_api::beep("off");
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled(u8"微调（步长可调，点一下立即生效）");
      ImGui::SetNextItemWidth(90.f);
      ImGui::InputFloat(u8"位置步长", &s_step_pos, 0.f, 0.f, "%.0f");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90.f);
      ImGui::InputFloat(u8"角度步长", &s_step_ang, 0.f, 0.f, "%.1f");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90.f);
      ImGui::InputFloat(u8"距离步长", &s_step_dist, 0.f, 0.f, "%.0f");

      auto nudge = [&](const char* id, const char* label, float dx, float dy, float dz,
                       float da, float dp, float dr, float dd) {
        ImGui::PushID(id);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        if (ImGui::Button(u8"-##m", ImVec2(button_width(u8"-##m"), 0))) {
          s_edit.x -= dx;
          s_edit.y -= dy;
          s_edit.z -= dz;
          s_edit.yaw -= da * kDeg2Rad;
          s_edit.pitch -= dp * kDeg2Rad;
          s_edit.roll -= dr * kDeg2Rad;
          s_edit.dist -= dd;
          if (s_edit.dist < 1.f) s_edit.dist = 1.f;
          push_edit();
        }
        ImGui::SameLine();
        if (ImGui::Button(u8"+##p", ImVec2(button_width(u8"+##p"), 0))) {
          s_edit.x += dx;
          s_edit.y += dy;
          s_edit.z += dz;
          s_edit.yaw += da * kDeg2Rad;
          s_edit.pitch += dp * kDeg2Rad;
          s_edit.roll += dr * kDeg2Rad;
          s_edit.dist += dd;
          push_edit();
        }
        ImGui::PopID();
      };
      nudge("camx", u8"X", s_step_pos, 0, 0, 0, 0, 0, 0);
      ImGui::SameLine();
      nudge("camy", u8"Y", 0, s_step_pos, 0, 0, 0, 0, 0);
      ImGui::SameLine();
      nudge("camz", u8"Z", 0, 0, s_step_pos, 0, 0, 0, 0);
      nudge("camyaw", u8"偏航", 0, 0, 0, s_step_ang, 0, 0, 0);
      ImGui::SameLine();
      nudge("campitch", u8"俯仰", 0, 0, 0, 0, s_step_ang, 0, 0);
      ImGui::SameLine();
      nudge("camroll", u8"滚转", 0, 0, 0, 0, 0, s_step_ang, 0);
      nudge("camdist", u8"距离", 0, 0, 0, 0, 0, 0, s_step_dist);

      ImGui::Spacing();
      ImGui::TextDisabled(
          u8"提示：Z 轴向上。偏航=atan2(矩阵[4],矩阵[0])，俯仰=asin(矩阵[9])，与引擎 0x7EC240 的算法一致。");
      ImGui::EndTabItem();
    }

    // ------------------------------- 轨迹与播放 -------------------------------
    if (ImGui::BeginTabItem(u8"轨迹与播放")) {
      const int interp = game_api::camera_interp();
      ImGui::TextUnformatted(u8"插值");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(140.f);
      int mode = interp;
      if (ImGui::Combo("##cam_interp", &mode, u8"线性插值\0贝塞尔曲线\0平滑样条\0")) {
        game_api::camera_set_interp(mode);
      }
      if (interp == 1) {
        ImGui::SameLine();
        ImGui::TextDisabled(u8"节点作为控制点，不一定经过节点");
      } else if (interp == 2) {
        ImGui::SameLine();
        ImGui::TextDisabled(u8"曲线穿过每个节点（额外选项）");
      }

      const int st = game_api::camera_play_state();
      const float total = game_api::camera_total_time();
      const float head = game_api::camera_playhead();
      const int key_n = game_api::camera_key_count();

      ImGui::Spacing();
      if (ImGui::Button(st == 1 ? u8"暂停" : u8"播放", ImVec2(ui_px(80.f), 0))) {
        if (st == 1) {
          game_api::camera_play_pause();
        } else if (key_n < 2) {
          game_api::set_status(u8"至少需要两个节点才能播放");
          game_api::beep("error");
        } else {
          game_api::camera_play_start();
          game_api::beep("click");
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"停止", ImVec2(button_width(u8"停止"), 0))) {
        game_api::camera_play_stop();
        game_api::beep("off");
      }
      ImGui::SameLine();
      bool loop = game_api::camera_loop();
      if (ImGui::Checkbox(u8"循环", &loop)) game_api::camera_set_loop(loop);
      ImGui::SameLine();
      float rate = game_api::camera_play_rate();
      ImGui::SetNextItemWidth(140.f);
      if (ImGui::SliderFloat(u8"速度", &rate, 0.1f, 4.f, "%.2fx")) {
        game_api::camera_set_play_rate(rate);
      }

      float t = head;
      ImGui::SetNextItemWidth(-60.f);
      if (ImGui::SliderFloat("##cam_head", &t, 0.f, total > 0.1f ? total : 1.f, "%.2f s")) {
        game_api::camera_set_playhead(t);
      }
      ImGui::SameLine();
      ImGui::Text("%.1f/%.1f", head, total);
      ImGui::SameLine();
      ImGui::TextDisabled(u8"节点 %d", key_n);

      ImGui::Separator();
      ImGui::SetNextItemWidth(120.f);
      ImGui::InputText(u8"新节点标签", s_new_label, sizeof(s_new_label));
      ImGui::SameLine();
      if (ImGui::Button(u8"复制当前坐标并添加节点", ImVec2(button_width(u8"复制当前坐标并添加节点"), 0))) {
        const int idx = game_api::camera_key_add_from_current(s_new_label);
        if (idx < 0) {
          game_api::set_status(u8"添加节点失败：还没读到摄像机");
          game_api::beep("error");
        } else {
          game_api::beep("click");
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"清空轨迹", ImVec2(ui_px(80.f), 0))) {
        game_api::camera_keys_clear();
        game_api::beep("off");
      }

      const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                 ImGuiTableFlags_SizingFixedFit;
      if (ImGui::BeginTable("##cam_keys", 13, tf, ImVec2(0, 230.f))) {
        const char* heads[13] = {"#",    u8"标签", u8"时间", "X",      "Y",    "Z",
                                 u8"偏航", u8"俯仰", u8"滚转",  u8"距离", u8"操作",
                                 "##mv", "##del"};
        for (int c = 0; c < 13; ++c) ImGui::TableSetupColumn(heads[c]);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < key_n; ++i) {
          game_api::CameraKey k;
          if (!game_api::camera_key(i, &k)) continue;
          game_api::CameraKey e = k;
          bool commit = false;
          ImGui::PushID(i);
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%d", i + 1);

          ImGui::TableSetColumnIndex(1);
          ImGui::SetNextItemWidth(80.f);
          char lab[32];
          std::memcpy(lab, e.label, sizeof(lab));
          if (ImGui::InputText("##lab", lab, sizeof(lab))) {
            std::snprintf(e.label, sizeof(e.label), "%s", lab);
            commit = true;
          }

          ImGui::TableSetColumnIndex(2);
          ImGui::SetNextItemWidth(60.f);
          ImGui::InputFloat("##t", &e.t, 0.f, 0.f, "%.2f");
          if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;

          const float vx[7] = {e.x, e.y, e.z, deg_of_rad(e.yaw), e.pitch * kRad2Deg,
                               e.roll * kRad2Deg, e.dist};
          float out[7] = {vx[0], vx[1], vx[2], vx[3], vx[4], vx[5], vx[6]};
          const char* ids[7] = {"x", "y", "z", "yaw", "pitch", "roll", "dist"};
          for (int c = 0; c < 7; ++c) {
            ImGui::TableSetColumnIndex(3 + c);
            ImGui::SetNextItemWidth(c == 6 ? 60.f : 62.f);
            ImGui::InputFloat(ids[c], &out[c], 0.f, 0.f, c >= 3 && c <= 5 ? "%.2f" : "%.1f");
            if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
          }
          e.x = out[0];
          e.y = out[1];
          e.z = out[2];
          e.yaw = out[3] * kDeg2Rad;
          e.pitch = out[4] * kDeg2Rad;
          e.roll = out[5] * kDeg2Rad;
          e.dist = out[6];

          ImGui::TableSetColumnIndex(10);
          if (ImGui::SmallButton(u8"用当前")) {
            game_api::camera_key_update_from_current(i);
            game_api::beep("click");
          }
          ImGui::SameLine();
          if (ImGui::SmallButton(u8"跳转")) {
            game_api::camera_set_playhead(e.t);
            game_api::beep("click");
          }
          ImGui::TableSetColumnIndex(11);
          if (ImGui::SmallButton(u8"删除")) {
            game_api::camera_key_remove(i);
            commit = false;
            ImGui::PopID();
            break;
          }

          const bool time_changed = commit && (e.t != k.t);
          if (commit) game_api::camera_key_set(i, e);
          ImGui::PopID();
          if (time_changed) break;  // 排序后本帧剩余行下标失效，下一帧再画
        }
        ImGui::EndTable();
      }

      // 俯视轨迹图
      ImGui::Spacing();
      ImGui::TextDisabled(u8"轨迹俯视图（X/Y 平面）");
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImVec2 avail = ImGui::GetContentRegionAvail();
      if (avail.y > 200.f) avail.y = 200.f;
      if (avail.x < 60.f) avail.x = 60.f;
      if (avail.y < 60.f) avail.y = 60.f;
      ImGui::InvisibleButton("##cam_pathmap", avail);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), FieldBg(), 4.f);
      dl->AddRect(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), EdgeIdle(), 4.f);
      if (key_n >= 1) {
        float minx = 0.f, maxx = 0.f, miny = 0.f, maxy = 0.f;
        for (int i = 0; i < key_n; ++i) {
          game_api::CameraKey k;
          if (!game_api::camera_key(i, &k)) continue;
          if (i == 0) {
            minx = maxx = k.x;
            miny = maxy = k.y;
          } else {
            minx = k.x < minx ? k.x : minx;
            maxx = k.x > maxx ? k.x : maxx;
            miny = k.y < miny ? k.y : miny;
            maxy = k.y > maxy ? k.y : maxy;
          }
        }
        float spanx = maxx - minx;
        float spany = maxy - miny;
        if (spanx < 1.f) spanx = 1.f;
        if (spany < 1.f) spany = 1.f;
        const float scale = std::min((avail.x - 30.f) / spanx, (avail.y - 30.f) / spany);
        auto map_pt = [&](float x, float y) {
          return ImVec2(p0.x + 15.f + (x - minx) * scale,
                        p0.y + avail.y - 15.f - (y - miny) * scale);
        };
        ImVec2 prev(0, 0);
        for (int i = 0; i < key_n; ++i) {
          game_api::CameraKey k;
          if (!game_api::camera_key(i, &k)) continue;
          const ImVec2 pt = map_pt(k.x, k.y);
          if (i > 0) dl->AddLine(prev, pt, skin::Fade(SK().accent, 0.86f), 2.f);
          dl->AddCircleFilled(pt, 4.f, SK().warn);
          char num[8];
          std::snprintf(num, sizeof(num), "%d", i + 1);
          dl->AddText(ImVec2(pt.x + 6.f, pt.y - 16.f), skin::Fade(SK().text, 0.92f), num);
          prev = pt;
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8.f, p0.y + 6.f));
        ImGui::TextDisabled(u8"X %.0f~%.0f  Y %.0f~%.0f", minx, maxx, miny, maxy);
      }
      ImGui::EndTabItem();
    }

    // ------------------------------- 定时事件 -------------------------------
    if (ImGui::BeginTabItem(u8"定时事件")) {
      const int ev_n = game_api::camera_event_count();
      const int fired = game_api::camera_events_fired();
      ImGui::TextDisabled(u8"事件触发一次后不会自动重置，需要手动点「重置已触发」（和 CameraBridge 一致）");
      ImGui::Spacing();
      if (ImGui::Button(u8"添加事件", ImVec2(ui_px(100.f), 0))) {
        game_api::CameraEvent ev;
        ev.t = 0.f;
        ev.base = 0;
        ev.action = 0;
        game_api::camera_event_add(ev);
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"重置已触发", ImVec2(button_width(u8"重置已触发"), 0))) {
        game_api::camera_events_reset();
        game_api::beep("click");
      }
      ImGui::SameLine();
      ImGui::TextColored(CV(SK().money), u8"%d / %d 个事件已触发", fired, ev_n);
      ImGui::SameLine();
      if (ImGui::Button(u8"清空", ImVec2(button_width(u8"清空"), 0))) game_api::camera_events_clear();

      const ImGuiTableFlags etf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
      if (ImGui::BeginTable("##cam_events", 6, etf, ImVec2(0, 260.f))) {
        ImGui::TableSetupColumn(u8"时间(s)");
        ImGui::TableSetupColumn(u8"基准");
        ImGui::TableSetupColumn(u8"动作");
        ImGui::TableSetupColumn(u8"参数");
        ImGui::TableSetupColumn(u8"状态");
        ImGui::TableSetupColumn(u8"操作");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < ev_n; ++i) {
          game_api::CameraEvent ev;
          if (!game_api::camera_event(i, &ev)) continue;
          game_api::CameraEvent e = ev;
          bool commit = false;
          ImGui::PushID(i);
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          ImGui::SetNextItemWidth(70.f);
          ImGui::InputFloat("##et", &e.t, 0.f, 0.f, "%.2f");
          if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;

          ImGui::TableSetColumnIndex(1);
          ImGui::SetNextItemWidth(120.f);
          if (ImGui::Combo("##eb", &e.base, u8"动画开始后\0进入对局后\0")) commit = true;

          ImGui::TableSetColumnIndex(2);
          ImGui::SetNextItemWidth(150.f);
          if (ImGui::Combo("##ea", &e.action,
                           u8"开始播放动画\0停止播放动画\0暂停/继续动画\0跳到指定节点\0使用节点坐标\0切换功能开关\0")) {
            commit = true;
          }

          ImGui::TableSetColumnIndex(3);
          ImGui::SetNextItemWidth(150.f);
          if (e.action == 3 || e.action == 4) {
            const int kn = game_api::camera_key_count();
            char items[1024] = {};
            size_t off = 0;
            for (int m = 0; m < kn && off + 32 < sizeof(items); ++m) {
              char one[32];
              const int len = std::snprintf(one, sizeof(one), u8"节点 %d", m + 1);
              if (len <= 0) break;
              std::memcpy(items + off, one, (size_t)len + 1);
              off += (size_t)len + 1;
            }
            if (kn > 0 && off + 2 <= sizeof(items)) {
              items[off] = 0;
              if (ImGui::Combo("##epn", &e.param, items)) commit = true;
            } else {
              ImGui::TextDisabled(u8"没有节点");
            }
          } else if (e.action == 5) {
            const int fn = game_api::camera_event_feature_count();
            char items[512] = {};
            size_t off = 0;
            for (int m = 0; m < fn && off + 64 < sizeof(items); ++m) {
              const char* s = game_api::camera_event_feature_label(m);
              const size_t len = std::strlen(s);
              std::memcpy(items + off, s, len + 1);
              off += len + 1;
            }
            if (fn > 0 && off + 2 <= sizeof(items)) {
              items[off] = 0;
              if (ImGui::Combo("##epf", &e.param, items)) commit = true;
            } else {
              ImGui::TextDisabled(u8"—");
            }
          } else {
            ImGui::TextDisabled(u8"—");
          }

          ImGui::TableSetColumnIndex(4);
          if (ev.fired) {
            ImGui::TextColored(CV(SK().good), u8"已触发");
          } else {
            ImGui::TextDisabled(u8"待触发");
          }

          ImGui::TableSetColumnIndex(5);
          if (ImGui::SmallButton(u8"删除")) {
            game_api::camera_event_remove(i);
            ImGui::PopID();
            break;
          }
          if (commit) game_api::camera_event_set(i, e);
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    // ------------------------------- 配置 -------------------------------
    if (ImGui::BeginTabItem(u8"配置")) {
      const int n = game_api::camera_config_count();
      const int active = game_api::camera_config_active();
      if (s_note_cfg != active) {
        s_note_cfg = active;
        std::snprintf(s_cfg_note, sizeof(s_cfg_note), "%s", game_api::camera_config_note(active));
        std::snprintf(s_cfg_name, sizeof(s_cfg_name), "%s", game_api::camera_config_name(active));
      }

      ImGui::TextDisabled(u8"一个配置就是一条轨迹 + 它的定时事件，自动保存在 DLL 同目录的 ra3_camera.ini");
      ImGui::Spacing();
      ImGui::SetNextItemWidth(180.f);
      if (ImGui::BeginCombo("##cfg", game_api::camera_config_name(active))) {
        for (int i = 0; i < n; ++i) {
          const bool sel = i == active;
          if (ImGui::Selectable(game_api::camera_config_name(i), sel)) {
            game_api::camera_config_select(i);
          }
          if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"新建", ImVec2(ui_px(60.f), 0))) {
        game_api::camera_config_add(u8"新轨迹");
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"克隆", ImVec2(ui_px(60.f), 0))) {
        game_api::camera_config_clone(active, u8"");
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"删除", ImVec2(ui_px(60.f), 0))) {
        if (!game_api::camera_config_remove(active)) game_api::beep("error");
        else game_api::beep("off");
      }

      ImGui::Spacing();
      ImGui::TextUnformatted(u8"名称");
      ImGui::SetNextItemWidth(220.f);
      if (ImGui::InputText("##cfgname", s_cfg_name, sizeof(s_cfg_name))) {
        game_api::camera_config_rename(active, s_cfg_name);
      }
      ImGui::Spacing();
      ImGui::TextUnformatted(u8"说明");
      ImGui::SetNextItemWidth(-1.f);
      if (ImGui::InputTextMultiline("##cfgnote", s_cfg_note, sizeof(s_cfg_note),
                                    ImVec2(-1.f, 90.f))) {
        game_api::camera_config_set_note(active, s_cfg_note);
      }

      ImGui::Spacing();
      if (ImGui::Button(u8"保存到文件", ImVec2(ui_px(120.f), 0))) {
        const bool ok = game_api::camera_config_save();
        game_api::beep(ok ? "click" : "error");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"从文件读取", ImVec2(ui_px(120.f), 0))) {
        game_api::camera_config_load();
        s_note_cfg = -1;
        game_api::beep("click");
      }
      ImGui::Spacing();
      ImGui::TextDisabled(u8"文件：%s", game_api::camera_file_path());
      ImGui::Spacing();
      ImGui::TextDisabled(u8"快捷键：Ctrl+E 播放/暂停动画，Ctrl+Alt+E 停止，Ctrl+K 记录节点，Ctrl+S 保存配置。");
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }
}

// ---------------------------------------------------------------------------
// 时间控制面板（RA3TimePausePlugin 对应）
// ---------------------------------------------------------------------------
static std::string fmt_clock(float seconds) {
  if (!(seconds > 0.f)) seconds = 0.f;
  const int total = (int)(seconds + 0.5f);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
  return buf;
}

void draw_time_panel() {
  static float s_scale = 1.0f;
  static bool s_enabled = false;
  static bool s_inited = false;

  game_api::TimeProbe probe;
  game_api::time_control_probe_snapshot(&probe);
  if (!s_inited) {
    s_scale = game_api::time_control_scale();
    s_enabled = game_api::time_control_enabled();
    s_inited = true;
  }

  ImGui::TextColored(CV(HeadingCol()), u8"时间控制（加速·实验）");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"候选旋钮 [0xCB4098] logicFPS / [0xCB409C] updateFPS");

  ImGui::Spacing();
  ImGui::TextWrapped(u8"引擎 0x00655166 的帧循环：edi = updateFPS ÷ logicFPS，然后跑大约 6/edi 次"
                     u8"逻辑更新。实测只把 updateFPS 调大，只会改变帧率、单位并不会变快（甚至略慢），"
                     u8"所以下面把写入目标做成可切换项，请逐个试出真正管用的那个。");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ---- 实时状态 ----
  const int logic_fps = probe.logic_fps > 0 ? probe.logic_fps : 15;
  const int update_fps = probe.update_fps > 0 ? probe.update_fps : 30;
  ImGui::Text(u8"logicFPS = %d", logic_fps);
  ImGui::SameLine();
  ImGui::TextColored(CV(SK().good), u8"| updateFPS = %d", update_fps);
  ImGui::SameLine();
  ImGui::TextDisabled(u8"| edi = %d", update_fps / (logic_fps > 0 ? logic_fps : 1));
  if (probe.original_logic_fps > 0) {
    ImGui::Text(u8"进局原值 logicFPS = %d  updateFPS = %d", probe.original_logic_fps,
                probe.original_update_fps);
  } else {
    ImGui::TextDisabled(u8"还没写过（原值将在第一次写入时记录）");
  }
  if (!game_api::battle_clock(nullptr)) {
    ImGui::TextDisabled(u8"当前不在对局中：加速只在遭遇战/战役里生效。");
  }

  ImGui::Spacing();
  ImGui::TextColored(CV(SK().bad),
                     u8"硬性不变式：logicFPS >= 1 且 updateFPS >= logicFPS。"
                     u8"破坏后者会让 0x0065516D 的整除得 0，再在 0x0065519A 处除零崩溃");
  ImGui::TextColored(CV(SK().bad),
                     u8"（上一版就是这么崩的）。所以减速/暂停做不了，只能加速。"
                     u8"旧实现写的 [[0xCE8138]+0x54] 其实是亮度，早已停止写入。");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ---- 写入目标 ----
  static int s_target = -1;
  if (s_target < 0) s_target = game_api::time_control_speed_target();
  ImGui::TextDisabled(u8"写入目标（默认「不改内存」，选错也不会崩，随时可切回）");
  const char* kTargets[] = {u8"不改内存（只读）", u8"只改 logicFPS [0xCB4098]",
                            u8"只改 updateFPS [0xCB409C]", u8"两者同步（保持 edi 不变）"};
  game_api::TimeProbe p2;
  ImGui::SetNextItemWidth(320.f);
  if (ImGui::Combo(u8"##time_target", &s_target, kTargets, 4)) {
    game_api::time_control_set_speed_target(s_target);
    game_api::time_control_set_scale(s_scale);  // 立刻按当前倍率应用
    g_last_msg = s_target == 0 ? u8"时间控制：已停止写内存并还原"
                               : u8"已切换写入目标，请对比单位速度是否变化";
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ---- 倍率 ----
  s_enabled = game_api::time_control_enabled();  // 以模块内状态为准
  ImGui::SetNextItemWidth(220.f);
  if (ImGui::SliderFloat(u8"倍率##time_scale", &s_scale, 1.0f, 8.0f, u8"%.2fx")) {
    if (s_target != 0) {
      game_api::time_control_set_scale(s_scale);
      s_enabled = true;
    }
    g_last_msg = u8"已设置加速倍率";
  }
  ImGui::SameLine();
  ImGui::TextDisabled(u8"1x = 进局原速，8x = 上限；低于 1x 会被抬到 1x");

  ImGui::BeginDisabled(s_target == 0);
  if (ImGui::Button(u8"1x##time_1x")) {
    s_scale = 1.0f;
    game_api::time_control_set_scale(1.0f);
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"1.5x##time_1_5x")) {
    s_scale = 1.5f;
    game_api::time_control_set_scale(1.5f);
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"2x##time_2x")) {
    s_scale = 2.0f;
    game_api::time_control_set_scale(2.0f);
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"3x##time_3x")) {
    s_scale = 3.0f;
    game_api::time_control_set_scale(3.0f);
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"4x##time_4x")) {
    s_scale = 4.0f;
    game_api::time_control_set_scale(4.0f);
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"6x##time_6x")) {
    s_scale = 6.0f;
    game_api::time_control_set_scale(6.0f);
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"8x##time_8x")) {
    s_scale = 8.0f;
    game_api::time_control_set_scale(8.0f);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(u8"恢复原始速度##time_restore")) {
    game_api::time_control_restore();
    s_scale = 1.0f;
    s_enabled = false;
    g_last_msg = u8"已恢复原始游戏速度";
  }

  ImGui::Spacing();
  ImGui::TextDisabled(u8"退出对局时会自动写回原值；写入次数 %d。", probe.writes);

  // ---- 只读诊断：给将来找减速/暂停挂载点用 ----
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (ImGui::CollapsingHeader(u8"只读诊断（不会改动游戏）##time_diag")) {
    if (probe.have_obj) {
      ImGui::Text(u8"GameSpeedFactor [[0xCE8138]+0x30] = %.4f", probe.game_speed_factor);
      ImGui::SameLine();
      ImGui::TextDisabled(u8"（+0x54 = %.4f 是亮度，不写）", probe.brightness);
    } else {
      ImGui::TextDisabled(u8"[[0xCE8138]+0x30/+0x54] 未读到");
    }
    if (probe.session) {
      ImGui::Text(u8"会话对象 [0xCE811C] = 0x%X", probe.session);
      if (probe.have_session_fps) {
        ImGui::SameLine();
        ImGui::Text(u8"| vtbl+0x40（max update FPS） = %d", probe.session_fps);
      } else {
        ImGui::SameLine();
        ImGui::TextDisabled(u8"| vtbl+0x40 未取到");
      }
    } else {
      ImGui::TextDisabled(u8"会话对象 [0xCE811C] 为空（不在对局中）");
    }
    if (ImGui::Button(u8"扫描会话对象字段##time_dump")) {
      game_api::time_control_probe_dump_session();
      g_last_msg = u8"已把会话对象字段扫描写入日志";
    }
    ImGui::SameLine();
    ImGui::TextDisabled(u8"用于以后寻找「真正的减速/暂停」入口");
  }

  // ---- 快捷键（原「时间控制（快捷键）」分组并入此处）----
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (ImGui::CollapsingHeader(u8"快捷键##time_hotkeys")) {
    ImGui::TextDisabled(u8"点右侧齿轮可改键；这些键只负责设倍率/恢复，仍受上面的「写入目标」约束。");
    end_chip_flow();
    end_pair_flow();
    int tk_count = 0;
    const char* const* tk = game_api::time_feature_keys(&tk_count);
    for (int i = 0; i < tk_count; ++i) {
      draw_feature_row(game_api::find_feature(tk[i]));
    }
  }

  // ---- 快进 ----
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(CV(HeadingCol()), u8"快进到指定对局时间");

  float now_s = 0.f;
  const bool have_clock = game_api::battle_clock(&now_s);
  if (have_clock) {
    ImGui::SameLine();
    ImGui::TextDisabled(u8"当前对局时间 %s", fmt_clock(now_s).c_str());
  } else {
    ImGui::SameLine();
    ImGui::TextDisabled(u8"读不到对局时钟");
  }

  static float s_ff_target = 60.f;
  static float s_ff_speed = 4.f;
  static bool  s_ff_auto = false;
  static bool  s_ff_inited = false;
  if (!s_ff_inited) {
    s_ff_auto = game_api::time_control_ff_auto();
    s_ff_inited = true;
  }

  ImGui::SetNextItemWidth(200.f);
  ImGui::InputFloat(u8"目标时间（秒）##ff_target", &s_ff_target, 10.f, 60.f, "%.1f");
  ImGui::SameLine();
  if (ImGui::SmallButton(u8"+1 分##ff_add60")) s_ff_target += 60.f;
  ImGui::SameLine();
  if (have_clock && ImGui::SmallButton(u8"+2 分##ff_add120")) s_ff_target = now_s + 120.f;

  ImGui::SetNextItemWidth(200.f);
  ImGui::SliderFloat(u8"##ff_speed", &s_ff_speed, 1.0f, 8.0f, u8"快进倍率 %.1fx");

  if (ImGui::Checkbox(u8"到达目标时间后自动开始快进##ff_auto", &s_ff_auto)) {
    game_api::time_control_set_ff_auto(s_ff_auto);
  }

  ImGui::Spacing();
  const bool ff_on = game_api::time_control_ff_active();
  if (!ff_on) {
    ImGui::BeginDisabled(!have_clock);
    if (ImGui::Button(u8"快进至目标时间")) {
      game_api::time_control_ff_start(s_ff_target, s_ff_speed);
      s_scale = game_api::time_control_scale();
      s_enabled = true;
      g_last_msg = u8"已开始快进";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(u8"把倍率拉到快进倍率，到达目标时间后自动回到常速。");
  } else {
    ImGui::TextColored(CV(SK().warn), u8"快进中：目标 %s，倍率 %.1fx",
                       fmt_clock(game_api::time_control_ff_target()).c_str(),
                       game_api::time_control_ff_speed());
    if (ImGui::Button(u8"取消快进")) {
      game_api::time_control_ff_cancel();
      s_scale = game_api::time_control_scale();
      s_enabled = game_api::time_control_enabled();
    }
  }
}

// 原「动画 / Lua」分组已并入本面板页，这里提供同一批快捷键的绑定入口。
static void draw_lua_hotkey_section() {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (!ImGui::CollapsingHeader(u8"快捷键##lua_hotkeys")) return;
  ImGui::TextDisabled(u8"点右侧齿轮可改键。");
  end_chip_flow();
  end_pair_flow();
  int n = 0;
  const char* const* keys = game_api::lua_feature_keys(&n);
  for (int i = 0; i < n; ++i) draw_feature_row(game_api::find_feature(keys[i]));
}

void draw_lua_panel() {
  const bool ready = game_api::lua_bridge_ready();  const int hooks = game_api::lua_bridge_hook_state();

  ImGui::TextColored(CV(HeadingCol()), u8"动画 / Lua");
  ImGui::SameLine();
  if (ready) {
    ImGui::TextDisabled(u8"lua_State = 0x%X（Lua API 命中 %d 次）", game_api::lua_bridge_state(),
                        game_api::lua_bridge_api_hits());
  } else if (hooks == 1) {
    ImGui::TextDisabled(u8"挂钩已就绪，等待游戏脚本第一次调用 Lua API…");
  } else {
    ImGui::TextDisabled(u8"未就绪（仅零售版 1.12、且进入对局后可用）");
  }

  if (!ready) {
    ImGui::Spacing();
    ImGui::TextWrapped(u8"这块对应 CameraBridge 的「动画跟踪 + Lua 桥」。原理是挂钩游戏内置的 "
                       u8"Lua 5.0 C API（lua_getglobal / lua_setglobal / lua_type 等），"
                       u8"从第一个参数拿到 lua_State*，之后就能读写脚本的全局变量。");
    ImGui::Spacing();
    ImGui::TextWrapped(u8"请先进入遭遇战对局（脚本开始运行）再回来。如果一直读不到，"
                       u8"请把 ra3_overlay.log 里以 lua: 开头的行发给我。");
    ImGui::TextDisabled(u8"hook 状态：%d（-1 = 尚未安装，1 = 已安装）", hooks);
    draw_lua_hotkey_section();
    return;
  }

  game_api::LuaAnimInfo anim{};
  game_api::lua_anim_read(&anim);

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextDisabled(u8"动画跟踪（原版动画窗口读的就是这几个全局变量，缓存由游戏线程 4Hz 刷新）");

  struct Row {
    const char* name;
    const game_api::LuaGlobal* v;
  };
  const Row rows[] = {
      {"AnimationLuaGetCurrentGameObjectAddress", &anim.game_object_address},
      {"AnimationLuaGetCurrentGameObjectId", &anim.game_object_id},
      {"willSwitchAnimation", &anim.will_switch_animation},
      {"willSwitchPause", &anim.will_switch_pause},
      {"gameTime", &anim.game_time},
      {"relativeTime", &anim.relative_time},
  };

  if (ImGui::BeginTable("##lua_anim", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(u8"Lua 全局变量", ImGuiTableColumnFlags_WidthFixed, 330.f);
    ImGui::TableSetupColumn(u8"类型", ImGuiTableColumnFlags_WidthFixed, 110.f);
    ImGui::TableSetupColumn(u8"值", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const Row& r : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.name);
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", game_api::lua_type_name(r.v->type));
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.v->text[0] ? r.v->text : "-");
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  if (ImGui::Button(u8"重新读取动画变量（写日志）")) {
    std::string msg;
    game_api::lua_bridge_engine_cmd("lua_probe", nullptr, &msg);
    g_last_msg = msg;
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"输出 Lua 状态指针")) {
    std::string msg;
    game_api::lua_bridge_engine_cmd("lua_dump_state", nullptr, &msg);
    g_last_msg = msg;
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(CV(HeadingCol()), u8"读写任意 Lua 全局变量");
  ImGui::TextDisabled(u8"对应原版 RA3LuaBridge 暴露的 getglobal / setglobal。");

  static char s_name[128] = "willSwitchAnimation";
  static char s_value[256] = "1";
  static std::string s_result;

  ImGui::Spacing();
  ImGui::SetNextItemWidth(320.f);
  ImGui::InputText(u8"变量名##lua_name", s_name, sizeof(s_name));
  ImGui::SetNextItemWidth(320.f);
  ImGui::InputText(u8"值##lua_value", s_value, sizeof(s_value));

  ImGui::Spacing();
  if (ImGui::Button(u8"读取")) {
    if (game_api::lua_request_read(s_name)) {
      s_result = u8"已请求读取：游戏线程执行后在这里显示结果（同时写日志）";
    } else {
      s_result = u8"请求失败（队列满 / 名字过长）";
    }
  }
  // 异步读取结果：游戏线程执行完后在这里取回（每帧 poll，只取一次）。
  {
    game_api::LuaGlobal v{};
    char got_name[128] = {};
    if (game_api::lua_read_take(&v, got_name, sizeof(got_name))) {
      char buf[360];
      std::snprintf(buf, sizeof(buf), u8"%s → 类型 %s，值 %s", got_name,
                    game_api::lua_type_name(v.type), v.text[0] ? v.text : "<空>");
      s_result = buf;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"写字符串")) {
    s_result = game_api::lua_global_write_string(s_name, s_value)
                   ? u8"已提交字符串（游戏线程下一帧执行，见日志）"
                   : u8"提交失败（队列满 / 名字过长）";
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"写数字")) {
    const double n = std::atof(s_value);
    s_result = game_api::lua_global_write_number(s_name, n)
                   ? u8"已提交数字（游戏线程下一帧执行，见日志）"
                   : u8"提交失败（队列满 / 名字过长）";
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"置为 nil")) {
    s_result = game_api::lua_global_write_nil(s_name)
                   ? u8"已提交 nil（游戏线程下一帧执行，见日志）"
                   : u8"提交失败（队列满 / 名字过长）";
  }

  if (!s_result.empty()) ImGui::TextWrapped("%s", s_result.c_str());

  ImGui::Spacing();
  ImGui::TextDisabled(u8"读写都在游戏线程执行（渲染线程只读快照/排队），待执行的写入：%d",
                      game_api::lua_pending_writes());

  ImGui::Spacing();
  ImGui::TextWrapped(u8"注意：这个定制版 Lua 的类型表里没有独立的 boolean 标签"
                     u8"（只有 userdata / nil / number / string / table / function）。"
                     u8"如果脚本里的开关量实际是布尔值，写数字不一定会被当成真/假，"
                     u8"请先「读取」看它原本的类型，再决定写什么。");
  draw_lua_hotkey_section();
}

static void draw_misc_panel() {
  game_api::MiscState st{};
  game_api::misc_read(&st);

  const bool any_ok = st.flags_ok || st.shadow_size_ok || st.shadow_dist_ok || st.light.ok;

  ImGui::TextColored(CV(HeadingCol()), u8"杂项操作");
  ImGui::SameLine();
  if (!any_ok) {
    ImGui::TextDisabled(u8"未读到（仅零售版 1.12、且进入对局后可用）");
  } else if (!st.flags_ok || !st.light.ok) {
    ImGui::TextDisabled(u8"部分可用");
  } else {
    ImGui::TextDisabled(u8"就绪");
  }
  ImGui::Separator();
  ImGui::Spacing();

  // 这三项也持久化：优先用 ini 里保存的意图，读不到才取游戏当前值。
  bool s_hide_hp = g_misc_hide_hp;
  bool s_hide_sil = g_misc_hide_sil;
  bool s_take = g_misc_take;
  static bool s_ui_inited = false;
  if (!s_ui_inited && st.flags_ok) {
    if (!s_hide_hp && !st.show_health) s_hide_hp = true;
    if (!s_hide_sil && !st.show_silhouette) s_hide_sil = true;
    s_ui_inited = true;
  }

  ImGui::TextColored(CV(HeadingCol()), u8"隐藏显示");
  ImGui::Spacing();
  {
    std::string msg;
    ImGui::BeginDisabled(!st.flags_ok);
    if (ImGui::Checkbox(u8"不显示血条和维修标志", &s_hide_hp)) {
      g_misc_hide_hp = s_hide_hp;
      mark_settings_dirty();
      game_api::misc_set_hide_health(s_hide_hp, s_take, &msg);
    }
    if (ImGui::Checkbox(u8"不显示建筑背后的单位剪影", &s_hide_sil)) {
      g_misc_hide_sil = s_hide_sil;
      mark_settings_dirty();
      game_api::misc_set_hide_silhouette(s_hide_sil, s_take, &msg);
    }
    if (ImGui::Checkbox(u8"每帧保持（换局、改画质后仍生效）", &s_take)) {
      g_misc_take = s_take;
      mark_settings_dirty();
      game_api::misc_set_hide_health(s_hide_hp, s_take, &msg);
      game_api::misc_set_hide_silhouette(s_hide_sil, s_take, &msg);
    }
    ImGui::EndDisabled();
    if (!msg.empty()) g_last_msg = msg;
  }
  if (!st.flags_ok) {
    ImGui::TextDisabled(u8"没读到渲染对象 [0xCE08F0]，先进入对局再回来。");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(CV(HeadingCol()), u8"阴影");
  ImGui::Spacing();

  // 下拉/滑条显示「想要的值」：优先 ini 里保存的，其次才是游戏当前值。
  // 这样重开游戏后不用再设一遍——tick 会在每一局进场后自动下发。
  const int s_size = (g_misc_shadow_size > 0)
                         ? g_misc_shadow_size
                         : (st.shadow_size_ok ? st.shadow_size : 1024);
  static float s_dist = 0.f;
  static bool s_dist_inited = false;
  if (!s_dist_inited) {
    if (g_misc_shadow_dist > 0.f) {
      s_dist = g_misc_shadow_dist;
      s_dist_inited = true;
    } else if (st.shadow_dist_ok) {
      s_dist = st.shadow_dist;
      s_dist_inited = true;
    }
  }

  {
    const char* items[] = {u8"1024（默认）", u8"2048", u8"4096", u8"8192"};
    const int kSizes[4] = {1024, 2048, 4096, 8192};
    int cur = 0;
    for (int i = 0; i < 4; ++i) {
      if (s_size == kSizes[i]) cur = i;
    }
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::Combo(u8"阴影贴图大小##misc_size", &cur, items, 4)) {
      g_misc_shadow_size = kSizes[cur];
      g_misc_shadow_take = true;
      mark_settings_dirty();
      std::string msg;
      game_api::misc_set_shadow_size(kSizes[cur], &msg);
      if (msg.empty()) g_last_msg = u8"阴影贴图大小已保存，重新开始一局后生效";
      else g_last_msg = msg + u8"（已保存，下一局自动下发）";
    }
    ImGui::SameLine();
    ImGui::TextDisabled(u8"（1024/2048/4096/8192，重新开始一局后生效，设置会保存）");
  }

  ImGui::SetNextItemWidth(320.f);
  ImGui::SliderFloat(u8"最大阴影距离##misc_dist", &s_dist, 0.f, 20000.f, u8"%.0f");
  // 松手后再写入，避免拖动时每帧改全局、退局拆设备时卡死。
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    g_misc_shadow_dist = s_dist;
    g_misc_shadow_take = true;
    mark_settings_dirty();
    std::string msg;
    game_api::misc_set_shadow_distance(s_dist, &msg);
    if (!msg.empty()) g_last_msg = msg;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(u8"默认##misc_dist_def")) {
    s_dist = 1000.f;
    g_misc_shadow_dist = s_dist;
    g_misc_shadow_take = true;
    mark_settings_dirty();
    std::string msg;
    game_api::misc_set_shadow_distance(s_dist, &msg);
    if (!msg.empty()) g_last_msg = msg;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(u8"应用##misc_dist_apply")) {
    g_misc_shadow_dist = s_dist;
    g_misc_shadow_take = true;
    mark_settings_dirty();
    std::string msg;
    game_api::misc_set_shadow_distance(s_dist, &msg);
    if (!msg.empty()) g_last_msg = msg;
  }
  if (!st.shadow_size_ok && !st.shadow_dist_ok) {
    ImGui::TextDisabled(u8"没读到阴影全局 [0xCEEDB4]/[0xCEEDB8]，先进入对局再回来。");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(CV(HeadingCol()), u8"光照颜色");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"点击色块打开调色盘（1.000 = 255）");

  static float s_light[4][3] = {};
  static bool s_light_inited = false;
  if (!s_light_inited && st.light.ok) {
    const float* src[4] = {st.light.ambient, st.light.main, st.light.tint1, st.light.tint2};
    for (int g = 0; g < 4; ++g) {
      for (int c = 0; c < 3; ++c) s_light[g][c] = src[g][c];
    }
    s_light_inited = true;
  }

  ImGui::Spacing();
  static bool s_lock = true;
  if (ImGui::Checkbox(u8"每帧保持光照颜色", &s_lock)) {
    game_api::misc_set_light_lock(s_lock);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(u8"恢复默认光照##misc_light_reset")) {
    game_api::misc_restore_lights();
    s_light_inited = false;
    s_lock = false;
    g_last_msg = u8"杂项：光照颜色已恢复默认";
  }

  ImGui::Spacing();
  static const char* kLightLabel[4] = {u8"环境光颜色##misc_l0", u8"主光源颜色##misc_l1",
                                       u8"调色颜色 #1##misc_l2", u8"调色颜色 #2##misc_l3"};
  // 只显示色块，点开就是调色盘；不再并排三个 RGB 滑块。
  const ImGuiColorEditFlags light_flags =
      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_NoInputs |
      ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayHex;
  ImGui::BeginDisabled(!st.light.ok);
  for (int g = 0; g < 4; ++g) {
    if (ImGui::ColorEdit3(kLightLabel[g], s_light[g], light_flags)) {
      std::string msg;
      game_api::misc_set_light(g, s_light[g], &msg);
      if (!msg.empty()) g_last_msg = msg;
    }
  }
  ImGui::EndDisabled();
  if (!st.light.ok) {
    ImGui::TextDisabled(u8"没读到光照结构 [[0xCE0D78]+0x24]，进入对局后可用。");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextDisabled(u8"说明：血条 / 剪影是渲染对象 [0xCE08F0] 的 +0x139 / +0x138 两个字节；"
                      u8"光照结构在 [[0xCE0D78]+0x24]；阴影贴图大小与最大阴影距离在 "
                      u8"[0xCEEDB4] / [0xCEEDB8]。写入前都会校验指针与当前值。"
                      u8"阴影贴图大小需要重新开始一局才会重建阴影贴图。");
}

static void draw_unit_inspector() {
  static bool s_edit_open = false;
  static bool s_open_popup = false;
  static char s_edit_key[96] = {};
  static char s_edit_name[96] = {};
  static std::string s_edit_msg;
  static int s_sel_total = 0;
  static double s_sel_t = -1.0;

  // Header only needs a cheap selection count; full inspect waits until open.
  const double now = ImGui::GetTime();
  if (s_sel_t < 0.0 || (now - s_sel_t) >= 0.35) {
    s_sel_total = game_api::selected_count();
    s_sel_t = now;
  }

  char header[96];
  if (s_sel_total > 1) {
    std::snprintf(header, sizeof(header), u8"单位属性（已选 %d）###unit_insp", s_sel_total);
  } else if (s_sel_total == 1) {
    std::snprintf(header, sizeof(header), u8"单位属性（已选 1）###unit_insp");
  } else {
    std::snprintf(header, sizeof(header), u8"单位属性（未选中）###unit_insp");
  }

  ImGui::SetNextItemOpen(g_unit_inspect_open, ImGuiCond_Always);
  const bool open = ImGui::CollapsingHeader(header);
  if (ImGui::IsItemToggledOpen()) {
    g_unit_inspect_open = open;
    mark_settings_dirty();
    g_settings_save_timer = 0.8f;
    // Shrink / grow the host window with the inspector body.
    constexpr float kBody = 220.f;
    ImVec2 sz = ImGui::GetWindowSize();
    if (open) {
      sz.y += kBody;
    } else {
      sz.y -= kBody;
      if (sz.y < 400.f) sz.y = 400.f;
    }
    ImGui::SetWindowSize(sz);
  }
  if (!open) return;

  game_api::UnitInspect info{};
  game_api::inspect_first_selected(&info);
  if (info.selected_total != s_sel_total) {
    s_sel_total = info.selected_total;
    s_sel_t = now;
  }

  ImGui::PushStyleColor(ImGuiCol_ChildBg, CVA(SK().card0, 0.80f));
  ImGui::BeginChild("##inspect", ImVec2(0, ui_px(210.f)), ImGuiChildFlags_Borders);

  if (!info.valid) {
    ImGui::TextWrapped(u8"在游戏中选中单位后，这里显示其实体指针、血量、速度、星级等。");
    if (const char* path = game_api::unit_names_store_path()) {
      ImGui::TextDisabled(u8"种类名文件: %s", path);
    }
  } else {
    ImGui::Columns(2, "##inspcols", false);
    ImGui::SetColumnWidth(0, 400.f);

    ImGui::Text(u8"实体  0x%08X", info.ent);

    ImGui::TextUnformatted(u8"种类");
    ImGui::SameLine();
    if (info.type_name[0]) {
      ImGui::TextUnformatted(info.type_name);
    } else if (info.type_id[0]) {
      ImGui::TextUnformatted(u8"（未收录中文名）");
    } else {
      ImGui::TextDisabled(u8"（未能解析）");
    }
    if (info.name_key[0]) {
      ImGui::SameLine();
      if (ImGui::SmallButton(u8"修改")) {
        std::snprintf(s_edit_key, sizeof(s_edit_key), "%s", info.name_key);
        if (info.type_name[0]) {
          std::snprintf(s_edit_name, sizeof(s_edit_name), "%s", info.type_name);
        } else {
          s_edit_name[0] = 0;
        }
        s_edit_msg.clear();
        s_edit_open = true;
        s_open_popup = true;
      }
    }
    if (info.type_id[0]) {
      ImGui::TextDisabled(u8"代号  %s", info.type_id);
    } else if (info.name_key[0]) {
      if (std::strncmp(info.name_key, "tpl_", 4) == 0) {
        ImGui::TextDisabled(u8"键名  %s（地址键，重启后失效）", info.name_key);
      } else if (std::strncmp(info.name_key, "sig_", 4) == 0) {
        ImGui::TextDisabled(u8"键名  %s（指纹键）", info.name_key);
      } else {
        ImGui::TextDisabled(u8"键名  %s", info.name_key);
      }
    }

    ImGui::Text(u8"模板  0x%08X", info.unit_data);
    if (info.owner) {
      ImGui::Text(u8"归属  0x%08X  %s", info.owner,
                  info.is_mine ? u8"[己方]" : u8"[其他]");
    } else {
      ImGui::TextDisabled(u8"归属  （无效）");
    }

    ImGui::NextColumn();

    if (info.has_hp) {
      const bool over = info.hp_max > 0.f && info.hp_cur > info.hp_max * 1.01f;
      if (over) {
        ImGui::Text(u8"血量  已锁满（上限 %.0f）", info.hp_max);
      } else {
        ImGui::Text(u8"血量  %.0f / %.0f", info.hp_cur, info.hp_max);
      }
      if (info.hp_max > 0.f) {
        float frac = info.hp_cur / info.hp_max;
        if (frac < 0.f) frac = 0.f;
        if (frac > 1.f) frac = 1.f;
        ImGui::ProgressBar(frac, ImVec2(-1, 14), nullptr);
      }
    } else {
      ImGui::TextDisabled(u8"血量  （无组件）");
    }

    if (info.has_speed) {
      ImGui::Text(u8"速度  %.2f", info.speed);
    } else {
      ImGui::TextDisabled(u8"速度  （无组件/建筑）");
    }

    if (info.has_rank) {
      ImGui::Text(u8"星级  %u   经验 %.0f / %.0f", info.rank_level, info.xp,
                  info.xp_next);
      ImGui::Text(u8"加成  ×%.2f", info.damage_mult);
    } else {
      ImGui::TextDisabled(u8"星级  （无组件）");
    }

    if (info.has_damage_type) {
      ImGui::Text(u8"攻击  %s", info.damage_type_zh);
      ImGui::TextDisabled(u8"类型  %s（护甲克制）", info.damage_type);
    } else {
      ImGui::TextDisabled(u8"攻击  （未能识别伤害类型）");
    }
    ImGui::TextDisabled(u8"单发伤害数值在武器资源内，尚未从内存解析");

    ImGui::Columns(1);

    if (const char* path = game_api::unit_names_store_path()) {
      ImGui::TextDisabled(u8"种类名文件: %s", path);
    }
  }

  if (s_open_popup) {
    ImGui::OpenPopup(u8"修改种类名");
    s_open_popup = false;
  }
  ImGui::SetNextWindowSize(ImVec2(460, 220), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal(u8"修改种类名", &s_edit_open, ImGuiWindowFlags_NoResize)) {
    ImGui::Text(u8"保存键（不可改）");
    ImGui::TextDisabled("%s", s_edit_key);
    ImGui::Spacing();
    ImGui::TextUnformatted(u8"显示名");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##edit_unit_name", s_edit_name, sizeof(s_edit_name));
    if (!s_edit_msg.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", s_edit_msg.c_str());
    }
    ImGui::Dummy(ImVec2(0, 12));
    if (ImGui::Button(u8"保存", ImVec2(ui_px(120.f), 0))) {
      std::string msg;
      if (game_api::set_unit_display_name(s_edit_key, s_edit_name, &msg)) {
        g_last_msg = msg;
        game_api::beep("click");
        s_edit_open = false;
        ImGui::CloseCurrentPopup();
      } else {
        s_edit_msg = msg;
        game_api::beep("error");
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"取消", ImVec2(ui_px(120.f), 0))) {
      s_edit_open = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// 单位操作扩展（对应 CameraBridge 的「选择哪些单位 / 切换单位的队伍 /
// 在游戏中禁用输入」）。挂在「单位操作」页底部。
// ---------------------------------------------------------------------------
static uint32_t g_unit_scope_player_ui = 0;
static uint32_t g_unit_team_player_ui = 0;

static void draw_unit_ops_extra() {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(CV(HeadingCol()), u8"单位操作扩展");

  // ---- 在游戏中禁用输入 ----
  bool input_off = game_api::game_input_disabled();
  if (ImGui::Checkbox(u8"在游戏中禁用鼠标输入##no_game_mouse", &input_off)) {
    game_api::set_game_input_disabled(input_off);
    game_api::beep("click");
  }
  ImGui::SameLine();
  ImGui::TextDisabled(u8"（拍运镜时不让游戏响应悬停 / 点击）");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ---- 选择哪些单位 ----
  static const char* kScopeLabels[4] = { u8"选中的单位", u8"未选中的单位", u8"所有的单位",
                                         u8"指定玩家的单位" };
  int scope = game_api::unit_scope();
  if (scope < 0 || scope > 3) scope = game_api::kUnitScopeSelected;

  ImGui::TextUnformatted(u8"选择哪些单位");
  ImGui::SetNextItemWidth(200.f);
  if (ImGui::BeginCombo("##unit_scope", kScopeLabels[scope])) {
    for (int i = 0; i < 4; ++i) {
      if (ImGui::Selectable(kScopeLabels[i], scope == i)) {
        game_api::set_unit_scope(i);
        game_api::beep("click");
      }
    }
    ImGui::EndCombo();
  }

  if (scope == game_api::kUnitScopePlayer) {
    ImGui::TextDisabled(u8"选择玩家范围（默认本地玩家）");
    const uint32_t who = draw_player_picker("unit_scope", &g_unit_scope_player_ui);
    game_api::set_unit_scope_player(who);
  }

  static uint32_t s_scope_buf[512];
  static int s_scope_n = 0;
  static double s_scope_t = -1.0;
  const double scope_now = ImGui::GetTime();
  if (s_scope_t < 0.0 || (scope_now - s_scope_t) >= 0.25) {
    s_scope_n = game_api::unit_collect_scope(s_scope_buf, 512);
    s_scope_t = scope_now;
  }
  const int n = s_scope_n;
  ImGui::SameLine();
  ImGui::TextDisabled(u8"当前范围命中 %d 个单位", n);

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ---- 切换单位的队伍 ----
  ImGui::TextUnformatted(u8"切换单位的队伍");
  ImGui::TextDisabled(u8"把上面所选范围内的单位改到指定玩家名下（不改模型，只改归属）");
  const uint32_t target = draw_player_picker("unit_team", &g_unit_team_player_ui);

  static bool s_enable_ai = true;
  if (ImGui::Checkbox(u8"启用 AI 操控##unit_team_ai", &s_enable_ai)) {
    // 勾上时立刻把之前被压成 0 的速度还回来，不必再切一次队伍才动。
    if (s_enable_ai) {
      std::string msg;
      if (game_api::unit_thaw_team_freeze(target, &msg) && !msg.empty()) g_last_msg = msg;
    }
    game_api::beep("click");
  }
  ImGui::SameLine();
  ImGui::TextDisabled(u8"不勾选时：改完归属顺手把单位速度压成 0，免得 AI 把它调走");

  ImGui::Spacing();
  ImGui::BeginDisabled(target == 0 || n <= 0);
  if (ImGui::Button(u8"把选中单位切换给该玩家")) {
    std::string msg;
    const bool ok = game_api::unit_change_team_ex(target, s_enable_ai, &msg);
    g_last_msg = msg;
    game_api::beep(ok ? "click" : "error");
  }
  ImGui::EndDisabled();
  if (n <= 0) {
    ImGui::SameLine();
    ImGui::TextDisabled(u8"范围内没有单位（先选中单位，或把范围改成「所有的单位」）");
  }
  ImGui::TextDisabled(u8"注意：改变队伍会让正在录制的录像不同步。");
}

void pump_pending_settings() {
  if (!g_misc_restore_pending) return;
  game_api::MiscPersist p{};
  p.shadow_take = g_misc_shadow_take;
  p.shadow_size = g_misc_shadow_size;
  p.shadow_dist = g_misc_shadow_dist;
  p.take = g_misc_take;
  p.hide_health = g_misc_hide_hp;
  p.hide_silhouette = g_misc_hide_sil;
  p.light_lock = g_misc_light_lock;
  std::string msg;
  if (!game_api::misc_apply_persist(p, &msg)) return;  // 杂项还没装好，下一帧再来
  g_misc_restore_pending = false;
  game_api::crate_select(g_crate_kind_persist, g_crate_count_persist);
  if (!msg.empty()) g_last_msg = msg;
}

void draw() {
  if (!g_inited) return;
  // 淡出期间菜单已经不可见，但还要再画 0.16 秒把它淡完。
  if (!input::menu_visible() && !(g_stats_open && g_stats_pinned) && s_menu_fade <= 0.f) return;

  // 字体自检：中文全变 '?' 说明字形没栅格化出来（ImGui 会退回 fallback '?'）。
  // 汉字是等宽的，'中' 的宽度必然明显大于 'A'；两个一样宽就说明拿到的是替换字符。
  // 只在前几帧和每隔 ~10 秒打一行，开销可忽略。
  {
    static int s_probe = 0;
    ++s_probe;
    if (s_probe <= 3 || s_probe % 600 == 0) {
      const ImVec2 a = ImGui::CalcTextSize("A");
      const ImVec2 cjk = ImGui::CalcTextSize(u8"中");
      const float ratio = (a.x > 0.01f) ? cjk.x / a.x : 0.f;
      game_api::log("font probe: ascii=%.2f cjk=%.2f ratio=%.2f fonts=%d %s", a.x, cjk.x, ratio,
                    ImGui::GetIO().Fonts->Fonts.Size,
                    (ratio < 1.3f) ? "CJK MISSING (rendering '?')" : "cjk ok");
    }
  }

  g_sync_timer += ImGui::GetIO().DeltaTime;
  // 皮肤过渡：切换后的 ~0.26 秒内逐帧重建 style，整窗平滑换色。
  skin::Update(ImGui::GetIO().DeltaTime);
  if (skin::Transitioning()) apply_ra3_theme();
  if (g_sync_timer > 1.0f) {
    g_sync_timer = 0.f;
    game_api::sync_spectator_hooks();
  }

  if (g_settings_dirty) {
    g_settings_save_timer -= ImGui::GetIO().DeltaTime;
    if (g_settings_save_timer <= 0.f) {
      save_ui_settings();
    }
  }

  if (g_async_cs_ready) {
    EnterCriticalSection(&g_async_cs);
    if (g_async_result.done) {
      g_last_msg = g_async_result.msg;
      g_async_result.done = false;
    }
    LeaveCriticalSection(&g_async_cs);
  }

  if (input::menu_visible()) {
    poll_hotkey_capture();
  } else if (g_capture_slot >= 0) {
    g_capture_slot = -1;
    g_capture_ready = false;
    game_api::set_hotkey_capture(false);
  }

  // ---- 整窗淡入淡出 ----
  // 菜单隐藏时不是"啪"地消失：0.16 秒淡到透明再停画，和皮肤过渡是同一套观感。
  // 淡出一半时菜单仍要接受输入（否则会有一瞬间点了没反应），所以整段逻辑不动，
  // 只是把颜色和 ImGuiStyle 的 alpha 一起压下去。
  {
    const bool want = input::menu_visible();
    const float dt = (std::max)(0.f, (std::min)(0.1f, ImGui::GetIO().DeltaTime));
    const float k = 1.f - std::exp(-(want ? 18.f : 26.f) * dt);
    s_menu_fade += ((want ? 1.f : 0.f) - s_menu_fade) * k;
    if (s_menu_fade < 0.004f) s_menu_fade = 0.f;
    if (s_menu_fade > 0.996f) s_menu_fade = 1.f;
    // 菜单彻底淡完再清拖动状态：否则窗口隐藏期间点击旧标题栏位置，会攒出一个
    // 幽灵位移，下次打开菜单窗口直接跳一下。
    if (s_menu_fade <= 0.f) shell::ResetDrag("menu");
  }

  if (s_menu_fade > 0.f) {
    const float fade = s_menu_fade;
    // 外壳是自绘的，取色来自皮肤：把淡出乘进皮肤槽位，几十个控件一起淡。
    skin::SetFade(fade);
    // 原生控件（文字 / 按钮 / 子窗口）走 ImGui 自己的 alpha。
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

    // Collapsed inspector frees ~220px; allow a shorter window in that state.
    const float kInspectBodyH = ui_px(220.f);
    const float win_min_h = g_unit_inspect_open ? 620.f : 400.f;
    const ImVec2 win_def = scaled_window_size(900.f, 720.f);
    // 位置在 Begin 之前定好：拖动位移先吃掉，窗口和内容同一帧移动。
    static ImVec2 s_menu_pos(28.f, 28.f);
    shell::DragUpdate("menu", &s_menu_pos);
    ImGui::SetNextWindowPos(s_menu_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(win_def, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2((std::min)(ui_px(780.f), win_def.x), (std::min)(ui_px(win_min_h), win_def.y)),
        ImVec2(FLT_MAX, FLT_MAX));

    // 标题栏 / 外框自己画：NoTitleBar 去掉 ImGui 的直角标题条，
    // NoBackground 去掉它的矩形底色，整块外壳由 ui::shell::Draw 负责。
    // NoMove：位移由 shell::DragUpdate 在 Begin 之前算，ImGui 不要再从空白处
    // 拖一次，否则两处位移叠加成两倍速。
    ImGui::Begin(u8"红色警戒3 · 进程内菜单", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove);

    shell::Hit shell_hit;
    const ImVec2 content0 = ImGui::GetCursorScreenPos();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float shell_h = shell::Draw(ImGui::GetWindowDrawList(), wp,
                                      ImVec2(wp.x + ImGui::GetWindowSize().x,
                                             wp.y + ImGui::GetWindowSize().y),
                                      g_ui_scale, g_ui_opacity, u8"红色警戒3", u8" · 进程内菜单",
                                      true, &shell_hit, true, "menu");
    // 正文从标题栏下面开始。横向沿用 WindowPadding，纵向让开外壳。
    ImGui::SetCursorScreenPos(ImVec2(content0.x, wp.y + shell_h + ui_px(6.f)));
    if (shell_hit.close) input::set_menu_visible(false);

    // 顶部状态 + 按钮行。界面放大到 2 倍以上时这一行放不下，逐项检查剩余宽度，
    // 放不下就换行 —— 否则"隐藏""调试"会被窗口右边缘直接裁掉，点不到。
    auto wrap_if_fits = [](float next_w) {
      const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
      if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_w <= right) {
        ImGui::SameLine();
      }
    };
    auto btn_w = [](const char* label) {
      return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.f;
    };
    auto chk_w = [](const char* label) {
      return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
             ImGui::CalcTextSize(label).x;
    };
    const char* inject_label = game_api::hooks_armed() ? u8"重新注入" : u8"注入";

    ImGui::TextColored(skin::Vec4(skin::Active().accent2), u8"RA3 Overlay");
    wrap_if_fits(ui_px(220.f));
    {
      static int s_sel_n = 0;
      static double s_sel_t = -1.0;
      const double t = ImGui::GetTime();
      if (s_sel_t < 0.0 || (t - s_sel_t) >= 0.35) {
        s_sel_n = game_api::selected_count();
        s_sel_t = t;
      }
      ImGui::TextDisabled(u8"|  %s  |  选中 %d", game_api::mode_label(), s_sel_n);
    }

    const bool busy = InterlockedCompareExchange(&g_injecting, 0, 0) != 0;
    wrap_if_fits((std::max)(ui_px(100.f), btn_w(inject_label)));
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button(inject_label, ImVec2(ui_px(100.f), 0))) {
      if (InterlockedCompareExchange(&g_injecting, 1, 0) == 0) {
        g_last_msg = u8"正在注入…";
        g_async_result.done = false;
        HANDLE th = CreateThread(nullptr, 0, inject_worker,
                                 g_spectate_inject ? (LPVOID)1 : nullptr, 0, nullptr);
        if (th) CloseHandle(th);
        else {
          InterlockedExchange(&g_injecting, 0);
          g_last_msg = u8"无法创建线程";
        }
      }
    }
    if (busy) ImGui::EndDisabled();
    wrap_if_fits(ui_px(60.f));
    if (ImGui::Button(u8"卸载", ImVec2(ui_px(60.f), 0))) {
      game_api::detach();
      g_last_msg = u8"已还原 hook";
      game_api::beep("off");
    }
    wrap_if_fits(chk_w(u8"观战模式"));
    ImGui::Checkbox(u8"观战模式", &g_spectate_inject);
    wrap_if_fits(ui_px(60.f));
    if (ImGui::Button(u8"设置", ImVec2(ui_px(60.f), 0))) {
      g_show_settings = true;
    }
    wrap_if_fits(ui_px(60.f));
    if (ImGui::Button(u8"统计", ImVec2(ui_px(60.f), 0))) {
      g_stats_open = true;
    }
    wrap_if_fits(ui_px(60.f));
    if (ImGui::Button(u8"隐藏", ImVec2(ui_px(60.f), 0))) input::set_menu_visible(false);
    wrap_if_fits(chk_w(u8"调试"));
    ImGui::Checkbox(u8"调试", &g_show_debug_status);
    if (ImGui::IsItemHovered()) {
      Tip(u8"在下方固定高度的信息框内显示底层状态；默认关闭，避免长日志把窗口撑高跳动。");
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          skin::Vec4A(skin::Active().sidebar1, g_ui_opacity));
    // 固定高度：内容超出时在框内滚动，绝不改变主窗口布局。
    const float msg_h = ui_px(g_show_debug_status ? 72.f : 44.f);
    ImGui::BeginChild("##msg", ImVec2(0, msg_h), true,
                      ImGuiWindowFlags_None);
    if (g_show_debug_status) {
      ImGui::PushStyleColor(ImGuiCol_Text, skin::Vec4(skin::Active().text2));
      ImGui::TextUnformatted(game_api::status_text());
      ImGui::PopStyleColor();
      ImGui::Separator();
    }
    ImGui::TextWrapped("%s", g_last_msg.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    int group_count = 0;
    const game_api::GroupInfo* groups = game_api::groups(&group_count);
    const float inspect_header_h = ImGui::GetFrameHeightWithSpacing() + ui_px(6.f);
    const float inspect_reserve =
        g_unit_inspect_open ? (inspect_header_h + kInspectBodyH) : inspect_header_h;
    // 侧栏宽度必须跟着界面缩放走：3 倍缩放下 156px 放不下"单位操作"四个字，
    // 会直接把导航文字裁掉。
    const float side_w = ui_px(156.f);

    ImGui::BeginChild("##side", ImVec2(side_w, -inspect_reserve), true);
    for (int i = 0; i < group_count; ++i) {
      const bool sel = (!g_show_settings && !g_show_build_lock && !g_show_protocol_lock &&
                        !g_show_camera && !g_show_time && !g_show_lua && !g_show_misc &&
                        g_group == i);
      ImGui::PushID(i);
      if (draw_nav_item(groups[i].name, groups[i].name, sel)) {
        g_group = i;
        g_show_settings = false;
        g_show_build_lock = false;
        g_show_protocol_lock = false;
        g_show_camera = false;
        g_show_time = false;
        g_show_lua = false;
        g_show_misc = false;
      }
      ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (draw_nav_item("stats", u8"战况统计", g_stats_open)) {
      if (!g_stats_open) {
        g_stats_open = true;
      } else if (!g_stats_pinned) {
        g_stats_open = false;
      }
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_protocol_lock = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("lock", u8"建造限制", g_show_build_lock)) {
      g_show_build_lock = true;
      g_show_settings = false;
      g_show_protocol_lock = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("proto", u8"协议限制", g_show_protocol_lock)) {
      g_show_protocol_lock = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("cam", u8"摄像机", g_show_camera)) {
      g_show_camera = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_protocol_lock = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("time", u8"时间控制", g_show_time)) {
      g_show_time = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_protocol_lock = false;
      g_show_camera = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("lua", u8"动画 / Lua", g_show_lua)) {
      g_show_lua = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_protocol_lock = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_misc = false;
    }
    if (draw_nav_item("misc", u8"杂项操作", g_show_misc)) {
      g_show_misc = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_protocol_lock = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
    }
    if (draw_nav_item("ui", u8"界面设置", g_show_settings)) {
      g_show_settings = true;
      g_show_build_lock = false;
      g_show_protocol_lock = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##body", ImVec2(0, -inspect_reserve), true);
    if (g_show_settings) {
      draw_settings_panel();
    } else if (g_show_build_lock) {
      draw_build_lock_panel();
    } else if (g_show_protocol_lock) {
      draw_protocol_lock_panel();
    } else if (g_show_camera) {
      draw_camera_panel();
    } else if (g_show_time) {
      draw_time_panel();
    } else if (g_show_lua) {
      draw_lua_panel();
    } else if (g_show_misc) {
      draw_misc_panel();
    } else if (g_group >= 0 && g_group < group_count) {
      const auto& g = groups[g_group];
      ImGui::TextColored(skin::Vec4(skin::Active().accent2), "%s", g.name);
      if (g.subtitle) ImGui::TextDisabled("%s", g.subtitle);
      ImGui::Separator();
      ImGui::Spacing();
      end_chip_flow();
      end_pair_flow();
      const bool unit_page = std::strcmp(g.name, u8"单位操作") == 0;
      const bool sw_page = std::strcmp(g.name, u8"超武 / 地图") == 0;
      const bool res_page = std::strcmp(g.name, u8"资源") == 0;
      for (int i = 0; i < g.key_count; ++i) {
        const game_api::FeatureInfo* feat = game_api::find_feature(g.keys[i]);
        const bool unit_full =
            feat && (std::strcmp(feat->key, "spawn_mcv") == 0 ||
                     std::strcmp(feat->key, "spawn_rank") == 0 ||
                     std::strcmp(feat->key, "summon_troops") == 0 ||
                     std::strcmp(feat->key, "invincible") == 0 ||
                     std::strcmp(feat->key, "airdrop") == 0 ||
                     // 危险等级是三段滑条，得整行画
                     std::strcmp(feat->key, "danger") == 0);
        const bool money_cell = feat && std::strcmp(feat->type, "money") == 0;
        const bool money_pick = feat && std::strcmp(feat->key, "money_sel") == 0;
        if (unit_page && feat && !unit_full) {
          draw_compact_cell(feat, 3);
        } else if (sw_page && feat) {
          draw_compact_cell(feat, 3);
        } else if (res_page && money_cell) {
          draw_money_cell(feat, 3);
        } else if (res_page && feat && !money_pick) {
          draw_compact_cell(feat, 3);
        } else {
          if (unit_page || sw_page || res_page) end_pair_flow();
          draw_feature_row(feat);
        }
      }
      if (unit_page) {
        end_pair_flow();
        draw_unit_ops_extra();
      }
    }
    ImGui::EndChild();

    draw_unit_inspector();

    ImGui::End();
    ImGui::PopStyleVar();
    skin::SetFade(1.f);
  }

  draw_stats_window();
}

// Ctrl+F12：把画完 overlay 的后备缓冲导成 BMP，放在 DLL 旁边。
//
// 投影、边缘分层、滚动条到底长什么样，这类问题靠描述对不齐；直接看像素最省事。
// 只用 D3D9 的 GetRenderTargetData + 一个 24 位 BMP 头，不引额外依赖。
static void dump_back_buffer(IDirect3DDevice9* device) {
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
  D3DSURFACE_DESC desc{};
  bb->GetDesc(&desc);
  if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) {
    game_api::log("ui: dump skipped, back buffer format=%d", (int)desc.Format);
    bb->Release();
    return;
  }
  IDirect3DSurface9* sys = nullptr;
  HRESULT hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                  D3DPOOL_SYSTEMMEM, &sys, nullptr);
  if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(bb, sys);
  D3DLOCKED_RECT lr{};
  if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);

  if (SUCCEEDED(hr)) {
    static int s_seq = 0;
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&dump_back_buffer), &self) &&
        self && GetModuleFileNameW(self, path, MAX_PATH)) {
      wchar_t* slash = std::wcsrchr(path, L'\\');
      if (slash) slash[1] = 0;
      std::swprintf(path + std::wcslen(path), MAX_PATH - std::wcslen(path),
                    L"overlay_shot_%02d.bmp", s_seq++ % 100);
      FILE* f = nullptr;
      if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        const int w = (int)desc.Width, h = (int)desc.Height;
        const int row = (w * 3 + 3) & ~3;
        const int img = row * h;
        unsigned char hdr[54] = {};
        hdr[0] = 'B';
        hdr[1] = 'M';
        const int file_size = 54 + img;
        std::memcpy(hdr + 2, &file_size, 4);
        const int off = 54;
        std::memcpy(hdr + 10, &off, 4);
        std::memcpy(hdr + 14, &off, 4);
        const int bi = 40;
        std::memcpy(hdr + 18, &bi, 4);
        std::memcpy(hdr + 22, &w, 4);
        std::memcpy(hdr + 26, &h, 4);
        const short planes = 1, bpp = 24;
        std::memcpy(hdr + 28, &planes, 2);
        std::memcpy(hdr + 30, &bpp, 2);
        std::memcpy(hdr + 34, &img, 4);
        std::fwrite(hdr, 1, 54, f);
        unsigned char* zeros = static_cast<unsigned char*>(std::calloc(row, 1));
        for (int y = h - 1; y >= 0; --y) {
          const unsigned char* src =
              static_cast<const unsigned char*>(lr.pBits) + (size_t)y * (size_t)lr.Pitch;
          unsigned char* dst = zeros;
          for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
          }
          std::fwrite(zeros, 1, (size_t)row, f);
        }
        std::free(zeros);
        std::fclose(f);
        game_api::log("ui: back buffer dumped to %ls (%dx%d)", path, w, h);
      }
    }
    sys->UnlockRect();
  }
  if (sys) sys->Release();
  if (bb) bb->Release();
}

void end_frame(IDirect3DDevice9* device) {
  if (!g_inited || !device) return;
  ImGui::EndFrame();
  ImGui::Render();
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

  // Ctrl+F12 存一张当前帧（含 overlay），方便核对投影 / 滚动条这类观感问题。
  static bool s_dump_prev = false;
  const bool dump_now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                        (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
  if (dump_now && !s_dump_prev) dump_back_buffer(device);
  s_dump_prev = dump_now;
}

bool overlay_visible() {
  if (input::menu_visible()) return true;
  return g_inited && g_stats_open && g_stats_pinned;
}

bool stats_roster_wanted() { return g_inited && g_stats_open; }

bool want_capture_mouse() {
  if (!overlay_visible()) return false;
  return ImGui::GetIO().WantCaptureMouse;
}

bool want_capture_keyboard() {
  if (!overlay_visible()) return false;
  return ImGui::GetIO().WantCaptureKeyboard;
}

}  // namespace ui
