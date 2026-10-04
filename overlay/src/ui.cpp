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
bool g_stats_open = false;
bool g_stats_pinned = false;
std::string g_last_msg =
    u8"先启动游戏并进局，再运行注入器 EXE。菜单内点「注入」。Home 显隐主菜单。";
bool g_spectate_inject = false;
float g_sync_timer = 0.f;
volatile LONG g_injecting = 0;
volatile LONG g_engine_busy = 0;

// UI appearance (persisted). Defaults match the previous large layout.
float g_ui_scale = 1.00f;     // 0.70 ~ 1.30
float g_ui_opacity = 0.80f;   // 0.35 ~ 1.00
bool g_unit_inspect_open = true;
char g_ui_settings_path[MAX_PATH] = {};
bool g_settings_dirty = false;
float g_settings_save_timer = 0.f;

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
        std::strcmp(job->key, "spawn_mcv") == 0;
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
  // .../overlay/bin/dll → repo root
  for (int i = 0; i < 3; ++i) {
    wchar_t* slash = wcsrchr(wpath, L'\\');
    if (!slash) return false;
    *slash = 0;
  }
  wchar_t wfile[MAX_PATH] = {};
  _snwprintf_s(wfile, _TRUNCATE, L"%s\\overlay_ui.ini", wpath);
  return WideCharToMultiByte(CP_UTF8, 0, wfile, -1, out, (int)out_len, nullptr,
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
    } else if (key == "money_self_step") {
      game_api::set_money_self_step((int)std::atoi(val.c_str()));
    } else if (key == "money_sel_step") {
      game_api::set_money_sel_step((int)std::atoi(val.c_str()));
    } else if (key == "mcv_faction") {
      game_api::set_mcv_faction((int)std::atoi(val.c_str()));
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
  f << "money_self_step=" << game_api::money_self_step() << "\n";
  f << "money_sel_step=" << game_api::money_sel_step() << "\n";
  f << "mcv_faction=" << game_api::mcv_faction() << "\n";
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
  ImGuiStyle& s = ImGui::GetStyle();
  // Base metrics at scale=1.0 (previous default layout).
  s.WindowPadding = ImVec2(12, 12);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(10, 8);
  s.ItemInnerSpacing = ImVec2(8, 5);
  s.WindowRounding = 8.0f;
  s.ChildRounding = 6.0f;
  s.FrameRounding = 5.0f;
  s.PopupRounding = 5.0f;
  s.ScrollbarRounding = 6.0f;
  s.GrabRounding = 4.0f;
  s.TabRounding = 5.0f;
  s.WindowBorderSize = 1.0f;
  s.FrameBorderSize = 0.0f;
  s.ScrollbarSize = 12.0f;

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

  // ScaleAllSizes is cumulative — always rebuild base first, then scale once.
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
  ImGui::TextDisabled(u8"界面和快捷键都会自动保存到修改器目录的 overlay_ui.ini");
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

  if (p.roster_count > 0 || p.unit_total || p.building_total) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float icon_sz = 46.f;
    const float gap = 6.f;
    const float cell_w = icon_sz + 4.f;
    const float cell_h = icon_sz + 14.f;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    auto draw_roster_section = [&](const char* title, bool buildings, ImU32 accent_u32) {
      int n = 0;
      for (int i = 0; i < p.roster_count; ++i) {
        if (p.roster[i].is_building == buildings && p.roster[i].count > 0) ++n;
      }
      if (n <= 0) return;

      ImGui::TextColored(ImVec4(0.70f, 0.78f, 0.90f, 1.f), "%s", title);
      ImGui::SameLine();
      ImGui::TextDisabled("%d", buildings ? p.building_total : p.unit_total);
      ImGui::Dummy(ImVec2(0.f, 2.f));

      const float avail = ImGui::GetContentRegionAvail().x;
      float used_x = 0.f;
      bool row_started = false;

      for (int i = 0; i < p.roster_count; ++i) {
        const auto& r = p.roster[i];
        if (r.is_building != buildings || r.count <= 0) continue;

        if (row_started && used_x + cell_w + gap > avail) {
          ImGui::Dummy(ImVec2(0.f, 2.f));
          used_x = 0.f;
          row_started = false;
        }
        if (row_started) {
          ImGui::SameLine(0.f, gap);
        }

        ImGui::PushID(r.key[0] ? r.key : r.name);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 chip0 = p0;
        const ImVec2 chip1 = ImVec2(p0.x + cell_w, p0.y + cell_h);

        draw->AddRectFilled(chip0, chip1, IM_COL32(12, 16, 24, 180), 6.f);
        draw->AddRect(chip0, chip1, accent_u32, 6.f, 0, 1.0f);

        IDirect3DTexture9* tex = unit_icons::get(r.key, r.name, r.is_building);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 2.f, p0.y + 2.f));
        if (tex) {
          ImGui::Image((ImTextureID)tex, ImVec2(icon_sz, icon_sz));
        } else {
          ImGui::Dummy(ImVec2(icon_sz, icon_sz));
          draw->AddRectFilled(ImVec2(p0.x + 2.f, p0.y + 2.f),
                              ImVec2(p0.x + 2.f + icon_sz, p0.y + 2.f + icon_sz),
                              IM_COL32(40, 48, 60, 220), 4.f);
          const char* short_l = buildings ? u8"建" : u8"兵";
          const ImVec2 sts = ImGui::CalcTextSize(short_l);
          draw->AddText(ImVec2(p0.x + 2.f + (icon_sz - sts.x) * 0.5f,
                               p0.y + 2.f + (icon_sz - sts.y) * 0.5f),
                        IM_COL32(200, 210, 220, 255), short_l);
        }
        if (ImGui::IsMouseHoveringRect(chip0, chip1)) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(r.name[0] ? r.name : r.key);
          ImGui::TextDisabled("%s  × %d", buildings ? u8"建筑" : u8"部队", r.count);
          ImGui::EndTooltip();
        }

        char cnt[16];
        std::snprintf(cnt, sizeof(cnt), "%d", r.count);
        const ImVec2 cts = ImGui::CalcTextSize(cnt);
        const float bx1 = chip1.x - 2.f;
        const float by1 = chip1.y - 2.f;
        const float bx0 = bx1 - cts.x - 6.f;
        const float by0 = by1 - cts.y - 2.f;
        draw->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(8, 10, 14, 230),
                            4.f);
        draw->AddText(ImVec2(bx0 + 3.f, by0 + 1.f), IM_COL32(255, 230, 160, 255), cnt);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + cell_w, p0.y));
        ImGui::Dummy(ImVec2(0.1f, cell_h));
        ImGui::PopID();

        used_x += cell_w + gap;
        row_started = true;
      }
      ImGui::Spacing();
    };

    const ImU32 unit_accent =
        p.is_local ? IM_COL32(200, 160, 50, 120) : IM_COL32(70, 130, 210, 120);
    const ImU32 bld_accent =
        p.is_local ? IM_COL32(180, 140, 40, 140) : IM_COL32(90, 150, 220, 140);
    draw_roster_section(u8"部队", false, unit_accent);
    draw_roster_section(u8"建筑", true, bld_accent);
  }

  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGui::PopID();
  ImGui::Spacing();
}

