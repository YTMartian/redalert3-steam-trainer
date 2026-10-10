// ---------------------------------------------------------------------------
// 杂项操作（对应 CameraBridge 的「杂项操作」）：阴影贴图 + 光照颜色 + 隐藏标记
//
// 详细地址说明见 game_api.h 里「杂项操作」一节。要点：
//   0x00CE08F0  -> 渲染对象：+0x138 剪影开关（1 显示/0 隐藏）、+0x139 血条开关
//   0x00CE0D78  -> 光照配置；[[0xCE0D78]+0x24] 才是光照结构，
//                  环境光 +00/04/08、主光源 +0C/10/14、调色#1 +30/34/38、调色#2 +54/58/5C
//   0x00CEEDB4  int   阴影贴图大小（1024/2048/4096/8192）
//   0x00CEEDB8  float 最大阴影距离（默认 1000.0）
//
// 全部按「读前校验、写前再校验」的方式处理，避免重演相机接管的崩溃。
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

// 绝对 VA（Steam 1.12 / ra3_1.12.game）。RVA = VA - kModBase。
constexpr uint32_t kRenderObjPtrVa = 0x00CE08F0;  // -> 渲染对象
constexpr uint32_t kOffSilhouette = 0x138;        // byte 单位剪影
constexpr uint32_t kOffHealth = 0x139;            // byte 血条 + 维修标志

constexpr uint32_t kLightCfgPtrVa = 0x00CE0D78;   // -> 光照配置对象
constexpr uint32_t kOffLightStruct = 0x24;        // 光照结构
constexpr uint32_t kShadowSizeVa = 0x00CEEDB4;    // int
constexpr uint32_t kShadowDistVa = 0x00CEEDB8;    // float

// 光照 4 组，各自的结构内偏移（RGB 连续三个 float）。
constexpr uint32_t kLightOffs[4] = {0x00, 0x0C, 0x30, 0x54};
const char* const kLightNames[4] = {u8"环境光颜色", u8"主光源颜色", u8"调色颜色 #1",
                                    u8"调色颜色 #2"};

const int kShadowSizes[4] = {1024, 2048, 4096, 8192};

// 允许写入的光照分量范围（游戏里主光源可到 2.0，其它一般 0..1；留足余量）。
constexpr float kLightMin = -8.f;
constexpr float kLightMax = 16.f;
constexpr float kShadowDistMin = 0.f;
constexpr float kShadowDistMax = 200000.f;

CRITICAL_SECTION g_cs;
bool g_cs_ready = false;

// 用户意图：take_* 为 true 时每帧把目标值写回。
bool g_take_health = false;
bool g_hide_health = false;
bool g_take_silhouette = false;
bool g_hide_silhouette = false;

bool g_shadow_take = false;
int g_shadow_size = 0;            // 0 = 不下发
bool g_have_shadow_dist = false;
float g_shadow_dist = 0.f;

bool g_light_lock = true;         // 默认每帧保持
bool g_light_set[4] = {false, false, false, false};
float g_light_target[4][3] = {};

// 第一次读到的原始值，用于恢复。
bool g_orig_light_ok[4] = {false, false, false, false};
float g_orig_light[4][3] = {};
bool g_orig_captured = false;

int g_write_fail = 0;
uint32_t g_last_robj = 0;
uint32_t g_last_light = 0;
DWORD g_next_log = 0;

bool finite_f(float v) { return v == v && v > -3.4e38f && v < 3.4e38f; }
bool is_ptr(uint32_t v) { return v >= 0x10000u && v < 0x7FFF0000u && (v & 3u) == 0; }

uint32_t rd_u32(uint32_t a) { return *reinterpret_cast<uint32_t*>(a); }
uint8_t rd_u8(uint32_t a) { return *reinterpret_cast<uint8_t*>(a); }
int32_t rd_i32(uint32_t a) { return *reinterpret_cast<int32_t*>(a); }
float rd_f32(uint32_t a) { return *reinterpret_cast<float*>(a); }
void wr_u8(uint32_t a, uint8_t v) { *reinterpret_cast<uint8_t*>(a) = v; }
void wr_i32(uint32_t a, int32_t v) { *reinterpret_cast<int32_t*>(a) = v; }
void wr_f32(uint32_t a, float v) { *reinterpret_cast<float*>(a) = v; }

