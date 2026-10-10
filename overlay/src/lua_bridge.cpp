// ---------------------------------------------------------------------------
// Lua 桥 + 动画跟踪
//   对应 CameraBridgeRelease 的 Lanyi::RA3Lua / RA3LuaBridge 以及
//   RA3TimePausePlugin 里的动画（Animation）相关功能。
//
// 反汇编结论（ra3_1.12.game / 零售版 1.12，内嵌的是 SAGE/Corona 定制版 Lua）：
//
//   TValue（栈上一个值，16 字节）：
//     +0x00  int      tt     类型标签
//     +0x08  union    值      double / GCObject* / int
//   类型标签（由 0x40A570 lua_typename 的名字表 0xBE2388 读出）：
//     0 = userdata   1 = nil     2 = number   3 = string
//     4 = table      5 = function  6 = lightuserdata
//   string 的字符数据在 ((TString*)value) + 0x14。
//
//   lua_State 布局：
//     +0x00  StkId  top          （+1 个槽 = +0x10）
//     +0x08  StkId  stack_last
//     +0x10  StkId  base
//
//   C API（全部 __cdecl，参数 1 都是 lua_State*）：
//     0x40A3F0  int          lua_gettop(L)
//     0x40A540  int          lua_type(L, idx)
//     0x40A7B0  const char*  lua_tostring(L, idx)   // 数字会就地转成字符串
//     0x40A8D0  void         lua_pushnil(L)
//     0x40A930  void         lua_pushnumber(L, double)
//     0x40A9A0  void         lua_pushstring(L, const char*)
//     0x40AA70  void         lua_getglobal(L, name)
//     0x40ACC0  void         lua_setglobal(L, name)
//
//   lua_State* 没有直接放在全局变量里（脚本对象的 +0x24 才是），所以这里用
//   MinHook 挂钩上面几个 API，从第一个参数捕获 lua_State*（原版插件也是这么做的）。
//
// 安全策略（和 time_control 一样，宁可读不到也不乱写）：
//   1) 写之前先校验 top/base/stack_last 都像合法指针、top 16 字节对齐；
//   2) 全程 __try/__except 兜底，任何异常都只是本次读写失败；
//   3) 读完之后直接把 top 还原成调用前的值（等价于 Lua 的弹栈），
//      不依赖 lua_settop 在 idx>=0 时那条不确定的路径。
// ---------------------------------------------------------------------------
#include "game_api.h"
#include "game_api_internal.h"

#include <windows.h>
#include "MinHook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kModBase = 0x400000;

// ---- Lua C API（RVA） ------------------------------------------------------
constexpr uint32_t kOffGetTop = 0x40A3F0 - kModBase;
constexpr uint32_t kOffType = 0x40A540 - kModBase;
constexpr uint32_t kOffToString = 0x40A7B0 - kModBase;
constexpr uint32_t kOffPushNil = 0x40A8D0 - kModBase;
constexpr uint32_t kOffPushNumber = 0x40A930 - kModBase;
constexpr uint32_t kOffPushString = 0x40A9A0 - kModBase;
constexpr uint32_t kOffGetGlobal = 0x40AA70 - kModBase;
constexpr uint32_t kOffSetGlobal = 0x40ACC0 - kModBase;

// ---- 类型标签 --------------------------------------------------------------
constexpr int kTyUserdata = 0;
constexpr int kTyNil = 1;
constexpr int kTyNumber = 2;
constexpr int kTyString = 3;
constexpr int kTyTable = 4;
constexpr int kTyFunction = 5;
constexpr int kTyLightUserdata = 6;

using fn_gettop = int(__cdecl*)(void*);
using fn_type = int(__cdecl*)(void*, int);
using fn_tostring = const char*(__cdecl*)(void*, int);
using fn_pushnil = void(__cdecl*)(void*);
using fn_pushnumber = void(__cdecl*)(void*, double);
using fn_pushstring = void(__cdecl*)(void*, const char*);
using fn_getglobal = void(__cdecl*)(void*, const char*);
using fn_setglobal = void(__cdecl*)(void*, const char*);

