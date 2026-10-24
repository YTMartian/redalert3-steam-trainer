#include "ui.h"
#include "input.h"
#include "game_api.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ui {
namespace {

bool g_inited = false;
HWND g_hwnd = nullptr;
int g_group = 0;
bool g_show_settings = false;
bool g_show_build_lock = false;
bool g_show_camera = false;
bool g_show_time = false;
bool g_show_lua = false;
bool g_show_misc = false;
bool g_stats_open = false;
bool g_stats_pinned = false;
bool g_stats_compact = false;
std::string g_last_msg =
    u8"先启动游戏并进局，再运行修改器。菜单内点「注入」。Home 显隐主菜单。";
bool g_spectate_inject = false;
float g_sync_timer = 0.f;
volatile LONG g_injecting = 0;
volatile LONG g_engine_busy = 0;

// UI appearance (persisted). Defaults match the previous large layout.
float g_ui_scale = 1.00f;     // 0.70 ~ 1.30
float g_ui_opacity = 0.80f;   // 0.35 ~ 1.00
// ScaleAllSizes / FontGlobalScale do not affect hardcoded Image sizes.
static float ui_px(float v) {
  const float s = (g_ui_scale > 0.01f) ? g_ui_scale : 1.f;
  return v * s;
}
bool g_unit_inspect_open = true;
bool g_show_debug_status = false;  // 顶部调试状态，默认关闭，避免长日志撑开窗口
char g_ui_settings_path[MAX_PATH] = {};
bool g_settings_dirty = false;
float g_settings_save_timer = 0.f;

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
  return WideCharToMultiByte(CP_UTF8, 0, beside, -1, out, (int)out_len, nullptr,
                             nullptr) > 0;
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void load_ui_settings() {
  if (!g_ui_settings_path[0]) {
    resolve_ui_settings_path(g_ui_settings_path, sizeof(g_ui_settings_path));
  }
  if (!g_ui_settings_path[0]) return;
  std::ifstream f(g_ui_settings_path);
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
      g_ui_scale = clampf((float)std::atof(val.c_str()), 0.70f, 1.30f);
    } else if (key == "opacity") {
      g_ui_opacity = clampf((float)std::atof(val.c_str()), 0.35f, 1.00f);
    } else if (key == "unit_inspect_open") {
      g_unit_inspect_open = (val == "1" || val == "true" || val == "yes");
    } else if (key == "stats_pinned") {
      // 固定是可选项，启动时一律不固定。
      g_stats_pinned = false;
    } else if (key == "stats_compact") {
      g_stats_compact = (val == "1" || val == "true" || val == "yes");
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
}

void save_ui_settings() {
  if (!g_ui_settings_path[0]) {
    resolve_ui_settings_path(g_ui_settings_path, sizeof(g_ui_settings_path));
  }
  if (!g_ui_settings_path[0]) return;
  std::ofstream f(g_ui_settings_path, std::ios::trunc);
  if (!f) return;
  f << "# RA3 Overlay UI settings\n";
  f << "scale=" << g_ui_scale << "\n";
  f << "opacity=" << g_ui_opacity << "\n";
  f << "unit_inspect_open=" << (g_unit_inspect_open ? 1 : 0) << "\n";
  f << "stats_pinned=" << (g_stats_pinned ? 1 : 0) << "\n";
  f << "stats_compact=" << (g_stats_compact ? 1 : 0) << "\n";
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
  g_settings_dirty = false;
  g_settings_save_timer = 0.f;
}

void apply_ra3_theme() {
  // ScaleAllSizes also multiplies _MainScale / WindowMinSize / IndentSpacing /
  // CellPadding / … — fields we used to leave dirty, so each slider tick
  // compounded and the whole window width drifted. Always restore a clean
  // scale=1 snapshot, then scale exactly once.
  static ImGuiStyle s_base;
  static bool s_base_ready = false;
  if (!s_base_ready) {
    s_base = ImGuiStyle();
    s_base.WindowPadding = ImVec2(12, 12);
    s_base.FramePadding = ImVec2(10, 6);
    s_base.ItemSpacing = ImVec2(10, 8);
    s_base.ItemInnerSpacing = ImVec2(8, 5);
    s_base.WindowRounding = 8.0f;
    s_base.ChildRounding = 6.0f;
    s_base.FrameRounding = 5.0f;
    s_base.PopupRounding = 5.0f;
    s_base.ScrollbarRounding = 6.0f;
    s_base.GrabRounding = 4.0f;
    s_base.TabRounding = 5.0f;
    s_base.WindowBorderSize = 1.0f;
    s_base.FrameBorderSize = 0.0f;
    s_base.ScrollbarSize = 12.0f;
    s_base_ready = true;
  }

  ImGuiStyle& s = ImGui::GetStyle();
  s = s_base;

  ImGui::StyleColorsDark();
  ImVec4* c = s.Colors;
  const float a = g_ui_opacity;
  const ImVec4 bg(0.08f, 0.09f, 0.11f, a);
  const ImVec4 panel(0.12f, 0.13f, 0.16f, a);
  const ImVec4 panel2(0.15f, 0.16f, 0.20f, a);
  const ImVec4 blue(0.26f, 0.59f, 0.98f, 1.0f);
  const ImVec4 blue_h(0.36f, 0.66f, 1.00f, 1.0f);
  const ImVec4 blue_a(0.18f, 0.45f, 0.85f, 1.0f);
  const ImVec4 text(0.92f, 0.93f, 0.95f, 1.0f);
  const ImVec4 muted(0.55f, 0.58f, 0.64f, 1.0f);
  const ImVec4 border(0.25f, 0.28f, 0.35f, a);

  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = muted;
  c[ImGuiCol_WindowBg] = bg;
  c[ImGuiCol_ChildBg] = panel;
  c[ImGuiCol_PopupBg] = panel2;
  c[ImGuiCol_Border] = border;
  c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.24f, 0.32f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.28f, 0.40f, 1.0f);
  c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.0f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.28f, 0.48f, 1.0f);
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.12f, 0.16f, 0.8f);
  c[ImGuiCol_MenuBarBg] = panel;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.35f, 0.48f, 1.0f);
  c[ImGuiCol_ScrollbarGrabHovered] = blue;
  c[ImGuiCol_ScrollbarGrabActive] = blue_h;
  c[ImGuiCol_CheckMark] = blue_h;
  c[ImGuiCol_SliderGrab] = blue;
  c[ImGuiCol_SliderGrabActive] = blue_h;
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.40f, 0.70f, 1.0f);
  c[ImGuiCol_ButtonHovered] = blue_h;
  c[ImGuiCol_ButtonActive] = blue_a;
  c[ImGuiCol_Header] = ImVec4(0.22f, 0.40f, 0.70f, 0.85f);
  c[ImGuiCol_HeaderHovered] = blue;
  c[ImGuiCol_HeaderActive] = blue_h;
  c[ImGuiCol_Separator] = border;
  c[ImGuiCol_Tab] = panel2;
  c[ImGuiCol_TabHovered] = blue;
  c[ImGuiCol_TabActive] = ImVec4(0.22f, 0.42f, 0.78f, 1.0f);
  c[ImGuiCol_TabUnfocused] = panel;
  c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.32f, 0.55f, 1.0f);

  if (g_ui_scale > 0.01f && g_ui_scale != 1.0f) {
    s.ScaleAllSizes(g_ui_scale);
  }
  ImGui::GetIO().FontGlobalScale = g_ui_scale;
}