// 解析渲染对象；失败返回 0。
uint32_t render_obj() {
  const uint32_t base = game_api::module_base();
  if (!base) return 0;
  uint32_t p = 0;
  __try {
    p = rd_u32(base + (kRenderObjPtrVa - kModBase));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    p = 0;
  }
  return is_ptr(p) ? p : 0;
}

// 解析光照结构；失败返回 0。
uint32_t light_struct() {
  const uint32_t base = game_api::module_base();
  if (!base) return 0;
  uint32_t cfg = 0, st = 0;
  __try {
    cfg = rd_u32(base + (kLightCfgPtrVa - kModBase));
    if (is_ptr(cfg)) st = rd_u32(cfg + kOffLightStruct);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    st = 0;
  }
  return is_ptr(st) ? st : 0;
}

// 四个分量是不是「像光照」的值。
bool light_plausible(const float* v3) {
  for (int i = 0; i < 3; ++i) {
    if (!finite_f(v3[i])) return false;
    if (v3[i] < kLightMin || v3[i] > kLightMax) return false;
  }
  return true;
}

void log_diag(uint32_t robj, uint32_t lst, const game_api::MiscState& s) {
  const DWORD now = GetTickCount();
  if (g_next_log && (int32_t)(now - g_next_log) < 0) return;
  g_next_log = now + 3000;
  game_api::log(
      "misc: diag robj=0x%X flags=%d hp=%d sil=%d shadow=%d dist=%.1f light=0x%X ok=%d "
      "fails=%d",
      robj, s.flags_ok ? 1 : 0, s.show_health ? 1 : 0, s.show_silhouette ? 1 : 0,
      s.shadow_size_ok ? s.shadow_size : -1, s.shadow_dist_ok ? s.shadow_dist : -1.f, lst,
      s.light.ok ? 1 : 0, g_write_fail);
}

}  // namespace

