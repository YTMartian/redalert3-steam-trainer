// 皮肤离线预览工具。
//
// 独立的 Win32 + D3D9 小窗口，只链接 skin / paint / skin_gallery 与 ImGui，
// 不依赖游戏和 game_api，用来在实机之外快速比对皮肤配色与控件观感。
//
// 快捷键：1~6 切换皮肤，F1 恢复默认，Esc 退出。
// 这个目标不参与 DLL 构建产物，仅供开发期核对视觉。

#include <Windows.h>
#include <d3d9.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "paint.h"
#include "shell.h"
#include "skin.h"
#include "skin_gallery.h"
#include "stats_minimal.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

// ---- 语义对比度自检（--audit）----
// 换皮肤最容易翻车的地方不是"不好看"，而是亮色皮肤上白字叠白底、暗色皮肤上
// 细描边和面底同色。这两类问题在游戏里才看得出来太贵，所以在这里先算一遍。
float Linear(float v) {
  return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float Luminance(ImU32 c) {
  const float r = Linear((float)((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.f);
  const float g = Linear((float)((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.f);
  const float b = Linear((float)((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.f);
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float Contrast(ImU32 fg, ImU32 bg) {
  const float a = Luminance(fg) + 0.05f;
  const float b = Luminance(bg) + 0.05f;
  return a > b ? a / b : b / a;
}

int RunAudit() {
  struct Pair {
    const char* name;
    float need;
  };
  const int n = ui::skin::Count();
  int bad = 0;
  std::printf("%-10s %-14s %6s %6s %s\n", "skin", "pair", "ratio", "need", "verdict");
  for (int i = 0; i < n; ++i) {
    const ui::skin::Def& d = ui::skin::At(i);
    const ui::skin::Skin& s = d.s;
    // 先报几何：形状是离散维度，自检里看一眼就知道哪套是切角。
    std::printf("%-10s shape=%-7s r=%.1f cardR=%.1f chamfer=%.1f %s\n", d.key,
                s.shape == ui::paint::BoxShape::Chamfer ? "chamfer" : "round", s.radius,
                s.cardRadius, s.chamfer, s.dark ? "dark" : "light");
    const ImU32 panel = ui::skin::PanelBg(s);
    const ImU32 field = ui::skin::FieldBg(s);
    const ImU32 edge = ui::skin::EdgeIdle(s);

    const Pair pairs[] = {
        {"text/panel", 4.5f},   {"text/win", 4.5f},     {"text2/panel", 3.0f},
        {"text3/win", 2.0f},    {"heading/panel", 3.0f}, {"money/panel", 3.0f},
        {"accent2/field", 2.5f}, {"good/bad tone", 0.f},
    };
    const ImU32 fgs[] = {s.text, s.text, s.text2, s.text3, s.accent2, s.money, s.accent2,
                         s.text};
    const ImU32 bgs[] = {panel, s.win1, panel, s.win1, panel, panel, field, panel};

    for (int k = 0; k < 7; ++k) {
      const float r = Contrast(fgs[k], bgs[k]);
      const bool ok = r >= pairs[k].need;
      if (!ok) ++bad;
      std::printf("%-10s %-14s %6.2f %6.2f %s\n", d.key, pairs[k].name, r, pairs[k].need,
                  ok ? "ok" : "FAIL");
    }
    // 描边是否和面底分得开（亮度差 + 色相不重合）。
    const float dl = std::fabs(Luminance(edge) - Luminance(panel));
    const bool edge_ok = dl >= 0.004f || Contrast(edge, panel) >= 1.08f;
    if (!edge_ok) ++bad;
    std::printf("%-10s %-14s %6.3f %6.3f %s\n", d.key, "edge/panel", dl, 0.004f,
                edge_ok ? "ok" : "FAIL");
  }
  std::printf("\n%s (%d issue%s)\n", bad == 0 ? "ALL PASS" : "HAS ISSUES", bad,
              bad == 1 ? "" : "s");
  return bad == 0 ? 0 : 2;
}

LPDIRECT3D9 g_d3d = nullptr;
LPDIRECT3DDEVICE9 g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp = {};
HWND g_hwnd = nullptr;
bool g_reset = false;
bool g_running = true;

float g_scale = 1.0f;
float g_opacity = 0.66f;  // 与游戏内默认一致
float g_mock_slider = 0.42f;
int g_mock_seg = 1;
bool g_mock_on[5] = {true, false, true, false, true};
constexpr float kScaleMin = 0.70f;
constexpr float kScaleMax = 3.00f;
bool g_shell_mode = false;  // --shell：只画主窗口外壳，用来核对标题栏与外框
bool g_stats_mode = false;  // --stats：极简战况面板自检（假数据 + 真排版）
// --shot=<bmp>：渲染几帧后直接从后备缓冲导出位图再退出。
// 比 PrintWindow 抓 D3D 窗口可靠（那条路经常给到 DWM 里的旧帧）。
char g_shot_path[MAX_PATH] = {};
int g_frame_no = 0;

// --scrolltest[=report.txt]：滚动自检。
//
// 战况统计窗口用的是"每帧 SetNextWindowPos(Always) + NoMove + 自绘外壳"这套配置。
// 这里原样复刻那套配置，塞进 60 行内容，用 io.AddMouseWheelEvent 灌一个滚轮事件，
// 把 ScrollMax / ScrollY 前后写进报告文件（WIN32 子系统没有控制台，只能落盘）。
bool g_scrolltest = false;
char g_scroll_report[MAX_PATH] = {};
float g_st_max = 0.f;
float g_st_y_before = 0.f;
float g_st_y_after = 0.f;
bool g_st_hover_entered = false;

void DrawScrollTest() {
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoMove;
  ImGui::SetNextWindowPos(ImVec2(60.f, 60.f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(400.f, 300.f), ImGuiCond_FirstUseEver);
  ImGui::Begin("##scrolltest", nullptr, flags);
  {
    const ImVec2 wp = ImGui::GetWindowPos();
    const float h = ui::shell::Draw(ImGui::GetWindowDrawList(), wp,
                                    ImVec2(wp.x + ImGui::GetWindowSize().x,
                                           wp.y + ImGui::GetWindowSize().y),
                                    1.f, 0.9f, u8"滚动自检", u8" · test", true, nullptr, true,
                                    "test");
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, wp.y + h + 6.f));
  }
  g_st_max = ImGui::GetScrollMaxY();
  if (g_frame_no == 5) g_st_y_before = ImGui::GetScrollY();
  for (int i = 0; i < 60; ++i) ImGui::Text("row %d", i);
  if (g_frame_no >= 20 && g_st_y_after == 0.f) g_st_y_after = ImGui::GetScrollY();
  ImGui::End();
}

// --stats：用假数据跑真的极简战况面板（stats_minimal.cpp，和游戏里同一份实现）。
//
// 并排放一个正常宽度和一个窄窗口：列对齐、行高、窄窗口下名字的省略号截断
// 都在一张图里看得见。配 --shot= 导出位图。
void DrawStatsMinimalTest() {
  static const char* kNames[] = {u8"玩家一", u8"LongPlayerName_Alpha", u8"老张",
                                 u8"Player_07_Extra_Long_Name", u8"电脑-AI", u8"观战"};
  // 每行挂几个兵种，名字只用首字做占位图标（预览没有游戏纹理）。
  static const char* kUnits[][7] = {
      {u8"征召兵", u8"防空部队", u8"磁暴步兵", u8"恐怖机器人", u8"天启坦克", u8"基洛夫飞艇", nullptr},
      {u8"步兵", u8"火箭飞行兵", u8"守护者坦克", u8"冷冻直升机", u8"世纪轰炸机", u8"雅典娜炮", u8"幻影坦克"},
      {u8"镰刀", u8"牛蛙", u8"恐龟", u8"飞弹车", u8"电鳐", nullptr, nullptr},
      {nullptr},
      {u8"工程师", u8"多功能步兵车", u8"链锯", nullptr, nullptr, nullptr, nullptr},
      {u8"战熊", u8"磁暴快艇", nullptr, nullptr, nullptr, nullptr, nullptr},
  };
  static const int kUnitCounts[][7] = {
      {12, 6, 4, 2, 1, 1, 0}, {30, 8, 6, 3, 1, 2, 5}, {9, 4, 3, 2, 2, 0, 0},
      {0}, {4, 2, 1, 0, 0, 0, 0}, {3, 1, 0, 0, 0, 0, 0}};

  static stats_minimal::UnitIcon unit_pool[40];
  static stats_minimal::Row rows[6];
  static bool init = false;
  if (!init) {
    init = true;
    int up = 0;
    for (int i = 0; i < 6; ++i) {
      rows[i].name = kNames[i];
      const int first = up;
      for (int k = 0; k < 7 && kUnits[i][k]; ++k) {
        if (kUnitCounts[i][k] <= 0) continue;
        unit_pool[up].key = kUnits[i][k];
        unit_pool[up].name = kUnits[i][k];
        unit_pool[up].count = kUnitCounts[i][k];
        ++up;
      }
      rows[i].unit_count = up - first;
      rows[i].units = rows[i].unit_count > 0 ? &unit_pool[first] : nullptr;
    }
    rows[0].is_local = true;
    rows[0].has_color = true;
    rows[0].color_r = 232;
    rows[0].color_g = 82;
    rows[0].color_b = 68;
    rows[0].has_money = true;
    rows[0].money = 24580;
    rows[0].has_power = true;
    rows[0].power_used = 120;
    rows[0].power_total = 200;
    rows[0].unit_total = 47;
    rows[0].building_total = 12;
    rows[1].has_color = true;
    rows[1].color_r = 72;
    rows[1].color_g = 132;
    rows[1].color_b = 235;
    rows[1].has_money = true;
    rows[1].money = 1284000;
    rows[1].has_power = true;
    rows[1].power_used = 195;
    rows[1].power_total = 200;
    rows[1].unit_total = 132;
    rows[1].building_total = 35;
    rows[2].has_color = true;
    rows[2].color_r = 226;
    rows[2].color_g = 196;
    rows[2].color_b = 62;
    rows[2].has_money = true;
    rows[2].money = 860;
    rows[2].has_power = true;
    rows[2].power_used = 60;
    rows[2].power_total = 50;  // 赤字：总电力 < 使用电力，应当显示红色
    rows[2].unit_total = 8;
    rows[2].building_total = 3;
    rows[3].defeated = true;
    rows[3].has_money = true;
    rows[3].money = 0;
    rows[3].unit_total = 0;
    rows[3].building_total = 0;
    rows[4].has_money = false;
    rows[4].has_power = true;
    rows[4].power_used = 80;
    rows[4].power_total = 100;  // 80%：黄色预警档
    rows[4].unit_total = 21;
    rows[4].building_total = 9;
    rows[4].has_color = true;
    rows[4].color_r = 140;
    rows[4].color_g = 140;
    rows[4].color_b = 148;
    rows[5].has_money = true;
    rows[5].money = 3420;
    rows[5].has_power = true;
    rows[5].power_used = 30;
    rows[5].power_total = 60;
    rows[5].unit_total = 4;
    rows[5].building_total = 1;
    rows[5].has_color = true;
    rows[5].color_r = 120;
    rows[5].color_g = 200;
    rows[5].color_b = 120;
  }

  const float widths[2] = {580.f, 380.f};
  float x = 30.f;
  for (int w = 0; w < 2; ++w) {
    char id[32];
    std::snprintf(id, sizeof(id), "##stats_test%d", w);
    ImGui::SetNextWindowPos(ImVec2(x, 30.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(widths[w], 260.f), ImGuiCond_Always);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 0.f));
    // 第二栏用 1.0 倍做对照，第一栏用 1.5 倍（默认设置）。
    stats_minimal::SetChipScale(w == 0 ? 1.5f : 1.0f);
    stats_minimal::Cols cols;
    stats_minimal::Measure(&cols, rows, 6, ImGui::GetContentRegionAvail().x, 1.f);
    stats_minimal::DrawHeader(cols, 1.f);
    for (int i = 0; i < 6; ++i) {
      stats_minimal::DrawRow(rows[i], i, cols, 1.f, nullptr, nullptr);
    }
    ImGui::PopStyleVar();
    ImGui::End();
    x += widths[w] + 30.f;
  }
}

// 把后备缓冲存成 32 位 BMP。不依赖 GDI+ / D3DX，只走 GetRenderTargetData。
bool SaveBackBufferBmp(const char* path) {
  if (!g_dev || !path || !path[0]) return false;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
  D3DSURFACE_DESC desc = {};
  bb->GetDesc(&desc);

  IDirect3DSurface9* sys = nullptr;
  HRESULT hr = g_dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                  D3DPOOL_SYSTEMMEM, &sys, nullptr);
  if (SUCCEEDED(hr)) hr = g_dev->GetRenderTargetData(bb, sys);
  D3DLOCKED_RECT lr = {};
  if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);

  bool ok = false;
  if (SUCCEEDED(hr)) {
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (f) {
      const int row_bytes = static_cast<int>(desc.Width) * 4;
      const int img_bytes = row_bytes * static_cast<int>(desc.Height);
      BITMAPFILEHEADER fh = {};
      fh.bfType = 0x4D42;  // 'BM'
      fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
      fh.bfSize = fh.bfOffBits + img_bytes;
      BITMAPINFOHEADER ih = {};
      ih.biSize = sizeof(ih);
      ih.biWidth = static_cast<LONG>(desc.Width);
      ih.biHeight = -static_cast<LONG>(desc.Height);  // 负数 = 自上而下
      ih.biPlanes = 1;
      ih.biBitCount = 32;
      ih.biCompression = BI_RGB;
      ih.biSizeImage = img_bytes;
      fwrite(&fh, sizeof(fh), 1, f);
      fwrite(&ih, sizeof(ih), 1, f);
      for (UINT y = 0; y < desc.Height; ++y) {
        fwrite(static_cast<const unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
               row_bytes, 1, f);
      }
      fclose(f);
      ok = true;
    }
    sys->UnlockRect();
  }
  if (sys) sys->Release();
  bb->Release();
  return ok;
}

void ApplyStyle() {
  ImGuiStyle& s = ImGui::GetStyle();
  ui::skin::BuildStyle(s, ui::skin::Active(), g_opacity);
  if (g_scale > 0.01f && g_scale != 1.0f) s.ScaleAllSizes(g_scale);
  // 和游戏内一致：字体倍数走 FontScaleMain（1.92+ 按最终尺寸动态栅格化），
  // 不用 io.FontGlobalScale，否则放大就是位图拉伸。
  s.FontScaleMain = (g_scale > 0.01f) ? g_scale : 1.f;
  ImGui::GetIO().FontGlobalScale = 1.f;
}

bool InitDevice(HWND hwnd) {
  if (!g_d3d) g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!g_d3d) return false;

  D3DPRESENT_PARAMETERS pp = {};
  RECT rc = {};
  GetClientRect(hwnd, &rc);
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.EnableAutoDepthStencil = FALSE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  pp.BackBufferWidth = static_cast<UINT>(rc.right - rc.left);
  pp.BackBufferHeight = static_cast<UINT>(rc.bottom - rc.top);
  pp.hDeviceWindow = hwnd;

  if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev))) {
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev))) {
      return false;
    }
  }
  g_pp = pp;
  return true;
}