void mark_settings_dirty() {
  g_settings_dirty = true;
  g_settings_save_timer = 0.6f;
  apply_ra3_theme();
}

void draw_settings_panel() {
  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"界面设置");
  ImGui::TextDisabled(u8"界面和快捷键自动保存到和 DLL 同一目录的 overlay_ui.ini");
  ImGui::Separator();
  ImGui::Spacing();

  float scale = g_ui_scale;
  ImGui::TextUnformatted(u8"界面缩放");
  ImGui::SetNextItemWidth(-1.f);
  if (ImGui::SliderFloat("##ui_scale", &scale, 0.70f, 1.30f, "%.2f")) {
    g_ui_scale = clampf(scale, 0.70f, 1.30f);
    mark_settings_dirty();
  }
  ImGui::TextDisabled(u8"同时影响字体与控件尺寸（默认 1.00）");

  ImGui::Spacing();
  ImGui::TextUnformatted(u8"透明度");
  float opacity_pct = g_ui_opacity * 100.f;
  ImGui::SetNextItemWidth(-1.f);
  if (ImGui::SliderFloat("##ui_opacity", &opacity_pct, 35.f, 100.f, "%.0f%%")) {
    g_ui_opacity = clampf(opacity_pct / 100.f, 0.35f, 1.00f);
    mark_settings_dirty();
  }
  ImGui::TextDisabled(u8"窗口背景不透明度（默认 80%%）");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (ImGui::Button(u8"恢复默认", ImVec2(140, 0))) {
    g_ui_scale = 1.00f;
    g_ui_opacity = 0.80f;
    g_unit_inspect_open = true;
    mark_settings_dirty();
    g_settings_save_timer = 0.05f;
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"立即保存", ImVec2(140, 0))) {
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
      IM_COL32(130, 214, 156, 255), IM_COL32(232, 122, 112, 255), IM_COL32(242, 186, 96, 255),
      IM_COL32(176, 132, 255, 255), IM_COL32(156, 186, 214, 255),
  };

  ImGui::TextColored(ImVec4(0.70f, 0.78f, 0.86f, 1.f), "%s", title);
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
  ImGui::BeginTooltip();
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
  ImGui::EndTooltip();
}

void draw_player_econ_card(const game_api::PlayerEconomy& p, int index) {
  ImVec4 accent(p.color_r / 255.f, p.color_g / 255.f, p.color_b / 255.f, 1.f);
  if (!p.has_color) {
    accent = p.is_local ? ImVec4(0.95f, 0.78f, 0.28f, 1.f) : ImVec4(0.55f, 0.62f, 0.72f, 1.f);
  }
  const ImVec4 panel = ImVec4(0.08f, 0.10f, 0.14f, 0.92f);
  const ImVec4 money_col = ImVec4(1.00f, 0.86f, 0.38f, 1.f);
  const ImVec4 power_ok = ImVec4(0.35f, 0.85f, 0.62f, 1.f);
  const ImVec4 power_warn = ImVec4(0.95f, 0.55f, 0.28f, 1.f);
  const ImVec4 power_bad = ImVec4(0.95f, 0.35f, 0.32f, 1.f);

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
  dl->AddRect(sw0, ImVec2(sw0.x + sw, sw0.y + sw), IM_COL32(255, 255, 255, 50), 3.f, 0, 1.f);
  ImGui::Dummy(ImVec2(sw, sw));
  ImGui::SameLine(0.f, 8.f);
  ImGui::TextColored(ImVec4(0.93f, 0.95f, 0.98f, 1.f), "%s", title);
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

    float frac = 0.f;
    if (p.power_total > 0) {
      frac = (float)p.power_used / (float)p.power_total;
      if (frac > 1.f) frac = 1.f;
    }
    ImVec4 bar = power_ok;
    if (frac >= 0.95f) bar = power_bad;
    else if (frac >= 0.75f) bar = power_warn;

    ImGui::TextColored(ImVec4(0.85f, 0.92f, 1.f, 1.f), "%s", ratio_s);
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

    const float icon_sz = ui_px(42.f);
    const float gap = ui_px(4.f);
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
      ImGui::TextColored(ImVec4(0.70f, 0.78f, 0.90f, 1.f), "%s", title);
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

        draw->AddRectFilled(p0, chip1, IM_COL32(12, 16, 24, 180), round);
        draw->AddRect(p0, chip1, accent_u32, round, 0, 1.0f);

        IDirect3DTexture9* tex = unit_icons::get(r.key, r.name, r.is_building);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
        if (tex) {
          ImGui::Image((ImTextureID)tex, ImVec2(icon_sz, icon_sz));
        } else {
          ImGui::Dummy(ImVec2(icon_sz, icon_sz));
          draw->AddRectFilled(ImVec2(p0.x + pad, p0.y + pad),
                              ImVec2(p0.x + pad + icon_sz, p0.y + pad + icon_sz),
                              IM_COL32(40, 48, 60, 220), ui_px(3.f));
          const char* short_l = buildings ? u8"建" : u8"兵";
          const ImVec2 sts = ImGui::CalcTextSize(short_l);
          draw->AddText(ImVec2(p0.x + pad + (icon_sz - sts.x) * 0.5f,
                               p0.y + pad + (icon_sz - sts.y) * 0.5f),
                        IM_COL32(200, 210, 220, 255), short_l);
        }
        if (ImGui::IsMouseHoveringRect(p0, chip1)) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(r.name[0] ? r.name : r.key);
          ImGui::TextDisabled("%s  × %d", buildings ? u8"建筑" : u8"部队", r.count);
          ImGui::EndTooltip();
        }

        char cnt[16];
        std::snprintf(cnt, sizeof(cnt), "%d", r.count);
        const ImVec2 cts = ImGui::CalcTextSize(cnt);
        const float bx1 = chip1.x - pad;
        const float by1 = chip1.y - pad;
        const float bx0 = bx1 - cts.x - 6.f;
        const float by0 = by1 - cts.y - 2.f;
        draw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(8, 10, 14, 230),
                            ui_px(3.f));
        draw->AddText(ImVec2(bx0 + 3.f, by0 + 1.f), IM_COL32(255, 230, 160, 255), cnt);

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
        p.is_local ? IM_COL32(200, 160, 50, 120) : IM_COL32(70, 130, 210, 120);
    const ImU32 bld_accent =
        p.is_local ? IM_COL32(180, 140, 40, 140) : IM_COL32(90, 150, 220, 140);
    const bool has_bld = p.building_total > 0;
    const bool has_proto = p.protocol_count > 0;
    draw_roster_section(u8"部队", false, unit_accent, has_bld || has_proto);
    draw_roster_section(u8"建筑", true, bld_accent, has_proto);
    if (p.protocol_count > 0) {
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, ui_px(3.f)));
      ImGui::TextColored(ImVec4(0.70f, 0.78f, 0.90f, 1.f), u8"协议");
      ImGui::SameLine(0.f, ui_px(6.f));
      ImGui::TextDisabled("%d", p.protocol_count);
      const float avail = ImGui::GetContentRegionAvail().x;
      float used_x = 0.f;
      bool row_started = false;
      const ImU32 proto_accent = IM_COL32(120, 196, 168, 160);
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
        draw->AddRectFilled(p0, chip1, IM_COL32(12, 22, 20, 180), round);
        draw->AddRect(p0, chip1, proto_accent, round, 0, 1.0f);
        IDirect3DTexture9* tex = unit_icons::get(pr.key, pr.name, false);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
        if (tex) ImGui::Image((ImTextureID)tex, ImVec2(icon_sz, icon_sz));
        else ImGui::Dummy(ImVec2(icon_sz, icon_sz));
        if (ImGui::IsMouseHoveringRect(p0, chip1)) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(pr.name[0] ? pr.name : pr.key);
          if (pr.remain_sec > 0) ImGui::TextDisabled(u8"剩余 %d 秒", pr.remain_sec);
          else ImGui::TextDisabled(u8"就绪");
          ImGui::EndTooltip();
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
          draw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(8, 10, 14, 230),
                              ui_px(3.f));
          draw->AddText(ImVec2(bx0 + 3.f, by0 + 1.f), IM_COL32(255, 214, 120, 255), clk);
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
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s);
}

