#include "ui.h"
#include "input.h"
#include "game_api.h"
#include "unit_icons.h"

#include <Windows.h>
#include <d3d9.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cfloat>
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
        std::strcmp(job->key, "spec_gift") == 0 ||
        std::strcmp(job->key, "unit_clone") == 0 ||
        std::strcmp(job->key, "spawn_unit") == 0 ||
        std::strcmp(job->key, "clone_multi") == 0 ||
        std::strcmp(job->key, "ore_convoy") == 0;
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
  ImGui::TextDisabled(u8"调整后自动保存到 overlay_ui.ini");
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
    if (tr->pts.size() > 2400) {
      tr->pts.erase(tr->pts.begin(), tr->pts.begin() + 400);
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

static void draw_trend_chart(const char* title, const TrendTrack* tr) {
  const float avail = ImGui::GetContentRegionAvail().x;
  const float h = 118.f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 box1(origin.x + avail, origin.y + h);
  dl->AddRectFilled(origin, box1, IM_COL32(10, 14, 22, 215), 6.f);
  dl->AddRect(origin, box1, IM_COL32(80, 100, 130, 70), 6.f, 0, 1.f);
  dl->AddText(ImVec2(origin.x + 8.f, origin.y + 4.f), IM_COL32(176, 192, 210, 255), title);

  const char* names[5] = {u8"建造", u8"损失", u8"消灭", u8"收入", u8"支出"};
  const ImU32 cols[5] = {
      IM_COL32(130, 214, 156, 255),
      IM_COL32(232, 122, 112, 255),
      IM_COL32(242, 186, 96, 255),
      IM_COL32(176, 132, 255, 255),
      IM_COL32(156, 186, 214, 255),
  };
  float lx = origin.x + ImGui::CalcTextSize(title).x + 18.f;
  for (int s = 0; s < 3; ++s) {
    dl->AddRectFilled(ImVec2(lx, origin.y + 8.f), ImVec2(lx + 10.f, origin.y + 16.f), cols[s], 2.f);
    lx += 14.f;
    dl->AddText(ImVec2(lx, origin.y + 3.f), cols[s], names[s]);
    lx += ImGui::CalcTextSize(names[s]).x + 10.f;
  }
  lx = origin.x + 8.f;
  for (int s = 3; s < 5; ++s) {
    dl->AddRectFilled(ImVec2(lx, origin.y + 26.f), ImVec2(lx + 10.f, origin.y + 34.f), cols[s], 2.f);
    lx += 14.f;
    dl->AddText(ImVec2(lx, origin.y + 21.f), cols[s], names[s]);
    lx += ImGui::CalcTextSize(names[s]).x + 10.f;
  }

  const float plot_x = origin.x + 8.f;
  const float plot_y = origin.y + 44.f;
  const float plot_w = std::max(8.f, avail - 16.f);
  const float plot_h = h - 52.f;
  dl->AddLine(ImVec2(plot_x, plot_y + plot_h * 0.5f),
              ImVec2(plot_x + plot_w, plot_y + plot_h * 0.5f), IM_COL32(255, 255, 255, 22));
  dl->AddLine(ImVec2(plot_x, plot_y + plot_h), ImVec2(plot_x + plot_w, plot_y + plot_h),
              IM_COL32(255, 255, 255, 28));

  const int n = tr ? (int)tr->pts.size() : 0;
  float tmin = 0.f, tmax = 1.f;
  uint32_t ymax = 4;
  uint32_t money_ymax = 4;
  if (n > 0) {
    tmin = tr->pts.front().t;
    tmax = tr->pts.back().t;
    if (tmax <= tmin) tmax = tmin + 1.f;
    uint32_t peak = 1;
    uint32_t money_peak = 1;
    for (const auto& pt : tr->pts) {
      for (int s = 0; s < 3; ++s) peak = std::max(peak, trend_total(pt, s));
      for (int s = 3; s < 5; ++s) money_peak = std::max(money_peak, trend_total(pt, s));
    }
    ymax = peak < 4 ? 4u : (peak + 3u) & ~3u;
    money_ymax = money_peak < 4 ? 4u : money_peak;
  }

  auto at_axis = [&](float t, uint32_t val, uint32_t axis_max) {
    const float u = (t - tmin) / (tmax - tmin);
    const float vv = (float)val / (float)axis_max;
    return ImVec2(plot_x + u * plot_w, plot_y + plot_h * (1.f - vv));
  };
  auto at = [&](float t, uint32_t val, int series) {
    return at_axis(t, val, series < 3 ? ymax : money_ymax);
  };

  if (n >= 1) {
    for (int s = 0; s < 5; ++s) {
      dl->PathClear();
      if (n == 1) {
        const uint32_t val = trend_total(tr->pts[0], s);
        dl->PathLineTo(at(tmin, val, s));
        dl->PathLineTo(at(tmax, val, s));
      } else {
        for (const auto& pt : tr->pts) dl->PathLineTo(at(pt.t, trend_total(pt, s), s));
      }
      dl->PathStroke(cols[s], 0, 2.f);
    }
  }

  const ImVec2 plot0(plot_x, plot_y);
  const ImVec2 plot1(plot_x + plot_w, plot_y + plot_h);
  if (n > 0 && ImGui::IsMouseHoveringRect(plot0, plot1)) {
    const float mx = ImGui::GetIO().MousePos.x;
    float u = (mx - plot_x) / plot_w;
    if (u < 0.f) u = 0.f;
    if (u > 1.f) u = 1.f;
    const float th = tmin + u * (tmax - tmin);
    int best = 0;
    float best_d = 1.0e9f;
    for (int i = 0; i < n; ++i) {
      float d = tr->pts[i].t - th;
      if (d < 0.f) d = -d;
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    const TrendPt& pt = tr->pts[best];
    const float x = at(pt.t, 0, 0).x;
    dl->AddLine(ImVec2(x, plot_y), ImVec2(x, plot_y + plot_h), IM_COL32(255, 255, 255, 110), 1.f);
    for (int s = 0; s < 5; ++s) {
      const ImVec2 p = at(pt.t, trend_total(pt, s), s);
      dl->AddCircleFilled(p, 3.f, cols[s]);
    }
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

  ImGui::Dummy(ImVec2(avail, h));
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

static void draw_feature_row(const game_api::FeatureInfo* f) {
  if (!f) return;
  const bool armed = game_api::hooks_armed();
  const char* hk = game_api::hotkey_hint(f->key);
  char label[160];
  if (hk) {
    std::snprintf(label, sizeof(label), "%s   (%s)", f->label, hk);
  } else {
    std::snprintf(label, sizeof(label), "%s", f->label);
  }

  if (std::strcmp(f->type, "money") == 0 || std::strcmp(f->type, "money_sel") == 0) {
    const bool sel = (std::strcmp(f->type, "money_sel") == 0);
    ImGui::PushID(f->key);
    int step = sel ? game_api::money_sel_step() : game_api::money_self_step();
    ImGui::TextUnformatted(f->label);
    if (sel) {
      ImGui::SameLine();
      ImGui::TextDisabled(u8"（先选中该玩家的单位）");
    } else {
      const char* hk = game_api::hotkey_hint(f->key);
      if (hk) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", hk);
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
    bool on = game_api::feature_enabled(f->key);
    if (!armed) ImGui::BeginDisabled();
    if (ImGui::Checkbox(label, &on)) {
      std::string msg;
      bool ok = game_api::toggle_feature(f->key, on, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? (on ? "on" : "off") : "error");
    }
    if (!armed) ImGui::EndDisabled();
    return;
  }

  if (std::strcmp(f->type, "pulse") == 0) {
    if (!armed) ImGui::BeginDisabled();
    if (ImGui::Button(label, ImVec2(-1, 0))) {
      std::string msg;
      bool ok = game_api::pulse_feature(f->key, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    if (!armed) ImGui::EndDisabled();
    return;
  }

  if (std::strcmp(f->type, "danger") == 0) {
    ImGui::TextUnformatted(label);
    int level = (int)game_api::get_flag(0x13);
    if (level < 0 || level > 2) level = 0;
    if (!armed) ImGui::BeginDisabled();
    bool changed = false;
    if (ImGui::RadioButton(u8"正常", &level, 0)) changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(u8"高", &level, 1)) changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(u8"最高", &level, 2)) changed = true;
    if (changed) {
      std::string msg;
      bool ok = game_api::set_danger(level, &msg);
      g_last_msg = msg;
      game_api::beep(ok ? "click" : "error");
    }
    if (!armed) ImGui::EndDisabled();
    return;
  }

  if (std::strcmp(f->type, "engine") == 0) {
    const bool need_hooks =
        std::strcmp(f->key, "protocol_ready") == 0 ||
        std::strcmp(f->key, "unit_skill_ready") == 0 ||
        std::strcmp(f->key, "disable_protocol") == 0 ||
        std::strcmp(f->key, "unit_clone") == 0 ||
        std::strcmp(f->key, "spawn_unit") == 0 ||
        std::strcmp(f->key, "clone_multi") == 0 ||
        std::strcmp(f->key, "ore_convoy") == 0 ||
        std::strcmp(f->key, "spec_gift") == 0 ||
        std::strcmp(f->key, "full_buff") == 0;
    const bool busy = InterlockedCompareExchange(&g_engine_busy, 0, 0) != 0;
    if ((need_hooks && !armed) || busy) ImGui::BeginDisabled();
    if (ImGui::Button(label, ImVec2(-1, 0))) {
      // Must not call CreateUnit while blocked inside Present — run async.
      if (start_engine_async(f->key)) {
        g_last_msg = u8"正在执行…（请把鼠标移到地形上）";
      } else {
        g_last_msg = u8"上一次操作仍在进行中";
        game_api::beep("error");
      }
    }
    if ((need_hooks && !armed) || busy) ImGui::EndDisabled();
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
    const float side_w = 168.f;
    const float row_h = 30.f;

    ImGui::BeginChild("##side", ImVec2(side_w, -inspect_reserve), true);
    for (int i = 0; i < group_count; ++i) {
      const bool sel = (!g_show_settings && !g_show_build_lock && g_group == i);
      if (sel) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.42f, 0.78f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.52f, 0.90f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.36f, 0.60f, 1.0f, 1.f));
      }
      if (ImGui::Selectable(groups[i].name, sel, 0, ImVec2(0, row_h))) {
        g_group = i;
        g_show_settings = false;
        g_show_build_lock = false;
      }
      if (sel) ImGui::PopStyleColor(3);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    {
      const bool sel = g_stats_open;
      if (sel) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.42f, 0.78f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.52f, 0.90f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.36f, 0.60f, 1.0f, 1.f));
      }
      if (ImGui::Selectable(u8"战况统计", sel, 0, ImVec2(0, row_h))) {
        if (!g_stats_open) {
          g_stats_open = true;
        } else if (!g_stats_pinned) {
          g_stats_open = false;
        }
        g_show_settings = false;
        g_show_build_lock = false;
      }
      if (sel) ImGui::PopStyleColor(3);
    }
    {
      const bool sel = g_show_build_lock;
      if (sel) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.42f, 0.78f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.52f, 0.90f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.36f, 0.60f, 1.0f, 1.f));
      }
      if (ImGui::Selectable(u8"建造限制", sel, 0, ImVec2(0, row_h))) {
        g_show_build_lock = true;
        g_show_settings = false;
      }
      if (sel) ImGui::PopStyleColor(3);
    }
    {
      const bool sel = g_show_settings;
      if (sel) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.42f, 0.78f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.52f, 0.90f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.36f, 0.60f, 1.0f, 1.f));
      }
      if (ImGui::Selectable(u8"界面设置", sel, 0, ImVec2(0, row_h))) {
        g_show_settings = true;
        g_show_build_lock = false;
      }
      if (sel) ImGui::PopStyleColor(3);
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
      for (int i = 0; i < g.key_count; ++i) {
        draw_feature_row(game_api::find_feature(g.keys[i]));
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
