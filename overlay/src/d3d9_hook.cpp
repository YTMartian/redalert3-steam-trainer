#include "d3d9_hook.h"
#include "ui.h"
#include "input.h"
#include "game_api.h"

#include <Windows.h>
#include <Psapi.h>
#include <d3d9.h>
#include <imm.h>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "imm32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                            LPARAM lParam);

namespace d3d9_hook {
namespace {

using EndScene_t = HRESULT(APIENTRY*)(IDirect3DDevice9*);
using Present_t = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                     const RGNDATA*);
using Reset_t = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SwapPresent_t = HRESULT(APIENTRY*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                         const RGNDATA*, DWORD);

EndScene_t g_orig_end_scene = nullptr;
Present_t g_orig_present = nullptr;
Reset_t g_orig_reset = nullptr;
SwapPresent_t g_orig_swap_present = nullptr;

using BeginScene_t = HRESULT(APIENTRY*)(IDirect3DDevice9*);
BeginScene_t g_orig_begin_scene = nullptr;

void* g_target_end_scene = nullptr;
void* g_target_present = nullptr;
void* g_target_begin_scene = nullptr;
void* g_target_reset = nullptr;
void* g_target_swap_present = nullptr;

bool g_hooked = false;
bool g_ui_ready = false;
WNDPROC g_orig_wndproc = nullptr;
HWND g_game_hwnd = nullptr;
HANDLE g_hook_thread = nullptr;
HANDLE g_input_thread = nullptr;
volatile bool g_stop = false;
// 卸载时用：还有几帧正卡在 hk_present 里没返回。必须等它归零再让 DLL 走。
volatile LONG g_present_inflight = 0;
// 正在卸载：所有 D3D detour 直接转发原函数。FreeLibrary 时游戏还在跑渲染线程，
// 如果 detour 继续执行 ImGui / 游戏调用，等 DLL 被映射掉就会在裸内存里崩。
volatile bool g_unloading = false;
volatile LONG g_endscene_count = 0;
volatile LONG g_present_count = 0;
volatile LONG g_swap_present_count = 0;
volatile LONG g_drew_this_frame = 0;
volatile LONG g_skip_draw = 0;
// 「禁用游戏中的鼠标输入」：勾选后吞掉发往游戏窗口的鼠标消息。
volatile LONG g_game_mouse_off = 0;
IDirect3DDevice9* g_ui_device = nullptr;

HMODULE g_self = nullptr;
HIMC g_overlay_imc = nullptr;

bool module_contains(HMODULE mod, void* addr) {
  if (!mod || !addr) {
    return false;
  }
  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info))) {
    return false;
  }
  auto* base = static_cast<unsigned char*>(info.lpBaseOfDll);
  auto* p = static_cast<unsigned char*>(addr);
  return p >= base && p < base + info.SizeOfImage;
}

bool is_our_module(void* addr) { return module_contains(g_self, addr); }

bool is_usable_target(void* addr) {
  if (!addr || is_our_module(addr)) {
    return false;
  }
  // Accept code inside d3d9 or Steam overlay trampolines.
  HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
  HMODULE ov = GetModuleHandleW(L"gameoverlayrenderer.dll");
  return module_contains(d3d9, addr) || module_contains(ov, addr) ||
         !is_our_module(addr);
}

bool io_want_keyboard_block(UINT msg) {
  if (!(msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_IME_CHAR ||
        msg == WM_IME_COMPOSITION || msg == WM_IME_STARTCOMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_IME_NOTIFY ||
        msg == WM_IME_REQUEST || msg == WM_IME_SETCONTEXT ||
        msg == WM_IME_SELECT || msg == WM_IME_CONTROL)) {
    return false;
  }
  return ImGui::GetIO().WantCaptureKeyboard || ImGui::GetIO().WantTextInput;
}

void ensure_ime_enabled(HWND hwnd) {
  if (!hwnd) return;
  // RA3 (and many games) call ImmAssociateContext(hwnd, NULL) to disable IME.
  // Keep our own HIMC associated while ImGui text fields are active.
  if (!g_overlay_imc) {
    g_overlay_imc = ImmCreateContext();
  }
  if (g_overlay_imc) {
    ImmAssociateContext(hwnd, g_overlay_imc);
  } else {
    ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
  }
}