// Draw one compact chip at an absolute screen position (no ImGui layout side effects).
void draw_compact_icon_at(ImDrawList* draw, const ImVec2& p0, const game_api::RosterType& r,
                          float cell, float icon_sz, ImU32 border, bool building, bool defeated) {
  const ImVec2 chip1(p0.x + cell, p0.y + cell);
  const ImU32 fill = building ? IM_COL32(32, 24, 14, 210) : IM_COL32(14, 20, 32, 210);
  const float round = ui_px(4.f);
  draw->AddRectFilled(p0, chip1, fill, round);
  draw->AddRect(p0, chip1, border, round, 0, 1.0f);

  const float inset = (cell - icon_sz) * 0.5f;
  IDirect3DTexture9* tex = unit_icons::get(r.key, r.name, r.is_building);
  const ImU32 tint = defeated ? IM_COL32(158, 158, 158, 204) : IM_COL32(255, 255, 255, 255);
  if (tex) {
    draw->AddImage((ImTextureID)tex, ImVec2(p0.x + inset, p0.y + inset),
                   ImVec2(p0.x + inset + icon_sz, p0.y + inset + icon_sz), ImVec2(0, 0),
                   ImVec2(1, 1), tint);
  } else {
    draw->AddRectFilled(ImVec2(p0.x + inset, p0.y + inset),
                        ImVec2(p0.x + inset + icon_sz, p0.y + inset + icon_sz),
                        IM_COL32(40, 48, 60, 220), ui_px(3.f));
    const char* short_l = building ? u8"建" : u8"兵";
    const ImVec2 sts = ImGui::CalcTextSize(short_l);
    draw->AddText(ImVec2(p0.x + inset + (icon_sz - sts.x) * 0.5f,
                         p0.y + inset + (icon_sz - sts.y) * 0.5f),
                  IM_COL32(200, 210, 220, 255), short_l);
  }
  if (ImGui::IsMouseHoveringRect(p0, chip1)) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(r.name[0] ? r.name : r.key);
    ImGui::TextDisabled("%s  × %d", building ? u8"建筑" : u8"部队", r.count);
    ImGui::EndTooltip();
  }

  char cnt[16];
  std::snprintf(cnt, sizeof(cnt), "%d", r.count);
  ImFont* font = ImGui::GetFont();
  const float csz = (std::max)(10.f, ImGui::GetFontSize() * 0.72f);
  const ImVec2 cts = font->CalcTextSizeA(csz, FLT_MAX, 0.f, cnt);
  const float bx1 = chip1.x - 1.f;
  const float by1 = chip1.y - 1.f;
  const float bx0 = bx1 - cts.x - 5.f;
  const float by0 = by1 - cts.y - 1.f;
  draw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(8, 10, 14, 230), ui_px(3.f));
  draw->AddText(font, csz, ImVec2(bx0 + 2.5f, by0), IM_COL32(255, 228, 150, 255), cnt);
}

void draw_compact_icon_row(const game_api::PlayerEconomy& p, bool buildings, bool defeated) {
  const float icon_sz = ui_px(32.f);
  const float cell = ui_px(34.f);
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
    draw->AddText(ImVec2(row0.x, row0.y), IM_COL32(176, 196, 220, 255), head);
    // Grow layout via Dummy only — do not SetCursor beyond content first.
    ImGui::Dummy(ImVec2(avail, ts.y + row_gap));
    return;
  }

  const ImU32 border = buildings
                           ? (p.is_local ? IM_COL32(196, 148, 64, 200) : IM_COL32(214, 164, 86, 190))
                           : (p.is_local ? IM_COL32(214, 176, 72, 180) : IM_COL32(96, 156, 224, 180));
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
  draw->AddText(ImVec2(row0.x, row0.y + (block_h - ts.y) * 0.5f), IM_COL32(176, 196, 220, 255),
                head);
  // Reserve the drawn block with Dummy so AutoResize / parent bounds grow.
  ImGui::Dummy(ImVec2(avail, block_h + row_gap));
}