namespace game_api {

void misc_read(MiscState* out) {
  if (!out) return;
  MiscState s;

  const uint32_t robj = render_obj();
  g_last_robj = robj;
  if (robj) {
    uint8_t sil = 1, hp = 1;
    bool ok = false;
    __try {
      sil = rd_u8(robj + kOffSilhouette);
      hp = rd_u8(robj + kOffHealth);
      ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (ok) {
      s.flags_ok = true;
      // 只认 0 / 1；出现别的值说明地址判断有误，按「显示」处理。
      s.show_silhouette = (sil == 0) ? false : true;
      s.show_health = (hp == 0) ? false : true;
    }
  }

  const uint32_t base = module_base();
  if (base) {
    int32_t sz = 0;
    float d = 0.f;
    bool ok = false;
    __try {
      sz = rd_i32(base + (kShadowSizeVa - kModBase));
      d = rd_f32(base + (kShadowDistVa - kModBase));
      ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (ok) {
      for (int v : kShadowSizes) {
        if (sz == v) {
          s.shadow_size_ok = true;
          s.shadow_size = sz;
          break;
        }
      }
      if (finite_f(d) && d >= kShadowDistMin && d <= kShadowDistMax) {
        s.shadow_dist_ok = true;
        s.shadow_dist = d;
      }
    }
  }

  const uint32_t lst = light_struct();
  g_last_light = lst;
  if (lst) {
    float v[12] = {};
    bool ok = false;
    __try {
      for (int g = 0; g < 4; ++g) {
        for (int c = 0; c < 3; ++c) {
          v[g * 3 + c] = rd_f32(lst + kLightOffs[g] + c * 4);
        }
      }
      ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (ok) {
      bool all_ok = true;
      for (int g = 0; g < 4; ++g) all_ok = all_ok && light_plausible(&v[g * 3]);
      if (all_ok) {
        s.light.ok = true;
        for (int g = 0; g < 4; ++g) {
          for (int c = 0; c < 3; ++c) {
            if (g == 0) s.light.ambient[c] = v[c];
            else if (g == 1) s.light.main[c] = v[3 + c];
            else if (g == 2) s.light.tint1[c] = v[6 + c];
            else s.light.tint2[c] = v[9 + c];
          }
        }
      }
    }
  }

  // 第一次成功读到光照时记下原始值。
  if (s.light.ok && !g_orig_captured && g_cs_ready) {
    EnterCriticalSection(&g_cs);
    if (!g_orig_captured) {
      const float* src[4] = {s.light.ambient, s.light.main, s.light.tint1, s.light.tint2};
      for (int g = 0; g < 4; ++g) {
        for (int c = 0; c < 3; ++c) g_orig_light[g][c] = src[g][c];
        g_orig_light_ok[g] = true;
      }
      g_orig_captured = true;
    }
    LeaveCriticalSection(&g_cs);
  }

  log_diag(robj, lst, s);
  *out = s;
}

void misc_tick() {
  if (!g_cs_ready) return;

  // 先把需要的目标值取出来（不要在 __try 里拿锁）。
  bool want_hp = false, hp_val = false;
  bool want_sil = false, sil_val = false;
  bool want_size = false;
  int size_val = 0;
  bool want_dist = false;
  float dist_val = 0.f;
  bool want_light[4] = {false, false, false, false};
  float light_val[4][3] = {};

  EnterCriticalSection(&g_cs);
  want_hp = g_take_health;
  hp_val = !g_hide_health;
  want_sil = g_take_silhouette;
  sil_val = !g_hide_silhouette;
  want_size = g_shadow_take && g_shadow_size != 0;
  size_val = g_shadow_size;
  want_dist = g_shadow_take && g_have_shadow_dist;
  dist_val = g_shadow_dist;
  if (g_light_lock) {
    for (int g = 0; g < 4; ++g) {
      want_light[g] = g_light_set[g];
      for (int c = 0; c < 3; ++c) light_val[g][c] = g_light_target[g][c];
    }
  }
  LeaveCriticalSection(&g_cs);

  const uint32_t robj = render_obj();
  if (robj && (want_hp || want_sil)) {
    __try {
      if (want_hp) {
        const uint8_t cur = rd_u8(robj + kOffHealth);
        const uint8_t want = hp_val ? 1 : 0;
        if (cur != want) wr_u8(robj + kOffHealth, want);
      }
      if (want_sil) {
        const uint8_t cur = rd_u8(robj + kOffSilhouette);
        const uint8_t want = sil_val ? 1 : 0;
        if (cur != want) wr_u8(robj + kOffSilhouette, want);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_write_fail++;
    }
  }

  const uint32_t base = module_base();
  if (base && (want_size || want_dist)) {
    __try {
      if (want_size) {
        const int32_t cur = rd_i32(base + (kShadowSizeVa - kModBase));
        if (cur != size_val) wr_i32(base + (kShadowSizeVa - kModBase), size_val);
      }
      if (want_dist) {
        const float cur = rd_f32(base + (kShadowDistVa - kModBase));
        const float d = cur - dist_val;
        if (!(d > -0.01f && d < 0.01f)) wr_f32(base + (kShadowDistVa - kModBase), dist_val);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_write_fail++;
    }
  }

  bool any_light = false;
  for (int g = 0; g < 4; ++g) any_light = any_light || want_light[g];
  if (any_light) {
    const uint32_t lst = light_struct();
    if (lst) {
      __try {
        for (int g = 0; g < 4; ++g) {
          if (!want_light[g]) continue;
          for (int c = 0; c < 3; ++c) {
            const float cur = rd_f32(lst + kLightOffs[g] + c * 4);
            const float d = cur - light_val[g][c];
            if (!(d > -0.0005f && d < 0.0005f)) {
              wr_f32(lst + kLightOffs[g] + c * 4, light_val[g][c]);
            }
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_write_fail++;
      }
    }
  }
}

bool misc_set_hide_health(bool hide, bool take_over, std::string* out_msg) {
  EnterCriticalSection(&g_cs);
  g_take_health = take_over;
  g_hide_health = hide;
  LeaveCriticalSection(&g_cs);
  // 立即写一次，不必等下一帧。
  misc_tick();
  if (out_msg) {
    *out_msg = hide ? u8"杂项：已隐藏血条与维修标志" : u8"杂项：已恢复血条与维修标志";
  }
  log("misc: hide_health=%d take=%d", hide ? 1 : 0, take_over ? 1 : 0);
  return true;
}

bool misc_set_hide_silhouette(bool hide, bool take_over, std::string* out_msg) {
  EnterCriticalSection(&g_cs);
  g_take_silhouette = take_over;
  g_hide_silhouette = hide;
  LeaveCriticalSection(&g_cs);
  misc_tick();
  if (out_msg) {
    *out_msg = hide ? u8"杂项：已隐藏建筑背后的单位剪影" : u8"杂项：已恢复单位剪影";
  }
  log("misc: hide_silhouette=%d take=%d", hide ? 1 : 0, take_over ? 1 : 0);
  return true;
}

bool misc_set_shadow_size(int size, std::string* out_msg) {
  bool valid = false;
  for (int v : kShadowSizes) {
    if (size == v) valid = true;
  }
  if (!valid) {
    if (out_msg) *out_msg = u8"杂项：阴影贴图大小只能是 1024 / 2048 / 4096 / 8192";
    return false;
  }
  const uint32_t base = module_base();
  if (!base) {
    if (out_msg) *out_msg = u8"杂项：未找到零售版 1.12 模块，阴影贴图不可用";
    return false;
  }
  int32_t cur = 0;
  bool ok = false;
  __try {
    cur = rd_i32(base + (kShadowSizeVa - kModBase));
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (!ok) {
    if (out_msg) *out_msg = u8"杂项：读不到阴影贴图地址，已放弃";
    return false;
  }
  EnterCriticalSection(&g_cs);
  g_shadow_take = true;
  g_shadow_size = size;
  LeaveCriticalSection(&g_cs);
  if (cur != size) {
    __try {
      wr_i32(base + (kShadowSizeVa - kModBase), size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (out_msg) *out_msg = u8"杂项：写入阴影贴图大小失败";
      return false;
    }
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), u8"杂项：阴影贴图大小 = %d（重新开始一局后生效）", size);
  if (out_msg) *out_msg = buf;
  log("misc: shadow_size %d -> %d", (int)cur, size);
  return true;
}

bool misc_set_shadow_distance(float dist, std::string* out_msg) {
  if (!finite_f(dist) || dist < kShadowDistMin || dist > kShadowDistMax) {
    if (out_msg) *out_msg = u8"杂项：最大阴影距离超出范围";
    return false;
  }
  const uint32_t base = module_base();
  if (!base) {
    if (out_msg) *out_msg = u8"杂项：未找到零售版 1.12 模块，阴影距离不可用";
    return false;
  }
  float cur = 0.f;
  bool ok = false;
  __try {
    cur = rd_f32(base + (kShadowDistVa - kModBase));
    ok = finite_f(cur) && cur >= 0.f && cur <= 1000000.f;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (!ok) {
    if (out_msg) *out_msg = u8"杂项：当前阴影距离不像正常值，已放弃写入";
    return false;
  }
  EnterCriticalSection(&g_cs);
  g_shadow_take = true;
  g_have_shadow_dist = true;
  g_shadow_dist = dist;
  LeaveCriticalSection(&g_cs);
  __try {
    wr_f32(base + (kShadowDistVa - kModBase), dist);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (out_msg) *out_msg = u8"杂项：写入最大阴影距离失败";
    return false;
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), u8"杂项：最大阴影距离 = %.1f（原 %.1f）", dist, cur);
  if (out_msg) *out_msg = buf;
  log("misc: shadow_dist %.1f -> %.1f", cur, dist);
  return true;
}

void misc_set_shadow_takeover(bool on) {
  EnterCriticalSection(&g_cs);
  g_shadow_take = on;
  LeaveCriticalSection(&g_cs);
}

bool misc_set_light(int which, const float rgb[3], std::string* out_msg) {
  if (which < 0 || which > 3 || !rgb) return false;
  if (!light_plausible(rgb)) {
    if (out_msg) *out_msg = u8"杂项：光照颜色超出允许范围";
    return false;
  }
  const uint32_t lst = light_struct();
  if (!lst) {
    if (out_msg) *out_msg = u8"杂项：还没读到光照结构（进入对局后可用）";
    return false;
  }
  float cur[3] = {};
  bool ok = false;
  __try {
    for (int c = 0; c < 3; ++c) cur[c] = rd_f32(lst + kLightOffs[which] + c * 4);
    ok = light_plausible(cur);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (!ok) {
    if (out_msg) *out_msg = u8"杂项：当前光照值不像正常值，已放弃写入";
    return false;
  }
  EnterCriticalSection(&g_cs);
  g_light_set[which] = true;
  for (int c = 0; c < 3; ++c) g_light_target[which][c] = rgb[c];
  LeaveCriticalSection(&g_cs);
  __try {
    for (int c = 0; c < 3; ++c) wr_f32(lst + kLightOffs[which] + c * 4, rgb[c]);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (out_msg) *out_msg = u8"杂项：写入光照颜色失败";
    return false;
  }
  if (out_msg) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), u8"杂项：%s = (%.3f, %.3f, %.3f)", kLightNames[which],
                  rgb[0], rgb[1], rgb[2]);
    *out_msg = buf;
  }
  return true;
}

void misc_set_light_lock(bool on) {
  EnterCriticalSection(&g_cs);
  g_light_lock = on;
  LeaveCriticalSection(&g_cs);
  log("misc: light lock %s", on ? "ON" : "OFF");
}

bool misc_light_lock() {
  EnterCriticalSection(&g_cs);
  const bool on = g_light_lock;
  LeaveCriticalSection(&g_cs);
  return on;
}

void misc_restore_lights() {
  if (!g_cs_ready) return;
  float restore[4][3] = {};
  bool have[4] = {false, false, false, false};
  EnterCriticalSection(&g_cs);
  for (int g = 0; g < 4; ++g) {
    have[g] = g_orig_light_ok[g];
    for (int c = 0; c < 3; ++c) restore[g][c] = g_orig_light[g][c];
    g_light_set[g] = false;
  }
  g_light_lock = false;
  LeaveCriticalSection(&g_cs);

  const uint32_t lst = light_struct();
  if (lst) {
    __try {
      for (int g = 0; g < 4; ++g) {
        if (!have[g]) continue;
        for (int c = 0; c < 3; ++c) wr_f32(lst + kLightOffs[g] + c * 4, restore[g][c]);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  log("misc: lights restored");
}

// 安装（init 时调用一次）。
void misc_install() {
  if (!g_cs_ready) {
    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
  }
  log("misc: ops ready (robj@0x%X light@0x%X shadow@0x%X/0x%X)", kRenderObjPtrVa,
      kLightCfgPtrVa, kShadowSizeVa, kShadowDistVa);
}

bool misc_engine_cmd(const char* key, bool* handled, std::string* out_msg) {
  if (handled) *handled = false;
  if (!key) return false;
  struct Cmd {
    const char* key;
    bool hide;
  };
  static const Cmd kHealthCmds[] = {
      {"misc_health_hide", true},
      {"misc_health_show", false},
  };
  for (const Cmd& c : kHealthCmds) {
    if (std::strcmp(key, c.key) == 0) {
      if (handled) *handled = true;
      misc_set_hide_health(c.hide, true, out_msg);
      return true;
    }
  }
  static const Cmd kSilCmds[] = {
      {"misc_sil_hide", true},
      {"misc_sil_show", false},
  };
  for (const Cmd& c : kSilCmds) {
    if (std::strcmp(key, c.key) == 0) {
      if (handled) *handled = true;
      misc_set_hide_silhouette(c.hide, true, out_msg);
      return true;
    }
  }
  if (std::strcmp(key, "misc_lights_restore") == 0) {
    if (handled) *handled = true;
    misc_restore_lights();
    if (out_msg) *out_msg = u8"杂项：光照颜色已恢复默认";
    return true;
  }
  return false;
}

}  // namespace game_api