bool is_ime_msg(UINT msg) {
  return msg == WM_IME_SETCONTEXT || msg == WM_IME_NOTIFY ||
         msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_ENDCOMPOSITION ||
         msg == WM_IME_COMPOSITION || msg == WM_IME_CHAR ||
         msg == WM_IME_REQUEST || msg == WM_IME_SELECT || msg == WM_IME_CONTROL;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ui::overlay_visible()) {
    if (ImGui::GetIO().WantTextInput) {
      ensure_ime_enabled(hwnd);
    }
    const LRESULT imgui_result =
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    // Swallow mouse to the game while cursor is over any ImGui window, so
    // clicking overlay buttons does not clear the in-game selection.
    const bool mouse_msg =
        (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL ||
        msg == WM_MOUSEHWHEEL;
    if (mouse_msg) {
      ImGuiIO& io = ImGui::GetIO();
      if (io.WantCaptureMouse || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        return 0;
      }
    }
    if (io_want_keyboard_block(msg)) {
      // ImGui already DefWindowProc'd WM_IME_COMPOSITION. For other IME msgs,
      // use DefWindowProc (not the game) so candidate/composition UI works and
      // the game cannot re-disable IME mid-input.
      if (is_ime_msg(msg)) {
        if (msg == WM_IME_COMPOSITION || imgui_result != 0) {
          return imgui_result;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
      }
      return 0;
    }
  }
  if (!g_orig_wndproc) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  // 「禁用游戏中的鼠标输入」：吞掉鼠标消息，游戏不再响应悬停/点击。
  if (InterlockedCompareExchange(&g_game_mouse_off, 0, 0) != 0) {
    const bool mouse_msg =
        (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL ||
        msg == WM_MOUSEHWHEEL;
    if (mouse_msg) return 0;
  }
  return CallWindowProcW(g_orig_wndproc, hwnd, msg, wparam, lparam);
}

HWND find_device_window(IDirect3DDevice9* device) {
  D3DDEVICE_CREATION_PARAMETERS params{};
  if (FAILED(device->GetCreationParameters(&params))) {
    return nullptr;
  }
  return params.hFocusWindow;
}

void teardown_ui() {
  if (g_ui_ready && g_game_hwnd && g_orig_wndproc) {
    SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  if (g_overlay_imc) {
    if (g_game_hwnd) {
      ImmAssociateContext(g_game_hwnd, nullptr);
    }
    ImmDestroyContext(g_overlay_imc);
    g_overlay_imc = nullptr;
  }
  ui::shutdown();
  g_ui_ready = false;
  g_game_hwnd = nullptr;
  g_ui_device = nullptr;
}

void ensure_ui(IDirect3DDevice9* device) {
  if (!device) {
    return;
  }
  HWND hwnd = find_device_window(device);
  if (!hwnd) {
    hwnd = FindWindowW(nullptr, L"Command & Conquer(tm) Red Alert(tm) 3");
  }
  if (!hwnd) {
    hwnd = GetForegroundWindow();
  }
  if (!hwnd) {
    return;
  }

  // Device or focus window changed (common when entering a match) — full reinit.
  if (g_ui_ready && (device != g_ui_device || hwnd != g_game_hwnd)) {
    game_api::log("D3D device/hwnd changed — reinit ImGui");
    teardown_ui();
  }

  if (g_ui_ready) {
    return;
  }
  if (!ui::init(device, hwnd)) {
    game_api::log("ImGui init failed");
    return;
  }
  g_game_hwnd = hwnd;
  g_ui_device = device;
  g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_ui_ready = true;
  ensure_ime_enabled(hwnd);
  // First frame: open menu once so user finds it; later reinits keep current visibility.
  static bool s_first_ui = true;
  if (s_first_ui) {
    input::set_menu_visible(true);
    s_first_ui = false;
  }
  game_api::log("ImGui ready hwnd=%p device=%p unicode=%d", hwnd, device,
                IsWindowUnicode(hwnd) ? 1 : 0);
}

static volatile LONG g_draw_faults = 0;

void render_overlay(IDirect3DDevice9* device);

static int draw_exception_filter(EXCEPTION_POINTERS* ep) {
  const LONG n = InterlockedIncrement(&g_draw_faults);
  if (n <= 3) game_api::log_exception(ep, "overlay draw");
  return EXCEPTION_EXECUTE_HANDLER;
}

void render_overlay_seh(IDirect3DDevice9* device) {
  game_api::install_crash_filter();
  if (g_draw_faults >= 3) return;
  __try {
    render_overlay(device);
  } __except (draw_exception_filter(GetExceptionInformation())) {
  }
}

static double qpc_ms_since(LARGE_INTEGER start, LARGE_INTEGER freq) {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  if (freq.QuadPart <= 0) return 0.0;
  return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

void render_overlay(IDirect3DDevice9* device) {
  if (!device || g_skip_draw) {
    return;
  }
  LARGE_INTEGER freq{}, t0{}, t1{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  game_api::build_lock_tick();
  LARGE_INTEGER t_bl{};
  QueryPerformanceCounter(&t_bl);
  game_api::refresh_power_cooldowns();
  game_api::sync_dispel_shroud();
  LARGE_INTEGER t_pw{};
  QueryPerformanceCounter(&t_pw);
  // Roster classify/scan only while 战况统计 is open; otherwise forget-only.
  game_api::roster_set_active(ui::stats_roster_wanted());
  game_api::roster_tick();
  QueryPerformanceCounter(&t1);
  const double ms_ticks = qpc_ms_since(t0, freq);
  const double ms_build = (freq.QuadPart > 0)
                              ? (double)(t_bl.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
                              : 0.0;
  const double ms_pwr = (freq.QuadPart > 0)
                            ? (double)(t_pw.QuadPart - t_bl.QuadPart) * 1000.0 / (double)freq.QuadPart
                            : 0.0;
  const double ms_roster = (freq.QuadPart > 0)
                               ? (double)(t1.QuadPart - t_pw.QuadPart) * 1000.0 / (double)freq.QuadPart
                               : 0.0;

  // Avoid touching D3D/ImGui during load screens when overlay is closed.
  if (!ui::overlay_visible() && g_ui_ready) {
    return;
  }
  const HRESULT coop = device->TestCooperativeLevel();
  if (coop != D3D_OK) {
    return;
  }
  // Only create UI when user opens the menu (Home) or pinned stats is on.
  if (!ui::overlay_visible() && !g_ui_ready) {
    return;
  }
  ensure_ui(device);
  if (!g_ui_ready) {
    return;
  }
  if (InterlockedCompareExchange(&g_drew_this_frame, 1, 0) != 0) {
    return;
  }
  // Game may re-disable IME every frame; re-associate while typing.
  // Check after draw so WantTextInput reflects the active InputText this frame.
  LARGE_INTEGER t_draw0{};
  QueryPerformanceCounter(&t_draw0);
  ui::begin_frame();
  ui::draw();
  if (ImGui::GetIO().WantTextInput) {
    ensure_ime_enabled(g_game_hwnd);
  }
  LARGE_INTEGER t_draw1{};
  QueryPerformanceCounter(&t_draw1);
  ui::end_frame(device);
  const double ms_ui = qpc_ms_since(t_draw0, freq);
  const double ms_end = qpc_ms_since(t_draw1, freq);
  const double ms_total = qpc_ms_since(t0, freq);

  // Log only slow Present draws so we can see which segment hitchs.
  static DWORD s_next_perf_log = 0;
  const DWORD now = GetTickCount();
  if (ms_total >= 8.0 && (int32_t)(now - s_next_perf_log) >= 0) {
    s_next_perf_log = now + 1000;
    ImDrawData* dd = ImGui::GetDrawData();
    game_api::log("perf: overlay total=%.1fms ticks=%.1f (bl=%.1f pwr=%.1f rost=%.1f) "
                  "ui=%.1f render=%.1f vtx=%d idx=%d cmdlists=%d menu=%d",
                  ms_total, ms_ticks, ms_build, ms_pwr, ms_roster, ms_ui - ms_end, ms_end,
                  dd ? dd->TotalVtxCount : 0, dd ? dd->TotalIdxCount : 0,
                  dd ? dd->CmdListsCount : 0, input::menu_visible() ? 1 : 0);
  }
}

HRESULT APIENTRY hk_end_scene(IDirect3DDevice9* device) {
  if (g_unloading) return g_orig_end_scene(device);
  game_api::drain_unit_spawns();
  InterlockedIncrement(&g_endscene_count);
  // Draw only from Present. Creating the font texture inside EndScene faults d3d9.
  return g_orig_end_scene(device);
}

// BeginScene 是"场景逻辑已经走完、刚开始画这一帧"的位置：在这里写对象位置，
// 本帧渲染就会用到我们的值。光在 Present 里写是来不及的——Present 在逻辑之后，
// 下一次物理更新又会把 z 按回地面，画面就一直是贴地的。
HRESULT APIENTRY hk_begin_scene(IDirect3DDevice9* device) {
  if (g_unloading) return g_orig_begin_scene(device);
  InterlockedIncrement(&g_present_inflight);
  __try {
    game_api::air_hold_tick();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  InterlockedDecrement(&g_present_inflight);
  return g_orig_begin_scene(device);
}

HRESULT APIENTRY hk_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst,
                            HWND hwnd, const RGNDATA* dirty) {
  // 卸载中：立刻转发，绝不再碰 ImGui / 游戏对象。否则这个 detour 会拿着
  // 即将被 FreeLibrary 掉的代码和资源继续干活（典型表现就是退出/重注入时崩）。
  if (g_unloading) return g_orig_present(device, src, dst, hwnd, dirty);
  InterlockedIncrement(&g_present_inflight);
  LARGE_INTEGER freq{}, t0{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  game_api::drain_unit_spawns();
  game_api::air_hold_tick();
  game_api::camera_tick();
  game_api::time_control_tick();
  game_api::lua_bridge_tick();
  game_api::misc_tick();
  game_api::invincible_tick();
  ui::pump_pending_settings();
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t1);
  InterlockedIncrement(&g_present_count);
  render_overlay_seh(device);
  const double ms_pre = (freq.QuadPart > 0)
                            ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
                            : 0.0;
  static DWORD s_next_pre_log = 0;
  const DWORD now = GetTickCount();
  if (ms_pre >= 5.0 && (int32_t)(now - s_next_pre_log) >= 0) {
    s_next_pre_log = now + 1000;
    game_api::log("perf: present-pre=%.1fms (drain+camera+time+lua+misc)", ms_pre);
  }
  const HRESULT hr = g_orig_present(device, src, dst, hwnd, dirty);
  InterlockedExchange(&g_drew_this_frame, 0);
  InterlockedDecrement(&g_present_inflight);
  return hr;
}

HRESULT APIENTRY hk_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
  if (g_unloading) return g_orig_reset(device, params);
  // Do not touch ImGui inside Reset — RA3 match transitions are fragile here.
  InterlockedExchange(&g_skip_draw, 1);
  InterlockedExchange(&g_drew_this_frame, 0);
  if (g_ui_ready) {
    teardown_ui();
    game_api::log("D3D Reset — ImGui torn down (will recreate on next menu open)");
  }
  const HRESULT hr = g_orig_reset(device, params);
  InterlockedExchange(&g_skip_draw, 0);
  return hr;
}

HRESULT APIENTRY hk_swap_present(IDirect3DSwapChain9* swap, const RECT* src, const RECT* dst,
                                 HWND hwnd, const RGNDATA* dirty, DWORD flags) {
  if (g_unloading) return g_orig_swap_present(swap, src, dst, hwnd, dirty, flags);
  game_api::drain_unit_spawns();
  InterlockedIncrement(&g_swap_present_count);
  IDirect3DDevice9* device = nullptr;
  if (swap && SUCCEEDED(swap->GetDevice(&device)) && device) {
    render_overlay_seh(device);
    device->Release();
  }
  const HRESULT hr = g_orig_swap_present(swap, src, dst, hwnd, dirty, flags);
  InterlockedExchange(&g_drew_this_frame, 0);
  return hr;
}

bool create_dummy_device(DWORD behavior, IDirect3DDevice9** out_device, HWND* out_hwnd,
                         IDirect3D9** out_d3d) {
  *out_device = nullptr;
  *out_hwnd = nullptr;
  *out_d3d = nullptr;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ra3_overlay_d3d9_dummy";
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64,
                            64, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    return false;
  }

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    DestroyWindow(hwnd);
    return false;
  }

  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;

  IDirect3DDevice9* device = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 behavior, &pp, &device);
  if (FAILED(hr) || !device) {
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behavior,
                           &pp, &device);
  }
  if (FAILED(hr) || !device) {
    d3d->Release();
    DestroyWindow(hwnd);
    return false;
  }
  *out_device = device;
  *out_hwnd = hwnd;
  *out_d3d = d3d;
  return true;
}

bool install_minhooks_from_device(IDirect3DDevice9* device) {
  void** vtable = *reinterpret_cast<void***>(device);
  if (!vtable) {
    return false;
  }

  void* present = vtable[17];
  void* reset = vtable[16];
  void* end_scene = vtable[42];
  void* begin_scene = vtable[41];

  game_api::log("vtable Present=%p EndScene=%p Reset=%p BeginScene=%p", present, end_scene,
                reset, begin_scene);

  if (is_our_module(present) || is_our_module(end_scene)) {
    game_api::log("ERROR: vtable already points into overlay DLL — restart the game, then inject once");
    return false;
  }

  if (!is_usable_target(present) && !is_usable_target(end_scene)) {
    game_api::log("ERROR: Present/EndScene targets not usable");
    return false;
  }

  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
    game_api::log("MH_Initialize failed: %d", (int)st);
    return false;
  }

  bool any = false;

  if (is_usable_target(present) && !g_target_present) {
    st = MH_CreateHook(present, reinterpret_cast<void*>(&hk_present),
                       reinterpret_cast<void**>(&g_orig_present));
    if (st == MH_OK) {
      g_target_present = present;
      any = true;
      game_api::log("hooked Present @ %p", present);
    } else {
      game_api::log("MH_CreateHook Present failed: %d", (int)st);
    }
  }

  if (is_usable_target(end_scene) && !g_target_end_scene) {
    st = MH_CreateHook(end_scene, reinterpret_cast<void*>(&hk_end_scene),
                       reinterpret_cast<void**>(&g_orig_end_scene));
    if (st == MH_OK) {
      g_target_end_scene = end_scene;
      any = true;
      game_api::log("hooked EndScene @ %p", end_scene);
    } else {
      game_api::log("MH_CreateHook EndScene failed: %d", (int)st);
    }
  }

  if (is_usable_target(begin_scene) && !g_target_begin_scene) {
    st = MH_CreateHook(begin_scene, reinterpret_cast<void*>(&hk_begin_scene),
                       reinterpret_cast<void**>(&g_orig_begin_scene));
    if (st == MH_OK) {
      g_target_begin_scene = begin_scene;
      any = true;
      game_api::log("hooked BeginScene @ %p (air hold pin point)", begin_scene);
    } else {
      game_api::log("MH_CreateHook BeginScene failed: %d", (int)st);
    }
  }

  if (is_usable_target(reset) && !g_target_reset) {
    st = MH_CreateHook(reset, reinterpret_cast<void*>(&hk_reset),
                       reinterpret_cast<void**>(&g_orig_reset));
    if (st == MH_OK) {
      g_target_reset = reset;
      any = true;
      game_api::log("hooked Reset @ %p", reset);
    }
  }

  // SwapChain::Present (index 3) — some titles present only via swapchain.
  IDirect3DSwapChain9* swap = nullptr;
  if (SUCCEEDED(device->GetSwapChain(0, &swap)) && swap) {
    void** svt = *reinterpret_cast<void***>(swap);
    void* swap_present = svt ? svt[3] : nullptr;
    game_api::log("swapchain Present=%p", swap_present);
    if (is_usable_target(swap_present) && !is_our_module(swap_present) &&
        !g_target_swap_present) {
      st = MH_CreateHook(swap_present, reinterpret_cast<void*>(&hk_swap_present),
                         reinterpret_cast<void**>(&g_orig_swap_present));
      if (st == MH_OK) {
        g_target_swap_present = swap_present;
        any = true;
        game_api::log("hooked SwapChain::Present @ %p", swap_present);
      } else {
        game_api::log("MH_CreateHook SwapPresent failed: %d", (int)st);
      }
    }
    swap->Release();
  }

  if (!any) {
    return false;
  }

  st = MH_EnableHook(MH_ALL_HOOKS);
  if (st != MH_OK) {
    game_api::log("MH_EnableHook failed: %d", (int)st);
    return false;
  }
  g_hooked = true;
  return true;
}

