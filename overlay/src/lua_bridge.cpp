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

// ---- 跨线程队列：渲染线程只入队，游戏线程执行 -------------------------------
// 为什么要这么绕：lua_pushstring / lua_pushnumber / lua_tostring(数字) 都会从
// Lua 堆里分配内存。退对局时脚本状态正在析构，这时从 Present 线程再去分配 /
// 改栈，游戏随后释放那块内存时就会踩到自己刚被改坏的结构（日志里
// RA3_1.12.game+00601643 那次 AV 就是这么来的）。
constexpr int kWriteQueueSize = 16;

struct LuaWrite {
  int kind;  // 0 = string, 1 = number, 2 = nil, 3 = read (结果写 g_read_result)
  double number;
  char name[kMaxName + 1];
  char value[1025];
};

LuaWrite g_wq[kWriteQueueSize];
volatile LONG g_wq_head = 0;   // 渲染线程写
volatile LONG g_wq_tail = 0;   // 游戏线程读
volatile LONG g_refresh_req = 0;   // 动画探针刷新请求
volatile LONG g_probe_log_req = 0; // 刷新后把结果写日志
volatile LONG g_busy = 0;

// 任意全局变量的异步读取结果（面板「读取」用）。
struct LuaReadResult {
  char name[kMaxName + 1];
  game_api::LuaGlobal value;
};
LuaReadResult g_read_result;
CRITICAL_SECTION g_read_cs;
bool g_read_cs_ready = false;
volatile LONG g_read_seq = 0;  // 游戏线程每写一次结果 +1

// 在游戏线程里执行的 Lua 动作（刷新缓存 / 应用写入队列）。
void game_thread_work(void* L);
void refresh_anim_game_thread(void* L);
bool read_global_api(void* L, const char* name, game_api::LuaGlobal* out);

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
      // Keep g_announced — re-logging on every L flap fopen-hitched Present.
    }
  } else {
    InterlockedExchange(&g_cand, static_cast<LONG>(v));
    g_cand_hits = 1;
  }
}

// ---- detour ---------------------------------------------------------------
// 每个 hook 在转发之前，先借"游戏线程正在调用 Lua API"这个时机做本 DLL 的
// Lua 读写（刷新动画快照、应用写入队列）。这样所有 Lua 访问都发生在游戏线程。
int __cdecl hk_gettop(void* L) {
  capture_lua(L);
  game_thread_work(L);
  return g_gettop ? g_gettop(L) : 0;
}

int __cdecl hk_type(void* L, int idx) {
  capture_lua(L);
  game_thread_work(L);
  return g_type ? g_type(L, idx) : -1;
}

const char* __cdecl hk_tostring(void* L, int idx) {
  capture_lua(L);
  game_thread_work(L);
  return g_tostring ? g_tostring(L, idx) : nullptr;
}

void __cdecl hk_pushnumber(void* L, double v) {
  capture_lua(L);
  game_thread_work(L);
  if (g_pushnumber) g_pushnumber(L, v);
}

void __cdecl hk_pushstring(void* L, const char* s) {
  capture_lua(L);
  game_thread_work(L);
  if (g_pushstring) g_pushstring(L, s);
}

void __cdecl hk_getglobal(void* L, const char* name) {
  capture_lua(L);
  game_thread_work(L);
  if (g_getglobal) g_getglobal(L, name);
}