void ResetDevice() {
  if (!g_dev) return;
  ImGui_ImplDX9_InvalidateDeviceObjects();
  RECT rc = {};
  GetClientRect(g_hwnd, &rc);
  g_pp.BackBufferWidth = static_cast<UINT>(rc.right - rc.left);
  g_pp.BackBufferHeight = static_cast<UINT>(rc.bottom - rc.top);
  if (FAILED(g_dev->Reset(&g_pp))) {
    g_running = false;
    return;
  }
  ImGui_ImplDX9_CreateDeviceObjects();
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
  switch (msg) {
    case WM_SIZE:
      if (g_dev && wParam != SIZE_MINIMIZED) g_reset = true;
      return 0;
    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

void LoadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.PixelSnapH = true;
  const char* candidates[] = {
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\msyh.ttf",
      "C:\\Windows\\Fonts\\simhei.ttf",
  };
  for (const char* path : candidates) {
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
      io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg,
                                   io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
      return;
    }
  }
  io.Fonts->AddFontDefault();
}

// ---------------------------------------------------------------------------
// 模拟控件：用于在 P2 之前先定下观感。全部只吃 skin 槽位。
// ---------------------------------------------------------------------------

// 用 ImGuiStorage 存每控件的动画进度，避免引入状态池。
float Anim(ImGuiID id, bool target, float dt, float speed = 14.f) {
  ImGuiStorage* st = ImGui::GetStateStorage();
  float v = st->GetFloat(id, target ? 1.f : 0.f);
  const float t = target ? 1.f : 0.f;
  const float k = 1.f - std::exp(-speed * dt);
  v += (t - v) * k;
  if (std::fabs(t - v) < 0.002f) v = t;
  st->SetFloat(id, v);
  return v;
}

