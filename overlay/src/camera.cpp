// ---------------------------------------------------------------------------
// 摄像机模块（对应 CameraBridge 视频拍摄插件的「摄像机操作」）
//
// 反汇编结论见 game_api.h 顶部的注释。要点：
//   * 相机状态的真身是 View+0x08 的 3x4 矩阵，+0x38/+0x3C/+0x40/+0x44 只是缓存；
//   * 0x7EC240(this=View, const float* m) 是引擎唯一的「设置相机矩阵」入口，
//     相机每次变化都会经过它，所以在这里接管最稳；
//   * 0x7EC240 的 this 也就是我们需要的 View 指针来源，另外还能从 0x00CEAAC0 的
//     登记表兜底扫描，保证 hook 还没被引擎调用过时也能读到相机。
// ---------------------------------------------------------------------------
#include "game_api.h"
#include "game_api_internal.h"
#include "MinHook.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace game_api {
namespace {

constexpr uint32_t kModBase = 0x400000;
constexpr uint32_t kImageMax = 0x02000000;  // 模块镜像最大范围（用于「指针是否指向本模块」）
constexpr uint32_t kFnSetCamera = 0x007EC240;
// 第二个「设置相机矩阵」入口。0x7EC240 经实测从不被调用（hits=0），
// View 类其实有两套 setTransform（0x7EC240 / 0x821760），签名都是
// __thiscall(const float* m)，ecx = View，ret 4。两个都挂，谁跑用谁。
constexpr uint32_t kFnSetCamera2 = 0x00821760;
// View 初始化（W3DView::Init）。CreateView(0x51A210) 建完对象立刻用它初始化，
// ecx = View（此时 vtable 已写好），无栈参数。每个 View 只调一次。
constexpr uint32_t kFnViewInit = 0x007DC060;
constexpr uint32_t kOffMatrix = 0x08;
constexpr uint32_t kOffPosX = 0x38;
constexpr uint32_t kOffPosY = 0x3C;
constexpr uint32_t kOffPosZ = 0x40;
constexpr uint32_t kOffAngle = 0x44;
constexpr uint32_t kOffDist = 0x260;
constexpr uint32_t kViewListBase = 0x00CEAAC0;   // View* 登记表（槽位 = 0x00CEAAC0 + idx*16）
constexpr uint32_t kViewListIndexVa = 0x00CEAA00;  // 当前使用的槽位索引
constexpr uint32_t kVtableMain = 0x00C54BB0;       // View 主 vtable（构造 0x7FFFE0 写 [this]）
// 派生链上的其它 vtable（构造里 [esi]/[edi] 被写成的值），扫描时一并接受：
// 0xC54D30 / 0xC54F60 / 0xC552A0 / 0xC55988 / 0xC55A60 / 0xC55AA0
constexpr uint32_t kVtableChain[] = {0x00C54BB0, 0x00C54D30, 0x00C54F60, 0x00C552A0,
                                     0x00C55988, 0x00C55A60, 0x00C55AA0};

constexpr float kPi = 3.14159265358979323846f;
constexpr int kMaxConfigs = 32;
constexpr int kMaxKeys = 256;

typedef int(__fastcall* SetCameraFn_t)(void*, void*, float*);

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
bool is_ptr(uint32_t v) {
  return v >= 0x10000u && v < 0x7FFF0000u && (v & 3u) == 0;
}

bool finite_f(float v) { return v == v && v > -3.4e38f && v < 3.4e38f; }

// 12 个矩阵分量是否都是有限值。写回游戏前必须过这一关：
// 一旦把 NaN/Inf 写进相机对象，游戏后续把它当指针/索引用就会崩溃。
bool matrix_finite(const float* m) {
  for (int i = 0; i < 12; ++i) {
    if (!finite_f(m[i])) return false;
  }
  return true;
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

uint32_t read_u32(uint32_t addr) { return *reinterpret_cast<uint32_t*>(addr); }
float read_f32(uint32_t addr) { return *reinterpret_cast<float*>(addr); }
void write_f32(uint32_t addr, float v) { *reinterpret_cast<float*>(addr) = v; }

uint32_t va_of(uint32_t va) {
  if (!module_base()) return 0;
  return module_base() + (va - kModBase);
}

// ---------------------------------------------------------------------------
// 锁
// ---------------------------------------------------------------------------
CRITICAL_SECTION g_cs;
bool g_cs_ready = false;

struct Lock {
  Lock() {
    if (g_cs_ready) EnterCriticalSection(&g_cs);
  }
  ~Lock() {
    if (g_cs_ready) LeaveCriticalSection(&g_cs);
  }
};

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
uint32_t g_view = 0;  // 最近一次确认可用的 View
bool g_takeover = false;
bool g_live_valid = false;
CameraState g_live;  // 接管目标

SetCameraFn_t g_orig_set_camera = nullptr;
int g_hook_state = 0;  // 0 未装 / 1 成功 / -1 失败
DWORD g_hook_next_try = 0;

SetCameraFn_t g_orig_set_camera2 = nullptr;
int g_hook2_state = 0;
DWORD g_hook2_next_try = 0;
volatile long g_hook2_hits = 0;

// 探针 C：View 初始化。记录创建过的所有 View，供发现真正的战术视图。
typedef int(__fastcall* ViewInitFn_t)(void*, void*);
ViewInitFn_t g_orig_view_init = nullptr;
int g_hook3_state = 0;
volatile long g_hook3_hits = 0;
uint32_t g_seen_views[16] = {};
int g_seen_count = 0;
uint32_t g_seen_vt[16] = {};

// ---------------------------------------------------------------------------
// 矩阵 <-> 状态
// ---------------------------------------------------------------------------
void build_matrix(float* m, const CameraState& st) {
  const float cy = std::cos(st.yaw), sy = std::sin(st.yaw);
  const float cp = std::cos(st.pitch), sp = std::sin(st.pitch);
  const float cr = std::cos(st.roll), sr = std::sin(st.roll);
  // R = Rz(yaw) * Rx(pitch) * Ry(roll)
  m[0] = cy * cr - sy * sp * sr;
  m[1] = -sy * cp;
  m[2] = cy * sr + sy * sp * cr;
  m[3] = st.x;
  m[4] = sy * cr + cy * sp * sr;
  m[5] = cy * cp;
  m[6] = sy * sr - cy * sp * cr;
  m[7] = st.y;
  m[8] = -cp * sr;
  m[9] = sp;
  m[10] = cp * cr;
  m[11] = st.z;
}

bool matrix_to_state(const float* m, CameraState* out) {
  if (!m || !out) return false;
  for (int i = 0; i < 12; ++i) {
    if (!finite_f(m[i])) return false;
  }
  if (std::fabs(m[3]) > 1.0e7f || std::fabs(m[7]) > 1.0e7f || std::fabs(m[11]) > 1.0e7f) {
    return false;
  }
  out->x = m[3];
  out->y = m[7];
  out->z = m[11];
  out->yaw = std::atan2(m[4], m[0]);
  out->pitch = std::asin(clampf(m[9], -1.f, 1.f));
  out->roll = std::atan2(-m[8], m[10]);
  return true;
}

// 引擎自己算的偏航角只认矩阵，这里照抄 0x7EC240 的算法。
float game_angle_of(const float* m) { return std::atan2(m[4], m[0]); }

// vtable 是否属于 View 类链。
// 注意：曾经这里是「只要 vtable 落在模块镜像内就算」，结果把另一个相机类
// （vtable 0xBF4EA8，位于 .rdata）误当成 View —— 接管时往它的 +0x260 写了
// 一个 float，冲掉了该类的指针字段，游戏随后读到野指针 0xFFEC9953 崩溃。
// 因此现在必须严格匹配 View 的 vtable，绝不「猜」。
bool is_view_vtable(uint32_t vt) {
  for (uint32_t k = 0; k < (uint32_t)(sizeof(kVtableChain) / sizeof(kVtableChain[0])); ++k) {
    if (vt == va_of(kVtableChain[k])) return true;
  }
  return false;
}

// 严格校验：vtable 必须是 View 类链上的值，且位置字段合法。
bool view_is_obj(uint32_t view) {
  if (!is_ptr(view) || !module_base()) return false;
  __try {
    const uint32_t vt = read_u32(view);
    if (!is_view_vtable(vt)) return false;
    const float x = read_f32(view + kOffPosX);
    const float y = read_f32(view + kOffPosY);
    const float z = read_f32(view + kOffPosZ);
    if (!finite_f(x) || !finite_f(y) || !finite_f(z)) return false;
    if (std::fabs(x) > 1.0e7f || std::fabs(y) > 1.0e7f || std::fabs(z) > 1.0e7f) return false;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// 严格校验（内存扫描用）：额外要求 0x7EC240 写下的恒等式成立，
// 即 +0x38/+0x3C/+0x40 必须等于矩阵平移 m[3]/m[7]/m[11]，避免误命中。
bool view_sane(uint32_t view) {
  if (!view_is_obj(view)) return false;
  __try {
    const float x = read_f32(view + kOffPosX);
    const float y = read_f32(view + kOffPosY);
    const float z = read_f32(view + kOffPosZ);
    const float mx = read_f32(view + kOffMatrix + 12);
    const float my = read_f32(view + kOffMatrix + 28);
    const float mz = read_f32(view + kOffMatrix + 44);
    if (!finite_f(mx) || !finite_f(my) || !finite_f(mz)) return false;
    if (std::fabs(x - mx) > 4.f || std::fabs(y - my) > 4.f || std::fabs(z - mz) > 4.f) {
      return false;
    }
    const float d = read_f32(view + kOffDist);
    if (!finite_f(d) || std::fabs(d) > 1.0e6f) return false;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// hook 还没被引擎调用过时，从 View 登记表里兜底找一个。
uint32_t scan_view_list() {
  const uint32_t base = module_base();
  if (!base) return 0;
  // 0x7EC240 内部是：idx = [0x00CEAA00]; vec = 0x00CEAAC0 + idx*16。
  // 先按真实索引取槽，再兜底扫 0~3，避免索引不是 0/1 时扑空。
  int first = 0, last = 3;
  __try {
    const uint32_t idx = read_u32(base + (kViewListIndexVa - kModBase));
    if (idx < 4u) first = (int)idx;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  for (int g = first; g <= last; ++g) {
    const uint32_t vec = base + (kViewListBase - kModBase) + (uint32_t)g * 16u;
    uint32_t begin = 0, end = 0;
    __try {
      begin = read_u32(vec);
      end = read_u32(vec + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      continue;
    }
    if (!is_ptr(begin) || !is_ptr(end) || end <= begin) continue;
    const int n = (int)((end - begin) / 4u);
    if (n > 8) continue;
    for (int i = 0; i < n; ++i) {
      uint32_t p = 0;
      __try {
        p = read_u32(begin + (uint32_t)i * 4u);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        break;
      }
      if (view_sane(p)) return p;
    }
  }
  return 0;
}

// 兜底：hook 没触发、登记表也空时，在模块可写段里按主 vtable 指针找 View 对象。
// 只在 g_view 为空时调用，且节流到 2 秒一次，避免影响帧率。
uint32_t scan_view_by_vtable() {
  const uint32_t base = module_base();
  if (!base) return 0;
  static DWORD s_next = 0;
  const DWORD now = GetTickCount();
  if (s_next && (int32_t)(now - s_next) < 0) return 0;
  s_next = now + 2000;

  uint32_t hit = 0;
  __try {
    const uint32_t pe = base + *reinterpret_cast<const uint32_t*>(base + 0x3C);
    if (*reinterpret_cast<const uint16_t*>(pe) != 0x4550u) return 0;  // 'PE'
    const uint16_t nsec = *reinterpret_cast<const uint16_t*>(pe + 6);
    const uint16_t opt_sz = *reinterpret_cast<const uint16_t*>(pe + 0x14);
    const uint32_t sec = pe + 0x18 + opt_sz;
    for (uint16_t i = 0; i < nsec && i < 16 && !hit; ++i) {
      const uint32_t s = sec + (uint32_t)i * 40u;
      const uint32_t vsz = *reinterpret_cast<const uint32_t*>(s + 8);
      const uint32_t va = *reinterpret_cast<const uint32_t*>(s + 12);
      const uint32_t flags = *reinterpret_cast<const uint32_t*>(s + 36);
      if (!(flags & 0x80000000u)) continue;  // 只看可写段
      if (vsz < 0x500u || vsz > 0x4000000u) continue;
      const uint32_t lo = base + va;
      const uint32_t hi = lo + vsz - 0x500u;
      for (uint32_t p = lo; p < hi; p += 4u) {
        const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
        bool wanted = false;
        for (uint32_t k = 0; k < (uint32_t)(sizeof(kVtableChain) / sizeof(kVtableChain[0])); ++k) {
          if (v == va_of(kVtableChain[k])) {
            wanted = true;
            break;
          }
        }
        if (!wanted) continue;
        if (view_sane(p)) {
          hit = p;
          break;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (hit) log("camera: view found by vtable scan -> 0x%X vt=0x%X", hit, read_u32(hit));
  return hit;
}

uint32_t current_view() {
  if (view_is_obj(g_view)) return g_view;
  g_view = 0;
  uint32_t found = scan_view_list();
  if (!found) {
    // 探针 C 记录下来的 View 里挑一个结构完整的。
    for (int i = 0; i < g_seen_count; ++i) {
      if (view_is_obj(g_seen_views[i])) {
        found = g_seen_views[i];
        break;
      }
    }
  }
  if (!found) found = scan_view_by_vtable();
  if (found) g_view = found;
  return found;
}

bool read_view_state(uint32_t view, CameraState* out) {
  if (!out || !view_is_obj(view)) return false;
  __try {
    float m[12];
    std::memcpy(m, reinterpret_cast<const void*>(view + kOffMatrix), sizeof(m));
    CameraState st;
    if (!matrix_to_state(m, &st)) return false;
    st.view = view;
    st.dist = read_f32(view + kOffDist);
    st.valid = true;
    *out = st;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool write_view_state(uint32_t view, const CameraState& st) {
  if (!view_is_obj(view)) return false;
  __try {
    float m[12];
    build_matrix(m, st);
    if (!matrix_finite(m)) return false;
    std::memcpy(reinterpret_cast<void*>(view + kOffMatrix), m, sizeof(m));
    write_f32(view + kOffPosX, m[3]);
    write_f32(view + kOffPosY, m[7]);
    write_f32(view + kOffPosZ, m[11]);
    write_f32(view + kOffAngle, game_angle_of(m));
    if (st.dist > 0.f && finite_f(st.dist)) write_f32(view + kOffDist, st.dist);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// setTransform hook：抓 View 指针 + 接管相机
// ---------------------------------------------------------------------------
// 诊断埋点：hook 是否被调用、拿到的 this 是否通过校验。
volatile long g_hook_hits = 0;
volatile long g_hook_rejects = 0;
uint32_t g_hook_self = 0;
uint32_t g_hook_self_vt = 0;
uint32_t g_hook_matrix_ptr = 0;
uint32_t g_hook_vt_min = 0;
uint32_t g_hook_vt_max = 0;
uint32_t g_hook2_self = 0;
uint32_t g_hook2_vt = 0;
// hook B 见到的 self vtable 直方图（最多 8 种），用于摸清它的对象类型。
uint32_t g_hook2_vt_tab[8] = {};
long g_hook2_vt_cnt[8] = {};
int g_hook2_vt_n = 0;

// 记录一次调用（不含 SEH，detour 负责保护）。
void note_call(uint32_t view, uint32_t m_ptr, int which) {
  const uint32_t vt = is_ptr(view) ? read_u32(view) : 0;
  if (which == 0) {
    g_hook_self = view;
    g_hook_self_vt = vt;
    g_hook_matrix_ptr = m_ptr;
  } else {
    g_hook2_self = view;
    g_hook2_vt = vt;
    int slot = -1;
    for (int i = 0; i < g_hook2_vt_n; ++i) {
      if (g_hook2_vt_tab[i] == vt) {
        slot = i;
        break;
      }
    }
    if (slot < 0 && g_hook2_vt_n < 8) {
      slot = g_hook2_vt_n++;
      g_hook2_vt_tab[slot] = vt;
    }
    if (slot >= 0) ++g_hook2_vt_cnt[slot];
  }
  if (!g_hook_vt_min || vt < g_hook_vt_min) g_hook_vt_min = vt;
  if (vt > g_hook_vt_max) g_hook_vt_max = vt;
}

// 校验通过则记下 View，并按需把接管目标写回矩阵/距离。
bool capture_and_maybe_takeover(uint32_t view, float* m) {
  if (!m || !is_ptr(view) || !view_is_obj(view)) {
    InterlockedIncrement(&g_hook_rejects);
    return false;
  }
  bool takeover = false;
  CameraState target;
  if (g_cs_ready) EnterCriticalSection(&g_cs);
  g_view = view;
  takeover = g_takeover && g_live_valid;
  target = g_live;
  if (g_cs_ready) LeaveCriticalSection(&g_cs);
  if (takeover) {
    float nm[12];
    build_matrix(nm, target);
    if (matrix_finite(nm)) {
      std::memcpy(m, nm, sizeof(nm));
      // 缩放字段（+0x260）正好在调用点之前被写脏，这里压回我们要的值。
      // 只有确认是 View 类才写（view_is_obj 已严格校验过）。
      if (target.dist > 0.f && finite_f(target.dist)) write_f32(view + kOffDist, target.dist);
    }
  }
  return true;
}

int __fastcall hk_set_camera(void* self, void* /*edx*/, float* m) {
  // 注意：本函数含 __try，MSVC 不允许出现需要对象析构的局部变量，
  // 所以这里用显式的 Enter/LeaveCriticalSection（不能用 RAII 的 Lock）。
  __try {
    if (m) {
      const uint32_t view = reinterpret_cast<uint32_t>(self);
      InterlockedIncrement(&g_hook_hits);
      note_call(view, reinterpret_cast<uint32_t>(m), 0);
      capture_and_maybe_takeover(view, m);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return g_orig_set_camera ? g_orig_set_camera(self, nullptr, m) : 0;
}

int __fastcall hk_set_camera2(void* self, void* /*edx*/, float* m) {
  __try {
    if (m) {
      const uint32_t view = reinterpret_cast<uint32_t>(self);
      InterlockedIncrement(&g_hook2_hits);
      note_call(view, reinterpret_cast<uint32_t>(m), 1);
      capture_and_maybe_takeover(view, m);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return g_orig_set_camera2 ? g_orig_set_camera2(self, nullptr, m) : 0;
}

int __fastcall hk_view_init(void* self, void* edx) {
  __try {
    const uint32_t v = reinterpret_cast<uint32_t>(self);
    InterlockedIncrement(&g_hook3_hits);
    if (is_ptr(v)) {
      bool known = false;
      for (int i = 0; i < g_seen_count; ++i) {
        if (g_seen_views[i] == v) {
          known = true;
          break;
        }
      }
      if (!known && g_seen_count < 16) {
        g_seen_views[g_seen_count] = v;
        g_seen_vt[g_seen_count] = read_u32(v);
        ++g_seen_count;
      }
      if (view_is_obj(v)) {
        if (g_cs_ready) EnterCriticalSection(&g_cs);
        g_view = v;
        if (g_cs_ready) LeaveCriticalSection(&g_cs);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return g_orig_view_init ? g_orig_view_init(self, edx) : 0;
}

void install_hook_locked() {
  if (g_hook_state == 1 && g_hook2_state == 1) return;
  if (!module_base()) return;  // 起义时刻或还没附加到游戏
  const DWORD now = GetTickCount();
  if (g_hook_next_try && (int32_t)(now - g_hook_next_try) < 0) return;
  g_hook_next_try = now + 3000;

  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
    log("camera: MH_Initialize failed %d", (int)st);
    g_hook_state = -1;
    g_hook2_state = -1;
    return;
  }

  if (g_hook_state != 1) {
    void* target = reinterpret_cast<void*>(va_of(kFnSetCamera));
    st = MH_CreateHook(target, reinterpret_cast<void*>(&hk_set_camera),
                       reinterpret_cast<void**>(&g_orig_set_camera));
    if (st != MH_OK) {
      log("camera: hook 0x%X failed %d", kFnSetCamera, (int)st);
      g_orig_set_camera = nullptr;
      g_hook_state = -1;
    } else if (MH_EnableHook(target) != MH_OK) {
      log("camera: enable hook 0x%X failed", kFnSetCamera);
      g_hook_state = -1;
    } else {
      g_hook_state = 1;
      log("camera: hook 0x%X armed (set_camera_matrix A)", kFnSetCamera);
    }
  }

  if (g_hook2_state != 1) {
    void* target2 = reinterpret_cast<void*>(va_of(kFnSetCamera2));
    st = MH_CreateHook(target2, reinterpret_cast<void*>(&hk_set_camera2),
                       reinterpret_cast<void**>(&g_orig_set_camera2));
    if (st != MH_OK) {
      log("camera: hook 0x%X failed %d", kFnSetCamera2, (int)st);
      g_orig_set_camera2 = nullptr;
      g_hook2_state = -1;
    } else if (MH_EnableHook(target2) != MH_OK) {
      log("camera: enable hook 0x%X failed", kFnSetCamera2);
      g_hook2_state = -1;
    } else {
      g_hook2_state = 1;
      log("camera: hook 0x%X armed (set_camera_matrix B)", kFnSetCamera2);
    }
  }

  if (g_hook3_state != 1) {
    void* target3 = reinterpret_cast<void*>(va_of(kFnViewInit));
    st = MH_CreateHook(target3, reinterpret_cast<void*>(&hk_view_init),
                       reinterpret_cast<void**>(&g_orig_view_init));
    if (st != MH_OK) {
      log("camera: hook 0x%X failed %d", kFnViewInit, (int)st);
      g_orig_view_init = nullptr;
      g_hook3_state = -1;
    } else if (MH_EnableHook(target3) != MH_OK) {
      log("camera: enable hook 0x%X failed", kFnViewInit);
      g_hook3_state = -1;
    } else {
      g_hook3_state = 1;
      log("camera: hook 0x%X armed (view_init C)", kFnViewInit);
    }
  }
}

// ---------------------------------------------------------------------------
// 轨迹 / 定时事件 / 配置
// ---------------------------------------------------------------------------
struct CamConfig {
  std::string name;
  std::string note;
  std::vector<CameraKey> keys;
  std::vector<CameraEvent> events;
};

std::vector<CamConfig> g_configs;
int g_active = 0;
int g_interp = kCamInterpLinear;
int g_play_state = kCamStopped;
float g_play_t = 0.f;
float g_rate = 1.f;
bool g_loop = false;
float g_match_seconds = -1.f;
float g_match_timer = 0.f;
DWORD g_last_tick = 0;
bool g_takeover_before_play = false;
bool g_loaded = false;
bool g_dirty = false;
float g_save_timer = 0.f;
char g_path_utf8[MAX_PATH * 3] = {};

const char* kEventFeatureKeys[] = {"map", "fastbuild", "power", "ammo",
                                   "superpower", "disableallsp"};
const char* kEventFeatureLabels[] = {u8"消散战争迷雾", u8"快速建造", u8"电力无限",
                                     u8"弹药无限", u8"超级武器", u8"禁用超武"};

void ensure_configs() {
  if (!g_configs.empty()) return;
  CamConfig c;
  c.name = u8"默认轨迹";
  g_configs.push_back(c);
  g_active = 0;
}

std::vector<CameraKey>& active_keys() {
  ensure_configs();
  if (g_active < 0 || g_active >= (int)g_configs.size()) g_active = 0;
  return g_configs[g_active].keys;
}

std::vector<CameraEvent>& active_events() {
  ensure_configs();
  if (g_active < 0 || g_active >= (int)g_configs.size()) g_active = 0;
  return g_configs[g_active].events;
}

void mark_dirty() {
  g_dirty = true;
  g_save_timer = 1.5f;
}

void sanitize_label(char* dst, size_t n, const char* src) {
  if (!dst || n == 0) return;
  dst[0] = 0;
  if (!src) return;
  size_t o = 0;
  for (const char* p = src; *p && o + 1 < n; ++p) {
    unsigned char ch = (unsigned char)*p;
    if (ch == '|' || ch == '\n' || ch == '\r' || ch == '=') ch = '_';
    if (ch < 0x20) continue;
    dst[o++] = (char)ch;
  }
  dst[o] = 0;
}

bool is_utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// 把带 UTF-8 中文的标签截到不超过 limit 字节（不切半个字）。
void clamp_utf8(char* s, size_t limit) {
  if (!s) return;
  const size_t len = std::strlen(s);
  if (len <= limit) return;
  size_t cut = limit;
  while (cut > 0 && is_utf8_cont((unsigned char)s[cut])) --cut;
  s[cut] = 0;
}

void key_to_state(const CameraKey& k, CameraState* st) {
  st->valid = true;
  st->x = k.x;
  st->y = k.y;
  st->z = k.z;
  st->yaw = k.yaw;
  st->pitch = k.pitch;
  st->roll = k.roll;
  st->dist = k.dist;
}

void lerp_key(const CameraKey& a, const CameraKey& b, float u, CameraState* out) {
  out->valid = true;
  out->x = a.x + (b.x - a.x) * u;
  out->y = a.y + (b.y - a.y) * u;
  out->z = a.z + (b.z - a.z) * u;
  out->yaw = a.yaw + (b.yaw - a.yaw) * u;
  out->pitch = a.pitch + (b.pitch - a.pitch) * u;
  out->roll = a.roll + (b.roll - a.roll) * u;
  out->dist = a.dist + (b.dist - a.dist) * u;
  if (out->dist < 0.f) out->dist = 0.f;
}

bool eval_linear(const std::vector<CameraKey>& keys, float t, CameraState* out) {
  const size_t n = keys.size();
  if (t <= keys[0].t) {
    key_to_state(keys[0], out);
    return true;
  }
  if (t >= keys[n - 1].t) {
    key_to_state(keys[n - 1], out);
    return true;
  }
  for (size_t i = 0; i + 1 < n; ++i) {
    if (t <= keys[i + 1].t) {
      const float span = keys[i + 1].t - keys[i].t;
      const float u = span > 1e-6f ? (t - keys[i].t) / span : 0.f;
      lerp_key(keys[i], keys[i + 1], u, out);
      return true;
    }
  }
  key_to_state(keys[n - 1], out);
  return true;
}

// Catmull-Rom：曲线穿过每个节点（额外选项，比贝塞尔好用）。
bool eval_smooth(const std::vector<CameraKey>& keys, float t, CameraState* out) {
  const size_t n = keys.size();
  if (t <= keys[0].t) {
    key_to_state(keys[0], out);
    return true;
  }
  if (t >= keys[n - 1].t) {
    key_to_state(keys[n - 1], out);
    return true;
  }
  size_t i = 0;
  for (size_t k = 0; k + 1 < n; ++k) {
    if (t <= keys[k + 1].t) {
      i = k;
      break;
    }
  }
  const float span = keys[i + 1].t - keys[i].t;
  const float u = span > 1e-6f ? (t - keys[i].t) / span : 0.f;
  const CameraKey& p0 = keys[i == 0 ? 0 : i - 1];
  const CameraKey& p1 = keys[i];
  const CameraKey& p2 = keys[i + 1];
  const CameraKey& p3 = keys[i + 2 < n ? i + 2 : n - 1];
  const float u2 = u * u;
  const float u3 = u2 * u;
  auto cr = [u, u2, u3](float a, float b, float c, float d) {
    return 0.5f * ((2.f * b) + (-a + c) * u + (2.f * a - 5.f * b + 4.f * c - d) * u2 +
                   (-a + 3.f * b - 3.f * c + d) * u3);
  };
  out->valid = true;
  out->x = cr(p0.x, p1.x, p2.x, p3.x);
  out->y = cr(p0.y, p1.y, p2.y, p3.y);
  out->z = cr(p0.z, p1.z, p2.z, p3.z);
  out->yaw = cr(p0.yaw, p1.yaw, p2.yaw, p3.yaw);
  out->pitch = cr(p0.pitch, p1.pitch, p2.pitch, p3.pitch);
  out->roll = cr(p0.roll, p1.roll, p2.roll, p3.roll);
  out->dist = cr(p0.dist, p1.dist, p2.dist, p3.dist);
  if (out->dist < 0.f) out->dist = 0.f;
  return true;
}

// 贝塞尔：把全部节点当作控制点（和 CameraBridge 一致，因此不一定经过节点）。
bool eval_bezier(const std::vector<CameraKey>& keys, float t, CameraState* out) {
  const size_t n = keys.size();
  const float t0 = keys[0].t;
  const float t1 = keys[n - 1].t;
  float u = 0.f;
  if (t1 - t0 > 1e-6f) u = clampf((t - t0) / (t1 - t0), 0.f, 1.f);
  const size_t m = n > (size_t)kMaxKeys ? (size_t)kMaxKeys : n;
  float px[kMaxKeys], py[kMaxKeys], pz[kMaxKeys], pyaw[kMaxKeys], ppit[kMaxKeys];
  float prol[kMaxKeys], pdst[kMaxKeys];
  for (size_t i = 0; i < m; ++i) {
    px[i] = keys[i].x;
    py[i] = keys[i].y;
    pz[i] = keys[i].z;
    pyaw[i] = keys[i].yaw;
    ppit[i] = keys[i].pitch;
    prol[i] = keys[i].roll;
    pdst[i] = keys[i].dist;
  }
  const float a = 1.f - u;
  for (size_t pass = m; pass > 1; --pass) {
    for (size_t i = 0; i + 1 < pass; ++i) {
      px[i] = px[i] * a + px[i + 1] * u;
      py[i] = py[i] * a + py[i + 1] * u;
      pz[i] = pz[i] * a + pz[i + 1] * u;
      pyaw[i] = pyaw[i] * a + pyaw[i + 1] * u;
      ppit[i] = ppit[i] * a + ppit[i + 1] * u;
      prol[i] = prol[i] * a + prol[i + 1] * u;
      pdst[i] = pdst[i] * a + pdst[i + 1] * u;
    }
  }
  out->valid = true;
  out->x = px[0];
  out->y = py[0];
  out->z = pz[0];
  out->yaw = pyaw[0];
  out->pitch = ppit[0];
  out->roll = prol[0];
  out->dist = pdst[0] < 0.f ? 0.f : pdst[0];
  return true;
}

bool eval_track(float t, CameraState* out) {
  const std::vector<CameraKey>& keys = active_keys();
  if (keys.empty()) return false;
  if (keys.size() == 1) {
    key_to_state(keys[0], out);
    return true;
  }
  if (g_interp == kCamInterpLinear) return eval_linear(keys, t, out);
  if (g_interp == kCamInterpSmooth) return eval_smooth(keys, t, out);
  return eval_bezier(keys, t, out);
}

float total_time() {
  const std::vector<CameraKey>& keys = active_keys();
  if (keys.empty()) return 0.f;
  return keys.back().t > 0.f ? keys.back().t : 0.f;
}

void sort_keys() {
  std::vector<CameraKey>& keys = active_keys();
  std::stable_sort(keys.begin(), keys.end(),
                   [](const CameraKey& a, const CameraKey& b) { return a.t < b.t; });
}

void push_live(const CameraState& st) {
  g_live = st;
  g_live.valid = true;
  g_live_valid = true;
}

void apply_track_to_live(float t) {
  CameraState st;
  if (!eval_track(t, &st)) return;
  if (g_live_valid && st.dist <= 0.f) st.dist = g_live.dist;
  st.view = g_view;
  push_live(st);
}

void fire_action(int action, int param);

void handle_events(float dt) {
  g_match_timer -= dt;
  if (g_match_timer <= 0.f) {
    g_match_timer = 0.4f;
    float secs = -1.f;
    g_match_seconds = battle_clock(&secs) ? secs : -1.f;
  }
  std::vector<CameraEvent>& evs = active_events();
  for (size_t i = 0; i < evs.size(); ++i) {
    if (evs[i].fired) continue;
    const float now = evs[i].base == 1 ? g_match_seconds : g_play_t;
    if (now < 0.f) continue;
    if (evs[i].t <= now) {
      evs[i].fired = true;
      mark_dirty();
      log("camera: event #%d fired (base=%d t=%.2f now=%.2f)", (int)i, evs[i].base, evs[i].t,
          now);
      fire_action(evs[i].action, evs[i].param);
    }
  }
}

void fire_action(int action, int param) {
  switch (action) {
    case 0:  // 开始动画
      camera_play_start();
      break;
    case 1:  // 停止动画
      camera_play_stop();
      break;
    case 2:  // 暂停 / 继续
      camera_play_pause();
      break;
    case 3:  // 跳到节点
      if (param >= 0 && param < (int)active_keys().size()) {
        g_play_t = active_keys()[param].t;
        g_takeover = true;
        apply_track_to_live(g_play_t);
      }
      break;
    case 4: {  // 使用节点坐标（并把相机钉在那里）
      const std::vector<CameraKey>& keys = active_keys();
      if (param >= 0 && param < (int)keys.size()) {
        CameraState st;
        key_to_state(keys[param], &st);
        if (st.dist <= 0.f && g_live_valid) st.dist = g_live.dist;
        push_live(st);
        g_takeover = true;
      }
      break;
    }
    case 5: {  // 切换功能开关
      const int n = camera_event_feature_count();
      if (param >= 0 && param < n) {
        const char* key = kEventFeatureKeys[param];
        const bool on = !feature_enabled(key);
        std::string msg;
        toggle_feature(key, on, &msg);
        log("camera: event toggled %s=%d %s", key, on ? 1 : 0, msg.c_str());
      }
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// 配置文件
// ---------------------------------------------------------------------------
bool file_path_w(wchar_t* out, size_t n) {
  if (!out || n < 8) return false;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&file_path_w), &self) ||
      !self) {
    return false;
  }
  wchar_t p[MAX_PATH] = {};
  if (!GetModuleFileNameW(self, p, MAX_PATH)) return false;
  wchar_t* slash = wcsrchr(p, L'\\');
  if (!slash) return false;
  *slash = 0;
  _snwprintf_s(out, n, _TRUNCATE, L"%s\\ra3_camera.ini", p);
  return true;
}

const char* path_utf8() {
  if (g_path_utf8[0]) return g_path_utf8;
  wchar_t w[MAX_PATH] = {};
  if (!file_path_w(w, MAX_PATH)) return "";
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, g_path_utf8, sizeof(g_path_utf8) - 1, nullptr,
                          nullptr) <= 0) {
    g_path_utf8[0] = 0;
    return "";
  }
  g_path_utf8[sizeof(g_path_utf8) - 1] = 0;
  return g_path_utf8;
}

void trim_eol(std::string* s) {
  while (!s->empty() && (s->back() == '\r' || s->back() == '\n' || s->back() == ' ')) {
    s->pop_back();
  }
}

int split_floats(const std::string& s, float* out, int max) {
  int n = 0;
  size_t pos = 0;
  while (pos <= s.size() && n < max) {
    const size_t bar = s.find('|', pos);
    const std::string tok =
        s.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
    out[n++] = (float)std::atof(tok.c_str());
    if (bar == std::string::npos) break;
    pos = bar + 1;
  }
  return n;
}

bool save_configs() {
  const char* path = path_utf8();
  if (!path || !path[0]) return false;
  std::string out;
  char line[640];
  out += "# RA3 overlay camera tracks (CameraBridge style)\n";
  out += "version=1\n";
  std::snprintf(line, sizeof(line), "active=%d\n", g_active);
  out += line;
  std::snprintf(line, sizeof(line), "count=%d\n", (int)g_configs.size());
  out += line;
  for (size_t i = 0; i < g_configs.size(); ++i) {
    const CamConfig& c = g_configs[i];
    std::snprintf(line, sizeof(line), "c%d.name=%s\n", (int)i, c.name.c_str());
    out += line;
    std::snprintf(line, sizeof(line), "c%d.note=%s\n", (int)i, c.note.c_str());
    out += line;
    for (const CameraKey& k : c.keys) {
      std::snprintf(line, sizeof(line),
                    "c%d.key=%.4f|%.3f|%.3f|%.3f|%.5f|%.5f|%.5f|%.1f|%s\n", (int)i, k.t, k.x,
                    k.y, k.z, k.yaw, k.pitch, k.roll, k.dist, k.label);
      out += line;
    }
    for (const CameraEvent& e : c.events) {
      std::snprintf(line, sizeof(line), "c%d.ev=%.3f|%d|%d|%d\n", (int)i, e.t, e.base, e.action,
                    e.param);
      out += line;
    }
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(out.data(), (std::streamsize)out.size());
  return f.good();
}

bool load_configs() {
  ensure_configs();
  const char* path = path_utf8();
  if (!path || !path[0]) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<CamConfig> parsed;
  int active = 0;
  std::string raw;
  while (std::getline(f, raw)) {
    trim_eol(&raw);
    if (raw.empty() || raw[0] == '#') continue;
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = raw.substr(0, eq);
    const std::string val = raw.substr(eq + 1);
    if (key == "active") {
      active = std::atoi(val.c_str());
      continue;
    }
    if (key == "version" || key == "count") continue;
    if (key.size() < 4 || key[0] != 'c') continue;
    const size_t dot = key.find('.');
    if (dot == std::string::npos) continue;
    const int idx = std::atoi(key.substr(1, dot - 1).c_str());
    if (idx < 0 || idx >= kMaxConfigs) continue;
    const std::string field = key.substr(dot + 1);
    if ((int)parsed.size() <= idx) parsed.resize((size_t)idx + 1);
    CamConfig& c = parsed[(size_t)idx];
    if (field == "name") {
      c.name = val;
    } else if (field == "note") {
      c.note = val;
    } else if (field == "key") {
      std::string head = val;
      std::string label;
      const size_t last = val.rfind('|');
      if (last != std::string::npos) {
        head = val.substr(0, last);
        label = val.substr(last + 1);
      }
      float v[8] = {};
      const int got = split_floats(head, v, 8);
      if (got < 1) continue;
      CameraKey k;
      k.t = v[0];
      if (got > 1) k.x = v[1];
      if (got > 2) k.y = v[2];
      if (got > 3) k.z = v[3];
      if (got > 4) k.yaw = v[4];
      if (got > 5) k.pitch = v[5];
      if (got > 6) k.roll = v[6];
      if (got > 7) k.dist = v[7];
      sanitize_label(k.label, sizeof(k.label), label.c_str());
      if ((int)c.keys.size() < kMaxKeys) c.keys.push_back(k);
    } else if (field == "ev") {
      float v[4] = {};
      const int got = split_floats(val, v, 4);
      if (got < 1) continue;
      CameraEvent e;
      e.t = v[0];
      if (got > 1) e.base = (int)v[1];
      if (got > 2) e.action = (int)v[2];
      if (got > 3) e.param = (int)v[3];
      if ((int)c.events.size() < 256) c.events.push_back(e);
    }
  }
  if (parsed.empty()) return true;
  for (CamConfig& c : parsed) {
    if (c.name.empty()) c.name = u8"未命名";
    std::stable_sort(c.keys.begin(), c.keys.end(),
                     [](const CameraKey& a, const CameraKey& b) { return a.t < b.t; });
  }
  g_configs = parsed;
  g_active = (active >= 0 && active < (int)g_configs.size()) ? active : 0;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
bool camera_ready() {
  Lock lock;
  return current_view() != 0;
}

int camera_hook_state() {
  Lock lock;
  if (g_hook_state != 1) install_hook_locked();
  return g_hook_state;
}

void camera_install_hook() {
  Lock lock;
  install_hook_locked();
}

bool camera_read(CameraState* out) {
  if (!out) return false;
  Lock lock;
  const uint32_t view = current_view();
  if (!view) return false;
  return read_view_state(view, out);
}

bool camera_apply(const CameraState* st) {
  if (!st) return false;
  Lock lock;
  const uint32_t view = current_view();
  if (!view) return false;
  CameraState s = *st;
  if (s.dist <= 0.f && g_live_valid) s.dist = g_live.dist;
  const bool ok = write_view_state(view, s);
  if (ok) {
    s.view = view;
    push_live(s);
  }
  return ok;
}

void camera_set_takeover(bool on) {
  Lock lock;
  if (on == g_takeover) return;
  if (on) {
    CameraState st;
    const uint32_t view = current_view();
    if (view && read_view_state(view, &st)) {
      if (st.dist <= 0.f) st.dist = 1000.f;
      push_live(st);
    } else if (!g_live_valid) {
      g_live = CameraState{};
      g_live_valid = true;
    }
    g_takeover = true;
    log("camera: takeover ON (view=%08X)", view);
  } else {
    g_takeover = false;
    log("camera: takeover OFF");
  }
}

bool camera_takeover() {
  Lock lock;
  return g_takeover;
}

void camera_set_live(const CameraState* st) {
  if (!st) return;
  Lock lock;
  g_live = *st;
  g_live.valid = true;
  g_live_valid = true;
}

bool camera_live(CameraState* out) {
  if (!out) return false;
  Lock lock;
  if (!g_live_valid) return false;
  *out = g_live;
  return true;
}

// ---------------------------------------------------------------------------
// 轨迹
// ---------------------------------------------------------------------------
int camera_key_count() {
  Lock lock;
  return (int)active_keys().size();
}

bool camera_key(int index, CameraKey* out) {
  if (!out) return false;
  Lock lock;
  const std::vector<CameraKey>& keys = active_keys();
  if (index < 0 || index >= (int)keys.size()) return false;
  *out = keys[(size_t)index];
  return true;
}

int camera_key_add_from_current(const char* label) {
  Lock lock;
  CameraState st;
  if (g_takeover && g_live_valid) {
    st = g_live;
  } else {
    const uint32_t view = current_view();
    if (!view || !read_view_state(view, &st)) return -1;
  }
  std::vector<CameraKey>& keys = active_keys();
  if ((int)keys.size() >= kMaxKeys) return -1;
  CameraKey k;
  k.t = keys.empty() ? 0.f : keys.back().t + 1.f;
  k.x = st.x;
  k.y = st.y;
  k.z = st.z;
  k.yaw = st.yaw;
  k.pitch = st.pitch;
  k.roll = st.roll;
  k.dist = st.dist;
  sanitize_label(k.label, sizeof(k.label), label);
  keys.push_back(k);
  sort_keys();
  mark_dirty();
  for (int i = 0; i < (int)keys.size(); ++i) {
    if (keys[(size_t)i].t == k.t) return i;
  }
  return (int)keys.size() - 1;
}

bool camera_key_update_from_current(int index) {
  Lock lock;
  std::vector<CameraKey>& keys = active_keys();
  if (index < 0 || index >= (int)keys.size()) return false;
  CameraState st;
  if (g_takeover && g_live_valid) {
    st = g_live;
  } else {
    const uint32_t view = current_view();
    if (!view || !read_view_state(view, &st)) return false;
  }
  CameraKey& k = keys[(size_t)index];
  k.x = st.x;
  k.y = st.y;
  k.z = st.z;
  k.yaw = st.yaw;
  k.pitch = st.pitch;
  k.roll = st.roll;
  k.dist = st.dist;
  mark_dirty();
  return true;
}

bool camera_key_set(int index, const CameraKey& key) {
  Lock lock;
  std::vector<CameraKey>& keys = active_keys();
  if (index < 0 || index >= (int)keys.size()) return false;
  CameraKey k = key;
  k.label[sizeof(k.label) - 1] = 0;
  clamp_utf8(k.label, sizeof(k.label) - 1);
  keys[(size_t)index] = k;
  sort_keys();
  mark_dirty();
  return true;
}

bool camera_key_remove(int index) {
  Lock lock;
  std::vector<CameraKey>& keys = active_keys();
  if (index < 0 || index >= (int)keys.size()) return false;
  keys.erase(keys.begin() + index);
  mark_dirty();
  return true;
}

void camera_keys_clear() {
  Lock lock;
  active_keys().clear();
  mark_dirty();
}

float camera_total_time() {
  Lock lock;
  return total_time();
}

int camera_interp() {
  Lock lock;
  return g_interp;
}

void camera_set_interp(int mode) {
  Lock lock;
  g_interp = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
  mark_dirty();
}

// ---------------------------------------------------------------------------
// 播放
// ---------------------------------------------------------------------------
int camera_play_state() {
  Lock lock;
  return g_play_state;
}

float camera_playhead() {
  Lock lock;
  return g_play_t;
}

void camera_set_playhead(float t) {
  Lock lock;
  if (t < 0.f) t = 0.f;
  g_play_t = t;
  apply_track_to_live(g_play_t);
  if (g_play_state != kCamStopped) g_takeover = true;
}

float camera_play_rate() {
  Lock lock;
  return g_rate;
}

void camera_set_play_rate(float rate) {
  Lock lock;
  g_rate = clampf(rate, 0.05f, 8.f);
  mark_dirty();
}

bool camera_loop() {
  Lock lock;
  return g_loop;
}

void camera_set_loop(bool on) {
  Lock lock;
  g_loop = on;
  mark_dirty();
}

void camera_play_start() {
  Lock lock;
  if (active_keys().empty()) return;
  if (g_play_state == kCamStopped) g_takeover_before_play = g_takeover;
  if (g_play_t >= total_time() - 1e-3f) g_play_t = 0.f;
  g_play_state = kCamPlaying;
  g_takeover = true;
  apply_track_to_live(g_play_t);
  log("camera: play start keys=%d total=%.2f interp=%d", (int)active_keys().size(), total_time(),
      g_interp);
}

void camera_play_pause() {
  Lock lock;
  if (g_play_state == kCamPlaying) {
    g_play_state = kCamPaused;
  } else if (g_play_state == kCamPaused) {
    g_play_state = kCamPlaying;
  }
}

void camera_play_stop() {
  Lock lock;
  g_play_state = kCamStopped;
  g_play_t = 0.f;
  if (!g_takeover_before_play) g_takeover = false;
  g_takeover_before_play = false;
  log("camera: play stop");
}

// ---------------------------------------------------------------------------
// 定时事件
// ---------------------------------------------------------------------------
int camera_event_count() {
  Lock lock;
  return (int)active_events().size();
}

bool camera_event(int index, CameraEvent* out) {
  if (!out) return false;
  Lock lock;
  const std::vector<CameraEvent>& evs = active_events();
  if (index < 0 || index >= (int)evs.size()) return false;
  *out = evs[(size_t)index];
  return true;
}

bool camera_event_set(int index, const CameraEvent& ev) {
  Lock lock;
  std::vector<CameraEvent>& evs = active_events();
  if (index < 0 || index >= (int)evs.size()) return false;
  CameraEvent& old = evs[(size_t)index];
  CameraEvent e = ev;
  if (e.t != old.t || e.base != old.base || e.action != old.action || e.param != old.param) {
    e.fired = false;
  } else {
    e.fired = old.fired;
  }
  old = e;
  mark_dirty();
  return true;
}

int camera_event_add(const CameraEvent& ev) {
  Lock lock;
  std::vector<CameraEvent>& evs = active_events();
  if ((int)evs.size() >= 128) return -1;
  evs.push_back(ev);
  std::stable_sort(evs.begin(), evs.end(),
                   [](const CameraEvent& a, const CameraEvent& b) { return a.t < b.t; });
  mark_dirty();
  return (int)evs.size() - 1;
}

bool camera_event_remove(int index) {
  Lock lock;
  std::vector<CameraEvent>& evs = active_events();
  if (index < 0 || index >= (int)evs.size()) return false;
  evs.erase(evs.begin() + index);
  mark_dirty();
  return true;
}

void camera_events_clear() {
  Lock lock;
  active_events().clear();
  mark_dirty();
}

void camera_events_reset() {
  Lock lock;
  for (CameraEvent& e : active_events()) e.fired = false;
  mark_dirty();
}

int camera_events_fired() {
  Lock lock;
  int n = 0;
  for (const CameraEvent& e : active_events()) {
    if (e.fired) ++n;
  }
  return n;
}

int camera_event_feature_count() {
  return (int)(sizeof(kEventFeatureKeys) / sizeof(kEventFeatureKeys[0]));
}

const char* camera_event_feature_key(int index) {
  const int n = camera_event_feature_count();
  if (index < 0 || index >= n) return "";
  return kEventFeatureKeys[index];
}

const char* camera_event_feature_label(int index) {
  const int n = camera_event_feature_count();
  if (index < 0 || index >= n) return "";
  return kEventFeatureLabels[index];
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
int camera_config_count() {
  Lock lock;
  ensure_configs();
  return (int)g_configs.size();
}

const char* camera_config_name(int index) {
  Lock lock;
  ensure_configs();
  if (index < 0 || index >= (int)g_configs.size()) return "";
  return g_configs[(size_t)index].name.c_str();
}

const char* camera_config_note(int index) {
  Lock lock;
  ensure_configs();
  if (index < 0 || index >= (int)g_configs.size()) return "";
  return g_configs[(size_t)index].note.c_str();
}

void camera_config_set_note(int index, const char* note) {
  if (!note) return;
  Lock lock;
  ensure_configs();
  if (index < 0 || index >= (int)g_configs.size()) return;
  g_configs[(size_t)index].note = note;
  mark_dirty();
}

int camera_config_active() {
  Lock lock;
  ensure_configs();
  return g_active;
}

bool camera_config_select(int index) {
  Lock lock;
  ensure_configs();
  if (index < 0 || index >= (int)g_configs.size()) return false;
  if (index == g_active) return true;
  g_active = index;
  mark_dirty();
  return true;
}

int camera_config_add(const char* name) {
  Lock lock;
  ensure_configs();
  if ((int)g_configs.size() >= kMaxConfigs) return -1;
  CamConfig c;
  c.name = (name && name[0]) ? name : u8"新轨迹";
  g_configs.push_back(c);
  g_active = (int)g_configs.size() - 1;
  mark_dirty();
  return g_active;
}

int camera_config_clone(int index, const char* name) {
  Lock lock;
  ensure_configs();
  if (index < 0 || index >= (int)g_configs.size()) return -1;
  if ((int)g_configs.size() >= kMaxConfigs) return -1;
  CamConfig c = g_configs[(size_t)index];
  c.name = (name && name[0]) ? name : (g_configs[(size_t)index].name + u8" 副本");
  for (CameraEvent& e : c.events) e.fired = false;
  g_configs.push_back(c);
  g_active = (int)g_configs.size() - 1;
  mark_dirty();
  return g_active;
}

bool camera_config_remove(int index) {
  Lock lock;
  ensure_configs();
  if (g_configs.size() <= 1) return false;
  if (index < 0 || index >= (int)g_configs.size()) return false;
  g_configs.erase(g_configs.begin() + index);
  if (g_active >= (int)g_configs.size()) g_active = (int)g_configs.size() - 1;
  mark_dirty();
  return true;
}

bool camera_config_rename(int index, const char* name) {
  if (!name || !name[0]) return false;
  Lock lock;
  ensure_configs();
  if (index < 0 || index >= (int)g_configs.size()) return false;
  g_configs[(size_t)index].name = name;
  mark_dirty();
  return true;
}

bool camera_config_save() {
  Lock lock;
  ensure_configs();
  const bool ok = save_configs();
  if (ok) {
    g_dirty = false;
    g_save_timer = 0.f;
  }
  return ok;
}

bool camera_config_load() {
  Lock lock;
  const bool ok = load_configs();
  g_loaded = true;
  g_dirty = false;
  g_save_timer = 0.f;
  return ok;
}

const char* camera_file_path() { return path_utf8(); }

// ---------------------------------------------------------------------------
// 每帧驱动
// ---------------------------------------------------------------------------
// 诊断：每 2 秒汇报一次 hook 命中情况，方便判断 View 为什么没抓到。
// 单独成函数是为了能安全使用 __try（camera_tick 里有 RAII 的 Lock）。
void camera_log_diag(DWORD now) {
  static DWORD s_next = 0;
  if ((int32_t)(now - s_next) < 0) return;
  s_next = now + 2000;

  const uint32_t base = module_base();
  uint32_t list_first = 0, list_last = 0, list_end = 0, list_ptr = 0;
  __try {
    if (base) {
      const uint32_t idx = read_u32(base + (kViewListIndexVa - kModBase));
      const uint32_t vec = base + (kViewListBase - kModBase) + (idx & 3u) * 16u;
      list_first = read_u32(vec);
      list_last = read_u32(vec + 4);
      list_end = read_u32(vec + 8);
      if (is_ptr(list_first) && is_ptr(list_last) && list_last > list_first) {
        list_ptr = read_u32(list_first);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  log("camera: diag A[hits=%ld rej=%ld self=0x%X vt=0x%X m=0x%X] "
      "B[hits=%ld self=0x%X vt=0x%X] C[hits=%ld seen=%d] view=0x%X list[F=0x%X p=0x%X]",
      (long)g_hook_hits, (long)g_hook_rejects, g_hook_self, g_hook_self_vt, g_hook_matrix_ptr,
      (long)g_hook2_hits, g_hook2_self, g_hook2_vt,
      (long)g_hook3_hits, g_seen_count,
      g_view, list_first, list_ptr);

  // 把探针 C 见到的 View 全部列出来（地址 + vtable），便于识别战术视图。
  for (int i = 0; i < g_seen_count; ++i) {
    log("camera: seen[%d] view=0x%X vt=0x%X sane=%d", i, g_seen_views[i], g_seen_vt[i],
        view_sane(g_seen_views[i]) ? 1 : 0);
  }

  // 探针 B 见到的对象类型（vtable 直方图），用于确认它是不是 View。
  for (int i = 0; i < g_hook2_vt_n; ++i) {
    log("camera: Bvt[%d] vt=0x%X count=%ld isView=%d", i, g_hook2_vt_tab[i],
        g_hook2_vt_cnt[i], is_view_vtable(g_hook2_vt_tab[i]) ? 1 : 0);
  }

  // 验证 hook 是否还在：装了 MinHook 的话目标开头应是 E9 xx xx xx xx。
  __try {
    const uint32_t f1 = va_of(kFnSetCamera);
    const uint32_t f2 = va_of(kFnSetCamera2);
    if (f1) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(f1);
      log("camera: bytes A@0x%X = %02X %02X %02X %02X %02X  tramp=0x%X",
          f1, p[0], p[1], p[2], p[3], p[4],
          g_orig_set_camera ? reinterpret_cast<uint32_t>(g_orig_set_camera) : 0);
    }
    if (f2) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(f2);
      log("camera: bytes B@0x%X = %02X %02X %02X %02X %02X  tramp=0x%X",
          f2, p[0], p[1], p[2], p[3], p[4],
          g_orig_set_camera2 ? reinterpret_cast<uint32_t>(g_orig_set_camera2) : 0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void camera_tick() {
  if (!g_cs_ready) {
    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
  }
  Lock lock;
  if (g_hook_state != 1) install_hook_locked();
  if (!g_loaded) {
    load_configs();
    g_loaded = true;
  }

  const DWORD now = GetTickCount();
  float dt = g_last_tick ? (float)(now - g_last_tick) / 1000.f : 0.f;
  g_last_tick = now;
  if (dt < 0.f) dt = 0.f;
  if (dt > 0.25f) dt = 0.25f;

  handle_events(dt);

  if (g_play_state == kCamPlaying) {
    g_play_t += dt * g_rate;
    const float total = total_time();
    if (total <= 1e-4f) {
      g_play_state = kCamStopped;
    } else if (g_play_t >= total) {
      if (g_loop) {
        g_play_t = std::fmod(g_play_t, total);
      } else {
        g_play_t = total;
        g_play_state = kCamStopped;
      }
    }
    apply_track_to_live(g_play_t);
  }

  if (g_dirty) {
    g_save_timer -= dt;
    if (g_save_timer <= 0.f) {
      save_configs();
      g_dirty = false;
      g_save_timer = 0.f;
    }
  }

  camera_log_diag(now);
}

// ---------------------------------------------------------------------------
// 热键 / engine 入口
// ---------------------------------------------------------------------------
bool camera_engine_cmd(const char* key, bool* handled, std::string* out_msg) {
  if (handled) *handled = false;
  if (!key) return false;
  const bool is_play = std::strcmp(key, "cam_play") == 0;
  const bool is_stop = std::strcmp(key, "cam_stop") == 0;
  const bool is_mark = std::strcmp(key, "cam_mark") == 0;
  const bool is_save = std::strcmp(key, "cam_save") == 0;
  if (!is_play && !is_stop && !is_mark && !is_save) return false;
  if (handled) *handled = true;

  if (is_save) {
    const bool ok = camera_config_save();
    if (out_msg) {
      *out_msg = ok ? (std::string(u8"摄像机轨道已保存: ") + camera_file_path())
                    : u8"保存摄像机轨道失败";
    }
    return ok;
  }
  if (is_mark) {
    const int idx = camera_key_add_from_current(nullptr);
    if (out_msg) {
      if (idx < 0) {
        *out_msg = u8"添加节点失败（还没读到摄像机）";
      } else {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), u8"已添加摄像机节点 #%d", idx + 1);
        *out_msg = buf;
      }
    }
    return idx >= 0;
  }
  if (is_stop) {
    camera_play_stop();
    if (out_msg) *out_msg = u8"摄像机动画已停止";
    return true;
  }
  const int st = camera_play_state();
  if (st == kCamPlaying) {
    camera_play_pause();
    if (out_msg) *out_msg = u8"摄像机动画已暂停（再按一次继续）";
    return true;
  }
  if (st == kCamPaused) {
    camera_play_pause();
    if (out_msg) *out_msg = u8"摄像机动画继续";
    return true;
  }
  if (camera_key_count() < 2) {
    if (out_msg) *out_msg = u8"至少需要两个摄像机节点才能播放";
    return false;
  }
  camera_play_start();
  if (out_msg) {
    char buf[128] = {};
    std::snprintf(buf, sizeof(buf), u8"摄像机动画开始（%d 个节点，%.1f 秒）", camera_key_count(),
                  camera_total_time());
    *out_msg = buf;
  }
  return true;
}

}  // namespace game_api