void draw_compact_player(const game_api::PlayerEconomy& p, int index) {
  ImVec4 accent(p.color_r / 255.f, p.color_g / 255.f, p.color_b / 255.f, 1.f);
  if (!p.has_color) {
    accent = p.is_local ? ImVec4(0.95f, 0.78f, 0.28f, 1.f) : ImVec4(0.55f, 0.62f, 0.72f, 1.f);
  }
  const ImVec4 money_col(1.00f, 0.86f, 0.38f, 1.f);
  const ImVec4 power_ok(0.45f, 0.90f, 0.62f, 1.f);
  const ImVec4 power_warn(0.95f, 0.62f, 0.28f, 1.f);
  const ImVec4 power_bad(0.95f, 0.38f, 0.34f, 1.f);
  const ImVec4 muted(0.55f, 0.60f, 0.68f, 1.f);
  const ImVec4 name_col = p.defeated ? muted : ImVec4(0.94f, 0.96f, 0.98f, 1.f);

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
    if (p.power_total > 0) {
      frac = (float)p.power_used / (float)p.power_total;
      if (frac > 1.f) frac = 1.f;
    }
    power_col = power_ok;
    if (frac >= 0.95f) power_col = power_bad;
    else if (frac >= 0.75f) power_col = power_warn;
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
    const ImU32 border = IM_COL32(96, 186, 156, 190);
    for (int i = 0; i < p.protocol_count; ++i) {
      const auto& pr = p.protocols[i];
      if (x > 0.f && x + cell > wrap_w) {
        x = 0.f;
        y += cell + gap;
        ++rows;
      }
      const ImVec2 p0(icons_x + x, row0.y + y);
      const ImVec2 chip1(p0.x + cell, p0.y + cell);
      pdraw->AddRectFilled(p0, chip1, IM_COL32(14, 28, 24, 210), ui_px(4.f));
      pdraw->AddRect(p0, chip1, border, ui_px(4.f), 0, 1.0f);
      const float inset = (cell - icon_sz) * 0.5f;
      IDirect3DTexture9* tex = unit_icons::get(pr.key, pr.name, false);
      const ImU32 tint = p.defeated ? IM_COL32(158, 158, 158, 204) : IM_COL32(255, 255, 255, 255);
      if (tex) {
        pdraw->AddImage((ImTextureID)tex, ImVec2(p0.x + inset, p0.y + inset),
                        ImVec2(p0.x + inset + icon_sz, p0.y + inset + icon_sz), ImVec2(0, 0),
                        ImVec2(1, 1), tint);
      }
      if (ImGui::IsMouseHoveringRect(p0, chip1)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(pr.name[0] ? pr.name : pr.key);
        if (pr.remain_sec > 0) ImGui::TextDisabled(u8"剩余 %d 秒", pr.remain_sec);
        else ImGui::TextDisabled(u8"就绪");
        ImGui::EndTooltip();
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
        pdraw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(8, 10, 14, 230),
                             ui_px(3.f));
        pdraw->AddText(font, csz, ImVec2(bx0 + 2.5f, by0), IM_COL32(255, 214, 120, 255), clk);
      }
      x += cell + gap;
    }
    const float block_h = (float)rows * cell + (float)(rows - 1) * gap;
    pdraw->AddText(ImVec2(row0.x, row0.y + (block_h - ts.y) * 0.5f), IM_COL32(150, 214, 186, 255),
                   head);
    ImGui::Dummy(ImVec2(avail_p, block_h + row_gap));
  }

  ImGui::EndGroup();
  const ImVec2 r0 = ImGui::GetItemRectMin();
  const ImVec2 r1 = ImGui::GetItemRectMax();
  ImGui::PopID();

  dl->ChannelsSetCurrent(0);
  const ImU32 bg = (index & 1) ? IM_COL32(18, 24, 34, 150) : IM_COL32(24, 32, 44, 130);
  dl->AddRectFilled(ImVec2(origin.x, r0.y), ImVec2(origin.x + avail, r1.y), bg, 4.f);
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

  ImGui::TextColored(ImVec4(0.78f, 0.90f, 1.0f, 1.f), u8"战况总览");
  if (st.spectator) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.f, 0.78f, 0.40f, 1.f), u8"观战");
  }
  if (st.valid) {
    char count_s[32];
    std::snprintf(count_s, sizeof(count_s), u8"%d 方", st.player_count);
    const float count_w = ImGui::CalcTextSize(count_s).x;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - count_w);
    ImGui::TextDisabled("%s", count_s);
  }
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.35f, 0.48f, 0.65f, 0.55f));
  ImGui::Separator();
  ImGui::PopStyleColor();
  if (!g_stats_compact) ImGui::Spacing();

  if (!st.valid) {
    ImGui::TextWrapped(u8"%s", st.note[0] ? st.note : u8"暂无数据：请先进入对局。");
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
  if (!g_stats_open) return;
  if (!input::menu_visible() && !g_stats_pinned) {
    g_stats_open = false;
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(420, 640), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(950, 28), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(320, 280), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::SetNextWindowBgAlpha(g_ui_opacity);

  // Pinned: no close button — must uncheck 固定 to hide.
  ImGuiWindowFlags flags = ImGuiWindowFlags_None;
  bool* p_open = nullptr;
  bool open = g_stats_open;
  if (g_stats_pinned) {
    flags |= ImGuiWindowFlags_NoCollapse;
  } else {
    p_open = &open;
  }

  if (!ImGui::Begin(u8"战况统计", p_open, flags)) {
    ImGui::End();
    if (!g_stats_pinned && !open) g_stats_open = false;
    return;
  }

  if (ImGui::Checkbox(u8"固定", &g_stats_pinned)) {
    mark_settings_dirty();
    g_settings_save_timer = 0.2f;
    if (g_stats_pinned) {
      g_stats_open = true;
    } else if (!input::menu_visible()) {
      g_stats_open = false;
      ImGui::End();
      return;
    }
  }
  ImGui::SameLine();
  if (ImGui::Checkbox(u8"精简", &g_stats_compact)) mark_settings_dirty();
  ImGui::SameLine();
  ImGui::TextDisabled(g_stats_compact ? u8"图标、资金、电力"
                                      : (g_stats_pinned ? u8"主菜单隐藏后仍显示"
                                                        : u8"取消固定后可关闭本窗口"));

  ImGui::Spacing();
  draw_stats_panel();
  ImGui::End();

  if (!g_stats_pinned && !open) g_stats_open = false;
}

}  // namespace

