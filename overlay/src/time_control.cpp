// ---------------------------------------------------------------------------
// 游戏时间控制（加速）—— 挂载点已确认
//
// 第一轮结论（保留备查）：
//   * [[0xCE8138]+0x54] 是**显示亮度**，不是时间倍率。它由“亮度百分比(50=中性)
//     → 伽马倍率”那套 0.6 / 0.008 / 0.02 公式算出；渲染路径 0x6232CC 读它与 1.0
//     比较后进客户端 vt+0x74，0xA8F8AD 写它的同时把 "Brightness" 这个 key 存回
//     设置对象。
//   * 0x6F91C0 也不是“原生设置速度”：它第一步（0x006F91DD）把栈参数当设置容器
//     传给 0x653080，而后者构造字符串 0x00C16B30（= "Brightness"）做 map 查找。
//     它的参数是**指针**，传 int 必然越界，所以旧的 call_native_set_pct() 每次都被
//     __try 吞掉，从未真正执行过。
//   * 同一对象里游戏自己叫 GameSpeedFactor 的是 +0x30（0x006FCDF1 压入
//     0x00C214D8 = "GameSpeedFactor" 后 fld/fstp [ecx+0x30]）。
//
// 真正的速度量 = [0x00CB409C]。引擎 0x00655166 的帧循环：
//     00655158  mov ecx, [0xcdde84]        ; TheGameLogic
//     00655164  call [edx+0x34]            ; 跑一次逻辑更新
//     00655166  mov eax, [0xcb409c]        ; update FPS
//     0065516D  div dword ptr [0xcb4098]   ; edi = updateFps / logicFps(15)
//     00655175  cmp edi, 6
//     0065519A  idiv edi                   ; edi == 0 → DIVIDE_BY_ZERO（曾实测崩过）
// 调试输出里 [0xCB4098] 叫 "current logic FPS"，[[0xCE811C]->vtbl+0x40] 叫
// "max update FPS (game speed)"。全 .text 里没有任何指令写这两个全局。
//
// 因此：**只能加速**。倍率相对“进入对局时读到的原值”（正常局 = 30），
// 写值必须落在 [logicFps, 240]（= 1x ~ 8x）。绝不碰 +0x54（亮度），
// 也绝不调用 0x6F91C0。
// ---------------------------------------------------------------------------
#include "game_api.h"
#include "game_api_internal.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kModBase = 0x400000;
constexpr uint32_t kGlobalDataPtrVa = 0x00CE8138;  // -> 显示/选项缓存对象
constexpr uint32_t kSetSpeedPctVa = 0x006F91C0;    // 已查明：重新应用“Brightness”，不再调用
constexpr uint32_t kOffSpeedFactor = 0x30;         // GameSpeedFactor（选项字段，速度候选）
constexpr uint32_t kOffTimeScale = 0x54;           // 亮度/伽马（**不是**时间倍率）

// 引擎真正的速度候选（见文件头反汇编说明）。
constexpr uint32_t kLogicFpsVa = 0x00CB4098;   // int  15  = logic FPS
constexpr uint32_t kUpdateFpsVa = 0x00CB409C;  // int  30  = 更新帧率上限（**真正的旋钮**）
constexpr uint32_t kSessionPtrVa = 0x00CE811C; // 每场对局的会话对象（只读指针，解引用留到进对局后）

// 0x00655166 的逻辑推进循环：
//   edi = [0xCB409C] / [0xCB4098]        ; 每帧要跑的逻辑更新次数
//   if (edi < 6) { ... idiv edi ... }    ; edi == 0 时 0x0065519A 处除零崩溃
// 所以 [0xCB409C] 必须 >= [0xCB4098]（=15）。写 0 会立刻让游戏崩在 0x25519A。
constexpr int kUpdateFpsSafeMax = 240;

// 倍率范围：只能加速。上限 8x（相对进局原值）。
constexpr float kSpeedScaleMax = 8.0f;

// [0xCB4098] 是 279 处代码共用的时间基（很多地方按 logicFps/2 当“半秒”），
// 所以上限收得比 updateFps 保守。
constexpr int kLogicFpsSafeMax = 120;

CRITICAL_SECTION g_tc_cs;
bool g_tc_cs_ready = false;

bool g_enabled = false;        // 是否持续维持 g_scale
float g_scale = 1.0f;          // 期望的**相对**倍率（相对进入对局时读到的原值）