bool try_install_hooks() {
  const DWORD behaviors[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };
  for (DWORD b : behaviors) {
    IDirect3DDevice9* device = nullptr;
    IDirect3D9* d3d = nullptr;
    HWND hwnd = nullptr;
    if (!create_dummy_device(b, &device, &hwnd, &d3d)) {
      continue;
    }
    const bool ok = install_minhooks_from_device(device);
    device->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    if (ok) {
      return true;
    }
    // If dirty vtable, don't keep trying other behaviors.
    if (is_our_module(g_target_present) || g_hooked) {
      break;
    }
  }
  return g_hooked;
}

DWORD WINAPI input_thread(LPVOID) {
  game_api::log("input thread started (Home / Insert / F8)");
  while (!g_stop) {
    input::poll();
    static LONG last_sum = -1;
    const LONG sum = g_endscene_count + g_present_count + g_swap_present_count;
    if (sum != last_sum) {
      last_sum = sum;
      if (sum == 1 || sum == 2 || (sum % 300) == 0) {
        game_api::log("frames EndScene=%ld Present=%ld SwapPresent=%ld ui=%d menu=%d",
                      g_endscene_count, g_present_count, g_swap_present_count,
                      g_ui_ready ? 1 : 0, input::menu_visible() ? 1 : 0);
      }
    }
    Sleep(16);
  }
  return 0;
}