bool MockTile(const ui::skin::Skin& sk, const char* id, const char* title, const char* key,
              bool* active, bool enabled, float w, float h) {
  ImGui::PushID(id);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##tile", ImVec2(w, h));
  const bool hov = enabled && ImGui::IsItemHovered();
  const bool held = enabled && ImGui::IsItemActive();
  if (pressed && enabled) *active = !*active;
  const bool on = *active;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const ImVec2 p1(p0.x + w, p0.y + h);
  const float r = sk.cardRadius * 0.62f;
  const float a = Anim(ImGui::GetItemID(), on, ImGui::GetIO().DeltaTime);

  using namespace ui;
  // 和 ui.cpp 的 DrawPlate 同一条路径：形状（圆角/切角）来自皮肤。
  paint::Plate pl;
  pl.top = skin::Mix(sk.win1, skin::Mix(sk.card0, sk.accent, 0.34f * a), 0.55f);
  pl.bot = skin::Mix(skin::Fade(sk.win1, 0.80f), skin::Mix(sk.cardInnerLo, sk.accent, 0.22f * a),
                     0.70f);
  pl.edge = enabled ? skin::Mix(sk.cardStroke, sk.accent, 0.35f + 0.65f * a) : sk.cardStroke;
  pl.inner = skin::Fade(sk.cardInnerHi, a > 0.5f ? 0.30f : 0.14f);
  pl.shadow = skin::Fade(sk.shadow, enabled ? 1.f : 0.5f);
  pl.dy = 3.f;
  pl.sigma = 7.f * (hov ? 1.3f : 1.f);
  pl.sk = sk.shadowK;
  pl.th = a > 0.5f ? 1.5f : sk.strokeW;
  paint::Panel(dl, p0, p1, r, sk.shape, sk.chamfer, pl);
  if (hov || held) {
    dl->AddRectFilled(p0, p1, skin::Fade(sk.cardInnerHi, held ? 0.14f : 0.07f), r);
  }
  // 左侧状态条
  const ImU32 bar = enabled ? skin::Mix(sk.text3, sk.accent2, a) : skin::Fade(sk.text3, 0.5f);
  dl->AddRectFilled(ImVec2(p0.x + 3.f, p0.y + 9.f), ImVec2(p0.x + 6.f, p1.y - 9.f), bar, 2.f);

  const float pad = 12.f;
  paint::TextClip(dl, font, 15.f, ImVec2(p0.x + pad, p0.y + h * 0.5f - 8.f), w - pad * 2.f,
                  enabled ? sk.text : skin::Fade(sk.text3, 0.9f), title);
  if (key && key[0]) {
    const ImVec2 ks = paint::TextSize(font, 12.f, key);
    const ImVec2 k0(p1.x - ks.x - pad - 6.f, p1.y - ks.y - 7.f);
    dl->AddRectFilled(k0, ImVec2(p1.x - pad + 2.f, p1.y - 4.f), skin::Fade(sk.win1, 0.72f),
                      sk.cardRadius * 0.32f);
    paint::Text(dl, font, 12.f, ImVec2(k0.x + 4.f, k0.y + 1.f),
                enabled ? skin::Fade(sk.accent2, 0.92f) : sk.text3, key);
  }
  if (a > 0.02f) {
    paint::Glow(dl, ImVec2(p1.x - 11.f, p0.y + 11.f), 4.5f, skin::Fade(sk.accentGlow, a), 3);
    dl->AddCircleFilled(ImVec2(p1.x - 11.f, p0.y + 11.f), 2.4f, skin::Fade(sk.accent2, a), 8);
  }
  ImGui::PopID();
  return pressed;
}