float g_seen_scale = 0.f;      // 最近一次读到的 +0x54（亮度/伽马）
float g_seen_factor = 0.f;     // 最近一次读到的 +0x30（GameSpeedFactor）
bool g_seen_valid = false;
uint32_t g_obj = 0;
DWORD g_next_log = 0;

// 快进（对应 CameraBridge 的「快进功能」）
bool g_ff_active = false;
bool g_ff_auto = false;
bool g_ff_arrived = false;
float g_ff_target = 0.f;   // 目标对局时间（秒）
float g_ff_speed = 4.0f;   // 快进时使用的倍率

// ---- 速度状态 -------------------------------------------------------------
float g_speed_applied = 0.f;   // 最近一次真正生效的倍率（0 = 没写过）
int g_speed_writes = 0;

// ---- 只读诊断 -------------------------------------------------------------
DWORD g_probe_next_log = 0;
uint32_t g_probe_last_session = 0;
bool g_probe_session_dump = false;  // UI 按钮触发的一次性字段扫描
CRITICAL_SECTION g_probe_cs;
bool g_probe_cs_ready = false;

bool finite_f(float v) { return v == v && v > -3.4e38f && v < 3.4e38f; }

bool is_ptr(uint32_t v) { return v >= 0x10000u && v < 0x7FFF0000u && (v & 3u) == 0; }

uint32_t read_u32(uint32_t a) { return *reinterpret_cast<uint32_t*>(a); }
float read_f32(uint32_t a) { return *reinterpret_cast<float*>(a); }
void write_f32(uint32_t a, float v) { *reinterpret_cast<float*>(a) = v; }

// 解析全局数据对象；失败返回 0。
uint32_t global_obj() {
  const uint32_t base = game_api::module_base();
  if (!base) return 0;
  uint32_t obj = 0;
  __try {
    obj = read_u32(base + (kGlobalDataPtrVa - kModBase));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    obj = 0;
  }
  return is_ptr(obj) ? obj : 0;
}