DWORD WINAPI hook_thread(LPVOID) {
  game_api::log("hook thread started (MinHook inline)");
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&hook_thread), &g_self);

  for (int i = 0; i < 1200 && !g_stop; ++i) {
    if (GetModuleHandleW(L"d3d9.dll")) {
      break;
    }
    Sleep(100);
  }
  if (g_stop) {
    return 0;
  }
  if (!GetModuleHandleW(L"d3d9.dll")) {
    game_api::log("d3d9.dll not loaded");
    return 0;
  }
  game_api::log("d3d9.dll @ %p self=%p", GetModuleHandleW(L"d3d9.dll"), g_self);

  bool ok = false;
  for (int attempt = 0; attempt < 20 && !g_stop; ++attempt) {
    if (try_install_hooks()) {
      ok = true;
      break;
    }
    Sleep(500);
  }
  if (!ok) {
    game_api::log("FAILED to install Present/EndScene hooks — restart game and reinject");
    return 0;
  }
  game_api::log("hooks armed — waiting for first Present/EndScene");
  return 0;
}

}  // namespace

bool start() {
  g_stop = false;
  g_hook_thread = CreateThread(nullptr, 0, hook_thread, nullptr, 0, nullptr);
  g_input_thread = CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr);
  return g_hook_thread != nullptr;
}

