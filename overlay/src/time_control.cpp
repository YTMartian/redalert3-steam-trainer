// ---------------------------------------------------------------------------
// 游戏时间控制（对应 CameraBridge 的 RA3TimePausePlugin）
//
// 反汇编结论（ra3_1.12.game / 零售版）：
//   全局指针 0x00CE8138 -> 全局数据对象（TheGlobalData 一类的大单例）。
//   该对象：
//     +0x30  float  GameSpeedFactor（调试面板里就叫这个名字）
//     +0x54  float  游戏时间倍率。0.6 = 最慢，1.0 = 正常，2.0 = 最快。
//   设置函数 0x6F9230 区域：先把「0..100 的百分比」（50 = 正常）换算成
//     scale = 1.0 + (pct-50) * 0.02   (pct>50)
//     scale = 1.0 - (50-pct) * 0.008  (pct<50)
//   然后写回 [[0xCE8138]+0x54]。
//   所以 +0x54 才是真正的时间倍率，改它即可控制游戏速度（0 相当于暂停）。
//
// 安全性：只写一个 4 字节 float，且写前校验：
//   1) 指针 [[0xCE8138]] 是合法模块内/堆地址；
//   2) 当前 +0x54 必须是有限浮点数；
//   3) 目标倍率在我们允许的范围内。
//   任何一条不满足就拒绝写入并记录日志，绝不盲写（避免重演相机接管崩溃）。
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
constexpr uint32_t kGlobalDataPtrVa = 0x00CE8138;  // -> 全局数据对象
constexpr uint32_t kOffSpeedFactor = 0x30;         // GameSpeedFactor（只读展示）
constexpr uint32_t kOffTimeScale = 0x54;           // 游戏时间倍率

// 允许写入的倍率范围。低于 0.01 视为「暂停游戏逻辑」。
constexpr float kScaleMin = 0.0f;
constexpr float kScaleMax = 6.0f;

CRITICAL_SECTION g_tc_cs;
bool g_tc_cs_ready = false;

bool g_enabled = false;        // 是否持续维持 g_scale
float g_scale = 1.0f;          // 期望的时间倍率
bool g_have_orig = false;
float g_orig_scale = 1.0f;     // 第一次见到的原始倍率（用于恢复）

float g_seen_scale = 0.f;      // 最近一次读到的 +0x54
float g_seen_factor = 0.f;     // 最近一次读到的 GameSpeedFactor
bool g_seen_valid = false;
uint32_t g_obj = 0;
DWORD g_next_log = 0;
int g_write_fail = 0;

// 快进（对应 CameraBridge 的「快进功能」）
bool g_ff_active = false;
bool g_ff_auto = false;
bool g_ff_arrived = false;
float g_ff_target = 0.f;   // 目标对局时间（秒）
float g_ff_speed = 4.0f;   // 快进时使用的倍率

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
  const DWORD now = GetTickCount();
  if (g_next_log && (int32_t)(now - g_next_log) < 0) return;
  g_next_log = now + 2000;
  game_api::log("time: diag obj=0x%X valid=%d scale=%.4f factor=%.4f want=%.4f en=%d fails=%d",
                obj, valid ? 1 : 0, scale, factor, g_scale, g_enabled ? 1 : 0, g_write_fail);
}

}  // namespace