bool MockSwitch(const ui::skin::Skin& sk, const char* id, const char* label, bool* v, float w) {
  using namespace ui;
  ImGui::PushID(id);
  const float h = ImGui::GetFrameHeight() + 4.f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##sw", ImVec2(w, h));
  if (pressed) *v = !*v;
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const ImVec2 p1(p0.x + w, p0.y + h);

  dl->AddRectFilled(p0, p1, hov ? skin::Fade(sk.cardInnerHi, 0.06f) : IM_COL32(0, 0, 0, 0),
                    sk.cardRadius * 0.45f);
  paint::TextClip(dl, font, 15.f, ImVec2(p0.x + 6.f, p0.y + h * 0.5f), w - 72.f, sk.text, label);

  const float t = Anim(ImGui::GetItemID(), *v, ImGui::GetIO().DeltaTime, 16.f);
  const float tw = 44.f, th = 24.f;
  const ImVec2 t0(p1.x - tw - 6.f, p0.y + (h - th) * 0.5f);
  const ImVec2 t1(t0.x + tw, t0.y + th);
  const ImU32 off0 = skin::WithA(sk.trackOff0, 255), off1 = skin::WithA(sk.trackOff1, 255);
  const ImU32 on0 = skin::WithA(sk.trackOn0, 255), on1 = skin::WithA(sk.trackOn1, 255);
  paint::Plate tp;
  tp.top = skin::Mix(off0, on0, t);
  tp.bot = skin::Mix(off1, on1, t);
  tp.edge = skin::Mix(sk.cardStroke, skin::Fade(sk.accent2, 0.9f), t);
  tp.inner = skin::Fade(sk.cardInnerHi, 0.14f);
  tp.th = 1.f;
  // 胶囊圆角；切角皮肤下自动变成八角轨道。
  paint::Panel(dl, t0, t1, th * 0.5f, sk.shape, sk.chamfer * 0.8f, tp);
  const float kr = th * 0.5f - 3.f;
  const ImVec2 kc(t0.x + th * 0.5f + (tw - th) * t, t0.y + th * 0.5f);
  paint::Knob(dl, kc, kr, sk.knob0, sk.knob1, sk.shadow, 1.5f, 3.f, 0.8f);
  ImGui::PopID();
  return pressed;
}