void stop() {
  // 卸载顺序很重要。游戏这时还在跑渲染线程，注入器是用 FreeLibrary 远程卸载我们的，
  // 所以：
  //   1) 先立 unload 旗标 —— 之后的 Present/EndScene 直接转发，不再碰 ImGui / 游戏对象
  //   2) 还原窗口过程（它是游戏消息循环会进来的指针，必须最先从我们代码上摘掉）
  //   3) 卸 MinHook（内部会挂起线程，保证没人在 detour/trampoline 里）
  //   4) 等一下，让还在 detour 尾巴上的帧走完，再停线程
  //   5) 界面资源不在这里释放：跨线程动 D3D 设备是这套崩溃的另一半原因，
  //      而且进程随后就没了，泄漏几 MB 纹理远比崩掉划算
  g_unloading = true;
  g_stop = true;
  // 先把高频夹持线程停掉：它每 8ms 往游戏内存里写位置，DLL 被卸掉之后还在跑就是
  // "退出时崩在 dll_unloaded"那一类。它不碰钩子，停在这里最安全。
  __try {
    game_api::air_hold_shutdown();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (g_game_hwnd && g_orig_wndproc) {
    SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  if (g_hooked || g_target_present || g_target_end_scene || g_target_reset ||
      g_target_swap_present || g_target_begin_scene) {
    MH_DisableHook(MH_ALL_HOOKS);
  }
  // 等还在 hk_present 里的帧走完（Present 可能被 vsync 卡住），最多等 800ms。
  for (int i = 0; i < 160; ++i) {
    if (InterlockedCompareExchange(&g_present_inflight, 0, 0) <= 0) break;
    Sleep(5);
  }
  if (g_hooked) {
    MH_Uninitialize();
    g_hooked = false;
  }
  auto stop_thread = [](HANDLE& h) {
    if (!h) return;
    if (WaitForSingleObject(h, 1200) == WAIT_TIMEOUT) {
      // 线程要是卡在游戏函数里出不来，就强杀：留着它，等 DLL 被映射掉照样是崩。
      TerminateThread(h, 0);
    }
    CloseHandle(h);
    h = nullptr;
  };
  stop_thread(g_input_thread);
  stop_thread(g_hook_thread);
  game_api::log("overlay unhooked (safe unload path)");
}

}  // namespace d3d9_hook

namespace game_api {

// 「禁用游戏中的鼠标输入」：勾选后 hk_wndproc 吞掉发往游戏窗口的鼠标消息。
// 对应 CameraBridge 的「在游戏中禁用输入」。
void set_game_input_disabled(bool on) {
  InterlockedExchange(&d3d9_hook::g_game_mouse_off, on ? 1 : 0);
}

bool game_input_disabled() {
  return InterlockedCompareExchange(&d3d9_hook::g_game_mouse_off, 0, 0) != 0;
}

}  // namespace game_api
