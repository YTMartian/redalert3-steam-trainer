#include "ui.h"
#include "input.h"
#include "game_api.h"

#include <Windows.h>
#include <d3d9.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace ui {
namespace {

bool g_inited = false;
HWND g_hwnd = nullptr;
int g_group = 0;
std::string g_last_msg =
    u8"先启动游戏并进局，再运行注入器 EXE。菜单内点「注入」。Home 显隐。";
bool g_spectate_inject = false;
float g_sync_timer = 0.f;
volatile LONG g_injecting = 0;
volatile LONG g_engine_busy = 0;

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

void apply_ra3_theme() {
  ImGuiStyle& s = ImGui::GetStyle();
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
  // Classic ImGui blue accents on dark panel
  const ImVec4 bg(0.08f, 0.09f, 0.11f, 0.96f);
  const ImVec4 panel(0.12f, 0.13f, 0.16f, 1.0f);
  const ImVec4 panel2(0.15f, 0.16f, 0.20f, 1.0f);
  const ImVec4 blue(0.26f, 0.59f, 0.98f, 1.0f);
  const ImVec4 blue_h(0.36f, 0.66f, 1.00f, 1.0f);
  const ImVec4 blue_a(0.18f, 0.45f, 0.85f, 1.0f);
  const ImVec4 text(0.92f, 0.93f, 0.95f, 1.0f);
  const ImVec4 muted(0.55f, 0.58f, 0.64f, 1.0f);
  const ImVec4 border(0.25f, 0.28f, 0.35f, 1.0f);

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
}

}  // namespace

bool init(IDirect3DDevice9* device, void* hwnd) {
  if (g_inited) return true;
  if (!device || !hwnd) return false;
  g_hwnd = static_cast<HWND>(hwnd);

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

  g_inited = true;
  if (!g_async_cs_ready) {
    InitializeCriticalSection(&g_async_cs);
    g_async_cs_ready = true;
  }
  game_api::log("ImGui UI ready (themed)");
  return true;
}

void shutdown() {
  if (!g_inited) return;
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

void draw() {
  if (!g_inited || !input::menu_visible()) return;

  g_sync_timer += ImGui::GetIO().DeltaTime;
  if (g_sync_timer > 1.0f) {
    g_sync_timer = 0.f;
    game_api::sync_spectator_hooks();
  }

  if (g_async_cs_ready) {
    EnterCriticalSection(&g_async_cs);
    if (g_async_result.done) {
      g_last_msg = g_async_result.msg;
      g_async_result.done = false;
    }
    LeaveCriticalSection(&g_async_cs);
  }

  ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(36, 36), ImGuiCond_FirstUseEver);

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
                    ImVec2(110, 0))) {
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
  if (ImGui::Button(u8"卸载", ImVec2(70, 0))) {
    game_api::detach();
    g_last_msg = u8"已还原 hook";
    game_api::beep("off");
  }
  ImGui::SameLine();
  ImGui::Checkbox(u8"观战模式", &g_spectate_inject);
  ImGui::SameLine();
  if (ImGui::Button(u8"隐藏菜单", ImVec2(90, 0))) input::set_menu_visible(false);

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 1.f));
  ImGui::BeginChild("##msg", ImVec2(0, 40), true);
  ImGui::TextWrapped("%s", g_last_msg.c_str());
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  int group_count = 0;
  const game_api::GroupInfo* groups = game_api::groups(&group_count);

  ImGui::BeginChild("##side", ImVec2(150, 0), true);
  for (int i = 0; i < group_count; ++i) {
    const bool sel = (g_group == i);
    if (sel) {
      ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.42f, 0.78f, 1.f));
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.52f, 0.90f, 1.f));
      ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.36f, 0.60f, 1.0f, 1.f));
    }
    if (ImGui::Selectable(groups[i].name, sel, 0, ImVec2(0, 28))) g_group = i;
    if (sel) ImGui::PopStyleColor(3);
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##body", ImVec2(0, 0), true);
  if (g_group >= 0 && g_group < group_count) {
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

  ImGui::End();
}

void end_frame(IDirect3DDevice9* device) {
  if (!g_inited || !device) return;
  ImGui::EndFrame();
  ImGui::Render();
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

bool want_capture_mouse() {
  if (!g_inited || !input::menu_visible()) return false;
  return ImGui::GetIO().WantCaptureMouse;
}

bool want_capture_keyboard() {
  if (!g_inited || !input::menu_visible()) return false;
  return ImGui::GetIO().WantCaptureKeyboard;
}

}  // namespace ui