void SectionLabel(const ui::skin::Skin& sk, const char* caps, const char* cn) {
  using namespace ui;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  ImGui::Dummy(ImVec2(0.f, 22.f));
  const ImVec2 p = ImGui::GetItemRectMin();
  paint::TextTracked(dl, font, 11.f, ImVec2(p.x, p.y + 3.f), sk.text3, caps, 1.1f);
  paint::Text(dl, font, 15.f, ImVec2(p.x, p.y + 17.f), sk.accent2, cn);
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 40.f));
  const float x1 = ImGui::GetWindowContentRegionMax().x;
  dl->AddRectFilled(ImVec2(p.x, p.y + 38.f), ImVec2(x1, p.y + 39.f), skin::Fade(sk.divider, 0.9f));
}

void MockPlayerCard(const ui::skin::Skin& sk) {
  using namespace ui;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = 92.f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, h));
  const ImVec2 p1(p0.x + w, p0.y + h);
  const float r = sk.cardRadius;
  const ImU32 ac = sk.money;

  paint::Plate pl;
  pl.top = skin::Fade(sk.win1, 0.94f);
  pl.bot = skin::Fade(sk.win1, 0.78f);
  pl.edge = sk.cardStroke;
  pl.inner = skin::Fade(sk.cardInnerHi, 0.40f);
  pl.shadow = sk.shadow;
  pl.dy = 3.f;
  pl.sigma = 8.f;
  pl.sk = sk.shadowK;
  pl.th = sk.strokeW;
  paint::Panel(dl, p0, p1, r, sk.shape, sk.chamfer, pl);
  dl->AddRectFilled(ImVec2(p0.x + 8.f, p0.y + 10.f), ImVec2(p0.x + 11.f, p1.y - 10.f),
                    skin::Fade(ac, 0.85f), 2.f);

  paint::Text(dl, font, 15.f, ImVec2(p0.x + 20.f, p0.y + 12.f), sk.text, u8"玩家 1（己方）");
  paint::Text(dl, font, 11.f, ImVec2(p0.x + 20.f, p0.y + 30.f), sk.text3, u8"资金 / 电力");

  dl->AddText(font, 24.f, ImVec2(p0.x + 20.f, p0.y + 46.f), ac, u8"125,000");
  dl->AddText(font, 15.f, ImVec2(p0.x + 150.f, p0.y + 52.f), sk.text2, u8"电力 780 / 1200");
  const ImVec2 b0(p0.x + 20.f, p1.y - 16.f), b1(p1.x - 20.f, p1.y - 10.f);
  dl->AddRectFilled(b0, b1, skin::Fade(sk.trackOff0, 0.9f), 3.f);
  dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * 0.65f, b1.y), sk.good, 3.f);
}