bool init(IDirect3DDevice9* device, void* hwnd) {
  if (g_inited) return true;
  if (!device || !hwnd) return false;
  g_hwnd = static_cast<HWND>(hwnd);

  load_ui_settings();

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
  for (const char* path : font_candidates) {
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
      // ChineseFull builds a huge atlas and freezes the first open for seconds.
      // Common glyphs cover the UI; missing rare chars fall back to '?'.
      io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg,
                                   io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
      font_ok = true;
      break;
    }
  }
  if (!font_ok) io.Fonts->AddFontDefault();

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
  game_api::log("ImGui UI ready scale=%.2f opacity=%.2f", g_ui_scale,
                g_ui_opacity);
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
  const float rnd = 10.f;

  ImU32 bg;
  ImU32 edge;
  ImU32 bar;
  if (!enabled) {
    bg = IM_COL32(24, 28, 36, 150);
    edge = IM_COL32(70, 80, 96, 110);
    bar = IM_COL32(80, 90, 110, 80);
  } else if (active) {
    bg = held ? IM_COL32(10, 72, 124, 250) : hov ? IM_COL32(16, 92, 154, 248)
                                                 : IM_COL32(12, 58, 112, 240);
    edge = IM_COL32(110, 220, 255, 240);
    bar = IM_COL32(80, 230, 255, 255);
  } else {
    bg = held ? IM_COL32(26, 46, 74, 245) : hov ? IM_COL32(30, 50, 82, 240)
                                                : IM_COL32(16, 22, 34, 230);
    edge = hov ? IM_COL32(96, 168, 236, 220) : IM_COL32(64, 92, 128, 170);
    bar = hov ? IM_COL32(90, 170, 240, 220) : IM_COL32(52, 96, 150, 140);
  }

  dl->AddRectFilled(ImVec2(p0.x + 2.f, p0.y + 3.f), ImVec2(p1.x + 2.f, p1.y + 3.f),
                    IM_COL32(0, 0, 0, 80), rnd);
  dl->AddRectFilled(p0, p1, bg, rnd);
  dl->PushClipRect(p0, p1, true);
  dl->AddRectFilledMultiColor(p0, p1, IM_COL32(255, 255, 255, active ? 28 : 16),
                              IM_COL32(255, 255, 255, active ? 28 : 16), IM_COL32(0, 0, 0, 0),
                              IM_COL32(0, 0, 0, 0));
  dl->PopClipRect();
  dl->AddRect(p0, p1, edge, rnd, 0, (hov || active) ? 1.7f : 1.0f);
  dl->AddRectFilled(ImVec2(p0.x + 2.f, p0.y + 9.f), ImVec2(p0.x + 5.f, p1.y - 9.f), bar, 2.f);

  dl->PushClipRect(ImVec2(p0.x + 12.f, p0.y + 3.f), ImVec2(p1.x - 8.f, p1.y - 3.f), true);
  const ImVec2 ts = ImGui::CalcTextSize(title ? title : "");
  const bool has_key = hotkey && hotkey[0];
  const float ty = p0.y + (has_key ? 7.f : (sz.y - ts.y) * 0.5f);
  dl->AddText(ImVec2(p0.x + 14.f, ty),
              enabled ? IM_COL32(236, 244, 252, 255) : IM_COL32(150, 158, 170, 180),
              title ? title : "");
  if (has_key) {
    const ImVec2 bs = ImGui::CalcTextSize(hotkey);
    const ImVec2 b0(p1.x - bs.x - 14.f, p1.y - bs.y - 7.f);
    dl->AddRectFilled(ImVec2(b0.x - 6.f, b0.y - 2.f), ImVec2(p1.x - 8.f, b0.y + bs.y + 2.f),
                      IM_COL32(6, 14, 26, 200), 4.f);
    dl->AddText(b0, enabled ? IM_COL32(150, 206, 255, 240) : IM_COL32(120, 140, 160, 140),
                hotkey);
  }
  if (active) {
    dl->AddCircleFilled(ImVec2(p1.x - 12.f, p0.y + 12.f), 3.5f, IM_COL32(140, 255, 220, 245));
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
  if (selected || hov) {
    const ImU32 bg = selected ? IM_COL32(14, 52, 98, 235) : IM_COL32(26, 36, 52, 190);
    dl->AddRectFilled(p0, p1, bg, 7.f);
    dl->AddRect(p0, p1, selected ? IM_COL32(90, 196, 255, 200) : IM_COL32(70, 100, 140, 120),
                7.f, 0, 1.f);
  }
  if (selected) {
    dl->AddRectFilled(ImVec2(p0.x + 1.f, p0.y + 5.f), ImVec2(p0.x + 4.f, p1.y - 5.f),
                      IM_COL32(110, 220, 255, 255), 2.f);
  }
  const ImVec2 ts = ImGui::CalcTextSize(label ? label : "");
  dl->AddText(ImVec2(p0.x + 12.f, p0.y + (h - ts.y) * 0.5f),
              selected ? IM_COL32(220, 242, 255, 255) : IM_COL32(186, 198, 214, 235),
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
  const ImU32 col = hov ? IM_COL32(150, 214, 255, 255) : IM_COL32(168, 184, 204, 230);
  if (hov) dl->AddCircleFilled(c, s * 0.40f, IM_COL32(36, 64, 102, 200), 18);
  dl->AddCircle(c, r, col, 16, 1.7f);
  dl->AddCircleFilled(c, r * 0.42f, col, 12);
  for (int i = 0; i < 8; ++i) {
    const float a = (float)i * 0.78539816f;
    const float cs = std::cos(a);
    const float sn = std::sin(a);
    dl->AddLine(ImVec2(c.x + cs * (r * 0.72f), c.y + sn * (r * 0.72f)),
                ImVec2(c.x + cs * (r + s * 0.16f), c.y + sn * (r + s * 0.16f)), col, 2.2f);
  }
  if (hov && tip && tip[0]) ImGui::SetTooltip("%s", tip);
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
  const ImU32 track = !enabled ? IM_COL32(46, 52, 62, 150)
                               : on ? IM_COL32(22, 118, 176, 245) : IM_COL32(46, 54, 68, 235);
  const ImU32 knob = !enabled ? IM_COL32(140, 148, 158, 170)
                              : on ? IM_COL32(236, 250, 255, 255) : IM_COL32(176, 188, 204, 235);
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), track, h * 0.5f);
  dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h),
              on && enabled ? IM_COL32(120, 214, 255, 220) : IM_COL32(86, 100, 120, 150), h * 0.5f,
              0, 1.3f);
  const float pad = 3.f;
  const float kr = (h - pad * 2.f) * 0.5f;
  const float kx = on ? (p0.x + w - pad - kr) : (p0.x + pad + kr);
  dl->AddCircleFilled(ImVec2(kx, p0.y + h * 0.5f), kr, knob, 18);
  if (hov) ImGui::SetTooltip("%s", !enabled ? u8"请先注入" : on ? u8"点击关闭" : u8"点击开启");
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
      if (ImGui::Button(line, ImVec2(180, 0))) {
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
  const float action_w = as_switch ? gear * 1.64f : 72.f;
  const float name_w = std::max(24.f, avail - gear - action_w - gap * 2.f);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float row_h = gear;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + row_h), IM_COL32(16, 22, 34, 215),
                    8.f);
  dl->AddRect(origin, ImVec2(origin.x + avail, origin.y + row_h), IM_COL32(58, 84, 116, 150), 8.f,
              0, 1.f);

  const ImVec2 ts = ImGui::CalcTextSize(f->label);
  dl->PushClipRect(ImVec2(origin.x + 4.f, origin.y), ImVec2(origin.x + name_w - 4.f, origin.y + row_h),
                   true);
  dl->AddText(ImVec2(origin.x + 12.f, origin.y + (row_h - ts.y) * 0.5f), IM_COL32(232, 240, 248, 255),
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
    if (use > 2 && avail < 460.f) use = 2;
    if (avail < 260.f) use = 1;
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
  const float action_w = as_switch ? sw_h * 1.9f : 52.f;
  const float gear_w = has_hk ? row_h : 0.f;
  const float inner = 4.f;
  const float name_w =
      std::max(20.f, g_pair_w - gear_w - action_w - inner * (has_hk ? 2.f : 1.f));

  ImGui::PushID(f->key);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool on = as_switch && game_api::feature_enabled(f->key);
  dl->AddRectFilled(origin, ImVec2(origin.x + g_pair_w, origin.y + row_h),
                    on ? IM_COL32(12, 40, 72, 230) : IM_COL32(16, 22, 34, 215), 6.f);
  dl->AddRect(origin, ImVec2(origin.x + g_pair_w, origin.y + row_h),
              on ? IM_COL32(90, 190, 240, 200) : IM_COL32(58, 84, 116, 150), 6.f, 0, 1.f);

  const bool name_click = ImGui::InvisibleButton("##name", ImVec2(name_w, row_h));
  if (ImGui::IsItemHovered()) {
    const char* hk = game_api::hotkey_hint(f->key);
    if (hk && hk[0]) ImGui::SetTooltip("%s\n%s", f->label, hk);
    else ImGui::SetTooltip("%s", f->label);
  }
  const ImVec2 ts = ImGui::CalcTextSize(f->label);
  dl->PushClipRect(ImVec2(origin.x + 2.f, origin.y), ImVec2(origin.x + name_w - 2.f, origin.y + row_h),
                   true);
  dl->AddText(ImVec2(origin.x + 8.f, origin.y + (row_h - ts.y) * 0.5f),
              action_ok ? IM_COL32(232, 240, 248, 255) : IM_COL32(150, 158, 170, 180), f->label);
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
  const float btn_w = 30.f;
  const float gear_w = has_hk ? row_h : 0.f;
  const float inner = 4.f;
  const float name_w =
      std::max(20.f, g_pair_w - gear_w - btn_w * 2.f - 2.f - inner * (has_hk ? 2.f : 1.f));

  ImGui::PushID(f->key);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + g_pair_w, origin.y + row_h), IM_COL32(16, 22, 34, 215),
                    6.f);
  dl->AddRect(origin, ImVec2(origin.x + g_pair_w, origin.y + row_h), IM_COL32(58, 84, 116, 150), 6.f,
              0, 1.f);

  const bool name_click = ImGui::InvisibleButton("##name", ImVec2(name_w, row_h));
  const bool name_right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  if (ImGui::IsItemHovered()) {
    const char* hk = game_api::hotkey_hint(f->key);
    if (sel) {
      if (hk && hk[0]) {
        ImGui::SetTooltip(u8"%s\n每次 %d\n%s\n点击名称修改每次金额", f->label, step, hk);
      } else {
        ImGui::SetTooltip(u8"%s\n每次 %d\n点击名称修改每次金额", f->label, step);
      }
    } else if (hk && hk[0]) {
      ImGui::SetTooltip(u8"%s\n每次 %d\n%s\n点击名称修改每次金额", f->label, step, hk);
    } else {
      ImGui::SetTooltip(u8"%s\n每次 %d\n点击名称修改每次金额", f->label, step);
    }
  }
  if (name_click || name_right) ImGui::OpenPopup("##step");
  const ImVec2 ts = ImGui::CalcTextSize(f->label);
  dl->PushClipRect(ImVec2(origin.x + 2.f, origin.y), ImVec2(origin.x + name_w - 2.f, origin.y + row_h),
                   true);
  dl->AddText(ImVec2(origin.x + 8.f, origin.y + (row_h - ts.y) * 0.5f), IM_COL32(232, 240, 248, 255),
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
    ImGui::SetTooltip(u8"+ %d", sel ? game_api::money_sel_step() : game_api::money_self_step());
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
    ImGui::SetTooltip(u8"- %d", sel ? game_api::money_sel_step() : game_api::money_self_step());
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
  const ImU32 bg = selected ? IM_COL32(12, 48, 88, 245) : IM_COL32(16, 22, 34, 235);
  const ImU32 edge = selected ? IM_COL32(110, 220, 255, 255)
                              : hov ? IM_COL32(96, 168, 236, 230) : IM_COL32(64, 92, 128, 180);
  dl->AddRectFilled(p0, ImVec2(p0.x + s, p0.y + s), bg, 8.f);
  dl->AddRect(p0, ImVec2(p0.x + s, p0.y + s), edge, 8.f, 0, selected ? 2.2f : 1.2f);
  IDirect3DTexture9* tex = unit_icons::get(type_id, "MCV", false);
  if (tex) {
    dl->AddImage((ImTextureID)tex, ImVec2(p0.x + 4.f, p0.y + 4.f), ImVec2(p0.x + s - 4.f, p0.y + s - 4.f));
  }
  if (selected) {
    dl->AddCircleFilled(ImVec2(p0.x + s - 10.f, p0.y + 10.f), 3.5f, IM_COL32(140, 255, 220, 245));
  }
  if (hov) ImGui::SetTooltip("%s", tip);
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
    ImGui::SetTooltip(u8"拖动选择：正常 / 高 / 最高");
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rnd = h * 0.45f;
  const float seg = w / 3.f;
  ImU32 accent = IM_COL32(90, 100, 116, 160);
  if (enabled) {
    accent = picked == 2 ? IM_COL32(255, 96, 88, 255)
                          : picked == 1 ? IM_COL32(255, 186, 72, 255)
                                        : IM_COL32(90, 210, 255, 255);
  }
  dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), IM_COL32(10, 16, 26, 235), rnd);
  const float xL = origin.x + seg * (float)picked;
  const float xR = xL + seg;
  ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
  if (picked == 0) corners = ImDrawFlags_RoundCornersLeft;
  else if (picked == 2) corners = ImDrawFlags_RoundCornersRight;
  const ImU32 fill = enabled ? (accent & 0x00FFFFFFu) | 0x55000000u : IM_COL32(70, 80, 96, 80);
  dl->AddRectFilled(ImVec2(xL, origin.y), ImVec2(xR, origin.y + h), fill, rnd, corners);
  dl->AddRect(origin, ImVec2(origin.x + w, origin.y + h),
              enabled ? accent : IM_COL32(80, 90, 108, 140), rnd, 0, 1.2f);
  for (int i = 0; i < 3; ++i) {
    const ImVec2 ts = ImGui::CalcTextSize(names[i]);
    const float tx = origin.x + seg * ((float)i + 0.5f) - ts.x * 0.5f;
    const ImU32 tc = (enabled && i == picked) ? IM_COL32(244, 250, 255, 255)
                                               : IM_COL32(150, 164, 182, 210);
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
    ImGui::SetTooltip(u8"拖动选择出场等级：不设置 / 一级 / 二级 / 三级");
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = origin.y + track_y;
  const float x0 = origin.x + pad;
  const float x1 = origin.x + w - pad;
  const float span = std::max(1.f, x1 - x0);
  const float gx = x0 + span * ((float)picked / 3.f);
  ImU32 accent = IM_COL32(150, 160, 172, 220);
  if (picked == 1) accent = IM_COL32(90, 210, 255, 255);
  else if (picked == 2) accent = IM_COL32(255, 186, 72, 255);
  else if (picked == 3) accent = IM_COL32(255, 120, 72, 255);
  const float track_h = 6.f;
  dl->AddRectFilled(ImVec2(x0, cy - track_h * 0.5f), ImVec2(x1, cy + track_h * 0.5f),
                    IM_COL32(16, 22, 34, 235), track_h);
  dl->AddRectFilled(ImVec2(x0, cy - track_h * 0.5f), ImVec2(gx, cy + track_h * 0.5f), accent,
                    track_h);
  for (int i = 0; i < 4; ++i) {
    const float tx = x0 + span * ((float)i / 3.f);
    const bool lit = i <= picked;
    dl->AddCircleFilled(ImVec2(tx, cy), i == picked ? 3.2f : 2.2f,
                        lit ? accent : IM_COL32(70, 86, 108, 210));
    const ImVec2 ts = ImGui::CalcTextSize(names[i]);
    const ImU32 tc = i == picked ? accent : IM_COL32(160, 172, 188, 210);
    dl->AddText(ImVec2(tx - ts.x * 0.5f, origin.y + track_y + grab_r + 4.f), tc, names[i]);
  }
  dl->AddCircleFilled(ImVec2(gx, cy), grab_r, IM_COL32(10, 16, 26, 255));
  dl->AddCircleFilled(ImVec2(gx, cy), grab_r - 3.2f, accent);
  dl->AddCircle(ImVec2(gx, cy), grab_r, IM_COL32(230, 244, 255, 230), 0, 1.4f);
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
  draw->AddRectFilled(p0, ImVec2(p0.x + kIcon, p0.y + kIcon), IM_COL32(16, 22, 32, 220), ui_px(4.f));
  draw->AddRect(p0, ImVec2(p0.x + kIcon, p0.y + kIcon),
                hovered ? IM_COL32(255, 214, 120, 255) : IM_COL32(70, 96, 130, 255), ui_px(4.f), 0,
                hovered ? 2.f : 1.f);
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
                  hovered ? IM_COL32(255, 226, 160, 255) : IM_COL32(196, 210, 232, 255), label);
  }
  if (hovered && tip && tip[0]) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(tip);
    if (icon_id && icon_id[0] && tip != icon_id) ImGui::TextDisabled("%s", icon_id);
    if (removable) ImGui::TextDisabled(u8"点击拿掉");
    ImGui::EndTooltip();
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
  dl->AddRectFilled(r0, ImVec2(r0.x + full_w, r1.y), IM_COL32(12, 18, 28, 220), 6.f);
  dl->AddRect(r0, ImVec2(r0.x + full_w, r1.y), IM_COL32(70, 96, 130, 220), 6.f, 0, 1.2f);
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
        draw->AddRectFilled(p0, ImVec2(p0.x + kIcon, p0.y + kIcon),
                            IM_COL32(16, 22, 32, 140), ui_px(4.f));
        draw->AddRect(p0, ImVec2(p0.x + kIcon, p0.y + kIcon), IM_COL32(60, 66, 78, 200),
                      ui_px(4.f));
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
                      IM_COL32(110, 118, 132, 255), label);
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(extra.name);
          ImGui::TextDisabled("%s", extra.id);
          ImGui::TextDisabled(u8"本版本的对象模板库里没有这个对象");
          ImGui::EndTooltip();
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
    const ImU32 bg = on ? IM_COL32(12, 48, 88, 245) : IM_COL32(16, 22, 34, 230);
    const ImU32 edge = on ? IM_COL32(110, 220, 255, 255)
                          : hov ? IM_COL32(96, 168, 236, 220) : IM_COL32(64, 92, 128, 170);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), bg, 6.f);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), edge, 6.f, 0, on ? 1.6f : 1.f);
    const ImU32 dot = s.has_color ? IM_COL32(s.color_r, s.color_g, s.color_b, 255)
                                  : (s.is_local ? IM_COL32(240, 200, 80, 255) : IM_COL32(140, 170, 210, 255));
    dl->AddRectFilled(ImVec2(p0.x + 6.f, p0.y + (h - 12.f) * 0.5f),
                      ImVec2(p0.x + 18.f, p0.y + (h + 12.f) * 0.5f), dot, 2.f);
    dl->AddText(ImVec2(p0.x + 22.f, p0.y + (h - ts.y) * 0.5f), IM_COL32(232, 240, 248, 255), label);
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
    if (ImGui::Button("+", ImVec2(36.f, 0))) {
      std::string msg;
      const bool ok = game_api::adjust_selected_player_money(game_api::money_sel_step(), &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    ImGui::SameLine();
    if (ImGui::Button("-", ImVec2(36.f, 0))) {
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
    if (ImGui::Button(add_l, ImVec2(140, 0))) {
      std::string msg;
      bool ok = sel ? game_api::adjust_selected_player_money(game_api::money_sel_step(), &msg)
                    : game_api::adjust_local_money(game_api::money_self_step(), &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    ImGui::SameLine();
    if (ImGui::Button(sub_l, ImVec2(140, 0))) {
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
    dl->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + row_h), IM_COL32(16, 22, 34, 215),
                      8.f);
    dl->AddRect(origin, ImVec2(origin.x + avail, origin.y + row_h), IM_COL32(58, 84, 116, 150), 8.f,
                0, 1.f);
    dl->AddText(ImVec2(origin.x + 12.f, origin.y + (row_h - ts.y) * 0.5f),
                IM_COL32(232, 240, 248, 255), f->label);
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
    if (ImGui::Button(u8"召唤", ImVec2(72.f, summon_y))) run_feature_action(f);
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
    if (ImGui::Button(u8"召唤", ImVec2(72.f, 0))) {
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
    if (ImGui::Button(u8"无敌", ImVec2(110.f, 0))) {
      std::string msg;
      const bool ok = game_api::invincible_apply(true, 0.f, 0, &msg);
      if (!msg.empty()) g_last_msg = msg;
      game_api::beep(ok ? "on" : "error");
    }
    ImGui::SameLine(0.f, 10.f);
    if (ImGui::Button(u8"解除无敌", ImVec2(110.f, 0))) {
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", game_api::crate_kind_name(i));
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"空投个数（1~20）");
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
      const ImU32 bg = want_banned ? IM_COL32(48, 22, 22, 220) : IM_COL32(16, 22, 32, 220);
      const ImU32 edge = hovered ? IM_COL32(255, 214, 120, 255)
                                 : (want_banned ? IM_COL32(180, 90, 80, 255)
                                                : IM_COL32(70, 96, 130, 255));
      draw->AddRectFilled(p0, ImVec2(p0.x + kIcon, p0.y + kIcon), bg, ui_px(4.f));
      draw->AddRect(p0, ImVec2(p0.x + kIcon, p0.y + kIcon), edge, ui_px(4.f), 0,
                    hovered ? 2.f : 1.f);
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
                      IM_COL32(210, 216, 224, 255), mark);
      }
      if (hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(e.name[0] ? e.name : e.type_id);
        if (e.paired) ImGui::TextDisabled(u8"车厂和船厂一起限制");
        ImGui::EndTooltip();
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
  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"建造限制");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"对局内所有玩家生效");

  int count = 0;
  const game_api::BuildLockEntry* cat = game_api::build_lock_catalog(&count);
  const float pane = ImGui::GetContentRegionAvail().y * 0.5f - 4.f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.12f, 0.10f, 0.55f));
  ImGui::BeginChild("##build_allow", ImVec2(0, pane), ImGuiChildFlags_Borders);
  ImGui::TextColored(ImVec4(0.55f, 0.86f, 0.62f, 1.f), u8"允许");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"点击图标后移到下方");
  if (count <= 0) {
    ImGui::TextWrapped(u8"还没读到可建造的单位。进入对局后再打开这一页。");
  } else {
    draw_lock_faction_icons(cat, count, false);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.08f, 0.08f, 0.55f));
  ImGui::BeginChild("##build_deny", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::TextColored(ImVec4(0.93f, 0.55f, 0.48f, 1.f), u8"禁止");
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

  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"摄像机");
  ImGui::SameLine();
  const int hook_state = game_api::camera_hook_state();
  if (hook_state == 1 && have_live) {
    ImGui::TextDisabled(u8"已接管引擎相机 (View %08X)", live.view);
  } else if (hook_state == 1) {
    ImGui::TextDisabled(u8"hook 已就绪，等待游戏更新相机…");
  } else if (hook_state < 0) {
    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.f), u8"相机 hook 安装失败，请重新注入");
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
        ImGui::SetTooltip(u8"打开后引擎每次设置相机都会被改成这里的目标值；关掉恢复游戏自己控制。"
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
      if (ImGui::Button(u8"把当前值应用到游戏", ImVec2(160, 0))) {
        push_edit();
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"读取游戏当前", ImVec2(120, 0))) {
        if (have_live) {
          s_edit = live;
          s_follow = true;
        }
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"停止接管", ImVec2(90, 0))) {
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
        if (ImGui::Button(u8"-##m", ImVec2(28, 0))) {
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
        if (ImGui::Button(u8"+##p", ImVec2(28, 0))) {
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
      if (ImGui::Button(st == 1 ? u8"暂停" : u8"播放", ImVec2(80, 0))) {
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
      if (ImGui::Button(u8"停止", ImVec2(70, 0))) {
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
      if (ImGui::Button(u8"复制当前坐标并添加节点", ImVec2(190, 0))) {
        const int idx = game_api::camera_key_add_from_current(s_new_label);
        if (idx < 0) {
          game_api::set_status(u8"添加节点失败：还没读到摄像机");
          game_api::beep("error");
        } else {
          game_api::beep("click");
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"清空轨迹", ImVec2(80, 0))) {
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
      dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), IM_COL32(14, 18, 24, 230), 4.f);
      dl->AddRect(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), IM_COL32(70, 80, 100, 200), 4.f);
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
          if (i > 0) dl->AddLine(prev, pt, IM_COL32(90, 170, 255, 220), 2.f);
          dl->AddCircleFilled(pt, 4.f, IM_COL32(255, 210, 90, 255));
          char num[8];
          std::snprintf(num, sizeof(num), "%d", i + 1);
          dl->AddText(ImVec2(pt.x + 6.f, pt.y - 16.f), IM_COL32(220, 230, 250, 255), num);
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
      if (ImGui::Button(u8"添加事件", ImVec2(100, 0))) {
        game_api::CameraEvent ev;
        ev.t = 0.f;
        ev.base = 0;
        ev.action = 0;
        game_api::camera_event_add(ev);
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"重置已触发", ImVec2(110, 0))) {
        game_api::camera_events_reset();
        game_api::beep("click");
      }
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.5f, 1.f), u8"%d / %d 个事件已触发", fired, ev_n);
      ImGui::SameLine();
      if (ImGui::Button(u8"清空", ImVec2(70, 0))) game_api::camera_events_clear();

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
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, 1.f), u8"已触发");
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
      if (ImGui::Button(u8"新建", ImVec2(60, 0))) {
        game_api::camera_config_add(u8"新轨迹");
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"克隆", ImVec2(60, 0))) {
        game_api::camera_config_clone(active, u8"");
        game_api::beep("click");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"删除", ImVec2(60, 0))) {
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
      if (ImGui::Button(u8"保存到文件", ImVec2(120, 0))) {
        const bool ok = game_api::camera_config_save();
        game_api::beep(ok ? "click" : "error");
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"从文件读取", ImVec2(120, 0))) {
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

  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"时间控制（加速·实验）");
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
  ImGui::TextColored(ImVec4(0.75f, 0.9f, 0.75f, 1.f), u8"| updateFPS = %d", update_fps);
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
  ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.f),
                     u8"硬性不变式：logicFPS >= 1 且 updateFPS >= logicFPS。"
                     u8"破坏后者会让 0x0065516D 的整除得 0，再在 0x0065519A 处除零崩溃");
  ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.f),
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
  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"快进到指定对局时间");

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
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.f), u8"快进中：目标 %s，倍率 %.1fx",
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

  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"动画 / Lua");
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
  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"读写任意 Lua 全局变量");
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

  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"杂项操作");
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

  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"隐藏显示");
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
  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"阴影");
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
  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"光照颜色");
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

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.16f, 0.80f));
  ImGui::BeginChild("##inspect", ImVec2(0, 210), ImGuiChildFlags_Borders);

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
    if (ImGui::Button(u8"保存", ImVec2(120, 0))) {
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
    if (ImGui::Button(u8"取消", ImVec2(120, 0))) {
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
  ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), u8"单位操作扩展");

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
  if (!input::menu_visible() && !(g_stats_open && g_stats_pinned)) return;

  g_sync_timer += ImGui::GetIO().DeltaTime;
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

  if (input::menu_visible()) {
    // Collapsed inspector frees ~220px; allow a shorter window in that state.
    const float kInspectBodyH = 220.f;
    const float win_min_h = g_unit_inspect_open ? 620.f : 400.f;
    ImGui::SetNextWindowSize(ImVec2(900, 720), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(780, win_min_h), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowPos(ImVec2(28, 28), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(g_ui_opacity);

    ImGui::Begin(u8"红色警戒3 · 进程内菜单", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.f), u8"RA3 Overlay");
    ImGui::SameLine();
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
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button(game_api::hooks_armed() ? u8"重新注入" : u8"注入",
                      ImVec2(100, 0))) {
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
    ImGui::SameLine();
    if (ImGui::Button(u8"卸载", ImVec2(60, 0))) {
      game_api::detach();
      g_last_msg = u8"已还原 hook";
      game_api::beep("off");
    }
    ImGui::SameLine();
    ImGui::Checkbox(u8"观战模式", &g_spectate_inject);
    ImGui::SameLine();
    if (ImGui::Button(u8"设置", ImVec2(60, 0))) {
      g_show_settings = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"统计", ImVec2(60, 0))) {
      g_stats_open = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"隐藏", ImVec2(60, 0))) input::set_menu_visible(false);
    ImGui::SameLine();
    ImGui::Checkbox(u8"调试", &g_show_debug_status);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(u8"在下方固定高度的信息框内显示底层状态；默认关闭，避免长日志把窗口撑高跳动。");
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(0.12f, 0.14f, 0.18f, g_ui_opacity));
    // 固定高度：内容超出时在框内滚动，绝不改变主窗口布局。
    const float msg_h = g_show_debug_status ? 72.f : 44.f;
    ImGui::BeginChild("##msg", ImVec2(0, msg_h), true,
                      ImGuiWindowFlags_None);
    if (g_show_debug_status) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.78f, 0.88f, 1.f));
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
    const float inspect_header_h = ImGui::GetFrameHeightWithSpacing() + 6.f;
    const float inspect_reserve =
        g_unit_inspect_open ? (inspect_header_h + kInspectBodyH) : inspect_header_h;
    const float side_w = 156.f;

    ImGui::BeginChild("##side", ImVec2(side_w, -inspect_reserve), true);
    for (int i = 0; i < group_count; ++i) {
      const bool sel = (!g_show_settings && !g_show_build_lock && !g_show_camera &&
                        !g_show_time && !g_show_lua && !g_show_misc && g_group == i);
      ImGui::PushID(i);
      if (draw_nav_item(groups[i].name, groups[i].name, sel)) {
        g_group = i;
        g_show_settings = false;
        g_show_build_lock = false;
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
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("lock", u8"建造限制", g_show_build_lock)) {
      g_show_build_lock = true;
      g_show_settings = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("cam", u8"摄像机", g_show_camera)) {
      g_show_camera = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_time = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("time", u8"时间控制", g_show_time)) {
      g_show_time = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_camera = false;
      g_show_lua = false;
      g_show_misc = false;
    }
    if (draw_nav_item("lua", u8"动画 / Lua", g_show_lua)) {
      g_show_lua = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_camera = false;
      g_show_time = false;
    }
    if (draw_nav_item("misc", u8"杂项操作", g_show_misc)) {
      g_show_misc = true;
      g_show_settings = false;
      g_show_build_lock = false;
      g_show_camera = false;
      g_show_time = false;
      g_show_lua = false;
    }
    if (draw_nav_item("ui", u8"界面设置", g_show_settings)) {
      g_show_settings = true;
      g_show_build_lock = false;
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
      ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.f), "%s", g.name);
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
  }

  draw_stats_window();
}

void end_frame(IDirect3DDevice9* device) {
  if (!g_inited || !device) return;
  ImGui::EndFrame();
  ImGui::Render();
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
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