namespace game_api {

void time_control_install() {
  if (!g_tc_cs_ready) {
    InitializeCriticalSection(&g_tc_cs);
    g_tc_cs_ready = true;
  }
  g_enabled = false;
  g_scale = 1.0f;
  g_have_orig = false;
  g_write_fail = 0;
  log("time: control ready (scale@+0x%X of [0x%X])", kOffTimeScale, kGlobalDataPtrVa);
}

// 由 Present 每帧调用。
void time_control_tick() {
  if (!g_tc_cs_ready) return;

  uint32_t obj = 0;
  float scale = 0.f, factor = 0.f;
  const bool valid = read_locked(&obj, &scale, &factor);

  bool want_write = false;
  float target = 1.0f;
  EnterCriticalSection(&g_tc_cs);
  if (valid && !g_have_orig) {
    g_orig_scale = scale;
    g_have_orig = true;
  }
  // 快进优先于普通持续覆盖：到达目标时间后自动停下时间。
  if (g_ff_active) {
    float now = 0.f;
    const bool have_clock = game_api::battle_clock(&now);
    if (!have_clock) {
      // 没有对局时钟（不在对局里）——取消，免得在主菜单里乱加速。
      g_ff_active = false;
    } else if (now >= g_ff_target) {
      g_ff_active = false;
      g_scale = 0.0f;  // 停下来（暂停游戏逻辑），方便接着拍摄
      g_enabled = true;
      target = 0.0f;
      want_write = true;
      g_ff_arrived = true;
    } else {
      g_scale = g_ff_speed;
      g_enabled = true;
      target = g_ff_speed;
      want_write = true;
    }
  } else if (valid && g_enabled) {
    target = g_scale;
    want_write = true;
  }
  g_obj = obj;
  g_seen_scale = scale;
  g_seen_factor = factor;
  g_seen_valid = valid;
  LeaveCriticalSection(&g_tc_cs);

  if (want_write && valid) {
    const bool ok_range = finite_f(target) && target >= kScaleMin && target <= kScaleMax;
    // 只有当前值看起来还像时间倍率时才敢写；否则说明地址判断有误，拒绝。
    const bool ok_cur = finite_f(scale) && scale >= 0.f && scale <= 100.f;
    if (ok_range && ok_cur) {
      __try {
        write_f32(obj + kOffTimeScale, target);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_write_fail++;
      }
    } else if (ok_range) {
      g_write_fail++;
    }
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
  LeaveCriticalSection(&g_tc_cs);
  log("time: control %s (scale=%.4f)", on ? "ON" : "OFF", g_scale);
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
  if (scale < kScaleMin) scale = kScaleMin;
  if (scale > kScaleMax) scale = kScaleMax;
  EnterCriticalSection(&g_tc_cs);
  g_scale = scale;
  g_enabled = true;
  LeaveCriticalSection(&g_tc_cs);
  log("time: scale -> %.4f", scale);
}

float time_control_scale() {
  if (!g_tc_cs_ready) return 1.0f;
  EnterCriticalSection(&g_tc_cs);
  const float s = g_scale;
  LeaveCriticalSection(&g_tc_cs);
  return s;
}

// 恢复原始倍率并关闭控制。
void time_control_restore() {
  if (!g_tc_cs_ready) return;
  float orig = 1.0f;
  bool have = false;
  EnterCriticalSection(&g_tc_cs);
  orig = g_have_orig ? g_orig_scale : 1.0f;
  have = g_have_orig;
  g_enabled = false;
  g_scale = orig;
  LeaveCriticalSection(&g_tc_cs);

  uint32_t obj = 0;
  float scale = 0.f, factor = 0.f;
  if (have && read_locked(&obj, &scale, &factor)) {
    const bool ok = finite_f(orig) && orig >= 0.f && orig <= 100.f;
    if (ok) {
      __try {
        write_f32(obj + kOffTimeScale, orig);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
  }
  log("time: restored to %.4f", orig);
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
  if (speed < 0.1f) speed = 0.1f;
  if (speed > kScaleMax) speed = kScaleMax;
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
      {"time_pause", 0.0f, u8"暂停游戏时间"},
      {"time_slow", 0.5f, u8"慢放 0.5x"},
      {"time_1x", 1.0f, u8"常速 1.0x"},
      {"time_fast", 2.0f, u8"快放 2.0x"},
      {"time_fast3", 3.0f, u8"快放 3.0x"},
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
    if (on) {
      time_control_set_scale(time_control_scale());
    } else {
      // 关闭时把当前倍率留在游戏里，只停止持续覆盖。
      EnterCriticalSection(&g_tc_cs);
      g_enabled = false;
      LeaveCriticalSection(&g_tc_cs);
    }
    if (out_msg) *out_msg = on ? u8"时间控制：开" : u8"时间控制：关";
    return true;
  }
  return false;
}

}  // namespace game_api