// 外壳预览（--shell）：一个和主菜单同尺寸的"无标题栏 + 无底色"窗口，
// 外壳交给 ui::shell::Draw（与游戏里同一份代码），正文只放骨架，
// 用来核对标题栏高度、主题胶囊、关闭按钮、正文起始位置是否正确。
void DrawShellUi() {
  using namespace ui;
  const float s = g_scale;
  const ImVec2 size(900.f * s, 720.f * s);
  ImGui::SetNextWindowPos(ImVec2(6.f, 6.f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("##shell_preview", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoScrollbar);
  const ImVec2 p0 = ImGui::GetWindowPos();
  shell::Hit hit;
  const float th = shell::Draw(ImGui::GetWindowDrawList(), p0,
                               ImVec2(p0.x + size.x, p0.y + size.y), s, g_opacity, u8"红色警戒3",
                               u8" · 进程内菜单", false, &hit);

  // 正文骨架：和真实菜单同结构（一行状态 + 信息框 + 侧栏 + 主体），
  // 只看"内容有没有被标题栏压住"。
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 12.f * s, p0.y + th + 6.f * s));
  const skin::Skin& sk = skin::Active();
  ImGui::TextColored(skin::Vec4(sk.accent2), u8"RA3 Overlay");
  ImGui::SameLine();
  ImGui::TextDisabled(u8"|  %s  |  选中 %d", u8"对局中", 0);
  ImGui::SameLine();
  ImGui::Button(u8"注入", ImVec2(100.f * s, 0.f));
  ImGui::SameLine();
  ImGui::Button(u8"卸载", ImVec2(60.f * s, 0.f));
  ImGui::SameLine();
  ImGui::Checkbox(u8"观战模式", &g_mock_on[0]);
  ImGui::SameLine();
  ImGui::Button(u8"设置", ImVec2(60.f * s, 0.f));

  ImGui::Spacing();
  ImGui::BeginChild("##msg", ImVec2(0.f, 44.f * s), true);
  ImGui::TextUnformatted(u8"外壳预览：标题栏、主题胶囊、关闭按钮、正文起点");
  ImGui::EndChild();

  ImGui::Spacing();
  ImGui::BeginChild("##side", ImVec2(156.f * s, -1.f), true);
  const char* nav[] = {u8"单位操作", u8"资源", u8"超武 / 地图", u8"摄像机", u8"界面设置"};
  for (int i = 0; i < 5; ++i) {
    ImGui::Selectable(nav[i], g_mock_seg == i);
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##body", ImVec2(0.f, -1.f), true);
  ImGui::TextDisabled(u8"正文区域");
  SectionLabel(sk, "HUD TILES", u8"功能块");
  {
    const float gap = 8.f * s;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float tw = (avail - gap * 2.f) / 3.f;
    const char* names[] = {u8"无限资金", u8"电力全满", u8"快速建造"};
    const char* keys[] = {"Ctrl+F1", "Ctrl+F2", "Ctrl+F5"};
    for (int i = 0; i < 3; ++i) {
      MockTile(sk, names[i], names[i], keys[i], &g_mock_on[i], true, tw, 62.f * s);
      if (i != 2) ImGui::SameLine(0.f, gap);
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

void DrawUi() {
  using namespace ui;
  const skin::Skin& sk = skin::Active();
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 full(io.DisplaySize.x, io.DisplaySize.y);
  ImGui::SetNextWindowPos(ImVec2(6.f, 6.f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(full.x - 12.f, full.y - 12.f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(g_opacity);
  if (ImGui::Begin(u8"RA3 Overlay · 皮肤预览", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextColored(ui::skin::Vec4(sk.accent2), u8"RA3 SKIN PREVIEW");
    ImGui::SameLine();
    ImGui::TextDisabled(u8"当前：%s / %s  ·  1~6 切换 · F1 默认 · Esc 退出",
                        ui::skin::At(ui::skin::Current()).name_cn,
                        ui::skin::KeyAt(ui::skin::Current()));

    ImGui::BeginChild("##side", ImVec2(150.f * g_scale, -1.f), true);
    const char* nav[] = {u8"单位操作", u8"资源", u8"超武 / 地图", u8"摄像机", u8"界面设置"};
    for (int i = 0; i < 5; ++i) {
      const bool sel = (g_mock_seg == i);
      ImGui::PushID(i);
      const float w = ImGui::GetContentRegionAvail().x;
      const float h = ImGui::GetFrameHeight() + 2.f;
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##nav", ImVec2(w, h))) g_mock_seg = i;
      const bool hov = ImGui::IsItemHovered();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImFont* font = ImGui::GetFont();
      const ImVec2 p1(p0.x + w, p0.y + h);
      const float r = sk.cardRadius * 0.55f;
      if (sel || hov) {
        dl->AddRectFilled(p0, p1, sel ? skin::Fade(sk.accent, 0.26f) : skin::Fade(sk.cardInnerHi, 0.07f), r);
        paint::Stroke(dl, p0, p1, r, sel ? skin::Fade(sk.accent, 0.85f) : skin::Fade(sk.divider, 0.9f), 1.f);
      }
      if (sel) {
        dl->AddRectFilled(ImVec2(p0.x + 1.f, p0.y + 5.f), ImVec2(p0.x + 4.f, p1.y - 5.f),
                          sk.accent2, 2.f);
      }
      paint::TextClip(dl, font, 15.f, ImVec2(p0.x + 12.f, p0.y + h * 0.5f), w - 18.f,
                      sel ? sk.text : sk.text2, nav[i]);
      ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##body", ImVec2(0, -1.f), true);

    SectionLabel(sk, "THEME LIBRARY", u8"皮肤");
    const int pick = skin::Gallery(sk, g_scale);
    if (pick >= 0 && pick != skin::Current()) {
      skin::Select(pick);
      if (g_opacity < skin::MinOpacity(pick)) g_opacity = skin::MinOpacity(pick);
      ApplyStyle();
    }

    SectionLabel(sk, "HUD TILES", u8"功能块");
    {
      const float gap = 8.f;
      const float avail = ImGui::GetContentRegionAvail().x;
      const float tw = (avail - gap * 2.f) / 3.f;
      const float th = 62.f * g_scale;
      const char* names[] = {u8"无限资金", u8"电力全满", u8"快速建造", u8"地图全开",
                             u8"超级武器", u8"单位无敌"};
      const char* keys[] = {"Ctrl+F1", "Ctrl+F2", "Ctrl+F5", "Ctrl+F9", "Ctrl+F6", "P"};
      static bool tile_on[6] = {false, true, false, false, true, false};
      for (int i = 0; i < 6; ++i) {
        MockTile(sk, names[i], names[i], keys[i], &tile_on[i], i != 5, tw, th);
        if (i % 3 != 2) ImGui::SameLine(0.f, gap);
      }
    }

    SectionLabel(sk, "CONTROLS", u8"控件");
    MockSwitch(sk, "sw0", u8"观战模式", &g_mock_on[0], ImGui::GetContentRegionAvail().x);
    MockSwitch(sk, "sw1", u8"隐藏血条", &g_mock_on[1], ImGui::GetContentRegionAvail().x);
    MockSwitch(sk, "sw2", u8"建造限制", &g_mock_on[2], ImGui::GetContentRegionAvail().x);

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##mock_slider", &g_mock_slider, 0.f, 1.f, u8"危险等级 %.0f%%");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##sc", &g_scale, kScaleMin, kScaleMax, u8"界面缩放 %.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) ApplyStyle();
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::SliderFloat("##op", &g_opacity, skin::MinOpacity(skin::Current()), 1.00f,
                           u8"透明度 %.0f%%")) {
      ApplyStyle();
    }

    SectionLabel(sk, "MATCH", u8"战况");
    MockPlayerCard(sk);

    ImGui::EndChild();
    ImGui::End();
  }
  (void)sk;
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
  // --audit：只跑配色自检并打印，不开窗口。给脚本和 CI 用。
  {
    const wchar_t* cmd = GetCommandLineW();
    if (wcsstr(cmd, L"--audit")) return RunAudit();
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"RA3SkinPreview";
  RegisterClassExW(&wc);

  g_hwnd = CreateWindowW(wc.lpszClassName, L"RA3 Overlay - Skin Preview", WS_OVERLAPPEDWINDOW,
                         100, 100, 1180, 800, nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) return 1;

  if (!InitDevice(g_hwnd)) {
    MessageBoxW(g_hwnd, L"D3D9 设备创建失败", L"Skin Preview", MB_ICONERROR);
    return 1;
  }

  ShowWindow(g_hwnd, SW_SHOWDEFAULT);
  UpdateWindow(g_hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  LoadFonts();
  // 命令行参数：--theme=N 定住皮肤，--scale=N 定住界面缩放，--shell 切外壳预览，
  // --shot=xxx.bmp 导出画面。用 CommandLineToArgvW 逐个解析，别用 wcsstr——
  // 那样 --shot= 会把后面跟的 --scale= 也当成文件名的一部分。
  int start_theme = 0;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
      const wchar_t* a = argv[i];
      if (wcsncmp(a, L"--theme=", 8) == 0) {
        start_theme = _wtoi(a + 8);
      } else if (wcsncmp(a, L"--scale=", 8) == 0) {
        const float v = (float)_wtof(a + 8);
        if (v > 0.1f) g_scale = (v < kScaleMin) ? kScaleMin : ((v > kScaleMax) ? kScaleMax : v);
      } else if (wcsncmp(a, L"--shot=", 7) == 0) {
        WideCharToMultiByte(CP_UTF8, 0, a + 7, -1, g_shot_path, MAX_PATH, nullptr, nullptr);
      } else if (wcsncmp(a, L"--scrolltest=", 13) == 0) {
        g_scrolltest = true;
        WideCharToMultiByte(CP_UTF8, 0, a + 13, -1, g_scroll_report, MAX_PATH, nullptr, nullptr);
      } else if (wcscmp(a, L"--scrolltest") == 0) {
        g_scrolltest = true;
      } else if (wcscmp(a, L"--shell") == 0) {
        g_shell_mode = true;
      } else if (wcscmp(a, L"--stats") == 0) {
        g_stats_mode = true;
      }
    }
    if (argv) LocalFree(argv);
  }
  ui::skin::SetCurrent(start_theme);
  ApplyStyle();  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX9_Init(g_dev);

  while (g_running) {
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      if (msg.message == WM_QUIT) g_running = false;
    }
    if (!g_running) break;

    if (g_reset) {
      ResetDevice();
      g_reset = false;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // 滚动自检：先把鼠标放到窗口内部（否则滚轮找不到 hover 窗口），
    // 再灌一个滚轮事件。事件在 NewFrame 里按顺序处理，所以必须在后端 NewFrame
    // 之后、ImGui::NewFrame 之前加，最后一条才生效。
    if (g_scrolltest) {
      if (g_frame_no >= 2 && !g_st_hover_entered) {
        io.AddMousePosEvent(200.f, 200.f);
        g_st_hover_entered = true;
      }
      if (g_frame_no == 8) io.AddMouseWheelEvent(0.f, -5.f);
    }
    ImGui::NewFrame();

    const float dt = io.DeltaTime;
    ui::skin::Update(dt);
    if (ui::skin::Transitioning()) ApplyStyle();

    if (ImGui::IsKeyPressed(ImGuiKey_1)) ui::skin::Select(0);
    if (ImGui::IsKeyPressed(ImGuiKey_2)) ui::skin::Select(1);
    if (ImGui::IsKeyPressed(ImGuiKey_3)) ui::skin::Select(2);
    if (ImGui::IsKeyPressed(ImGuiKey_4)) ui::skin::Select(3);
    if (ImGui::IsKeyPressed(ImGuiKey_5)) ui::skin::Select(4);
    if (ImGui::IsKeyPressed(ImGuiKey_6)) ui::skin::Select(5);
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
      g_scale = 1.0f;
      g_opacity = 0.66f;
      ui::skin::Select(0);
      ApplyStyle();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_running = false;

    if (g_scrolltest) {
      DrawScrollTest();
    } else if (g_stats_mode) {
      DrawStatsMinimalTest();
    } else if (g_shell_mode) {
      DrawShellUi();
    } else {
      DrawUi();
    }

    ImGui::Render();
    if (g_dev->BeginScene() >= 0) {
      // 用当前皮肤的压暗层做背景，模拟游戏画面的明暗。
      const ui::skin::Skin& sk = ui::skin::Active();
      const ImU32 bg = ui::skin::Mix(sk.win1, sk.scrim, 0.72f);
      const D3DCOLOR clear = D3DCOLOR_RGBA(int((bg >> IM_COL32_R_SHIFT) & 0xFF),
                                           int((bg >> IM_COL32_G_SHIFT) & 0xFF),
                                           int((bg >> IM_COL32_B_SHIFT) & 0xFF), 255);
      g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0);
      ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
      g_dev->EndScene();
      ++g_frame_no;
      // --shot：第三帧稳定后从后备缓冲直接导出，然后退出。
      if (g_shot_path[0] && g_frame_no >= 3) {
        SaveBackBufferBmp(g_shot_path);
        g_running = false;
      }
      // --scrolltest：第 25 帧写报告（滚轮在第 8 帧灌入，之后有十几帧反应时间）。
      if (g_scrolltest && g_frame_no >= 25) {
        if (g_scroll_report[0]) {
          FILE* f = nullptr;
          fopen_s(&f, g_scroll_report, "w");
          if (f) {
            std::fprintf(f, "scroll_max=%.1f\nscroll_before=%.1f\nscroll_after=%.1f\nhover=%d\n",
                         g_st_max, g_st_y_before, g_st_y_after, g_st_hover_entered ? 1 : 0);
            std::fclose(f);
          }
        }
        g_running = false;
      }
    }
    if (g_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST) {
      g_reset = true;
    }
  }

  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  if (g_dev) g_dev->Release();
  if (g_d3d) g_d3d->Release();
  DestroyWindow(g_hwnd);
  UnregisterClassW(wc.lpszClassName, hInst);
  return 0;
}