fn_gettop g_gettop = nullptr;
fn_type g_type = nullptr;
fn_tostring g_tostring = nullptr;
fn_pushnil g_pushnil = nullptr;
fn_pushnumber g_pushnumber = nullptr;
fn_pushstring g_pushstring = nullptr;
fn_getglobal g_getglobal = nullptr;
fn_setglobal g_setglobal = nullptr;

// 捕获到的 lua_State*。多个状态同时存在时用候选计数避免来回抖动。
volatile LONG g_lua = 0;
volatile LONG g_cand = 0;
volatile LONG g_cand_hits = 0;
volatile LONG g_cap_hits = 0;
bool g_announced = false;

// 安装状态：-1 = 未装好，1 = 已装好。
int g_hook_state = -1;
DWORD g_next_install = 0;

// 动画信息缓存（4Hz 刷新），避免 UI 每帧都去动 Lua 栈。
CRITICAL_SECTION g_cs;
bool g_cs_ready = false;
game_api::LuaAnimInfo g_anim{};
DWORD g_anim_next = 0;

constexpr int kMaxName = 200;

// 捕获候选（在 hook 里跑，只做最轻的事）。
inline void capture_lua(void* L) {
  ++g_cap_hits;  // 非原子计数，只做「桥是否在工作」的粗略指标
  const uint32_t v = reinterpret_cast<uint32_t>(L);
  if (v < 0x10000u || v > 0x7FFF0000u) return;
  if ((v & 3u) != 0) return;
  const LONG cur = g_lua;
  if (cur == static_cast<LONG>(v)) return;
  if (cur == 0) {
    InterlockedCompareExchange(&g_lua, static_cast<LONG>(v), 0);
    return;
  }
  // 已有一个状态；只有连续见到同一个新地址才认为状态被重建了。
  const LONG cand = g_cand;
  if (cand == static_cast<LONG>(v)) {
    if (++g_cand_hits >= 64) {
      InterlockedExchange(&g_lua, static_cast<LONG>(v));
      g_cand_hits = 0;
      g_announced = false;
    }
  } else {
    InterlockedExchange(&g_cand, static_cast<LONG>(v));
    g_cand_hits = 1;
  }
}

// ---- detour ---------------------------------------------------------------
int __cdecl hk_gettop(void* L) {
  capture_lua(L);
  return g_gettop ? g_gettop(L) : 0;
}

int __cdecl hk_type(void* L, int idx) {
  capture_lua(L);
  return g_type ? g_type(L, idx) : -1;
}

const char* __cdecl hk_tostring(void* L, int idx) {
  capture_lua(L);
  return g_tostring ? g_tostring(L, idx) : nullptr;
}

void __cdecl hk_pushnumber(void* L, double v) {
  capture_lua(L);
  if (g_pushnumber) g_pushnumber(L, v);
}

void __cdecl hk_pushstring(void* L, const char* s) {
  capture_lua(L);
  if (g_pushstring) g_pushstring(L, s);
}

void __cdecl hk_getglobal(void* L, const char* name) {
  capture_lua(L);
  if (g_getglobal) g_getglobal(L, name);
}

void __cdecl hk_setglobal(void* L, const char* name) {
  capture_lua(L);
  if (g_setglobal) g_setglobal(L, name);
}

// ---- 基础工具 --------------------------------------------------------------
bool is_ptr(uint32_t v) { return v >= 0x10000u && v < 0x7FFF0000u && (v & 3u) == 0; }

void* current_lua() {
  const LONG v = g_lua;
  return v ? reinterpret_cast<void*>(static_cast<uint32_t>(v)) : nullptr;
}