void __cdecl hk_setglobal(void* L, const char* name) {
  capture_lua(L);
  game_thread_work(L);
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

// ---- 游戏线程内的 Lua 操作 --------------------------------------------------
// 读一个全局变量。只做"压入 -> 取值 -> 还原栈顶"，且不调用会分配内存的
// lua_tostring（数字直接从 TValue+8 读 double）。调用方必须保证这是游戏线程。
bool read_global_api(void* L, const char* name, game_api::LuaGlobal* out) {
  if (out) {
    out->type = -1;
    out->number = 0.0;
    out->text[0] = '\0';
  }
  if (!L || !name || !name[0]) return false;
  if (!g_getglobal || !g_type) return false;
  if (!stack_sane(L)) return false;

  uint32_t top0 = 0;
  __try {
    top0 = *reinterpret_cast<const uint32_t*>(L);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }

  int type = -1;
  double num = 0.0;
  char text[256];
  text[0] = '\0';

  __try {
    g_getglobal(L, name);  // 需要 1 个空槽，stack_sane 已保证至少 2 个
    type = g_type(L, -1);
    if (type == kTyNumber) {
      const uint32_t top = *reinterpret_cast<const uint32_t*>(L);
      if (top >= top0 + 16u) {
        num = *reinterpret_cast<const double*>(top - 16u + 8u);
        fmt_number(num, text, sizeof(text));
      }
    } else if (type != -1 && type != kTyNil && type != kTyUserdata &&
               type != kTyLightUserdata && g_tostring) {
      const char* s = g_tostring(L, -1);
      if (s) {
        size_t n = std::strlen(s);
        if (n > sizeof(text) - 1) n = sizeof(text) - 1;
        std::memcpy(text, s, n);
        text[n] = '\0';
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    type = -1;
  }

  // 精确还原栈顶：等价于弹掉刚压入的值，即使中途被游戏自身改动过也回到原值。
  __try {
    *reinterpret_cast<uint32_t*>(L) = top0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  if (type == -1) return false;
  if (out) {
    out->type = type;
    out->number = num;
    std::snprintf(out->text, sizeof(out->text), "%s", text);
  }
  return true;
}

// 应用一条写入请求。必须在游戏线程里调用（pushstring/pushnumber 会分配内存）。
bool apply_write_locked(void* L, const LuaWrite& w) {
  if (!L || !g_setglobal || !stack_sane(L)) return false;
  uint32_t top0 = 0;
  __try {
    top0 = *reinterpret_cast<const uint32_t*>(L);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  bool ok = false;
  __try {
    if (w.kind == 0) {
      if (!g_pushstring) return false;
      g_pushstring(L, w.value);
    } else if (w.kind == 1) {
      if (!g_pushnumber) return false;
      g_pushnumber(L, w.number);
    } else {
      if (!g_pushnil) return false;
      g_pushnil(L);
    }
    g_setglobal(L, w.name);  // 弹掉栈顶
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  __try {
    *reinterpret_cast<uint32_t*>(L) = top0;  // 异常时也把残留值清掉
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return ok;
}

void game_thread_work(void* L) {
  if (!L) return;
  if (g_refresh_req == 0 && g_wq_head == g_wq_tail) return;
  if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;

  // 1) 应用渲染线程排队的写入 / 读取。
  LONG tail = g_wq_tail;
  const LONG head = g_wq_head;
  while (tail != head) {
    const LuaWrite w = g_wq[tail % kWriteQueueSize];
    if (w.kind == 3) {
      game_api::LuaGlobal v;
      if (read_global_api(L, w.name, &v) && g_read_cs_ready) {
        EnterCriticalSection(&g_read_cs);
        std::snprintf(g_read_result.name, sizeof(g_read_result.name), "%s", w.name);
        g_read_result.value = v;
        LeaveCriticalSection(&g_read_cs);
        InterlockedIncrement(&g_read_seq);
        game_api::log("lua: read %s -> %s (%s)", w.name, game_api::lua_type_name(v.type),
                      v.text[0] ? v.text : "<空>");
      } else {
        game_api::log("lua: read %s failed (Lua busy / no such global)", w.name);
      }
    } else if (apply_write_locked(L, w)) {
      if (w.kind == 0) {
        game_api::log("lua: %s = \"%s\"", w.name, w.value);
      } else if (w.kind == 1) {
        char buf[48];
        fmt_number(w.number, buf, sizeof(buf));
        game_api::log("lua: %s = %s (number)", w.name, buf);
      } else {
        game_api::log("lua: %s = nil", w.name);
      }
    } else {
      game_api::log("lua: write %s failed (Lua busy / stack rejected)", w.name);
    }
    ++tail;
    InterlockedExchange(&g_wq_tail, tail);
  }

  // 2) 刷新动画探针缓存（真正的读取发生在游戏线程）。
  if (InterlockedExchange(&g_refresh_req, 0)) refresh_anim_game_thread(L);

  InterlockedExchange(&g_busy, 0);
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

// 只在游戏线程（Lua API hook 内部）调用。
void refresh_anim_game_thread(void* L) {
  game_api::LuaAnimInfo info;
  bool any = false;
  game_api::LuaGlobal* slots[] = {
      &info.game_object_address, &info.game_object_id,   &info.will_switch_animation,
      &info.will_switch_pause,   &info.game_time,        &info.relative_time,
  };
  const int count = static_cast<int>(sizeof(slots) / sizeof(slots[0]));
  for (int i = 0; i < count; ++i) {
    any = read_global_api(L, kAnimProbes[i].name, slots[i]) || any;
  }
  info.valid = any;
  EnterCriticalSection(&g_cs);
  g_anim = info;
  LeaveCriticalSection(&g_cs);

  if (InterlockedExchange(&g_probe_log_req, 0)) {
    for (int i = 0; i < count; ++i) {
      game_api::log("lua probe: %s (%s) = %s", kAnimProbes[i].name,
                    game_api::lua_type_name(slots[i]->type),
                    slots[i]->text[0] ? slots[i]->text : "<空>");
    }
  }
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
  if (!g_read_cs_ready) {
    InitializeCriticalSection(&g_read_cs);
    g_read_cs_ready = true;
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
    // Do not re-log when L pointer flaps; it flooded Present-thread fopen.
    // 这里只置一个请求标志：真正的读取在游戏线程的下一次 Lua API 调用里。
    // 对局外不请求，避免在脚本状态析构/重载期间去动 Lua。
    if (!g_anim_next || static_cast<int32_t>(now - g_anim_next) >= 0) {
      g_anim_next = now + 250;  // 4Hz
      if (battle_clock(nullptr)) InterlockedExchange(&g_refresh_req, 1);
    }
  }

  // Periodic path/anim diag was fopen-spamming from Present; keep quiet.
  (void)now;
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

// 异步读取：入队，由游戏线程执行；UI 每帧 poll 一次结果。
bool lua_request_read(const char* name);

bool lua_read_take(LuaGlobal* out, char* name_out, size_t name_len) {
  if (!out || !g_read_cs_ready) return false;
  static LONG s_seen = 0;
  const LONG seq = g_read_seq;
  if (seq == s_seen) return false;
  s_seen = seq;
  EnterCriticalSection(&g_read_cs);
  *out = g_read_result.value;
  if (name_out && name_len) {
    std::snprintf(name_out, name_len, "%s", g_read_result.name);
  }
  LeaveCriticalSection(&g_read_cs);
  return true;
}

namespace {

// 把一次写入排进队列。真正的 Lua 调用由游戏线程执行（见 game_thread_work）。
bool enqueue_write(int kind, const char* name, const char* value, double number) {
  if (!name) return false;
  const size_t name_len = std::strlen(name);
  if (name_len == 0 || name_len > kMaxName) return false;
  const LONG head = g_wq_head;
  if (head - g_wq_tail >= kWriteQueueSize) return false;  // 队列满
  LuaWrite& w = g_wq[head % kWriteQueueSize];
  w.kind = kind;
  w.number = number;
  std::memcpy(w.name, name, name_len + 1);
  w.value[0] = '\0';
  if (value) {
    const size_t n = std::strlen(value);
    if (n > sizeof(w.value) - 1) return false;
    std::memcpy(w.value, value, n + 1);
  }
  InterlockedExchange(&g_wq_head, head + 1);
  return true;
}

}  // namespace

bool lua_request_read(const char* name) { return enqueue_write(3, name, nullptr, 0.0); }

bool lua_global_write_string(const char* name, const char* value) {
  if (!name || !value) return false;
  if (std::strlen(value) > 1024) return false;
  return enqueue_write(0, name, value, 0.0);
}

bool lua_global_write_number(const char* name, double value) {
  if (!name) return false;
  if (value != value) return false;  // NaN
  return enqueue_write(1, name, nullptr, value);
}

bool lua_global_write_nil(const char* name) {
  if (!name) return false;
  return enqueue_write(2, name, nullptr, 0.0);
}

int lua_pending_writes() {
  const LONG pending = g_wq_head - g_wq_tail;
  return pending > 0 ? static_cast<int>(pending) : 0;
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
    // 只发请求：读取必须由游戏线程做，结果写日志。
    InterlockedExchange(&g_probe_log_req, 1);
    InterlockedExchange(&g_refresh_req, 1);
    if (out_msg) {
      *out_msg = u8"已请求读取动画全局变量：会在游戏线程下一次 Lua 调用时执行，结果见日志";
    }
    return true;
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