// 读一次时间倍率 / GameSpeedFactor；成功返回 true。
bool read_locked(uint32_t* obj_out, float* scale_out, float* factor_out) {
  const uint32_t obj = global_obj();
  if (!obj) return false;
  float sc = 0.f, fa = 0.f;
  __try {
    sc = read_f32(obj + kOffTimeScale);
    fa = read_f32(obj + kOffSpeedFactor);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (!finite_f(sc)) return false;
  if (obj_out) *obj_out = obj;
  if (scale_out) *scale_out = sc;
  if (factor_out) *factor_out = fa;
  return true;
}

void log_diag(uint32_t obj, float scale, float factor, bool valid) {
  // Disabled: Present-thread fopen every 2s was hitching with the overlay open.
  (void)obj;
  (void)scale;
  (void)factor;
  (void)valid;
}

// 带 SEH 的任意读/写，仅供探测使用。
bool peek_u32(uint32_t a, uint32_t* out) {
  if (!is_ptr(a) || !out) return false;
  __try {
    *out = read_u32(a);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool peek_f32(uint32_t a, float* out) {
  if (!is_ptr(a) || !out) return false;
  __try {
    *out = read_f32(a);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return finite_f(*out);
}

bool poke_f32(uint32_t a, float v) {
  if (!is_ptr(a) || !finite_f(v)) return false;
  __try {
    write_f32(a, v);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool poke_u32(uint32_t a, uint32_t v) {
  if (!is_ptr(a)) return false;
  __try {
    *reinterpret_cast<uint32_t*>(a) = v;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

struct ProbeSnap {
  bool have_obj = false;
  float gsf = 0.f;      // [[0xCE8138]+0x30] GameSpeedFactor（速度候选）
  float bright = 0.f;   // [[0xCE8138]+0x54] 亮度/伽马（旧实现误写的字段）
  bool have_logic = false;
  int logic_fps = 0;     // [0xCB4098] logic FPS
  bool have_update = false;
  int update_fps = 0;    // [0xCB409C] 更新帧率上限（速度候选）
  uint32_t session = 0;  // [0xCE811C] 每场对局的会话对象
};

// 全部只读，而且只读内存：不再调用 session 的虚函数 getter。
// 退出对局时这个会话对象正在被拆（对象还在、vtbl 还在，但内部字段已经释放），
// 从渲染线程调 vtbl+0x40 的 getter 等于替游戏在错误的时刻访问半销毁对象。
ProbeSnap probe_read() {
  ProbeSnap s{};
  const uint32_t obj = global_obj();
  if (obj) {
    s.have_obj = true;
    peek_f32(obj + kOffSpeedFactor, &s.gsf);
    peek_f32(obj + kOffTimeScale, &s.bright);
  }
  uint32_t v = 0;
  if (peek_u32(kLogicFpsVa, &v)) {
    s.have_logic = true;
    s.logic_fps = (int)v;
  }
  if (peek_u32(kUpdateFpsVa, &v)) {
    s.have_update = true;
    s.update_fps = (int)v;
  }
  uint32_t session = 0;
  if (peek_u32(kSessionPtrVa, &session)) s.session = session;
  return s;
}

// 每 2 秒打一行候选值，用来对照“改游戏内速度选项后哪个数动了”。
void probe_log(bool force) {
  const DWORD now = GetTickCount();
  // GetTickCount 会回绕（约 49 天），回绕时把守卫放掉，宁可多打一行。
  const bool due = (now >= g_probe_next_log) || (g_probe_next_log - now) > 0x40000000u;
  if (!force && !due) return;
  g_probe_next_log = now + 2000;
  const ProbeSnap s = probe_read();
  game_api::log("time: logicFps=%d updateFps=%d speed=x%.3f | session=%08X | "
                "gsf+0x30=%.3f bright+0x54=%.3f | writes=%d",
      s.logic_fps, s.update_fps, g_speed_applied, s.session, s.gsf, s.bright,
      g_speed_writes);
  // 一次性的会话对象字段扫描：把看着像帧率/倍率的字段列出来。
  if (g_probe_session_dump && s.session) {
    g_probe_session_dump = false;
    g_probe_last_session = s.session;
    char line[512] = {};
    int used = 0;
    for (uint32_t off = 0; off <= 0x100 && used < (int)sizeof(line) - 40; off += 4) {
      uint32_t raw = 0;
      if (!peek_u32(s.session + off, &raw)) break;
      const int iv = (int)raw;
      float fv = 0.f;
      peek_f32(s.session + off, &fv);
      const bool int_like = iv >= 1 && iv <= 240;
      const bool float_like = finite_f(fv) && fv > 0.05f && fv < 8.0f;
      if (!int_like && !float_like) continue;
      used += std::snprintf(line + used, sizeof(line) - (size_t)used,
                            used ? " +%X i=%d/f=%.3f" : "+%X i=%d/f=%.3f", off, iv, fv);
    }
    game_api::log("probe: session dump %08X size<=0x100 : %s", s.session, used ? line : "(no candidates)");
  } else if (g_probe_session_dump) {
    g_probe_session_dump = false;
    game_api::log("probe: session dump skipped ([0x%X] 为空，未进入对局)", kSessionPtrVa);
  }
}

// ---- 游戏速度：唯一已验证有效的旋钮 ----------------------------------------
// 引擎 0x00655166 读 [0xCB409C] 算出「每帧要跑几次逻辑更新」= updateFps / logicFps：
//     00655166  mov eax, [0xcb409c]
//     0065516D  div dword ptr [0xcb4098]
//     0065519A  idiv edi              ; edi == 0 → DIVIDE_BY_ZERO 崩溃
// 所以本值必须 >= [0xCB4098]（logic FPS，=15）：**只能加速，不能减速/暂停**。
// 倍率是相对“进入对局时读到的原值”（正常局原值 30，1x 即 30、4x 即 120）。
// 全 .text 里没有任何指令写这两个全局，所以只要值没被别处改掉就不必每帧重写。
// 目标选择：0 = 不改内存；1 = 只改 logic FPS；2 = 只改 update FPS；3 = 两者同步。
// 为什么需要选：引擎 0x00655166 的循环是
//     edi = updateFps / logicFps ; 然后循环里跑大约 6/edi 次逻辑更新
// 也就是说 updateFps 单独变大反而会**减少**逻辑更新次数（实测“只提帧率、甚至变慢”
// 就是这个原因）。到底哪个才是真正的速度量还没验证，所以这里做成可切换的实验项。
int g_speed_target = 0;
int g_orig_logic = 0;
int g_orig_update = 0;
bool g_speed_have_orig = false;

int live_logic_fps() {
  uint32_t raw = 0;
  int v = peek_u32(kLogicFpsVa, &raw) ? (int)raw : 15;
  if (v < 1) v = 15;
  if (v > kLogicFpsSafeMax) v = kLogicFpsSafeMax;
  return v;
}

int live_update_fps() {
  uint32_t raw = 0;
  int v = peek_u32(kUpdateFpsVa, &raw) ? (int)raw : 30;
  if (v < 1) v = 30;
  if (v > kUpdateFpsSafeMax) v = kUpdateFpsSafeMax;
  return v;
}

int clamp_logic_fps(int v) {
  if (v < 1) v = 1;
  if (v > kLogicFpsSafeMax) v = kLogicFpsSafeMax;
  return v;
}

// 不变式（两个都不能破）：logicFps >= 1 且 updateFps >= logicFps。
// 前者防 0 除数，后者防 0x0065516D 的 updateFps/logicFps 得到 0 后再 idiv 崩溃。
void normalize_pair(int* logic, int* update) {
  *logic = clamp_logic_fps(*logic);
  if (*update < *logic) *update = *logic;
  if (*update > kUpdateFpsSafeMax) *update = kUpdateFpsSafeMax;
  if (*update < *logic) *logic = *update;
}

// scale：相对进局原值的倍率，只接受 [1, kSpeedScaleMax]。
bool apply_speed_locked(float scale) {
  if (g_speed_target < 1) return false;
  if (!finite_f(scale)) scale = 1.0f;
  if (scale < 1.0f) scale = 1.0f;  // 减速做不了（会除零），一律按 1x
  if (scale > kSpeedScaleMax) scale = kSpeedScaleMax;

  const int cur_logic = live_logic_fps();
  const int cur_update = live_update_fps();
  if (!g_speed_have_orig) {
    g_orig_logic = cur_logic;
    g_orig_update = cur_update;
    g_speed_have_orig = true;
  }

  const int scaled_logic = (int)((float)g_orig_logic * scale + 0.5f);
  const int scaled_update = (int)((float)g_orig_update * scale + 0.5f);

  int want_logic = cur_logic;
  int want_update = cur_update;
  switch (g_speed_target) {
    case 1: want_logic = scaled_logic; break;               // 只动 logic FPS
    case 2: want_update = scaled_update; break;             // 只动 update FPS
    default: want_logic = scaled_logic; want_update = scaled_update; break;  // 同步
  }
  normalize_pair(&want_logic, &want_update);

  const bool same = (want_logic == cur_logic) && (want_update == cur_update);
  g_speed_applied = scale;
  if (same) return true;  // 已经是目标值：不写、不刷日志

  if (want_logic != cur_logic && !poke_u32(kLogicFpsVa, (uint32_t)want_logic)) return false;
  if (want_update != cur_update && !poke_u32(kUpdateFpsVa, (uint32_t)want_update)) return false;
  ++g_speed_writes;
  game_api::log("time: speed x%.3f target=%d -> logicFps %d->%d updateFps %d->%d (orig %d/%d)",
      scale, g_speed_target, cur_logic, want_logic, cur_update, want_update, g_orig_logic,
      g_orig_update);
  return true;
}

void restore_speed_locked() {
  if (!g_speed_have_orig) return;
  int l = g_orig_logic;
  int u = g_orig_update;
  normalize_pair(&l, &u);
  const int cur_logic = live_logic_fps();
  const int cur_update = live_update_fps();
  if (cur_logic != l && !poke_u32(kLogicFpsVa, (uint32_t)l)) return;
  if (cur_update != u && !poke_u32(kUpdateFpsVa, (uint32_t)u)) return;
  if (cur_logic != l || cur_update != u) {
    game_api::log("time: speed restored to logicFps %d updateFps %d", l, u);
  }
  g_speed_have_orig = false;
  g_speed_applied = 0.f;
}

// 加锁包装，供 tick / UI / 热键共用。
bool apply_speed(float scale) {
  if (!g_probe_cs_ready) return false;
  EnterCriticalSection(&g_probe_cs);
  const bool ok = apply_speed_locked(scale);
  LeaveCriticalSection(&g_probe_cs);
  return ok;
}

void restore_speed() {
  if (!g_probe_cs_ready) return;
  EnterCriticalSection(&g_probe_cs);
  restore_speed_locked();
  LeaveCriticalSection(&g_probe_cs);
}

int speed_target() {
  if (!g_probe_cs_ready) return 0;
  EnterCriticalSection(&g_probe_cs);
  const int t = g_speed_target;
  LeaveCriticalSection(&g_probe_cs);
  return t;
}

void set_speed_target(int target) {
  if (!g_probe_cs_ready) return;
  if (target < 0 || target > 3) return;
  EnterCriticalSection(&g_probe_cs);
  const bool changed = (g_speed_target != target);
  g_speed_target = target;
  LeaveCriticalSection(&g_probe_cs);
  if (!changed) return;
  if (target == 0) {
    restore_speed();
    game_api::log("time: speed target -> OFF (memory restored)");
    return;
  }
  // 换目标前先把上一目标的写入还原，避免两个字段互相污染。
  restore_speed();
  game_api::log("time: speed target -> %d", target);
}

}  // namespace

namespace game_api {

void time_control_install() {
  if (!g_tc_cs_ready) {
    InitializeCriticalSection(&g_tc_cs);
    g_tc_cs_ready = true;
  }
  if (!g_probe_cs_ready) {
    InitializeCriticalSection(&g_probe_cs);
    g_probe_cs_ready = true;
  }
  g_enabled = false;
  g_scale = 1.0f;
  g_speed_have_orig = false;
  g_speed_applied = 0.f;
  g_speed_writes = 0;
  g_probe_next_log = 0;
  log("time: speed control ready. knobs=[0x%X] logicFps / [0x%X] updateFps; "
      "target=%d (0=off 1=logic 2=update 3=both), x1..x%.0f. "
      "0x%X/+0x%X (Brightness) are no longer touched.",
      kLogicFpsVa, kUpdateFpsVa, g_speed_target, kSpeedScaleMax, kSetSpeedPctVa, kOffTimeScale);
}

// 由 Present 每帧调用。
void time_control_tick() {
  if (!g_tc_cs_ready) return;

  uint32_t obj = 0;
  float scale = 0.f, factor = 0.f;
  const bool valid = read_locked(&obj, &scale, &factor);
  float clock = 0.f;
  const bool in_battle = game_api::battle_clock(&clock);
  // 去抖：真正退出对局 500ms 后才算 leaving（见 battle_left_generation 的注释）。
  static uint32_t s_seen_left = 0;
  const uint32_t left_gen = game_api::battle_left_generation();
  const bool leaving = (left_gen != s_seen_left);
  s_seen_left = left_gen;

  // 退出战局：停写、取消快进，并把 updateFps 写回进局原值。
  if (leaving) {
    EnterCriticalSection(&g_tc_cs);
    g_ff_active = false;
    g_enabled = false;
    g_scale = 1.0f;
    LeaveCriticalSection(&g_tc_cs);
    restore_speed();
    log("time: battle left — updateFps restored");
  }
  if (in_battle) probe_log(false);

  bool want_write = false;
  float target = 1.0f;
  EnterCriticalSection(&g_tc_cs);
  // 快进优先于普通维持：到达目标时间后停止加速（回到 1x）。
  if (in_battle && g_ff_active) {
    if (clock >= g_ff_target) {
      g_ff_active = false;
      g_ff_arrived = true;
      g_enabled = false;
      g_scale = 1.0f;
      target = 1.0f;  // 不能暂停（会除零），到达后回到常速
      want_write = true;
    } else {
      g_scale = g_ff_speed;
      g_enabled = true;
      target = g_ff_speed;
      want_write = true;
    }
  } else if (in_battle && g_enabled) {
    target = g_scale;
    want_write = true;
  } else if (!in_battle) {
    g_ff_active = false;
  }
  g_obj = obj;
  g_seen_scale = scale;
  g_seen_factor = factor;
  g_seen_valid = valid;
  LeaveCriticalSection(&g_tc_cs);

  // 维持目标值：apply_speed 内部会在“值已是目标”时直接返回，不写、不刷日志。
  if (want_write && in_battle) {
    apply_speed(target);
  }

  log_diag(obj, scale, factor, valid);
}

bool time_control_read(float* scale_out, float* factor_out, bool* valid_out) {
  uint32_t obj = 0;
  float scale = 0.f, factor = 0.f;
  const bool valid = read_locked(&obj, &scale, &factor);
  if (scale_out) *scale_out = scale;
  if (factor_out) *factor_out = factor;
  if (valid_out) *valid_out = valid;
  return valid;
}

void time_control_set_enabled(bool on) {
  if (!g_tc_cs_ready) return;
  EnterCriticalSection(&g_tc_cs);
  g_enabled = on;
  const float s = g_scale;
  LeaveCriticalSection(&g_tc_cs);
  if (!on) {
    restore_speed();  // 关闭 = 写回进局原值
  } else if (battle_clock(nullptr)) {
    apply_speed(s);
  }
  log("time: control %s (scale=%.3f)", on ? "ON" : "OFF", s);
}

bool time_control_enabled() {
  if (!g_tc_cs_ready) return false;
  EnterCriticalSection(&g_tc_cs);
  const bool on = g_enabled;
  LeaveCriticalSection(&g_tc_cs);
  return on;
}

void time_control_set_scale(float scale) {
  if (!g_tc_cs_ready) return;
  if (!finite_f(scale)) return;
  if (scale < 1.0f) scale = 1.0f;  // 不支持减速
  if (scale > kSpeedScaleMax) scale = kSpeedScaleMax;
  EnterCriticalSection(&g_tc_cs);
  g_scale = scale;
  g_enabled = true;
  LeaveCriticalSection(&g_tc_cs);

  const bool in_battle = battle_clock(nullptr);
  if (in_battle) apply_speed(scale);
  log("time: scale -> %.3fx (battle=%d)", scale, in_battle ? 1 : 0);
}

float time_control_scale() {
  if (!g_tc_cs_ready) return 1.0f;
  EnterCriticalSection(&g_tc_cs);
  const float s = g_scale;
  LeaveCriticalSection(&g_tc_cs);
  return s;
}

// 恢复原始值并关闭控制。
void time_control_restore() {
  if (!g_tc_cs_ready) return;
  EnterCriticalSection(&g_tc_cs);
  g_enabled = false;
  g_scale = 1.0f;
  g_ff_active = false;
  LeaveCriticalSection(&g_tc_cs);

  restore_speed();
  log("time: restored to original updateFps, control off");
}

float time_control_seen_scale() {
  if (!g_tc_cs_ready) return 0.f;
  EnterCriticalSection(&g_tc_cs);
  const float s = g_seen_scale;
  LeaveCriticalSection(&g_tc_cs);
  return s;
}

float time_control_seen_factor() {
  if (!g_tc_cs_ready) return 0.f;
  EnterCriticalSection(&g_tc_cs);
  const float s = g_seen_factor;
  LeaveCriticalSection(&g_tc_cs);
  return s;
}

bool time_control_valid() {
  if (!g_tc_cs_ready) return false;
  EnterCriticalSection(&g_tc_cs);
  const bool v = g_seen_valid;
  LeaveCriticalSection(&g_tc_cs);
  return v;
}

// ---- 快进（对应 CameraBridge 的「快进功能」）---------------------------------
void time_control_ff_start(float target_seconds, float speed) {
  if (!g_tc_cs_ready) return;
  if (!finite_f(target_seconds)) return;
  if (!finite_f(speed)) speed = 4.0f;
  if (speed < 1.0f) speed = 1.0f;  // 低于 1x 不可用（会除零）
  if (speed > kSpeedScaleMax) speed = kSpeedScaleMax;
  if (target_seconds < 0.f) target_seconds = 0.f;
  EnterCriticalSection(&g_tc_cs);
  g_ff_active = true;
  g_ff_arrived = false;
  g_ff_target = target_seconds;
  g_ff_speed = speed;
  g_scale = speed;
  g_enabled = true;
  LeaveCriticalSection(&g_tc_cs);
  log("time: fast-forward -> %.1fs at %.2fx", target_seconds, speed);
}

void time_control_ff_cancel() {
  if (!g_tc_cs_ready) return;
  EnterCriticalSection(&g_tc_cs);
  g_ff_active = false;
  LeaveCriticalSection(&g_tc_cs);
  log("time: fast-forward cancelled");
}

bool time_control_ff_active() {
  if (!g_tc_cs_ready) return false;
  EnterCriticalSection(&g_tc_cs);
  const bool v = g_ff_active;
  LeaveCriticalSection(&g_tc_cs);
  return v;
}

float time_control_ff_target() {
  if (!g_tc_cs_ready) return 0.f;
  EnterCriticalSection(&g_tc_cs);
  const float v = g_ff_target;
  LeaveCriticalSection(&g_tc_cs);
  return v;
}

float time_control_ff_speed() {
  if (!g_tc_cs_ready) return 4.0f;
  EnterCriticalSection(&g_tc_cs);
  const float v = g_ff_speed;
  LeaveCriticalSection(&g_tc_cs);
  return v;
}

void time_control_set_ff_auto(bool on) {
  if (!g_tc_cs_ready) return;
  EnterCriticalSection(&g_tc_cs);
  g_ff_auto = on;
  if (on && !g_ff_active) g_ff_arrived = false;
  LeaveCriticalSection(&g_tc_cs);
}

bool time_control_ff_auto() {
  if (!g_tc_cs_ready) return false;
  EnterCriticalSection(&g_tc_cs);
  const bool v = g_ff_auto;
  LeaveCriticalSection(&g_tc_cs);
  return v;
}

// engine/hotkey 入口。handled=false 表示不是本模块的键。
bool time_control_engine_cmd(const char* key, bool* handled, std::string* out_msg) {
  if (handled) *handled = false;
  if (!key) return false;

  struct Preset {
    const char* key;
    float scale;
    const char* label;
  };
  static const Preset kPresets[] = {
      {"time_1x", 1.0f, u8"常速 1x"},
      {"time_fast", 2.0f, u8"加速 2x"},
      {"time_fast3", 3.0f, u8"加速 3x"},
      {"time_fast4", 4.0f, u8"加速 4x"},
      {"time_fast6", 6.0f, u8"加速 6x"},
      {"time_fast8", 8.0f, u8"加速 8x"},
  };
  for (const Preset& p : kPresets) {
    if (std::strcmp(key, p.key) == 0) {
      if (handled) *handled = true;
      time_control_set_scale(p.scale);
      if (out_msg) *out_msg = std::string(u8"时间控制：") + p.label;
      return true;
    }
  }
  if (std::strcmp(key, "time_restore") == 0) {
    if (handled) *handled = true;
    time_control_restore();
    if (out_msg) *out_msg = u8"时间控制：已恢复原始速度";
    return true;
  }
  if (std::strcmp(key, "time_ff_cancel") == 0) {
    if (handled) *handled = true;
    time_control_ff_cancel();
    if (out_msg) *out_msg = u8"时间控制：已取消快进";
    return true;
  }
  if (std::strcmp(key, "time_toggle") == 0) {
    if (handled) *handled = true;
    const bool on = !time_control_enabled();
    time_control_set_enabled(on);
    if (out_msg) *out_msg = on ? u8"时间控制：开" : u8"时间控制：关";
    return true;
  }
  return false;
}

// ---- 原始时钟诊断（只读）-------------------------------
void time_control_probe_snapshot(TimeProbe* out) {
  if (!out) return;
  // UI 每帧都会问一次，这里缓存 250ms（probe_read 只读内存，不调用游戏函数）。
  static TimeProbe s_cache{};
  static DWORD s_cache_tick = 0;
  const DWORD now = GetTickCount();
  if (s_cache_tick == 0 || (now - s_cache_tick) >= 250u) {
    const ProbeSnap s = probe_read();
    s_cache = TimeProbe{};
    s_cache.have_obj = s.have_obj;
    s_cache.game_speed_factor = s.gsf;
    s_cache.brightness = s.bright;
    s_cache.have_logic_fps = s.have_logic;
    s_cache.logic_fps = s.logic_fps;
    s_cache.have_update_fps = s.have_update;
    s_cache.update_fps = s.update_fps;
    s_cache.session = s.session;
    // 会话对象的 getter 不再从渲染线程调用（退局时会踩到半销毁对象）。
    s_cache.have_session_fps = false;
    s_cache.session_fps = 0;
    s_cache_tick = now;
  }
  EnterCriticalSection(&g_probe_cs);
  s_cache.applied_scale = g_speed_applied;
  s_cache.original_logic_fps = g_speed_have_orig ? g_orig_logic : 0;
  s_cache.original_update_fps = g_speed_have_orig ? g_orig_update : 0;
  s_cache.target = g_speed_target;
  s_cache.writes = g_speed_writes;
  LeaveCriticalSection(&g_probe_cs);
  *out = s_cache;
}

void time_control_probe_dump_session() {
  g_probe_session_dump = true;
  probe_log(true);
}

int time_control_speed_target() { return speed_target(); }

void time_control_set_speed_target(int target) { set_speed_target(target); }

}  // namespace game_api