void draw_stats_panel() {
  static game_api::MatchEconomy st{};
  static double last_poll = -1.0;
  const double now = ImGui::GetTime();
  if (last_poll < 0.0 || (now - last_poll) >= 0.20) {
    game_api::collect_match_economy(&st);
    last_poll = now;
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
  ImGui::Spacing();

  if (!st.valid) {
    ImGui::TextWrapped(u8"%s", st.note[0] ? st.note : u8"暂无数据：请先进入对局。");
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
  ImGui::TextDisabled(g_stats_pinned ? u8"主菜单隐藏后仍显示"
                                     : u8"取消固定后可关闭本窗口");

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
  cfg.OversampleH = 2;
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
      io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg,
                                   io.Fonts->GetGlyphRangesChineseFull());
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
         std::strcmp(key, "full_buff") == 0 || std::strcmp(key, "spawn_mcv") == 0;
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
  const float s = 56.f;
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
  constexpr float kIcon = 40.f;
  constexpr float kGap = 4.f;
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
      draw->AddRectFilled(p0, ImVec2(p0.x + kIcon, p0.y + kIcon), bg, 4.f);
      draw->AddRect(p0, ImVec2(p0.x + kIcon, p0.y + kIcon), edge, 4.f, 0, hovered ? 2.f : 1.f);
      const char* icon_id = e.icon_id[0] ? e.icon_id : e.type_id;
      const char* icon_name = e.icon_name[0] ? e.icon_name : e.name;
      IDirect3DTexture9* tex = unit_icons::get(icon_id, icon_name, e.building);
      if (tex) {
        draw->AddImage((ImTextureID)tex, ImVec2(p0.x + 2.f, p0.y + 2.f),
                       ImVec2(p0.x + kIcon - 2.f, p0.y + kIcon - 2.f));
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

static void draw_unit_inspector() {
  game_api::UnitInspect info{};
  game_api::inspect_first_selected(&info);

  static bool s_edit_open = false;
  static bool s_open_popup = false;
  static char s_edit_key[96] = {};
  static char s_edit_name[96] = {};
  static std::string s_edit_msg;

  char header[96];
  if (info.selected_total > 1) {
    std::snprintf(header, sizeof(header), u8"单位属性（已选 %d）###unit_insp",
                  info.selected_total);
  } else if (info.selected_total == 1) {
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
      ImGui::Text(u8"血量  %.0f / %.0f", info.hp_cur, info.hp_max);
      if (info.hp_max > 0.f) {
        ImGui::ProgressBar(info.hp_cur / info.hp_max, ImVec2(-1, 14), nullptr);
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
    ImGui::TextDisabled(u8"|  %s  |  选中 %d", game_api::mode_label(),
                        game_api::selected_count());
    ImGui::TextWrapped("%s", game_api::status_text());
    ImGui::Spacing();

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

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(0.12f, 0.14f, 0.18f, g_ui_opacity));
    ImGui::BeginChild("##msg", ImVec2(0, 44), true);
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
      const bool sel = (!g_show_settings && !g_show_build_lock && g_group == i);
      ImGui::PushID(i);
      if (draw_nav_item(groups[i].name, groups[i].name, sel)) {
        g_group = i;
        g_show_settings = false;
        g_show_build_lock = false;
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
    }
    if (draw_nav_item("lock", u8"建造限制", g_show_build_lock)) {
      g_show_build_lock = true;
      g_show_settings = false;
    }
    if (draw_nav_item("ui", u8"界面设置", g_show_settings)) {
      g_show_settings = true;
      g_show_build_lock = false;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##body", ImVec2(0, -inspect_reserve), true);
    if (g_show_settings) {
      draw_settings_panel();
    } else if (g_show_build_lock) {
      draw_build_lock_panel();
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
        const bool unit_full = feat && (std::strcmp(feat->key, "spawn_mcv") == 0 ||
                                        std::strcmp(feat->key, "spawn_rank") == 0);
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

bool want_capture_mouse() {
  if (!overlay_visible()) return false;
  return ImGui::GetIO().WantCaptureMouse;
}

bool want_capture_keyboard() {
  if (!overlay_visible()) return false;
  return ImGui::GetIO().WantCaptureKeyboard;
}

}  // namespace ui