// lua_State 的三个栈指针是否都可信。
bool stack_sane(void* L) {
  uint32_t top = 0, last = 0, base = 0;
  __try {
    const uint8_t* p = static_cast<const uint8_t*>(L);
    top = *reinterpret_cast<const uint32_t*>(p);
    last = *reinterpret_cast<const uint32_t*>(p + 8);
    base = *reinterpret_cast<const uint32_t*>(p + 0x10);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (!is_ptr(top) || !is_ptr(last) || !is_ptr(base)) return false;
  if (top < base || last < top) return false;
  if ((top & 0xFu) != 0) return false;
  if ((top - base) > (1u << 20)) return false;  // 65536 个槽，明显不合理
  // 还必须有空槽：Lua 只有在 top == stack_last 时才会扩栈，
  // 扩栈会搬动整块栈内存，可能让游戏侧持有的 StkId 失效，所以宁可不读。
  if ((last - top) < 2u * 16u) return false;
  return true;
}

// 弹掉栈顶一个值。用当前的 L->top 计算，所以即使 Lua 自己扩过栈也算对。
void pop_one(void* L) {
  __try {
    uint8_t* p = static_cast<uint8_t*>(L);
    const uint32_t top = *reinterpret_cast<uint32_t*>(p);
    const uint32_t base = *reinterpret_cast<uint32_t*>(p + 0x10);
    if (top >= base + 16u) *reinterpret_cast<uint32_t*>(p) = top - 16u;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// 把 Lua 的 double 转成可显示文本（Lua 的 %g 语义）。
void fmt_number(double v, char* out, size_t len) {
  const double a = v < 0 ? -v : v;
  if (v == v && a != 0.0 && (a >= 1e10 || a < 1e-4)) {
    std::snprintf(out, len, "%.6g", v);
  } else if (v == (double)(long long)v && a < 1e15) {
    std::snprintf(out, len, "%lld", (long long)v);
  } else {
    std::snprintf(out, len, "%.6g", v);
  }
}

// ---- 安装 ----------------------------------------------------------------
struct HookSpec {
  uint32_t rva;
  void* detour;
  void** trampoline;
  const char* name;
};

bool install_one(const HookSpec& spec, uint32_t base, const char*& failed) {
  void* target = reinterpret_cast<void*>(base + spec.rva);
  const MH_STATUS st = MH_CreateHook(target, spec.detour, spec.trampoline);
  if (st != MH_OK) {
    failed = spec.name;
    return false;
  }
  if (MH_EnableHook(target) != MH_OK) {
    failed = spec.name;
    return false;
  }
  return true;
}

void install_locked() {
  if (g_hook_state == 1) return;
  const uint32_t base = game_api::module_base();
  if (!base) return;  // 起义时刻或还没注入
  const DWORD now = GetTickCount();
  if (g_next_install && static_cast<int32_t>(now - g_next_install) < 0) return;
  g_next_install = now + 3000;

  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
    game_api::log("lua: MH_Initialize failed %d", static_cast<int>(init));
    return;
  }

  const HookSpec specs[] = {
      {kOffGetGlobal, reinterpret_cast<void*>(&hk_getglobal),
       reinterpret_cast<void**>(&g_getglobal), "lua_getglobal"},
      {kOffSetGlobal, reinterpret_cast<void*>(&hk_setglobal),
       reinterpret_cast<void**>(&g_setglobal), "lua_setglobal"},
      {kOffPushString, reinterpret_cast<void*>(&hk_pushstring),
       reinterpret_cast<void**>(&g_pushstring), "lua_pushstring"},
      {kOffGetTop, reinterpret_cast<void*>(&hk_gettop), reinterpret_cast<void**>(&g_gettop),
       "lua_gettop"},
      {kOffType, reinterpret_cast<void*>(&hk_type), reinterpret_cast<void**>(&g_type),
       "lua_type"},
      {kOffToString, reinterpret_cast<void*>(&hk_tostring),
       reinterpret_cast<void**>(&g_tostring), "lua_tostring"},
      {kOffPushNumber, reinterpret_cast<void*>(&hk_pushnumber),
       reinterpret_cast<void**>(&g_pushnumber), "lua_pushnumber"},
  };

  int armed = 0;
  for (const HookSpec& s : specs) {
    const char* failed = nullptr;
    if (install_one(s, base, failed)) {
      ++armed;
    } else {
      game_api::log("lua: hook %s failed (%s)", failed ? failed : "?", "0x40xxxx");
    }
  }
  if (armed == 0) {
    game_api::log("lua: no hook armed");
    return;
  }
  g_hook_state = 1;
  game_api::log("lua: bridge armed (%d/%d hooks), L captured via first API call",
                armed, static_cast<int>(sizeof(specs) / sizeof(specs[0])));
}

// ---- 动画探针 --------------------------------------------------------------
struct ProbeSlot {
  const char* name;
  const char* label;
};

const ProbeSlot kAnimProbes[] = {
    {"AnimationLuaGetCurrentGameObjectAddress", u8"当前动画对象地址"},
    {"AnimationLuaGetCurrentGameObjectId", u8"当前动画对象 ID"},
    {"willSwitchAnimation", u8"willSwitchAnimation"},
    {"willSwitchPause", u8"willSwitchPause"},
    {"gameTime", u8"gameTime"},
    {"relativeTime", u8"relativeTime"},
};

void refresh_anim_locked() {
  game_api::LuaAnimInfo info;
  game_api::lua_global_read(kAnimProbes[0].name, &info.game_object_address);
  game_api::lua_global_read(kAnimProbes[1].name, &info.game_object_id);
  game_api::lua_global_read(kAnimProbes[2].name, &info.will_switch_animation);
  game_api::lua_global_read(kAnimProbes[3].name, &info.will_switch_pause);
  game_api::lua_global_read(kAnimProbes[4].name, &info.game_time);
  game_api::lua_global_read(kAnimProbes[5].name, &info.relative_time);
  info.valid = true;
  EnterCriticalSection(&g_cs);
  g_anim = info;
  LeaveCriticalSection(&g_cs);
}

DWORD g_next_diag = 0;

// 诊断用：试试能不能绕过 hook，直接从全局对象拿到 lua_State*。
// 0x00CE231C 是脚本管理器单例；+0x128 是它持有的脚本对象（引用计数智能指针）；
// 脚本对象 +0x24 才是 lua_State*（由 0x5AFCxx / 0x5E18xx 等函数里的 [esi+0x24] 推出来）。
struct PathProbe {
  uint32_t mgr = 0;
  uint32_t obj = 0;
  uint32_t state = 0;
  bool state_sane = false;
};

PathProbe probe_paths() {
  PathProbe p;
  const uint32_t base = game_api::module_base();
  if (!base) return p;
  __try {
    p.mgr = *reinterpret_cast<const uint32_t*>(base + (0x00CE231C - kModBase));
    if (is_ptr(p.mgr)) {
      p.obj = *reinterpret_cast<const uint32_t*>(p.mgr + 0x128);
      if (is_ptr(p.obj)) p.state = *reinterpret_cast<const uint32_t*>(p.obj + 0x24);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    p.mgr = 0;
    p.obj = 0;
    p.state = 0;
  }
  if (is_ptr(p.state) && p.state != static_cast<uint32_t>(g_lua)) {
    p.state_sane = stack_sane(reinterpret_cast<void*>(p.state));
  }
  return p;
}

}  // namespace

namespace game_api {

// ---- 对外接口 --------------------------------------------------------------

void lua_bridge_install() {
  if (!g_cs_ready) {
    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
  }
  g_anim = LuaAnimInfo{};
  install_locked();
  log("lua: bridge ready (retail 1.12 only)");
}

void lua_bridge_tick() {
  install_locked();

  const DWORD now = GetTickCount();
  const bool have_lua = current_lua() != nullptr;

  if (have_lua) {
    if (!g_announced) {
      g_announced = true;
      log("lua: captured lua_State* = 0x%X (api hits=%d)", static_cast<uint32_t>(g_lua),
          static_cast<int>(g_cap_hits));
    }
    if (!g_anim_next || static_cast<int32_t>(now - g_anim_next) >= 0) {
      g_anim_next = now + 250;  // 4Hz
      refresh_anim_locked();
    }
  }

  if (g_next_diag && static_cast<int32_t>(now - g_next_diag) < 0) return;
  g_next_diag = now + 5000;

  const PathProbe pp = probe_paths();
  if (have_lua) {
    LuaAnimInfo info{};
    lua_anim_read(&info);
    log("lua: diag L=0x%X apiHits=%d | animAddr=%s animId=%s switchAnim=%s switchPause=%s"
        " | path[0xCE231C]=0x%X +0x128=0x%X +0x24=0x%X sane=%d",
        static_cast<uint32_t>(g_lua), static_cast<int>(g_cap_hits),
        info.game_object_address.text[0] ? info.game_object_address.text : "-",
        info.game_object_id.text[0] ? info.game_object_id.text : "-",
        info.will_switch_animation.text[0] ? info.will_switch_animation.text : "-",
        info.will_switch_pause.text[0] ? info.will_switch_pause.text : "-", pp.mgr, pp.obj,
        pp.state, pp.state_sane ? 1 : 0);
  } else {
    log("lua: waiting for lua_State* (hook=%d apiHits=%d) | path[0xCE231C]=0x%X +0x128=0x%X"
        " +0x24=0x%X sane=%d",
        g_hook_state, static_cast<int>(g_cap_hits), pp.mgr, pp.obj, pp.state,
        pp.state_sane ? 1 : 0);
  }
}

bool lua_bridge_ready() { return g_hook_state == 1 && g_lua != 0; }

uint32_t lua_bridge_state() { return static_cast<uint32_t>(g_lua); }

int lua_bridge_hook_state() { return g_hook_state; }

int lua_bridge_api_hits() { return static_cast<int>(g_cap_hits); }

const char* lua_type_name(int t) {
  switch (t) {
    case -1:
      return u8"读取失败";
    case kTyUserdata:
      return u8"userdata";
    case kTyNil:
      return u8"nil";
    case kTyNumber:
      return u8"number";
    case kTyString:
      return u8"string";
    case kTyTable:
      return u8"table";
    case kTyFunction:
      return u8"function";
    case kTyLightUserdata:
      return u8"lightuserdata";
    default:
      return u8"异常标签";
  }
}

bool lua_global_read(const char* name, LuaGlobal* out) {
  if (out) {
    out->type = -1;
    out->number = 0.0;
    out->text[0] = '\0';
  }
  if (!name) return false;
  const size_t name_len = std::strlen(name);
  if (name_len == 0 || name_len > kMaxName) return false;
  if (!g_getglobal || !g_type || !g_tostring) return false;

  void* L = current_lua();
  if (!L) return false;
  if (!stack_sane(L)) return false;

  int type = -1;
  double num = 0.0;
  char text[256];
  text[0] = '\0';
  bool pushed = false;

  __try {
    g_getglobal(L, name);  // 压入 1 个值
    pushed = true;
    type = g_type(L, -1);
    if (type != -1 && type != kTyNil && type != kTyUserdata && type != kTyLightUserdata) {
      const char* s = g_tostring(L, -1);
      if (s) {
        size_t n = std::strlen(s);
        if (n > sizeof(text) - 1) n = sizeof(text) - 1;
        std::memcpy(text, s, n);
        text[n] = '\0';
      }
      if (type == kTyNumber && text[0]) {
        num = std::strtod(text, nullptr);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // 异常时也可能已经压进去了，统一在下面弹掉。
  }
  if (pushed) pop_one(L);

  if (type == -1 && text[0] == '\0') return false;
  if (out) {
    out->type = type;
    out->number = num;
    std::memcpy(out->text, text, sizeof(out->text));
  }
  return true;
}

bool lua_global_read_text(const char* name, char* out, size_t out_len) {
  if (!out || out_len == 0) return false;
  out[0] = '\0';
  LuaGlobal v{};
  if (!lua_global_read(name, &v)) return false;
  std::snprintf(out, out_len, "%s", v.text);
  return true;
}

bool lua_global_read_number(const char* name, double* out) {
  LuaGlobal v{};
  if (!lua_global_read(name, &v)) return false;
  if (out) *out = v.number;
  return v.type == kTyNumber;
}

bool lua_global_write_string(const char* name, const char* value) {
  if (!name || !value) return false;
  const size_t name_len = std::strlen(name);
  if (name_len == 0 || name_len > kMaxName) return false;
  if (std::strlen(value) > 1024) return false;
  if (!g_pushstring || !g_setglobal) return false;

  void* L = current_lua();
  if (!L || !stack_sane(L)) return false;

  bool ok = false;
  bool pushed = false;
  __try {
    g_pushstring(L, value);
    pushed = true;
    g_setglobal(L, name);  // setglobal 会弹掉栈顶那个值
    pushed = false;
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (pushed) pop_one(L);
  if (ok) log("lua: %s = \"%s\"", name, value);
  return ok;
}

bool lua_global_write_number(const char* name, double value) {
  if (!name) return false;
  const size_t name_len = std::strlen(name);
  if (name_len == 0 || name_len > kMaxName) return false;
  if (value != value) return false;  // NaN
  if (!g_pushnumber || !g_setglobal) return false;

  void* L = current_lua();
  if (!L || !stack_sane(L)) return false;

  bool ok = false;
  bool pushed = false;
  __try {
    g_pushnumber(L, value);
    pushed = true;
    g_setglobal(L, name);
    pushed = false;
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (pushed) pop_one(L);
  if (ok) {
    char buf[48];
    fmt_number(value, buf, sizeof(buf));
    log("lua: %s = %s (number)", name, buf);
  }
  return ok;
}

bool lua_global_write_nil(const char* name) {
  if (!name) return false;
  const size_t name_len = std::strlen(name);
  if (name_len == 0 || name_len > kMaxName) return false;
  if (!g_pushnil || !g_setglobal) return false;

  void* L = current_lua();
  if (!L || !stack_sane(L)) return false;

  bool ok = false;
  bool pushed = false;
  __try {
    g_pushnil(L);
    pushed = true;
    g_setglobal(L, name);
    pushed = false;
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (pushed) pop_one(L);
  if (ok) log("lua: %s = nil", name);
  return ok;
}

bool lua_anim_read(LuaAnimInfo* out) {
  if (!g_cs_ready) return false;
  EnterCriticalSection(&g_cs);
  const LuaAnimInfo info = g_anim;
  LeaveCriticalSection(&g_cs);
  if (out) *out = info;
  return info.valid;
}

int lua_anim_probe_count() {
  return static_cast<int>(sizeof(kAnimProbes) / sizeof(kAnimProbes[0]));
}

const char* lua_anim_probe_name(int index) {
  if (index < 0 || index >= lua_anim_probe_count()) return "";
  return kAnimProbes[index].name;
}

const char* lua_anim_probe_label(int index) {
  if (index < 0 || index >= lua_anim_probe_count()) return "";
  return kAnimProbes[index].label;
}

bool lua_bridge_engine_cmd(const char* key, bool* handled, std::string* out_msg) {
  if (handled) *handled = false;
  if (!key) return false;

  if (std::strcmp(key, "lua_probe") == 0) {
    if (handled) *handled = true;
    if (!lua_bridge_ready()) {
      if (out_msg) *out_msg = u8"Lua 桥未就绪（需要零售版 1.12 且进入对局）";
      return false;
    }
    char buf[256];
    int got = 0;
    for (int i = 0; i < lua_anim_probe_count(); ++i) {
      LuaGlobal v{};
      if (!lua_global_read(lua_anim_probe_name(i), &v)) continue;
      ++got;
      log("lua probe: %s (%s) = %s", lua_anim_probe_name(i), lua_type_name(v.type),
          v.text[0] ? v.text : "<空>");
    }
    std::snprintf(buf, sizeof(buf), u8"Lua 探针：读到 %d/%d 个动画全局变量（详见日志）", got,
                  lua_anim_probe_count());
    if (out_msg) *out_msg = buf;
    return got > 0;
  }

  if (std::strcmp(key, "lua_dump_state") == 0) {
    if (handled) *handled = true;
    const uint32_t L = lua_bridge_state();
    if (out_msg) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    u8"Lua 状态指针 0x%X（API 命中 %d 次，hook 状态 %d）", L,
                    lua_bridge_api_hits(), lua_bridge_hook_state());
      *out_msg = buf;
    }
    return L != 0;
  }

  return false;
}

}  // namespace game_api
