#include "game_api.h"
#include "game_api_internal.h"
#include "unit_names_zh.h"
#include "MinHook.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace game_api {
namespace {

constexpr uint32_t kModBase = 0x400000;
constexpr uint32_t kLocalPlayerRva = 0x8EDE2C;

// Defined below (Uprising uses a different ThePlayerList rva).
static uint32_t player_list_rva();

// Uprising moved the entity fields we touch: everything from the health object
// (retail +0x33C) up to +0x450 grew by 0x10, and the unit-data sub object
// (+0x374) gained another 0x0C at its own speed-control field. Fields below
// +0x138 (owner list of the wrapper object, +0x138 itself, the template at +4)
// are unchanged, verified against ra3ep1_1.0.game.
static uint32_t ent_off(uint32_t retail, uint32_t ep1) {
  return uprising_active() ? ep1 : retail;
}
constexpr uint32_t kEntUnitData = 0x374;   // speed tower lives behind this
constexpr uint32_t kEntUnitDataEp1 = 0x384;
constexpr uint32_t kEntHp = 0x33C;
constexpr uint32_t kEntHpEp1 = 0x34C;
constexpr uint32_t kEntOwner = 0x418;
constexpr uint32_t kEntOwnerEp1 = 0x428;
constexpr uint32_t kEntXp = 0x3CC;
constexpr uint32_t kEntXpEp1 = 0x3DC;
constexpr uint32_t kSpeedCtrl = 0x200;     // inside the unit-data object
constexpr uint32_t kSpeedCtrlEp1 = 0x20C;
constexpr uint32_t kFnAddXp = 0x005173F0;
constexpr uint32_t kFnDestroy = 0x007DCDF0;
constexpr uint32_t kFnCreateUnit = 0x006440F0;
constexpr uint32_t kFnFindTemplate = 0x00822510;
constexpr uint32_t kFnGetMouseXyz = 0x0062C500;

// Retail virtual addresses are meaningless inside the Uprising build (its
// functions live at different RVAs). Refuse the call instead of jumping into
// an unrelated function; the SEH wrapper turns this into a "failed" result.
uint32_t va_of(uint32_t va) {
  if (!module_base()) return 0;
  return module_base() + (va - kModBase);
}

bool is_ptr(uint32_t v) {
  return v >= 0x10000 && v < 0x7FFF0000u && (v & 3u) == 0;
}

uint32_t read_u32(uint32_t addr) { return *reinterpret_cast<uint32_t*>(addr); }
float read_f32(uint32_t addr) { return *reinterpret_cast<float*>(addr); }
bool write_f32(uint32_t addr, float v) {
  *reinterpret_cast<float*>(addr) = v;
  return true;
}
bool write_u32(uint32_t addr, uint32_t v) {
  *reinterpret_cast<uint32_t*>(addr) = v;
  return true;
}

// 游戏对象上的指针随时会过期（单位被摧毁后内存会被堆复用），而无敌补血是
// 每 50ms 定时跑一次的**写**操作：一次写到野指针上就够把堆写坏。日志里那次
// 崩溃 EIP 落在堆空闲块合并函数里，就是这类"写坏堆、几秒后才炸"的典型表现。
// 所以凡是拿游戏对象地址算出来的读写，一律走 SEH 兜底的版本。
static bool safe_read_f32(uint32_t addr, float* out) {
  if (!out) return false;
  __try {
    *out = *reinterpret_cast<float*>(addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
static bool safe_write_f32(uint32_t addr, float v) {
  __try {
    *reinterpret_cast<float*>(addr) = v;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
// 实现在后面（CallJob 附近）。这里先声明，写血量要用。
static bool safe_read_u32(uint32_t addr, uint32_t* out);

uint32_t local_owner() {
  const uint32_t base = loaded_module();
  if (!base) return 0;
  uint32_t player = read_u32(base + player_list_rva());
  if (!is_ptr(player)) return 0;
  for (uint32_t off : {0x28u, 0x30u}) {
    uint32_t owner = read_u32(player + off);
    if (is_ptr(owner)) return owner;
  }
  return 0;
}

uint32_t local_owner_for_ops() {
  uint32_t o = local_owner();
  if (is_ptr(o)) return o;
  if (idb_base()) {
    uint32_t cached = read_u32(reinterpret_cast<uint32_t>(idb_base()));
    if (is_ptr(cached)) return cached;
  }
  return 0;
}

// 归属切换（改动 / 收编）的核心 helper，实现在文件后部（要等 g_live 就位）。
static uint32_t ent_owner_of(uint32_t ent);
static uint32_t ent_team_of(uint32_t ent);
static bool team_owned_by(uint32_t team, uint32_t player);
static uint32_t player_team_of(uint32_t player);
static bool engine_set_team(uint32_t ent, uint32_t team);
static bool assign_entity_to_player(uint32_t ent, uint32_t player, uint32_t team);

// 把指定单位从「无敌」的锁血名单里摘掉（定义在无敌那套静态变量之后）。
// 返回摘掉的个数。
// 「残血(1点)」必须能在无敌状态下生效，否则 50ms 后就会被 tick 续满。
int invincible_release_entities(const std::vector<uint32_t>& ents);

struct SpeedNode {
  uint32_t addr;
};

// Uprising-only guards. They exist so that a wrong entity offset can never be
// turned into a wild write into the game heap: if the object does not look like
// the component we expect, the feature simply reports "nothing writable".
static bool speed_node_sane(uint32_t node) {
  if (!uprising_active()) return true;
  const float cur = read_f32(node + 8);
  const float backup = read_f32(node + 0x40);
  if (!(cur >= 0.f && cur <= 5000.f)) return false;
  if (!(backup >= 0.f && backup <= 5000.f)) return false;
  return true;
}

// 锁血时写进 Body 的血量。
// 这个数值本身不代表"能扛多少伤害"：真正的不掉血靠每 50ms 把它续满，
// 所以不需要无限大。1e9 仍在 int32 范围内（引擎内部有 int 转换，
// 再大就会溢出成负数），同时远远大于任何单次伤害，续血间隔内打不穿。
constexpr float kHpLockValue = 1.0e9f;
// 校验上限：要比 kHpLockValue 高一点，否则自己写进去的值下次会被判成脏数据。
constexpr float kHpSaneMax = 1.5e9f;

static bool hp_object_sane(uint32_t hp) {
  float cur = 0.f, mx = 0.f, alt = 0.f;
  if (!safe_read_f32(hp + 4, &cur) || !safe_read_f32(hp + 0xC, &mx) ||
      !safe_read_f32(hp + 0x10, &alt)) {
    return false;
  }
  if (!uprising_active()) return true;  // 零售版这三个槽位没逐版本核对过，只做可读性校验
  if (!(cur >= 0.f && cur <= kHpSaneMax)) return false;
  if (!(mx >= 0.f && mx <= kHpSaneMax)) return false;
  if (!(alt >= 0.f && alt <= kHpSaneMax)) return false;
  return true;
}

std::vector<uint32_t> speed_nodes(uint32_t ent) {
  std::vector<uint32_t> nodes;
  uint32_t vec = read_u32(ent + ent_off(kEntUnitData, kEntUnitDataEp1));
  if (!is_ptr(vec)) return nodes;
  uint32_t ctrl = read_u32(vec + ent_off(kSpeedCtrl, kSpeedCtrlEp1));
  if (!is_ptr(ctrl)) return nodes;
  uint32_t first = read_u32(ctrl);
  if (is_ptr(first) && speed_node_sane(first)) nodes.push_back(first);
  uint32_t second = read_u32(ctrl + 4);
  if (is_ptr(second)) {
    float flag = read_f32(second + 0x18);
    if (std::fabs(flag - 1.0f) < 1e-6f && speed_node_sane(second)) nodes.push_back(second);
  }
  return nodes;
}

bool set_speed_node(uint32_t node, float target) {
  const float specials[] = {500.f, 10.f, 0.f};
  float cur = read_f32(node + 8);
  if (std::fabs(cur - target) < 1e-4f) return true;
  bool is_special = false;
  for (float s : specials) {
    if (std::fabs(cur - s) < 1e-4f) {
      is_special = true;
      break;
    }
  }
  if (!is_special) write_f32(node + 0x40, cur);
  return write_f32(node + 8, target);
}

bool restore_speed_node(uint32_t node) {
  const float specials[] = {500.f, 10.f, 0.f};
  float cur = read_f32(node + 8);
  bool is_special = false;
  for (float s : specials) {
    if (std::fabs(cur - s) < 1e-4f) {
      is_special = true;
      break;
    }
  }
  if (!is_special) return true;
  return write_f32(node + 8, read_f32(node + 0x40));
}

bool write_entity_hp(uint32_t ent, const char* mode) {
  uint32_t hp = 0;
  if (!safe_read_u32(ent + ent_off(kEntHp, kEntHpEp1), &hp)) return false;
  if (!is_ptr(hp)) return false;
  if (!hp_object_sane(hp)) return false;
  if (std::strcmp(mode, "max") == 0) {
    return safe_write_f32(hp + 4, kHpLockValue) && safe_write_f32(hp + 0xC, kHpLockValue);
  }
  if (std::strcmp(mode, "min") == 0) {
    return safe_write_f32(hp + 4, 1.f);
  }
  if (std::strcmp(mode, "normal") == 0) {
    float mx = 0.f;
    if (!safe_read_f32(hp + 0x10, &mx)) return false;
    return safe_write_f32(hp + 4, mx) && safe_write_f32(hp + 0xC, mx);
  }
  return false;
}

// ---- in-process game calls on a worker thread ----

struct CallJob {
  enum Kind {
    kCdecl,
    kThiscallDestroy,
    // __thiscall(this, a0[, a1])，被调方自己 ret 4 / ret 8。
    // 用于引擎的 AttributeModifier 施加 / 解除（它们都是 thiscall + 栈清理）。
    kThiscallArgs,
  } kind;
  uint32_t fn = 0;
  uint32_t thisptr = 0;
  uint32_t args[6] = {};
  int nargs = 0;
  uint32_t eax_out = 0;
  bool ok = false;
};

static bool safe_read_u32(uint32_t addr, uint32_t* out) {
  if (!out) return false;
  __try {
    *out = *reinterpret_cast<uint32_t*>(addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool safe_write_u32(uint32_t addr, uint32_t v) {
  __try {
    *reinterpret_cast<uint32_t*>(addr) = v;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool safe_read_u8(uint32_t addr, unsigned char* out) {
  if (!out) return false;
  __try {
    *out = *reinterpret_cast<unsigned char*>(addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool safe_write_u8(uint32_t addr, unsigned char v) {
  __try {
    *reinterpret_cast<unsigned char*>(addr) = v;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool safe_read_bytes(uint32_t addr, void* dst, size_t n) {
  if (!dst || !n) return false;
  __try {
    std::memcpy(dst, reinterpret_cast<const void*>(addr), n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

struct CiLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return _stricmp(a.c_str(), b.c_str()) < 0;
  }
};

using NameMap = std::map<std::string, std::string, CiLess>;
NameMap g_unit_names;       // merged lookup (CSF + builtins + user)
NameMap g_user_names;       // only unit_names.txt (what we persist)
bool g_unit_names_loaded = false;
char g_unit_names_path[MAX_PATH] = {};
CRITICAL_SECTION g_unit_names_cs;
bool g_unit_names_cs_ready = false;

void ensure_unit_names_cs() {
  if (!g_unit_names_cs_ready) {
    InitializeCriticalSection(&g_unit_names_cs);
    g_unit_names_cs_ready = true;
  }
}

// User name overrides live outside the release folder: the CSF base catalog and
// the default override table are packed into this DLL (RCDATA id 2, filled by
// overlay/tools/pack_name_tables.py), so a shipped folder holds only main.exe
// and the two DLLs. Saves go to %LOCALAPPDATA%\RA3Overlay\unit_names.txt.
//
// A unit_names.txt that already sits beside the DLL (portable install or a
// hand-edited table) still wins, so nothing moves under those users.
bool resolve_unit_names_path(char* out, size_t out_len) {
  if (!out || out_len < 8) return false;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&resolve_unit_names_path),
                          &self) ||
      !self) {
    return false;
  }
  wchar_t wpath[MAX_PATH] = {};
  if (!GetModuleFileNameW(self, wpath, MAX_PATH)) return false;

  wchar_t wdir[MAX_PATH] = {};
  std::wcsncpy(wdir, wpath, MAX_PATH - 1);
  if (wchar_t* slash = wcsrchr(wdir, L'\\')) *slash = 0;
  else wdir[0] = 0;

  auto to_utf8 = [&](const wchar_t* wfile) {
    return WideCharToMultiByte(CP_UTF8, 0, wfile, -1, out, (int)out_len, nullptr,
                               nullptr) > 0;
  };
  auto exists = [](const wchar_t* wfile) {
    return GetFileAttributesW(wfile) != INVALID_FILE_ATTRIBUTES;
  };

  // 1) Beside the DLL.
  if (wdir[0]) {
    wchar_t wfile[MAX_PATH] = {};
    _snwprintf_s(wfile, _TRUNCATE, L"%s\\unit_names.txt", wdir);
    if (exists(wfile)) return to_utf8(wfile);
  }

  // 2) Dev fallback: <repo>/overlay/bin/<dll> → <repo>/unit_names.txt
  {
    wchar_t wdev[MAX_PATH] = {};
    std::wcsncpy(wdev, wdir, MAX_PATH - 1);
    bool ok = wdev[0] != 0;
    for (int i = 0; ok && i < 3; ++i) {
      wchar_t* slash = wcsrchr(wdev, L'\\');
      if (!slash) {
        ok = false;
        break;
      }
      *slash = 0;
    }
    if (ok) {
      wchar_t wfile[MAX_PATH] = {};
      _snwprintf_s(wfile, _TRUNCATE, L"%s\\unit_names.txt", wdev);
      if (exists(wfile)) return to_utf8(wfile);
    }
  }

  // 3) Per-user store.
  wchar_t appdata[MAX_PATH] = {};
  const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return false;
  wchar_t wstore[MAX_PATH] = {};
  _snwprintf_s(wstore, _TRUNCATE, L"%s\\RA3Overlay", appdata);
  CreateDirectoryW(wstore, nullptr);
  _snwprintf_s(wstore, _TRUNCATE, L"%s\\RA3Overlay\\unit_names.txt", appdata);
  return to_utf8(wstore);
}

// Locate one entry of the packed name-table blob inside this DLL.
// Blob layout: u32 magic 'UNAM', u32 version, u32 count,
//              then per entry: u16 name_len, name, u32 data_len, data.
bool embedded_name_table(const char* entry, const char** data, size_t* len) {
  if (!entry || !data || !len) return false;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&embedded_name_table),
                          &self) ||
      !self) {
    return false;
  }
  HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));
  if (!res) return false;
  HGLOBAL glob = LoadResource(self, res);
  if (!glob) return false;
  const DWORD nbytes = SizeofResource(self, res);
  const char* base = static_cast<const char*>(LockResource(glob));
  if (!base || nbytes < 12) return false;

  unsigned magic = 0, version = 0, count = 0;
  std::memcpy(&magic, base, 4);
  std::memcpy(&version, base + 4, 4);
  std::memcpy(&count, base + 8, 4);
  if (magic != 0x4D414E55u || version != 1u) return false;

  const size_t key_len = std::strlen(entry);
  size_t off = 12;
  for (unsigned i = 0; i < count; ++i) {
    if (off + 2 > nbytes) return false;
    unsigned short klen = 0;
    std::memcpy(&klen, base + off, 2);
    off += 2;
    if (off + klen + 4 > nbytes) return false;
    const bool hit = klen == key_len && std::memcmp(base + off, entry, klen) == 0;
    off += klen;
    unsigned dlen = 0;
    std::memcpy(&dlen, base + off, 4);
    off += 4;
    if (off + dlen > nbytes) return false;
    if (hit) {
      *data = base + off;
      *len = dlen;
      return true;
    }
    off += dlen;
  }
  return false;
}

// Sibling of unit_names.txt → unit_names_csf.txt
bool sibling_names_path(const char* base_utf8, const char* filename, char* out,
                        size_t out_len) {
  if (!base_utf8 || !filename || !out || out_len < 8) return false;
  wchar_t wbase[MAX_PATH] = {};
  if (MultiByteToWideChar(CP_UTF8, 0, base_utf8, -1, wbase, MAX_PATH) <= 0) {
    return false;
  }
  wchar_t* slash = wcsrchr(wbase, L'\\');
  if (!slash) return false;
  *slash = 0;
  wchar_t wfile[MAX_PATH] = {};
  wchar_t wname[64] = {};
  if (MultiByteToWideChar(CP_UTF8, 0, filename, -1, wname, 64) <= 0) return false;
  _snwprintf_s(wfile, _TRUNCATE, L"%s\\%s", wbase, wname);
  return WideCharToMultiByte(CP_UTF8, 0, wfile, -1, out, (int)out_len, nullptr,
                             nullptr) > 0;
}

void seed_builtins(NameMap* m) {
  if (!m) return;
  for (const auto& e : kUnitNamesZh) {
    if (!e.id || !e.zh) continue;
    // Don't overwrite values already loaded from file.
    if (m->find(e.id) == m->end()) (*m)[e.id] = e.zh;
  }
}

bool parse_name_line(const std::string& line, std::string* id, std::string* zh) {
  if (!id || !zh) return false;
  std::string s = line;
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
    s.pop_back();
  if (s.empty() || s[0] == '#' || s[0] == ';') return false;
  const size_t eq = s.find('=');
  if (eq == std::string::npos || eq == 0) return false;
  *id = s.substr(0, eq);
  *zh = s.substr(eq + 1);
  while (!id->empty() && id->back() == ' ') id->pop_back();
  while (!zh->empty() && (*zh)[0] == ' ') zh->erase(zh->begin());
  return !id->empty() && !zh->empty();
}

void load_names_text_into(NameMap* m, const char* data, size_t len,
                          bool overwrite) {
  if (!m || !data || !len) return;
  size_t i = 0;
  if (len >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
      static_cast<unsigned char>(data[1]) == 0xBB &&
      static_cast<unsigned char>(data[2]) == 0xBF) {
    i = 3;  // UTF-8 BOM
  }
  std::string line;
  for (; i <= len; ++i) {
    const bool eol = (i == len) || data[i] == '\n';
    if (!eol) {
      line.push_back(data[i]);
      continue;
    }
    std::string id, zh;
    if (parse_name_line(line, &id, &zh) &&
        (overwrite || m->find(id) == m->end())) {
      (*m)[id] = zh;
    }
    line.clear();
  }
}

bool load_names_file_into(NameMap* m, const char* path, bool overwrite) {
  if (!m || !path || !path[0]) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::string text;
  std::string line;
  while (std::getline(f, line)) {
    text += line;
    text.push_back('\n');
  }
  load_names_text_into(m, text.data(), text.size(), overwrite);
  return true;
}

bool write_unit_names_file(const NameMap& m, const char* path) {
  if (!path || !path[0]) return false;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  // UTF-8 BOM helps Notepad
  const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
  f.write(reinterpret_cast<const char*>(bom), 3);
  f << u8"# RA3 Overlay 单位种类名（UTF-8）\r\n";
  f << u8"# 保存位置: %LOCALAPPDATA%\\RA3Overlay\\unit_names.txt\r\n";
  f << u8"# （若 DLL 同目录存在 unit_names.txt，则沿用该文件）\r\n";
  f << u8"# 格式: TypeId=显示名\r\n";
  f << u8"#       sig_XXXXXXXX=显示名（模板字符串指纹，跨局可用）\r\n";
  f << u8"#       tpl_XXXXXXXX=显示名（旧版内存地址键，重启后失效，可删）\r\n";
  f << u8"# CSF 基底: 已打包进 DLL；本文件覆盖优先\r\n";
  f << u8"# 可在游戏内点「修改」保存，也可手动编辑本文件。\r\n";
  for (const auto& kv : m) {
    f << kv.first << '=' << kv.second << "\r\n";
  }
  return true;
}

void load_unit_names_unlocked() {
  if (g_unit_names_loaded) return;
  g_unit_names.clear();
  g_user_names.clear();
  if (!g_unit_names_path[0]) {
    resolve_unit_names_path(g_unit_names_path, sizeof(g_unit_names_path));
  }

  // 1) Official CSF export (Name:<TypeId>) — packed inside the DLL. A loose
  //    unit_names_csf.txt next to the DLL is only a fallback for older layouts.
  const char* blob = nullptr;
  size_t blob_len = 0;
  if (embedded_name_table("csf", &blob, &blob_len)) {
    load_names_text_into(&g_unit_names, blob, blob_len, true);
    log("unit names CSF loaded from DLL (%d entries)", (int)g_unit_names.size());
  } else {
    char csf_path[MAX_PATH] = {};
    if (g_unit_names_path[0] &&
        sibling_names_path(g_unit_names_path, "unit_names_csf.txt", csf_path,
                           sizeof(csf_path))) {
      if (load_names_file_into(&g_unit_names, csf_path, true)) {
        log("unit names CSF loaded: %s (%d entries)", csf_path,
            (int)g_unit_names.size());
      }
    }
  }

  // 2) Builtins fill gaps only.
  seed_builtins(&g_unit_names);

  // 3) Default override table, also packed inside the DLL. Disk overrides are
  //    persisted separately so saves never dump the whole CSF catalog.
  if (embedded_name_table("user", &blob, &blob_len)) {
    load_names_text_into(&g_user_names, blob, blob_len, true);
    for (const auto& kv : g_user_names) g_unit_names[kv.first] = kv.second;
    log("unit names overrides loaded from DLL (%d entries)",
        (int)g_user_names.size());
  }

  // 4) User edits: portable file beside the DLL, or %LOCALAPPDATA%\RA3Overlay.
  //    Nothing is created here on first run, so the folder stays clean.
  if (g_unit_names_path[0] &&
      load_names_file_into(&g_user_names, g_unit_names_path, true)) {
    for (const auto& kv : g_user_names) g_unit_names[kv.first] = kv.second;
    log("unit names store loaded: %s (%d entries)", g_unit_names_path,
        (int)g_user_names.size());
  }
  g_unit_names_loaded = true;
}

static void fold_trad_glyphs(std::string* s) {
  if (!s || s->empty()) return;
  struct Pair {
    const char* from;
    const char* to;
  };
  static const Pair kFold[] = {
      {u8"質", u8"质"}, {u8"擊", u8"击"}, {u8"車", u8"车"}, {u8"採", u8"采"},
      {u8"礦", u8"矿"}, {u8"戰", u8"战"}, {u8"國", u8"国"}, {u8"機", u8"机"},
      {u8"毀", u8"毁"}, {u8"滅", u8"灭"}, {u8"裝", u8"装"}, {u8"內", u8"内"},
      {u8"動", u8"动"}, {u8"衛", u8"卫"}, {u8"達", u8"达"}, {u8"艦", u8"舰"},
      {u8"彈", u8"弹"}, {u8"導", u8"导"}, {u8"廠", u8"厂"}, {u8"場", u8"场"},
      {u8"營", u8"营"}, {u8"術", u8"术"}, {u8"無", u8"无"}, {u8"電", u8"电"},
      {u8"氣", u8"气"}, {u8"飛", u8"飞"}, {u8"轉", u8"转"}, {u8"傳", u8"传"},
      {u8"層", u8"层"}, {u8"衝", u8"冲"}, {u8"鍾", u8"钟"}, {u8"鐵", u8"铁"},
      {u8"鋼", u8"钢"}, {u8"總", u8"总"}, {u8"協", u8"协"}, {u8"聯", u8"联"},
      {u8"軍", u8"军"}, {u8"禦", u8"御"}, {u8"隊", u8"队"}, {u8"點", u8"点"},
      {u8"門", u8"门"}, {u8"開", u8"开"}, {u8"關", u8"关"}, {u8"東", u8"东"},
      {u8"馬", u8"马"}, {u8"鳥", u8"鸟"}, {u8"魚", u8"鱼"}, {u8"龍", u8"龙"},
      {u8"發", u8"发"}, {u8"對", u8"对"}, {u8"時", u8"时"}, {u8"間", u8"间"},
      {u8"長", u8"长"}, {u8"雲", u8"云"}, {u8"電", u8"电"}, {u8"網", u8"网"},
      {u8"線", u8"线"}, {u8"擊", u8"击"}, {u8"亞", u8"亚"}, {u8"爾", u8"尔"},
      {u8"個", u8"个"}, {u8"這", u8"这"}, {u8"還", u8"还"}, {u8"後", u8"后"},
      {u8"從", u8"从"}, {u8"將", u8"将"}, {u8"經", u8"经"}, {u8"過", u8"过"},
      {u8"進", u8"进"}, {u8"遠", u8"远"}, {u8"運", u8"运"}, {u8"選", u8"选"},
      {u8"達", u8"达"}, {u8"戰", u8"战"}, {u8"護", u8"护"}, {u8"衛", u8"卫"},
      {u8"寶", u8"宝"}, {u8"實", u8"实"}, {u8"業", u8"业"}, {u8"產", u8"产"},
      {u8"強", u8"强"}, {u8"彈", u8"弹"}, {u8"擊", u8"击"}, {u8"砲", u8"炮"},
      {u8"槍", u8"枪"}, {u8"艦", u8"舰"}, {u8"艇", u8"艇"}, {u8"潛", u8"潜"},
      {u8"島", u8"岛"}, {u8"灣", u8"湾"}, {u8"區", u8"区"}, {u8"廳", u8"厅"},
      {u8"廣", u8"广"}, {u8"庫", u8"库"}, {u8"倉", u8"仓"}, {u8"樓", u8"楼"},
      {u8"橋", u8"桥"}, {u8"燈", u8"灯"}, {u8"礦", u8"矿"}, {u8"煉", u8"炼"},
      {u8"廠", u8"厂"}, {u8"機", u8"机"}, {u8"構", u8"构"}, {u8"築", u8"筑"},
      {u8"師", u8"师"}, {u8"團", u8"团"}, {u8"軍", u8"军"}, {u8"陣", u8"阵"},
      {u8"隱", u8"隐"}, {u8"顯", u8"显"}, {u8"驚", u8"惊"}, {u8"嚇", u8"吓"},
      {u8"擊", u8"击"}, {u8"滅", u8"灭"}, {u8"毀", u8"毁"}, {u8"壞", u8"坏"},
      {u8"裝", u8"装"}, {u8"備", u8"备"}, {u8"補", u8"补"}, {u8"給", u8"给"},
      {u8"統", u8"统"}, {u8"計", u8"计"}, {u8"設", u8"设"}, {u8"備", u8"备"},
      {u8"醫", u8"医"}, {u8"療", u8"疗"}, {u8"藥", u8"药"}, {u8"傷", u8"伤"},
      {u8"殘", u8"残"}, {u8"獨", u8"独"}, {u8"特", u8"特"}, {u8"種", u8"种"},
      {u8"類", u8"类"}, {u8"別", u8"别"}, {u8"複", u8"复"}, {u8"雜", u8"杂"},
      {u8"簡", u8"简"}, {u8"單", u8"单"}, {u8"雙", u8"双"}, {u8"隻", u8"只"},
      {u8"條", u8"条"}, {u8"萬", u8"万"}, {u8"億", u8"亿"}, {u8"與", u8"与"},
      {u8"為", u8"为"}, {u8"無", u8"无"}, {u8"開", u8"开"}, {u8"關", u8"关"},
      {u8"門", u8"门"}, {u8"間", u8"间"}, {u8"問", u8"问"}, {u8"題", u8"题"},
      {u8"頭", u8"头"}, {u8"顏", u8"颜"}, {u8"體", u8"体"}, {u8"動", u8"动"},
      {u8"員", u8"员"}, {u8"帥", u8"帅"}, {u8"師", u8"师"}, {u8"隊", u8"队"},
      {u8"戰", u8"战"}, {u8"鬥", u8"斗"}, {u8"爭", u8"争"}, {u8"勝", u8"胜"},
      {u8"敗", u8"败"}, {u8"敵", u8"敌"}, {u8"擊", u8"击"}, {u8"殺", u8"杀"},
      {u8"斬", u8"斩"}, {u8"首", u8"首"}, {u8"級", u8"级"}, {u8"階", u8"阶"},
      {u8"層", u8"层"}, {u8"級", u8"级"}, {u8"極", u8"极"}, {u8"端", u8"端"},
      {u8"終", u8"终"}, {u8"結", u8"结"}, {u8"構", u8"构"}, {u8"築", u8"筑"},
      {u8"牆", u8"墙"}, {u8"圍", u8"围"}, {u8"欄", u8"栏"}, {u8"桿", u8"杆"},
      {u8"標", u8"标"}, {u8"槍", u8"枪"}, {u8"彈", u8"弹"}, {u8"藥", u8"药"},
      {u8"炸", u8"炸"}, {u8"彈", u8"弹"}, {u8"轟", u8"轰"}, {u8"爆", u8"爆"},
      {u8"雷", u8"雷"}, {u8"達", u8"达"}, {u8"碟", u8"碟"}, {u8"氣", u8"气"},
      {u8"球", u8"球"}, {u8"飛", u8"飞"}, {u8"機", u8"机"}, {u8"無", u8"无"},
      {u8"人", u8"人"}, {u8"防", u8"防"}, {u8"禦", u8"御"}, {u8"點", u8"点"},
  };
  for (const auto& p : kFold) {
    if (std::strcmp(p.from, p.to) == 0) continue;
    const size_t from_n = std::strlen(p.from);
    const size_t to_n = std::strlen(p.to);
    for (;;) {
      const size_t pos = s->find(p.from);
      if (pos == std::string::npos) break;
      s->replace(pos, from_n, p.to, to_n);
    }
  }
}

static std::string simplify_zh_display(const char* u8) {
  if (!u8 || !u8[0]) return {};
  bool wide = false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(u8); *p; ++p) {
    if (*p >= 0x80) {
      wide = true;
      break;
    }
  }
  std::string out = u8;
  if (wide) {
    const int wn = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
    if (wn > 1) {
      std::wstring w((size_t)wn, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, u8, -1, &w[0], wn);
      const int sn = LCMapStringW(0x0804, LCMAP_SIMPLIFIED_CHINESE, w.c_str(), -1, nullptr, 0);
      if (sn > 1) {
        std::wstring sim((size_t)sn, L'\0');
        LCMapStringW(0x0804, LCMAP_SIMPLIFIED_CHINESE, w.c_str(), -1, &sim[0], sn);
        const int un = WideCharToMultiByte(CP_UTF8, 0, sim.c_str(), -1, nullptr, 0, nullptr,
                                            nullptr);
        if (un > 1) {
          out.assign((size_t)un, '\0');
          WideCharToMultiByte(CP_UTF8, 0, sim.c_str(), -1, &out[0], un, nullptr, nullptr);
          if (!out.empty() && out.back() == '\0') out.pop_back();
        }
      }
    }
    fold_trad_glyphs(&out);
  }
  return out;
}

const char* lookup_zh_name(const char* type_id) {
  if (!type_id || !type_id[0]) return nullptr;
  ensure_unit_names_cs();
  EnterCriticalSection(&g_unit_names_cs);
  load_unit_names_unlocked();
  static thread_local std::string tls;
  auto it = g_unit_names.find(type_id);
  if (it == g_unit_names.end()) {
    LeaveCriticalSection(&g_unit_names_cs);
    return nullptr;
  }
  tls = simplify_zh_display(it->second.c_str());
  LeaveCriticalSection(&g_unit_names_cs);
  return tls.c_str();
}

static bool charset_ok_typeid(const char* s, int n) {
  if (!s || n < 6 || n > 64) return false;
  if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
    return false;
  }
  for (int i = 0; i < n; ++i) {
    const char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_') {
      continue;
    }
    return false;
  }
  return true;
}

static bool has_bad_typeid_token(const char* s) {
  // NOTE: Do NOT list "Tech2"/"Tech3" — they appear in real GameObject ids
  // (e.g. SovietAntiVehicleVehicleTech3 = Apocalypse Tank).
  static const char* kBad[] = {
      "Armor", "CommandSet", "Collapse", "Debris", "Weapon", "Warhead",
      "Projectile", "Locomotor", "Voice", "Sound", "Button", "Portrait",
      "BuildState", "Experience", "Upgrade", "Attribute", "Wheel", "Dust",
      "Tread", "Flash", "Glow", "Smoke", "Spark", "Dying", "Missile",
      "Launcher", "Torpedo", "Evacuate", "Repair", "Egg",
      "Function", "Personality", "SpecialPower", "ObjectFilter", "Effect",
      "Turret", "Barrel", "Anim", "Death", "DamageFire", "Explosion",
      "Platform", "Turbine", "Mask", "Beacon", "_on", "_big", "_small",
      "_NRM", "_SPM", "UnitIntro", "ModuleTag", "Draw", "Geometry"};
  for (const char* b : kBad) {
    if (std::strstr(s, b)) return true;
  }
  return false;
}

static bool looks_like_faction_typeid(const char* s, int n) {
  if (!charset_ok_typeid(s, n)) return false;

  // Known catalog entries always win (before bad-token filter).
  char exact[96];
  if (n < (int)sizeof(exact)) {
    std::memcpy(exact, s, n);
    exact[n] = 0;
    if (lookup_zh_name(exact)) return true;
  }

  if (has_bad_typeid_token(s)) return false;

  static const char* kPrefix[] = {
      "Allied", "Soviet", "Japan", "Civilian", "Neutral", "Celestial",
      "OreNode", "OilDerrick", "Hospital", "Garage", "Observation",
      "ShipYard", "Airport", "Veterancy", "Defensive", "Bonus", "Crate",
  };
  for (const char* p : kPrefix) {
    const int pl = (int)std::strlen(p);
    if (n >= pl && _strnicmp(s, p, pl) == 0) return true;
  }
  // Tech / map props: HospitalTechStructure, GE_Apartment01, NY_Structure_01
  if (std::strstr(s, "TechStructure") || std::strstr(s, "OreNode") ||
      std::strstr(s, "OilDerrick") || std::strstr(s, "Submarine")) {
    return true;
  }
  // Civilian map props: two-letter map code + underscore + CapWord
  if (n >= 8 && s[2] == '_' &&
      ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
      ((s[1] >= 'A' && s[1] <= 'Z') || (s[1] >= 'a' && s[1] <= 'z')) &&
      ((s[3] >= 'A' && s[3] <= 'Z'))) {
    return true;
  }
  return false;
}

static bool is_known_typeid(const char* type_id) {
  return lookup_zh_name(type_id) != nullptr;
}

static bool copy_c_string(uint32_t addr, char* out, size_t out_len) {
  if (!out || out_len < 2) return false;
  char tmp[96] = {};
  if (!safe_read_bytes(addr, tmp, sizeof(tmp) - 1)) return false;
  tmp[sizeof(tmp) - 1] = 0;
  int n = 0;
  while (n < (int)sizeof(tmp) - 1 && tmp[n]) ++n;
  if (!looks_like_faction_typeid(tmp, n)) return false;
  std::snprintf(out, out_len, "%s", tmp);
  return true;
}

static int score_typeid(const char* s) {
  if (!s || !s[0]) return -1;
  if (is_known_typeid(s)) return 1000 + (int)std::strlen(s);
  if (!looks_like_faction_typeid(s, (int)std::strlen(s))) return -1;
  int score = (int)std::strlen(s);
  // Prefer canonical GameObject-style names.
  if (std::strstr(s, "Infantry") || std::strstr(s, "Vehicle") ||
      std::strstr(s, "Aircraft") || std::strstr(s, "Ship") ||
      std::strstr(s, "Submarine") || std::strstr(s, "Barracks") ||
      std::strstr(s, "Factory") || std::strstr(s, "PowerPlant") ||
      std::strstr(s, "Refinery") || std::strstr(s, "MCV") ||
      std::strstr(s, "Miner") || std::strstr(s, "ConstructionYard") ||
      std::strstr(s, "ConYard") || std::strstr(s, "Airfield") ||
      std::strstr(s, "Outpost") || std::strstr(s, "Crane") ||
      std::strstr(s, "BaseDefense") || std::strstr(s, "TechStructure") ||
      std::strstr(s, "OreNode") || std::strstr(s, "OilDerrick") ||
      std::strstr(s, "Hospital") || std::strstr(s, "Garage") ||
      std::strstr(s, "Observation") || std::strstr(s, "Commando")) {
    score += 50;
  }
  // Civilian map props are weaker than faction units when both appear.
  if (s[2] == '_' && !(std::strstr(s, "Allied") || std::strstr(s, "Soviet") ||
                       std::strstr(s, "Japan"))) {
    score -= 10;
  }
  return score;
}

static uint32_t fnv1a_update(uint32_t h, const char* s, int n) {
  for (int i = 0; i < n; ++i) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

// Prefer official GameObject ids on the unit template; ignore nickname/VFX strings.
// Also returns a content signature for unresolved templates (stable across restarts).
static bool resolve_type_id_from_template(uint32_t unit_data, char* out, size_t out_len,
                                          char* sig_out, size_t sig_len) {
  if (!is_ptr(unit_data) || !out || out_len < 2) return false;
  out[0] = 0;
  if (sig_out && sig_len) sig_out[0] = 0;

  char best[96] = {};
  int best_score = -1;
  uint32_t sig = 2166136261u;
  int sig_parts = 0;

  auto consider = [&](const char* cand) {
    if (!cand || !cand[0]) return;
    const int n = (int)std::strlen(cand);
    // Include known catalog ids in the signature even if they match a soft
    // filter (e.g. historical Tech3 false-positive).
    if (charset_ok_typeid(cand, n) &&
        (!has_bad_typeid_token(cand) || is_known_typeid(cand))) {
      sig = fnv1a_update(sig, cand, n);
      sig = fnv1a_update(sig, "|", 1);
      ++sig_parts;
    }
    const int sc = score_typeid(cand);
    if (sc > best_score) {
      best_score = sc;
      std::snprintf(best, sizeof(best), "%s", cand);
    }
  };

  char tmp[96] = {};
  // Inline name fields near the start of the template object.
  for (uint32_t off = 0; off <= 0x140; off += 4) {
    if (!safe_read_bytes(unit_data + off, tmp, 64)) continue;
    tmp[63] = 0;
    int n = 0;
    while (n < 63 && tmp[n]) ++n;
    if (n >= 6) consider(tmp);
  }

  // Pointers to name / type descriptor strings (naval templates can sit deeper).
  for (uint32_t off = 4; off <= 0x280; off += 4) {
    uint32_t p = 0;
    if (!safe_read_u32(unit_data + off, &p) || !is_ptr(p)) continue;
    if (copy_c_string(p, tmp, sizeof(tmp))) consider(tmp);
    char raw[96] = {};
    if (safe_read_bytes(p, raw, sizeof(raw) - 1)) {
      raw[sizeof(raw) - 1] = 0;
      int n = 0;
      while (n < (int)sizeof(raw) - 1 && raw[n]) ++n;
      if (n >= 6) consider(raw);
    }
    uint32_t p2 = 0;
    if (safe_read_u32(p, &p2) && is_ptr(p2)) {
      if (copy_c_string(p2, tmp, sizeof(tmp))) consider(tmp);
      if (safe_read_bytes(p2, raw, sizeof(raw) - 1)) {
        raw[sizeof(raw) - 1] = 0;
        int n = 0;
        while (n < (int)sizeof(raw) - 1 && raw[n]) ++n;
        if (n >= 6) consider(raw);
      }
    }
  }

  if (sig_out && sig_len >= 16 && sig_parts > 0) {
    std::snprintf(sig_out, sig_len, "sig_%08X", sig);
  }

  if (best_score < 0 || !best[0]) return false;
  // Only accept if it actually looks like a TypeId (score path already checks).
  if (!looks_like_faction_typeid(best, (int)std::strlen(best))) return false;
  std::snprintf(out, out_len, "%s", best);
  return true;
}

struct DmgTypeEntry {
  const char* token;  // ASCII as stored / TypeId hint
  const char* zh;
};

// RA3 DamageType / WeaponCategory labels (armor matrix), not raw DPS numbers.
static const DmgTypeEntry kDamageTypes[] = {
    {"CANNON", u8"加农（对载具）"},
    {"Cannon", u8"加农（对载具）"},
    {"AUTO_CANNON", u8"机炮"},
    {"AutoCannon", u8"机炮"},
    {"GUN", u8"枪弹"},
    {"Gun", u8"枪弹"},
    {"ROCKET", u8"火箭"},
    {"Rocket", u8"火箭"},
    {"SNIPER", u8"狙击"},
    {"Sniper", u8"狙击"},
    {"EXPLOSIVE", u8"爆破"},
    {"Explosive", u8"爆破"},
    {"FLAME", u8"火焰"},
    {"Flame", u8"火焰"},
    {"TESLA", u8"磁暴"},
    {"Tesla", u8"磁暴"},
    {"SPECTRUM", u8"光谱"},
    {"Spectrum", u8"光谱"},
    {"LASER", u8"激光"},
    {"Laser", u8"激光"},
    {"RADIATION", u8"辐射"},
    {"Radiation", u8"辐射"},
    {"GRENADE", u8"榴弹"},
    {"Grenade", u8"榴弹"},
    {"MELEE", u8"近战"},
    {"Melee", u8"近战"},
    {"TORPEDO", u8"鱼雷"},
    {"Torpedo", u8"鱼雷"},
    {"MAGIC", u8"特殊"},
};

static bool dmg_type_from_token(const char* tok, char* out_en, size_t en_len,
                                char* out_zh, size_t zh_len) {
  if (!tok || !tok[0]) return false;
  for (const auto& e : kDamageTypes) {
    if (_stricmp(tok, e.token) == 0) {
      if (out_en && en_len) std::snprintf(out_en, en_len, "%s", e.token);
      if (out_zh && zh_len) std::snprintf(out_zh, zh_len, "%s", e.zh);
      return true;
    }
  }
  return false;
}

// Guess from official TypeId naming patterns when template strings are missing.
static bool dmg_type_from_typeid(const char* type_id, char* out_en, size_t en_len,
                                 char* out_zh, size_t zh_len) {
  if (!type_id || !type_id[0]) return false;
  if (std::strstr(type_id, "Commando") || std::strstr(type_id, "Infiltration")) {
    return dmg_type_from_token("GUN", out_en, en_len, out_zh, zh_len);
  }
  if (std::strstr(type_id, "Bomber") || std::strstr(type_id, "AntiStructure")) {
    // Many siege weapons are rocket/explosive; prefer EXPLOSIVE label.
    if (std::strstr(type_id, "Ship") && std::strstr(type_id, "AntiStructure")) {
      return dmg_type_from_token("ROCKET", out_en, en_len, out_zh, zh_len);
    }
    return dmg_type_from_token("EXPLOSIVE", out_en, en_len, out_zh, zh_len);
  }
  if (std::strstr(type_id, "AntiAir") || std::strstr(type_id, "AntiVehicleInfantry") ||
      std::strstr(type_id, "FinalSquadron") || std::strstr(type_id, "SupportAircraft")) {
    return dmg_type_from_token("ROCKET", out_en, en_len, out_zh, zh_len);
  }
  if (std::strstr(type_id, "AntiVehicleVehicle") || std::strstr(type_id, "AntiNavy") ||
      std::strstr(type_id, "HeavyAntiVehicle")) {
    if (std::strstr(type_id, "Infantry")) {
      // Flak / Tesla troopers etc. — often rocket or tesla; flak is special.
      if (std::strstr(type_id, "Heavy")) {
        return dmg_type_from_token("TESLA", out_en, en_len, out_zh, zh_len);
      }
      return dmg_type_from_token("ROCKET", out_en, en_len, out_zh, zh_len);
    }
    return dmg_type_from_token("CANNON", out_en, en_len, out_zh, zh_len);
  }
  if (std::strstr(type_id, "AntiInfantry")) {
    return dmg_type_from_token("GUN", out_en, en_len, out_zh, zh_len);
  }
  if (std::strstr(type_id, "Fighter") || std::strstr(type_id, "AntiShipAircraft") ||
      std::strstr(type_id, "AntiGroundAircraft")) {
    return dmg_type_from_token("AUTO_CANNON", out_en, en_len, out_zh, zh_len);
  }
  if (std::strstr(type_id, "BaseDefenseAdvanced") && std::strstr(type_id, "Soviet")) {
    return dmg_type_from_token("TESLA", out_en, en_len, out_zh, zh_len);
  }
  if (std::strstr(type_id, "BaseDefenseAdvanced") && std::strstr(type_id, "Allied")) {
    return dmg_type_from_token("SPECTRUM", out_en, en_len, out_zh, zh_len);
  }
  return false;
}

static bool resolve_damage_type(uint32_t unit_data, const char* type_id, char* out_en,
                                size_t en_len, char* out_zh, size_t zh_len) {
  if (out_en && en_len) out_en[0] = 0;
  if (out_zh && zh_len) out_zh[0] = 0;

  // Prefer explicit DamageType / WeaponCategory strings on the template.
  if (is_ptr(unit_data)) {
    char tmp[64] = {};
    for (uint32_t off = 0; off <= 0x200; off += 4) {
      if (!safe_read_bytes(unit_data + off, tmp, sizeof(tmp) - 1)) continue;
      tmp[sizeof(tmp) - 1] = 0;
      if (dmg_type_from_token(tmp, out_en, en_len, out_zh, zh_len)) return true;
    }
    for (uint32_t off = 4; off <= 0x280; off += 4) {
      uint32_t p = 0;
      if (!safe_read_u32(unit_data + off, &p) || !is_ptr(p)) continue;
      if (!safe_read_bytes(p, tmp, sizeof(tmp) - 1)) continue;
      tmp[sizeof(tmp) - 1] = 0;
      if (dmg_type_from_token(tmp, out_en, en_len, out_zh, zh_len)) return true;
    }
  }

  return dmg_type_from_typeid(type_id, out_en, en_len, out_zh, zh_len);
}

static DWORD WINAPI call_thread(LPVOID param) {
  auto* job = static_cast<CallJob*>(param);
  __try {
    if (job->kind == CallJob::kCdecl) {
      using Fn0 = uint32_t(__cdecl*)();
      using Fn1 = uint32_t(__cdecl*)(uint32_t);
      using Fn2 = uint32_t(__cdecl*)(uint32_t, uint32_t);
      using Fn4 = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t);
      using Fn5 = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
      uint32_t eax = 0;
      switch (job->nargs) {
        case 0:
          eax = reinterpret_cast<Fn0>(job->fn)();
          break;
        case 1:
          eax = reinterpret_cast<Fn1>(job->fn)(job->args[0]);
          break;
        case 2:
          eax = reinterpret_cast<Fn2>(job->fn)(job->args[0], job->args[1]);
          break;
        case 4:
          eax = reinterpret_cast<Fn4>(job->fn)(job->args[0], job->args[1], job->args[2],
                                               job->args[3]);
          break;
        case 5:
          eax = reinterpret_cast<Fn5>(job->fn)(job->args[0], job->args[1], job->args[2],
                                               job->args[3], job->args[4]);
          break;
        default:
          job->ok = false;
          return 0;
      }
      job->eax_out = eax;
      job->ok = true;
    } else if (job->kind == CallJob::kThiscallDestroy) {
      uint32_t fn = job->fn;
      uint32_t ent = job->thisptr;
      __asm {
        mov ecx, ent
        push 0
        push 0x19
        push 6
        mov eax, fn
        call eax
      }
      job->ok = true;
    } else if (job->kind == CallJob::kThiscallArgs) {
      const uint32_t fn = job->fn;
      const uint32_t self = job->thisptr;
      const uint32_t a0 = job->args[0];
      const uint32_t a1 = job->args[1];
      if (job->nargs == 2) {
        __asm {
          mov ecx, self
          push a1
          push a0
          mov eax, fn
          call eax
        }
      } else {
        __asm {
          mov ecx, self
          push a0
          mov eax, fn
          call eax
        }
      }
      job->ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    job->ok = false;
    job->eax_out = 0;
  }
  return 0;
}

bool run_job(CallJob& job, DWORD timeout_ms = 4000) {
  HANDLE th = CreateThread(nullptr, 0, call_thread, &job, 0, nullptr);
  if (!th) return false;
  DWORD w = WaitForSingleObject(th, timeout_ms);
  CloseHandle(th);
  return w == WAIT_OBJECT_0 && job.ok;
}

HWND game_hwnd() {
  struct Ctx {
    DWORD pid;
    HWND hwnd;
  } ctx{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER)) return TRUE;
        if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;
        c->hwnd = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.hwnd;
}

bool mouse_world_pos(float out[3]) {
  uint8_t* mc = mc_base();
  if (!mc) return false;
  HWND hwnd = game_hwnd();
  if (!hwnd) return false;
  POINT pt{};
  if (!GetCursorPos(&pt)) return false;
  if (!ScreenToClient(hwnd, &pt)) return false;
  uint32_t in_buf = reinterpret_cast<uint32_t>(mc + 0x1110);
  uint32_t out_buf = reinterpret_cast<uint32_t>(mc + 0x1080);
  *reinterpret_cast<int32_t*>(in_buf) = pt.x;
  *reinterpret_cast<int32_t*>(in_buf + 4) = pt.y;
  *reinterpret_cast<uint32_t*>(out_buf) = 0x7F7F7F7F;
  *reinterpret_cast<uint32_t*>(out_buf + 4) = 0x7F7F7F7F;
  *reinterpret_cast<uint32_t*>(out_buf + 8) = 0x7F7F7F7F;

  CallJob job{};
  job.kind = CallJob::kCdecl;
  job.fn = va_of(kFnGetMouseXyz);
  job.nargs = 4;
  job.args[0] = in_buf;
  job.args[1] = out_buf;
  job.args[2] = 0;
  job.args[3] = 0;
  if (!run_job(job)) return false;
  if (*reinterpret_cast<uint32_t*>(out_buf) == 0x7F7F7F7F) return false;
  out[0] = read_f32(out_buf);
  out[1] = read_f32(out_buf + 4);
  out[2] = read_f32(out_buf + 8);
  return true;
}

static uint32_t g_clone_seq = 0;

bool clone_selected(bool as_mine, int copies, std::string* out_msg) {
  if (copies < 1) copies = 1;
  if (uprising_active()) {
    if (out_msg) *out_msg = u8"复制单位：起义时刻暂未适配";
    return false;
  }
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  if (!mc_base()) {
    if (out_msg) *out_msg = u8"MustCode 未分配";
    return false;
  }
  float spawn[3];
  if (!mouse_world_pos(spawn)) {
    if (out_msg) *out_msg = u8"读不到鼠标地图坐标（请把鼠标移到战场地形上）";
    return false;
  }
  // Dedicated spawn buffer (avoid sharing with GetMouseXyz out at +0x1080).
  uint32_t pos_buf = reinterpret_cast<uint32_t>(mc_base() + 0x10A0);
  g_clone_seq = (g_clone_seq + 1) & 0xFFFF;
  int ok_n = 0, fail_n = 0;
  int slot_base = (int)g_clone_seq * 3;
  for (size_t i = 0; i < ents.size(); ++i) {
    uint32_t ent = ents[i];
    if (!is_ptr(ent)) {
      fail_n += copies;
      continue;
    }
    uint32_t unit_data = 0;
    if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) {
      fail_n += copies;
      continue;
    }
    uint32_t owner = 0;
    if (as_mine) {
      owner = local_owner();
      if (!is_ptr(owner)) {
        if (out_msg) *out_msg = u8"读不到本地玩家归属";
        return false;
      }
    } else {
      uint32_t owner_off = ent_off(kEntOwner, kEntOwnerEp1);
      if (!safe_read_u32(ent + owner_off, &owner) || !is_ptr(owner)) {
        fail_n += copies;
        continue;
      }
    }
    uint32_t owner_vt = 0;
    if (!safe_read_u32(owner, &owner_vt) || !is_ptr(owner_vt)) {
      fail_n += copies;
      continue;
    }
    uint32_t owner_info = 0;
    if (!safe_read_u32(owner + 0x10, &owner_info)) {
      fail_n += copies;
      continue;
    }
    // owner+0x10 may be 0 (MustCode pushes it as-is).
    for (int c = 0; c < copies; ++c) {
      int slot = slot_base + (int)i * copies + c;
      float radius = 35.f + 18.f * (float)(slot % 12);
      float angle = (float)slot * 2.399963f;
      float x = spawn[0] + radius * std::cos(angle);
      float y = spawn[1] + radius * std::sin(angle);
      float z = spawn[2];
      write_f32(pos_buf, x);
      write_f32(pos_buf + 4, y);
      write_f32(pos_buf + 8, z);
      CallJob job{};
      job.kind = CallJob::kCdecl;
      job.fn = va_of(kFnCreateUnit);
      job.nargs = 5;
      job.args[0] = 0;
      job.args[1] = unit_data;
      job.args[2] = pos_buf;
      job.args[3] = owner;
      job.args[4] = owner_info;
      if (run_job(job) && is_ptr(job.eax_out)) {
        ++ok_n;
      } else {
        ++fail_n;
      }
      Sleep(50);
    }
  }
  if (ok_n == 0) {
    if (out_msg) *out_msg = u8"复制失败：引擎未生成任何单位（请确认已选中单位，且鼠标在地形上）";
    return false;
  }
  if (out_msg) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  u8"已生成 %d 个单位到鼠标附近（归属%s）", ok_n,
                  as_mine ? u8"己方" : u8"原阵营");
    *out_msg = buf;
    if (fail_n) *out_msg += u8"；部分失败";
  }
  return true;
}

bool apply_speed(const char* mode, std::string* out_msg) {
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  float target = 0.f;
  bool restore = std::strcmp(mode, "restore") == 0;
  if (std::strcmp(mode, "max") == 0) target = 500.f;
  else if (std::strcmp(mode, "slow") == 0) target = 10.f;
  else if (std::strcmp(mode, "freeze") == 0) target = 0.f;
  int n = 0;
  for (uint32_t ent : ents) {
    auto nodes = speed_nodes(ent);
    if (nodes.empty()) continue;
    bool ok = true;
    for (uint32_t node : nodes) {
      ok = (restore ? restore_speed_node(node) : set_speed_node(node, target)) && ok;
    }
    if (ok) ++n;
  }
  if (n == 0) {
    if (out_msg) *out_msg = u8"选中对象没有可写的速度组件";
    return false;
  }
  if (out_msg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), u8"已对 %d 个单位改速度", n);
    *out_msg = buf;
  }
  return true;
}

bool apply_hp(const char* mode, const std::vector<uint32_t>* ents_in,
              std::string* out_msg) {
  auto ents = ents_in ? *ents_in : selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  // 「残血(1点)」优先于无敌：先把这些单位从锁血名单里摘掉，再写血量。
  // 否则无敌 tick 会在 50ms 内把血续满，看起来就像必须手动解除无敌。
  const int released =
      std::strcmp(mode, "min") == 0 ? invincible_release_entities(ents) : 0;
  int n = 0;
  for (uint32_t ent : ents) {
    if (write_entity_hp(ent, mode)) ++n;
  }
  if (n == 0) {
    if (out_msg) *out_msg = u8"选中对象没有可写的血量组件";
    return false;
  }
  if (out_msg) {
    char buf[128];
    if (released > 0) {
      std::snprintf(buf, sizeof(buf), u8"已对 %d 个单位改血量（其中 %d 个退出了无敌锁血）", n,
                    released);
    } else {
      std::snprintf(buf, sizeof(buf), u8"已对 %d 个单位改血量", n);
    }
    *out_msg = buf;
  }
  return true;
}

bool kill_selected(std::string* out_msg) {
  if (uprising_active()) {
    if (out_msg) *out_msg = u8"摧毁单位：起义时刻暂未适配";
    return false;
  }
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  int ok_n = 0;
  for (uint32_t ent : ents) {
    CallJob job{};
    job.kind = CallJob::kThiscallDestroy;
    job.fn = va_of(kFnDestroy);
    job.thisptr = ent;
    if (run_job(job)) ++ok_n;
  }
  if (ok_n == 0) {
    if (out_msg) *out_msg = u8"摧毁失败";
    return false;
  }
  if (out_msg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), u8"已摧毁 %d 个单位", ok_n);
    *out_msg = buf;
  }
  return true;
}

bool rank_up(std::string* out_msg) {
  if (uprising_active()) {
    if (out_msg) *out_msg = u8"单位晋升：起义时刻暂未适配";
    return false;
  }
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  int ok_cnt = 0, rose_cnt = 0;
  for (uint32_t ent : ents) {
    uint32_t tracker = read_u32(ent + 0x3CC);
    if (!is_ptr(tracker)) continue;
    uint32_t prev = read_u32(tracker + 0x24);
    int rose = 0;
    for (int i = 0; i < 8; ++i) {
      CallJob job{};
      job.kind = CallJob::kCdecl;
      job.fn = va_of(kFnAddXp);
      job.nargs = 2;
      job.args[0] = ent;
      job.args[1] = 200000;
      if (!run_job(job)) break;
      uint32_t now = read_u32(tracker + 0x24);
      if (now <= prev) break;
      ++rose;
      prev = now;
    }
    ++ok_cnt;
    if (rose) ++rose_cnt;
  }
  if (ok_cnt == 0) {
    if (out_msg) *out_msg = u8"晋升失败：无星级组件";
    return false;
  }
  if (rose_cnt == 0) {
    if (out_msg) *out_msg = u8"调用成功但等级无变化（可能已满级）";
    return false;
  }
  if (out_msg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), u8"已提升 %d 个单位星级", rose_cnt);
    *out_msg = buf;
  }
  return true;
}

bool convert_selected(std::string* out_msg) {
  if (uprising_active()) {
    if (out_msg) *out_msg = u8"收编单位：起义时刻暂未适配";
    return false;
  }
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  uint32_t owner = local_owner_for_ops();
  if (!is_ptr(owner) || !is_ptr(read_u32(owner))) {
    if (out_msg) *out_msg = u8"读不到本地玩家归属";
    return false;
  }
  int n = 0;
  const uint32_t owner_team = player_team_of(owner);
  for (uint32_t ent : ents) {
    if (read_u32(ent + 0x418) == owner) continue;
    if (assign_entity_to_player(ent, owner, owner_team)) ++n;
  }
  if (n == 0) {
    if (out_msg) *out_msg = u8"选中单位已是己方或写入失败";
    return false;
  }
  if (out_msg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), u8"已收编 %d 个单位", n);
    *out_msg = buf;
  }
  return true;
}

bool apply_damage_mult(std::string* out_msg) {
  if (uprising_active()) {
    if (out_msg) *out_msg = u8"伤害倍率：起义时刻暂未适配";
    return false;
  }
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  int n = 0;
  for (uint32_t ent : ents) {
    uint32_t tracker = read_u32(ent + 0x3CC);
    if (!is_ptr(tracker)) continue;
    uint32_t sub = read_u32(tracker + 0x2C);
    if (!is_ptr(sub)) continue;
    if (write_f32(sub + 0x08, 5.f)) ++n;
  }
  if (n == 0) {
    if (out_msg) *out_msg = u8"无可用的星级加成组件";
    return false;
  }
  if (out_msg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), u8"已对 %d 个单位设置伤害×5", n);
    *out_msg = buf;
  }
  return true;
}

bool chaos_selected(std::string* out_msg) {
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"没读到选中单位（请先选中）";
    return false;
  }
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_real_distribution<float> uni(0.f, 1.f);
  const char* speed_modes[] = {"max", "slow", "freeze", "restore"};
  const char* hp_modes[] = {"max", "min", "normal"};
  int n = 0;
  for (uint32_t ent : ents) {
    bool did = false;
    bool do_speed = uni(rng) < 0.7f;
    bool do_hp = uni(rng) < 0.7f;
    if (!do_speed && !do_hp) do_speed = true;
    if (do_speed) {
      const char* sm = speed_modes[rng() % 4];
      auto nodes = speed_nodes(ent);
      if (!nodes.empty()) {
        for (uint32_t node : nodes) {
          if (std::strcmp(sm, "restore") == 0)
            restore_speed_node(node);
          else if (std::strcmp(sm, "max") == 0)
            set_speed_node(node, 500.f);
          else if (std::strcmp(sm, "slow") == 0)
            set_speed_node(node, 10.f);
          else
            set_speed_node(node, 0.f);
        }
        did = true;
      }
    }
    if (do_hp && write_entity_hp(ent, hp_modes[rng() % 3])) did = true;
    if (did) ++n;
  }
  if (n == 0) {
    if (out_msg) *out_msg = u8"没有可写的速度/血量组件";
    return false;
  }
  if (out_msg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), u8"混乱已施加到 %d 个单位", n);
    *out_msg = buf;
  }
  return true;
}

// ThePlayerList (RVA 0x8EDE2C): +0x28 local, +0x2C count, +0x30 Player*[0x14].
// Money: resource table at player+0xE4, first entry is money obj (+0x04 funds).
// Power: player+0x74 -> obj with +0x04 total / +0x08 used (MustCode Power).
constexpr uint32_t kPlayerListLocalOff = 0x28;
constexpr uint32_t kPlayerListCountOff = 0x2C;
constexpr uint32_t kPlayerListArrayOff = 0x30;
constexpr uint32_t kPlayerListSlots = 0x14;
constexpr uint32_t kPlayerMoneyTableOff = 0xE4;
constexpr uint32_t kPlayerPowerObjOff = 0x74;

// Uprising 1.0 keeps the player-list object (+0x28 local, +0x2C count, +0x30
// array) and the battle-logic fields (+0x50 ticks, +0x148 mode). Only the
// globals moved. Retail keeps the constants above.
static uint32_t player_list_rva() {
  return uprising_active() ? 0x9009C4u : kLocalPlayerRva;
}
static uint32_t battle_logic_rva() {
  return uprising_active() ? 0x8F0808u : 0x8DDE84u;
}

static void read_player_display_name(uint32_t player, char* out, size_t out_len) {
  if (!out || out_len < 2) return;
  out[0] = 0;
  if (!is_ptr(player)) return;
  uint32_t name_ptr = 0;
  if (!safe_read_u32(player + 0x14, &name_ptr) || !is_ptr(name_ptr)) return;
  char narrow[64] = {};
  wchar_t wide[64] = {};
  if (safe_read_bytes(name_ptr, narrow, sizeof(narrow) - 1)) {
    narrow[sizeof(narrow) - 1] = 0;
    bool ok = narrow[0] != 0;
    for (int i = 0; ok && narrow[i] && i < 48; ++i) {
      unsigned char c = (unsigned char)narrow[i];
      if (c < 0x20 && c != '\t') ok = false;
    }
    if (ok) {
      std::snprintf(out, out_len, "%s", narrow);
      return;
    }
  }
  if (safe_read_bytes(name_ptr, wide, sizeof(wide) - sizeof(wchar_t))) {
    wide[(sizeof(wide) / sizeof(wchar_t)) - 1] = 0;
    if (wide[0]) {
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_len, nullptr, nullptr);
      out[out_len - 1] = 0;
    }
  }
}

// Case-insensitive substring (RA3 type ids mix CamelCase and ALLCAPS CSF keys).
static bool id_has_token(const char* id, const char* tok) {
  if (!id || !tok || !tok[0]) return false;
  const size_t n = std::strlen(id);
  const size_t m = std::strlen(tok);
  if (m > n) return false;
  for (size_t i = 0; i + m <= n; ++i) {
    if (_strnicmp(id + i, tok, m) == 0) return true;
  }
  return false;
}

// BFI / Sage KindOf STRUCTURE analogue: name tokens the engine uses for
// buildings. Units like AlliedAntiStructureVehicle must stay units.
static bool type_id_is_building(const char* id) {
  if (!id || !id[0]) return false;
  if (id_has_token(id, "AntiStructure")) return false;
  static const char* kBuild[] = {
      "Barracks", "WarFactory", "PowerPlant", "Refinery",
      "ConstructionYard", "ConYard", "NavalYard", "Shipyard", "Airfield",
      "Outpost", "Crane", "BaseDefense", "WallHub", "TeslaWallHub", "WallPiece",
      "TechStructure", "Chronosphere", "ProtonCollider", "IronCurtain",
      "VacuumImploder", "SuperWeapon", "Nanotech", "Psionic", "Bunker",
      "VIPBunker", "Hospital", "Garage", "Observation", "OilDerrick", "OreNode",
      "TechBuilding", "ShipYardTech", "AirportTech", "Veterancy",
      "DefensiveStructure", "CommandHub", "Egg", "Nanocore", "NanoHive",
      "FloatingFortress", "VolcanoFortress", "EconomicStructure",
  };
  for (const char* b : kBuild) {
    if (id_has_token(id, b)) return true;
  }
  // Bare "...Factory" (WarFactory already matched); avoid random "Factory" in
  // non-building ids by requiring a faction / Build prefix nearby.
  if (id_has_token(id, "Factory") &&
      (id_has_token(id, "War") || id_has_token(id, "Allied") ||
       id_has_token(id, "Soviet") || id_has_token(id, "Japan") ||
       id_has_token(id, "Build"))) {
    return true;
  }
  return false;
}

static bool looks_like_entity(uint32_t ent) {
  if (!is_ptr(ent)) return false;
  uint32_t ow = 0, unit_data = 0;
  if (!safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow) || !is_ptr(ow)) {
    return false;
  }
  if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) return false;
  return true;
}

static bool entity_alive(uint32_t ent) {
  uint32_t hp = 0;
  if (!safe_read_u32(ent + ent_off(kEntHp, kEntHpEp1), &hp) || !is_ptr(hp)) return true;
  float cur = 0.f, mx = 0.f;
  if (!safe_read_bytes(hp + 4, &cur, sizeof(cur))) return true;
  safe_read_bytes(hp + 0x10, &mx, sizeof(mx));
  if (!(cur == cur) || cur > kHpSaneMax || cur < -1.f) return true;  // NaN / garbage
  // Only drop clearly-dead objects (max HP known and current is ~0).
  if (mx > 1.f && cur <= 0.5f) return false;
  return true;
}

static bool entity_belongs_to(uint32_t ent, uint32_t player, uint32_t /*player_id*/) {
  if (!looks_like_entity(ent) || !is_ptr(player)) return false;
  uint32_t ow = 0;
  if (!safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow) || !is_ptr(ow)) {
    return false;
  }
  // Owner must be this Player object. player+0x20 is shared by faction/team
  // and would dump another seat's army onto the first matching player.
  return ow == player;
}

// ===========================================================================
// 真无敌：AttributeModifier 通道
//
// 为什么不是"改血量"：把 [Body+4]/[Body+0xC] 写成 9999999.0f 只是把血条拉长，
// 伤害照样进 —— 按百分比结算的伤害、直接 kill/shatter 的路径、以及引擎重算
// 血量上限后把当前值 clamp 回去，任何一条都能让它失效（冰冻后掉血快就是这个）。
//
// 真正的免疫是引擎自己的 AttributeModifier，原版就有（WBStringHashes.big 可查）：
//   AttributeModifier_IronCurtain          0x6516AA2D  无敌 + 铁幕外观
//   AttributeModifier_InvulnerabilityCrate 0x37C61161  无敌、无外观
//
// 两条调用链（__thiscall，被调方自己清栈；地址按特征码现场定位，双版本通用）：
//   施加  AttributeModifier 施加  (Thing, modifier*, frames)  ret 8
//   解除  AttributeModifier 解除  (Thing, modifier*)          ret 4
//
// 唯一没有照抄的是"查表"：日冕版把 modifier 库暴露成 (库哈希, 名称哈希, out)，
// 原版则把库写死在全局 0xCD7ADC，参数里的哈希是"库内键"。而那个查表函数是
// find-or-create —— 猜错键会往游戏的表里塞垃圾条目。所以这里改成只读内存扫描：
//   在哈希树节点上 node+0x10 = 名称哈希、node+0x14 = modifier 对象。
// 依据 0x4A6F50 的反汇编：`cmp dword ptr [ecx+0x10], ebp` 后 `mov eax, [edx+0x14]` / ret。
// 校验三道：哈希相等 / 树链接像指针 / [modifier] 是游戏模块内的 vtable。
// ===========================================================================

constexpr uint32_t kHashIronCurtain = 0x6516AA2Du;  // AttributeModifier_IronCurtain（有外观）
// 已用引擎自己的哈希函数验算：MAP_INVULNERABLE_Up 的真实 Key 是 0xF24F7254，
// 之前那个 0x508D8641 是另一份构建/模组里的常量，在零售版查不到，别再用了。
// 零售版映像里确实存在这个字符串：AttributeModifier_InvulnerabilityCrate
constexpr uint32_t kHashInvulnCrate = 0x37C61161u;
constexpr uint32_t kModStoreHash = 0xC5E07887u;      // 全局 modifier 库容器哈希

// 施加（22 字节版，两个游戏版本里都只有 1 处）：56 57 8B 7C 24 0C 85 FF 8B F1 75 07
// 5F 32 C0 5E C2 08 00 8B 8E
static const uint8_t kSigModApply[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x85, 0xFF,
                                       0x8B, 0xF1, 0x75, 0x07, 0x5F, 0x32, 0xC0, 0x5E,
                                       0xC2, 0x08, 0x00, 0x8B, 0x8E};
// 解除：56 57 8B 7C 24 0C 83 7F 04 00 8B F1 74 64（唯一）
static const uint8_t kSigModRemove[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x83,
                                        0x7F, 0x04, 0x00, 0x8B, 0xF1, 0x74, 0x64};
// modifier 库查表 __cdecl(容器哈希, 名字哈希, out) -> AttributeModifier*：55 8B 6C 24 0C 85 ED 75
static const uint8_t kSigModFind[] = {0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x85, 0xED, 0x75};

struct ModApi {
  uint32_t apply = 0;
  uint32_t remove = 0;
  uint32_t find = 0;
  bool ok = false;
  char note[192] = {};
};
static ModApi g_mod_api;
static uint32_t g_mod_api_base = 0;

struct ModNode {
  uint32_t hash = 0;
  uint32_t mod = 0;
};
static ModNode g_mod_nodes[8];
static int g_mod_node_n = 0;
static uint32_t g_mod_node_base = 0;
static uint32_t g_mod_miss_hash = 0;   // 上次没找到的哈希（避免每帧重扫堆）
static DWORD g_mod_miss_tick = 0;

static bool image_range(uint32_t* lo, uint32_t* hi) {
  const uint32_t base = loaded_module();
  if (!base) return false;
  __try {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *lo = base;
    *hi = base + nt->OptionalHeader.SizeOfImage;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// 在游戏模块映像里找特征码，最多收 max_hits 个。
static int find_sig_all(const uint8_t* sig, int sig_len, uint32_t* hits, int max_hits) {
  uint32_t lo = 0, hi = 0;
  if (!image_range(&lo, &hi) || !sig || sig_len <= 0 || !hits || max_hits <= 0) return 0;
  int n = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(lo);
  const size_t span = hi - lo;
  if (span <= static_cast<size_t>(sig_len)) return 0;
  __try {
    for (size_t i = 0; i + static_cast<size_t>(sig_len) <= span && n < max_hits; ++i) {
      if (p[i] != sig[0]) continue;
      if (std::memcmp(p + i, sig, static_cast<size_t>(sig_len)) == 0) {
        hits[n++] = lo + static_cast<uint32_t>(i);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

// 施加和解除在二进制里紧挨着（解除 = 施加 + 0x90，两个版本都成立）。
// 不硬编码这个距离：先按特征码各找一遍，只接受满足 "apply + 0x90 == remove" 的那一对。
static void resolve_modifier_api() {
  const uint32_t base = loaded_module();
  if (!base) return;
  if (g_mod_api.ok && g_mod_api_base == base) return;
  g_mod_api = ModApi{};
  g_mod_api_base = base;

  uint32_t rem[4] = {};
  uint32_t app[8] = {};
  uint32_t fnd[4] = {};
  const int nrem = find_sig_all(kSigModRemove, sizeof(kSigModRemove), rem, 4);
  const int napp = find_sig_all(kSigModApply, sizeof(kSigModApply), app, 8);
  const int nfnd = find_sig_all(kSigModFind, sizeof(kSigModFind), fnd, 4);
  if (nrem != 1) {
    std::snprintf(g_mod_api.note, sizeof(g_mod_api.note),
                  u8"modifier 解除函数特征码命中 %d 处（应为 1，版本不匹配）", nrem);
    return;
  }
  uint32_t picked = 0;
  for (int i = 0; i < napp; ++i) {
    if (app[i] + 0x90u == rem[0]) {
      picked = app[i];
      break;
    }
  }
  if (!picked) {
    std::snprintf(g_mod_api.note, sizeof(g_mod_api.note),
                  u8"modifier 施加函数没定位到（施加 %d 处，解除 @%08X）", napp,
                  (unsigned)rem[0]);
    return;
  }
  g_mod_api.apply = picked;
  g_mod_api.remove = rem[0];
  if (nfnd == 1) {
    g_mod_api.find = fnd[0];
  } else {
    std::snprintf(g_mod_api.note, sizeof(g_mod_api.note),
                  u8"modifier 查表函数特征码命中 %d 处（应为 1）", nfnd);
  }
  g_mod_api.ok = true;
  log("modifier api: apply=%08X remove=%08X find=%08X", (unsigned)g_mod_api.apply,
      (unsigned)g_mod_api.remove, (unsigned)g_mod_api.find);
}

// [modifier] 是否像一个真的 AttributeModifier。
// 注意：零售版里 AttributeModifier 没有 vtable（SAGE 的 RefCountClass 不是多态类），
// 之前拿「首字段是映像内 vtable」来判断，会把查到的合法对象全判成垃圾——
// 这就是「未解析」的真正原因。现在改成只做可读性检查。
static bool looks_like_attribute_modifier(uint32_t mod) {
  if (!is_ptr(mod)) return false;
  uint32_t f0 = 0, f4 = 0, f8 = 0, fc = 0;
  if (!safe_read_u32(mod, &f0)) return false;
  if (!safe_read_u32(mod + 4, &f4)) return false;
  if (!safe_read_u32(mod + 8, &f8)) return false;
  if (!safe_read_u32(mod + 0xC, &fc)) return false;
  // 全 0 的块基本是「表头/占位」，不像 modifier。
  if (!f0 && !f4 && !f8 && !fc) return false;
  return true;
}

// 严格版：首字段必须指向映像内（只有做堆扫描兜底时才需要这么严）。
static bool modifier_has_vtable(uint32_t mod) {
  if (!is_ptr(mod)) return false;
  uint32_t vt = 0;
  if (!safe_read_u32(mod, &vt)) return false;
  uint32_t lo = 0, hi = 0;
  if (!image_range(&lo, &hi)) return false;
  return vt >= lo && vt < hi;
}

static bool is_committed_readable(DWORD state, DWORD protect) {
  if (state != MEM_COMMIT) return false;
  if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  const DWORD p = protect & 0xFFu;
  return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
         p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
         p == PAGE_EXECUTE_WRITECOPY;
}

// 走引擎自己的 modifier 库查表：__cdecl(容器哈希, 键, 0) -> AttributeModifier*
// 键既可能是「名字哈希」，也可能是「驻留名字字符串的地址」，两种都试。
static uint32_t modifier_lookup_key(uint32_t key) {
  if (!g_mod_api.ok || !g_mod_api.find) return 0;
  using Find_t = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t);
  uint32_t mod = 0;
  __try {
    mod = reinterpret_cast<Find_t>(g_mod_api.find)(kModStoreHash, key, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  return looks_like_attribute_modifier(mod) ? mod : 0;
}

static uint32_t modifier_lookup_engine(uint32_t hash) { return modifier_lookup_key(hash); }

// 在游戏模块映像里找一段 ASCII 字面量，命中返回它的 VA。
static int image_find_literal(const char* s, uint32_t* out, int max_out) {
  uint32_t lo = 0, hi = 0;
  if (!s || !s[0] || !out || max_out <= 0) return 0;
  if (!image_range(&lo, &hi)) return 0;
  const size_t n = std::strlen(s);
  int got = 0;
  __try {
    const auto* p = reinterpret_cast<const uint8_t*>(lo);
    const size_t span = hi - lo;
    for (size_t i = 0; i + n < span && got < max_out; ++i) {
      const void* hit = std::memchr(p + i, s[0], span - i - n);
      if (!hit) break;
      const auto* c = static_cast<const uint8_t*>(hit);
      i = static_cast<size_t>(c - p);
      if (std::memcmp(c, s, n) == 0) out[got++] = lo + static_cast<uint32_t>(i);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return got;
}

// 取一个 modifier 对象：先问引擎，查不到再退回堆扫描（兼容改过容器哈希的版本）。
static uint32_t find_modifier_by_hash(uint32_t hash, char* why, size_t why_n) {
  const uint32_t base = loaded_module();
  if (!base) {
    if (why && why_n) std::snprintf(why, why_n, u8"游戏模块未加载");
    return 0;
  }
  if (g_mod_node_base != base) {
    g_mod_node_n = 0;
    g_mod_node_base = base;
  }
  for (int i = 0; i < g_mod_node_n; ++i) {
    if (g_mod_nodes[i].hash == hash) return g_mod_nodes[i].mod;
  }
  // 查表失败过就冷却一阵：UI 每帧都会来问，不能每帧扫一遍堆。
  const DWORD now_tick = GetTickCount();
  if (g_mod_miss_hash == hash && g_mod_miss_tick &&
      static_cast<int32_t>(now_tick - g_mod_miss_tick) < 30000) {
    if (why && why_n) std::snprintf(why, why_n, u8"30 秒内不再重扫");
    return 0;
  }

  const uint32_t via_engine = modifier_lookup_engine(hash);
  if (via_engine) {
    if (g_mod_node_n < (int)(sizeof(g_mod_nodes) / sizeof(g_mod_nodes[0]))) {
      g_mod_nodes[g_mod_node_n].hash = hash;
      g_mod_nodes[g_mod_node_n].mod = via_engine;
      ++g_mod_node_n;
    }
    if (why && why_n) std::snprintf(why, why_n, u8"engine@%08X", (unsigned)via_engine);
    return via_engine;
  }

  const uint8_t low = static_cast<uint8_t>(hash & 0xFFu);
  const uint8_t b1 = static_cast<uint8_t>((hash >> 8) & 0xFFu);
  const uint8_t b2 = static_cast<uint8_t>((hash >> 16) & 0xFFu);
  const uint8_t b3 = static_cast<uint8_t>((hash >> 24) & 0xFFu);
  int key_hits = 0, node_hits = 0;

  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  auto* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
  auto* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
    const size_t size = mbi.RegionSize;
    auto* region = static_cast<uint8_t*>(mbi.BaseAddress);
    auto* next = region + size;
    if (is_committed_readable(mbi.State, mbi.Protect) && size >= 24) {
      // memchr 先按最低字节筛，避免逐 dword 走 2GB。
      const uint8_t* lim = region + size - 24;
      for (const uint8_t* q = region; q <= lim;) {
        const void* hit = std::memchr(q, low, static_cast<size_t>(lim - q) + 1);
        if (!hit) break;
        const auto* c = static_cast<const uint8_t*>(hit);
        q = c + 1;
        const uint32_t at = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c));
        if (at & 3u) continue;                        // 哈希是 4 字节对齐写的
        if (c[1] != b1 || c[2] != b2 || c[3] != b3) continue;
        ++key_hits;
        if (at < 0x10000) continue;
        // 树节点布局：node+0x00 left / +0x04 right / +0x10 key / +0x14 value
        uint32_t left = 0, right = 0, mod = 0;
        if (!safe_read_u32(at - 0x10, &left) || !safe_read_u32(at - 0x0C, &right)) continue;
        if (!safe_read_u32(at + 4, &mod)) continue;
        if (!looks_like_attribute_modifier(mod)) continue;
        auto link_ok = [at](uint32_t v) {
          if (v == 0) return true;
          if (!is_ptr(v)) return false;
          const uint32_t d = (v > at) ? (v - at) : (at - v);
          return d < (64u << 20);                     // 树节点在同一个堆里
        };
        if (!link_ok(left) || !link_ok(right)) continue;
        ++node_hits;
        if (g_mod_node_n < (int)(sizeof(g_mod_nodes) / sizeof(g_mod_nodes[0]))) {
          g_mod_nodes[g_mod_node_n].hash = hash;
          g_mod_nodes[g_mod_node_n].mod = mod;
          ++g_mod_node_n;
        }
        log("modifier %08X: key@%08X mod=%08X vt=%08X", (unsigned)hash, (unsigned)at,
            (unsigned)mod, (unsigned)read_u32(mod));
        if (why && why_n) {
          std::snprintf(why, why_n, u8"node@%08X", (unsigned)at);
        }
        return mod;
      }
    }
    if (next <= p) break;
    p = next;
  }
  if (why && why_n) {
    std::snprintf(why, why_n, u8"没找到（哈希命中 %d 次，像节点的 %d 个）", key_hits, node_hits);
  }
  g_mod_miss_hash = hash;
  g_mod_miss_tick = GetTickCount();
  return 0;
}

// 零售版 / 起义时刻的 modifier 名字和哈希常量不一定对得上（那套常量来自另一份
// 构建）。这里按「候选哈希 → 候选名字的驻留字符串指针 → 堆扫描」逐级试，
// 成功一次就缓存，失败也缓存 15 秒避免 UI 每帧重试。
static uint32_t g_inv_mod[2] = {0, 0};
static uint8_t g_inv_mod_tried[2] = {0, 0};
static DWORD g_inv_mod_tick[2] = {0, 0};
static char g_inv_mod_how[2][128] = {};

// 引擎自己的「名字 → Key」哈希函数（实现见下）：hash(s, len, len, 1)。
static uint32_t name_hash(const char* name);

static uint32_t resolve_invuln_modifier(int flavor, char* why, size_t why_n) {
  flavor = (flavor == 1) ? 1 : 0;
  auto set_why = [why, why_n](const char* fmt, ...) {
    if (!why || !why_n) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, why_n, fmt, ap);
    va_end(ap);
  };
  if (g_inv_mod[flavor]) {
    if (why && why_n) std::snprintf(why, why_n, "%s", g_inv_mod_how[flavor]);
    return g_inv_mod[flavor];
  }
  resolve_modifier_api();
  if (!g_mod_api.ok) {
    set_why("%s", g_mod_api.note);
    return 0;
  }
  const DWORD now = GetTickCount();
  if (g_inv_mod_tried[flavor] &&
      static_cast<int32_t>(now - g_inv_mod_tick[flavor]) < 15000) {
    set_why(u8"15 秒内不再重试");
    return 0;
  }
  g_inv_mod_tried[flavor] = 1;
  g_inv_mod_tick[flavor] = now;

  // 0x6516AA2D / 0x37C61161 已经用引擎自己的哈希函数验算过：
  //   AttributeModifier_IronCurtain            -> 6516AA2D
  //   AttributeModifier_InvulnerabilityCrate   -> 37C61161
  const uint32_t hashes[2] = {
      flavor ? kHashInvulnCrate : kHashIronCurtain,
      flavor ? kHashIronCurtain : kHashInvulnCrate,
  };
  for (uint32_t h : hashes) {
    const uint32_t m = modifier_lookup_key(h);
    log("invincible: find(hash=%08X) -> %08X", (unsigned)h, (unsigned)m);
    if (m) {
      g_inv_mod[flavor] = m;
      std::snprintf(g_inv_mod_how[flavor], sizeof(g_inv_mod_how[flavor]), "hash@%08X",
                    (unsigned)h);
      if (why && why_n) std::snprintf(why, why_n, "%s", g_inv_mod_how[flavor]);
      return m;
    }
  }

  // 用引擎自己的哈希函数现算候选名字的 Key，再查库（哈希不写死，换版本也认）。
  const char* const ordered[4] = {
      flavor ? "AttributeModifier_InvulnerabilityCrate" : "AttributeModifier_IronCurtain",
      flavor ? "AttributeModifier_IronCurtain" : "AttributeModifier_InvulnerabilityCrate",
      "AttributeModifier_Invulnerability",
      "AttributeModifier_Invulnerable",
  };
  for (const char* name : ordered) {
    const uint32_t h = name_hash(name);
    if (!h) continue;
    const uint32_t m = modifier_lookup_key(h);
    log("invincible: find(%s hash=%08X) -> %08X", name, (unsigned)h, (unsigned)m);
    if (m) {
      g_inv_mod[flavor] = m;
      std::snprintf(g_inv_mod_how[flavor], sizeof(g_inv_mod_how[flavor]), "%s", name);
      if (why && why_n) std::snprintf(why, why_n, "%s@%08X", name, (unsigned)h);
      return m;
    }
  }

  char scan_why[192] = {};
  const uint32_t via_scan = find_modifier_by_hash(hashes[0], scan_why, sizeof(scan_why));
  if (via_scan) {
    g_inv_mod[flavor] = via_scan;
    std::snprintf(g_inv_mod_how[flavor], sizeof(g_inv_mod_how[flavor]), "scan(%s)", scan_why);
    if (why && why_n) std::snprintf(why, why_n, "%s", g_inv_mod_how[flavor]);
    return via_scan;
  }
  set_why(u8"库里没有（%s）", scan_why);
  return 0;
}

// 引擎自己的「名字 → Key」哈希函数。找法：映像里有字符串
// "AttributeModifier_InvulnerabilityCrate"，箱子的模块初始化代码是
//   push <字符串VA>; call nameHash  ->  再用结果去 modifier 库里查。
// 所以顺着这条引用就能把哈希函数挖出来（零售版 RVA 0x18040，起义时刻 0x17F2C）。
static uint32_t g_name_hash_fn = 0;
static bool g_name_hash_tried = false;

static uint32_t resolve_name_hash_fn() {
  if (g_name_hash_tried) return g_name_hash_fn;
  g_name_hash_tried = true;
  uint32_t lo = 0, hi = 0;
  if (!image_range(&lo, &hi)) return 0;
  uint32_t addr[1] = {};
  if (image_find_literal("AttributeModifier_InvulnerabilityCrate", addr, 1) <= 0) {
    log("name hash: literal not found in image");
    return 0;
  }
  const uint32_t sva = addr[0];
  const auto* img = reinterpret_cast<const uint8_t*>(lo);
  const size_t span = hi - lo;
  __try {
    for (size_t i = 0; i + 12 < span; ++i) {
      if (img[i] != 0x68) continue;  // push imm32
      uint32_t imm = 0;
      std::memcpy(&imm, img + i + 1, 4);
      if (imm != sva) continue;
      for (size_t k = i + 5; k + 5 <= i + 14; ++k) {
        if (img[k] != 0xE8) continue;  // call rel32
        int32_t rel = 0;
        std::memcpy(&rel, img + k + 1, 4);
        const uint32_t tgt = static_cast<uint32_t>(lo + k + 5 + static_cast<uint32_t>(rel));
        if (tgt < lo || tgt >= hi) continue;
        g_name_hash_fn = tgt;
        log("name hash: fn @%08X (from ref @%08X)", (unsigned)tgt, (unsigned)(lo + i));
        return g_name_hash_fn;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  log("name hash: no call reference found");
  return 0;
}

// 和引擎完全一样的调用方式：hash(s, len, len, 1)。
static uint32_t name_hash(const char* name) {
  const uint32_t fn = resolve_name_hash_fn();
  if (!is_ptr(fn) || !name || !name[0]) return 0;
  const int n = static_cast<int>(std::strlen(name));
  using Hash_t = uint32_t(__cdecl*)(const char*, int, int, int);
  __try {
    return reinterpret_cast<Hash_t>(fn)(name, n, n, 1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}
static bool modifier_invoke(uint32_t ent, uint32_t mod, int frames, bool remove,
                            bool on_game_thread) {
  if (!g_mod_api.ok || !is_ptr(ent) || !is_ptr(mod)) return false;
  if (on_game_thread) {
    using Apply_t = int(__thiscall*)(void*, uint32_t, int);
    using Remove_t = int(__thiscall*)(void*, uintptr_t);
    __try {
      if (remove) {
        reinterpret_cast<Remove_t>(g_mod_api.remove)(reinterpret_cast<void*>(ent), mod);
      } else {
        reinterpret_cast<Apply_t>(g_mod_api.apply)(reinterpret_cast<void*>(ent), mod, frames);
      }
      return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
    }
  }
  CallJob job{};
  job.kind = CallJob::kThiscallArgs;
  job.thisptr = ent;
  job.args[0] = mod;
  job.args[1] = static_cast<uint32_t>(frames);
  job.nargs = remove ? 1 : 2;
  job.fn = remove ? g_mod_api.remove : g_mod_api.apply;
  return run_job(job);
}

static std::vector<uint32_t> scope_entities(int cap = 1024) {
  std::vector<uint32_t> out;
  std::vector<uint32_t> buf(cap > 0 ? static_cast<size_t>(cap) : 0u);
  if (buf.empty()) return out;
  const int n = unit_collect_scope(buf.data(), cap);
  out.reserve(static_cast<size_t>(n > 0 ? n : 0));
  for (int i = 0; i < n; ++i) out.push_back(buf[static_cast<size_t>(i)]);
  return out;
}

// 无敌 = 锁血。点一次「无敌」先把**当时选中**那批单位的血量拉满，
// 再把这批单位记进名单；之后每 50ms 只给名单里的单位续满。
// 换选、点别的单位都不会被加进来——旧实现每帧重读当前选择，才会「点谁谁无敌」。
static bool g_inv_active = false;
static std::vector<uint32_t> g_inv_targets;   // 点无敌那一刻锁定的单位
static int g_inv_last_hit = 0;                // 上一轮实际补血到几个
static DWORD g_inv_next_tick = 0;             // 50ms 节流
static DWORD g_inv_log_tick = 0;
static DWORD g_inv_off_battle_tick = 0;       // 离开战局的起始时刻（宽限用）

// ---- 目标身份指纹 + 销毁记录 ----
// 只记住 GameObject 指针是不够的：单位被鬼王（冲撞）碾压、或走任何"直接摧毁"
// 的路径之后，引擎会把对象内存还给堆，之后又被别的对象复用。锁血是每 50ms
// 一次的**写**操作，只要有一轮写到旧地址，就可能落进别人的对象甚至堆控制块里——
// 崩的时候堆已经在别处了（最后一次日志的 EIP 就落在堆空闲块合并函数里，
// 读的是个垃圾地址 0x56D8DB84）。
//
// 两道闸：
//   1) 每个目标记下 [模板指针 / 血量对象 / 血上限]，每轮先校验，对不上就摘掉；
//   2) 销毁钩子把刚销毁的地址写进环形缓冲，命中就立刻剔除，绝不再写。
struct InvFingerprint {
  uint32_t unit_data = 0;  // GameObject+4：ThingTemplate*，对象被复用后必然不同
  uint32_t hp = 0;         // GameObject+0x33C：血量对象
  float hp_cap = 0.f;      // 血量对象+0x10：血量上限（升级/科技会微调，容差 2%）
};
static std::map<uint32_t, InvFingerprint> g_inv_fp;

constexpr int kInvDeadRing = 256;
static uint32_t g_inv_dead_ent[kInvDeadRing] = {};
static DWORD g_inv_dead_at[kInvDeadRing] = {};
static volatile LONG g_inv_dead_seq = 0;

// 由销毁钩子调用（游戏逻辑线程），只做定长环形写入，不分配内存。
void invincible_note_destroyed(uint32_t ent) {
  if (!is_ptr(ent)) return;
  const LONG n = InterlockedIncrement(&g_inv_dead_seq) - 1;
  const int slot = static_cast<int>(static_cast<uint32_t>(n) % (uint32_t)kInvDeadRing);
  g_inv_dead_ent[slot] = ent;
  g_inv_dead_at[slot] = GetTickCount();
}

static bool inv_ent_recently_destroyed(uint32_t ent) {
  const DWORD now = GetTickCount();
  for (int i = 0; i < kInvDeadRing; ++i) {
    if (g_inv_dead_ent[i] != ent) continue;
    if (static_cast<int32_t>(now - g_inv_dead_at[i]) < 4000) return true;
  }
  return false;
}

static void inv_fp_capture(uint32_t ent, InvFingerprint* fp) {
  if (!fp) return;
  *fp = InvFingerprint{};
  uint32_t ud = 0, hp = 0;
  if (safe_read_u32(ent + 4, &ud) && is_ptr(ud)) fp->unit_data = ud;
  if (safe_read_u32(ent + ent_off(kEntHp, kEntHpEp1), &hp) && is_ptr(hp)) {
    fp->hp = hp;
    float cap = 0.f;
    if (safe_read_f32(hp + 0x10, &cap) && cap > 0.f) fp->hp_cap = cap;
  }
}

// 目标还活着吗？——不是问引擎，而是问"这块内存还是当初那个单位吗"。
static bool inv_fp_ok(uint32_t ent, const InvFingerprint& fp) {
  if (!is_ptr(ent) || !fp.unit_data || !fp.hp) return false;
  if (inv_ent_recently_destroyed(ent)) return false;
  uint32_t ud = 0, hp = 0;
  if (!safe_read_u32(ent + 4, &ud) || ud != fp.unit_data) return false;  // 已被复用
  if (!safe_read_u32(ent + ent_off(kEntHp, kEntHpEp1), &hp) || hp != fp.hp) return false;
  float cap = 0.f, cur = 0.f;
  if (!safe_read_f32(fp.hp + 0x10, &cap) || !(cap > 0.f && cap <= kHpSaneMax)) return false;
  if (fp.hp_cap > 0.f) {
    const float tol = fp.hp_cap * 0.02f + 1.f;
    if (std::fabs(cap - fp.hp_cap) > tol) return false;
  }
  if (!safe_read_f32(fp.hp + 4, &cur) || !(cur >= 0.f && cur <= kHpSaneMax)) return false;
  return true;
}

// 补血前先过指纹；已经满到锁值就不重复写（写一次就少一次踩空的机会）。
static bool inv_heal(uint32_t ent, const InvFingerprint& fp) {
  if (!inv_fp_ok(ent, fp)) return false;
  float cur = 0.f;
  if (safe_read_f32(fp.hp + 4, &cur) && cur >= kHpLockValue) return true;
  return safe_write_f32(fp.hp + 4, kHpLockValue) && safe_write_f32(fp.hp + 0xC, kHpLockValue);
}

// ---- 第二层：引擎自带的无敌 AttributeModifier ----
// 锁血只挡"走伤害计算"的攻击。鬼王冲撞 / 碾压这类直接摧毁的路径根本不看血量，
// 只有引擎自己的 AttributeModifier_InvulnerabilityCrate（100% 免伤、无外观）能挡。
// 这里的施加/解除都走引擎函数（特征码定位，两个版本通用），施加时 frames = -1
// 表示永久——就是引擎里所有长期修饰器的用法（反汇编里见到的调用都是 push -1）。
// 解析不到修饰器 / API 时静默退回纯锁血，绝不因为这条通道失败就取消无敌。
static std::map<uint32_t, bool> g_inv_mod_applied;  // ent -> 已成功施加
static uint32_t g_inv_mod_true = 0;                 // 修饰器对象
static bool g_inv_mod_resolved = false;
static DWORD g_inv_mod_retry = 0;
static char g_inv_mod_why[160] = {};

static uint32_t inv_true_invuln_modifier() {
  if (g_inv_mod_true) return g_inv_mod_true;
  const DWORD now = GetTickCount();
  if (g_inv_mod_resolved && static_cast<int32_t>(now - g_inv_mod_retry) < 20000) return 0;
  g_inv_mod_resolved = true;
  g_inv_mod_retry = now;
  char why[128] = {};
  const uint32_t mod = resolve_invuln_modifier(1, why, sizeof(why));  // 1 = InvulnerabilityCrate
  std::snprintf(g_inv_mod_why, sizeof(g_inv_mod_why), "%s", why[0] ? why : "unknown");
  if (!mod) {
    log("invincible: true-invuln unavailable (%s)", g_inv_mod_why);
    return 0;
  }
  g_inv_mod_true = mod;
  log("invincible: true-invuln modifier=%08X (%s)", (unsigned)mod, g_inv_mod_why);
  return mod;
}

static bool inv_true_invuln_apply(uint32_t ent) {
  auto it = g_inv_mod_applied.find(ent);
  if (it != g_inv_mod_applied.end() && it->second) return true;
  const uint32_t mod = inv_true_invuln_modifier();
  if (!mod || !is_ptr(ent)) return false;
  const bool ok = modifier_invoke(ent, mod, -1, false, false);
  g_inv_mod_applied[ent] = ok;
  log("invincible: true-invuln apply ent=%08X -> %s", (unsigned)ent, ok ? "ok" : "failed");
  return ok;
}

static void inv_true_invuln_remove(uint32_t ent) {
  auto it = g_inv_mod_applied.find(ent);
  if (it == g_inv_mod_applied.end()) return;
  const bool applied = it->second;
  g_inv_mod_applied.erase(it);
  if (!applied || !g_inv_mod_true) return;
  const bool ok = modifier_invoke(ent, g_inv_mod_true, 0, true, false);
  log("invincible: true-invuln remove ent=%08X -> %s", (unsigned)ent, ok ? "ok" : "failed");
}

// 定义在文件后面（战局时钟），这里只需要判断「现在是不是在打」。
static bool read_battle_clock(uint32_t* logic_out, float* seconds_out);

static void invincible_clear_targets() {
  g_inv_targets.clear();
  g_inv_fp.clear();
  g_inv_mod_applied.clear();
  g_inv_active = false;
  g_inv_next_tick = 0;
  g_inv_last_hit = 0;
  g_inv_off_battle_tick = 0;
}

// 把指定单位从锁血名单里摘掉：「残血(1点)」要能在无敌状态下直接生效。
// 名单空了就把无敌整体关掉（状态文字也跟着变回「待命」）。
static int invincible_release_entities(const std::vector<uint32_t>& ents) {
  if (!g_inv_active || g_inv_targets.empty() || ents.empty()) return 0;
  const size_t before = g_inv_targets.size();
  for (uint32_t e : ents) {
    g_inv_targets.erase(std::remove(g_inv_targets.begin(), g_inv_targets.end(), e),
                        g_inv_targets.end());
    g_inv_fp.erase(e);
  }
  const int released = (int)(before - g_inv_targets.size());
  if (released <= 0) return 0;
  log("invincible: released %d unit(s) for hp_min, %d left", released,
      (int)g_inv_targets.size());
  if (g_inv_targets.empty()) invincible_clear_targets();
  return released;
}

// on = 先把当前选中单位血量拉满，再锁住；off = 恢复原血量并清空名单。
bool invincible_apply_impl(bool on, float seconds, int flavor, std::string* out_msg) {
  (void)seconds;
  (void)flavor;

  if (!on) {
    // 解除：把名单里的单位恢复成正常血量，再清空（不碰其它单位）。
    // 恢复同样要过指纹：已经死掉/被复用的地址一律跳过，别再写第二遍。
    int restored = 0;
    int skipped = 0;
    for (uint32_t ent : g_inv_targets) {
      auto it = g_inv_fp.find(ent);
      if (it == g_inv_fp.end() || !inv_fp_ok(ent, it->second)) {
        ++skipped;
        continue;
      }
      if (write_entity_hp(ent, "normal")) ++restored;
      inv_true_invuln_remove(ent);  // 引擎无敌一起摘掉（已过指纹，地址还安全）
    }
    const int had = (int)g_inv_targets.size();
    invincible_clear_targets();
    log("invincible: released %d units (restored %d, skipped %d)", had, restored, skipped);
    if (out_msg) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), u8"已解除无敌（%d 个单位）", had);
      *out_msg = buf;
    }
    return true;
  }

  // 只取「当前选中」；之前已经锁住的继续保留（多次点 = 累加）。
  const std::vector<uint32_t> sel = selected_entities_stable(256);
  std::vector<uint32_t> targets = g_inv_targets;
  int added = 0;
  int maxed = 0;
  for (uint32_t ent : sel) {
    if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
    // 先记指纹再写：hp+0x10（血量上限）不会被这次写入改动，
    // 但拿不到血量对象就别碰这个单位，免得之后照着野指针补血。
    InvFingerprint fp;
    inv_fp_capture(ent, &fp);
    if (!fp.hp) continue;
    // 第一步：血量直接拉到最大。
    if (!write_entity_hp(ent, "max")) continue;
    ++maxed;
    if (std::find(targets.begin(), targets.end(), ent) == targets.end()) {
      targets.push_back(ent);
      ++added;
    }
    g_inv_fp[ent] = fp;
    // 第二层：能解析到引擎的无敌修饰器就顺手施加（失败不影响锁血）。
    inv_true_invuln_apply(ent);
  }
  if (targets.empty()) {
    if (out_msg) *out_msg = u8"无敌：请先在游戏里选中要保护的单位";
    return false;
  }
  // 第二步：开始锁血（tick 每 50ms 把名单里的血续回最大值）。
  g_inv_targets = targets;
  g_inv_active = true;
  g_inv_last_hit = (int)targets.size();
  g_inv_next_tick = 0;
  log("invincible: +%d new, total=%d, maxed=%d", added, (int)targets.size(), maxed);
  if (out_msg) {
    char buf[220];
    if (added > 0) {
      std::snprintf(buf, sizeof(buf), u8"选中的 %d 个单位已满血并锁血（累计 %d 个，换选不会跟着变）",
                    added, (int)targets.size());
    } else {
      std::snprintf(buf, sizeof(buf), u8"选中的单位已在无敌名单里（累计 %d 个）",
                    (int)targets.size());
    }
    *out_msg = buf;
  }
  return true;
}

bool invincible_lock_impl() { return g_inv_active; }

// `[` 热键：只是「把当前选中加进无敌名单」，不再一刀切当成总开关。
// 老实现是「已开启就全部解除」，于是选中第二个单位再按 `[` 会把第一个解掉、
// 第二个又没加进去。现在：
//   * 选中里只要还有没锁过的单位 → 累加（前面的单位继续锁）；
//   * 选中的单位全都已经在名单里（对同一批再按一次）→ 只解掉这批。
static bool invincible_toggle_impl(std::string* out_msg) {
  const std::vector<uint32_t> sel = selected_entities_stable(256);
  bool any_unlocked = false;
  for (uint32_t ent : sel) {
    if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
    if (std::find(g_inv_targets.begin(), g_inv_targets.end(), ent) == g_inv_targets.end()) {
      any_unlocked = true;
      break;
    }
  }
  if (!g_inv_active || any_unlocked) return invincible_apply_impl(true, 600.f, 0, out_msg);

  // 这批已经在名单里：只把它们解出来，别的单位继续无敌。
  int restored = 0;
  int released = 0;
  for (uint32_t ent : sel) {
    auto it = g_inv_fp.find(ent);
    if (it != g_inv_fp.end() && inv_fp_ok(ent, it->second)) {
      if (write_entity_hp(ent, "normal")) ++restored;
      inv_true_invuln_remove(ent);
    }
    const size_t before = g_inv_targets.size();
    g_inv_targets.erase(std::remove(g_inv_targets.begin(), g_inv_targets.end(), ent),
                        g_inv_targets.end());
    g_inv_fp.erase(ent);
    if (g_inv_targets.size() != before) ++released;
  }
  if (g_inv_targets.empty()) invincible_clear_targets();
  log("invincible: toggle released %d selected (restored %d), %d left", released, restored,
      (int)g_inv_targets.size());
  if (out_msg) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), u8"已把这 %d 个单位解除无敌（名单还剩 %d 个）", released,
                  (int)g_inv_targets.size());
    *out_msg = buf;
  }
  return released > 0;
}

// 每 50ms 给名单里的单位补满血；死掉的自动剔除，不会碰其它单位。
void invincible_tick_impl() {
  if (!g_inv_active) return;
  // 不在战局里（结算 / 主菜单）就收摊，否则旧指针会被下一局复用。
  // 5 秒宽限，避免进场瞬间时钟抖动一下就把名单丢了。
  if (!read_battle_clock(nullptr, nullptr)) {
    const DWORD now_off = GetTickCount();
    if (!g_inv_off_battle_tick) {
      g_inv_off_battle_tick = now_off ? now_off : 1;
      return;
    }
    if (static_cast<int32_t>(now_off - g_inv_off_battle_tick) < 5000) return;
    log("invincible: left battle, dropping targets");
    invincible_clear_targets();
    return;
  }
  g_inv_off_battle_tick = 0;

  const DWORD now = GetTickCount();
  if (g_inv_next_tick && static_cast<int32_t>(now - g_inv_next_tick) < 0) return;
  g_inv_next_tick = now + 50;

  int healed = 0;
  int dropped = 0;
  std::vector<uint32_t> alive;
  alive.reserve(g_inv_targets.size());
  for (uint32_t ent : g_inv_targets) {
    auto it = g_inv_fp.find(ent);
    // 死了 / 被摧毁 / 内存已被别的对象复用 → 立刻摘掉，这一轮绝不写。
    if (it == g_inv_fp.end() || !inv_fp_ok(ent, it->second)) {
      ++dropped;
      continue;
    }
    alive.push_back(ent);
    if (inv_heal(ent, it->second)) ++healed;   // 续回锁值，血量不掉
  }
  if (alive.size() != g_inv_targets.size()) {
    g_inv_targets.swap(alive);
    for (auto it = g_inv_fp.begin(); it != g_inv_fp.end();) {
      if (std::find(g_inv_targets.begin(), g_inv_targets.end(), it->first) ==
          g_inv_targets.end()) {
        it = g_inv_fp.erase(it);
      } else {
        ++it;
      }
    }
    // 死掉的目标不用再解除修饰器（对象都没了，指针也不能碰），只清记录。
    for (auto it = g_inv_mod_applied.begin(); it != g_inv_mod_applied.end();) {
      if (std::find(g_inv_targets.begin(), g_inv_targets.end(), it->first) ==
          g_inv_targets.end()) {
        it = g_inv_mod_applied.erase(it);
      } else {
        ++it;
      }
    }
  }
  g_inv_last_hit = healed;
  if (dropped > 0) {
    log("invincible: dropped %d dead/reused target(s), %d left", dropped,
        (int)g_inv_targets.size());
  }
  if (g_inv_targets.empty()) {
    log("invincible: all targets gone, stopping");
    invincible_clear_targets();
    return;
  }
  if (static_cast<int32_t>(now - g_inv_log_tick) > 10000) {
    g_inv_log_tick = now;
    log("invincible: holding %d units", healed);
  }
}

const char* invincible_status_impl() {
  static char buf[192];
  int real = 0;
  for (const auto& kv : g_inv_mod_applied) {
    if (kv.second) ++real;
  }
  std::snprintf(buf, sizeof(buf), u8"%s · %s · 护住 %d 个单位",
                g_inv_active ? u8"已开启" : u8"待命",
                real > 0 ? u8"真无敌" : u8"锁血",
                g_inv_active ? (int)g_inv_targets.size() : 0);
  return buf;
}


// Cached player-relative entity vector offsets (units + buildings often separate).
// High bit set => vector lives on [player+0x40] + (off&~hi).
constexpr int kMaxEntVecOffs = 8;
static uint32_t g_ent_vec_offs[kMaxEntVecOffs] = {};
static int g_ent_vec_off_count = 0;
static uint32_t g_roster_local_id = 0;
static uint32_t g_logic_list_off = 0;
constexpr int kMaxLogicLists = 16;
static uint32_t g_logic_offs[kMaxLogicLists] = {};
static int g_logic_off_n = 0;
static int g_logic_refresh = 0;

static bool valid_ent_vector_range(uint32_t begin, uint32_t end, uint32_t* out_n) {
  if (!is_ptr(begin) || !is_ptr(end) || end <= begin) return false;
  const uint32_t bytes = end - begin;
  if (bytes & 3u) return false;
  const uint32_t n = bytes / 4u;
  if (n < 1 || n > 65536) return false;
  if (out_n) *out_n = n;
  return true;
}

struct EntVecScore {
  int score = 0;
  int owners = 0;
  int owned = 0;
  uint32_t begin = 0;
};

// Score: owned-by-player matches, or multi-owner global lists.
static EntVecScore score_entity_vector(uint32_t begin, uint32_t end, uint32_t player,
                                       uint32_t player_id) {
  EntVecScore r{};
  uint32_t n = 0;
  if (!valid_ent_vector_range(begin, end, &n)) return r;
  r.begin = begin;
  uint32_t seen_ow[12] = {};
  int nown = 0;
  int any = 0;
  const uint32_t sample = n < 32 ? n : 32;
  for (uint32_t i = 0; i < sample; ++i) {
    uint32_t ent = 0;
    if (!safe_read_u32(begin + i * 4u, &ent)) continue;
    if (!looks_like_entity(ent)) continue;
    ++any;
    uint32_t ow = 0;
    safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow);
    if (is_ptr(ow) && nown < 12) {
      bool dup = false;
      for (int k = 0; k < nown; ++k) {
        if (seen_ow[k] == ow) {
          dup = true;
          break;
        }
      }
      if (!dup) seen_ow[nown++] = ow;
    }
    if (is_ptr(player) && entity_belongs_to(ent, player, player_id)) ++r.owned;
  }
  r.owners = nown;
  if (r.owned > 0) r.score = r.owned * 10 + nown;
  else if (nown >= 2 && any >= 4) r.score = any + nown * 3;
  else if (any >= 6) r.score = any;
  return r;
}

static void remember_ent_vec_off(uint32_t off) {
  if (!off) return;
  for (int i = 0; i < g_ent_vec_off_count; ++i) {
    if (g_ent_vec_offs[i] == off) return;
  }
  if (g_ent_vec_off_count >= kMaxEntVecOffs) return;
  g_ent_vec_offs[g_ent_vec_off_count++] = off;
}

// Collect up to kMaxEntVecOffs distinct high-scoring vectors (units + buildings).
static int discover_entity_vector_offs_for(uint32_t player, uint32_t player_id,
                                          uint32_t* out_offs, int max_out) {
  if (!is_ptr(player) || !out_offs || max_out <= 0) return 0;

  struct Cand {
    uint32_t off = 0;
    uint32_t begin = 0;
    int score = 0;
    int owned = 0;
  };
  Cand cands[32];
  int nc = 0;

  auto consider = [&](uint32_t off, uint32_t begin, uint32_t end) {
    const EntVecScore sc = score_entity_vector(begin, end, player, player_id);
    // Keep weaker owned lists too (building lists are often smaller than armies).
    if (sc.score < 3 && sc.owned < 1) return;
    if (sc.owned < 1 && sc.score < 8) return;
    for (int i = 0; i < nc; ++i) {
      if (cands[i].begin == begin) {
        if (sc.score > cands[i].score) {
          cands[i].off = off;
          cands[i].score = sc.score;
          cands[i].owned = sc.owned;
        }
        return;
      }
    }
    if (nc >= 32) return;
    cands[nc++] = Cand{off, begin, sc.score, sc.owned};
  };

  for (uint32_t off = 0x40; off <= 0x600; off += 4) {
    uint32_t begin = 0, end = 0;
    if (!safe_read_u32(player + off, &begin)) continue;
    if (!safe_read_u32(player + off + 4, &end)) continue;
    consider(off, begin, end);
  }
  uint32_t mid = 0;
  if (safe_read_u32(player + 0x40, &mid) && is_ptr(mid)) {
    for (uint32_t off = 0; off <= 0x200; off += 4) {
      uint32_t begin = 0, end = 0;
      if (!safe_read_u32(mid + off, &begin)) continue;
      if (!safe_read_u32(mid + off + 4, &end)) continue;
      consider(0x80000000u | off, begin, end);
    }
  }

  std::sort(cands, cands + nc, [](const Cand& a, const Cand& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.owned > b.owned;
  });

  int got = 0;
  for (int i = 0; i < nc && got < max_out; ++i) {
    out_offs[got++] = cands[i].off;
  }
  return got;
}

// Per-player cached vector offsets (avoid full scan every frame).
struct PlayerVecCache {
  uint32_t player = 0;
  uint32_t offs[kMaxEntVecOffs] = {};
  int count = 0;
};
static PlayerVecCache g_player_vec_cache[kMaxEconPlayers];
static int g_player_vec_cache_n = 0;

static int cached_offs_for_player(uint32_t player, uint32_t player_id, uint32_t* out_offs,
                                  int max_out) {
  if (!is_ptr(player) || !out_offs || max_out <= 0) return 0;
  for (int i = 0; i < g_player_vec_cache_n; ++i) {
    if (g_player_vec_cache[i].player == player && g_player_vec_cache[i].count > 0) {
      int n = g_player_vec_cache[i].count;
      if (n > max_out) n = max_out;
      for (int k = 0; k < n; ++k) out_offs[k] = g_player_vec_cache[i].offs[k];
      return n;
    }
  }
  uint32_t found[kMaxEntVecOffs] = {};
  const int nf = discover_entity_vector_offs_for(player, player_id, found, kMaxEntVecOffs);
  if (nf <= 0) return 0;
  if (g_player_vec_cache_n < kMaxEconPlayers) {
    PlayerVecCache& c = g_player_vec_cache[g_player_vec_cache_n++];
    c.player = player;
    c.count = nf;
    for (int k = 0; k < nf; ++k) c.offs[k] = found[k];
  }
  int n = nf;
  if (n > max_out) n = max_out;
  for (int k = 0; k < n; ++k) out_offs[k] = found[k];
  return n;
}

static bool resolve_vector_off(uint32_t player, uint32_t off, uint32_t* begin,
                               uint32_t* end) {
  if (!is_ptr(player) || !begin || !end) return false;
  uint32_t base = player;
  uint32_t o = off;
  if (o & 0x80000000u) {
    if (!safe_read_u32(player + 0x40, &base) || !is_ptr(base)) return false;
    o &= 0x7FFFFFFFu;
  }
  return safe_read_u32(base + o, begin) && safe_read_u32(base + o + 4, end) &&
         valid_ent_vector_range(*begin, *end, nullptr);
}

static int collect_ents_from_vector(uint32_t begin, uint32_t end, uint32_t player,
                                    uint32_t player_id, uint32_t* out, int max_out,
                                    bool require_owner) {
  uint32_t n = 0;
  if (!valid_ent_vector_range(begin, end, &n) || !out || max_out <= 0) return 0;
  int got = 0;
  for (uint32_t i = 0; i < n && got < max_out; ++i) {
    uint32_t ent = 0;
    if (!safe_read_u32(begin + i * 4u, &ent)) continue;
    if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
    if (require_owner && !entity_belongs_to(ent, player, player_id)) continue;
    out[got++] = ent;
  }
  return got;
}

// Sticky roster per player: dampens incomplete scans (esp. when armies grow).
// - new type / count up → apply immediately, append at end
// - count down / missing → keep previous until several consecutive lows
// - incomplete *category* (units vs buildings scanned separately) → never drop
constexpr int kRosterHoldPolls = 2;     // one extra poll, then accept the new count
constexpr int kRosterEggHoldPolls = 1;  // cores unpack on the next poll

struct RosterStickySlot {
  char key[96] = {};
  char name[64] = {};
  bool building = false;
  int shown = 0;
  int last_raw = 0;
  int low_streak = 0;
};

struct RosterOrderCache {
  uint32_t player = 0;
  uint32_t player_id = 0;
  int count = 0;
  int last_raw_units = 0;
  int last_raw_bld = 0;
  int unit_miss = 0;
  int bld_miss = 0;
  RosterStickySlot slots[kMaxRosterTypes] = {};
};
static RosterOrderCache g_roster_order[kMaxEconPlayers];
static int g_roster_order_n = 0;

struct EntIdent {
  char key[96] = {};
  char disp[64] = {};
  bool building = false;
};
static std::map<uint32_t, EntIdent> g_ent_ident;

// Stable TypeId per template pointer (resolver can otherwise jitter).
static std::map<uint32_t, std::string> g_tpl_type_cache;
static CRITICAL_SECTION g_tpl_cs;
static volatile LONG g_tpl_cs_state = 0;  // 0 uninit, 1 initializing, 2 ready

static void ensure_tpl_cs() {
  const LONG s = InterlockedCompareExchange(&g_tpl_cs_state, 1, 0);
  if (s == 0) {
    InitializeCriticalSection(&g_tpl_cs);
    InterlockedExchange(&g_tpl_cs_state, 2);
    return;
  }
  while (g_tpl_cs_state != 2) Sleep(0);
}

struct LiveEnt {
  uint32_t owner = 0;
  uint32_t pending_owner = 0;
  uint32_t unit_data = 0;  // ThingTemplate*; detect unpack / morph
  int pending_hits = 0;
  int seen_gen = 0;   // last object-list scan that still contained this ent
  int dead_streak = 0;  // consecutive polls with HP≈0
  char key[96] = {};
  char name[64] = {};
  bool building = false;
};
static std::map<uint32_t, LiveEnt> g_live;
// Fixed queues so Create/Spawn/Destroy never allocate on the game thread.
// A save load allocates tens of thousands of objects; a std::vector growth
// there can exhaust the 32-bit heap and the game then crashes on a null alloc.
constexpr int kLiveQueueCap = 8192;
static uint32_t g_add_q[kLiveQueueCap] = {};
static uint32_t g_add_hint[kLiveQueueCap] = {};
static int g_add_n = 0;
static uint32_t g_forget_q[kLiveQueueCap] = {};
static int g_forget_n = 0;
static CRITICAL_SECTION g_live_cs;
static bool g_live_cs_ready = false;
static bool g_live_seeded = false;
static int g_live_scan_tick = 0;

// 真无敌兜底用：从 g_live 里挑出属于本地玩家的存活单位。
int own_entities_snapshot(uint32_t* out, int max_out) {
  if (!out || max_out <= 0) return 0;
  const uint32_t me = local_owner_for_ops();
  if (!is_ptr(me)) return 0;
  int n = 0;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  for (const auto& kv : g_live) {
    if (n >= max_out) break;
    const uint32_t ent = kv.first;
    if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
    if (!entity_belongs_to(ent, me, 0)) continue;
    out[n++] = ent;
  }
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  return n;
}

// ---------------------------------------------------------------------------
// 归属切换的底层：Team / 玩家归属
//
// GameObject 上有两个互相关联的字段：+0x414 是 Team*，+0x418 是 Player*
// （Team::getPlayer() == [[team+4]+8]）。只写 +0x418 不动 +0x414 的话，
// 引擎随后会按 Team 里的玩家把 +0x418 覆盖回去，表现就是「改了等于没改」。
// 引擎自己的 GameObject::setTeam(Team*, int) 会同时刷新两个字段、挪动小队 /
// AI 链表并广播归属变更，所以改归属一律走它。
// ---------------------------------------------------------------------------
constexpr uint32_t kEntTeam = 0x414;          // Team*（原版 1.12）
constexpr uint32_t kFnSetTeam = 0x007CBF50;   // GameObject::setTeam(Team*, int)

static bool is_module_ptr(uint32_t v) {
  // 模块映像里的指针（虚表 / 代码 / 只读数据）。范围放宽到 32MB，避免因为
  // 段布局不同把合法的虚表判掉。
  const uint32_t base = loaded_module();
  if (!base || (v & 3u) != 0) return false;
  return v >= base + 0x1000 && v < base + 0x02000000u;
}

uint32_t ent_team_of(uint32_t ent) {
  if (!is_ptr(ent)) return 0;
  uint32_t t = 0;
  if (!safe_read_u32(ent + kEntTeam, &t) || !is_ptr(t)) return 0;
  return t;
}

// 一个 Team 是否属于该玩家：Team::getPlayer() = [[team+4]+8]。
bool team_owned_by(uint32_t team, uint32_t player) {
  if (!is_ptr(team) || !is_ptr(player)) return false;
  uint32_t vtable = 0, sub = 0, owner = 0;
  if (!safe_read_u32(team, &vtable) || !is_module_ptr(vtable)) return false;
  if (!safe_read_u32(team + 4, &sub) || !is_ptr(sub)) return false;
  if (!safe_read_u32(sub + 8, &owner)) return false;
  return owner == player;
}

// 找出该玩家在引擎里的 Team：先看地图上属于他的单位挂的 Team，再从玩家对象
// 里扫一遍指针。找不到返回 0（调用方退回「只写 +0x418」的旧行为）。
uint32_t player_team_of(uint32_t player) {
  if (!is_ptr(player) || uprising_active()) return 0;

  std::vector<uint32_t> all;
  if (g_live_cs_ready) {
    EnterCriticalSection(&g_live_cs);
    all.reserve(g_live.size());
    for (const auto& kv : g_live) all.push_back(kv.first);
    LeaveCriticalSection(&g_live_cs);
  }
  for (uint32_t ent : all) {
    if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
    if (ent_owner_of(ent) != player) continue;
    const uint32_t t = ent_team_of(ent);
    if (team_owned_by(t, player)) return t;
  }

  for (uint32_t off = 0x40; off <= 0x800; off += 4) {
    uint32_t p = 0;
    if (!safe_read_u32(player + off, &p) || !is_ptr(p)) continue;
    if (team_owned_by(p, player)) return p;
  }
  return 0;
}

// 调引擎的 setTeam：__thiscall(this=ent, team, 0)。放到子线程里跑，避免
// 在渲染线程里直接改游戏逻辑链表；调用前后各读一次 +0x418 判断是否真的生效。
bool engine_set_team(uint32_t ent, uint32_t team) {
  const uint32_t fn = va_of(kFnSetTeam);
  if (!fn || !is_ptr(ent) || !is_ptr(team) || uprising_active()) return false;
  CallJob job{};
  job.kind = CallJob::kThiscallArgs;
  job.fn = fn;
  job.thisptr = ent;
  job.args[0] = team;
  job.args[1] = 0;
  job.nargs = 2;
  return run_job(job);
}

// 把一个单位改到某个玩家名下，返回是否真的生效。
static bool assign_entity_to_player(uint32_t ent, uint32_t player, uint32_t team) {
  if (!is_ptr(ent) || !is_ptr(player)) return false;
  const uint32_t off = ent_off(kEntOwner, kEntOwnerEp1);
  if (team && engine_set_team(ent, team) && read_u32(ent + off) == player) return true;
  // 兜底：引擎调用没成立时，把 Team 和 Player 一起写上（不要只写 +0x418）。
  if (team && is_ptr(team)) {
    write_u32(ent + kEntTeam, team);
    write_u32(ent + off, player);
    if (read_u32(ent + off) == player) return true;
  }
  return write_u32(ent + off, player);
}

static int g_live_scan_gen = 0;
static bool g_roster_active = false;  // UI wants live roster (stats open)
// Mid-match open seed (amortized — NOT a 200ms sync burst on Present):
// 1) one cheap player-list walk that only enqueues unknown ents
// 2) Present drains ~12 catalog resolves per frame
// 3) board fills over ~1s without hitching. A sync 2048-resolve pass was
// measured at ~170ms/poll with 6800 budget misses and never finished.
static bool g_roster_seed_burst = false;
static bool g_seed_walk_done = false;     // at least one enqueue walk finished
static bool g_seed_queue_overflow = false;
static std::unordered_set<uint32_t> g_live_drop;  // ents that are not roster units
static std::unordered_map<uint32_t, uint8_t> g_live_retries;  // kRetry storms → drop
// After a match change or save load, keep rescanning. A long battle's object
// list is rebuilt in one burst and is easy to sample before it is complete.
static DWORD g_rescan_began = 0;
static DWORD g_roster_rescan_until = 0;
static DWORD g_roster_next_scan = 0;
static DWORD g_roster_next_light = 0;
static DWORD g_roster_next_rediscover = 0;
static int g_rescan_last_n = -1;
static int g_rescan_stable = 0;
static void queue_live_add(uint32_t ent, uint32_t hint) {
  if (!is_ptr(ent) || !g_live_cs_ready) return;
  EnterCriticalSection(&g_live_cs);
  if (g_live_drop.find(ent) != g_live_drop.end()) {
    LeaveCriticalSection(&g_live_cs);
    return;
  }
  // Already tracked with a TypeId — nothing to classify.
  auto known = g_live.find(ent);
  if (known != g_live.end() && known->second.key[0]) {
    LeaveCriticalSection(&g_live_cs);
    return;
  }
  // Dedup against the pending queue (open-panel seed can rewalk).
  for (int i = 0; i < g_add_n; ++i) {
    if (g_add_q[i] == ent) {
      if (is_ptr(hint) && !is_ptr(g_add_hint[i])) g_add_hint[i] = hint;
      LeaveCriticalSection(&g_live_cs);
      return;
    }
  }
  if (g_add_n < kLiveQueueCap) {
    g_add_q[g_add_n] = ent;
    g_add_hint[g_add_n] = hint;
    ++g_add_n;
  } else {
    g_seed_queue_overflow = true;
  }
  LeaveCriticalSection(&g_live_cs);
}

static void queue_live_forget(uint32_t ent) {
  if (!is_ptr(ent) || !g_live_cs_ready) return;
  EnterCriticalSection(&g_live_cs);
  for (int i = 0; i < g_add_n;) {
    if (g_add_q[i] != ent) {
      ++i;
      continue;
    }
    const int last = g_add_n - 1;
    g_add_q[i] = g_add_q[last];
    g_add_hint[i] = g_add_hint[last];
    g_add_n = last;
  }
  if (g_forget_n < kLiveQueueCap) g_forget_q[g_forget_n++] = ent;
  LeaveCriticalSection(&g_live_cs);
}

struct FacCache {
  uint32_t player = 0;
  int faction = 0;  // 1 allied, 2 soviet, 3 empire
  int pending = 0;
  int pending_hits = 0;
  uint32_t next_try = 0;  // throttle for the pointer walk when nothing matched
  int fails = 0;          // consecutive failed probes — drives the backoff
};
static FacCache g_fac_cache[16] = {};
static int g_fac_cache_n = 0;

// Players who have actually fought this match. Kept after defeat, when the
// game drops them from the active count or flags them as observers.
struct SeenSeat {
  uint32_t player = 0;
  PlayerEconomy last{};
};
static SeenSeat g_seen[kMaxEconPlayers] = {};
static int g_seen_n = 0;

static void clear_live_ents() {
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  g_live.clear();
  g_live_drop.clear();
  g_live_retries.clear();
  g_add_n = 0;
  g_forget_n = 0;
  g_live_seeded = false;
  g_live_scan_tick = 0;
  g_live_scan_gen = 0;
  g_roster_seed_burst = false;
  g_seed_walk_done = false;
  g_seed_queue_overflow = false;
  g_rescan_began = GetTickCount();
  // Soft rescan window: light scans after 4s, settle by ~6–8s. Avoid the old
  // pattern of rediscovering every 600ms with full catalog walks.
  g_roster_rescan_until = g_rescan_began + 8000;
  g_roster_next_scan = 0;
  g_roster_next_light = 0;
  g_roster_next_rediscover = 0;
  g_rescan_last_n = -1;
  g_rescan_stable = 0;
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
}

static void clear_seat_diag();

static void clear_roster_caches() {
  g_ent_vec_off_count = 0;
  g_player_vec_cache_n = 0;
  g_roster_order_n = 0;
  g_roster_local_id = 0;
  g_logic_list_off = 0;
  g_logic_off_n = 0;
  g_logic_refresh = 0;
  // Keep g_tpl_type_cache across battles — ThingTemplate TypeIds are stable for
  // the process lifetime. Clearing it on every new match forces thousands of
  // catalog walks during the spawn storm and hitch Present for seconds.
  g_ent_ident.clear();
  g_fac_cache_n = 0;
  g_seen_n = 0;
  std::memset(g_fac_cache, 0, sizeof(g_fac_cache));
  std::memset(g_seen, 0, sizeof(g_seen));
  std::memset(g_ent_vec_offs, 0, sizeof(g_ent_vec_offs));
  std::memset(g_player_vec_cache, 0, sizeof(g_player_vec_cache));
  std::memset(g_roster_order, 0, sizeof(g_roster_order));
  clear_seat_diag();
  clear_live_ents();
}

// True for the first seconds after a match load / battle reset. Spawn hooks
// and rediscover must stay cheap or the game freezes on Present.
static bool roster_early_load() {
  const DWORD now = GetTickCount();
  if (g_rescan_began == 0) return false;
  const int since = (int)(now - g_rescan_began);
  if (since < 0 || since >= 5000) return false;
  return !g_live_seeded || (int)(g_roster_rescan_until - now) > 0;
}

static RosterOrderCache* roster_order_for(uint32_t player, uint32_t player_id) {
  if (!is_ptr(player)) return nullptr;
  for (int i = 0; i < g_roster_order_n; ++i) {
    if (g_roster_order[i].player == player) {
      g_roster_order[i].player_id = player_id;
      return &g_roster_order[i];
    }
  }
  if (g_roster_order_n >= kMaxEconPlayers) return nullptr;
  RosterOrderCache& c = g_roster_order[g_roster_order_n++];
  c = RosterOrderCache{};
  c.player = player;
  c.player_id = player_id;
  return &c;
}

static bool roster_key_unstable(const char* key) {
  return key && (std::strncmp(key, "sig_", 4) == 0 || std::strncmp(key, "tpl_", 4) == 0);
}

static bool roster_type_live(const char* key, const char* name) {
  // Japan *Egg cores and finished buildings must drop as soon as they unpack / finish.
  if (key && std::strstr(key, "Egg")) return true;
  if (name && std::strstr(name, u8"核心")) return true;
  return false;
}

static int faction_of_key(const char* key);

static void add_roster_count(PlayerEconomy* pe, const char* key, const char* disp,
                             bool building) {
  if (!pe || !key || !key[0] || !disp || !disp[0]) return;
  // Prefer merging by display name so TypeId jitter doesn't split one unit type.
  for (int i = 0; i < pe->roster_count; ++i) {
    const bool key_hit = (std::strcmp(pe->roster[i].key, key) == 0);
    const int fac_key = faction_of_key(key);
    const int fac_row = faction_of_key(pe->roster[i].key);
    const bool same_fac = fac_key == 0 || fac_row == 0 || fac_key == fac_row;
    const bool name_hit =
        same_fac && pe->roster[i].name[0] && disp[0] &&
        std::strcmp(pe->roster[i].name, disp) == 0 && pe->roster[i].is_building == building;
    if (!key_hit && !name_hit) continue;
    pe->roster[i].count++;
    if (key_hit) {
      if (!pe->roster[i].name[0]) {
        std::snprintf(pe->roster[i].name, sizeof(pe->roster[i].name), "%s", disp);
      }
    } else {
      // Same display name under a better (stable) TypeId → upgrade key.
      if (roster_key_unstable(pe->roster[i].key) && !roster_key_unstable(key)) {
        std::snprintf(pe->roster[i].key, sizeof(pe->roster[i].key), "%s", key);
      }
    }
    return;
  }
  if (pe->roster_count >= kMaxRosterTypes) return;
  RosterType& t = pe->roster[pe->roster_count++];
  std::snprintf(t.key, sizeof(t.key), "%s", key);
  std::snprintf(t.name, sizeof(t.name), "%s", disp);
  t.count = 1;
  t.is_building = building;
}

static void build_roster_text(PlayerEconomy* pe) {
  if (!pe) return;
  pe->roster_text[0] = 0;
  pe->unit_total = 0;
  pe->building_total = 0;
  for (int i = 0; i < pe->roster_count; ++i) {
    if (pe->roster[i].count <= 0) continue;
    if (pe->roster[i].is_building) pe->building_total += pe->roster[i].count;
    else pe->unit_total += pe->roster[i].count;
  }

  size_t used = 0;
  for (int i = 0; i < pe->roster_count; ++i) {
    if (pe->roster[i].count <= 0) continue;
    char piece[80];
    std::snprintf(piece, sizeof(piece), "%s%s %d", (used ? "  " : ""),
                  pe->roster[i].name, pe->roster[i].count);
    const size_t plen = std::strlen(piece);
    if (used + plen + 1 >= sizeof(pe->roster_text)) break;
    std::memcpy(pe->roster_text + used, piece, plen + 1);
    used += plen;
  }
}

static void build_roster_text_unused_sticky(PlayerEconomy* pe) {
  if (!pe) return;
  pe->roster_text[0] = 0;

  RosterOrderCache* ord = roster_order_for(pe->player, pe->player_id);
  if (ord) {
    int raw_units = 0, raw_bld = 0;
    for (int j = 0; j < pe->roster_count; ++j) {
      if (pe->roster[j].count <= 0) continue;
      if (pe->roster[j].is_building) raw_bld += pe->roster[j].count;
      else raw_units += pe->roster[j].count;
    }
    int shown_units = 0, shown_bld = 0;
    for (int i = 0; i < ord->count; ++i) {
      if (ord->slots[i].shown <= 0) continue;
      if (ord->slots[i].building) shown_bld += ord->slots[i].shown;
      else shown_units += ord->slots[i].shown;
    }

    if (raw_units <= 0 && shown_units >= 2) ord->unit_miss++;
    else ord->unit_miss = 0;
    if (raw_bld <= 0 && shown_bld >= 2) ord->bld_miss++;
    else ord->bld_miss = 0;
    // Swallow a single empty scan. The next one is a real wipe and updates.
    const bool hole_units = (ord->unit_miss == 1);
    const bool hole_bld = (ord->bld_miss == 1);
    if (raw_units > 0) ord->last_raw_units = raw_units;
    if (raw_bld > 0) ord->last_raw_bld = raw_bld;

    bool seen[kMaxRosterTypes] = {};

    for (int j = 0; j < pe->roster_count; ++j) {
      const RosterType& raw = pe->roster[j];
      if (raw.count <= 0 || !raw.key[0]) continue;

      int idx = -1;
      for (int i = 0; i < ord->count; ++i) {
        if (std::strcmp(ord->slots[i].key, raw.key) == 0) {
          idx = i;
          break;
        }
      }
      if (idx < 0 && raw.name[0]) {
        for (int i = 0; i < ord->count; ++i) {
          if (ord->slots[i].name[0] &&
              std::strcmp(ord->slots[i].name, raw.name) == 0 &&
              ord->slots[i].building == raw.is_building) {
            idx = i;
            break;
          }
        }
      }
      if (idx < 0) {
        if (ord->count >= kMaxRosterTypes) continue;
        idx = ord->count++;
        RosterStickySlot& s = ord->slots[idx];
        s = RosterStickySlot{};
        std::snprintf(s.key, sizeof(s.key), "%s", raw.key);
        std::snprintf(s.name, sizeof(s.name), "%s", raw.name);
        s.building = raw.is_building;
        s.shown = raw.count;
        s.last_raw = raw.count;
        s.low_streak = 0;
        seen[idx] = true;
        continue;
      }

      seen[idx] = true;
      RosterStickySlot& s = ord->slots[idx];
      if (raw.name[0]) {
        std::snprintf(s.name, sizeof(s.name), "%s", raw.name);
      }
      if (roster_key_unstable(s.key) && !roster_key_unstable(raw.key)) {
        std::snprintf(s.key, sizeof(s.key), "%s", raw.key);
      }
      // Do not flip unit↔building — that makes the two sections flash.
      s.last_raw = raw.count;
      const bool hole = s.building ? hole_bld : hole_units;
      if (raw.count >= s.shown) {
        s.shown = raw.count;
        s.low_streak = 0;
      } else if (!hole) {
        s.low_streak++;
        const int hold = roster_type_live(s.key, s.name) ? kRosterEggHoldPolls
                                                         : kRosterHoldPolls;
        if (s.low_streak >= hold) {
          s.shown = raw.count;
          s.low_streak = 0;
        }
      }
    }

    for (int i = 0; i < ord->count;) {
      if (seen[i]) {
        ++i;
        continue;
      }
      RosterStickySlot& s = ord->slots[i];
      if (s.building ? hole_bld : hole_units) {
        ++i;
        continue;
      }
      s.low_streak++;
      const int hold = roster_type_live(s.key, s.name) ? kRosterEggHoldPolls
                                                       : kRosterHoldPolls;
      if (s.low_streak < hold) {
        ++i;
        continue;
      }
      for (int k = i; k + 1 < ord->count; ++k) {
        ord->slots[k] = ord->slots[k + 1];
        seen[k] = seen[k + 1];
      }
      --ord->count;
    }

    pe->unit_total = 0;
    pe->building_total = 0;
    pe->roster_count = 0;
    for (int i = 0; i < ord->count && pe->roster_count < kMaxRosterTypes; ++i) {
      const RosterStickySlot& s = ord->slots[i];
      if (s.shown <= 0 || !s.key[0]) continue;
      RosterType& t = pe->roster[pe->roster_count++];
      std::snprintf(t.key, sizeof(t.key), "%s", s.key);
      std::snprintf(t.name, sizeof(t.name), "%s", s.name[0] ? s.name : s.key);
      t.count = s.shown;
      t.is_building = s.building;
      if (s.building) pe->building_total += s.shown;
      else pe->unit_total += s.shown;
    }
  }

  size_t used = 0;
  for (int i = 0; i < pe->roster_count; ++i) {
    char piece[80];
    std::snprintf(piece, sizeof(piece), "%s%s %d", (used ? "  " : ""),
                  pe->roster[i].name, pe->roster[i].count);
    const size_t plen = std::strlen(piece);
    if (used + plen + 1 >= sizeof(pe->roster_text)) break;
    std::memcpy(pe->roster_text + used, piece, plen + 1);
    used += plen;
  }
}

static bool should_hide_roster_type(const char* key) {
  if (!key || !key[0]) return false;
  // Carrier-launched fighters clutter the roster; skip.
  if (id_has_token(key, "AttackDron")) return true;  // AttackDrone / AttackDron
  if (_stricmp(key, "AlliedAttackDrone") == 0) return true;
  // Top-secret chronosphere / satellite sweep helper objects.
  if (id_has_token(key, "SatelliteSweep")) return true;
  // Empire point-defense drones are spawned by the tower, not a fielded unit.
  if (id_has_token(key, "PointDefense")) return true;
  // BFI also drops helper / UI ghosts: return-to-base markers, repair pads,
  // power-plant turbines, superweapon timer shells, upgrade stubs.
  if (id_has_token(key, "ReturnToAirfield") || id_has_token(key, "ReturnToBase")) {
    return true;
  }
  if (id_has_token(key, "RepairBay") || id_has_token(key, "Turbine")) return true;
  if (id_has_token(key, "SuperWeaponTimer")) return true;
  if (id_has_token(key, "BaseDefenseTransform")) return true;
  if (_strnicmp(key, "Upgrade", 7) == 0) return true;
  if (_strnicmp(key, "BuildAllied", 11) == 0 || _strnicmp(key, "BuildSoviet", 11) == 0 ||
      _strnicmp(key, "BuildJapan", 10) == 0) {
    return true;
  }
  return false;
}

static bool catalog_id_at(uint32_t addr, char* out, size_t out_len) {
  char tmp[96] = {};
  if (!safe_read_bytes(addr, tmp, sizeof(tmp) - 1)) return false;
  tmp[sizeof(tmp) - 1] = 0;
  int n = 0;
  while (n < (int)sizeof(tmp) - 1 && tmp[n]) ++n;
  if (n < 4 || n > 80) return false;
  tmp[n] = 0;
  const bool naval_miner = _strnicmp(tmp, "JapanMiner", 10) == 0 ||
                           _strnicmp(tmp, "SovietMiner", 11) == 0;
  if (naval_miner && has_bad_typeid_token(tmp)) return false;
  if (!lookup_zh_name(tmp) && !naval_miner) return false;
  std::snprintf(out, out_len, "%s", tmp);
  return true;
}

static int faction_of_key(const char* key);
static int detect_faction_from_player(uint32_t player);
static int cached_faction_of(uint32_t player);

// Score a TypeId candidate. BFI-style STRUCTURE/unit filter is approximated
// by name tokens; here we also prefer the owner's faction and finished
// buildings over Japan *Egg pack-cores that sit in nested template data.
static int score_catalog_candidate(const char* id, int owner_fac, uint32_t depth) {
  if (!id || !id[0] || should_hide_roster_type(id)) return -100000;
  int s = 2000 - (int)depth;
  if (s < 0) s = 0;
  const int fac = faction_of_key(id);
  if (owner_fac > 0 && fac > 0) {
    if (fac == owner_fac) {
      s += 800;
    } else if (type_id_is_building(id) || id_has_token(id, "Egg")) {
      // Captured buildings / pack-cores may keep another side's TypeId.
      s -= 600;
    } else {
      // Nested weapon/spawn strings (e.g. AlliedEngineer inside an MCV blob)
      // must not label a living Soviet/Japan unit.
      return -100000;
    }
  }
  // Pack-cores are transitional; prefer the finished building id when both
  // appear in the same template blob.
  if (id_has_token(id, "Egg")) s -= 350;
  if (id_has_token(id, "AntiStructure")) s -= 50;
  // Prefer the unit's own TypeId over a nested helper that shares a catalog name.
  if (id_has_token(id, "Engineer") && depth > 8) s -= 200;
  return s;
}

static void consider_catalog_cand(const char* id, int owner_fac, uint32_t depth, char* best,
                                  size_t best_len, int* best_score) {
  if (!id || !id[0] || !best || !best_score) return;
  const int sc = score_catalog_candidate(id, owner_fac, depth);
  if (sc <= *best_score) return;
  *best_score = sc;
  std::snprintf(best, best_len, "%s", id);
}

// The template's own GameObject id is the nearest catalog string.
// Nested weapon / spawn names sit further out and were being picked before,
// which labeled a unit as another faction (e.g. Allied seat showing 防卫者核心).
static bool tpl_cached_reject(uint32_t unit_data) {
  if (!is_ptr(unit_data) || g_tpl_cs_state != 2) return false;
  EnterCriticalSection(&g_tpl_cs);
  auto it = g_tpl_type_cache.find(unit_data);
  const bool reject = it != g_tpl_type_cache.end() && (it->second.empty() || it->second == "-");
  LeaveCriticalSection(&g_tpl_cs);
  return reject;
}

static bool resolve_primary_catalog_id(uint32_t unit_data, char* out, size_t out_len,
                                       int owner_fac = 0) {
  if (!is_ptr(unit_data) || !out || out_len < 2) return false;
  // Cache key ignores owner_fac; cached ids are already faction-scored once.
  ensure_tpl_cs();
  EnterCriticalSection(&g_tpl_cs);
  auto it = g_tpl_type_cache.find(unit_data);
  if (it != g_tpl_type_cache.end()) {
    if (it->second.empty() || it->second == "-") {
      LeaveCriticalSection(&g_tpl_cs);
      return false;
    }
    std::snprintf(out, out_len, "%s", it->second.c_str());
    LeaveCriticalSection(&g_tpl_cs);
    // Cache ignores owner_fac. Re-resolve when a non-capturable unit was labeled
    // with another faction (AlliedEngineer ghosts on every seat), or when a
    // cross-faction Egg was cached before the owner was known.
    const int cached_fac = faction_of_key(out);
    const bool cross = owner_fac > 0 && cached_fac > 0 && cached_fac != owner_fac;
    const bool re_resolve =
        cross && (!type_id_is_building(out) || id_has_token(out, "Egg"));
    if (re_resolve) {
      EnterCriticalSection(&g_tpl_cs);
      g_tpl_type_cache.erase(unit_data);
      LeaveCriticalSection(&g_tpl_cs);
    } else {
      return lookup_zh_name(out) != nullptr;
    }
  } else {
    LeaveCriticalSection(&g_tpl_cs);
  }

  char best[96] = {};
  int best_score = -100000;
  char cand[96] = {};
  for (uint32_t off = 0; off <= 0x180; off += 4) {
    if (catalog_id_at(unit_data + off, cand, sizeof(cand))) {
      consider_catalog_cand(cand, owner_fac, off, best, sizeof(best), &best_score);
    }
    uint32_t p = 0;
    if (!safe_read_u32(unit_data + off, &p) || !is_ptr(p)) continue;
    if (catalog_id_at(p, cand, sizeof(cand))) {
      consider_catalog_cand(cand, owner_fac, off + 1, best, sizeof(best), &best_score);
    }
    uint32_t p2 = 0;
    if (safe_read_u32(p, &p2) && is_ptr(p2) && catalog_id_at(p2, cand, sizeof(cand))) {
      consider_catalog_cand(cand, owner_fac, off + 2, best, sizeof(best), &best_score);
    }
  }
  ensure_tpl_cs();
  EnterCriticalSection(&g_tpl_cs);
  if (!best[0] || best_score < 0) {
    if (best[0]) g_tpl_type_cache[unit_data] = "-";
    LeaveCriticalSection(&g_tpl_cs);
    return false;
  }
  g_tpl_type_cache[unit_data] = best;
  LeaveCriticalSection(&g_tpl_cs);
  std::snprintf(out, out_len, "%s", best);
  return true;
}

static int seat_for_owner(const MatchEconomy* out, uint32_t owner) {
  if (!out || !is_ptr(owner)) return -1;
  for (int p = 0; p < out->player_count; ++p) {
    if (out->players[p].player == owner) return p;
  }
  return -1;
}

static bool entity_catalog_for_roster(uint32_t ent, const MatchEconomy* out, int* seat,
                                      char* type_id, size_t type_len) {
  if (seat) *seat = -1;
  if (!looks_like_entity(ent)) return false;
  uint32_t ow = 0;
  if (!safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow)) return false;
  const int s = seat_for_owner(out, ow);
  if (s < 0) return false;
  uint32_t unit_data = 0;
  if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) return false;
  // Never trigger a faction walk here: this runs per entity while tallying.
  const int owner_fac = cached_faction_of(ow);
  if (!resolve_primary_catalog_id(unit_data, type_id, type_len, owner_fac)) return false;
  if (seat) *seat = s;
  return true;
}

// Cheap list scoring for rediscover — ownership only, no TypeId catalog walks.
static int score_owned_vector(uint32_t begin, uint32_t end, const MatchEconomy* out) {
  uint32_t n = 0;
  if (!out || !valid_ent_vector_range(begin, end, &n)) return 0;
  int owned = 0;
  const uint32_t sample = n < 24 ? n : 24;
  for (uint32_t i = 0; i < sample; ++i) {
    const uint32_t idx = n <= sample ? i : (i * n / sample);
    uint32_t ent = 0;
    if (!safe_read_u32(begin + idx * 4u, &ent) || !is_ptr(ent)) continue;
    uint32_t ow = 0;
    if (!safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow) || !is_ptr(ow)) continue;
    if (seat_for_owner(out, ow) >= 0) ++owned;
  }
  if (sample == 0 || owned == 0) return 0;
  return owned * (int)((n + sample - 1) / sample);
}

static int tally_catalog_vector(uint32_t begin, uint32_t end, MatchEconomy* out) {
  uint32_t n = 0;
  if (!out || !valid_ent_vector_range(begin, end, &n)) return 0;
  int catalog = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t ent = 0;
    if (!safe_read_u32(begin + i * 4u, &ent)) continue;
    char type_id[96] = {};
    int seat = -1;
    if (!entity_catalog_for_roster(ent, out, &seat, type_id, sizeof(type_id))) continue;
    const char* zh = lookup_zh_name(type_id);
    if (!zh || !zh[0] || seat < 0) continue;
    add_roster_count(&out->players[seat], type_id, zh, type_id_is_building(type_id));
    ++catalog;
  }
  return catalog;
}

static void tally_entity_into_player(PlayerEconomy* pe, uint32_t ent) {
  if (!pe || !is_ptr(ent)) return;
  char type_id[96] = {};
  int seat = -1;
  MatchEconomy fake{};
  fake.player_count = 1;
  fake.players[0].player = pe->player;
  if (!entity_catalog_for_roster(ent, &fake, &seat, type_id, sizeof(type_id))) return;
  const char* zh = lookup_zh_name(type_id);
  if (!zh || !zh[0]) return;
  add_roster_count(pe, type_id, zh, type_id_is_building(type_id));
}

// In-place money/power only — used by the stats panel at ~10Hz.
static bool refresh_match_resources_impl(MatchEconomy* inout) {
  if (!inout || !inout->valid || inout->player_count <= 0) return false;
  if (!loaded_module()) return false;
  for (int i = 0; i < inout->player_count; ++i) {
    PlayerEconomy& pe = inout->players[i];
    if (!is_ptr(pe.player)) continue;

    uint32_t money_table = 0, money_obj = 0, money = 0;
    if (safe_read_u32(pe.player + kPlayerMoneyTableOff, &money_table) && is_ptr(money_table) &&
        safe_read_u32(money_table, &money_obj) && is_ptr(money_obj) &&
        safe_read_u32(money_obj + 0x04, &money) && money <= 200000000u) {
      pe.has_money = true;
      pe.money = money;
    }

    uint32_t power_obj = 0;
    if (safe_read_u32(pe.player + kPlayerPowerObjOff, &power_obj) && is_ptr(power_obj)) {
      uint32_t self_ref = 0;
      const bool ok_self =
          safe_read_u32(power_obj + 0x20, &self_ref) && self_ref == (power_obj + 0x20);
      uint32_t total = 0, used = 0;
      if (ok_self && safe_read_u32(power_obj + 0x04, &total) &&
          safe_read_u32(power_obj + 0x08, &used) && total < 500000u && used < 500000u) {
        pe.has_power = true;
        pe.power_total = total;
        pe.power_used = used;
      }
    }
  }
  return true;
}

static void fill_player_economy_basic(PlayerEconomy* pe, uint32_t player,
                                      uint32_t local) {
  if (!pe || !is_ptr(player)) return;
  *pe = PlayerEconomy{};
  pe->player = player;
  pe->is_local = (local && player == local);
  safe_read_u32(player + 0x20, &pe->player_id);

  uint32_t argb = 0;
  auto take_color = [&](uint32_t c) {
    const int r = (int)((c >> 16) & 255u);
    const int g = (int)((c >> 8) & 255u);
    const int b = (int)(c & 255u);
    if ((r | g | b) == 0) return false;
    pe->has_color = true;
    pe->color_r = (uint8_t)r;
    pe->color_g = (uint8_t)g;
    pe->color_b = (uint8_t)b;
    return true;
  };
  if (!(safe_read_u32(player + 0x80, &argb) && take_color(argb))) {
    if (!(safe_read_u32(player + 0x84, &argb) && take_color(argb))) {
      // Uprising inserts one extra dword before the member block; fall back to
      // the next slot so a shifted colour still renders. Retail is untouched.
      if (uprising_active() && safe_read_u32(player + 0x88, &argb)) take_color(argb);
    }
  }

  uint32_t money_table = 0, money_obj = 0, money = 0;
  if (safe_read_u32(player + kPlayerMoneyTableOff, &money_table) && is_ptr(money_table) &&
      safe_read_u32(money_table, &money_obj) && is_ptr(money_obj) &&
      safe_read_u32(money_obj + 0x04, &money) && money <= 200000000u) {
    pe->has_money = true;
    pe->money = money;
  }

  uint32_t power_obj = 0;
  if (safe_read_u32(player + kPlayerPowerObjOff, &power_obj) && is_ptr(power_obj)) {
    uint32_t self_ref = 0;
    const bool ok_self =
        safe_read_u32(power_obj + 0x20, &self_ref) && self_ref == (power_obj + 0x20);
    uint32_t total = 0, used = 0;
    if (ok_self && safe_read_u32(power_obj + 0x04, &total) &&
        safe_read_u32(power_obj + 0x08, &used) && total < 500000u && used < 500000u) {
      pe->has_power = true;
      pe->power_total = total;
      pe->power_used = used;
    }
  }

  // Same object the end-of-match score screen reads. The game adds to it
  // when something is built, lost, or killed; it is not a battlefield scan.
  uint32_t sk = 0;
  if (safe_read_u32(player + 0x1F0, &sk) && is_ptr(sk)) {
    auto sane = [](uint32_t addr, uint32_t* out) -> bool {
      uint32_t v = 0;
      if (!safe_read_u32(addr, &v) || v > 50000000u) return false;
      *out = v;
      return true;
    };
    auto sum_range = [&](uint32_t begin, uint32_t end) -> uint32_t {
      uint32_t sum = 0;
      for (uint32_t off = begin; off <= end; off += 4) {
        uint32_t v = 0;
        if (!sane(sk + off, &v)) return 0;
        sum += v;
      }
      return sum > 50000000u ? 0u : sum;
    };
    uint32_t earned = 0, spent = 0, ub = 0, ul = 0, bb = 0, bl = 0;
    if (sane(sk + 0x04, &earned) && sane(sk + 0x08, &spent) &&
        sane(sk + 0x6C, &ub) && sane(sk + 0x70, &ul) &&
        sane(sk + 0xC4, &bb) && sane(sk + 0xC8, &bl)) {
      pe->has_score = true;
      pe->money_earned = earned;
      pe->money_spent = spent;
      pe->units_built = ub;
      pe->units_lost = ul;
      pe->buildings_built = bb;
      pe->buildings_lost = bl;
      pe->units_destroyed = sum_range(0x1C, 0x68);
      pe->buildings_destroyed = sum_range(0x74, 0xC0);
    }
  }
}

// kOk: recorded. kRetry: object exists but owner is not written yet. kDrop: not a roster unit.
enum class LiveNote { kOk, kRetry, kDrop };

// Cap expensive catalog resolves per Present / stats poll. Mass production
// otherwise stalls the render thread classifying hundreds of new templates.
static int g_catalog_budget = 0;

static bool key_is_nano_egg(const char* key);
static int detect_faction_from_player(uint32_t player);
static int detect_faction(const PlayerEconomy& pe);

static LiveNote note_live_ent(uint32_t ent, uint32_t owner_hint) {
  if (!is_ptr(ent)) return LiveNote::kDrop;
  if (!entity_alive(ent)) return LiveNote::kDrop;

  uint32_t unit_data = 0;
  if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) return LiveNote::kRetry;
  uint32_t ow = 0;
  safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow);
  // Player object-list membership is authoritative when +0x418 is still 0
  // (common right after Spawn). Prefer a live owner field when present.
  if (!is_ptr(ow)) ow = owner_hint;

  // Fast path: already tracked with same template — skip catalog walks.
  // Still refresh owner, and force re-resolve for pack-cores / template morphs.
  // Cross-faction TypeId ghosts are dropped in publish_live_roster (seat vs key).
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  auto known = g_live.find(ent);
  if (known != g_live.end() && known->second.key[0] &&
      known->second.unit_data == unit_data && !key_is_nano_egg(known->second.key)) {
    if (is_ptr(ow)) known->second.owner = ow;
    else if (is_ptr(owner_hint)) known->second.owner = owner_hint;
    known->second.seen_gen = g_live_scan_gen;
    known->second.dead_streak = 0;
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    return LiveNote::kOk;
  }
  if (g_live_cs_ready) {
    if (g_live_drop.find(ent) != g_live_drop.end()) {
      LeaveCriticalSection(&g_live_cs);
      return LiveNote::kDrop;
    }
    LeaveCriticalSection(&g_live_cs);
  }

  if (g_catalog_budget <= 0) return LiveNote::kRetry;
  --g_catalog_budget;

  const int owner_fac = is_ptr(ow) ? cached_faction_of(ow) : 0;
  char type_id[96] = {};
  if (!resolve_primary_catalog_id(unit_data, type_id, sizeof(type_id), owner_fac)) {
    if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
    g_live_drop.insert(ent);
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    return LiveNote::kDrop;
  }
  const char* zh = lookup_zh_name(type_id);
  if (!zh || !zh[0]) {
    if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
    g_live_drop.insert(ent);
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    return LiveNote::kDrop;
  }
  if (!is_ptr(ow)) return LiveNote::kRetry;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  auto it = g_live.find(ent);
  if (it == g_live.end()) {
    LiveEnt rec{};
    rec.owner = ow;
    rec.unit_data = unit_data;
    rec.seen_gen = g_live_scan_gen;
    rec.dead_streak = 0;
    std::snprintf(rec.key, sizeof(rec.key), "%s", type_id);
    std::snprintf(rec.name, sizeof(rec.name), "%s", zh);
    rec.building = type_id_is_building(type_id);
    g_live.emplace(ent, rec);
  } else {
    it->second.owner = ow;
    it->second.unit_data = unit_data;
    std::snprintf(it->second.key, sizeof(it->second.key), "%s", type_id);
    std::snprintf(it->second.name, sizeof(it->second.name), "%s", zh);
    it->second.building = type_id_is_building(type_id);
    it->second.seen_gen = g_live_scan_gen;
    it->second.dead_streak = 0;
  }
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  return LiveNote::kOk;
}

// Light object-list walk: bump known ents; enqueue unknowns for Present to
// classify. Never call resolve_primary_catalog_id here — that is what made
// open-panel seed hitch Present for ~170ms.
// list_owner: Player* when walking that player's vectors (hint for new notes).
static void touch_or_note_live(uint32_t ent, uint32_t list_owner = 0) {
  if (!is_ptr(ent) || !entity_alive(ent)) return;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  if (g_live_drop.find(ent) != g_live_drop.end()) {
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    return;
  }
  auto it = g_live.find(ent);
  if (it != g_live.end() && it->second.key[0]) {
    it->second.seen_gen = g_live_scan_gen;
    it->second.dead_streak = 0;
    // Only adopt list ownership when the entity owner field agrees (or is
    // still unset). Shared/global lists must not reassign every seat's army.
    if (is_ptr(list_owner)) {
      uint32_t ow = 0;
      const bool got =
          safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow) && is_ptr(ow);
      if (!got || ow == list_owner) it->second.owner = list_owner;
      else it->second.owner = ow;
    }
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    return;
  }
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  queue_live_add(ent, list_owner);
}

// GameLogic global lists are noisy (projectiles, helpers). Only refresh
// already-tracked ents; new classification comes from player lists / hooks.
static void touch_live_only(uint32_t ent) {
  if (!is_ptr(ent) || !entity_alive(ent)) return;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  auto it = g_live.find(ent);
  if (it != g_live.end() && it->second.key[0]) {
    it->second.seen_gen = g_live_scan_gen;
    it->second.dead_streak = 0;
  }
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
}

static void forget_live_ent(uint32_t ent) {
  if (!is_ptr(ent)) return;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  g_live.erase(ent);
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
}

static bool key_is_nano_egg(const char* key) {
  if (!key || !key[0]) return false;
  for (const char* p = key; p[2]; ++p) {
    const unsigned char a = (unsigned char)p[0];
    const unsigned char b = (unsigned char)p[1];
    const unsigned char c = (unsigned char)p[2];
    if ((a == 'E' || a == 'e') && (b == 'G' || b == 'g') && (c == 'G' || c == 'g')) return true;
  }
  return false;
}

// An unpacked nanocore keeps the egg id until its template is replaced. Re-read
// it here, and drop the core once it is dead so the finished weapon is the row.
static void refresh_core_identity(uint32_t ent, LiveEnt* rec) {
  if (!rec || !key_is_nano_egg(rec->key)) return;
  uint32_t unit_data = 0;
  if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) return;
  // Template pointer changed → clear stale TypeId cache for the old blob.
  if (rec->unit_data && rec->unit_data != unit_data && g_tpl_cs_state == 2) {
    EnterCriticalSection(&g_tpl_cs);
    g_tpl_type_cache.erase(rec->unit_data);
    LeaveCriticalSection(&g_tpl_cs);
  }
  rec->unit_data = unit_data;
  const int owner_fac = is_ptr(rec->owner) ? cached_faction_of(rec->owner) : 0;
  char type_id[96] = {};
  if (!resolve_primary_catalog_id(unit_data, type_id, sizeof(type_id), owner_fac)) return;
  if (std::strcmp(rec->key, type_id) == 0) return;
  const char* zh = lookup_zh_name(type_id);
  if (!zh || !zh[0]) return;
  std::snprintf(rec->key, sizeof(rec->key), "%s", type_id);
  std::snprintf(rec->name, sizeof(rec->name), "%s", zh);
  rec->building = type_id_is_building(type_id);
}

// Drain a slice of Create/Destroy queues. Returns true if work remains.
static bool drain_live_queues(int max_add, int max_forget) {
  if (max_add < 0) max_add = 0;
  if (max_forget < 0) max_forget = 0;
  uint32_t adds[64];
  uint32_t hints[64];
  uint32_t forgets[64];
  int nadd = 0, nforget = 0;
  if (max_add > 64) max_add = 64;
  if (max_forget > 64) max_forget = 64;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  nadd = g_add_n < max_add ? g_add_n : max_add;
  for (int i = 0; i < nadd; ++i) {
    adds[i] = g_add_q[i];
    hints[i] = g_add_hint[i];
  }
  if (nadd < g_add_n) {
    const int rest = g_add_n - nadd;
    std::memmove(g_add_q, g_add_q + nadd, (size_t)rest * sizeof(g_add_q[0]));
    std::memmove(g_add_hint, g_add_hint + nadd, (size_t)rest * sizeof(g_add_hint[0]));
    g_add_n = rest;
  } else {
    g_add_n = 0;
  }
  nforget = g_forget_n < max_forget ? g_forget_n : max_forget;
  for (int i = 0; i < nforget; ++i) forgets[i] = g_forget_q[i];
  if (nforget < g_forget_n) {
    const int rest = g_forget_n - nforget;
    std::memmove(g_forget_q, g_forget_q + nforget, (size_t)rest * sizeof(g_forget_q[0]));
    g_forget_n = rest;
  } else {
    g_forget_n = 0;
  }
  const bool more = (g_add_n > 0 || g_forget_n > 0);
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  for (int i = 0; i < nforget; ++i) forget_live_ent(forgets[i]);
  for (int i = 0; i < nadd; ++i) {
    const LiveNote note = note_live_ent(adds[i], hints[i]);
    if (note == LiveNote::kOk) {
      g_live_retries.erase(adds[i]);
      continue;
    }
    if (note == LiveNote::kDrop) {
      g_live_retries.erase(adds[i]);
      continue;
    }
    // kRetry: a few chances (owner/template still writing), then drop so a
    // 7000-entry junk vector cannot spin forever and keep the seed pending.
    uint8_t& tries = g_live_retries[adds[i]];
    if (tries < 4) {
      ++tries;
      queue_live_add(adds[i], hints[i]);
    } else {
      if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
      g_live_drop.insert(adds[i]);
      if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
      g_live_retries.erase(adds[i]);
    }
  }
  return more;
}

static void publish_live_roster(MatchEconomy* out) {
  if (!out) return;
  // Classification is amortized on Present (roster_tick). The economy poll
  // only forgets / does a tiny drain so it never hitchs the render thread.
  if (roster_early_load()) {
    if (g_catalog_budget < 2) g_catalog_budget = 2;
    drain_live_queues(1, 16);
  } else {
    if (g_catalog_budget < 4) g_catalog_budget = 4;
    drain_live_queues(2, 24);
  }

  // Resolve seat faction from the cache only — the graph walk itself is
  // budgeted to a couple of seats per poll inside detect_faction_from_player(),
  // and apply_seat_labels() already runs it.
  int seat_fac[kMaxEconPlayers] = {};
  for (int s = 0; s < out->player_count && s < kMaxEconPlayers; ++s) {
    seat_fac[s] = cached_faction_of(out->players[s].player);
  }

  static std::vector<std::pair<uint32_t, LiveEnt>> copy;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  copy.clear();
  copy.reserve(g_live.size());
  for (auto it = g_live.begin(); it != g_live.end();) {
    if (!looks_like_entity(it->first)) {
      it = g_live.erase(it);
      continue;
    }
    // Eggs only (function no-ops for others); avoid catalog work under the lock.
    if (key_is_nano_egg(it->second.key)) refresh_core_identity(it->first, &it->second);
    // Pack-cores die / unpack quickly. Drop dead eggs immediately (don't wait
    // for DestroyUnit), and drop eggs that vanished from every object list —
    // otherwise Allied seats keep a stuck「防卫者核心」row forever.
    if (key_is_nano_egg(it->second.key)) {
      if (!entity_alive(it->first)) {
        it = g_live.erase(it);
        continue;
      }
      if (g_live_scan_gen > 0 && it->second.seen_gen > 0 &&
          (g_live_scan_gen - it->second.seen_gen) >= 2) {
        it = g_live.erase(it);
        continue;
      }
    }
    // Drop ghosts that left every scanned list (mis-noted helpers / stale
    // CreateUnit leftovers). Need a few missed gens so a unit mid-transport
    // isn't wiped on a single incomplete light scan.
    if (!key_is_nano_egg(it->second.key) && g_live_scan_gen > 0 &&
        it->second.seen_gen > 0 && (g_live_scan_gen - it->second.seen_gen) >= 3) {
      it = g_live.erase(it);
      continue;
    }
    // Field roster = living objects only (BFI filters DESTROYED / dead).
    if (!entity_alive(it->first)) {
      if (++it->second.dead_streak >= 2) {
        it = g_live.erase(it);
        continue;
      }
      ++it;
      continue;
    }
    it->second.dead_streak = 0;
    uint32_t ow = 0;
    const bool got =
        safe_read_u32(it->first + ent_off(kEntOwner, kEntOwnerEp1), &ow) && is_ptr(ow);
    if (got && ow != it->second.owner && seat_for_owner(out, ow) >= 0) {
      if (seat_for_owner(out, it->second.owner) < 0) {
        it->second.owner = ow;
        it->second.pending_owner = 0;
        it->second.pending_hits = 0;
      } else if (it->second.pending_owner == ow) {
        if (++it->second.pending_hits >= 2) {
          it->second.owner = ow;
          it->second.pending_owner = 0;
          it->second.pending_hits = 0;
        }
      } else {
        it->second.pending_owner = ow;
        it->second.pending_hits = 1;
      }
    } else {
      it->second.pending_owner = 0;
      it->second.pending_hits = 0;
    }
    // Drop cross-faction units / pack-cores that cannot belong to this seat.
    // Captured buildings keep their original TypeId and are allowed through.
    {
      const int type_fac = faction_of_key(it->second.key);
      const int seat = seat_for_owner(out, it->second.owner);
      if (type_fac > 0 && seat >= 0 && seat < kMaxEconPlayers) {
        const int sf = seat_fac[seat];
        if (sf > 0 && sf != type_fac) {
          if (key_is_nano_egg(it->second.key) || !it->second.building) {
            if (it->second.unit_data && g_tpl_cs_state == 2) {
              EnterCriticalSection(&g_tpl_cs);
              g_tpl_type_cache.erase(it->second.unit_data);
              LeaveCriticalSection(&g_tpl_cs);
            }
            it = g_live.erase(it);
            continue;
          }
        }
      }
    }
    copy.emplace_back(it->first, it->second);
    ++it;
  }
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  for (const auto& item : copy) {
    const int seat = seat_for_owner(out, item.second.owner);
    if (seat < 0 || !item.second.key[0]) continue;
    add_roster_count(&out->players[seat], item.second.key, item.second.name,
                     item.second.building);
  }
  for (int i = 0; i < out->player_count; ++i) build_roster_text(&out->players[i]);
}

using CreateUnitFn = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using SpawnObjectFn = uint32_t(__cdecl*)(void* req);
static CreateUnitFn g_orig_create = nullptr;
static SpawnObjectFn g_orig_spawn = nullptr;
static void* g_orig_destroy = nullptr;

static uint32_t __cdecl hk_create_unit(uint32_t a0, uint32_t tmpl, uint32_t pos, uint32_t owner,
                                      uint32_t info) {
  uint32_t ent = 0;
  if (g_orig_create) ent = g_orig_create(a0, tmpl, pos, owner, info);
  __try {
    if (is_ptr(ent)) queue_live_add(ent, owner);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return ent;
}

// 0x51A210 allocates every object (production, buildings, projectiles).
// Owner is often written by the caller after this returns, so remember the
// pointer and classify it on the next stats refresh.
static uint32_t __cdecl hk_spawn_object(void* req) {
  uint32_t ent = g_orig_spawn ? g_orig_spawn(req) : 0;
  __try {
    // Match load allocates map props / FX / helpers in a burst. Queuing them
    // all for TypeId classification freezes Present — CreateUnit still tracks
    // real units, and the light player-list scan catches the rest after settle.
    if (is_ptr(ent) && !roster_early_load()) {
      uint32_t unit_data = 0;
      safe_read_u32(ent + 4, &unit_data);
      if (!tpl_cached_reject(unit_data)) queue_live_add(ent, 0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return ent;
}

static void __cdecl forget_live_from_hook(uint32_t ent) {
  __try {
    if (ent) air_hold_forget(ent);
    // 无敌名单也要立刻知道：对象一被销毁，锁血就绝不能再往这个地址写。
    if (ent) invincible_note_destroyed(ent);
    queue_live_forget(ent);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
static void __declspec(naked) hk_destroy_unit() {
  __asm {
    push ecx
    push ecx
    call forget_live_from_hook
    add esp, 4
    pop ecx
    jmp dword ptr [g_orig_destroy]
  }
}

static void install_roster_hooks_impl() {
  if (!g_live_cs_ready) {
    InitializeCriticalSection(&g_live_cs);
    g_live_cs_ready = true;
  }
  if (!module_base()) return;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
    log("roster MH_Initialize failed: %d", (int)st);
    return;
  }
  void* create = reinterpret_cast<void*>(module_base() + (kFnCreateUnit - kModBase));
  void* spawn = reinterpret_cast<void*>(module_base() + (0x0051A210 - kModBase));
  void* destroy = reinterpret_cast<void*>(module_base() + (kFnDestroy - kModBase));
  if (!g_orig_create) {
    st = MH_CreateHook(create, reinterpret_cast<void*>(&hk_create_unit),
                       reinterpret_cast<void**>(&g_orig_create));
    if (st != MH_OK) log("hook CreateUnit failed: %d", (int)st);
  }
  if (!g_orig_spawn) {
    st = MH_CreateHook(spawn, reinterpret_cast<void*>(&hk_spawn_object),
                       reinterpret_cast<void**>(&g_orig_spawn));
    if (st != MH_OK) log("hook SpawnObject failed: %d", (int)st);
  }
  if (!g_orig_destroy) {
    st = MH_CreateHook(destroy, reinterpret_cast<void*>(&hk_destroy_unit), &g_orig_destroy);
    if (st != MH_OK) log("hook Destroy failed: %d", (int)st);
  }
  if (g_orig_create) MH_EnableHook(create);
  if (g_orig_spawn) MH_EnableHook(spawn);
  if (g_orig_destroy) MH_EnableHook(destroy);
  log("roster hooks armed create=%p spawn=%p destroy=%p", create, spawn, destroy);
}

// ---------------------------------------------------------------------------
// Uprising superweapon / protocol owner diagnostics.
//
// The engine resolves the cooldown container as [module + 0x428] and then walks
// a circular list at owner + 0x218.  These two hooks record every distinct
// owner that reaches the charge-ratio getter / ready predicate, together with
// the node count of its +0x218 list and whether the pointer matches a seat in
// ThePlayerList, so we can tell whether the object we read from the player list
// really is the owner the engine uses.
// ---------------------------------------------------------------------------
static uint32_t g_sp_probe_owners[24];
static int g_sp_probe_count = 0;

static int sp_list_nodes(uint32_t owner) {
  const uint32_t sentinel = owner + 0x218;
  uint32_t node = 0;
  if (!safe_read_u32(sentinel, &node)) return -1;
  int n = 0;
  for (int i = 0; i < 32 && is_ptr(node) && node != sentinel; ++i) {
    ++n;
    uint32_t next = 0;
    if (!safe_read_u32(node, &next) || next == node) break;
    node = next;
  }
  return n;
}

static void sp_probe_report(const char* tag, uint32_t self, uint32_t arg) {
  if (!self) return;
  for (int i = 0; i < g_sp_probe_count; ++i) {
    if (g_sp_probe_owners[i] == self) return;
  }
  if (g_sp_probe_count >= 24) return;
  const int idx = g_sp_probe_count;
  g_sp_probe_owners[g_sp_probe_count++] = self;

  uint32_t vt = 0, head = 0, local = 0, list = 0;
  safe_read_u32(self, &vt);
  safe_read_u32(self + 0x218, &head);
  int seat = -1;
  if (safe_read_u32(loaded_module() + player_list_rva(), &list) && is_ptr(list)) {
    safe_read_u32(list + kPlayerListLocalOff, &local);
    for (int i = 0; i < (int)kPlayerListSlots; ++i) {
      uint32_t p = 0;
      if (safe_read_u32(list + kPlayerListArrayOff + (uint32_t)i * 4u, &p) && p == self) {
        seat = i;
        break;
      }
    }
  }
  const int nodes = sp_list_nodes(self);
  uint32_t n0 = 0, k0 = 0, r0 = 0, d0 = 0;
  if (nodes > 0) {
    safe_read_u32(head + 0, &n0);
    safe_read_u32(head + 8, &k0);
    safe_read_u32(head + 0x0C, &r0);
    safe_read_u32(head + 0x10, &d0);
  }
  log("spowner[%s] #%d self=%08X vt=%08X seat=%d local=%08X isLocal=%d arg=%08X head=%08X "
      "nodes=%d n0=[next=%08X k=%08X r=%08X d=%08X]",
      tag, idx, self, vt, seat, local, self == local ? 1 : 0, arg, head, nodes, n0, k0, r0, d0);
}

typedef float(__fastcall* fn_sp_charge)(void* self, void* edx, void* arg);
static fn_sp_charge g_orig_sp_charge = nullptr;
static float __fastcall hk_sp_charge(void* self, void* edx, void* arg) {
  __try {
    sp_probe_report("charge", (uint32_t)(uintptr_t)self, (uint32_t)(uintptr_t)arg);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (g_orig_sp_charge) return g_orig_sp_charge(self, edx, arg);
  return 0.f;
}

typedef unsigned char(__fastcall* fn_sp_ready)(void* self, void* edx, void* arg);
static fn_sp_ready g_orig_sp_ready = nullptr;
static unsigned char __fastcall hk_sp_ready(void* self, void* edx, void* arg) {
  __try {
    sp_probe_report("ready", (uint32_t)(uintptr_t)self, (uint32_t)(uintptr_t)arg);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (g_orig_sp_ready) return g_orig_sp_ready(self, edx, arg);
  return 0;
}

static void install_sp_probe() {
  static bool tried = false;
  if (tried || !uprising_active() || !loaded_module()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
    log("sp probe MH_Initialize failed: %d", (int)st);
    return;
  }
  void* charge = reinterpret_cast<void*>(loaded_module() + (0x88C3B0u - kModBase));
  void* ready = reinterpret_cast<void*>(loaded_module() + (0x88C420u - kModBase));
  st = MH_CreateHook(charge, reinterpret_cast<void*>(&hk_sp_charge),
                     reinterpret_cast<void**>(&g_orig_sp_charge));
  if (st == MH_OK) {
    MH_EnableHook(charge);
    log("sp probe charge hook armed at %p", charge);
  } else {
    log("sp probe charge hook failed: %d", (int)st);
    g_orig_sp_charge = nullptr;
  }
  st = MH_CreateHook(ready, reinterpret_cast<void*>(&hk_sp_ready),
                     reinterpret_cast<void**>(&g_orig_sp_ready));
  if (st == MH_OK) {
    MH_EnableHook(ready);
    log("sp probe ready hook armed at %p", ready);
  } else {
    log("sp probe ready hook failed: %d", (int)st);
    g_orig_sp_ready = nullptr;
  }
}

// Gather entities from every player's lists (and global multi-owner lists),
// then bucket into each PlayerEconomy by owner / player_id.
//
// BFI-style approach: keep walking TheGameLogic / player object vectors on a
// short interval instead of trusting Create/Destroy hooks alone. Hooks still
// make adds/removes feel instant; the scan corrects misses and save loads.
// Sub-phase timings of the last fill_all_rosters() call: rediscover branch,
// object-list light scan, and the publish/tally pass. Logged by the economy
// poll so a remaining stall can be attributed without guessing.
static double g_roster_ph_rediscover = 0, g_roster_ph_light = 0, g_roster_ph_publish = 0;
static bool g_roster_ph_logged = false;

static void fill_all_rosters(MatchEconomy* out) {
  if (!out || out->player_count <= 0) return;
  g_roster_ph_logged = false;

  // Sub-phase timings for the perf log (read after the call).
  LARGE_INTEGER rf{}, ft0{}, ftp{};
  QueryPerformanceFrequency(&rf);
  QueryPerformanceCounter(&ft0);
  ftp = ft0;
  auto mark = [&](double* slot) {
    QueryPerformanceCounter(&ftp);
    if (rf.QuadPart > 0) {
      *slot = (double)(ftp.QuadPart - ft0.QuadPart) * 1000.0 / (double)rf.QuadPart;
      ft0 = ftp;
    }
  };
  double ph_rediscover = 0, ph_light = 0, ph_publish = 0;

  uint32_t local_id = 0;
  for (int i = 0; i < out->player_count; ++i) {
    if (out->players[i].is_local) {
      local_id = out->players[i].player_id;
      break;
    }
  }
  if (g_roster_local_id && local_id && local_id != g_roster_local_id) {
    clear_roster_caches();
  }
  if (local_id) g_roster_local_id = local_id;

  const DWORD now = GetTickCount();
  const bool rescan = (int)(g_roster_rescan_until - now) > 0;
  // Open → one seed walk. After that, while the panel stays open, reconcile
  // with another cheap full player-list walk every ~2s (same path, no clear of
  // g_live) so counts stay accurate without hide/show. Money/power stay on the
  // separate 100ms refresh.
  const bool need_seed_walk = g_roster_seed_burst && !g_seed_walk_done;
  const bool scan_wait =
      !need_seed_walk && rescan && (int)(now - g_rescan_began) < 4000;
  const bool light_due =
      need_seed_walk ||
      (!g_roster_seed_burst && !scan_wait && (int)(now - g_roster_next_light) >= 0);
  const bool rediscover_due =
      (need_seed_walk && g_logic_off_n == 0) ||
      (!g_roster_seed_burst && !scan_wait &&
       ((rescan && g_logic_off_n == 0) || (int)(now - g_roster_next_rediscover) >= 0));
  if (g_catalog_budget < 4) g_catalog_budget = 4;

  constexpr uint32_t kGameLogicRva = 0x8DDE84;  // VA 0xCDDE84
  uint32_t logic = 0;
  safe_read_u32(module_base() + kGameLogicRva, &logic);

  auto read_logic_vec = [&](uint32_t off, uint32_t* begin, uint32_t* end) -> bool {
    if (!is_ptr(logic) || !begin || !end) return false;
    if (!safe_read_u32(logic + off, begin) || !safe_read_u32(logic + off + 4, end)) return false;
    return valid_ent_vector_range(*begin, *end, nullptr);
  };

  if (rediscover_due) {
    if (rescan || !g_live_seeded) {
      g_logic_off_n = 0;
      g_logic_refresh = 0;
      g_player_vec_cache_n = 0;
    }
    // Units and buildings live in different lists. Refresh the set periodically
    // instead of locking onto whichever list was largest once.
    if (is_ptr(logic) && (g_logic_off_n == 0 || ++g_logic_refresh >= 2)) {
      g_logic_refresh = 0;
      g_player_vec_cache_n = 0;
      struct Cand {
        uint32_t off = 0;
        int n = 0;
      };
      Cand cands[80] = {};
      int nc = 0;
      uint32_t seen_begin[400] = {};
      int nseen = 0;
      // Coarse step — enough to find the big object lists without melting Present.
      for (uint32_t off = 0x20; off <= 0x600; off += 8) {
        uint32_t begin = 0, end = 0;
        if (!read_logic_vec(off, &begin, &end)) continue;
        bool dup = false;
        for (int k = 0; k < nseen; ++k) {
          if (seen_begin[k] == begin) {
            dup = true;
            break;
          }
        }
        if (dup || nseen >= 200) continue;
        seen_begin[nseen++] = begin;
        const int n = score_owned_vector(begin, end, out);
        if (n < 1) continue;
        if (nc >= 40) {
          int worst = 0;
          for (int i = 1; i < nc; ++i) {
            if (cands[i].n < cands[worst].n) worst = i;
          }
          if (n <= cands[worst].n) continue;
          cands[worst] = Cand{off, n};
        } else {
          cands[nc++] = Cand{off, n};
        }
      }
      std::sort(cands, cands + nc, [](const Cand& a, const Cand& b) { return a.n > b.n; });
      g_logic_off_n = 0;
      for (int i = 0; i < nc && g_logic_off_n < kMaxLogicLists; ++i) {
        if (cands[i].n < 1) break;
        g_logic_offs[g_logic_off_n++] = cands[i].off;
      }
    }
    g_roster_next_rediscover = now + (rescan ? 4000u : 20000u);
  }
  mark(&ph_rediscover);

  if (light_due || rediscover_due) {
    ++g_live_scan_gen;
    if (g_live_scan_gen <= 0) g_live_scan_gen = 1;
    g_live_scan_tick = 0;
    // Reconcile walks must re-consider ents that were dropped during the noisy
    // open-seed (junk vectors / owner not ready). Keeping g_live_drop forever
    // made mid-match counts freeze until the user hid and reopened the panel.
    if (!need_seed_walk && g_live_seeded) {
      if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
      g_live_drop.clear();
      g_live_retries.clear();
      if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    }

    auto note_player_vec = [&](uint32_t begin, uint32_t end, uint32_t list_owner) {
      uint32_t n = 0;
      if (!valid_ent_vector_range(begin, end, &n)) return;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t ent = 0;
        if (!safe_read_u32(begin + i * 4u, &ent) || !is_ptr(ent)) continue;
        touch_or_note_live(ent, list_owner);
      }
    };
    auto touch_logic_vec = [&](uint32_t begin, uint32_t end) {
      uint32_t n = 0;
      if (!valid_ent_vector_range(begin, end, &n)) return;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t ent = 0;
        if (!safe_read_u32(begin + i * 4u, &ent) || !is_ptr(ent)) continue;
        touch_live_only(ent);
      }
    };

    for (int i = 0; i < out->player_count; ++i) {
      uint32_t offs[kMaxEntVecOffs] = {};
      const int no =
          cached_offs_for_player(out->players[i].player, out->players[i].player_id, offs,
                                 kMaxEntVecOffs);
      for (int k = 0; k < no; ++k) {
        uint32_t begin = 0, end = 0;
        if (!resolve_vector_off(out->players[i].player, offs[k], &begin, &end)) continue;
        note_player_vec(begin, end, out->players[i].player);
      }
    }
    // Global lists only keep known ents alive in seen_gen — never seed ghosts.
    for (int i = 0; i < g_logic_off_n; ++i) {
      uint32_t begin = 0, end = 0;
      if (read_logic_vec(g_logic_offs[i], &begin, &end)) touch_logic_vec(begin, end);
    }

    int live_n = 0;
    int pending = 0;
    if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
    live_n = (int)g_live.size();
    pending = g_add_n;
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    if (live_n == g_rescan_last_n) {
      ++g_rescan_stable;
    } else {
      g_rescan_stable = 0;
      g_rescan_last_n = live_n;
    }

    // Enqueue walk is cheap; Present drains the classify queue.
    if (need_seed_walk) {
      g_seed_walk_done = true;
      log("roster: full scan enqueued pending=%d live=%d overflow=%d logicLists=%d", pending,
          live_n, g_seed_queue_overflow ? 1 : 0, g_logic_off_n);
    } else if (g_live_seeded && light_due) {
      log("roster: reconcile walk pending=%d live=%d logicLists=%d", pending, live_n,
          g_logic_off_n);
    }
    // Keep reconcile aligned with the 2s economy poll while the panel is open.
    g_roster_next_light = now + 2000u;
    g_roster_next_scan = g_roster_next_light;
    log("roster: scan light=%d rediscover=%d live=%d pending=%d logicLists=%d seeded=%d burst=%d",
        light_due ? 1 : 0, rediscover_due ? 1 : 0, live_n, pending, g_logic_off_n,
        g_live_seeded ? 1 : 0, g_roster_seed_burst ? 1 : 0);
    if (rescan && g_rescan_stable >= 2 && (int)(now - g_rescan_began) >= 5000) {
      log("roster rescan settled live=%d", live_n);
      g_roster_rescan_until = now;
    }
  }
  mark(&ph_light);

  publish_live_roster(out);
  mark(&ph_publish);
  g_roster_ph_rediscover = ph_rediscover;
  g_roster_ph_light = ph_light;
  g_roster_ph_publish = ph_publish;
  g_roster_ph_logged = true;
}

static bool seat_has_record(const PlayerEconomy& pe) {
  if (pe.unit_total > 0 || pe.building_total > 0) return true;
  if (pe.units_built || pe.units_lost || pe.units_destroyed) return true;
  if (pe.buildings_built || pe.buildings_lost || pe.buildings_destroyed) return true;
  if (pe.money_earned || pe.money_spent) return true;
  return false;
}

static int find_seen_seat(uint32_t player) {
  if (!player) return -1;
  for (int i = 0; i < g_seen_n; ++i) {
    if (g_seen[i].player == player) return i;
  }
  return -1;
}

// Uprising: before the skirmish seats are initialized, ThePlayerList already
// exposes an uninitialized dummy "local" player (id 0, no money object). It is
// replaced by the real seat a fraction of a second later, so if it ever lands
// in g_seen it comes back as a phantom row for the rest of the match.
// Logged signature of the dummy: "seat p=... id=0 loc=1 money=0 hm=0 ...".
// A real local seat always has a money object, so hm=0 is the discriminator.
static bool seat_is_dummy_local(const PlayerEconomy& pe) {
  if (!uprising_active()) return false;
  if (!pe.is_local) return false;
  if (pe.player_id != 0) return false;
  return !pe.has_money;
}

static void drop_dummy_local_seats() {
  int kept = 0;
  for (int i = 0; i < g_seen_n; ++i) {
    if (seat_is_dummy_local(g_seen[i].last)) continue;
    if (kept != i) g_seen[kept] = g_seen[i];
    ++kept;
  }
  if (kept != g_seen_n) {
    log("econ: dropped %d dummy local seat(s)", g_seen_n - kept);
    g_seen_n = kept;
  }
}

// +0x106 / +0x123C are set for spectators. Defeat uses the same bytes, so a
// seat that already has a score stays visible instead of disappearing.
static bool seat_entered_battle(const PlayerEconomy& pe) {
  if (!is_ptr(pe.player)) return false;
  const bool record = seat_has_record(pe);
  uint8_t observer = 0, replay = 0;
  if (!safe_read_bytes(pe.player + 0x106, &observer, 1) ||
      !safe_read_bytes(pe.player + 0x123C, &replay, 1)) {
    return false;
  }
  if ((observer != 0 || replay != 0) && !record) return false;
  return record;
}

static void mark_defeated_flag(PlayerEconomy* pe) {
  if (!pe || !is_ptr(pe->player)) return;
  uint8_t observer = 0, replay = 0;
  if (!safe_read_bytes(pe->player + 0x106, &observer, 1) ||
      !safe_read_bytes(pe->player + 0x123C, &replay, 1)) {
    return;
  }
  if (observer != 0 || replay != 0) pe->defeated = true;
}

static int faction_of_key(const char* key) {
  if (!key || !key[0]) return 0;
  if (std::strncmp(key, "Allied", 6) == 0) return 1;
  if (std::strncmp(key, "Soviet", 6) == 0) return 2;
  if (std::strncmp(key, "Japan", 5) == 0) return 3;
  return 0;
}

static bool faction_anchor_key(const char* key) {
  if (!key) return false;
  return std::strstr(key, "MCV") || std::strstr(key, "ConYard") ||
         std::strstr(key, "ConstructionYard") || std::strstr(key, "Barracks") ||
         std::strstr(key, "WarFactory") || std::strstr(key, "Refinery") ||
         std::strstr(key, "PowerPlant");
}

static int faction_from_side_name(const char* s) {
  if (!s || !s[0]) return 0;
  if (std::strstr(s, "Allied") || std::strstr(s, "Allies")) return 1;
  if (std::strstr(s, "Soviet")) return 2;
  if (std::strstr(s, "Japan") || std::strstr(s, "Empire") || std::strstr(s, "Yuriko"))
    return 3;
  return 0;
}

// The side names live in the image as plain C strings. A player reaches one
// through a short pointer chain (Player -> Side/PlayerTemplate -> name), so
// matching the string *address* is layout independent and identical for both
// profiles: only the image base differs.
static int faction_from_side_string_va(uint32_t image_va) {
  switch (image_va) {
    case 0x00C62A2Cu:  // retail FactionAllies
    case 0x00C74B8Cu:  // uprising FactionAllies
      return 1;
    case 0x00C62A1Cu:  // retail FactionSoviet
    case 0x00C74B7Cu:  // uprising FactionSoviet
      return 2;
    case 0x00C62A0Cu:  // retail FactionJapan
    case 0x00C74B6Cu:  // uprising FactionJapan
      return 3;
    default:
      return 0;
  }
}

static int detect_faction_by_string(uint32_t player);
static FacCache* fac_slot(uint32_t player, bool create);
static int cached_faction_of(uint32_t player);

// Rate limit for the expensive faction graph walk (~40ms per seat). Shared by
// the stats board and the player pickers: one walk per second, two in a burst.
// Without it, an unlabelled seat cost ~40ms on every poll/frame — that is what
// made the match hitch every couple of seconds with 战况统计 open.
static int g_fac_walk_budget = 0;
static DWORD g_fac_walk_refill = 0;

static void fac_walk_refill_budget() {
  const DWORD now = GetTickCount();
  if (g_fac_walk_refill == 0) {
    g_fac_walk_refill = now;
    g_fac_walk_budget = 2;
    return;
  }
  if ((int32_t)(now - g_fac_walk_refill) < 1000) return;
  g_fac_walk_refill = now;
  if (g_fac_walk_budget < 2) ++g_fac_walk_budget;
}

// Expensive half of faction detection: bounded walk of the player object graph
// looking for a pointer to one of the image's side-name strings. Depth 3 covers
// Player -> Template -> Side -> name. Up to 128 nodes x 0x400 bytes (~33k
// protected reads), so callers must go through detect_faction_from_player(),
// which throttles and budgets it.
static int faction_walk_from_player(uint32_t player) {
  const uint32_t base = loaded_module();
  if (!base) return 0;
  const uint32_t lo = base;
  const uint32_t hi = base + 0x900000u;
  auto match = [&](uint32_t raw) -> int {
    if (raw < lo || raw >= hi) return 0;
    return faction_from_side_string_va(kModBase + (raw - base));
  };

  uint32_t queue[128];
  uint32_t seen[128];
  int qn = 0, sn = 0, qi = 0;
  auto enqueue = [&](uint32_t p) {
    if (!is_ptr(p) || qn >= 128) return;
    for (int i = 0; i < sn; ++i) {
      if (seen[i] == p) return;
    }
    if (sn < 128) seen[sn++] = p;
    queue[qn++] = p;
  };
  enqueue(player);
  int depth_end = qn;
  int found = 0;
  for (int depth = 0; depth < 3 && qi < qn && !found; ++depth) {
    const int stop = depth_end;
    for (; qi < stop; ++qi) {
      const uint32_t node = queue[qi];
      for (uint32_t off = 0; off <= 0x400; off += 4) {
        uint32_t v = 0;
        if (!safe_read_u32(node + off, &v)) continue;
        const int f = match(v);
        if (f) {
          found = f;
          break;
        }
        enqueue(v);
      }
      if (found) break;
    }
    depth_end = qn;
  }
  return found;
}

// Faction of one seat. Hits come straight from the cache.
//
// Misses run the residual probes (cheap one-hop string read first, then the
// graph walk). Both the stats board and the player pickers call this, and
// before it was throttled the walk ran for every seat on every poll — ~40ms
// each, i.e. the "hitch every couple of seconds" hang while 战况统计 was open.
// Now: one throttle slot per seat (always created, so no caller can bypass it),
// a per-poll walk budget, and exponential backoff for seats that never resolve.
static int detect_faction_from_player(uint32_t player) {
  if (!is_ptr(player)) return 0;
  for (int i = 0; i < g_fac_cache_n; ++i) {
    if (g_fac_cache[i].player == player && g_fac_cache[i].faction > 0) {
      return g_fac_cache[i].faction;
    }
  }
  FacCache* slot = fac_slot(player, true);
  const DWORD now_ms = GetTickCount();
  if (slot && slot->next_try && (int32_t)(now_ms - slot->next_try) < 0) return 0;

  int found = detect_faction_by_string(player);
  fac_walk_refill_budget();
  if (!found && g_fac_walk_budget > 0) {
    --g_fac_walk_budget;
    found = faction_walk_from_player(player);
  }
  if (found) {
    if (slot) {
      slot->faction = found;
      slot->pending = 0;
      slot->pending_hits = 0;
      slot->fails = 0;
    }
    return found;
  }
  // Nothing matched. Back off so an unlabelled seat stops costing ~40ms a poll;
  // 4s, 8s, 16s ... capped at a minute.
  if (slot) {
    if (slot->fails < 6) ++slot->fails;
    uint32_t back = 4000u;
    for (int i = 1; i < slot->fails && back < 60000u; ++i) back <<= 1;
    if (back > 60000u) back = 60000u;
    slot->next_try = now_ms + back;
  }
  return 0;
}

// Last resort: read a "Faction*" string straight off the player or one hop
// away. Cheap and only runs when the address walk found nothing.
static int detect_faction_by_string(uint32_t player) {
  if (!is_ptr(player)) return 0;
  for (uint32_t off = 0x40; off <= 0x280; off += 4) {
    uint32_t p = 0;
    if (!safe_read_u32(player + off, &p) || !is_ptr(p)) continue;
    char buf[48] = {};
    if (safe_read_bytes(p, buf, sizeof(buf) - 1) && std::strncmp(buf, "Faction", 7) == 0) {
      const int f = faction_from_side_name(buf);
      if (f) return f;
    }
    uint32_t p2 = 0;
    if (!safe_read_u32(p, &p2) || !is_ptr(p2)) continue;
    if (safe_read_bytes(p2, buf, sizeof(buf) - 1) && std::strncmp(buf, "Faction", 7) == 0) {
      const int f = faction_from_side_name(buf);
      if (f) return f;
    }
  }
  return 0;
}

static int detect_faction(const PlayerEconomy& pe) {
  int anchor[4] = {};
  int all[4] = {};
  for (int i = 0; i < pe.roster_count; ++i) {
    const int f = faction_of_key(pe.roster[i].key);
    if (f <= 0) continue;
    const int n = pe.roster[i].count > 0 ? pe.roster[i].count : 0;
    all[f] += n;
    if (faction_anchor_key(pe.roster[i].key)) anchor[f] += n;
  }
  int best = 0, best_n = 0;
  for (int f = 1; f <= 3; ++f) {
    if (anchor[f] > best_n) {
      best = f;
      best_n = anchor[f];
    }
  }
  if (best) return best;
  for (int f = 1; f <= 3; ++f) {
    if (all[f] > best_n) {
      best = f;
      best_n = all[f];
    }
  }
  if (best) return best;
  return detect_faction_from_player(pe.player);
}

// The engine always creates one extra seat for the neutral / civilian side.
// It carries no team colour at all (the default 0x808080 gray) and never
// receives starting money, so it can be told apart from a real participant.
static bool uprising_neutral_seat(const PlayerEconomy& pe) {
  if (pe.is_local) return false;
  const bool gray = pe.has_color && pe.color_r == 0x80 && pe.color_g == 0x80 && pe.color_b == 0x80;
  if (!gray) return false;
  // A real participant always receives starting cash, the neutral seat never does.
  return !pe.money;
}

// Uprising: no roster yet, so retail seat_entered_battle (needs unit counts)
// would hide everyone. Keep seats that look like real match players.
static bool uprising_seat_ok(const PlayerEconomy& pe) {
  if (!is_ptr(pe.player)) return false;
  if (seat_is_dummy_local(pe)) return false;
  if (!pe.is_local && pe.player_id == 0) return false;
  if (uprising_neutral_seat(pe)) return false;
  uint8_t observer = 0, replay = 0;
  if (!safe_read_bytes(pe.player + 0x106, &observer, 1) ||
      !safe_read_bytes(pe.player + 0x123C, &replay, 1)) {
    return pe.is_local;
  }
  if ((observer != 0 || replay != 0) && !pe.is_local) return false;
  if (pe.is_local) return true;
  if (!pe.has_money) return false;
  return pe.has_color || pe.has_power || pe.has_score;
}

static int anchor_count_for(const PlayerEconomy& pe, int faction) {
  int n = 0;
  if (faction <= 0) return 0;
  for (int i = 0; i < pe.roster_count; ++i) {
    if (faction_of_key(pe.roster[i].key) != faction) continue;
    if (!faction_anchor_key(pe.roster[i].key)) continue;
    if (pe.roster[i].count > 0) n += pe.roster[i].count;
  }
  return n;
}

static int remember_faction(uint32_t player, const PlayerEconomy& pe, int detected) {
  for (int i = 0; i < g_fac_cache_n; ++i) {
    if (g_fac_cache[i].player != player) continue;
    if (!g_fac_cache[i].faction) {
      g_fac_cache[i].faction = detected;
      g_fac_cache[i].pending = 0;
      g_fac_cache[i].pending_hits = 0;
      return detected;
    }
    // One stray enemy building (or a one-frame mis-count) must not retitle the
    // seat. Only switch after the new side's anchors lead for several polls.
    if (detected && detected != g_fac_cache[i].faction &&
        anchor_count_for(pe, detected) > anchor_count_for(pe, g_fac_cache[i].faction)) {
      if (g_fac_cache[i].pending == detected) {
        ++g_fac_cache[i].pending_hits;
      } else {
        g_fac_cache[i].pending = detected;
        g_fac_cache[i].pending_hits = 1;
      }
      if (g_fac_cache[i].pending_hits >= 4) {
        g_fac_cache[i].faction = detected;
        g_fac_cache[i].pending = 0;
        g_fac_cache[i].pending_hits = 0;
      }
    } else {
      g_fac_cache[i].pending = 0;
      g_fac_cache[i].pending_hits = 0;
    }
    return g_fac_cache[i].faction ? g_fac_cache[i].faction : detected;
  }
  if (g_fac_cache_n < 16) {
    g_fac_cache[g_fac_cache_n].player = player;
    g_fac_cache[g_fac_cache_n].faction = detected;
    ++g_fac_cache_n;
  }
  return detected;
}

// One diagnostic line per seat per match so a bad offset, a filtered-out real
// player, or a missed side pointer is visible in ra3_overlay.log.
static uint32_t g_seat_diag[16] = {};
static int g_seat_diag_n = 0;

static bool seat_diag_once(uint32_t player) {
  for (int i = 0; i < g_seat_diag_n; ++i) {
    if (g_seat_diag[i] == player) return false;
  }
  if (g_seat_diag_n < 16) g_seat_diag[g_seat_diag_n++] = player;
  return true;
}

static void clear_seat_diag() { g_seat_diag_n = 0; }

// Side objects and their name strings live inside the game image, so any
// player field that points back into the image is a candidate for the side
// pointer. Logged once per seat per match to pin the layout down.
static void dump_player_image_refs_once(uint32_t player) {
  const uint32_t base = loaded_module();
  if (!base || !is_ptr(player)) return;
  for (uint32_t off = 0; off < 0x400; off += 4) {
    uint32_t v = 0;
    if (!safe_read_u32(player + off, &v)) continue;
    if (v < base || v >= base + 0x900000u) continue;
    uint32_t w[4] = {};
    safe_read_u32(v, &w[0]);
    safe_read_u32(v + 4, &w[1]);
    safe_read_u32(v + 8, &w[2]);
    safe_read_u32(v + 12, &w[3]);
    char txt[24] = {};
    if (safe_read_bytes(v, txt, sizeof(txt) - 1)) {
      txt[sizeof(txt) - 1] = 0;
      for (size_t k = 0; k < sizeof(txt); ++k) {
        if (txt[k] == 0) break;
        if (txt[k] < 0x20 || txt[k] > 0x7E) {
          txt[0] = 0;
          break;
        }
      }
    }
    log("imgref p=%08X off=%03X rva=%X [%08X %08X %08X %08X] %s", player, off, v - base, w[0],
        w[1], w[2], w[3], txt[0] ? txt : "");
  }
}

static FacCache* fac_slot(uint32_t player, bool create) {
  for (int i = 0; i < g_fac_cache_n; ++i) {
    if (g_fac_cache[i].player == player) return &g_fac_cache[i];
  }
  if (create && g_fac_cache_n < 16) {
    g_fac_cache[g_fac_cache_n].player = player;
    ++g_fac_cache_n;
    return &g_fac_cache[g_fac_cache_n - 1];
  }
  return nullptr;
}

// Per-seat gate for the expensive faction probes (player graph walk + side
// string search). Shared by the economy board and the player pickers so a seat
// that stays unknown is never re-probed more than once every couple of seconds.
static bool fac_probe_due(uint32_t player) {
  FacCache* slot = fac_slot(player, true);
  if (!slot) return false;
  return !slot->next_try || (int32_t)(GetTickCount() - slot->next_try) >= 0;
}

static void apply_seat_labels(MatchEconomy* out) {
  if (!out) return;
  const bool up = uprising_active();
  for (int i = 0; i < out->player_count; ++i) {
    PlayerEconomy& pe = out->players[i];
    FacCache* slot = fac_slot(pe.player, true);
    int faction = slot ? slot->faction : 0;
    // detect_faction() is a roster tally first and only falls back to the
    // throttled, budgeted pointer walk inside detect_faction_from_player().
    if (!faction) {
      const int detected = detect_faction(pe);
      if (detected) faction = remember_faction(pe.player, pe, detected);
    }
    if (up && seat_diag_once(pe.player)) {
      uint8_t observer = 0, replay = 0;
      safe_read_bytes(pe.player + 0x106, &observer, 1);
      safe_read_bytes(pe.player + 0x123C, &replay, 1);
      log("seat p=%08X id=%u loc=%d money=%u hm=%d hp=%d hs=%d obs=%02X/%02X gray=%d fac=%d ok=%d",
          pe.player, pe.player_id, pe.is_local ? 1 : 0, pe.money, pe.has_money ? 1 : 0,
          pe.has_power ? 1 : 0, pe.has_score ? 1 : 0, (unsigned)observer, (unsigned)replay,
          uprising_neutral_seat(pe) ? 1 : 0, faction, uprising_seat_ok(pe) ? 1 : 0);
      dump_player_image_refs_once(pe.player);
    }
    const char* fac = faction == 1 ? u8"盟军" : faction == 2 ? u8"苏联" : faction == 3 ? u8"帝国" : u8"玩家";
    std::snprintf(pe.name, sizeof(pe.name), "%s", fac);
  }
}

// GameLogic+0x148. 1/2/4 are live matches; 6/7 are replay variants of those.
// 9 is the freshly constructed / shell value. Anything else is not a battle.
static uint32_t g_battle_logic = 0;
static float g_battle_seconds = -1.f;

struct ProtocolFace {
  const char* id;
  const char* name;
};

// Runtime object names are usually PlayerPower*. SpecialPower* is the template id.
static const ProtocolFace kProtocolFaces[] = {
    {"AbilityParadrop", u8"一级伞兵空降"},
    {"PlayerPowerCryoSatelliteLvl1", u8"一级冷冻射击"},
    {"SpecialPowerCryoSatelliteLvl1", u8"一级冷冻射击"},
    {"PlayerPowerCryoSatelliteLvl2", u8"二级冷冻射击"},
    {"SpecialPowerCryoSatelliteLvl2", u8"二级冷冻射击"},
    {"PlayerPowerCryoSatelliteLvl3", u8"三级冷冻射击"},
    {"SpecialPowerCryoSatelliteLvl3", u8"三级冷冻射击"},
    {"PlayerPowerChronoRiftLvl1", u8"一级时空裂缝"},
    {"SpecialPowerChronoRiftSelectLvl1", u8"一级时空裂缝"},
    {"SpecialPowerChronoRiftTeleportLvl1", u8"一级时空裂缝"},
    {"PlayerPowerChronoRiftLvl2", u8"二级时空裂缝"},
    {"SpecialPowerChronoRiftSelectLvl2", u8"二级时空裂缝"},
    {"SpecialPowerChronoRiftTeleportLvl2", u8"二级时空裂缝"},
    {"PlayerPowerChronoRiftLvl3", u8"三级时空裂缝"},
    {"SpecialPowerChronoRiftSelectLvl3", u8"三级时空裂缝"},
    {"SpecialPowerChronoRiftTeleportLvl3", u8"三级时空裂缝"},
    {"PlayerPowerTimeBombLvl1", u8"一级超时空炸弹"},
    {"SpecialPowerTimeBombLvl1", u8"一级超时空炸弹"},
    {"PlayerPowerTimeBombLvl2", u8"二级超时空炸弹"},
    {"SpecialPowerTimeBombLvl2", u8"二级超时空炸弹"},
    {"PlayerPowerTimeBombLvl3", u8"三级超时空炸弹"},
    {"SpecialPowerTimeBombLvl3", u8"三级超时空炸弹"},
    {"PlayerPowerSatelliteSweep", u8"侦查扫描"},
    {"SpecialPowerSatelliteSweep", u8"侦查扫描"},
    {"PlayerPowerAirPower", u8"先进航空学"},
    {"SpecialPowerAirPower", u8"先进航空学"},
    {"PlayerPowerPrecisionStrike", u8"精准轰炸"},
    {"SpecialPowerPrecisionStrike", u8"精准轰炸"},
    {"PlayerPowerAlliedFreeTrade", u8"自由贸易"},
    {"SpecialPowerAlliedFreeTrade", u8"自由贸易"},
    {"PlayerPowerChronoSwap", u8"超时空互换"},
    {"SpecialPowerChronoSwapSelect", u8"超时空互换"},
    {"SpecialPowerChronoSwapTeleport", u8"超时空互换"},
    {"PlayerPowerHighTechnology", u8"高科技"},
    {"SpecialPowerHighTechnology", u8"高科技"},
    {"PlayerPowerChronosphere", u8"超时空传送仪协议"},
    {"SpecialPowerChrono_Base", u8"超时空传送仪协议"},
    {"SpecialPowerChronosphereObjectSelect", u8"超时空传送仪协议"},
    {"SpecialPowerChronosphereObjectSpawn", u8"超时空传送仪协议"},
    {"PlayerPowerParticleCannon", u8"质子撞击炮协议"},
    {"SpecialPowerParticleCannon", u8"质子撞击炮协议"},
    {"SpecialPowerParticleCannonControlParticleCannon", u8"质子撞击炮协议"},
    {"SuperweaponTimerParticleCannon", u8"质子撞击炮协议"},
    {"PlayerPowerDesolatorBomb_Level1", u8"一级毁灭空袭"},
    {"SpecialPowerDesolatorBomb_Level1", u8"一级毁灭空袭"},
    {"PlayerPowerDesolatorBomb_Level2", u8"二级毁灭空袭"},
    {"SpecialPowerDesolatorBomb_Level2", u8"二级毁灭空袭"},
    {"PlayerPowerDesolatorBomb_Level3", u8"三级毁灭空袭"},
    {"SpecialPowerDesolatorBomb_Level3", u8"三级毁灭空袭"},
    {"PlayerPowerMagneticSatelliteLvl1", u8"一级磁力卫星"},
    {"SpecialPowerMagneticSatelliteLvl1", u8"一级磁力卫星"},
    {"PlayerPowerMagneticSatelliteLvl2", u8"二级磁力卫星"},
    {"SpecialPowerMagneticSatelliteLvl2", u8"二级磁力卫星"},
    {"PlayerPowerMagneticSatelliteLvl3", u8"三级磁力卫星"},
    {"SpecialPowerMagneticSatelliteLvl3", u8"三级磁力卫星"},
    {"PlayerPowerOrbitalRefuse_Rank1", u8"一级轨道抛掷"},
    {"SpecialPowerOrbitalRefuse_Rank1", u8"一级轨道抛掷"},
    {"PlayerPowerOrbitalRefuse_Rank2", u8"二级轨道抛掷"},
    {"SpecialPowerOrbitalRefuse_Rank2", u8"二级轨道抛掷"},
    {"PlayerPowerOrbitalRefuse_Rank3", u8"三级轨道抛掷"},
    {"SpecialPowerOrbitalRefuse_Rank3", u8"三级轨道抛掷"},
    {"PlayerPowerTerrorDroneEggs", u8"恐怖机器人奇袭"},
    {"SpecialPowerTerrorDroneEggs", u8"恐怖机器人奇袭"},
    {"PlayerPowerSovietMassProduction", u8"大生产"},
    {"SpecialPowerSovietMassProduction", u8"大生产"},
    {"PlayerPowerIrradiateTarget", u8"毒质腐蚀"},
    {"SpecialPower_IrradiateTarget", u8"毒质腐蚀"},
    {"PlayerPowerIronCurtain", u8"铁幕协议"},
    {"SpecialPowerIronCurtain", u8"铁幕协议"},
    {"SuperweaponTimerIronCurtain", u8"铁幕协议"},
    {"PlayerPowerProductionKickbacks", u8"现金奖励"},
    {"SpecialPower_ProductionKickbacks", u8"现金奖励"},
    {"PlayerPowerMagneticSingularity", u8"磁力奇点"},
    {"SpecialPowerMagneticSingularity", u8"磁力奇点"},
    {"PlayerPowerCrushPuppies", u8"重机碾压"},
    {"SpecialPowerCrushPuppies", u8"重机碾压"},
    {"PlayerPowerVacuumBomb", u8"真空内爆弹协议"},
    {"SpecialPowerVacuumBomb", u8"真空内爆弹协议"},
    {"PlayerPowerJapanEmperorsResolve_L1", u8"一级天皇之怒"},
    {"SpecialPowerJapanEmperorsResolve_L1", u8"一级天皇之怒"},
    {"PlayerPowerJapanEmperorsResolve_L2", u8"二级天皇之怒"},
    {"SpecialPowerJapanEmperorsResolve_L2", u8"二级天皇之怒"},
    {"PlayerPowerJapanEmperorsResolve_L3", u8"三级天皇之怒"},
    {"SpecialPowerJapanEmperorsResolve_L3", u8"三级天皇之怒"},
    {"PlayerPowerJapanBalloonAttack_L1", u8"一级气球炸弹"},
    {"SpecialPowerJapanBalloonAttack_L1", u8"一级气球炸弹"},
    {"PlayerPowerJapanBalloonAttack_L2", u8"二级气球炸弹"},
    {"SpecialPowerJapanBalloonAttack_L2", u8"二级气球炸弹"},
    {"PlayerPowerJapanBalloonAttack_L3", u8"三级气球炸弹"},
    {"SpecialPowerJapanBalloonAttack_L3", u8"三级气球炸弹"},
    {"PlayerPowerFinalSquadron_L1", u8"一级决胜中队"},
    {"SpecialPowerFinalSquadron_L1", u8"一级决胜中队"},
    {"PlayerPowerFinalSquadron_L2", u8"二级决胜中队"},
    {"SpecialPowerFinalSquadron_L2", u8"二级决胜中队"},
    {"PlayerPowerFinalSquadron_L3", u8"三级决胜中队"},
    {"SpecialPowerFinalSquadron_L3", u8"三级决胜中队"},
    {"PlayerPowerJapanNavalPower", u8"强化舰队"},
    {"SpecialPowerJapanNavalPower", u8"强化舰队"},
    {"PlayerPowerJapanEnhancedKamikaze", u8"光荣自爆"},
    {"SpecialPowerJapanEnhancedKamikaze", u8"光荣自爆"},
    {"PlayerPowerJapanAdvancedMissilePacks", u8"先进导弹匣"},
    {"SpecialPowerJapanAdvancedMissilePacks", u8"先进导弹匣"},
    {"PlayerPowerJapanAmbush", u8"惊骇伏击"},
    {"SpecialPowerJapanAmbush", u8"惊骇伏击"},
    {"PlayerPowerJapanRoboticAssembly", u8"机械化组装"},
    {"SpecialPowerJapanRoboticAssembly", u8"机械化组装"},
    {"PlayerPowerPointDefenseDrones", u8"点防御无人机"},
    {"SpecialPower_PointDefenseDrones", u8"点防御无人机"},
    {"PlayerPowerNanoSwarmHive", u8"纳米虫群协议"},
    {"SpecialPowerNanoSwarmHive", u8"纳米虫群协议"},
    {"PlayerPowerTelekineticProjectorDevice", u8"超能波毁灭装置协议"},
    {"SpecialPowerTelekineticProjectorDevice", u8"超能波毁灭装置协议"},
    {"PsyonicBlastSpecialPower", u8"超能波毁灭装置协议"},
    {"SpecialPower_PsyonicBlast", u8"超能波毁灭装置协议"},
    // Sidebar / science ids. These are the names stored on the player, not the template ids above.
    {"PlayerPower_AdvMissilePacks", u8"先进导弹匣"},
    {"PlayerPower_AirPower", u8"先进航空学"},
    {"PlayerPower_Ambush", u8"惊骇伏击"},
    {"PlayerPower_BalloonAttack1", u8"一级气球炸弹"},
    {"PlayerPower_BalloonAttack2", u8"二级气球炸弹"},
    {"PlayerPower_BalloonAttack3", u8"三级气球炸弹"},
    {"PlayerPower_ChronoRift1", u8"一级时空裂缝"},
    {"PlayerPower_ChronoRift2", u8"二级时空裂缝"},
    {"PlayerPower_ChronoRift3", u8"三级时空裂缝"},
    {"PlayerPower_ChronoSwap", u8"超时空互换"},
    {"PlayerPower_Chronosphere", u8"超时空传送仪协议"},
    {"PlayerPower_CrushPuppies", u8"重机碾压"},
    {"PlayerPower_CryoSatellite1", u8"一级冷冻射击"},
    {"PlayerPower_CryoSatellite2", u8"二级冷冻射击"},
    {"PlayerPower_CryoSatellite3", u8"三级冷冻射击"},
    {"PlayerPower_DesolatorBomb1", u8"一级毁灭空袭"},
    {"PlayerPower_DesolatorBomb2", u8"二级毁灭空袭"},
    {"PlayerPower_DesolatorBomb3", u8"三级毁灭空袭"},
    {"PlayerPower_EmperorRage1", u8"一级天皇之怒"},
    {"PlayerPower_EmperorRage2", u8"二级天皇之怒"},
    {"PlayerPower_EmperorRage3", u8"三级天皇之怒"},
    {"PlayerPower_EnhancedKamikaze", u8"光荣自爆"},
    {"PlayerPower_FinalSquadron1", u8"一级决胜中队"},
    {"PlayerPower_FinalSquadron2", u8"二级决胜中队"},
    {"PlayerPower_FinalSquadron3", u8"三级决胜中队"},
    {"PlayerPower_FreeTrade", u8"自由贸易"},
    {"PlayerPower_HighTechnology", u8"高科技"},
    {"PlayerPower_IronCurtain", u8"铁幕协议"},
    {"PlayerPower_IrradiateTarget", u8"毒质腐蚀"},
    {"PlayerPower_MagneticSatellite1", u8"一级磁力卫星"},
    {"PlayerPower_MagneticSatellite2", u8"二级磁力卫星"},
    {"PlayerPower_MagneticSatellite3", u8"三级磁力卫星"},
    {"PlayerPower_MagneticSingularity", u8"磁力奇点"},
    {"PlayerPower_MassProduction", u8"大生产"},
    {"PlayerPower_Nanoswarm", u8"纳米虫群协议"},
    {"PlayerPower_NavalPower", u8"强化舰队"},
    {"PlayerPower_OrbitalRefuse1", u8"一级轨道抛掷"},
    {"PlayerPower_OrbitalRefuse2", u8"二级轨道抛掷"},
    {"PlayerPower_OrbitalRefuse3", u8"三级轨道抛掷"},
    {"PlayerPower_Paradrop1", u8"一级伞兵空降"},
    {"PlayerPower_Paradrop2", u8"二级伞兵空降"},
    {"PlayerPower_Paradrop3", u8"三级伞兵空降"},
    {"PlayerPower_ParticleCannon", u8"质子撞击炮协议"},
    {"PlayerPower_PointDefenseDrones", u8"点防御无人机"},
    {"PlayerPower_PrecisionStrike", u8"精准轰炸"},
    {"PlayerPower_ProductionKickback", u8"现金奖励"},
    {"PlayerPower_RoboticAssembly", u8"机械化组装"},
    {"PlayerPower_SatelliteSweep", u8"侦查扫描"},
    {"PlayerPower_TerrorDroneEggs", u8"恐怖机器人奇袭"},
    {"PlayerPower_TimeBomb1", u8"一级超时空炸弹"},
    {"PlayerPower_TimeBomb2", u8"二级超时空炸弹"},
    {"PlayerPower_TimeBomb3", u8"三级超时空炸弹"},
    {"PlayerPower_VacuumBomb", u8"真空内爆弹协议"},
};

static const ProtocolFace* match_protocol_text(const char* s) {
  if (!s || !s[0]) return nullptr;
  for (const ProtocolFace& face : kProtocolFaces) {
    if (std::strcmp(s, face.id) == 0) return &face;
  }
  // Button art ids are Button_PlayerPower_CryoSatellite1 and the same tail is the science id.
  if (std::strncmp(s, "Button_", 7) == 0) {
    for (const ProtocolFace& face : kProtocolFaces) {
      if (std::strcmp(s + 7, face.id) == 0) return &face;
    }
  }
  return nullptr;
}

static const ProtocolFace* protocol_at_cstr(uint32_t addr) {
  char buf[96] = {};
  if (!safe_read_bytes(addr, buf, sizeof(buf) - 1)) return nullptr;
  buf[sizeof(buf) - 1] = 0;
  size_t n = 0;
  while (n < 80 && buf[n]) {
    const unsigned char c = (unsigned char)buf[n];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) return nullptr;
    ++n;
  }
  if (n < 8 || buf[n] != 0) return nullptr;
  return match_protocol_text(buf);
}

static const ProtocolFace* protocol_from_object(uint32_t obj) {
  if (!is_ptr(obj)) return nullptr;
  const ProtocolFace* hit = protocol_at_cstr(obj);
  if (hit) return hit;
  for (uint32_t off = 0; off <= 0x180; off += 4) {
    hit = protocol_at_cstr(obj + off);
    if (hit) return hit;
    uint32_t p = 0;
    if (!safe_read_u32(obj + off, &p) || !is_ptr(p)) continue;
    hit = protocol_at_cstr(p);
    if (hit) return hit;
    uint32_t p2 = 0;
    if (safe_read_u32(p, &p2) && is_ptr(p2)) {
      hit = protocol_at_cstr(p2);
      if (hit) return hit;
    }
    if (safe_read_u32(p + 4, &p2) && is_ptr(p2)) {
      hit = protocol_at_cstr(p2);
      if (hit) return hit;
    }
  }
  return nullptr;
}

static int protocol_remain_sec(uint32_t ready, uint32_t now, uint32_t dur) {
  if (ready <= now) return 0;
  const uint32_t left = ready - now;
  if (left > 15u * 900u) return 0;
  if (dur >= 15 && dur <= 15u * 900u && left > dur + 15u * 8u) return 0;
  int sec = (int)((left + 14u) / 15u);
  if (sec < 1) sec = 1;
  return sec;
}

static void note_protocol(PlayerEconomy* pe, const ProtocolFace* face, int remain) {
  if (!pe || !face) return;
  for (int i = 0; i < pe->protocol_count; ++i) {
    if (std::strcmp(pe->protocols[i].name, face->name) == 0) {
      if (remain > pe->protocols[i].remain_sec) pe->protocols[i].remain_sec = remain;
      return;
    }
  }
  if (pe->protocol_count >= kMaxProtocols) return;
  ProtocolStatus& row = pe->protocols[pe->protocol_count++];
  std::snprintf(row.key, sizeof(row.key), "%s", face->id);
  std::snprintf(row.name, sizeof(row.name), "%s", face->name);
  row.remain_sec = remain;
}

// Special-power templates keep the interned name at [[[obj+8]]+4].
static const ProtocolFace* face_from_name_chain(uint32_t obj) {
  if (!is_ptr(obj)) return nullptr;
  uint32_t a = 0, b = 0, s = 0;
  if (safe_read_u32(obj + 8, &a) && is_ptr(a) && safe_read_u32(a, &b) && is_ptr(b) &&
      safe_read_u32(b + 4, &s) && is_ptr(s)) {
    if (const ProtocolFace* face = protocol_at_cstr(s)) return face;
  }
  if (safe_read_u32(obj, &a) && is_ptr(a)) {
    if (safe_read_u32(a + 4, &s) && is_ptr(s)) {
      if (const ProtocolFace* face = protocol_at_cstr(s)) return face;
    }
    if (safe_read_u32(a, &b) && is_ptr(b) && safe_read_u32(b + 4, &s) && is_ptr(s)) {
      if (const ProtocolFace* face = protocol_at_cstr(s)) return face;
    }
  }
  return nullptr;
}

static const ProtocolFace* face_from_ptr(uint32_t obj) {
  if (!is_ptr(obj)) return nullptr;
  if (const ProtocolFace* face = protocol_at_cstr(obj)) return face;
  if (const ProtocolFace* face = face_from_name_chain(obj)) return face;
  if (const ProtocolFace* face = protocol_from_object(obj)) return face;
  uint32_t inner = 0;
  if (safe_read_u32(obj + 4, &inner) && is_ptr(inner) && inner != obj) {
    if (const ProtocolFace* face = protocol_at_cstr(inner)) return face;
    if (const ProtocolFace* face = face_from_name_chain(inner)) return face;
    if (const ProtocolFace* face = protocol_from_object(inner)) return face;
  }
  return nullptr;
}

// The player bag stores [template+0xDC], and command slots store the interned
// key that logic+0xBC maps back to that template. Neither value is the template
// pointer, so the name has to be recovered from the factory first.
struct ProtoKey {
  uint32_t key = 0;
  const ProtocolFace* face = nullptr;
};
static ProtoKey g_proto_keys[640];
static int g_proto_key_n = 0;
static uint32_t g_proto_factory = 0;
static DWORD g_proto_try_at = 0;
static int g_proto_empty_tries = 0;

static void remember_proto_key(uint32_t key, const ProtocolFace* face) {
  if (key == 0 || !face) return;
  for (int i = 0; i < g_proto_key_n; ++i) {
    if (g_proto_keys[i].key != key) continue;
    if (g_proto_keys[i].face && std::strcmp(g_proto_keys[i].face->name, face->name) != 0) {
      g_proto_keys[i].face = nullptr;
    }
    return;
  }
  if (g_proto_key_n >= 640) return;
  g_proto_keys[g_proto_key_n].key = key;
  g_proto_keys[g_proto_key_n].face = face;
  ++g_proto_key_n;
}

static const ProtocolFace* face_from_proto_key(uint32_t key) {
  if (key == 0) return nullptr;
  for (int i = 0; i < g_proto_key_n; ++i) {
    if (g_proto_keys[i].key == key) return g_proto_keys[i].face;
  }
  return nullptr;
}

static bool buf_is_protocol_prefix(const char* buf) {
  return std::memcmp(buf, "PlayerPower", 11) == 0 || std::memcmp(buf, "SpecialPower", 12) == 0 ||
         std::memcmp(buf, "Superweapon", 11) == 0 || std::memcmp(buf, "AbilityParadrop", 15) == 0 ||
         std::memcmp(buf, "PsyonicBlast", 12) == 0;
}

static bool template_might_be_protocol(uint32_t tmpl) {
  char buf[20] = {};
  auto hit_at = [&](uint32_t addr) {
    if (!safe_read_bytes(addr, buf, 16)) return false;
    buf[16] = 0;
    return buf_is_protocol_prefix(buf);
  };
  for (uint32_t off = 0; off <= 0x80; off += 4) {
    if (hit_at(tmpl + off)) return true;
    uint32_t p = 0;
    if (!safe_read_u32(tmpl + off, &p) || !is_ptr(p)) continue;
    if (hit_at(p)) return true;
    uint32_t p2 = 0;
    if (safe_read_u32(p + 4, &p2) && is_ptr(p2) && hit_at(p2)) return true;
  }
  uint32_t a = 0, b = 0, s = 0;
  if (safe_read_u32(tmpl + 8, &a) && is_ptr(a) && safe_read_u32(a, &b) && is_ptr(b) &&
      safe_read_u32(b + 4, &s) && is_ptr(s) && hit_at(s)) {
    return true;
  }
  return false;
}

static void rebuild_protocol_keys() {
  g_proto_key_n = 0;
  g_proto_factory = 0;
  if (!module_base()) return;
  uint32_t factory = 0;
  if (!safe_read_u32(module_base() + 0x008EBDE8, &factory) || !is_ptr(factory)) return;
  g_proto_factory = factory;
  uint32_t tmpl = 0;
  if (safe_read_u32(factory + 0x24, &tmpl)) {
    for (int n = 0; n < 8192 && is_ptr(tmpl); ++n) {
      uint32_t vt = 0;
      if (!safe_read_u32(tmpl, &vt) || !is_ptr(vt)) break;
      if (template_might_be_protocol(tmpl)) {
        if (const ProtocolFace* face = face_from_ptr(tmpl)) {
          remember_proto_key(tmpl, face);
          uint32_t dc = 0;
          uint32_t sci = 0;
          if (safe_read_u32(tmpl + 0xDC, &dc)) remember_proto_key(dc, face);
          if (safe_read_u32(tmpl + 0x134, &sci)) remember_proto_key(sci, face);
        }
      }
      uint32_t next = 0;
      if (!safe_read_u32(tmpl + 0xC, &next) || next == tmpl) break;
      tmpl = next;
    }
  }
  uint32_t logic = 0;
  if (!safe_read_u32(module_base() + 0x8DDE84, &logic) || !is_ptr(logic)) return;
  uint32_t buckets = 0;
  uint32_t count = 0;
  if (!safe_read_u32(logic + 0xBC + 4, &buckets) || !is_ptr(buckets)) return;
  if (!safe_read_u32(logic + 0xBC + 8, &count) || count == 0 || count > 20000) return;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = 0;
    if (!safe_read_u32(buckets + i * 4u, &node)) continue;
    for (int guard = 0; is_ptr(node) && guard < 32; ++guard) {
      uint32_t key = 0, val = 0, next = 0;
      if (!safe_read_u32(node, &key)) break;
      safe_read_u32(node + 4, &val);
      const ProtocolFace* face = face_from_proto_key(val);
      if (!face && is_ptr(val)) {
        uint32_t dc = 0;
        if (safe_read_u32(val + 0xDC, &dc)) face = face_from_proto_key(dc);
      }
      if (!face) face = protocol_at_cstr(key);
      if (face) {
        remember_proto_key(key, face);
        if (is_ptr(val)) remember_proto_key(val, face);
      }
      if (!safe_read_u32(node + 8, &next) || next == node) break;
      node = next;
    }
  }
}

static void ensure_protocol_keys() {
  uint32_t factory = 0;
  if (module_base()) safe_read_u32(module_base() + 0x008EBDE8, &factory);
  if (g_proto_key_n > 0 && factory == g_proto_factory && is_ptr(factory)) return;
  const DWORD now = GetTickCount();
  // A rebuild walks the whole ThingFactory template list plus the GameLogic
  // object hash (~45ms). It used to retry every 2s when it came back empty —
  // i.e. on nearly every other stats poll, which is a 45ms hitch each time.
  // Retry quickly a couple of times (factory may still be filling up), then
  // back off hard.
  if (g_proto_key_n == 0 && factory == g_proto_factory && is_ptr(factory)) {
    const DWORD wait = g_proto_empty_tries < 3 ? 4000u : 30000u;
    if (now - g_proto_try_at < wait) return;
  }
  g_proto_try_at = now;
  rebuild_protocol_keys();
  if (g_proto_key_n == 0) {
    if (g_proto_empty_tries < 1000) ++g_proto_empty_tries;
  } else {
    g_proto_empty_tries = 0;
  }
}

static bool read_ptr_span(uint32_t begin_addr, uint32_t end_addr, uint32_t* begin, uint32_t* end,
                          uint32_t max_n) {
  if (!begin || !end) return false;
  *begin = 0;
  *end = 0;
  if (!safe_read_u32(begin_addr, begin) || !safe_read_u32(end_addr, end)) return false;
  if (!is_ptr(*begin) || !is_ptr(*end) || *end < *begin) return false;
  if (((*end - *begin) % 4u) != 0) return false;
  const uint32_t n = (*end - *begin) / 4u;
  return n > 0 && n <= max_n;
}

static void fill_player_protocols(PlayerEconomy* pe) {
  if (!pe || !is_ptr(pe->player)) return;
  pe->protocol_count = 0;
  ensure_protocol_keys();
  uint32_t logic = 0;
  uint32_t now = 0;
  if (safe_read_u32(loaded_module() + battle_logic_rva(), &logic) && is_ptr(logic)) {
    safe_read_u32(logic + 0x50, &now);
  }

  struct TimerHit {
    uint32_t key = 0;
    int remain = 0;
    const ProtocolFace* face = nullptr;
  };
  TimerHit timers[24] = {};
  int timer_n = 0;
  const uint32_t head = pe->player + 0x218;
  uint32_t node = 0;
  if (safe_read_u32(head, &node)) {
    for (int n = 0; n < 24 && is_ptr(node) && node != head; ++n) {
      uint32_t key = 0, ready = 0, dur = 0, next = 0;
      safe_read_u32(node + 8, &key);
      safe_read_u32(node + 0x0C, &ready);
      safe_read_u32(node + 0x10, &dur);
      const ProtocolFace* face = face_from_ptr(key);
      if (!face) face = face_from_proto_key(key);
      const int remain = protocol_remain_sec(ready, now, dur);
      if (timer_n < 24 && key != 0) {
        timers[timer_n].key = key;
        timers[timer_n].remain = remain;
        timers[timer_n].face = face;
        ++timer_n;
      }
      if (!safe_read_u32(node, &next) || next == node) break;
      node = next;
    }
  }

  auto remain_for = [&](const ProtocolFace* face, uint32_t key) -> int {
    int best = 0;
    if (key != 0) {
      for (int i = 0; i < timer_n; ++i) {
        if (timers[i].key == key && timers[i].remain > best) best = timers[i].remain;
      }
    }
    if (!face) return best;
    for (int i = 0; i < timer_n; ++i) {
      if (!timers[i].face) continue;
      if (std::strcmp(timers[i].face->name, face->name) != 0) continue;
      if (timers[i].remain > best) best = timers[i].remain;
    }
    return best;
  };

  auto note_value = [&](uint32_t v) {
    if (v == 0) return;
    const ProtocolFace* face = nullptr;
    if (is_ptr(v)) face = face_from_ptr(v);
    if (!face) face = face_from_proto_key(v);
    if (!face && is_ptr(v)) {
      uint32_t dc = 0;
      if (safe_read_u32(v + 0xDC, &dc)) face = face_from_proto_key(dc);
      if (!face) {
        uint32_t sci = 0;
        if (safe_read_u32(v + 0x134, &sci)) face = face_from_proto_key(sci);
      }
    }
    if (!face) return;
    note_protocol(pe, face, remain_for(face, v));
  };

  auto note_span = [&](uint32_t begin, uint32_t end) {
    const uint32_t n = (end - begin) / 4u;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t obj = 0;
      if (!safe_read_u32(begin + i * 4u, &obj) || obj == 0) continue;
      note_value(obj);
    }
  };

  // Command-bar slots. Each chosen protocol and each built superweapon sits here.
  for (uint32_t i = 0; i < 0x55u; ++i) {
    const uint32_t slot = pe->player + 0x248u + i * 0x30u;
    uint32_t begin = 0, end = 0;
    if (read_ptr_span(slot + 8, slot + 0xC, &begin, &end, 8)) note_span(begin, end);
    if (read_ptr_span(slot + 0x1C, slot + 0x20, &begin, &end, 8)) note_span(begin, end);
  }

  uint32_t bag = 0;
  if (safe_read_u32(pe->player + 0x1238, &bag) && is_ptr(bag)) {
    uint32_t begin = 0, end = 0;
    if (read_ptr_span(bag + 8, bag + 0xC, &begin, &end, 24)) note_span(begin, end);
  }

  uint32_t sci = 0;
  if (safe_read_u32(pe->player + 0x1320, &sci) && is_ptr(sci)) {
    uint32_t begin = 0, end = 0;
    if (read_ptr_span(sci + 0x18, sci + 0x1C, &begin, &end, 24)) note_span(begin, end);
    if (read_ptr_span(sci + 0x44, sci + 0x48, &begin, &end, 24)) note_span(begin, end);
  }

  for (int i = 0; i < timer_n; ++i) {
    if (timers[i].face) note_protocol(pe, timers[i].face, timers[i].remain);
  }
}

// Uprising: dump the raw dwords around the special-power list head plus any
// node reachable from the player whose +0x10 looks like a cooldown duration,
// so the real container can be identified instead of guessed.
static void probe_power_list_once(uint32_t player) {
  uint32_t now = 0;
  uint32_t logic = 0;
  if (safe_read_u32(loaded_module() + battle_logic_rva(), &logic) && is_ptr(logic)) {
    safe_read_u32(logic + 0x50, &now);
  }
  log("-- probe player=%08X now=%08X --", player, now);
  {
    uint32_t list = 0;
    safe_read_u32(loaded_module() + player_list_rva(), &list);
    uint32_t cnt = 0;
    safe_read_u32(list + 0x2C, &cnt);
    log("plist=%08X active=%u local=%08X head218=%08X", list, cnt, player,
        *reinterpret_cast<uint32_t*>(player + 0x218));
  }
  for (uint32_t base = 0x200; base <= 0x2A0; base += 0x10) {
    uint32_t v[4] = {};
    safe_read_u32(player + base, &v[0]);
    safe_read_u32(player + base + 4, &v[1]);
    safe_read_u32(player + base + 8, &v[2]);
    safe_read_u32(player + base + 12, &v[3]);
    log("pb %03X: %08X %08X %08X %08X", base, v[0], v[1], v[2], v[3]);
  }
  const uint32_t heads[] = {0x218, 0x248, 0x1238, 0x1320};
  for (uint32_t hi = 0; hi < 4; ++hi) {
    const uint32_t head = player + heads[hi];
    uint32_t node = 0;
    if (!safe_read_u32(head, &node)) continue;
    log("ph %03X: node=%08X self=%d", heads[hi], node, node == head ? 1 : 0);
    for (int k = 0; k < 4 && is_ptr(node) && node != head; ++k) {
      uint32_t w[6] = {};
      for (int j = 0; j < 6; ++j) safe_read_u32(node + j * 4, &w[j]);
      log("pn %03X[%d] %08X: %08X %08X %08X %08X %08X %08X", heads[hi], k, node, w[0], w[1],
          w[2], w[3], w[4], w[5]);
      uint32_t next = 0;
      if (!safe_read_u32(node, &next) || next == node) break;
      node = next;
    }
  }
  // Any pointer reachable from the player that carries a cooldown-shaped pair.
  int hits = 0;
  for (uint32_t off = 0; off < 0x1400 && hits < 40; off += 4) {
    uint32_t p = 0;
    if (!safe_read_u32(player + off, &p) || !is_ptr(p)) continue;
    uint32_t key = 0, ready = 0, dur = 0;
    if (!safe_read_u32(p + 8, &key)) continue;
    if (!safe_read_u32(p + 0x0C, &ready)) continue;
    if (!safe_read_u32(p + 0x10, &dur)) continue;
    if (dur == 0 || dur >= 0x200000u) continue;
    if (ready == 0 || ready > now + 0x200000u) continue;
    log("pcand off=%03X p=%08X key=%08X ready=%08X dur=%08X d=%d", off, p, key, ready, dur,
        (int32_t)(ready - now));
    ++hits;
  }
  log("-- probe done hits=%d --", hits);
}

// ---------------------------------------------------------------------------
// Uprising: local player plumbing for the no-cooldown family.
//
// Retail flips the special-power module's "ready at" field ([module + 0x20]) to
// 1 from the MustCode SuperPower stub at 0x729F19. The Uprising twin sits at
// 0x73BB19 and reads the owner as [module - 8 + 0x428] instead of +0x418, but
// the stub only knows the local player through IDB, and IDB is refreshed by the
// PlayerID hook, which the engine only calls while it draws the local player's
// id. After a new match IDB can therefore still point at the previous player and
// the owner test never matches — superweapons and protocol powers keep their
// cooldowns. Keep IDB in step with ThePlayerList and answer the module
// readiness predicates (SpecialPowerModule::isReady / ready-ratio) directly for
// the local player's modules, so the feature no longer depends on the stub.
// ---------------------------------------------------------------------------
static uint32_t uprising_list_local() {
  if (!uprising_active() || !loaded_module()) return 0;
  uint32_t list = 0;
  if (!safe_read_u32(loaded_module() + player_list_rva(), &list) || !is_ptr(list)) return 0;
  uint32_t local = 0;
  if (!safe_read_u32(list + kPlayerListLocalOff, &local) || !is_ptr(local)) return 0;
  return local;
}

static uint32_t g_last_module_owner = 0;
static uint32_t g_ready_bool_hits = 0;
static uint32_t g_ready_ratio_hits = 0;

// [module - 8] is the module container; its +0x428 field is the owner player
// (the engine itself reads it at 0x73BB19). Smallest possible work per call.
static bool sp_module_owned_by_local(void* self) {
  const uint32_t s = reinterpret_cast<uint32_t>(self);
  if (!is_ptr(s)) return false;
  uint32_t container = 0;
  if (!safe_read_u32(s - 8u, &container) || !is_ptr(container)) return false;
  uint32_t owner = 0;
  // 容器 +0x418 = Player*（零售 1.12，见 0x007489C6），起义是 +0x428。
  const uint32_t off = uprising_active() ? 0x428u : 0x418u;
  if (!safe_read_u32(container + off, &owner) || !is_ptr(owner)) return false;
  g_last_module_owner = owner;
  const uint32_t local = uprising_list_local();
  if (local && owner == local) return true;
  if (idb_base()) {
    const uint32_t cached = read_u32(reinterpret_cast<uint32_t>(idb_base()));
    if (is_ptr(cached) && owner == cached) return true;
  }
  return false;
}

// Uprising 0x75BB90: SpecialPowerModule::isReady(). Both of its branches agree
// once [module + 0x20] is in the past, so report ready outright for the local
// player's modules while a ready toggle (0x18/0x19/0x1A -> 0x0D) is on.
// 协议限制优先：「禁止」的协议即使开了「协议无冷却」也不许就绪。
//
// 名字链（0x0087CBE0 / 0x00883640 逐条核对过）：
//   A        = [template + 8]
//   NameDesc = [A]                  <- { const char* name; uint32_t key; }
//   name     = [NameDesc + 0]       <- cstr
//   key      = [NameDesc + 4]       <- 冷却表 / 禁用表里比的就是这个 dword
// 旧代码把 [NameDesc+4] 当成字符串指针，所以一次都没匹配上（日志 hits=0）。
// 这条链不依赖 [module-8]+0x418 之类的容器偏移，零售/起义同一套代码。
// 禁止表对整个对局的所有座位生效（本机也算），改哪条协议那条就点不了。
static bool protocol_banned(const char* id);                     // defined below
static bool protocol_ban_active();                               // defined below
static void remember_ban_ptr(uint32_t self, uint32_t tmpl);      // defined below
static const char* row_id_for_engine_name(const char* s);        // defined below
static const char* row_for_engine_key(uint32_t key);             // defined below
static void record_engine_key(uint32_t key, const char* row);    // defined below
static bool read_name_ascii(uint32_t addr, char* buf, size_t cap);// defined below
static void install_protocol_tech_hook();                        // defined below
static void install_protocol_store_hook();                       // defined below
// 协议商店对话框逐行填充的统计（安装钩子在下方，这里先声明供 tick 日志使用）
static uint32_t g_fill_calls;
static uint32_t g_fill_hits;
static char g_fill_log_last[160];
static char g_fill_seen[8192];
static char g_fill_seen_logged[8192];
// 协议面板（igm_PlayerTechStoreDialog）行槽探测
static void install_protocol_panel_hook();
static void install_protocol_invoke_hook();
static void install_protocol_grid_hook();
static void install_protocol_avail_hook();
static void install_protocol_lock_hook();
static void install_protocol_avail_set_hook();
static void install_protocol_flash_set_hook();
static bool protocol_key_banned(uint32_t key);                    // defined below
static bool btn_holds_forced_obj(uint32_t self);                  // defined below
static void majority_locked_image(uint32_t* ref_out);             // defined below
static void force_cell(uint32_t impl, uint32_t idx, const char* why);  // defined below
static void protocol_panel_live_lock();                           // defined below
static void protocol_panel_clear_locks();                         // defined below
static bool tech_graph_matches_ban(uint32_t obj, char* out, size_t out_sz);   // below
static bool tech_namekey(uint32_t tech, uint32_t* key_out, char* name_out, size_t name_sz);

// ---------------------------------------------------------------------------
// 0xA44AB0：协议面板每个格子按钮的"刷新外观"虚函数（vtable 0xC79670）。
//     [this+0x60] = 0 available / 1 locked / 2 owned
//     0xC78FA8   = { "available", "locked", "owned" }
//     00A44ADE   mov ecx,[ecx*4+0xC78FA8]     <- 取状态名
//     00A44AFF   push "SetAvailability"       <- 真正画锁图的属性
//     00A44B06   call 0x953F60
// 只要在这里把 [this+0x60] 改成 1，引擎就会自己调 SetAvailability("locked")，
// 锁图和置灰完全由引擎绘制 —— 和建造限制的观感一致。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 协议面板槽位几何（用实测日志反推，15 格全部吻合）
//
//   dialog      = [rowlookup_this + 4]
//   slot(idx)   = dialog + 0x108 + idx*0xBC     // 0xA71BB0 的返回值
//   button(idx) = slot(idx) + 8                 // 0xA44AB0 的 this
//   idx         = a1 + 5*a2                     // 0xA1B470 的算法
//
// 定义在文件前面的按钮钩子附近（btn_refresh_pre 需要用到）。
// ---------------------------------------------------------------------------

static uint32_t g_panel_dialog = 0;        // 面板 dialog 对象（= [rowlookup_this+4]）
static bool g_cell_locked[16] = {};        // 被限制协议所在格子（按 idx）
static DWORD g_panel_row_tick = 0;         // 面板最近一次构建行数据的时间
static uint32_t g_cell_impl = 0;         // 状态数组持有者（0xA22480 的 [self+4]）
static uint32_t g_panel_lookup_self = 0;  // 0xA22480 的 this（供主动重扫）

// ---------------------------------------------------------------------------
// 把被限制的格子做成"和引擎原生锁定格一模一样"
//
// 0xA44AB0 推给 Flash 的属性只有四个（反汇编确认）：
//     [this+0x5C] -> "Image"                        图标资源
//     [this+0x60] -> "SetAvailability"              available/locked/owned
//     [this+0x70]/[this+0x71] -> "SetDependenceUponPriorRow"
//     [this+0x72] -> "SetSelected"
// 没有单独的 SetLocked，所以锁图只可能来自 Image 或 SetAvailability("locked")。
// 与其猜，不如直接抄一个引擎自己锁着的格子的这些字段：只要所有原生锁定格
// 的 Image 指针一致，它就一定是通用锁图资源，抄过来必然是同一张锁图。
// ---------------------------------------------------------------------------
static uint32_t g_ref_cell = 0;          // 参考用的原生锁定格（按钮地址）
static uint32_t g_ref_image = 0;         // 参考格的 Image 指针
static int g_ref_state = 0;              // 0=未知 1=可安全抄 -1=Image 不一致
static bool g_cell_visual_logged[16] = {};
static bool g_visual_copied[16] = {};    // 已把该格外观抄成锁定样式

// 扫描 15 格，找出一个"原生锁定"的参考格，并判断它的 Image 是否可以通用。
static void compute_lock_reference(uint32_t dialog) {
  if (!is_ptr(dialog)) return;
  uint32_t first_img = 0;
  uint32_t first_cell = 0;
  int n = 0;
  bool differ = false;
  for (uint32_t idx = 0; idx < 15; ++idx) {
    const uint32_t cell = dialog + 0x110u + idx * 0xBCu;
    uint32_t st = 0, img = 0;
    if (!safe_read_u32(cell + 0x60u, &st)) continue;
    safe_read_u32(cell + 0x5Cu, &img);
    if (!g_cell_visual_logged[idx]) {
      g_cell_visual_logged[idx] = true;
      unsigned char f70 = 0, f71 = 0, f72 = 0;
      safe_read_u8(cell + 0x70u, &f70);
      safe_read_u8(cell + 0x71u, &f71);
      safe_read_u8(cell + 0x72u, &f72);
      char s[64] = {};
      if (is_ptr(img)) read_name_ascii(img, s, sizeof(s));
      uint32_t flash = 0;
      safe_read_u32(va_of(0x00CF0EE4u), &flash);
      log("cellv idx=%u state=%u img=%08X \"%s\" dep70=%u dep71=%u sel72=%u flash=%08X",
          idx, st, img, s, (unsigned)f70, (unsigned)f71, (unsigned)f72, flash);
    }
    if (st != 1) continue;
    if (n == 0) {
      first_img = img;
      first_cell = cell;
    } else if (img != first_img) {
      differ = true;
    }
    ++n;
  }
  if (n == 0) {
    g_ref_state = 0;
    return;
  }
  g_ref_cell = first_cell;
  g_ref_image = first_img;
  if (differ) {
    if (g_ref_state != -1) {
      log("cellv: %d native locked cells have DIFFERENT images -> not safe to copy", n);
    }
    g_ref_state = -1;
  } else {
    if (g_ref_state != 1) {
      char s[64] = {};
      if (is_ptr(first_img)) read_name_ascii(first_img, s, sizeof(s));
      log("cellv: %d native locked cells share img=%08X \"%s\" -> safe to copy", n, first_img,
          s);
    }
    g_ref_state = 1;
  }
}

static uint32_t cell_button_addr(uint32_t idx) {
  if (!is_ptr(g_panel_dialog) || idx >= 15) return 0;
  return g_panel_dialog + 0x110u + idx * 0xBCu;
}

// 反向：由按钮 this 算出格子下标。不是协议格按钮就返回 15。
static uint32_t button_cell_index(uint32_t self) {
  if (!is_ptr(g_panel_dialog) || self < g_panel_dialog + 0x110u) return 15;
  const uint32_t delta = self - g_panel_dialog - 0x110u;
  if (delta % 0xBCu != 0) return 15;
  const uint32_t idx = delta / 0xBCu;
  return idx < 15 ? idx : 15;
}

static void* g_orig_btn_refresh = nullptr;
static uint32_t g_btn_refresh_calls = 0;
static int g_btn_refresh_seq = 0;
static uint32_t g_btn_refresh_self[32];

static void __cdecl btn_refresh_pre(uint32_t self) {
  __try {
    if (!is_ptr(self)) return;
    uint32_t state = 0, elem = 0;
    safe_read_u32(self + 0x60u, &state);
    safe_read_u32(self + 0x08u, &elem);
    // 同一个面板刷新里每个按钮一次，按出现顺序和 filling 顺序一致。
    const int seq = g_btn_refresh_seq;
    if (seq < 32) {
      g_btn_refresh_self[seq] = self;
      ++g_btn_refresh_seq;
    }

    // ★ 真正生效的一步
    //   用实测反推出的槽位公式定位格子：btn = dialog + 0x110 + idx*0xBC。
    //   命中被限制的格子时，在引擎执行原函数（会把 [self+0x60] 映射成
    //   "SetAvailability" 推给 Flash、画出锁图）之前，先把状态改成 1。
    //   注意这里不能提前 return —— 上面的 80 次上限只能限制日志输出，
    //   否则用户中途改限制、面板再刷新时就完全不生效了。
    uint32_t idx = button_cell_index(self);

    // ★ 锁图持久化
    //   0xA44AB0 的执行顺序是：先读 [self+0x5C] 推 "Image"，再读 [self+0x60]
    //   推 "SetAvailability"。所以在这里把 Image 换成通用锁图，
    //   引擎紧接着就会把锁图推给 Flash —— 面板每次重绘都会保持一致，
    //   不需要靠悬停（avail 钩子）才触发。
    if (idx < 15) {
      uint32_t ref = 0;
      majority_locked_image(&ref);
      uint32_t cell_state = 0, my_img = 0;
      if (safe_read_u32(self + 0x60u, &cell_state) &&
          safe_read_u32(self + 0x5Cu, &my_img) && is_ptr(ref) && my_img != ref) {
        // 只有"引擎判为锁定"的格子才换图：可用/已拥有格保持原样。
        const bool should_lock = g_cell_locked[idx] || cell_state == 1;
        if (should_lock) {
          safe_write_u32(self + 0x5Cu, ref);
          static bool img_logged[16] = {};
          if (!img_logged[idx]) {
            img_logged[idx] = true;
            log("btn IMAGE idx=%u img %08X -> %08X (state=%u)", idx, my_img, ref, cell_state);
          }
        }
      }
    }

    if (idx < 15 && g_cell_locked[idx] && state != 1) {
      safe_write_u32(self + 0x60u, 1);
      uint32_t after = 0;
      safe_read_u32(self + 0x60u, &after);
      state = after;
      log("btn LOCK idx=%u self=%08X state -> %u (padlock forced)", idx, self, after);
    }

    // 把外观字段抄成"原生锁定格"的样子：Image / dependence / selected。
    // 这是让 SWF 真正画出锁图的关键 —— 只改 SetAvailability 只会置灰。
    if (idx < 15 && g_cell_locked[idx] && g_ref_state == 1 && is_ptr(g_ref_cell) &&
        g_ref_cell != self && !g_visual_copied[idx]) {
      g_visual_copied[idx] = true;
      uint32_t ref_img = 0, my_img = 0;
      safe_read_u32(g_ref_cell + 0x5Cu, &ref_img);
      safe_read_u32(self + 0x5Cu, &my_img);
      if (my_img != ref_img) {
        safe_write_u32(self + 0x5Cu, ref_img);
        char s[64] = {};
        if (is_ptr(ref_img)) read_name_ascii(ref_img, s, sizeof(s));
        log("btn IMAGE idx=%u img %08X -> %08X \"%s\"", idx, my_img, ref_img, s);
      }
      // +0x70/+0x71 是 SetDependenceUponPriorRow 的一对开关，+0x72 是 SetSelected。
      unsigned char r70 = 0, r71 = 0, r72 = 0;
      safe_read_u8(g_ref_cell + 0x70u, &r70);
      safe_read_u8(g_ref_cell + 0x71u, &r71);
      safe_read_u8(g_ref_cell + 0x72u, &r72);
      safe_write_u8(self + 0x70u, r70);
      safe_write_u8(self + 0x71u, r71);
      safe_write_u8(self + 0x72u, r72);
      log("btn FLAGS idx=%u dep70=%u dep71=%u sel72=%u (copied from ref=%08X)", idx,
          (unsigned)r70, (unsigned)r71, (unsigned)r72, g_ref_cell);
    }

    if (g_btn_refresh_calls >= 400) return;
    ++g_btn_refresh_calls;
    uint32_t vt = 0;
    safe_read_u32(self, &vt);
    log("btnrefresh seq=%d idx=%u self=%08X vt=%08X elem=%08X state=%u locked=%d",
        seq, idx, self, vt, elem, state, idx < 15 ? (g_cell_locked[idx] ? 1 : 0) : -1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// __thiscall(this)，ret。取 ecx 转发。
static __declspec(naked) void hk_btn_refresh(void) {
  __asm {
    pushfd
    pushad
    mov ecx, [esp + 0x18]
    push ecx
    call btn_refresh_pre
    add esp, 4
    mov ecx, [esp + 0x18]
    call dword ptr [g_orig_btn_refresh]
    popad
    popfd
    ret
  }
}

static void install_protocol_btn_refresh_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0xA44AB0u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_btn_refresh),
                     reinterpret_cast<void**>(&g_orig_btn_refresh));
  if (st == MH_OK && g_orig_btn_refresh) {
    MH_EnableHook(fn);
    log("protocol btn-refresh hook armed at %p", fn);
  } else {
    log("protocol btn-refresh hook failed: %d", (int)st);
    g_orig_btn_refresh = nullptr;
  }
}
static uint32_t g_panel_calls;
static uint32_t g_panel_rows;
static bool protocol_key_banned(uint32_t key);                   // defined below
static uint32_t name_key(const char* s);                         // defined below

// 引擎自己在 isReady 里就是用 [vtable+0x1C] 取模板的（0x007489D0）。
static uint32_t sp_module_template(void* self) {
  const uint32_t s = reinterpret_cast<uint32_t>(self);
  if (!is_ptr(s)) return 0;
  uint32_t vt = 0, getter = 0, tmpl = 0;
  if (!safe_read_u32(s, &vt) || !is_ptr(vt)) return 0;
  if (!safe_read_u32(vt + 0x1Cu, &getter) || !is_ptr(getter)) return 0;
  __try {
    using Get_t = uint32_t(__fastcall*)(void*, void*);
    tmpl = reinterpret_cast<Get_t>(getter)(self, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    tmpl = 0;
  }
  return is_ptr(tmpl) ? tmpl : 0;
}

// 模板 → NameDesc（= [[tmpl+8]]）。0 = 这条模板没有名字。
static uint32_t sp_template_namedesc(uint32_t tmpl) {
  if (!is_ptr(tmpl)) return 0;
  uint32_t a = 0, nd = 0;
  if (!safe_read_u32(tmpl + 8u, &a) || !is_ptr(a)) return 0;
  if (!safe_read_u32(a, &nd) || !is_ptr(nd)) return 0;
  return nd;
}

static const char* sp_template_name(uint32_t tmpl, uint32_t* key_out) {
  const uint32_t nd = sp_template_namedesc(tmpl);
  if (!nd) return nullptr;
  if (key_out) {
    uint32_t key = 0;
    safe_read_u32(nd + 4u, &key);
    *key_out = key;
  }
  uint32_t str = 0;
  if (!safe_read_u32(nd, &str) || !is_ptr(str)) return nullptr;
  static char s_buf[80];
  if (!read_name_ascii(str, s_buf, sizeof(s_buf))) return nullptr;
  return s_buf;
}

static uint32_t g_ban_hook_hits = 0;
static uint32_t g_ban_hook_calls = 0;
static char g_ban_hook_last[64] = {};
// hasTech（协议面板锁图）钩子的计数，定义在下面，tick 里也要用。
static uint32_t g_tech_hook_hits = 0;
static uint32_t g_tech_hook_calls = 0;

// 兜底：模板名字链读不出来时，直接在模板对象里找以 SpecialPower / PlayerPower
// 开头的字符串。
static const char* row_from_template_scan(uint32_t tmpl) {
  char buf[72] = {};
  auto try_addr = [&](uint32_t addr) -> const char* {
    if (!read_name_ascii(addr, buf, sizeof(buf))) return nullptr;
    if (std::strncmp(buf, "SpecialPower", 12) != 0 && std::strncmp(buf, "PlayerPower", 11) != 0 &&
        std::strncmp(buf, "Ability", 7) != 0) {
      return nullptr;
    }
    return row_id_for_engine_name(buf);
  };
  for (uint32_t off = 0; off <= 0x120u; off += 4u) {
    if (const char* row = try_addr(tmpl + off)) return row;
    uint32_t p = 0;
    if (!safe_read_u32(tmpl + off, &p) || !is_ptr(p)) continue;
    if (const char* row = try_addr(p)) return row;
    uint32_t p2 = 0;
    if (!safe_read_u32(p, &p2) || !is_ptr(p2)) continue;
    if (const char* row = try_addr(p2)) return row;
  }
  return nullptr;
}

// 模块 → 协议 id：只比 NameKey。
// 零售版里模板 +8 那条链拿不到字符串（[NameDesc+0] 不是 char*），但 [NameDesc+4]
// 就是货真价实的 NameKey —— 实测 24 个模块的 key 全部能在游戏的 stringhashes 表里
// 反查到 SpecialPower 模板名，所以按 key 比才是可靠的路。
struct ModuleBanCache {
  uint32_t self = 0;
  bool hit = false;
};
static ModuleBanCache g_mod_ban_cache[256];

static bool module_protocol_banned(void* self) {
  if (!protocol_ban_active() || !self) return false;
  const uint32_t s = reinterpret_cast<uint32_t>(self);
  ModuleBanCache& cached = g_mod_ban_cache[(s >> 4) & 255u];
  if (cached.self == s) return cached.hit;
  cached.self = s;
  cached.hit = false;
  const uint32_t tmpl = sp_module_template(self);
  if (tmpl) {
    uint32_t key = 0;
    sp_template_name(tmpl, &key);
    if (key && protocol_key_banned(key)) {
      cached.hit = true;
      ++g_ban_hook_hits;
      std::snprintf(g_ban_hook_last, sizeof(g_ban_hook_last), "key=%08X", key);
      // 记住这个协议在引擎里的身份，供协议面板格子匹配用。
      remember_ban_ptr(s, tmpl);
      return true;
    }
  }
  return false;
}

// 诊断：把 isReady / getReadyPercent 见到的模块打出来（每种模块一次）。
// 只在一份 banning 表非空时做，正常游戏里零开销。
static uint32_t g_ready_calls = 0;
static uint32_t g_ready_seen_n = 0;
static uint32_t g_ready_seen[24];

static void note_ready_module(void* self, const char* tag) {
  ++g_ready_calls;
  if (g_ready_seen_n >= 24) return;
  const uint32_t s = reinterpret_cast<uint32_t>(self);
  for (uint32_t i = 0; i < g_ready_seen_n; ++i) {
    if (g_ready_seen[i] == s) return;
  }
  g_ready_seen[g_ready_seen_n++] = s;
  uint32_t vt = 0;
  safe_read_u32(s, &vt);
  const uint32_t tmpl = sp_module_template(self);
  uint32_t key = 0;
  if (tmpl) sp_template_name(tmpl, &key);
  char owner_buf[32] = {};
  uint32_t cont = 0, owner = 0;
  if (safe_read_u32(s - 8u, &cont) && is_ptr(cont)) {
    if (safe_read_u32(cont + 0x418u, &owner) && is_ptr(owner)) std::snprintf(owner_buf, sizeof(owner_buf), "%08X", owner);
  }
  log("ready-module[%s] self=%08X vt=%08X tmpl=%08X key=%08X banned=%d owner=%s",
      tag, s, vt, tmpl, key, protocol_key_banned(key) ? 1 : 0,
      owner_buf[0] ? owner_buf : "-");
}

// Uprising 0x75BB90: SpecialPowerModule::isReady(). Both of its branches agree
// once [module + 0x20] is in the past, so report ready outright for the local
// player's modules while a ready toggle (0x18/0x19/0x1A -> 0x0D) is on.
typedef unsigned char(__fastcall* fn_sp_ready_bool)(void* self, void* edx);static fn_sp_ready_bool g_orig_sp_ready_bool = nullptr;
static unsigned char __fastcall hk_sp_ready_bool(void* self, void* edx) {
  __try {
    if (protocol_ban_active()) note_ready_module(self, "bool");
    if (module_protocol_banned(self)) return 0;
    if (get_flag(0x0D) != 0 && sp_module_owned_by_local(self)) {
      ++g_ready_bool_hits;
      return 1;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (g_orig_sp_ready_bool) return g_orig_sp_ready_bool(self, edx);
  return 0;
}

// Uprising 0x73BAA0: the ready ratio (0 = just fired, 1 = ready) that paints the
// command bar and feeds the list based check. This is the function the retail
// SuperPower stub patches; return 1.0f directly instead of relying on the stub.
typedef float(__fastcall* fn_sp_ready_ratio)(void* self, void* edx);
static fn_sp_ready_ratio g_orig_sp_ready_ratio = nullptr;
static float __fastcall hk_sp_ready_ratio(void* self, void* edx) {
  __try {
    if (protocol_ban_active()) note_ready_module(self, "ratio");
    if (module_protocol_banned(self)) return 0.f;
    if (get_flag(0x0D) != 0 && sp_module_owned_by_local(self)) {
      ++g_ready_ratio_hits;
      return 1.0f;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (g_orig_sp_ready_ratio) return g_orig_sp_ready_ratio(self, edx);
  return 0.f;
}

static void install_power_ready_hooks() {
  static bool tried = false;
  if (tried || !loaded_module()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
    log("power ready MH_Initialize failed: %d", (int)st);
    return;
  }
  // 零售 1.12 / 起义 1.0 的同一对函数，偏移差 0x1000 / 0x103200。
  const bool ep1 = uprising_active();
  const uint32_t ratio_va = ep1 ? 0x0073BAA0u : 0x00729EA0u;
  const uint32_t ready_va = ep1 ? 0x0075BB90u : 0x00748990u;
  void* ratio = reinterpret_cast<void*>(loaded_module() + (ratio_va - kModBase));
  st = MH_CreateHook(ratio, reinterpret_cast<void*>(&hk_sp_ready_ratio),
                     reinterpret_cast<void**>(&g_orig_sp_ready_ratio));
  if (st == MH_OK) {
    MH_EnableHook(ratio);
    log("power ready ratio hook armed at %p", ratio);
  } else {
    log("power ready ratio hook failed: %d", (int)st);
    g_orig_sp_ready_ratio = nullptr;
  }
  void* ready = reinterpret_cast<void*>(loaded_module() + (ready_va - kModBase));
  st = MH_CreateHook(ready, reinterpret_cast<void*>(&hk_sp_ready_bool),
                     reinterpret_cast<void**>(&g_orig_sp_ready_bool));
  if (st == MH_OK) {
    MH_EnableHook(ready);
    log("power ready bool hook armed at %p", ready);
  } else {
    log("power ready bool hook failed: %d", (int)st);
    g_orig_sp_ready_bool = nullptr;
  }
}

// Publish the engine's own local player into IDB (Player* at +0, player id at
// +4) so every [IDB] comparing stub sees the player of the current match.
static void sync_idb_local_player() {
  uint8_t* idb = idb_base();
  if (!idb) return;
  const uint32_t local = uprising_list_local();
  if (!local) return;
  uint32_t id = 0;
  safe_read_u32(local + 0x20, &id);
  uint32_t* box = reinterpret_cast<uint32_t*>(idb);
  if (box[0] == local && box[1] == id) return;
  static uint32_t next_log = 0;
  const uint32_t tick = GetTickCount();
  if ((int32_t)(tick - next_log) >= 0) {
    next_log = tick + 5000;
    log("idb sync %08X/%u -> %08X/%u", box[0], box[1], local, id);
  }
  box[0] = local;
  box[1] = id;
}

static void clear_local_superweapon_cooldowns() {
  if (!loaded_module()) return;
  // 协议限制的就绪钩子两个版本都要装（零售 1.12 / 起义 1.0 各一对地址）。
  install_power_ready_hooks();
  // Uprising only: keep 0x0D in sync with the toggles every frame so a state
  // that never went through the toggle path can still arm the clear.
  if (uprising_active()) {
    sync_power_flags_from_toggles();
    install_sp_probe();
    sync_idb_local_player();
  }

  if (uprising_active()) {
    static uint32_t probed = 0;
    static int probes = 0;
    static uint32_t next_probe = 0;
    uint32_t lp = 0;
    if (safe_read_u32(loaded_module() + player_list_rva(), &lp) && is_ptr(lp) &&
        safe_read_u32(lp + kPlayerListLocalOff, &lp) && is_ptr(lp)) {
      if (lp != probed) {
        probed = lp;
        probes = 0;
        next_probe = 0;
      }
      const uint32_t tick = GetTickCount();
      if (probes < 8 && (int32_t)(tick - next_probe) >= 0) {
        ++probes;
        next_probe = tick + 15000;
        probe_power_list_once(lp);
      }
    }
  }


  static uint32_t next_log = 0;
  const uint32_t now = GetTickCount();
  const bool want_log = uprising_active() && (int32_t)(now - next_log) >= 0;

  if (get_flag(0x0D) == 0) {
    if (want_log) {
      next_log = now + 3000;
      log("pwc derived flag 0x0D=0 (sw=%d proto=%d skill=%d)", (int)get_flag(0x18),
          (int)get_flag(0x19), (int)get_flag(0x1A));
    }
    return;
  }

  uint32_t list = 0;
  if (!safe_read_u32(loaded_module() + player_list_rva(), &list) || !is_ptr(list)) {
    if (want_log) {
      next_log = now + 3000;
      log("pwc no player list");
    }
    return;
  }
  uint32_t player = 0;
  if (!safe_read_u32(list + kPlayerListLocalOff, &player) || !is_ptr(player)) {
    if (want_log) {
      next_log = now + 3000;
      log("pwc no local player list=%08X", list);
    }
    return;
  }

  // The list is circular with its sentinel embedded at Player+0x218.
  const uint32_t head = player + 0x218;
  uint32_t node = 0;
  if (!safe_read_u32(head, &node)) return;
  if (want_log) log("pwc head raw=%08X self=%d", node, node == head ? 1 : 0);

  int seen = 0, cleared = 0;
  uint32_t dbg[3][3] = {};
  int dbg_n = 0;
  for (int n = 0; n < 64 && is_ptr(node) && node != head; ++n) {
    uint32_t key = 0, ready = 0, dur = 0, next = 0;
    safe_read_u32(node + 8, &key);
    safe_read_u32(node + 0x0C, &ready);
    safe_read_u32(node + 0x10, &dur);
    if (dbg_n < 3) {
      dbg[dbg_n][0] = key;
      dbg[dbg_n][1] = ready;
      dbg[dbg_n][2] = dur;
      ++dbg_n;
    }
    ++seen;
    // The entry is a cooldown when it has a duration; anything odd is left
    // alone so unrelated nodes never get corrupted.
    if (dur != 0 && dur < 0x100000u) {
      if (ready != 0) {
        safe_write_u32(node + 0x0C, 0);
        ++cleared;
      }
    }
    if (!safe_read_u32(node, &next) || next == node) break;
    node = next;
  }

  if (want_log) {
    next_log = now + 3000;
    uint32_t idb_p = 0, idb_id = 0;
    if (idb_base()) {
      idb_p = *reinterpret_cast<uint32_t*>(idb_base());
      idb_id = *reinterpret_cast<uint32_t*>(idb_base() + 4);
    }
    uint32_t p428 = 0;
    safe_read_u32(player + 0x428, &p428);
    log("pwc local=%08X head=%08X seen=%d cleared=%d idb=%08X/%08X owner428=%08X(%s) "
        "readyHits=%u/%u lastOwner=%08X n0=[k=%08X r=%08X d=%08X] n1=[k=%08X r=%08X d=%08X]",
        player, head, seen, cleared, idb_p, idb_id, p428, p428 == player ? "self" : "other",
        (unsigned)g_ready_bool_hits, (unsigned)g_ready_ratio_hits, g_last_module_owner,
        dbg[0][0], dbg[0][1], dbg[0][2], dbg[1][0], dbg[1][1], dbg[1][2]);
    if (seen == 0) {
      uint32_t p1c = 0, p20 = 0, p24 = 0;
      safe_read_u32(player + 0x1C, &p1c);
      safe_read_u32(player + 0x20, &p20);
      safe_read_u32(player + 0x24, &p24);
      int m20 = is_ptr(p20) ? sp_list_nodes(p20) : -2;
      log("pwc hint: +0x218 empty; p1C=%08X p20=%08X(%d nodes) p24=%08X p428=%08X "
          "(build/charge a superweapon for this feature to matter)",
          p1c, p20, m20, p24, p428);
    }
  }
}

static bool read_battle_clock(uint32_t* logic_out, float* seconds_out) {
  if (logic_out) *logic_out = 0;
  if (seconds_out) *seconds_out = 0.f;
  const uint32_t base = loaded_module();
  if (!base) return false;
  uint32_t logic = 0;
  if (!safe_read_u32(base + battle_logic_rva(), &logic) || !is_ptr(logic)) return false;
  uint32_t mode = 0;
  if (!safe_read_u32(logic + 0x148, &mode)) return false;
  const bool active = mode == 1 || mode == 2 || mode == 4 || mode == 6 || mode == 7 ||
                      (uprising_active() && (mode == 5 || mode == 8));
  if (!active) return false;
  uint32_t ticks = 0;
  float secs = 0.f;
  if (safe_read_u32(logic + 0x50, &ticks) && ticks < 15u * 6u * 3600u) {
    secs = (float)ticks / 15.f;
  }
  if (logic_out) *logic_out = logic;
  if (seconds_out) *seconds_out = secs;
  return true;
}

bool collect_match_economy_impl(MatchEconomy* out) {
  if (!out) return false;
  // Phase timings. The whole poll is throttled to 2s by the stats board, but if
  // it is still slow the log should say which half is responsible.
  LARGE_INTEGER perf_freq{}, perf_t0{}, perf_prev{}, perf_now{};
  QueryPerformanceFrequency(&perf_freq);
  QueryPerformanceCounter(&perf_t0);
  perf_prev = perf_t0;
  double ph_walk = 0, ph_roster = 0, ph_label = 0, ph_proto = 0;
  auto mark = [&](double* slot) {
    QueryPerformanceCounter(&perf_now);
    if (perf_freq.QuadPart > 0) {
      *slot = (double)(perf_now.QuadPart - perf_prev.QuadPart) * 1000.0 /
              (double)perf_freq.QuadPart;
    }
    perf_prev = perf_now;
  };
  *out = MatchEconomy{};
  // The faction graph walk is rate-limited globally (see fac_walk_refill_budget),
  // so even the first poll of a match cannot stall Present for most of a second.
  if (!loaded_module()) {
    std::snprintf(out->note, sizeof(out->note), u8"未找到游戏模块");
    return false;
  }

  out->spectator = is_spectator();

  uint32_t battle_logic = 0;
  float battle_seconds = 0.f;
  if (!read_battle_clock(&battle_logic, &battle_seconds)) {
    if (g_battle_logic != 0) {
      log("battle left logic=%08X clock=%.1f", g_battle_logic, g_battle_seconds);
    }
    clear_roster_caches();
    g_battle_logic = 0;
    g_battle_seconds = -1.f;
    std::snprintf(out->note, sizeof(out->note), u8"未进入对局");
    return false;
  }
  if (battle_logic != g_battle_logic ||
      (g_battle_seconds >= 0.f && battle_seconds + 2.f < g_battle_seconds)) {
    if (g_battle_logic != 0 || g_battle_seconds >= 0.f) {
      log("battle reset logic %08X -> %08X clock %.1f -> %.1f", g_battle_logic, battle_logic,
          g_battle_seconds, battle_seconds);
    }
    clear_roster_caches();
  }
  g_battle_logic = battle_logic;
  g_battle_seconds = battle_seconds;
  out->match_seconds = battle_seconds;

  uint32_t list = 0;
  if (!safe_read_u32(loaded_module() + player_list_rva(), &list) || !is_ptr(list)) {
    clear_roster_caches();
    std::snprintf(out->note, sizeof(out->note), u8"未进入对局（PlayerList 为空）");
    return false;
  }

  uint32_t local = 0;
  safe_read_u32(list + kPlayerListLocalOff, &local);
  if (!is_ptr(local)) local = local_owner_for_ops();

  // Walk every slot. The active count shrinks when someone is defeated, but
  // that player's object is still in the array and should stay on the board.
  for (int i = 0; i < (int)kPlayerListSlots && out->player_count < kMaxEconPlayers; ++i) {
    uint32_t player = 0;
    if (!safe_read_u32(list + kPlayerListArrayOff + (uint32_t)i * 4u, &player)) continue;
    if (!is_ptr(player)) continue;
    bool dup = false;
    for (int j = 0; j < out->player_count; ++j) {
      if (out->players[j].player == player) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    PlayerEconomy pe{};
    fill_player_economy_basic(&pe, player, local);
    // Player index 0 is the neutral/civilian seat (oil derricks, map props).
    // It has a money object, so it would otherwise show up as “玩家 #0”.
    if (!pe.is_local && pe.player_id == 0) continue;
    const bool seen = find_seen_seat(player) >= 0;
    if (!pe.has_money && !pe.has_power && !pe.has_score && !pe.is_local && !seen) continue;
    out->players[out->player_count++] = pe;
  }

  // Fallback: local even if values not ready yet.
  if (out->player_count == 0 && is_ptr(local)) {
    PlayerEconomy pe{};
    fill_player_economy_basic(&pe, local, local);
    if (!seat_is_dummy_local(pe)) {
      out->players[out->player_count++] = pe;
    }
  }

  mark(&ph_walk);

  // Unit tallies use retail entity offsets. Leave them off on Uprising.
  if (!uprising_active()) {
    fill_all_rosters(out);
    mark(&ph_roster);
  }

  // Drop spectators and seats that have not entered. Keep anyone already
  // shown this match, including after defeat.
  {
    int kept = 0;
    for (int i = 0; i < out->player_count; ++i) {
      PlayerEconomy& pe = out->players[i];
      const int seen_i = find_seen_seat(pe.player);
      if (uprising_active()) {
        if (seat_is_dummy_local(pe)) continue;
        if (!uprising_seat_ok(pe) && seen_i < 0) continue;
      } else {
        if (!seat_entered_battle(pe) && seen_i < 0) continue;
        mark_defeated_flag(&pe);
      }
      if (seen_i >= 0 && !seat_has_record(pe) && seat_has_record(g_seen[seen_i].last)) {
        PlayerEconomy snap = g_seen[seen_i].last;
        snap.defeated = true;
        snap.is_local = pe.is_local;
        if (pe.has_color) {
          snap.has_color = true;
          snap.color_r = pe.color_r;
          snap.color_g = pe.color_g;
          snap.color_b = pe.color_b;
        }
        snap.roster_count = 0;
        snap.unit_total = 0;
        snap.building_total = 0;
        pe = snap;
      }
      if (kept != i) out->players[kept] = pe;
      ++kept;
    }
    out->player_count = kept;
  }

  apply_seat_labels(out);
  mark(&ph_label);

  // Scrub any dummy local seat recorded before this fix / earlier in the match.
  if (uprising_active()) drop_dummy_local_seats();

  {
    bool present[kMaxEconPlayers] = {};
    for (int i = 0; i < out->player_count; ++i) {
      PlayerEconomy& pe = out->players[i];
      int seen_i = find_seen_seat(pe.player);
      const bool keep_seen =
          uprising_active() ? (uprising_seat_ok(pe) || pe.defeated)
                            : (seat_entered_battle(pe) || pe.defeated);
      if (seen_i < 0 && g_seen_n < kMaxEconPlayers && keep_seen) {
        seen_i = g_seen_n++;
        g_seen[seen_i].player = pe.player;
      }
      if (seen_i >= 0) {
        present[seen_i] = true;
        g_seen[seen_i].last = pe;
      }
    }
    for (int s = 0; s < g_seen_n && out->player_count < kMaxEconPlayers; ++s) {
      if (present[s]) continue;
      PlayerEconomy snap = g_seen[s].last;
      if (seat_is_dummy_local(snap)) continue;
      snap.defeated = true;
      snap.roster_count = 0;
      snap.unit_total = 0;
      snap.building_total = 0;
      out->players[out->player_count++] = snap;
    }
  }

  // Stable order: local first, then player_id, then pointer. Never sort by money.
  auto seat_less = [](const PlayerEconomy& a, const PlayerEconomy& b) {
    if (a.is_local != b.is_local) return a.is_local;
    if (a.player_id != b.player_id) return a.player_id < b.player_id;
    return a.player < b.player;
  };
  for (int i = 0; i < out->player_count; ++i) {
    for (int j = i + 1; j < out->player_count; ++j) {
      if (seat_less(out->players[j], out->players[i])) {
        PlayerEconomy tmp = out->players[i];
        out->players[i] = out->players[j];
        out->players[j] = tmp;
      }
    }
  }

  // Protocol key rebuild alone is ~40ms. Skip it while a full-scan seed is
  // still draining — the board only needs unit/building tallies then, and the
  // 500ms seed poll was hitching Present on proto every time.
  if (!uprising_active() && !g_roster_seed_burst) {
    for (int i = 0; i < out->player_count; ++i) fill_player_protocols(&out->players[i]);
  }
  mark(&ph_proto);

  out->valid = out->player_count > 0;
  if (!out->valid) {
    std::snprintf(out->note, sizeof(out->note), u8"还没有玩家进入战局");
  } else if (out->spectator) {
    std::snprintf(out->note, sizeof(out->note), u8"观战视角 · %d 方有统计",
                  out->player_count);
  } else {
    std::snprintf(out->note, sizeof(out->note), u8"对局资源 · %d 方有统计",
                  out->player_count);
  }
  {
    QueryPerformanceCounter(&perf_now);
    const double total = (perf_freq.QuadPart > 0)
                             ? (double)(perf_now.QuadPart - perf_t0.QuadPart) * 1000.0 /
                                   (double)perf_freq.QuadPart
                             : 0.0;
    if (total >= 25.0) {
      if (g_roster_ph_logged) {
        log("perf: economy total=%.1fms walk=%.1f roster=%.1f labels=%.1f proto=%.1f players=%d "
            "| roster rd=%.1f light=%.1f pub=%.1f",
            total, ph_walk, ph_roster, ph_label, ph_proto, out->player_count,
            g_roster_ph_rediscover, g_roster_ph_light, g_roster_ph_publish);
      } else {
        log("perf: economy total=%.1fms walk=%.1f roster=%.1f labels=%.1f proto=%.1f players=%d",
            total, ph_walk, ph_roster, ph_label, ph_proto, out->player_count);
      }
    }
  }
  return out->valid;
}

static bool resolve_money_obj(uint32_t player, uint32_t* out_obj, uint32_t* out_cur) {
  if (!is_ptr(player) || !out_obj || !out_cur) return false;
  uint32_t money_table = 0, money_obj = 0, money = 0;
  if (!safe_read_u32(player + kPlayerMoneyTableOff, &money_table) || !is_ptr(money_table)) {
    return false;
  }
  if (!safe_read_u32(money_table, &money_obj) || !is_ptr(money_obj)) return false;
  if (!safe_read_u32(money_obj + 0x04, &money)) return false;
  *out_obj = money_obj;
  *out_cur = money;
  return true;
}

static bool apply_money_delta(uint32_t player, int delta, std::string* out_msg) {
  if (!is_ptr(player)) {
    if (out_msg) *out_msg = u8"无效玩家";
    return false;
  }
  uint32_t obj = 0, cur = 0;
  if (!resolve_money_obj(player, &obj, &cur)) {
    if (out_msg) *out_msg = u8"读不到该玩家资金";
    return false;
  }
  int64_t next = (int64_t)cur + (int64_t)delta;
  if (next < 0) next = 0;
  if (next > 200000000) next = 200000000;
  if (!write_u32(obj + 0x04, (uint32_t)next)) {
    if (out_msg) *out_msg = u8"写入资金失败";
    return false;
  }
  char who[64] = {};
  read_player_display_name(player, who, sizeof(who));
  char buf[160];
  std::snprintf(buf, sizeof(buf), u8"%s资金 %u → %u",
                who[0] ? who : u8"该玩家", cur, (uint32_t)next);
  if (out_msg) *out_msg = buf;
  return true;
}

// Player production lock. Script "player cannot build from an object type"
// inserts the template into the disabled-type list at player+0x200
// (0x883AA0) and removes it again (0x87FF80). Both refresh the command bar.
constexpr uint32_t kFnBuildEnable = 0x0087FF80;
constexpr uint32_t kFnBuildDisable = 0x00883AA0;
constexpr uint32_t kThingFactoryRva = 0x008EBDE8;

struct BuildTpl {
  uint32_t ptrs[6] = {};
  int ptr_n = 0;
  char id[96] = {};
  char name[64] = {};
  bool building = false;
};

struct BuildBan {
  char id[96] = {};
  char name[64] = {};
  bool building = false;
  uint32_t tmpls[6] = {};
  int tmpl_n = 0;
  uint32_t applied[kMaxEconPlayers] = {};
  int applied_n = 0;
};

static std::vector<BuildTpl> g_build_catalog;
static std::vector<BuildBan> g_build_bans;
static DWORD g_build_lock_tick = 0;
static bool g_build_catalog_ready = false;
static uint32_t g_lock_battle_logic = 0;
static float g_lock_battle_seconds = -1.f;
static bool g_lock_seen_battle = false;
static DWORD g_lock_enter_tick = 0;
static bool g_sw_toggle_latched = false;
// 0x883AA0 refreshes the command bar. During a save restore that vector is still
// empty, the grow returns NULL, and memmove writes address 0. Same window for
// the DisableAllSP flag (0x0E), which mutates cooldown fields mid-restore.
static bool g_lock_world_ready = false;
static bool g_sw_flag_held = false;
static uint32_t g_world_logic = 0;
static float g_world_base_secs = -1.f;
static DWORD g_world_live_since = 0;
static DWORD g_world_moved_tick = 0;
static int g_lock_tick_depth = 0;
static bool g_lock_syncing = false;
static void sync_disable_superweapon_toggle();

static int live_count_peek() {
  int n = 0;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  n = (int)g_live.size();
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  return n;
}

// Units on the field mean the save has published objects. A frozen clock with
// live==0 is still restoring. An empty map falls through once the clock has
// actually been moving for a while.
static void refresh_match_world_gate() {
  uint32_t logic = 0;
  float secs = 0.f;
  const bool in_battle = read_battle_clock(&logic, &secs);
  const DWORD now = GetTickCount();
  if (!in_battle) {
    g_world_logic = 0;
    g_world_base_secs = -1.f;
    g_world_live_since = 0;
    g_world_moved_tick = 0;
    g_lock_world_ready = false;
  } else if (logic != g_world_logic) {
    g_world_logic = logic;
    g_world_base_secs = secs;
    g_world_live_since = 0;
    g_world_moved_tick = 0;
    g_lock_world_ready = false;
  } else {
    if (g_world_base_secs < 0.f) g_world_base_secs = secs;
    if (g_world_moved_tick == 0 && secs > g_world_base_secs + 0.20f) {
      g_world_moved_tick = now;
    }
    const int live = live_count_peek();
    if (live > 0) {
      if (g_world_live_since == 0) g_world_live_since = now;
      if (now - g_world_live_since >= 2000) g_lock_world_ready = true;
    } else {
      g_world_live_since = 0;
      if (g_world_moved_tick != 0 && now - g_world_moved_tick >= 12000) {
        g_lock_world_ready = true;
      }
    }
  }
  if (!g_lock_world_ready) {
    if (get_flag(0x0E) != 0) set_flag(0x0E, 0);
    if (get_flag(0x1B) != 0) g_sw_flag_held = true;
  } else {
    const bool block = get_flag(0x1B) != 0 || get_flag(0x1C) != 0;
    set_flag(0x0E, block ? 1 : 0);
    g_sw_flag_held = false;
  }
}

static bool thiscall_build_lock(uint32_t player, uint32_t tmpl, bool locked) {
  if (!g_lock_world_ready) return false;
  if (!is_ptr(player) || !is_ptr(tmpl) || !module_base()) return false;
  const uint32_t fn =
      module_base() + ((locked ? kFnBuildDisable : kFnBuildEnable) - kModBase);
  __try {
    __asm {
      mov ecx, player
      push tmpl
      mov eax, fn
      call eax
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

static bool contains_ic(const char* hay, const char* needle) {
  if (!hay || !needle || !needle[0]) return false;
  const size_t n = std::strlen(needle);
  for (const char* p = hay; *p; ++p) {
    if (_strnicmp(p, needle, n) == 0) return true;
  }
  return false;
}

static bool build_lock_skip_name(const char* name) {
  if (!name || !name[0]) return false;
  static const char* kNames[] = {
      u8"磁暴坦克", u8"磁爆坦克", u8"雷达碟", u8"斩首中队",
      u8"气球炸弹", u8"点防御无人机", u8"建造工厂", u8"建造场", u8"建筑工厂",
      u8"前哨战", u8"战斗碉堡"};
  for (const char* s : kNames) {
    if (std::strcmp(name, s) == 0) return true;
  }
  return false;
}

static bool build_lock_skip_id(const char* id) {
  if (!id || !id[0]) return true;
  if (should_hide_roster_type(id)) return true;
  if (contains_ic(id, "RadarDish") || contains_ic(id, "BalloonBomb") ||
      contains_ic(id, "FinalSquadron") || contains_ic(id, "PointDefense") ||
      contains_ic(id, "AntiVehicleVehicleTech2") || contains_ic(id, "ConYard") ||
      contains_ic(id, "ConstructionYard") || contains_ic(id, "EconomicStructure") ||
      _strnicmp(id, "AlliedOutpost", 12) == 0 || _strnicmp(id, "SovietBunker", 12) == 0) {
    return true;
  }
  static const char* kSkip[] = {
      "Projectile", "Missile", "Husk", "Debris", "Dummy", "OCL",
      "Parachute", "Revealer", "Indicator", "Wreck", "Fragment",
      "Decal", "Cinematic", "Splash", "Pilot", "Eject", "Crate",
      "Create", "Hole", "Nugget", "Locomotor", "Behavior", "Unpacked",
      "Packing", "Scaffold", "Rubble", "UnderConstruction"};
  for (const char* s : kSkip) {
    if (contains_ic(id, s)) return true;
  }
  if (const char* zh = lookup_zh_name(id)) {
    if (build_lock_skip_name(zh)) return true;
  }
  const bool faction = _strnicmp(id, "Allied", 6) == 0 ||
                       _strnicmp(id, "Soviet", 6) == 0 ||
                       _strnicmp(id, "Japan", 5) == 0;
  return !faction && !type_id_is_building(id);
}

static void build_tpl_add_ptr(BuildTpl* row, uint32_t tmpl) {
  if (!row || !is_ptr(tmpl) || row->ptr_n >= 6) return;
  for (int i = 0; i < row->ptr_n; ++i) {
    if (row->ptrs[i] == tmpl) return;
  }
  row->ptrs[row->ptr_n++] = tmpl;
}

static void copy_tpl_ptrs(BuildBan* ban, const BuildTpl& row) {
  if (!ban) return;
  for (int i = 0; i < row.ptr_n && ban->tmpl_n < 6; ++i) {
    bool dup = false;
    for (int k = 0; k < ban->tmpl_n; ++k) {
      if (ban->tmpls[k] == row.ptrs[i]) dup = true;
    }
    if (!dup) ban->tmpls[ban->tmpl_n++] = row.ptrs[i];
  }
}

static bool template_name_hint(uint32_t tmpl) {
  char tmp[72] = {};
  auto hit = [](const char* s) {
    if (!s || std::strlen(s) < 4) return false;
    static const char* kHint[] = {
        "Allied", "Soviet", "Japan", "Tech", "Yard", "Factory", "Barracks",
        "Plant", "Defense", "Wall", "Crane", "Hospital", "Garage", "Airport",
        "Ore", "Oil", "Bunker", "Super", "Nano", "Chrono", "Proton", "Iron",
        "Vacuum", "Psionic", "MCV", "Radar", "Balloon"};
    for (const char* h : kHint) {
      if (contains_ic(s, h)) return true;
    }
    return false;
  };
  for (uint32_t off = 0; off <= 0x90; off += 4) {
    if (safe_read_bytes(tmpl + off, tmp, 64)) {
      tmp[63] = 0;
      if (hit(tmp)) return true;
    }
    uint32_t p = 0;
    if (safe_read_u32(tmpl + off, &p) && is_ptr(p) && safe_read_bytes(p, tmp, 48)) {
      tmp[47] = 0;
      if (hit(tmp)) return true;
    }
  }
  return false;
}

static int build_faction_rank(const char* id) {
  if (!id) return 9;
  if (_strnicmp(id, "Allied", 6) == 0) return 0;
  if (_strnicmp(id, "Soviet", 6) == 0) return 1;
  if (_strnicmp(id, "Japan", 5) == 0) return 2;
  return 3;
}

static void rebuild_build_catalog() {
  g_build_catalog.clear();
  g_build_catalog_ready = false;
  if (!module_base()) return;
  uint32_t factory = 0;
  if (!safe_read_u32(module_base() + kThingFactoryRva, &factory) || !is_ptr(factory)) {
    return;
  }
  uint32_t tmpl = 0;
  if (!safe_read_u32(factory + 0x24, &tmpl) || !is_ptr(tmpl)) return;
  g_build_catalog_ready = true;
  for (int n = 0; n < 4096 && is_ptr(tmpl); ++n) {
    uint32_t vt = 0;
    if (!safe_read_u32(tmpl, &vt) || !is_ptr(vt)) break;
    char id[96] = {};
    if (template_name_hint(tmpl) &&
        resolve_primary_catalog_id(tmpl, id, sizeof(id)) && !build_lock_skip_id(id)) {
      int found = -1;
      for (int i = 0; i < (int)g_build_catalog.size(); ++i) {
        if (std::strcmp(g_build_catalog[i].id, id) == 0) {
          found = i;
          break;
        }
      }
      if (found < 0) {
        BuildTpl row{};
        std::snprintf(row.id, sizeof(row.id), "%s", id);
        if (const char* zh = lookup_zh_name(id)) {
          std::snprintf(row.name, sizeof(row.name), "%s", zh);
        } else {
          std::snprintf(row.name, sizeof(row.name), "%s", id);
        }
        row.building = type_id_is_building(id);
        build_tpl_add_ptr(&row, tmpl);
        g_build_catalog.push_back(row);
      } else {
        build_tpl_add_ptr(&g_build_catalog[found], tmpl);
      }
    }
    uint32_t next = 0;
    if (!safe_read_u32(tmpl + 0xC, &next) || next == tmpl) break;
    tmpl = next;
  }
  std::sort(g_build_catalog.begin(), g_build_catalog.end(),
            [](const BuildTpl& a, const BuildTpl& b) {
              const int fa = build_faction_rank(a.id);
              const int fb = build_faction_rank(b.id);
              if (fa != fb) return fa < fb;
              if (a.building != b.building) return !a.building;
              return std::strcmp(a.name, b.name) < 0;
            });
  for (BuildBan& ban : g_build_bans) {
    ban.tmpl_n = 0;
    for (const BuildTpl& row : g_build_catalog) {
      if (std::strcmp(row.id, ban.id) != 0) continue;
      copy_tpl_ptrs(&ban, row);
      if (!ban.name[0]) std::snprintf(ban.name, sizeof(ban.name), "%s", row.name);
      ban.building = row.building;
    }
  }
}

static void ensure_build_catalog() {
  if (!g_build_catalog_ready) rebuild_build_catalog();
}

static void collect_lock_players(uint32_t* out, int* n) {
  *n = 0;
  if (!read_battle_clock(nullptr, nullptr)) return;
  uint32_t list = 0;
  if (!safe_read_u32(module_base() + kLocalPlayerRva, &list) || !is_ptr(list)) return;
  for (int i = 0; i < (int)kPlayerListSlots && *n < kMaxEconPlayers; ++i) {
    uint32_t player = 0;
    if (!safe_read_u32(list + kPlayerListArrayOff + (uint32_t)i * 4u, &player) ||
        !is_ptr(player)) {
      continue;
    }
    uint32_t pid = 0;
    safe_read_u32(player + 0x20, &pid);
    if (pid == 0) continue;
    bool dup = false;
    for (int k = 0; k < *n; ++k) {
      if (out[k] == player) dup = true;
    }
    if (!dup) out[(*n)++] = player;
  }
}

static BuildBan* find_build_ban(const char* id) {
  if (!id || !id[0]) return nullptr;
  for (BuildBan& ban : g_build_bans) {
    if (std::strcmp(ban.id, id) == 0) return &ban;
  }
  return nullptr;
}

// Disable list is an EASTL sentinel embedded at Player+0x200.
// Node is 12 bytes: +0 next, +4 prev, +8 key. Key = [[[template+0x14]]+4].
// 0x883AA0 always inserts, so a second call would duplicate the node.
static bool lock_list_ready(uint32_t player) {
  const uint32_t head = player + 0x200;
  uint32_t nxt = 0, prv = 0;
  if (!safe_read_u32(head, &nxt) || !safe_read_u32(head + 4, &prv)) return false;
  return is_ptr(nxt) && is_ptr(prv);
}

static bool template_ban_key(uint32_t tmpl, uint32_t* key_out) {
  uint32_t a = 0, b = 0, key = 0;
  if (!safe_read_u32(tmpl + 0x14, &a) || !is_ptr(a)) return false;
  if (!safe_read_u32(a, &b) || !is_ptr(b)) return false;
  if (!safe_read_u32(b + 4, &key) || key == 0) return false;
  if (key_out) *key_out = key;
  return true;
}

static bool player_has_ban_key(uint32_t player, uint32_t key) {
  const uint32_t head = player + 0x200;
  uint32_t node = 0;
  if (!safe_read_u32(head, &node)) return false;
  for (int n = 0; n < 512 && is_ptr(node) && node != head; ++n) {
    uint32_t k = 0;
    if (safe_read_u32(node + 8, &k) && k == key) return true;
    uint32_t next = 0;
    if (!safe_read_u32(node, &next) || next == node) break;
    node = next;
  }
  return false;
}

static void apply_ban_to_players(BuildBan* ban, const uint32_t* players, int pn) {
  if (!ban || ban->tmpl_n <= 0 || pn <= 0) return;
  for (int p = 0; p < pn; ++p) {
    if (!lock_list_ready(players[p])) continue;
    for (int t = 0; t < ban->tmpl_n; ++t) {
      uint32_t key = 0;
      if (!template_ban_key(ban->tmpls[t], &key)) continue;
      if (player_has_ban_key(players[p], key)) continue;
      thiscall_build_lock(players[p], ban->tmpls[t], true);
    }
  }
}

static void forget_lock_targets() {
  g_build_catalog_ready = false;
  for (BuildBan& ban : g_build_bans) {
    ban.applied_n = 0;
    ban.tmpl_n = 0;
  }
  // Do NOT clear g_tpl_type_cache here — build_lock_tick used to wipe it every
  // 400ms during match open, forcing mass TypeId re-walks and hitching Present.
}

static void build_lock_tick_impl() {
  refresh_match_world_gate();
  if (g_lock_tick_depth) return;
  const DWORD now = GetTickCount();
  if (g_build_lock_tick != 0 && now - g_build_lock_tick < 400) return;
  g_build_lock_tick = now;
  ++g_lock_tick_depth;
  uint32_t battle_logic = 0;
  float battle_seconds = 0.f;
  const bool in_battle = read_battle_clock(&battle_logic, &battle_seconds);
  if (!in_battle) {
    if (g_lock_seen_battle || g_lock_battle_logic != 0 || g_lock_battle_seconds >= 0.f) {
      for (BuildBan& ban : g_build_bans) {
        ban.applied_n = 0;
        ban.tmpl_n = 0;
      }
      g_build_catalog_ready = false;
    }
    g_lock_battle_logic = 0;
    g_lock_battle_seconds = -1.f;
    g_lock_seen_battle = false;
    g_lock_enter_tick = 0;
  } else {
    const bool restarted = battle_logic != g_lock_battle_logic ||
                           (g_lock_battle_seconds >= 0.f &&
                            battle_seconds + 2.f < g_lock_battle_seconds);
    const bool entered = !g_lock_seen_battle;
    const bool new_match =
        restarted && (g_lock_battle_logic != 0 || g_lock_battle_seconds >= 0.f);
    // The shell has no previous logic pointer, so a pre-match ban used to keep
    // stale templates. Rebind for the first few seconds as well: the match
    // start scripts clear Player+0x200 after the mode flag flips.
    if (entered || new_match) g_lock_enter_tick = now;
    const bool opening = g_lock_enter_tick != 0 && now - g_lock_enter_tick < 8000;
    if (entered || new_match || opening) forget_lock_targets();
    g_lock_seen_battle = true;
    g_lock_battle_logic = battle_logic;
    g_lock_battle_seconds = battle_seconds;
  }
  sync_disable_superweapon_toggle();
  const bool lock_ready = in_battle && g_lock_world_ready && g_lock_enter_tick != 0 &&
                          now - g_lock_enter_tick >= 3000;
  if (!g_build_bans.empty() && module_base() && lock_ready) {
    uint32_t players[kMaxEconPlayers] = {};
    int pn = 0;
    collect_lock_players(players, &pn);
    if (pn == 0) {
      for (BuildBan& ban : g_build_bans) ban.applied_n = 0;
    } else {
      ensure_build_catalog();
      for (BuildBan& ban : g_build_bans) {
        if (ban.tmpl_n <= 0) {
          for (const BuildTpl& row : g_build_catalog) {
            if (std::strcmp(row.id, ban.id) != 0) continue;
            copy_tpl_ptrs(&ban, row);
          }
        }
        apply_ban_to_players(&ban, players, pn);
      }
    }
  }
  --g_lock_tick_depth;
}

static bool starts_ic(const char* s, const char* prefix) {
  return s && prefix && prefix[0] && _strnicmp(s, prefix, std::strlen(prefix)) == 0;
}

struct LockFace {
  char group[96] = {};
  char name[64] = {};
  char icon_id[96] = {};
  char icon_name[64] = {};
  int pref = 0;
  bool superweapon = false;
  bool paired = false;
};

static void face_set(LockFace* o, const char* group, const char* name, const char* icon_id,
                     const char* icon_name, int pref, bool sw, bool paired) {
  if (!o) return;
  std::snprintf(o->group, sizeof(o->group), "%s", group ? group : "");
  std::snprintf(o->name, sizeof(o->name), "%s", name ? name : "");
  std::snprintf(o->icon_id, sizeof(o->icon_id), "%s", icon_id ? icon_id : "");
  std::snprintf(o->icon_name, sizeof(o->icon_name), "%s", icon_name ? icon_name : "");
  o->pref = pref;
  o->superweapon = sw;
  o->paired = paired;
}

static bool is_superweapon_lock_id(const char* id) {
  if (!id || !id[0]) return false;
  if (contains_ic(id, "Timer")) return false;
  return contains_ic(id, "Chronosphere") || contains_ic(id, "IronCurtain") ||
         contains_ic(id, "ProtonCollider") || contains_ic(id, "VacuumImploder") ||
         contains_ic(id, "PsionicDecimator") || contains_ic(id, "SuperWeapon");
}

static int dual_pref(const char* id) {
  if (contains_ic(id, "Naval")) return 2;
  if (contains_ic(id, "_Ground")) return 1;
  return 0;
}

// Build-lock presentation only. Stats keep the raw lookup name.
static void lock_face_of(const char* id, const char* fallback_name, LockFace* out) {
  if (!out) return;
  *out = LockFace{};
  const char* fb = fallback_name && fallback_name[0] ? fallback_name : (id ? id : "");
  face_set(out, id ? id : "", fb, id ? id : "", fb, 0, false, false);
  if (!id || !id[0]) return;
  const int fac = build_faction_rank(id);
  const bool egg = contains_ic(id, "Egg");

  if (is_superweapon_lock_id(id) && fac <= 2) {
    const bool adv = contains_ic(id, "Advanced") || contains_ic(id, "ProtonCollider") ||
                     contains_ic(id, "VacuumImploder") || contains_ic(id, "PsionicDecimator");
    if (fac == 0) {
      if (adv) {
        face_set(out, "sw_al_proton", u8"质子撞击炮", "AlliedProtonCollider", u8"质子撞击炮",
                 egg ? 1 : 0, true, false);
      } else {
        face_set(out, "sw_al_chrono", u8"超时空传送仪", "AlliedChronosphere", u8"超时空传送仪", 0,
                 true, false);
      }
      return;
    }
    if (fac == 1) {
      if (adv) {
        face_set(out, "sw_sv_vacuum", u8"真空内爆弹", "SovietVacuumImploder", u8"真空内爆弹",
                 egg ? 1 : 0, true, false);
      } else {
        face_set(out, "sw_sv_iron", u8"铁幕装置", "SovietIronCurtain", u8"铁幕装置", 0, true, false);
      }
      return;
    }
    if (adv) {
      face_set(out, "sw_jp_psi", u8"超能波毁灭装置", "JapanPsionicDecimator", u8"超能波毁灭装置",
               egg ? 1 : 0, true, false);
    } else {
      // The hive itself stays in the lock, but the icon is the nanocore.
      face_set(out, "sw_jp_nano", u8"纳米虫群核心", "JapanSuperWeapon", u8"纳米虫群巢穴", egg ? 0 : 1,
               true, false);
    }
    return;
  }
  if (is_superweapon_lock_id(id)) {
    face_set(out, "sw_all", u8"超级武器", "SuperWeapon", u8"超级武器", 0, true, false);
    return;
  }

  if (contains_ic(id, "MCV") && fac <= 2) {
    const char* g = fac == 0 ? "dual_al_mcv" : fac == 1 ? "dual_sv_mcv" : "dual_jp_mcv";
    face_set(out, g, "MCV", id, "MCV", dual_pref(id), false, true);
    return;
  }
  if (fac == 0 && (starts_ic(id, "AlliedMiner") || starts_ic(id, "AlliedSurveyor"))) {
    const int pref = dual_pref(id) + (starts_ic(id, "AlliedMiner") ? 1 : 0);
    face_set(out, "dual_al_prospect", u8"勘探者", "AlliedSurveyor", u8"勘探者", pref, false, true);
    return;
  }
  if (fac == 0 && starts_ic(id, "AlliedAntiInfantryVehicle")) {
    face_set(out, "dual_al_acv", u8"激流ACV", "AlliedAntiInfantryVehicle", u8"激流ACV",
             dual_pref(id), false, true);
    return;
  }
  if (fac == 1 && starts_ic(id, "SovietSurveyor")) {
    face_set(out, "dual_sv_sputnik", u8"史普尼克勘察车", "SovietSurveyor", u8"史普尼克勘察车",
             dual_pref(id), false, true);
    return;
  }
  if (fac == 1 &&
      (starts_ic(id, "SovietAntiAirShip") || starts_ic(id, "SovietAntiAirVehicleTech1"))) {
    int pref = 2;
    if (starts_ic(id, "SovietAntiAirShip") && !contains_ic(id, "Ground")) pref = 0;
    else if (contains_ic(id, "Ground")) pref = 1;
    face_set(out, "dual_sv_frog", u8"牛蛙载具", "SovietAntiAirShip", u8"牛蛙载具", pref, false,
             true);
    return;
  }
  if (fac == 2 && starts_ic(id, "JapanAntiVehicleVehicleTech1")) {
    face_set(out, "dual_jp_tsunami", u8"海啸坦克", "JapanAntiVehicleVehicleTech1", u8"海啸坦克",
             dual_pref(id), false, true);
    return;
  }
  if (fac == 2 && starts_ic(id, "JapanMiner")) {
    face_set(out, "dual_jp_miner", u8"采矿车", "JapanMiner", u8"采矿车", dual_pref(id), false, true);
    return;
  }
  if (fac == 1 && starts_ic(id, "SovietMiner")) {
    face_set(out, "dual_sv_miner", u8"苏联矿车", "SovietMiner", u8"苏联矿车", dual_pref(id), false,
             true);
    return;
  }

  if (fac != 2) return;
  struct CoreRow {
    const char* prefix;
    const char* group;
    const char* name;
    const char* icon;
  };
  static const CoreRow kCore[] = {
      {"JapanBaseDefenseAdv", "jp_tower", u8"波能塔", "JapanBaseDefenseAdv"},
      {"JapanBaseDefense", "jp_vx", u8"防卫者-VX", "JapanBaseDefense"},
      {"JapanBarracks", "jp_dojo", u8"瞬息道场", "JapanBarracks"},
      {"JapanNavalYard", "jp_dock", u8"帝国码头", "JapanNavalYard"},
      {"JapanPowerPlant", "jp_power", u8"瞬息发电厂", "JapanPowerPlant"},
      {"JapanRefinery", "jp_refinery", u8"矿石精炼厂", "JapanRefinery"},
      {"JapanWarFactory", "jp_mech", u8"机甲工厂", "JapanWarFactory"},
      {"JapanTechStructure", "jp_host", u8"纳米主机", "JapanNanotechMainframe"},
      {"JapanNanotechMainframe", "jp_host", u8"纳米主机", "JapanNanotechMainframe"},
  };
  for (const CoreRow& r : kCore) {
    if (!starts_ic(id, r.prefix)) continue;
    if (contains_ic(id, "Repair")) continue;
    face_set(out, r.group, r.name, r.icon, r.name, egg ? 1 : 0, false, false);
    return;
  }
}

// 1 = changed, 0 = already in the requested state, -1 = template missing.
static int build_lock_apply_one(const char* type_id, bool locked) {
  if (!locked) {
    BuildBan* ban = find_build_ban(type_id);
    if (!ban) return 0;
    uint32_t players[kMaxEconPlayers] = {};
    int pn = 0;
    collect_lock_players(players, &pn);
    for (int p = 0; p < pn; ++p) {
      for (int t = 0; t < ban->tmpl_n; ++t) {
        thiscall_build_lock(players[p], ban->tmpls[t], false);
      }
    }
    g_build_bans.erase(
        std::remove_if(g_build_bans.begin(), g_build_bans.end(),
                       [&](const BuildBan& b) { return std::strcmp(b.id, type_id) == 0; }),
        g_build_bans.end());
    return 1;
  }
  if (find_build_ban(type_id)) return 0;
  BuildBan ban{};
  std::snprintf(ban.id, sizeof(ban.id), "%s", type_id);
  for (const BuildTpl& row : g_build_catalog) {
    if (std::strcmp(row.id, type_id) != 0) continue;
    if (!ban.name[0]) std::snprintf(ban.name, sizeof(ban.name), "%s", row.name);
    ban.building = row.building;
    copy_tpl_ptrs(&ban, row);
  }
  if (ban.tmpl_n <= 0) return -1;
  if (!ban.name[0]) {
    if (const char* zh = lookup_zh_name(type_id)) {
      std::snprintf(ban.name, sizeof(ban.name), "%s", zh);
    } else {
      std::snprintf(ban.name, sizeof(ban.name), "%s", type_id);
    }
  }
  g_build_bans.push_back(ban);
  return 1;
}

static bool build_lock_set_impl(const char* type_id, bool locked, std::string* out_msg) {
  if (!type_id || !type_id[0]) {
    if (out_msg) *out_msg = u8"没有种类代号";
    return false;
  }
  ensure_build_catalog();
  LockFace face{};
  lock_face_of(type_id, nullptr, &face);
  const bool ban_all_sw = std::strcmp(face.group, "sw_all") == 0;
  const bool grouped = ban_all_sw || std::strcmp(face.group, type_id) != 0;
  if (grouped) {
    int changed = 0;
    int known = 0;
    for (const BuildTpl& row : g_build_catalog) {
      LockFace rf{};
      lock_face_of(row.id, row.name, &rf);
      if (ban_all_sw) {
        if (!rf.superweapon) continue;
      } else if (std::strcmp(rf.group, face.group) != 0) {
        continue;
      }
      ++known;
      const int st = build_lock_apply_one(row.id, locked);
      if (st > 0) ++changed;
    }
    if (known == 0) {
      if (out_msg) *out_msg = u8"单位表里没有这个种类，进对局后再试";
      return false;
    }
    if (changed > 0 && locked) {
      g_build_lock_tick = 0;
      build_lock_tick_impl();
    }
    if (out_msg) {
      if (ban_all_sw) {
        *out_msg = locked ? u8"已禁止超级武器：超时空传送仪、质子撞击炮、铁幕装置、真空内爆弹、纳米虫群核心、超能波毁灭装置"
                          : u8"已解禁超级武器：超时空传送仪、质子撞击炮、铁幕装置、真空内爆弹、纳米虫群核心、超能波毁灭装置";
      } else if (face.paired) {
        *out_msg = std::string(face.name) +
                   (locked ? u8" 已禁止（车厂和船厂都不能建造）"
                           : u8" 已解禁（车厂和船厂都可以建造）");
      } else if (!locked) {
        *out_msg = std::string(face.name) + u8" 已解禁";
      } else if (read_battle_clock(nullptr, nullptr)) {
        *out_msg = std::string(face.name) + u8" 已禁止建造";
      } else {
        *out_msg = std::string(face.name) + u8" 已加入禁止列表，进入对局后生效";
      }
    }
    return true;
  }
  const int st = build_lock_apply_one(type_id, locked);
  if (st < 0) {
    if (out_msg) *out_msg = u8"单位表里没有这个种类，进对局后再试";
    return false;
  }
  if (st > 0 && locked) {
    g_build_lock_tick = 0;
    build_lock_tick_impl();
  }
  if (out_msg) {
    const char* zh = lookup_zh_name(type_id);
    const char* name = zh && zh[0] ? zh : type_id;
    if (!locked) {
      *out_msg = std::string(name) + u8" 已解禁";
    } else if (read_battle_clock(nullptr, nullptr)) {
      *out_msg = std::string(name) + u8" 已禁止建造";
    } else {
      *out_msg = std::string(name) + u8" 已加入禁止列表，进入对局后生效";
    }
  }
  return true;
}

static void sync_disable_superweapon_toggle() {
  if (g_lock_syncing) return;
  g_lock_syncing = true;
  const bool on = feature_enabled("disableallsp");
  if (on) {
    if (!g_sw_toggle_latched) {
      if (build_lock_set_impl("SuperWeapon", true, nullptr)) g_sw_toggle_latched = true;
    }
  } else if (g_sw_toggle_latched) {
    build_lock_set_impl("SuperWeapon", false, nullptr);
    g_sw_toggle_latched = false;
  }
  g_lock_syncing = false;
}

static bool build_lock_selected_impl(bool locked, std::string* out_msg) {
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"请先在游戏里选中一个单位或建筑";
    return false;
  }
  uint32_t unit_data = 0;
  if (!safe_read_u32(ents[0] + 4, &unit_data) || !is_ptr(unit_data)) {
    if (out_msg) *out_msg = u8"读不到选中单位的种类";
    return false;
  }
  char id[96] = {};
  if (!resolve_primary_catalog_id(unit_data, id, sizeof(id))) {
    char sig[32] = {};
    if (!resolve_type_id_from_template(unit_data, id, sizeof(id), sig, sizeof(sig)) ||
        !id[0]) {
      if (out_msg) *out_msg = u8"这个种类还不能加入建造限制";
      return false;
    }
  }
  ensure_build_catalog();
  bool known = false;
  for (const BuildTpl& row : g_build_catalog) {
    if (std::strcmp(row.id, id) == 0) known = true;
  }
  if (!known) {
    BuildTpl row{};
    build_tpl_add_ptr(&row, unit_data);
    std::snprintf(row.id, sizeof(row.id), "%s", id);
    if (const char* zh = lookup_zh_name(id)) {
      std::snprintf(row.name, sizeof(row.name), "%s", zh);
    } else {
      std::snprintf(row.name, sizeof(row.name), "%s", id);
    }
    row.building = type_id_is_building(id);
    g_build_catalog.push_back(row);
  }
  return build_lock_set_impl(id, locked, out_msg);
}

static const BuildLockEntry* build_lock_catalog_impl(int* count) {
  ensure_build_catalog();
  static std::vector<BuildLockEntry> view;
  view.clear();
  view.reserve(g_build_catalog.size());
  for (const BuildTpl& row : g_build_catalog) {
    LockFace face{};
    lock_face_of(row.id, row.name, &face);
    BuildLockEntry e{};
    std::snprintf(e.type_id, sizeof(e.type_id), "%s", row.id);
    std::snprintf(e.name, sizeof(e.name), "%s", face.name[0] ? face.name : row.name);
    std::snprintf(e.group, sizeof(e.group), "%s", face.group);
    std::snprintf(e.icon_id, sizeof(e.icon_id), "%s", face.icon_id);
    std::snprintf(e.icon_name, sizeof(e.icon_name), "%s", face.icon_name);
    e.group_pref = face.pref;
    e.building = row.building;
    e.banned = find_build_ban(row.id) != nullptr;
    e.superweapon = face.superweapon;
    e.paired = face.paired;
    view.push_back(e);
  }
  if (count) *count = (int)view.size();
  return view.empty() ? nullptr : view.data();
}

// ---------------------------------------------------------------------------
// 协议限制（第一步：只搭界面和状态，尚未写进引擎）
// 界面平铺：一个阵营的协议从左往右直接排开，不再有分支标题。family 只用于
// 排版——同一协议的 1/2/3 级算一族，换行时整族一起走，别把分级拆到两行。
// icon_name 对应 unit_images\<阵营>\协议\<icon_name>.png|webp。
// ---------------------------------------------------------------------------
struct ProtocolRow {
  const char* id;
  const char* name;
  const char* icon_name;
  const char* family;
  int faction;
  int tier;
  bool ultimate;
};

static const ProtocolRow kProtocolRows[] = {
    // ---------------- 盟军 ----------------
    {"PlayerPower_CryoSatellite1", u8"一级冷冻射击", u8"一级冷冻射击", u8"冷冻射击", 0, 1, false},
    {"PlayerPower_CryoSatellite2", u8"二级冷冻射击", u8"二级冷冻射击", u8"冷冻射击", 0, 2, false},
    {"PlayerPower_CryoSatellite3", u8"三级冷冻射击", u8"三级冷冻射击", u8"冷冻射击", 0, 3, false},
    {"PlayerPower_Paradrop1", u8"一级伞兵空降", u8"一级伞兵空降", u8"伞兵空降", 0, 1, false},
    {"PlayerPower_Paradrop2", u8"二级伞兵空降", u8"二级伞兵空降", u8"伞兵空降", 0, 2, false},
    {"PlayerPower_Paradrop3", u8"三级伞兵空降", u8"三级伞兵空降", u8"伞兵空降", 0, 3, false},
    {"PlayerPower_AirPower", u8"先进航空学", u8"先进航空学", u8"先进航空学", 0, 0, false},
    {"PlayerPower_PrecisionStrike", u8"精准轰炸", u8"精准轰炸", u8"精准轰炸", 0, 0, false},
    {"PlayerPower_SatelliteSweep", u8"侦查扫描", u8"侦查扫描", u8"侦查扫描", 0, 0, false},
    {"PlayerPower_TimeBomb1", u8"一级超时空炸弹", u8"一级超时空炸弹", u8"超时空炸弹", 0, 1, false},
    {"PlayerPower_TimeBomb2", u8"二级超时空炸弹", u8"二级超时空炸弹", u8"超时空炸弹", 0, 2, false},
    {"PlayerPower_TimeBomb3", u8"三级超时空炸弹", u8"三级超时空炸弹", u8"超时空炸弹", 0, 3, false},
    {"PlayerPower_ChronoRift1", u8"一级时空裂缝", u8"一级时空裂缝", u8"时空裂缝", 0, 1, false},
    {"PlayerPower_ChronoRift2", u8"二级时空裂缝", u8"二级时空裂缝", u8"时空裂缝", 0, 2, false},
    {"PlayerPower_ChronoRift3", u8"三级时空裂缝", u8"三级时空裂缝", u8"时空裂缝", 0, 3, false},
    {"PlayerPower_ChronoSwap", u8"超时空互换", u8"超时空互换", u8"超时空互换", 0, 0, false},
    {"PlayerPower_HighTechnology", u8"高科技", u8"高科技", u8"高科技", 0, 0, false},
    {"PlayerPower_FreeTrade", u8"自由贸易", u8"自由贸易", u8"自由贸易", 0, 0, false},
    {"PlayerPower_ParticleCannon", u8"质子撞击炮", u8"质子撞击炮协议", u8"质子撞击炮", 0, 0, true},
    {"PlayerPower_Chronosphere", u8"超时空传送仪", u8"超时空传送仪协议", u8"超时空传送仪", 0, 0, true},

    // ---------------- 苏联 ----------------
    {"PlayerPower_OrbitalRefuse1", u8"一级轨道抛掷", u8"一级轨道抛掷", u8"轨道抛掷", 1, 1, false},
    {"PlayerPower_OrbitalRefuse2", u8"二级轨道抛掷", u8"二级轨道抛掷", u8"轨道抛掷", 1, 2, false},
    {"PlayerPower_OrbitalRefuse3", u8"三级轨道抛掷", u8"三级轨道抛掷", u8"轨道抛掷", 1, 3, false},
    {"PlayerPower_MagneticSingularity", u8"磁力奇点", u8"磁力奇点", u8"磁力奇点", 1, 0, false},
    {"PlayerPower_CrushPuppies", u8"重机碾压", u8"重机碾压", u8"重机碾压", 1, 0, false},
    {"PlayerPower_MagneticSatellite1", u8"一级磁力卫星", u8"一级磁力卫星", u8"磁力卫星", 1, 1, false},
    {"PlayerPower_MagneticSatellite2", u8"二级磁力卫星", u8"二级磁力卫星", u8"磁力卫星", 1, 2, false},
    {"PlayerPower_MagneticSatellite3", u8"三级磁力卫星", u8"三级磁力卫星", u8"磁力卫星", 1, 3, false},
    {"PlayerPower_DesolatorBomb1", u8"一级毁灭空袭", u8"一级毁灭空袭", u8"毁灭空袭", 1, 1, false},
    {"PlayerPower_DesolatorBomb2", u8"二级毁灭空袭", u8"二级毁灭空袭", u8"毁灭空袭", 1, 2, false},
    {"PlayerPower_DesolatorBomb3", u8"三级毁灭空袭", u8"三级毁灭空袭", u8"毁灭空袭", 1, 3, false},
    {"PlayerPower_TerrorDroneEggs", u8"恐怖机器人奇袭", u8"恐怖机器人奇袭", u8"恐怖机器人奇袭", 1, 0, false},
    {"PlayerPower_MassProduction", u8"大生产", u8"大生产", u8"大生产", 1, 0, false},
    {"PlayerPower_IrradiateTarget", u8"毒质腐蚀", u8"毒质腐蚀", u8"毒质腐蚀", 1, 0, false},
    {"PlayerPower_ProductionKickback", u8"现金奖励", u8"现金奖励", u8"现金奖励", 1, 0, false},
    {"PlayerPower_VacuumBomb", u8"真空内爆弹", u8"真空内爆弹协议", u8"真空内爆弹", 1, 0, true},
    {"PlayerPower_IronCurtain", u8"铁幕装置", u8"铁幕协议", u8"铁幕装置", 1, 0, true},

    // ---------------- 帝国 ----------------
    {"PlayerPower_BalloonAttack1", u8"一级气球炸弹", u8"一级气球炸弹", u8"气球炸弹", 2, 1, false},
    {"PlayerPower_BalloonAttack2", u8"二级气球炸弹", u8"二级气球炸弹", u8"气球炸弹", 2, 2, false},
    {"PlayerPower_BalloonAttack3", u8"三级气球炸弹", u8"三级气球炸弹", u8"气球炸弹", 2, 3, false},
    {"PlayerPower_FinalSquadron1", u8"一级决胜中队", u8"一级决胜中队", u8"决胜中队", 2, 1, false},
    {"PlayerPower_FinalSquadron2", u8"二级决胜中队", u8"二级决胜中队", u8"决胜中队", 2, 2, false},
    {"PlayerPower_FinalSquadron3", u8"三级决胜中队", u8"三级决胜中队", u8"决胜中队", 2, 3, false},
    {"PlayerPower_AdvMissilePacks", u8"先进导弹匣", u8"先进导弹匣", u8"先进导弹匣", 2, 0, false},
    {"PlayerPower_PointDefenseDrones", u8"点防御无人机", u8"点防御无人机", u8"点防御无人机", 2, 0, false},
    {"PlayerPower_EmperorRage1", u8"一级天皇之怒", u8"一级天皇之怒", u8"天皇之怒", 2, 1, false},
    {"PlayerPower_EmperorRage2", u8"二级天皇之怒", u8"二级天皇之怒", u8"天皇之怒", 2, 2, false},
    {"PlayerPower_EmperorRage3", u8"三级天皇之怒", u8"三级天皇之怒", u8"天皇之怒", 2, 3, false},
    {"PlayerPower_EnhancedKamikaze", u8"光荣自爆", u8"光荣自爆", u8"光荣自爆", 2, 0, false},
    {"PlayerPower_Ambush", u8"惊骇伏击", u8"惊骇伏击", u8"惊骇伏击", 2, 0, false},
    {"PlayerPower_RoboticAssembly", u8"机械化组装", u8"机械化组装", u8"机械化组装", 2, 0, false},
    {"PlayerPower_NavalPower", u8"强化舰队", u8"强化舰队", u8"强化舰队", 2, 0, false},
    {"PlayerPower_Nanoswarm", u8"纳米虫群巢穴", u8"纳米虫群协议", u8"纳米虫群协议", 2, 0, true},
    {"PlayerPower_TelekineticProjectorDevice", u8"超能波毁灭装置", u8"超能波毁灭装置协议",
     u8"超能波毁灭装置协议", 2, 0, true},
};

static std::unordered_set<std::string> g_protocol_bans;

static const char* protocol_faction_key(int faction) {
  if (faction == 0) return "Allied";
  if (faction == 1) return "Soviet";
  return "Japan";
}

static bool protocol_banned(const char* id) {
  return id && id[0] && g_protocol_bans.find(id) != g_protocol_bans.end();
}

// 给就绪钩子用的快速判断：一条禁止都没有时不做任何名字解析。
static bool protocol_ban_active() { return !g_protocol_bans.empty(); }

static const ProtocolLockEntry* protocol_lock_catalog_impl(int* count) {
  static std::vector<ProtocolLockEntry> view;
  view.clear();
  view.reserve(sizeof(kProtocolRows) / sizeof(kProtocolRows[0]));
  for (const ProtocolRow& row : kProtocolRows) {
    ProtocolLockEntry e{};
    std::snprintf(e.type_id, sizeof(e.type_id), "%s", row.id);
    std::snprintf(e.name, sizeof(e.name), "%s", row.name);
    std::snprintf(e.family, sizeof(e.family), "%s", row.family);
    std::snprintf(e.icon_name, sizeof(e.icon_name), "%s", row.icon_name);
    // 阵营前缀让 unit_icons 只在这家的「协议」目录里找图。
    std::snprintf(e.icon_id, sizeof(e.icon_id), "%sProtocolZ_%s",
                  protocol_faction_key(row.faction), row.id);
    e.faction = row.faction;
    e.tier = row.tier;
    e.ultimate = row.ultimate;
    e.banned = protocol_banned(row.id);
    view.push_back(e);
  }
  if (count) *count = (int)view.size();
  return view.empty() ? nullptr : view.data();
}

static bool protocol_lock_set_impl(const char* type_id, bool locked, std::string* out_msg) {
  if (!type_id || !type_id[0]) {
    if (out_msg) *out_msg = u8"缺少协议代号";
    return false;
  }
  const ProtocolRow* hit = nullptr;
  for (const ProtocolRow& row : kProtocolRows) {
    if (std::strcmp(row.id, type_id) == 0) {
      hit = &row;
      break;
    }
  }
  if (!hit) {
    if (out_msg) *out_msg = u8"这个协议不在限制表里";
    return false;
  }
  if (locked) {
    g_protocol_bans.insert(type_id);
  } else {
    g_protocol_bans.erase(type_id);
  }
  log("protocol lock %s: %s (%d banned)", type_id, locked ? "ban" : "allow",
      (int)g_protocol_bans.size());
  if (out_msg) {
    *out_msg = std::string(locked ? u8"已限制「" : u8"已放开「") + hit->name + u8"」";
  }
  return true;
}

static void protocol_lock_reset_impl() { g_protocol_bans.clear(); }

// ---------------------------------------------------------------------------
// 协议限制（引擎侧）
//
// 协议在引擎里是一条「特殊技能冷却」记录，挂在玩家自己的有序表上：
//   表头 = Player + 0x218（哨兵自身也在这，节点 +0 next / +4 prev 互指）
//   节点 = { +0 next, +4 prev, +8 名字指针, +0xC 就绪帧, +0x10 总时长(帧) }
// +8 是引擎驻留的模板名字符串指针（SpecialPower 模板名）。引擎查表时比的是
// 指针本身（0x0087CB92 的 `cmp [eax+8], edx`），所以同名一定同指针。
//
// 零售 1.12 读这张表的函数：
//   0x0087CBE0  Player::isSpecialPowerReady(tmpl)    -> bool，查不到节点就当没就绪
//   0x0087CB70  Player::specialPowerReadyRatio(tmpl) -> float 0..1，画按钮用
//   0x00883640  Player::startSpecialPowerCooldown(tmpl)
//   0x0087FF30  Player::removeSpecialPowerTimer(tmpl)
// 所以禁用一个协议不用改代码：把节点的 +0xC 顶到当前帧之后它就点不了；
// 不再顶，几秒之内自己就恢复。
//
// 旧实现往 Player+0x20C 那张 lockTech 表里插东西，那条路对协议无效：
// 整份映像里只有 0x00A6A685 一处读它（按钮刷新），没有逻辑分支；而且 +0x20C /
// +0x218 里存的是驻留字符串指针，按协议名算出来的哈希根本不在表里。
// ---------------------------------------------------------------------------

// 界面 id（同时是图标目录名）→ 引擎里的模板名。一个协议可能对应多个模板：
// 时空裂缝有 Select / Teleport 两套，超级武器还各自带一个 _Base。
struct ProtocolEngineName {
  const char* engine;
  const char* row;
};

static const ProtocolEngineName kProtocolEngineNames[] = {
    // ---------------- 盟军 ----------------
    {"SpecialPowerCryoSatelliteLvl1", "PlayerPower_CryoSatellite1"},
    {"SpecialPowerCryoSatelliteLvl2", "PlayerPower_CryoSatellite2"},
    {"SpecialPowerCryoSatelliteLvl3", "PlayerPower_CryoSatellite3"},
    {"SpecialPowerParadropLvl1", "PlayerPower_Paradrop1"},
    {"SpecialPowerParadropLvl2", "PlayerPower_Paradrop2"},
    {"SpecialPowerParadropLvl3", "PlayerPower_Paradrop3"},
    {"AbilityParadrop", "PlayerPower_Paradrop1"},
    {"SpecialPowerAirPower", "PlayerPower_AirPower"},
    {"SpecialPowerPrecisionStrike", "PlayerPower_PrecisionStrike"},
    {"SpecialPowerSatelliteSweep", "PlayerPower_SatelliteSweep"},
    {"SpecialPowerTimeBombLvl1", "PlayerPower_TimeBomb1"},
    {"SpecialPowerTimeBombLvl2", "PlayerPower_TimeBomb2"},
    {"SpecialPowerTimeBombLvl3", "PlayerPower_TimeBomb3"},
    {"SpecialPowerChronoRiftSelectLvl1", "PlayerPower_ChronoRift1"},
    {"SpecialPowerChronoRiftSelectLvl2", "PlayerPower_ChronoRift2"},
    {"SpecialPowerChronoRiftSelectLvl3", "PlayerPower_ChronoRift3"},
    {"SpecialPowerChronoRiftTeleportLvl1", "PlayerPower_ChronoRift1"},
    {"SpecialPowerChronoRiftTeleportLvl2", "PlayerPower_ChronoRift2"},
    {"SpecialPowerChronoRiftTeleportLvl3", "PlayerPower_ChronoRift3"},
    {"SpecialPowerChronoRift_Base", "PlayerPower_ChronoRift1"},
    {"SpecialPowerChronoSwapSelect", "PlayerPower_ChronoSwap"},
    {"SpecialPowerChronoSwapTeleport", "PlayerPower_ChronoSwap"},
    {"SpecialPowerHighTechnology", "PlayerPower_HighTechnology"},
    {"SpecialPowerAlliedFreeTrade", "PlayerPower_FreeTrade"},
    {"SpecialPowerParticleCannon", "PlayerPower_ParticleCannon"},
    {"SpecialPowerParticleCannonControlParticleCannon", "PlayerPower_ParticleCannon"},
    {"SpecialPowerChrono_Base", "PlayerPower_Chronosphere"},
    {"SpecialPowerChronosphereObjectSelect", "PlayerPower_Chronosphere"},
    {"SpecialPowerChronosphereObjectSpawn", "PlayerPower_Chronosphere"},

    // ---------------- 苏联 ----------------
    {"SpecialPowerOrbitalRefuse_Rank1", "PlayerPower_OrbitalRefuse1"},
    {"SpecialPowerOrbitalRefuse_Rank2", "PlayerPower_OrbitalRefuse2"},
    {"SpecialPowerOrbitalRefuse_Rank3", "PlayerPower_OrbitalRefuse3"},
    {"SpecialPowerMagneticSingularity", "PlayerPower_MagneticSingularity"},
    {"SpecialPowerCrushPuppies", "PlayerPower_CrushPuppies"},
    {"SpecialPowerMagneticSatelliteLvl1", "PlayerPower_MagneticSatellite1"},
    {"SpecialPowerMagneticSatelliteLvl2", "PlayerPower_MagneticSatellite2"},
    {"SpecialPowerMagneticSatelliteLvl3", "PlayerPower_MagneticSatellite3"},
    {"SpecialPower_DesolatorBomb_Level1", "PlayerPower_DesolatorBomb1"},
    {"SpecialPower_DesolatorBomb_Level2", "PlayerPower_DesolatorBomb2"},
    {"SpecialPower_DesolatorBomb_Level3", "PlayerPower_DesolatorBomb3"},
    {"SpecialPower_DesolatorBomb_Base", "PlayerPower_DesolatorBomb1"},
    {"SpecialPowerTerrorDroneEggs", "PlayerPower_TerrorDroneEggs"},
    {"SpecialPowerSovietMassProduction", "PlayerPower_MassProduction"},
    {"SpecialPower_IrradiateTarget", "PlayerPower_IrradiateTarget"},
    {"PlayerPowerIrradiateTargetWeapon", "PlayerPower_IrradiateTarget"},
    {"SpecialPower_ProductionKickbacks", "PlayerPower_ProductionKickback"},
    {"PlayerPowerProductionKickbacksWeapon", "PlayerPower_ProductionKickback"},
    {"SpecialPowerVacuumBomb", "PlayerPower_VacuumBomb"},
    {"SpecialPowerIronCurtain", "PlayerPower_IronCurtain"},

    // ---------------- 帝国 ----------------
    {"SpecialPowerJapanBalloonAttack_L1", "PlayerPower_BalloonAttack1"},
    {"SpecialPowerJapanBalloonAttack_L2", "PlayerPower_BalloonAttack2"},
    {"SpecialPowerJapanBalloonAttack_L3", "PlayerPower_BalloonAttack3"},
    {"SpecialPowerFinalSquadron_L1", "PlayerPower_FinalSquadron1"},
    {"SpecialPowerFinalSquadron_L2", "PlayerPower_FinalSquadron2"},
    {"SpecialPowerFinalSquadron_L3", "PlayerPower_FinalSquadron3"},
    {"SpecialPowerJapanAdvancedMissilePacks", "PlayerPower_AdvMissilePacks"},
    {"SpecialPower_PointDefenseDrones", "PlayerPower_PointDefenseDrones"},
    {"PlayerPowerPointDefenseDronesWeapon", "PlayerPower_PointDefenseDrones"},
    {"SpecialPowerJapanEmperorsResolve_L1", "PlayerPower_EmperorRage1"},
    {"SpecialPowerJapanEmperorsResolve_L2", "PlayerPower_EmperorRage2"},
    {"SpecialPowerJapanEmperorsResolve_L3", "PlayerPower_EmperorRage3"},
    {"PlayerPowerJapanEmperorsResolveWeapon_L1", "PlayerPower_EmperorRage1"},
    {"PlayerPowerJapanEmperorsResolveWeapon_L2", "PlayerPower_EmperorRage2"},
    {"PlayerPowerJapanEmperorsResolveWeapon_L3", "PlayerPower_EmperorRage3"},
    {"SpecialPowerJapanEnhancedKamikaze", "PlayerPower_EnhancedKamikaze"},
    {"SpecialPowerJapanAmbush", "PlayerPower_Ambush"},
    {"SpecialPowerJapanRoboticAssembly", "PlayerPower_RoboticAssembly"},
    {"SpecialPowerJapanNavalPower", "PlayerPower_NavalPower"},
    {"SpecialPowerNanoSwarmHive", "PlayerPower_Nanoswarm"},
    {"SpecialPowerTelekineticProjectorDevice", "PlayerPower_TelekineticProjectorDevice"},
};

// 前缀只是分类，去掉之后剩下的尾巴才是稳定标识：
//   SpecialPowerCryoSatelliteLvl1  -> cryosatellitelvl1
//   ButtonPlayerPowerCryoSatelliteLvl1 -> 同样落成 cryosatellitelvl1
// 长前缀排前面，否则 "PlayerPower" 会先把 "PlayerPower_" 吃掉。
static const char* const kProtoNamePrefixes[] = {
    "ButtonPlayerPower", "ButtonSpecialPower", "Button_", "Button",
    "PlayerPower_",      "PlayerPower",        "SpecialPower_", "SpecialPower",
    "SuperweaponTimer",  "Superweapon",        "Ability",
};

static void normalize_proto_name(const char* s, char* out, size_t cap) {
  if (!out || cap < 2) return;
  out[0] = 0;
  if (!s) return;
  for (const char* p : kProtoNamePrefixes) {
    const size_t n = std::strlen(p);
    if (_strnicmp(s, p, n) == 0) {
      s += n;
      break;
    }
  }
  while (*s == '_') ++s;
  // 先把 "lvl" / "level" / "rank" 和 '_' 全部抹掉，再做「L 后面跟数字就删 L」，
  // 这样 SpecialPowerCryoSatelliteLvl1 / PlayerPower_CryoSatellite1 /
  // FinalSquadron_L1 / OrbitalRefuse_Rank1 都能落到同一个尾巴上。
  std::string t;
  for (size_t i = 0; s[i]; ++i) {
    char c = s[i];
    if (c == '_') continue;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    t.push_back(c);
  }
  const char* drop[] = {"level", "lvl", "rank", "tier"};
  for (const char* d : drop) {
    for (size_t pos = t.find(d); pos != std::string::npos; pos = t.find(d, pos)) t.erase(pos, std::strlen(d));
  }
  std::string u;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == 'l' && i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9') continue;
    u.push_back(t[i]);
  }
  std::snprintf(out, cap, "%s", u.c_str());
}

static const char* row_id_for_engine_name(const char* s) {
  if (!s || !s[0]) return nullptr;
  for (const ProtocolEngineName& e : kProtocolEngineNames) {
    if (std::strcmp(e.engine, s) == 0) return e.row;
  }
  char n[72] = {};
  normalize_proto_name(s, n, sizeof(n));
  if (!n[0]) return nullptr;
  for (const ProtocolEngineName& e : kProtocolEngineNames) {
    char m[72] = {};
    normalize_proto_name(e.engine, m, sizeof(m));
    if (std::strcmp(n, m) == 0) return e.row;
  }
  for (const ProtocolRow& r : kProtocolRows) {
    char m[72] = {};
    normalize_proto_name(r.id, m, sizeof(m));
    if (std::strcmp(n, m) == 0) return r.id;
  }
  return nullptr;
}

// 驻留字符串的指针在一局里是稳定的，按指针缓存名字 → 界面 id，命中就是一次比较。
struct ProtoNameSlot {
  uint32_t addr = 0;
  const char* row = nullptr;
};
static ProtoNameSlot g_proto_name_cache[256];

static bool read_name_ascii(uint32_t addr, char* buf, size_t cap) {
  if (!buf || cap < 2 || !is_ptr(addr)) return false;
  if (!safe_read_bytes(addr, buf, cap - 1)) return false;
  buf[cap - 1] = 0;
  for (size_t i = 0; i < cap - 1 && buf[i]; ++i) {
    const unsigned char c = static_cast<unsigned char>(buf[i]);
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return buf[0] != 0;
}

// 名字指针 / NameKey → 界面 id。
// 驻留字符串的指针在一局里是稳定的，按指针缓存名字 → 界面 id，命中就是一次比较。
static const char* row_id_for_engine_name_ptr(uint32_t addr) {
  if (!is_ptr(addr)) return nullptr;
  ProtoNameSlot& slot = g_proto_name_cache[(addr >> 3) & 255u];
  if (slot.addr == addr) return slot.row;
  char buf[72] = {};
  slot.addr = addr;
  slot.row = read_name_ascii(addr, buf, sizeof(buf)) ? row_id_for_engine_name(buf) : nullptr;
  return slot.row;
}

// NameKey → 界面 id。键是引擎自己算的 dword，我们不去复现它的哈希，
// 而是运行中遇到「模板 / 按钮」时把 (key, id) 记下来 —— 之后冷却表里
// 只看得见 key 的地方（Player+0x218 的节点 +8）也能对上。
struct ProtoKeySlot {
      uint32_t key = 0;
  const char* row = nullptr;
};
static ProtoKeySlot g_proto_key_map[512];
static int g_proto_key_map_n = 0;

static void record_engine_key(uint32_t key, const char* row) {
  if (!key || !row) return;
  for (int i = 0; i < g_proto_key_map_n; ++i) {
    if (g_proto_key_map[i].key != key) continue;
    g_proto_key_map[i].row = row;
    return;
  }
  if (g_proto_key_map_n >= 512) g_proto_key_map_n = 0;  // 丢光重来，避免一局里越积越多
  g_proto_key_map[g_proto_key_map_n].key = key;
  g_proto_key_map[g_proto_key_map_n].row = row;
  ++g_proto_key_map_n;
}

static const char* row_for_engine_key(uint32_t key) {
  if (!key) return nullptr;
  for (int i = 0; i < g_proto_key_map_n; ++i) {
    if (g_proto_key_map[i].key == key) return g_proto_key_map[i].row;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// 协议 → 引擎 NameKey
//
// 引擎的 NameKey 哈希在 0x00418040，签名是 (str, len, seed, finalize)，调用点
// 一律 seed = strlen(str)、finalize = 1，并且逐字节做小写折叠。用游戏自带的
// WBStringHashes.big 里 60792 条 (名字, hash) 对照校验过：288/290 逐位相同
// （两个例外是多字节 UTF-8 词条，协议名全是 ASCII，不受影响）。
//
// 每个协议在引擎里对应一个 SpecialPower 模板，名字和按钮不一样，
// 例如 PlayerPower_CryoSatellite1 → SpecialPowerCryoSatelliteLvl1。
// 下面这张表是逐条从 WBStringHashes 里核出来的（59 个 key 全部存在）。
// ---------------------------------------------------------------------------
struct ProtocolPowerName {
  const char* row;
  const char* power[2];  // 第 2 项为 nullptr 表示只有一个模板
};

static const ProtocolPowerName kProtocolPowerNames[] = {
    {"PlayerPower_AdvMissilePacks", {"SpecialPowerJapanAdvancedMissilePacks", nullptr}},
    {"PlayerPower_AirPower", {"SpecialPowerAirPower", nullptr}},
    {"PlayerPower_Ambush", {"SpecialPowerJapanAmbush", nullptr}},
    {"PlayerPower_BalloonAttack1", {"SpecialPowerJapanBalloonAttack_L1", nullptr}},
    {"PlayerPower_BalloonAttack2", {"SpecialPowerJapanBalloonAttack_L2", nullptr}},
    {"PlayerPower_BalloonAttack3", {"SpecialPowerJapanBalloonAttack_L3", nullptr}},
    {"PlayerPower_ChronoRift1", {"SpecialPowerChronoRiftSelectLvl1", "SpecialPowerChronoRiftTeleportLvl1"}},
    {"PlayerPower_ChronoRift2", {"SpecialPowerChronoRiftSelectLvl2", "SpecialPowerChronoRiftTeleportLvl2"}},
    {"PlayerPower_ChronoRift3", {"SpecialPowerChronoRiftSelectLvl3", "SpecialPowerChronoRiftTeleportLvl3"}},
    {"PlayerPower_ChronoSwap", {"SpecialPowerChronoSwapSelect", "SpecialPowerChronoSwapTeleport"}},
    {"PlayerPower_Chronosphere", {"SpecialPowerChronosphereObjectSelect", "SpecialPowerChronosphereObjectSpawn"}},
    {"PlayerPower_CrushPuppies", {"SpecialPowerCrushPuppies", nullptr}},
    {"PlayerPower_CryoSatellite1", {"SpecialPowerCryoSatelliteLvl1", nullptr}},
    {"PlayerPower_CryoSatellite2", {"SpecialPowerCryoSatelliteLvl2", nullptr}},
    {"PlayerPower_CryoSatellite3", {"SpecialPowerCryoSatelliteLvl3", nullptr}},
    {"PlayerPower_DesolatorBomb1", {"SpecialPower_DesolatorBomb_Level1", nullptr}},
    {"PlayerPower_DesolatorBomb2", {"SpecialPower_DesolatorBomb_Level2", nullptr}},
    {"PlayerPower_DesolatorBomb3", {"SpecialPower_DesolatorBomb_Level3", nullptr}},
    {"PlayerPower_EmperorRage1", {"SpecialPowerJapanEmperorsResolve_L1", nullptr}},
    {"PlayerPower_EmperorRage2", {"SpecialPowerJapanEmperorsResolve_L2", nullptr}},
    {"PlayerPower_EmperorRage3", {"SpecialPowerJapanEmperorsResolve_L3", nullptr}},
    {"PlayerPower_EnhancedKamikaze", {"SpecialPowerJapanEnhancedKamikaze", nullptr}},
    {"PlayerPower_FinalSquadron1", {"SpecialPowerFinalSquadron_L1", nullptr}},
    {"PlayerPower_FinalSquadron2", {"SpecialPowerFinalSquadron_L2", nullptr}},
    {"PlayerPower_FinalSquadron3", {"SpecialPowerFinalSquadron_L3", nullptr}},
    {"PlayerPower_FreeTrade", {"SpecialPowerAlliedFreeTrade", nullptr}},
    {"PlayerPower_HighTechnology", {"SpecialPowerHighTechnology", nullptr}},
    {"PlayerPower_IronCurtain", {"SpecialPowerIronCurtain", nullptr}},
    {"PlayerPower_IrradiateTarget", {"SpecialPower_IrradiateTarget", nullptr}},
    {"PlayerPower_MagneticSatellite1", {"SpecialPowerMagneticSatelliteLvl1", nullptr}},
    {"PlayerPower_MagneticSatellite2", {"SpecialPowerMagneticSatelliteLvl2", nullptr}},
    {"PlayerPower_MagneticSatellite3", {"SpecialPowerMagneticSatelliteLvl3", nullptr}},
    {"PlayerPower_MagneticSingularity", {"SpecialPowerMagneticSingularity", nullptr}},
    {"PlayerPower_MassProduction", {"SpecialPowerSovietMassProduction", nullptr}},
    {"PlayerPower_Nanoswarm", {"SpecialPowerNanoSwarmHive", nullptr}},
    {"PlayerPower_NavalPower", {"SpecialPowerJapanNavalPower", nullptr}},
    {"PlayerPower_OrbitalRefuse1", {"SpecialPowerOrbitalRefuse_Rank1", nullptr}},
    {"PlayerPower_OrbitalRefuse2", {"SpecialPowerOrbitalRefuse_Rank2", nullptr}},
    {"PlayerPower_OrbitalRefuse3", {"SpecialPowerOrbitalRefuse_Rank3", nullptr}},
    {"PlayerPower_Paradrop1", {"SpecialPowerParadropLvl1", nullptr}},
    {"PlayerPower_Paradrop2", {"SpecialPowerParadropLvl2", nullptr}},
    {"PlayerPower_Paradrop3", {"SpecialPowerParadropLvl3", nullptr}},
    {"PlayerPower_ParticleCannon", {"SpecialPowerParticleCannon", "SpecialPowerParticleCannonControlParticleCannon"}},
    {"PlayerPower_PointDefenseDrones", {"SpecialPower_PointDefenseDrones", nullptr}},
    {"PlayerPower_PrecisionStrike", {"SpecialPowerPrecisionStrike", nullptr}},
    {"PlayerPower_ProductionKickback", {"SpecialPower_ProductionKickbacks", nullptr}},
    {"PlayerPower_RoboticAssembly", {"SpecialPowerJapanRoboticAssembly", nullptr}},
    {"PlayerPower_SatelliteSweep", {"SpecialPowerSatelliteSweep", nullptr}},
    {"PlayerPower_TelekineticProjectorDevice", {"SpecialPowerTelekineticProjectorDevice", nullptr}},
    {"PlayerPower_TerrorDroneEggs", {"SpecialPowerTerrorDroneEggs", nullptr}},
    {"PlayerPower_TimeBomb1", {"SpecialPowerTimeBombLvl1", nullptr}},
    {"PlayerPower_TimeBomb2", {"SpecialPowerTimeBombLvl2", nullptr}},
    {"PlayerPower_TimeBomb3", {"SpecialPowerTimeBombLvl3", nullptr}},
    {"PlayerPower_VacuumBomb", {"SpecialPowerVacuumBomb", nullptr}},
};

// 引擎 0x00418040 的 NameKey 哈希（逐字节小写、seed = 长度、带 finalize）。
static uint32_t name_key(const char* s) {
  if (!s || !s[0]) return 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const uint32_t len = static_cast<uint32_t>(std::strlen(s));
  auto fold = [](unsigned char c) -> uint32_t {
    return (c >= 'A' && c <= 'Z') ? static_cast<uint32_t>(c + 32u) : static_cast<uint32_t>(c);
  };
  uint32_t h = len;
  uint32_t i = 0;
  for (uint32_t words = len >> 2; words; --words) {
    const uint32_t f0 = fold(p[i]), f1 = fold(p[i + 1]);
    const uint32_t f2 = fold(p[i + 2]), f3 = fold(p[i + 3]);
    h += (f1 << 8) | f0;
    uint32_t a = (f3 << 8) | f2;
    a ^= h << 5;
    a <<= 11;
    h ^= a;
    h += h >> 11;
    i += 4;
  }
  const uint32_t rem = len & 3u;
  if (rem == 3) {
    const uint32_t f0 = fold(p[i]), f1 = fold(p[i + 1]), f2 = fold(p[i + 2]);
    h += (f1 << 8) | f0;
    uint32_t a = f2 * 4u;
    a ^= h;
    a <<= 16;
    h ^= a;
    h += h >> 11;
  } else if (rem == 2) {
    const uint32_t f0 = fold(p[i]), f1 = fold(p[i + 1]);
    h += (f1 << 8) | f0;
    h ^= h << 11;
    h += h >> 17;
  } else if (rem == 1) {
    h += fold(p[i]);
    h ^= h << 10;
    h += h >> 1;
  }
  h ^= h << 3;
  h += h >> 5;
  h ^= h << 2;
  h += h >> 15;
  h ^= h << 10;
  return h;
}

// 界面 id → 它在引擎里的 1~2 个 NameKey。
static int protocol_row_keys(const char* row, uint32_t* out, int cap) {
  if (!row || !row[0] || !out || cap <= 0) return 0;
  for (const ProtocolPowerName& e : kProtocolPowerNames) {
    if (std::strcmp(e.row, row) != 0) continue;
    int n = 0;
    for (int i = 0; i < 2; ++i) {
      if (!e.power[i]) break;
      const uint32_t k = name_key(e.power[i]);
      if (k && n < cap) out[n++] = k;
    }
    return n;
  }
  return 0;
}

// 一次性自检：拿游戏里给出的真实 NameKey 对照，确认哈希逐位一致。
static void log_name_key_selftest() {
  static bool done = false;
  if (done) return;
  done = true;
  struct Pair {
    const char* name;
    uint32_t want;
  };
  const Pair pairs[] = {
      {"SpecialPowerCryoSatelliteLvl1", 0x47324218u},
      {"SpecialPowerSatelliteSweep", 0xE6846942u},
      {"SpecialPowerTimeBombLvl3", 0x0867C30Eu},
      {"SpecialPowerMagneticSingularity", 0xFF29BFB7u},
  };
  int ok = 0;
  for (const Pair& p : pairs) {
    const uint32_t got = name_key(p.name);
    if (got == p.want) {
      ++ok;
    } else {
      log("name-key MISMATCH %s got=%08X want=%08X", p.name, got, p.want);
    }
  }
  log("name-key selftest: %d/%d (engine NameKey for protocol powers)", ok,
      static_cast<int>(sizeof(pairs) / sizeof(pairs[0])));
}

// 当前禁止集合对应的 NameKey 集合，只在禁止表内容变化时重算。
static uint32_t g_ban_keys[128];
static int g_ban_keys_n = 0;
static char g_ban_keys_sig[512] = {};

static void rebuild_ban_keys() {
  char sig[512] = {};
  size_t used = 0;
  for (const std::string& id : g_protocol_bans) {
    if (used + id.size() + 2 >= sizeof(sig)) break;
    std::memcpy(sig + used, id.c_str(), id.size());
    used += id.size();
    sig[used++] = '\x1f';
  }
  if (std::strcmp(sig, g_ban_keys_sig) == 0) return;
  std::snprintf(g_ban_keys_sig, sizeof(g_ban_keys_sig), "%s", sig);
  g_ban_keys_n = 0;
  for (const std::string& id : g_protocol_bans) {
    uint32_t keys[2] = {};
    const int n = protocol_row_keys(id.c_str(), keys, 2);
    if (n == 0) {
      log("protocol key: no engine template for %s", id.c_str());
      continue;
    }
    for (int i = 0; i < n && g_ban_keys_n < 128; ++i) g_ban_keys[g_ban_keys_n++] = keys[i];
  }
}

static bool protocol_key_banned(uint32_t key) {
  if (!key) return false;
  // 热路径：本函数会被扫描循环调用上万次，绝不能每次都 rebuild_ban_keys()
  // （它要构造 512 字节签名 + 每项字符串拷贝，之前正是卡顿主因）。
  // 改为：直接查现成的表；只在表为空时补建一次。
  if (g_ban_keys_n == 0 && !g_protocol_bans.empty()) rebuild_ban_keys();
  for (int i = 0; i < g_ban_keys_n; ++i) {
    if (g_ban_keys[i] == key) return true;
  }
  return false;
}

constexpr uint32_t kFnPlayerLockTech = 0x00883B10;
constexpr uint32_t kFnPlayerUnlockTech = 0x0087FFC0;

struct TechLockedKey {
  uint32_t key = 0;
  char id[96] = {};
};
static TechLockedKey g_tech_locked[128];
static int g_tech_locked_n = 0;

static void apply_protocol_tech_locks() {
  if (!module_base() || uprising_active()) {
    log("ptl: skip module_base=%08X uprising=%d", module_base(), uprising_active() ? 1 : 0);
    return;
  }
  log_name_key_selftest();
  uint32_t list = 0;
  if (!safe_read_u32(loaded_module() + player_list_rva(), &list) || !is_ptr(list)) {
    log("ptl: skip no player list (list=%08X)", list);
    return;
  }
  uint32_t seats[kPlayerListSlots] = {};
  int n = 0;
  for (int i = 0; i < static_cast<int>(kPlayerListSlots); ++i) {
    uint32_t p = 0;
    if (!safe_read_u32(list + kPlayerListArrayOff + static_cast<uint32_t>(i) * 4u, &p)) continue;
    if (is_ptr(p)) seats[n++] = p;
  }
  if (!n) {
    log("ptl: skip no seats (list=%08X)", list);
    return;
  }
  using FnTech = void(__thiscall*)(uint32_t player, const uint32_t* key_ptr);
  const uint32_t fn_lock = module_base() + (kFnPlayerLockTech - kModBase);
  const uint32_t fn_unlock = module_base() + (kFnPlayerUnlockTech - kModBase);

  rebuild_ban_keys();

  // 用引擎自己的 Player::hasTech 验证锁定是否真的生效。
  using FnHasTech = unsigned char(__thiscall*)(uint32_t player, const uint32_t* key_ptr);
  const uint32_t fn_has_tech = module_base() + (0x87CE90u - kModBase);
  uint32_t local_player = 0;
  safe_read_u32(list + kPlayerListLocalOff, &local_player);

  // 上锁：每个被禁协议的每个 NameKey 写进所有座位。
  for (const std::string& id : g_protocol_bans) {
    uint32_t keys[2] = {};
    const int kn = protocol_row_keys(id.c_str(), keys, 2);
    if (kn == 0) {
      log("ptl: %s has no engine key", id.c_str());
      continue;
    }
    for (int ki = 0; ki < kn; ++ki) {
      const uint32_t key = keys[ki];
      bool known = false;
      for (int i = 0; i < g_tech_locked_n; ++i) {
        if (g_tech_locked[i].key == key) {
          known = true;
          break;
        }
      }
      // 修改前/后的 hasTech，用来判断引擎锁定到底有没有落地。
      int before = -1, after = -1;
      if (is_ptr(local_player)) {
        __try {
          before = reinterpret_cast<FnHasTech>(fn_has_tech)(local_player, &key) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          before = -2;
        }
      }
      __try {
        for (int s = 0; s < n; ++s) reinterpret_cast<FnTech>(fn_lock)(seats[s], &key);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      if (is_ptr(local_player)) {
        __try {
          after = reinterpret_cast<FnHasTech>(fn_has_tech)(local_player, &key) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          after = -2;
        }
      }
      if (!known || before != after) {
        log("ptl: lock %s key=%08X seats=%d hasTech %d->%d", id.c_str(), key, n, before, after);
      }
      if (!known && g_tech_locked_n < 128) {
        TechLockedKey& row = g_tech_locked[g_tech_locked_n++];
        row.key = key;
        std::snprintf(row.id, sizeof(row.id), "%s", id.c_str());
      }
    }
  }

  // 解锁：key 已经不在禁止集合里的，从所有座位删掉。
  for (int i = 0; i < g_tech_locked_n;) {
    if (protocol_key_banned(g_tech_locked[i].key)) {
      ++i;
      continue;
    }
    const uint32_t key = g_tech_locked[i].key;
    __try {
      for (int s = 0; s < n; ++s) reinterpret_cast<FnTech>(fn_unlock)(seats[s], &key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    log("protocol tech-lock: engine unlock %s key=%08X", g_tech_locked[i].id, key);
    g_tech_locked[i] = g_tech_locked[--g_tech_locked_n];
  }
}

// 每局一次：把本地玩家对象里跟「协议 / 科技」有关的区域原样打出来，
// 这样不用再猜偏移 —— 名单对不上时，直接看真实的表头、节点和 key。
static uint32_t g_dumped_player = 0;

static void dump_player_protocol_tables(uint32_t player) {
  if (!is_ptr(player) || player == g_dumped_player) return;
  g_dumped_player = player;
  uint32_t list = 0, cnt = 0;
  safe_read_u32(loaded_module() + player_list_rva(), &list);
  if (is_ptr(list)) safe_read_u32(list + 0x2Cu, &cnt);
  log("pdp player=%08X plist=%08X count=%u", player, list, cnt);
  for (uint32_t base = 0x1C0; base <= 0x2A0; base += 0x10) {
    uint32_t v[4] = {};
    for (int i = 0; i < 4; ++i) safe_read_u32(player + base + (uint32_t)i * 4u, &v[i]);
    if (!v[0] && !v[1] && !v[2] && !v[3]) continue;
    log("pdp pb %03X: %08X %08X %08X %08X", base, v[0], v[1], v[2], v[3]);
  }
  const uint32_t heads[] = {0x20C, 0x218, 0x248, 0x1238, 0x1320};
  for (uint32_t hi = 0; hi < 5; ++hi) {
    const uint32_t head = player + heads[hi];
    uint32_t node = 0;
    if (!safe_read_u32(head, &node)) continue;
    log("pdp ph %03X: node=%08X self=%d", heads[hi], node, node == head ? 1 : 0);
    for (int k = 0; k < 6 && is_ptr(node) && node != head; ++k) {
      uint32_t w[6] = {};
      for (int j = 0; j < 6; ++j) safe_read_u32(node + (uint32_t)j * 4u, &w[j]);
      log("pdp pn %03X[%d] %08X: %08X %08X %08X %08X %08X %08X", heads[hi], k, node, w[0],
          w[1], w[2], w[3], w[4], w[5]);
      uint32_t next = 0;
      if (!safe_read_u32(node, &next) || next == node) break;
      node = next;
    }
  }
}

// 每 400ms 把被禁协议在冷却表里的就绪帧顶到当前帧之后。
// 只有表里真有这个协议时才写得到；没有节点的协议本来就用不了。
// 解禁不用做任何回滚：不再续期，几秒后自己就恢复。
//
// 判定不依赖 Player 的任何其它字段：只用 ThePlayerList 的座位数组和每个座位
// 自己的 +0x218 表头（0x0087CBE0 里就是这么走的），所以不会有偏移猜错的问题。
static void protocol_ban_tick() {
  if (!loaded_module()) return;
  const DWORD now = GetTickCount();
  static DWORD next_apply = 0;
  if (next_apply != 0 && (int32_t)(now - next_apply) < 0) return;
  next_apply = now + 400;

  static DWORD next_log = 0;
  const bool want_log = (int32_t)(now - next_log) >= 0;

  // 钩子一律尽早装好：面板的行数据只在首次打开时构建，装晚了就永远看不到。
  install_protocol_tech_hook();
  install_protocol_store_hook();
  install_protocol_panel_hook();
  install_protocol_grid_hook();
  install_protocol_avail_hook();
  install_protocol_lock_hook();
  install_protocol_btn_refresh_hook();
  install_protocol_invoke_hook();

  if (!protocol_ban_active()) {
    if (want_log) {
      next_log = now + 5000;
      log("protocol ban idle: 0 banned, hook hits=%u last=%s panelCalls=%u panelRows=%u", (unsigned)g_ban_hook_hits,
          g_ban_hook_last[0] ? g_ban_hook_last : "-", (unsigned)g_panel_calls, (unsigned)g_panel_rows);
    }
    return;
  }
  if (want_log) next_log = now + 4000;

  uint32_t list = 0;
  if (!safe_read_u32(loaded_module() + player_list_rva(), &list) || !is_ptr(list)) {
    if (want_log) log("protocol ban: no player list");
    return;
  }
  uint32_t logic = 0, frame = 0;
  if (!safe_read_u32(loaded_module() + battle_logic_rva(), &logic) || !is_ptr(logic)) {
    if (want_log) log("protocol ban: no GameLogic (list=%08X)", list);
    return;
  }
  if (!safe_read_u32(logic + 0x50, &frame) || frame == 0) {
    if (want_log) log("protocol ban: GameLogic=%08X frame=0 (not in battle yet)", logic);
    return;
  }

  const uint32_t hold = 15u * 4u;  // 4 秒，够覆盖 400ms 的刷新间隔
  // 先把已知 key 的禁止项写成引擎自己的科技锁定（协议面板出锁图）。
  apply_protocol_tech_locks();

  // 禁止集合变化时，重算哪些格子该锁：清掉旧记录，等面板下次构建时重新识别。
  {
    static size_t last_n = (size_t)-1;
    static uint32_t last_hash = 0;
    uint32_t h = 2166136261u;
    for (const std::string& s : g_protocol_bans) {
      for (char c : s) h = (h ^ (unsigned char)c) * 16777619u;
      h = (h ^ 0xFFu) * 16777619u;
    }
    if (last_n != g_protocol_bans.size() || last_hash != h) {
      last_n = g_protocol_bans.size();
      last_hash = h;
      protocol_panel_clear_locks();
      log("protocol ban set changed: %u items, panel lock map reset", (unsigned)last_n);
    }
  }
  // 面板正开着的时候也立刻把状态推给 Flash，不用关掉重开。
  protocol_panel_live_lock();
  uint32_t local0 = 0;
  safe_read_u32(list + kPlayerListLocalOff, &local0);
  dump_player_protocol_tables(local0);
  int held = 0, seats = 0, nodes = 0, matched = 0;
  uint32_t first_player = 0, local = 0;
    safe_read_u32(list + kPlayerListLocalOff, &local);
  std::string watch;

  for (int i = 0; i < static_cast<int>(kPlayerListSlots); ++i) {
    uint32_t p = 0;
    if (!safe_read_u32(list + kPlayerListArrayOff + static_cast<uint32_t>(i) * 4u, &p) ||
        !is_ptr(p)) {
      continue;
    }
    uint32_t pid = 0;
    if (!safe_read_u32(p + 0x20, &pid) || pid == 0) continue;  // 空座位
    ++seats;
    const uint32_t head = p + 0x218;
    uint32_t node = 0;
    if (!safe_read_u32(head, &node) || !is_ptr(node)) continue;
    for (int n = 0; n < 96 && is_ptr(node) && node != head; ++n) {
      // 节点 +8 是 NameKey（引擎在 0x00883664 就是 `cmp [node+8], [[[tmpl+8]]+4]`）。
      uint32_t key = 0, ready = 0;
      if (!safe_read_u32(node + 8u, &key) || key == 0) break;
      ++nodes;
      const char* row = row_for_engine_key(key);
      if (protocol_key_banned(key)) {
        ++matched;
        if (!first_player) first_player = p;
        if (want_log) {
          char tmp[64];
          std::snprintf(tmp, sizeof(tmp), " %08X:%08X(%s)", p, key, row ? row : "ban");
          watch += tmp;
        }
        safe_read_u32(node + 0x0Cu, &ready);
        if (static_cast<int32_t>(ready - frame) <= static_cast<int32_t>(hold)) {
          if (safe_write_u32(node + 0x0Cu, frame + hold)) ++held;
        }
      }
      uint32_t next_node = 0;
      if (!safe_read_u32(node, &next_node) || next_node == node) break;
      node = next_node;
    }
  }

  if (want_log) {
    log("protocol ban tick: seats=%d nodes=%d keys=%d matched=%d held=%d frame=%u local=%08X "
        "first=%08X readyHits=%u readyCalls=%u techHits=%u techCalls=%u fillCalls=%u "
        "fillHits=%u%s%s",
        seats, nodes, g_ban_keys_n, matched, held, frame, local, first_player,
        (unsigned)g_ban_hook_hits, (unsigned)g_ready_calls, (unsigned)g_tech_hook_hits,
        (unsigned)g_tech_hook_calls, (unsigned)g_fill_calls, (unsigned)g_fill_hits,
        g_fill_log_last[0] ? " | " : "", g_fill_log_last);
    if (g_fill_seen[0] && g_fill_seen[0] != g_fill_seen_logged[0]) {
      std::snprintf(g_fill_seen_logged, sizeof(g_fill_seen_logged), "%s", g_fill_seen);
      // 一次性可能几百行，按块打，避免单行过长被截断。
      const size_t total = std::strlen(g_fill_seen);
      for (size_t off = 0; off < total; off += 700) {
        char chunk[720];
        size_t take = total - off;
        if (take > 700) take = 700;
        std::memcpy(chunk, g_fill_seen + off, take);
        chunk[take] = 0;
        log("store rows[%u]: %s", (unsigned)off, chunk);
      }
    }
  }
}
// ---------------------------------------------------------------------------
// 协议面板锁图：Player::hasTech
//
// 0x0087CE90 = bool Player::hasTech(const uint32_t* key)   （ecx=Player, ret 4）
//   `mov eax,[ecx+0x20C] ... cmp [eax+8], *key`  → 遍历「玩家+0x20C」禁用表，
//   表里找不到才算「有这项科技」。整份映像里只有一个调用者 0x00A6A685，
//   就在协议按钮的刷新函数里：
//       00A6A672  mov edx,[ebp+0xC]      ; ebp = 科技对象（[按钮+0x48]+0x14）
//       00A6A675  mov ecx,[edx]          ; NameDesc
//       00A6A677  mov edx,[ecx+4]        ; key = [NameDesc+4]
//       00A6A685  call hasTech
//       00A6A68C  sete bl                ; bl = 被锁
//       00A6A69A  call 0xA69800          ; 把按钮设成"锁住"的样子
//   所以返回 0 就等于给那个协议挂上锁图 —— 和建造限制一个观感。
//
// 名字直接从调用者现场取（只有这一个调用点，ebp 恒定是科技对象）：
//   tech = [ebp+0xC]，NameDesc = [tech]，name = [NameDesc+0]，key = [NameDesc+4]
// 这样不用复现引擎的 NameKey 哈希，也不会因为偏移猜错而静默失效。
//
// 函数是 ret 4（被调用方清栈），C 里没法写成 __fastcall 探针，用 naked 转发。
// ---------------------------------------------------------------------------
typedef unsigned char(__fastcall* fn_has_tech)(void* self, void* edx, const uint32_t* key);
static fn_has_tech g_orig_has_tech = nullptr;
static char g_tech_hook_last[64] = {};
static uint32_t g_tech_hook_last_key = 0;

static unsigned char __cdecl has_tech_body(const uint32_t* key, void* self, void* edx,
                                          uint32_t caller_ebp, uint32_t caller_edi,
                                          uint32_t return_addr);

// 被禁协议对应的科技对象（由 hasTech 拦截时从引擎现场抄下来）。
static uint32_t g_banned_tech[32];
static int g_banned_tech_n = 0;

// 最近一次 avail 查询发生在哪一格、什么时候。
// 面板刷新的顺序是"查完这一格的可用性，紧接着查这一格的科技"，
// 所以 hasTech 命中禁止项时，紧邻的那个格子就是它所属的协议 ——
// 用调用邻接做身份绑定，不需要任何对象偏移。
static int g_last_avail_idx = -1;
static DWORD g_last_avail_tick = 0;
static int g_adj_logged = 0;

static void apply_cell_visual_lock(uint32_t idx);

// ---------------------------------------------------------------------------
// 协议身份反查（确定性，不做递归、开销有界）
//
// 我们手里有被限制协议的 NameKey（例如超级磁力卫星 = 0B115EEE），
// 但不知道它落在第几格。格子对象里不一定直接存这个 key，可能藏在一层
// 指针后面（NameDesc 之类）。所以：
//   在 tech 自身 0..0x100 里、以及它引用的每个子对象 0..0x20 里，
//   逐个 dword 与"被禁 key 集合"比对。
// 这是一次面板构建只跑 15 次的有界扫描（每格约百余次读），
// 不会像之前的递归那样拖垮帧率。
// ---------------------------------------------------------------------------
static bool scan_dwords_for_ban_key(uint32_t base, uint32_t span, uint32_t* key_out) {
  for (uint32_t off = 0; off < span; off += 4) {
    uint32_t v = 0;
    if (!safe_read_u32(base + off, &v)) continue;
    if (protocol_key_banned(v)) {
      if (key_out) *key_out = v;
      return true;
    }
  }
  return false;
}

static bool find_ban_key_in_tech(uint32_t tech, uint32_t* key_out, uint32_t* where_out) {
  if (!is_ptr(tech)) return false;
  // 1) 自身
  if (scan_dwords_for_ban_key(tech, 0x100, key_out)) {
    if (where_out) *where_out = 0;
    return true;
  }
  // 2) 一层子对象
  for (uint32_t off = 0; off < 0x100; off += 4) {
    uint32_t p = 0;
    if (!safe_read_u32(tech + off, &p) || !is_ptr(p)) continue;
    if (scan_dwords_for_ban_key(p, 0x20, key_out)) {
      if (where_out) *where_out = off + 1;
      return true;
    }
  }
  return false;
}

static void note_banned_tech_from_row(uint32_t row) {
  // 参数名沿用 row，但实际传入的是调用者现场的 ebp。
  // 依据 0xA6A610 的反汇编：
  //     00A6A65F  mov ebp, [eax+0x14]   ; ebp = 科技对象
  //     00A6A672  mov edx, [ebp+0xC]    ; 用 ebp 取 NameDesc
  //     00A6A6BA  pop ebp               ; 到这里才恢复
  // 因此在 has_tech 调用点上，ebp 恒等于当前行对应的科技对象。
  const uint32_t tech = row;
  if (!is_ptr(tech)) return;
  for (int i = 0; i < g_banned_tech_n; ++i) {
    if (g_banned_tech[i] == tech) return;
  }
  if (g_banned_tech_n >= 32) return;
  g_banned_tech[g_banned_tech_n++] = tech;
  log("banned tech recorded tech=%08X (n=%d)", tech, g_banned_tech_n);
}

static bool is_banned_tech(uint32_t tech) {
  for (int i = 0; i < g_banned_tech_n; ++i) {
    if (g_banned_tech[i] == tech) return true;
  }
  return false;
}

static __declspec(naked) void hk_has_tech(void) {
  __asm {
    push edi
    push ebp
    push edx
    push ecx
    // 以 esp4 为基（此时 +0=ecx/self, +4=edx, +8=ebp, +12=edi, +16=retaddr, +20=key）。
    // 每 push 一次 esp 下移 4，所以固定的 [esp+16] 恰好依次命中 retaddr→edi→ebp→edx→self。
    push dword ptr [esp + 16]    // return_addr
    push dword ptr [esp + 16]    // caller_edi
    push dword ptr [esp + 16]    // caller_ebp
    push dword ptr [esp + 16]    // edx
    push dword ptr [esp + 16]    // self
    push dword ptr [esp + 40]    // key（相对 esp4 是 +20，esp9=esp4-20）
    call has_tech_body
    add esp, 24
    ret 4
  }
}

// 科技对象 → NameDesc：tech+0 指向 { const char* name; uint32_t key; }。
static uint32_t tech_object_namedesc(uint32_t tech) {
  if (!is_ptr(tech)) return 0;
  uint32_t nd = 0;
  if (!safe_read_u32(tech, &nd) || !is_ptr(nd)) return 0;
  return nd;
}

static const char* namedesc_name(uint32_t nd, uint32_t* key_out) {
  if (!nd) return nullptr;
  if (key_out) {
    uint32_t k = 0;
    safe_read_u32(nd + 4u, &k);
    *key_out = k;
  }
  uint32_t sp = 0;
  if (!safe_read_u32(nd, &sp) || !is_ptr(sp)) return nullptr;
  static char buf[80];
  return read_name_ascii(sp, buf, sizeof(buf)) ? buf : nullptr;
}

static unsigned char __cdecl has_tech_body(const uint32_t* key, void* self, void* edx,
                                          uint32_t caller_ebp, uint32_t caller_edi,
                                          uint32_t return_addr) {
  __try {
    ++g_tech_hook_calls;
    uint32_t raw_key = 0;
    if (key && is_ptr(reinterpret_cast<uint32_t>(key))) {
      safe_read_u32(reinterpret_cast<uint32_t>(key), &raw_key);
    }
    if (raw_key) {
      g_tech_hook_last_key = raw_key;
      if (protocol_key_banned(raw_key)) {
        ++g_tech_hook_hits;
        std::snprintf(g_tech_hook_last, sizeof(g_tech_hook_last), "key=%08X", raw_key);
        // ★ 记下"被禁协议对应的科技对象"
        //   现场抄自引擎 0xA6A610：edi = 行对象，tech = [[edi+0x48]+0x14]。
        //   面板网格 0xA22480 返回的也是科技对象，两者指针相同就能把
        //   「被禁协议」精确落到具体格子 idx 上，不再依赖名字/哈希猜测。
        note_banned_tech_from_row(caller_ebp);
        return 0;  // hasTech = false → 引擎把该协议判为锁定
      }
    }
    if (!protocol_ban_active()) return g_orig_has_tech ? g_orig_has_tech(self, edx, key) : 1;
    (void)caller_edi;
    (void)return_addr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }  if (g_orig_has_tech) return g_orig_has_tech(self, edx, key);
  return 1;
}

static void install_protocol_tech_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* has_tech = reinterpret_cast<void*>(loaded_module() + (0x87CE90u - kModBase));
  st = MH_CreateHook(has_tech, reinterpret_cast<void*>(&hk_has_tech),
                     reinterpret_cast<void**>(&g_orig_has_tech));
  if (st == MH_OK) {
    MH_EnableHook(has_tech);
    log("protocol hasTech hook armed at %p", has_tech);
  } else {
    log("protocol hasTech hook failed: %d", (int)st);
    g_orig_has_tech = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 协议商店对话框（Flash）逐行填充 —— 界面锁图真正的位置
//
// 0x00677A80：dialog 在 ecx，row 指针是栈上第一个参数。它把一行协议推到 Flash：
//   prop 0x3D <- row+0x38   (AsciiString，char* 在 +0)
//   prop 0x3F <- row+0x40   (标题)
//   prop 0x40 <- (row+0x44 != 0)
//   prop 0x41 <- (row+0x4C != 0)
// 唯一调用者是 0x0068A061，位于对话框状态机 0x0068A050 内（[dialog+0x6C4]==4 时填一行）。
// 也就是说：在这两个字节上做手脚，能直接改变协议格子的锁定外观，
// 而 Player+0x20C 那张科技禁用表跟这个面板毫无关系（已实测确认）。
// ---------------------------------------------------------------------------
static void* g_orig_fill_row = nullptr;

// 把界面 id 归一化成纯小写字母数字，便于和引擎里的名字做宽松匹配。
static void normalize_id(const char* src, char* dst, size_t cap) {
  size_t n = 0;
  if (!src || !cap) { if (cap) dst[0] = 0; return; }
  for (const char* p = src; *p && n + 1 < cap; ++p) {
    const char c = *p;
    if (c >= 'A' && c <= 'Z') { dst[n++] = static_cast<char>(c + 32); }
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { dst[n++] = c; }
  }
  dst[n] = 0;
}

// 行名是否命中"被禁协议"。
// 先按 NameKey 精确比（引擎模板名），再按归一化名字做包含匹配
// （例如 PlayerTech_Soviet_MagneticSatellite_Rank_2 ⊃ magneticsatellite + rank2）。
static bool row_name_banned(const char* name) {
  if (!name || !name[0]) return false;
  const uint32_t key = name_key(name);
  if (key && protocol_key_banned(key)) return true;

  char norm[128];
  normalize_id(name, norm, sizeof(norm));
  if (!norm[0]) return false;
  for (const std::string& id : g_protocol_bans) {
    char other[128];
    normalize_id(id.c_str(), other, sizeof(other));
    if (!other[0]) continue;
    // 去掉 "playerpower" 前缀
    const char* core = other;
    const char* pfx = "playerpower";
    const size_t plen = std::strlen(pfx);
    if (std::strncmp(core, pfx, plen) == 0) core += plen;
    if (!core[0]) continue;
    // 拆出结尾的等级数字
    size_t len = std::strlen(core);
    size_t digits = 0;
    while (digits < len && core[len - 1 - digits] >= '0' && core[len - 1 - digits] <= '9') ++digits;
    char stem[128];
    const size_t stem_len = len - digits;
    if (stem_len == 0 || stem_len >= sizeof(stem)) continue;
    std::memcpy(stem, core, stem_len);
    stem[stem_len] = 0;
    if (!std::strstr(norm, stem)) continue;
    if (digits == 0) return true;
    // 有等级时必须等级也对上，避免 "…Satellite2" 命中 Rank_3
    const char* rank = core + stem_len;
    char want1[16], want2[16];
    std::snprintf(want1, sizeof(want1), "rank%s", rank);
    std::snprintf(want2, sizeof(want2), "lvl%s", rank);
    if (std::strstr(norm, want1) || std::strstr(norm, want2)) return true;
    // 名字末尾就是该数字的情况也接受
    const size_t nl = std::strlen(norm);
    const size_t dl = std::strlen(rank);
    if (nl >= dl && std::strcmp(norm + nl - dl, rank) == 0) return true;
  }
  return false;
}

static void __cdecl fill_row_probe(uint32_t self, uint32_t row) {
  (void)self;
  __try {
    ++g_fill_calls;
    if (!is_ptr(row)) return;
    uint32_t namep = 0, titlep = 0;
    safe_read_u32(row + 0x38u, &namep);
    safe_read_u32(row + 0x40u, &titlep);
    char name[96] = {};
    char title[96] = {};
    if (is_ptr(namep)) read_name_ascii(namep, name, sizeof(name));
    if (is_ptr(titlep)) read_name_ascii(titlep, title, sizeof(title));

    uint32_t f44w = 0, f4cw = 0;
    safe_read_u32(row + 0x44u, &f44w);
    safe_read_u32(row + 0x4Cu, &f4cw);
    const unsigned f44 = f44w & 0xFFu, f4c = f4cw & 0xFFu;

    // 前一百多行的真实名字 + 整段原始结构记下来。同一批行里 rank3 是引擎自己
    // 锁住的，对比 rank1/rank2（可选）就能反推"锁"到底落在哪个字节上。
    const size_t used = std::strlen(g_fill_seen);
    if (name[0] && g_fill_calls <= 140 && used + std::strlen(name) + 80 < sizeof(g_fill_seen)) {
      uint32_t w30 = 0, w34 = 0, w48 = 0, w50 = 0, w54 = 0;
      safe_read_u32(row + 0x30u, &w30);
      safe_read_u32(row + 0x34u, &w34);
      safe_read_u32(row + 0x48u, &w48);
      safe_read_u32(row + 0x50u, &w50);
      safe_read_u32(row + 0x54u, &w54);
      std::snprintf(g_fill_seen + used, sizeof(g_fill_seen) - used,
                    "%s[44=%u 4C=%u 30=%08X 34=%08X 48=%08X 50=%08X 54=%08X] ",
                    name, f44, f4c, w30, w34, w48, w50, w54);
    }

    if (!protocol_ban_active()) return;
    if (!row_name_banned(name)) return;

    ++g_fill_hits;
    // 锁住：把两个布尔都变成 false（"不可用"/"未拥有"）。
    // 这两个字节是面板 prop 0x40 / 0x41 的唯一来源，哪一个代表
    // "available" 都会被清掉 —— 显示与建造限制一致的锁定样式。
    safe_write_u32(row + 0x44u, (f44w & 0xFFFFFF00u) | 0u);
    safe_write_u32(row + 0x4Cu, (f4cw & 0xFFFFFF00u) | 0u);
    std::snprintf(g_fill_log_last, sizeof(g_fill_log_last),
                  "row=%08X f44=%u->0 f4c=%u->0 name=%s", row, f44, f4c, name);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// thiscall + 栈参数 + 返回值在 al：用 naked 转发，保证寄存器/栈完全原样进入跳板。
static __declspec(naked) void hk_fill_row(void) {
  __asm {
    pushfd
    pushad
    mov eax, [esp + 0x28]     // 原始 [esp+4]：row 指针
    push eax
    mov eax, [esp + 0x1C]     // pushad 存下的 ecx（this），已因上面的 push 偏移 4
    push eax
    call fill_row_probe
    add esp, 8
    popad
    popfd
    jmp dword ptr [g_orig_fill_row]
  }
}

static void install_protocol_store_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fill = reinterpret_cast<void*>(loaded_module() + (0x677A80u - kModBase));
  st = MH_CreateHook(fill, reinterpret_cast<void*>(&hk_fill_row),
                     reinterpret_cast<void**>(&g_orig_fill_row));
  if (st == MH_OK) {
    MH_EnableHook(fill);
    log("protocol store row hook armed at %p", fill);
  } else {
    log("protocol store row hook failed: %d", (int)st);
    g_orig_fill_row = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 协议面板（igm_PlayerTechStoreDialog）真正的行数据
//
//   0x00A78C40  面板对象构造：creates the .swf, 行槽 15 个，
//               每个 0xBC 字节，起始于 dialog+0x108。
//   0x00A71BB0  行槽取址：eax = dialog + 0x108 + (a1 + 5*a2) * 0xBC
//               唯一调用者 0x00A0D969（逐行喂给 Flash 的迭代器）。
// 这里把行槽原样打出来，一次对局就能看出"锁定"落在哪个字节。
static void* g_orig_panel_row = nullptr;

// 行槽只探有限次数：这个取址函数在面板每帧都会跑，重复打印会把日志写爆。
// 同一行取址会早于"填好数据"，所以每行转储前几次，保证能看到填充后的样子。
static uint32_t g_panel_seen[256];
static unsigned char g_panel_seen_n[256];
static int g_panel_seen_used = 0;
static int g_panel_dumps = 0;

static bool panel_row_should_dump(uint32_t row) {
  if (g_panel_dumps >= 30) return false;   // 只抓第一轮 15 个格子

  for (int i = 0; i < g_panel_seen_used; ++i) {
    if (g_panel_seen[i] == row) {
      if (g_panel_seen_n[i] >= 3) return false;
      ++g_panel_seen_n[i];
      ++g_panel_dumps;
      return true;
    }
  }
  if (g_panel_seen_used >= 256) return false;
  g_panel_seen[g_panel_seen_used] = row;
  g_panel_seen_n[g_panel_seen_used] = 1;
  ++g_panel_seen_used;
  ++g_panel_dumps;
  return true;
}

// ---------------------------------------------------------------------------
// 一次性"身份定位"：面板各方块到底怎么和 PlayerPower_* 对上号？
// 直接在 dialog / 行槽 / 状态对象 / impl 技术对象里逐层搜被禁协议的 NameKey。
// 只跑一次（有禁止名单后），避免影响 UI 帧时间。
// ---------------------------------------------------------------------------
static bool g_id_hunted = false;

static void idhunt_scan(const char* tag, int idx, uint32_t base, uint32_t span) {
  if (!is_ptr(base)) return;
  for (uint32_t off = 0; off + 4u <= span; off += 4u) {
    uint32_t v = 0;
    if (!safe_read_u32(base + off, &v)) continue;
    if (!protocol_key_banned(v)) continue;
    log("idhunt %s idx=%d base=%08X +%03X key=%08X", tag, idx, base, (unsigned)off, v);
  }
}

static void protocol_identity_hunt(uint32_t self) {
  if (g_id_hunted) return;
  if (g_ban_keys_n == 0) return;          // 还没有禁止名单，等下一帧
  uint32_t dialog = 0;
  if (!safe_read_u32(self + 4u, &dialog) || !is_ptr(dialog)) return;
  g_id_hunted = true;
  log("idhunt START dialog=%08X banks=%d", dialog, g_ban_keys_n);

  // 1) dialog 头部（行槽从 +0x108 开始，先扫一大片头部）
  idhunt_scan("dialog", -1, dialog, 0x1400);

  // 2) 每个行槽 + 它的状态对象 + 状态对象里的指针
  for (int idx = 0; idx < 15; ++idx) {
    const uint32_t slot = dialog + 0x108u + (uint32_t)idx * 0xBCu;
    idhunt_scan("slot", idx, slot, 0xBC);
    uint32_t obj = 0;
    if (safe_read_u32(slot + 0x60u, &obj) && is_ptr(obj)) {
      idhunt_scan("obj", idx, obj, 0x100);
      for (uint32_t o = 0; o <= 0x3C; o += 4u) {
        uint32_t p = 0;
        if (!safe_read_u32(obj + o, &p) || !is_ptr(p)) continue;
        idhunt_scan("obj>>", idx, p, 0x60);
      }
    }
  }

  // 3) impl 里的技术对象（g_cell_impl 由 grid 钩子捕获）
  if (is_ptr(g_cell_impl)) {
    for (int idx = 0; idx < 15; ++idx) {
      uint32_t o = 0;
      if (!safe_read_u32(g_cell_impl + 0x20u + (uint32_t)idx * 4u, &o) || !is_ptr(o)) continue;
      idhunt_scan("impl", idx, o, 0x60);
      for (uint32_t q = 0; q <= 0x58; q += 4u) {
        uint32_t p = 0;
        if (!safe_read_u32(o + q, &p) || !is_ptr(p)) continue;
        idhunt_scan("impl>>", idx, p, 0x60);
      }
    }
  }
  log("idhunt DONE");
}

static void __cdecl panel_row_probe(uint32_t self, uint32_t a1, uint32_t a2, uint32_t row) {
  __try {
    ++g_panel_calls;
    if (!is_ptr(row)) return;
    // 一次性：把 dialog 对象扫一遍，找出它持有的协议条目数组。
    // 面板用 dialog+0x110 起步、行步长 0xBC、列步长 0x3AC 遍历 5x3，条目里
    // 一定带着协议身份（PlayerTech_*），那就是"锁定"状态所在的记录。
    static bool dialog_scanned = false;
    if (!dialog_scanned && is_ptr(self)) {
      uint32_t dlg = 0;
      if (safe_read_u32(self + 4u, &dlg) && is_ptr(dlg)) {
        dialog_scanned = true;
        log("panel dialog=%08X scan start", dlg);
        for (uint32_t off = 0; off < 0x6000; off += 4) {
          uint32_t p = 0;
          if (!safe_read_u32(dlg + off, &p) || !is_ptr(p)) continue;
          char s[64] = {};
          if (!read_name_ascii(p, s, sizeof(s))) continue;
          // 协议 id 实际是 PlayerPower_* / SpecialPower_* / PlayerUpgrade_*，
          // 早先只过滤 PlayerTech 导致整个 dialog 扫描一个都没匹配上。
          if (std::strncmp(s, "Player", 6) != 0 && std::strncmp(s, "Special", 7) != 0 &&
              std::strstr(s, "Satellite") == nullptr && std::strstr(s, "Power") == nullptr) {
      continue;
    }
          log("panel dialog +%04X -> %08X %s", off, p, s);
        }
        // 顺带把头部字段打出来（含任务点数、行/列计数等）
        for (uint32_t base = 0; base < 0x140; base += 12) {
          char line[200] = {};
          size_t used = 0;
          for (uint32_t k = 0; k < 12; ++k) {
            uint32_t v = 0;
            safe_read_u32(dlg + base + k * 4u, &v);
            used += (size_t)std::snprintf(line + used, sizeof(line) - used, "%03X:%08X ",
                                          (unsigned)(base + k * 4u), v);
            if (used >= sizeof(line) - 24) break;
          }
          log("panel dlg %08X %s", dlg + base, line);
        }
      }
    }
    protocol_identity_hunt(self);
    if (!panel_row_should_dump(row)) return;
    ++g_panel_rows;
    uint32_t dialog = 0;
    safe_read_u32(self + 4u, &dialog);
    if (is_ptr(dialog)) g_panel_dialog = dialog;
    g_panel_row_tick = GetTickCount();
    compute_lock_reference(dialog);
    log("panel row self=%08X dialog=%08X a1=%u a2=%u row=%08X idx=%u btn=%08X", self, dialog,
        a1, a2, row, a1 + 5u * a2, cell_button_addr(a1 + 5u * a2));
    uint32_t w[0xBC / 4] = {};
    for (uint32_t i = 0; i < 0xBC / 4; ++i) safe_read_u32(row + i * 4u, &w[i]);
    for (uint32_t base = 0; base < 0xBC / 4; base += 12) {
      char line[200] = {};
      size_t used = 0;
      for (uint32_t k = 0; k < 12 && base + k < 0xBC / 4; ++k) {
        used += (size_t)std::snprintf(line + used, sizeof(line) - used, "%02X:%08X ",
                                      (unsigned)((base + k) * 4u), w[base + k]);
        if (used >= sizeof(line) - 24) break;
      }
      log("panel dump %08X +%03X %s", row, (unsigned)(base * 4u), line);
    }
    // 指针字段上的可读串（协议名/标题）
    for (uint32_t i = 0; i < 0xBC / 4; ++i) {
      const uint32_t p = w[i];
      if (!is_ptr(p)) continue;
      char s[64] = {};
      if (!read_name_ascii(p, s, sizeof(s))) continue;
      if (std::strlen(s) < 3) continue;
      log("panel str +%02X -> %08X %s", (unsigned)(i * 4u), p, s);
    }
    // +0x60 是指向该格协议按钮对象的指针。那个对象才是真正带协议身份和
    // 状态字节的地方，深入两层把它的可读串和原始字段挖出来。
    for (int depth = 0; depth < 2; ++depth) {
      uint32_t obj = w[(0x60 + depth * 4) / 4];
      if (!is_ptr(obj)) continue;
      log("panel obj row+%02X = %08X (depth %d)", 0x60 + depth * 4, obj, depth);
      for (uint32_t off = 0; off < 0x100; off += 4) {
        uint32_t p = 0;
        if (!safe_read_u32(obj + off, &p) || !is_ptr(p)) continue;
        char s[64] = {};
        if (!read_name_ascii(p, s, sizeof(s))) continue;
        if (std::strlen(s) < 3) continue;
        log("panel obj +%03X -> %08X %s", off, p, s);
      }
      for (uint32_t base = 0; base < 0x100; base += 12) {
        char line[200] = {};
        size_t used = 0;
        for (uint32_t k = 0; k < 12; ++k) {
          uint32_t v = 0;
          safe_read_u32(obj + base + k * 4u, &v);
          used += (size_t)std::snprintf(line + used, sizeof(line) - used, "%03X:%08X ",
                                        (unsigned)(base + k * 4u), v);
          if (used >= sizeof(line) - 24) break;
        }
        log("panel objdump %08X %s", obj + base, line);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// 用 naked 转发：原函数是 ecx=this、两个栈参数、ret 8。
// 进来时（pushfd+pushad 之后）栈布局固定为：
//   [esp+0x00]=edi [esp+0x04]=esi [esp+0x08]=ebp [esp+0x0C]=esp
//   [esp+0x10]=ebx [esp+0x14]=edx [esp+0x18]=ecx(=this) [esp+0x1C]=eax
//   [esp+0x20]=eflags [esp+0x24]=返回地址 [esp+0x28]=arg1 [esp+0x2C]=arg2
static __declspec(naked) void hk_panel_row(void) {
  __asm {
    pushfd
    pushad
    mov eax, [esp + 0x2C]        // arg2
    mov edx, [esp + 0x28]        // arg1
    mov ecx, [esp + 0x18]        // this
    push eax
    push edx
    call dword ptr [g_orig_panel_row]   // 原函数自己 ret 8 清掉这两个参数
    mov [esp + 0x1C], eax        // 写回保存的 eax，popad 后就是返回值
    push eax                     // panel_row_probe(this, a1, a2, row)
    push dword ptr [esp + 0x30]  // arg2（已因上面的 push 右移 4）
    push dword ptr [esp + 0x30]  // arg1（再右移 4，正好落在同一个偏移）
    push dword ptr [esp + 0x24]  // this
    call panel_row_probe
    add esp, 16
    popad
    popfd
    ret 8
  }
}

static void install_protocol_panel_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0xA71BB0u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_panel_row),
                     reinterpret_cast<void**>(&g_orig_panel_row));
  if (st == MH_OK && g_orig_panel_row) {
    MH_EnableHook(fn);
    log("protocol panel row hook armed at %p", fn);
  } else {
    log("protocol panel row hook failed: %d", (int)st);
    g_orig_panel_row = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 协议面板格子 → PlayerTech 映射探针
//
// 0xA71BB0 只给出"界面格子"，格子本身不含协议身份。真正把 (列, 行) 映射到
// 协议数据的是 0xA22480：
//     impl = [this+4]; vec = [impl+0x1C];            // vector<vector<ptr>>
//     outer = vec[col];  ptr = outer.data[row];
// 返回的就是该格对应的 PlayerTech 对象。锁定/可购买状态就挂在这个对象上。
// ---------------------------------------------------------------------------
static void* g_orig_grid_lookup = nullptr;
static uint32_t g_grid_seen[64];
static int g_grid_seen_used = 0;

// 被限制协议的引擎身份（isReady 侧解析出来的模块/模板指针）。
// 面板格子里只要出现相同的指针，就能确认"这一格就是这个协议"。
static uint32_t g_ban_ptrs[32];
static int g_ban_ptr_n = 0;
static bool g_ban_ptr_logged[32];

static void remember_ban_ptr(uint32_t self, uint32_t tmpl) {
  if (g_ban_ptr_n >= 32) return;
  uint32_t vals[2] = {self, tmpl};
  for (int k = 0; k < 2; ++k) {
    const uint32_t v = vals[k];
    if (!is_ptr(v)) continue;
    bool dup = false;
    for (int i = 0; i < g_ban_ptr_n; ++i) {
      if (g_ban_ptrs[i] == v) { dup = true; break; }
    }
    if (dup) continue;
    g_ban_ptr_logged[g_ban_ptr_n] = false;
    g_ban_ptrs[g_ban_ptr_n++] = v;
  }
}

static bool is_ban_ptr(uint32_t v) {
  for (int i = 0; i < g_ban_ptr_n; ++i) {
    if (g_ban_ptrs[i] == v) return true;
  }
  return false;
}

// 面板每个格子的锁定状态 = [impl+0x20+idx*4] 对象的 byte[+0x18]（1 = 锁定）。
// impl 是 0xA22480 里的 [self+4]，idx = a1 + 5*a2（0xA1B470 的算法）。
static uint32_t g_locked_idx[16];
static uint32_t g_locked_impl[16];
static int g_locked_n = 0;

static void apply_cell_lock(uint32_t impl, uint32_t idx, const char* why) {
  if (!is_ptr(impl) || idx >= 16) return;
  uint32_t obj = 0;
  if (!safe_read_u32(impl + 0x20u + idx * 4u, &obj) || !is_ptr(obj)) return;
  unsigned char lock_before = 0, lock_after = 0;
  uint32_t row_idx = 0;
  safe_read_u8(obj + 0x18, &lock_before);
  safe_read_u32(obj + 0x08, &row_idx);
  // 只写 +0x18 的最低位：这是引擎自己锁定格子时置 1 的锁标志。
  // 注意：+0x08 不是 alpha 而是该格所在的行号（实测 0,1,2,3,4 循环），
  // 写它会破坏格子坐标，之前退出时的 0xC0000005 就是这类越界写入导致。
  // +0x28 / +0x2C / +0x30 里含指针，同样绝对不能写。
  safe_write_u8(obj + 0x18, 1);
  safe_read_u8(obj + 0x18, &lock_after);
  log("cell lock %s impl=%08X idx=%u obj=%08X lock %u->%u row=%u", why, impl, idx,
      obj, (unsigned)lock_before, (unsigned)lock_after, row_idx);
  if (g_locked_n < 16) {
    g_locked_impl[g_locked_n] = impl;
    g_locked_idx[g_locked_n++] = idx;
  }
}

// 调试用的"只锁指定格子"开关，正式逻辑里必须保持 -1（不锁）。
static int g_debug_lock_idx = -1;
static int g_debug_lock_done = 0;

// ---------------------------------------------------------------------------
// 协议面板槽位几何（用实测日志反推，15 格全部吻合）
//
//   dialog      = [rowlookup_this + 4]
//   slot(idx)   = dialog + 0x108 + idx*0xBC     // 0xA71BB0 的返回值
//   button(idx) = slot(idx) + 8                 // 0xA44AB0 的 this
//   idx         = a1 + 5*a2                     // 0xA1B470 的算法
//
// 按钮外观状态在 [button+0x60]：0 = available，1 = locked，2 = owned。
// 0xA44AB0 会把这个值映射成 "SetAvailability" 参数推给 Flash，锁图由引擎
// 自己绘制。所以把某个格子的按钮状态改成 1，就能得到和"建造限制"完全
// 一样的科技锁定外观（锁图 + 不可点击）。
//
// 校验依据（实测日志，state 与格子 lockbyte 完全对应）：
//   idx0 state=2  owned     ← 一级磁力卫星（已拥有）
//   idx1 state=0  available ← 二级磁力卫星（可用）
//   idx2..14 state=1 locked ← 引擎科技锁定
//
// 这些对象与数组在文件前部按钮钩子处定义，这里只做说明。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 真正生效的锁定通道
//
// 0xA44AB0 是每个格子按钮的"刷新外观"函数，它读 [self+0x60]：
//     0 = available / 1 = locked / 2 = owned
// 再 push "SetAvailability" -> 0x953F60，锁图由引擎自己画。
// 所以只要能确定"哪个按钮对象对应被限制的协议"，在调用原函数前把
// [self+0x60] 改成 1 即可，外观完全交给引擎，和建造限制一个观感。
//
// 按钮与格子的对应关系已由实测反推（见 cell_button_addr 注释），
// 所以这里不再靠扫描指针猜测，直接用 dialog + 0x110 + idx*0xBC 定位。
// ---------------------------------------------------------------------------
static uint32_t g_force_objs[16];       // 对应格子状态对象指针（供按钮比对）
static int g_force_obj_n = 0;
static uint32_t g_orig_img[16] = {};    // 被覆盖前的原始协议图标

static void force_cell(uint32_t impl, uint32_t idx, const char* why) {
  if (idx >= 15) return;
  if (!g_cell_locked[idx]) {
    g_cell_locked[idx] = true;
    log("force cell %s idx=%u button=%08X (dialog=%08X)", why, idx, cell_button_addr(idx),
        g_panel_dialog);
    // 两个字段分工不同，都要写：
    //   [button+0x60] = 1  -> 引擎画锁图（外观）
    //   状态对象 +0x18 = 1 -> 面板判定"不可用"，拦截点击与悬停提示（科技锁定）
    if (is_ptr(impl)) apply_cell_lock(impl, idx, why);
  }
  // 记住原始图标，解除限制时才能精确还原。
  const uint32_t btn = cell_button_addr(idx);
  if (is_ptr(btn)) {
    uint32_t img = 0;
    if (safe_read_u32(btn + 0x5Cu, &img) && is_ptr(img) && g_orig_img[idx] == 0) {
      g_orig_img[idx] = img;
    }
  }
  uint32_t obj = 0;
  if (is_ptr(impl)) safe_read_u32(impl + 0x20u + idx * 4u, &obj);
  if (is_ptr(obj)) {
    bool dup = false;
    for (int i = 0; i < g_force_obj_n; ++i) {
      if (g_force_objs[i] == obj) { dup = true; break; }
    }
    if (!dup && g_force_obj_n < 16) g_force_objs[g_force_obj_n++] = obj;
  }
}

// 按钮对象内部是否持有某个"被锁定格子"的状态对象指针。
static bool btn_holds_forced_obj(uint32_t self) {
  for (uint32_t off = 0; off < 0x200; off += 4) {
    uint32_t v = 0;
    if (!safe_read_u32(self + off, &v)) continue;
    for (int i = 0; i < g_force_obj_n; ++i) {
      if (v == g_force_objs[i]) return true;
    }
  }
  return false;
}

// 禁止集合变化时清空"哪些格子该锁"的记录。
static void protocol_panel_clear_locks() {
  // 还原：锁字节、按钮状态、原始图标都恢复，避免解除限制后残留锁定外观。
  if (is_ptr(g_panel_dialog)) {
    for (uint32_t idx = 0; idx < 16; ++idx) {
      if (!g_cell_locked[idx]) continue;
      const uint32_t cell = cell_button_addr(idx);
      if (!is_ptr(cell)) continue;
      uint32_t vt = 0;
      if (!safe_read_u32(cell, &vt) || vt != 0x00C7F8D0u) continue;
      if (g_orig_img[idx]) {
        safe_write_u32(cell + 0x5Cu, g_orig_img[idx]);
      }
      safe_write_u32(cell + 0x60u, 0);
      // 状态对象的锁字节也还原成"未锁"。
      uint32_t impl = 0, obj = 0;
      if (safe_read_u32(g_panel_lookup_self + 4u, &impl) && is_ptr(impl) &&
          safe_read_u32(impl + 0x20u + idx * 4u, &obj) && is_ptr(obj)) {
        safe_write_u8(obj + 0x18, 0);
      }
      using FnRefresh = void(__thiscall*)(uint32_t self);
      FnRefresh refresh = reinterpret_cast<FnRefresh>(g_orig_btn_refresh);
      if (refresh) {
        __try {
          refresh(cell);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
      }
    }
  }
  for (int i = 0; i < 16; ++i) {
    g_cell_locked[i] = false;
    g_cell_visual_logged[i] = false;
    g_visual_copied[i] = false;
    g_orig_img[i] = 0;
  }
  g_force_obj_n = 0;
  g_grid_seen_used = 0;   // 让面板重新识别（下次构建时）
  g_ref_state = 0;        // 参考锁定格重新计算
  g_ref_cell = 0;
  g_btn_refresh_calls = 0;   // 允许重新打一轮日志
}

// 主动重扫面板 15 格，找出被限制协议落在哪一格。
// 这样即使"先开面板、再设限制"，也能立刻生效，不用关掉重开。
static void protocol_panel_rescan() {
  using FnLookup = uint32_t(__thiscall*)(uint32_t self, uint32_t a1, uint32_t a2);
  FnLookup lookup = reinterpret_cast<FnLookup>(g_orig_grid_lookup);
  if (!lookup || !is_ptr(g_panel_lookup_self)) return;
  for (uint32_t a2 = 0; a2 < 3; ++a2) {
    for (uint32_t a1 = 0; a1 < 5; ++a1) {
      const uint32_t idx = a1 + 5u * a2;
      if (g_cell_locked[idx]) continue;
      uint32_t tech = 0;
      __try {
        tech = lookup(g_panel_lookup_self, a1, a2);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
      }
      if (!is_ptr(tech)) continue;
      uint32_t key = 0, where = 0;
      if (is_banned_tech(tech)) {
        log("rescan TECHHIT idx=%u tech=%08X", idx, tech);
        force_cell(g_panel_dialog, idx, "rescan");
        apply_cell_visual_lock(idx);
      } else if (find_ban_key_in_tech(tech, &key, &where)) {
        log("rescan KEYHIT idx=%u tech=%08X key=%08X where=%u", idx, tech, key, where);
        force_cell(g_panel_dialog, idx, "rescan");
        apply_cell_visual_lock(idx);
      }
    }
  }
}

// 面板正开着时立刻生效：写状态并主动让引擎重推一次 SetAvailability，
// 这样不用关掉重开面板就能看到锁图。
static void protocol_panel_live_lock() {
  if (g_protocol_bans.empty()) return;
  if (!is_ptr(g_panel_dialog)) return;
  // 面板很久没构建过行数据，说明早就关了，别去碰可能已释放的对象。
  const DWORD now = GetTickCount();
  if (g_panel_row_tick == 0) return;
  if ((int32_t)(now - g_panel_row_tick) > 8000) return;

  // 先确认哪些格子该锁（先开面板、后设限制的情况靠这一步补上）。
  bool any = false;
  for (int i = 0; i < 16; ++i) {
    if (g_cell_locked[i]) { any = true; break; }
  }
  if (!any) {
    // 这个 rescan 会从渲染线程反向调用引擎的 lookup，代价高且不适合每帧跑。
    // 限流到 2 秒一次，避免开面板时卡顿。
    static DWORD last_rescan = 0;
    if (last_rescan != 0 && (int32_t)(now - last_rescan) < 2000) return;
    last_rescan = now;
    protocol_panel_rescan();
    for (int i = 0; i < 16; ++i) {
      if (g_cell_locked[i]) { any = true; break; }
    }
    if (!any) return;
  }

  using FnRefresh = void(__thiscall*)(uint32_t self);
  FnRefresh refresh = reinterpret_cast<FnRefresh>(g_orig_btn_refresh);
  if (!refresh) return;

  // ★ 只维护"已经确定要锁"的格子，并且限流。
  //   之前每次 tick 都对全部格子调 refresh，会把面板刷爆导致明显卡顿。
  static DWORD last_live = 0;
  if (last_live != 0 && (int32_t)(now - last_live) < 500) return;
  last_live = now;

  for (uint32_t idx = 0; idx < 15; ++idx) {
    if (g_cell_locked[idx]) apply_cell_visual_lock(idx);
  }
}



// ---------------------------------------------------------------------------
// 协议身份识别（两条独立通道，只要一条命中就够）
//
// A) 字符串子串递归搜索：协议内部名一定含 "MagneticSatellite" 这类特征词。
//    不依赖任何结构偏移，也不受 UI 语言影响，是最稳的一条。
// B) NameKey 三层解引用：路径抄自引擎自己的 0xA6A672 现场代码
//        key = [[[tech + 0xC] + 0] + 4]
//    拿到 key 就能和禁止集合精确比对。
// ---------------------------------------------------------------------------

// 从禁止集合里取出"特征词"：去掉常见前缀后剩下的部分。
static bool ban_feature_matches(const char* s) {
  if (!s || !*s) return false;
  for (const std::string& id : g_protocol_bans) {
    if (id.size() < 6) continue;
    // 全等
    if (_stricmp(s, id.c_str()) == 0) return true;
    // 子串：id 里去掉 PlayerPower_ / PlayerTech_ 等前缀后的核心词
    const char* core = id.c_str();
    static const char* kPrefixes[] = {"PlayerPower_", "PlayerTech_", "Player_", "Power_"};
    for (const char* pfx : kPrefixes) {
      const size_t n = std::strlen(pfx);
      if (_strnicmp(core, pfx, n) == 0) {
        core += n;
        break;
      }
    }
    if (std::strlen(core) >= 5 && strstr(s, core) != nullptr) return true;
    // s 反过来包含于 id（有些结构只存短名）
    if (std::strlen(s) >= 6 && _strnicmp(s, core, std::strlen(s)) == 0) return true;
  }
  return false;
}

// 递归扫描对象指针图（深度 2，每层 0..0x200），找协议名字特征词。
static bool tech_graph_scan(uint32_t obj, int depth, char* out, size_t out_sz);

static bool tech_graph_matches_ban(uint32_t obj, char* out, size_t out_sz) {
  return tech_graph_scan(obj, 0, out, out_sz);
}

static bool tech_graph_scan(uint32_t obj, int depth, char* out, size_t out_sz) {
  if (!is_ptr(obj) || depth > 2) return false;
  for (uint32_t off = 0; off < 0x200; off += 4) {
    uint32_t p = 0;
    if (!safe_read_u32(obj + off, &p) || !is_ptr(p)) continue;
    char s[128] = {};
    if (read_name_ascii(p, s, sizeof(s)) && std::strlen(s) >= 6) {
      if (ban_feature_matches(s)) {
        if (out && out_sz) std::snprintf(out, out_sz, "%s", s);
        return true;
      }
      continue;   // 已经是可读串，不必再当指针往下钻
    }
    if (depth < 2 && tech_graph_scan(p, depth + 1, out, out_sz)) return true;
  }
  return false;
}

// key = [[[tech + 0xC] + 0] + 4]，name = [[[tech + 0xC] + 0] + 0]
static bool tech_namekey(uint32_t tech, uint32_t* key_out, char* name_out, size_t name_sz) {
  if (!is_ptr(tech)) return false;
  uint32_t p1 = 0, p2 = 0;
  if (!safe_read_u32(tech + 0x0Cu, &p1) || !is_ptr(p1)) return false;
  if (!safe_read_u32(p1, &p2) || !is_ptr(p2)) return false;
  uint32_t k = 0;
  safe_read_u32(p2 + 4u, &k);
  if (key_out) *key_out = k;
  if (name_out && name_sz) {
    name_out[0] = 0;
    uint32_t sp = 0;
    if (safe_read_u32(p2, &sp) && is_ptr(sp)) read_name_ascii(sp, name_out, name_sz);
  }
  return k != 0;
}

static void __cdecl panel_grid_probe(uint32_t self, uint32_t a1, uint32_t a2, uint32_t tech) {
  __try {
    if (!is_ptr(tech)) return;

    // impl 与 idx：0xA22480 用 [self+4] 作为 impl，0xA1B470 用 idx = a1 + 5*a2。
    uint32_t impl = 0;
    const bool have_impl = safe_read_u32(self + 4u, &impl) && is_ptr(impl);
    const uint32_t idx = a1 + 5u * a2;
    // 注意：这里的 impl 是「状态数组持有者」，和 0xA71BB0 拿到的 dialog 是
    // 两个不同的对象（实测 impl == avail_self+4，dialog 在别处）。
    // 绝不能拿 impl 覆盖 g_panel_dialog，否则格子几何全部失效。
    if (have_impl) g_cell_impl = impl;
    if (is_ptr(self)) g_panel_lookup_self = self;

    // ★ 识别必须放在去重之前跑。
    //   面板重开后 tech 指针常常和上次相同，若先去重就 return，
    //   第二次打开面板永远不会再识别 —— 这正是"重开就没锁图"的原因之一。
    if (idx < 15 && !g_cell_locked[idx]) {
      uint32_t key = 0, where = 0;
      if (find_ban_key_in_tech(tech, &key, &where)) {
        log("cell KEYHIT idx=%u tech=%08X key=%08X where=%u", idx, tech, key, where);
        force_cell(impl, idx, "key");
        apply_cell_visual_lock(idx);
      } else if (is_banned_tech(tech)) {
        log("cell TECHHIT idx=%u tech=%08X", idx, tech);
        force_cell(impl, idx, "techptr");
        apply_cell_visual_lock(idx);
      }
    }

    // 下面只是详细 dump，按 tech 指针去重，避免刷屏和掉帧。
    for (int i = 0; i < g_grid_seen_used; ++i) {
      if (g_grid_seen[i] == tech) return;
    }
    if (g_grid_seen_used >= 64) return;
    g_grid_seen[g_grid_seen_used++] = tech;

    log("grid self=%08X a1=%u a2=%u tech=%08X", self, a1, a2, tech);

    // ★ 1) 主识别：在科技对象的指针图里递归找"被限制协议的名字片段"。
    //    协议内部名一定包含 PlayerPower_MagneticSatellite 这类字样，因此用
    //    子串匹配（而不是哈希或全等），既不依赖结构偏移也不受语言影响。
    if (is_ptr(tech)) {
      char hit_name[96] = {};
      if (tech_graph_matches_ban(tech, hit_name, sizeof(hit_name))) {
        log("cell NAMEHIT idx=%u tech=%08X name=%s", idx, tech, hit_name);
        force_cell(impl, idx, "name");
      }
    }

    // ★ 2) 辅助识别：按引擎自己的 NameDesc 三层解引用取 NameKey。
    //    路径来自 0xA6A672 的现场代码：
    //        key = [[[tech + 0xC] + 0] + 4]
    if (is_ptr(tech)) {
      uint32_t key = 0;
      char nm[96] = {};
      if (tech_namekey(tech, &key, nm, sizeof(nm))) {
        const bool bad = protocol_key_banned(key);
        log("cellkey idx=%u tech=%08X key=%08X name=%s banned=%d", idx, tech, key,
            nm[0] ? nm : "-", bad ? 1 : 0);
        if (bad) force_cell(impl, idx, "techkey");
      }
    }

    // 1) 最可靠的识别：格子对象里是否出现"被限制协议的引擎名"。
    //    PlayerTech 内部持有 "PlayerPower_MagneticSatellite1" 这类名字指针，
    //    直接做字符串比较，不受哈希/版本差异影响。
    if (is_ptr(tech)) {
      bool matched = false;
      char hit_name[64] = {};
      uint32_t hit_off = 0;
      uint32_t cand[64] = {};
      int nc = 0;
      for (uint32_t off = 0; off < 0x100 && nc < 64; off += 4) {
        uint32_t v = 0;
        if (!safe_read_u32(tech + off, &v) || !is_ptr(v)) continue;
        cand[nc++] = v;
      }
      for (int ci = 0; ci < nc && !matched; ++ci) {
        char s[64] = {};
        if (!read_name_ascii(cand[ci], s, sizeof(s))) continue;
        if (std::strlen(s) < 6) continue;
        for (const std::string& id : g_protocol_bans) {
          if (id.empty()) continue;
          if (_stricmp(s, id.c_str()) == 0) {
            matched = true;
            hit_off = 0;
            std::snprintf(hit_name, sizeof(hit_name), "%s", s);
            break;
          }
        }
      }
      if (matched) {
        log("cell NAMHIT a1=%u a2=%u idx=%u tech=%08X name=%s", a1, a2, idx, tech, hit_name);
        force_cell(impl, idx, "name");
      }
      // 顺便把这一格的原始字段打出来（只打一次），用于下次核对偏移。
      static bool tech_dumped = false;
      if (!tech_dumped) {
        tech_dumped = true;
        for (uint32_t base = 0; base < 0x80; base += 8) {
          char line[200] = {};
          size_t used = 0;
          for (uint32_t k = 0; k < 8; ++k) {
            uint32_t v = 0;
            safe_read_u32(tech + base + k * 4u, &v);
            used += (size_t)std::snprintf(line + used, sizeof(line) - used, "%02X:%08X ",
                                          (unsigned)(base + k * 4u), v);
            if (used >= sizeof(line) - 24) break;
          }
          log("techdump %08X +%03X %s", tech, (unsigned)base, line);
        }
      }
    }

    // 2) 兜底识别：格子对象里是否出现"被限制协议的引擎 NameKey"。
    if (is_ptr(tech)) {
      bool matched = false;
      uint32_t hit_off = 0, hit_val = 0;
      for (uint32_t off = 0; off < 0x400 && !matched; off += 4) {
        uint32_t v = 0;
        if (!safe_read_u32(tech + off, &v)) continue;
        if (protocol_key_banned(v)) { matched = true; hit_off = off; hit_val = v; }
      }
      // 再往一层子对象里找
      if (!matched) {
        const uint32_t subs[3] = {0x00u, 0x18u, 0x28u};
        for (int si = 0; si < 3 && !matched; ++si) {
          uint32_t sub = 0;
          if (!safe_read_u32(tech + subs[si], &sub) || !is_ptr(sub)) continue;
          for (uint32_t off = 0; off < 0x400 && !matched; off += 4) {
            uint32_t v = 0;
            if (!safe_read_u32(sub + off, &v)) continue;
            if (protocol_key_banned(v)) {
              matched = true;
              hit_off = 0x10000u + off;
              hit_val = v;
            }
          }
        }
      }
      if (matched) {
        log("cell KEYHIT a1=%u a2=%u idx=%u tech=%08X off=%08X key=%08X", a1, a2, idx, tech,
            hit_off, hit_val);
        if (have_impl) force_cell(impl, idx, "key");
      }
    }

    // 2) 次级识别：格子对象里出现被限制协议的模块/模板指针。
    if (g_ban_ptr_n > 0 && is_ptr(tech)) {
      bool matched = false;
      uint32_t hit_off = 0, hit_val = 0;
      // 自身 0..0x300
      for (uint32_t off = 0; off < 0x300 && !matched; off += 4) {
        uint32_t v = 0;
        if (!safe_read_u32(tech + off, &v)) continue;
        if (is_ban_ptr(v)) { matched = true; hit_off = off; hit_val = v; }
      }
      // 一层子对象（+0x00 / +0x18）里再找一遍
      if (!matched) {
        const uint32_t subs[2] = {0x00u, 0x18u};
        for (int si = 0; si < 2 && !matched; ++si) {
          uint32_t sub = 0;
          if (!safe_read_u32(tech + subs[si], &sub) || !is_ptr(sub)) continue;
          for (uint32_t off = 0; off < 0x300 && !matched; off += 4) {
            uint32_t v = 0;
            if (!safe_read_u32(sub + off, &v)) continue;
            if (is_ban_ptr(v)) { matched = true; hit_off = 0x10000u + off; hit_val = v; }
          }
        }
      }
      if (matched) {
        log("cell HIT a1=%u a2=%u idx=%u tech=%08X off=%08X val=%08X",
            a1, a2, idx, tech, hit_off, hit_val);
        if (have_impl) apply_cell_lock(impl, idx, "banptr");
      }
    }

    // 一次性：把 15 格的 alpha(+0x08) 与 impl+0x20 的锁定字节打出来对照
    static bool alpha_dumped = false;
    if (!alpha_dumped && have_impl) {
      alpha_dumped = true;
      for (uint32_t i = 0; i < 15; ++i) {
        uint32_t obj = 0;
        unsigned char lb = 0xFF;
        uint32_t alpha = 0;
        if (safe_read_u32(impl + 0x20u + i * 4u, &obj) && is_ptr(obj)) {
          safe_read_u8(obj + 0x18, &lb);
          safe_read_u32(obj + 0x08, &alpha);
        }
        log("cell state idx=%u lockbyte=%u alpha=%08X total=%08X", i, (unsigned)lb, alpha, impl);
      }
      // 状态对象全量 dump：对比"可用格"与"锁定格"，找出决定锁图的那个字段。
      for (uint32_t i = 0; i < 15; ++i) {
        uint32_t obj = 0;
        if (!safe_read_u32(impl + 0x20u + i * 4u, &obj) || !is_ptr(obj)) continue;
        for (uint32_t base = 0; base < 0x80; base += 12) {
          char line[200] = {};
          size_t used = 0;
          for (uint32_t k = 0; k < 12 && base + k * 4u < 0x80; ++k) {
            uint32_t v = 0;
            safe_read_u32(obj + base + k * 4u, &v);
            used += (size_t)std::snprintf(line + used, sizeof(line) - used, "%02X:%08X ",
                                          (unsigned)(base + k * 4u), v);
            if (used >= sizeof(line) - 24) break;
          }
          log("cell obj idx=%u %08X %s", i, obj + base, line);
        }
      }
    }


    // 调试实验：只锁一个格子，确认它对应哪一级协议。
    if (have_impl && g_debug_lock_idx >= 0 && !g_debug_lock_done) {
      g_debug_lock_done = 1;
      apply_cell_lock(impl, (uint32_t)g_debug_lock_idx, "debug");
    }


    // impl+0x20 是 15 元素的状态数组，每项对象 +0x18 是"该格是否可用"字节。
    // 0xA1B470 就是读它。一次性把 15 格的可用标志全打出来。
    static bool avail_dumped = false;
    if (!avail_dumped) {
      uint32_t impl = 0;
      if (safe_read_u32(self + 4u, &impl) && is_ptr(impl)) {
        avail_dumped = true;
        log("grid impl=%08X", impl);
        for (uint32_t idx = 0; idx < 15; ++idx) {
          uint32_t obj = 0;
          if (!safe_read_u32(impl + 0x20u + idx * 4u, &obj) || !is_ptr(obj)) {
            log("grid avail idx=%u col=%u row=%u obj=0", idx, idx % 5, idx / 5);
      continue;
    }
          uint32_t w18 = 0;
          safe_read_u32(obj + 0x18, &w18);
          uint32_t techp = 0;
          safe_read_u32(obj + 0x08, &techp);
          log("grid avail idx=%u col=%u row=%u obj=%08X +18=%08X +08=%08X",
              idx, idx % 5, idx / 5, obj, w18, techp);
        }
      }
    }

    // 协议名字串（两层）
    for (int depth = 0; depth < 3; ++depth) {
      uint32_t base = tech;
      if (depth > 0) {
        if (!safe_read_u32(tech + (depth == 1 ? 0x00u : 0x18u), &base)) break;
        if (!is_ptr(base)) break;
      }
      for (uint32_t off = 0; off < 0x120; off += 4) {
        uint32_t p = 0;
        if (!safe_read_u32(base + off, &p) || !is_ptr(p)) continue;
        char s[64] = {};
        if (!read_name_ascii(p, s, sizeof(s))) continue;
        if (std::strlen(s) < 8) continue;
        log("grid d%d +%03X -> %08X %s", depth, off, p, s);
      }
    }

    // 原始字段
    for (uint32_t base = 0; base < 0x80; base += 12) {
      char line[200] = {};
      size_t used = 0;
      for (uint32_t k = 0; k < 12 && base + k * 4u < 0x80; ++k) {
        uint32_t v = 0;
        safe_read_u32(tech + base + k * 4u, &v);
        used += (size_t)std::snprintf(line + used, sizeof(line) - used, "%02X:%08X ",
                                      (unsigned)(base + k * 4u), v);
        if (used >= sizeof(line) - 24) break;
      }
      log("grid dump %08X %s", tech + base, line);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// 0xA22480(ecx=this, arg1, arg2) -> eax，ret 8。
// pushfd+pushad 之后的栈布局与 hk_panel_row 完全一致。
static __declspec(naked) void hk_grid_lookup(void) {
  __asm {
    pushfd
    pushad
    mov eax, [esp + 0x2C]        // arg2
    mov edx, [esp + 0x28]        // arg1
    mov ecx, [esp + 0x18]        // this
    push eax
    push edx
    call dword ptr [g_orig_grid_lookup]
    mov [esp + 0x1C], eax        // 写回返回值
    push eax                     // panel_grid_probe(this, a1, a2, tech)
    push dword ptr [esp + 0x30]
    push dword ptr [esp + 0x30]
    push dword ptr [esp + 0x24]
    call panel_grid_probe
    add esp, 16
    popad
    popfd
    ret 8
  }
}

static void install_protocol_grid_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0xA22480u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_grid_lookup),
                     reinterpret_cast<void**>(&g_orig_grid_lookup));
  if (st == MH_OK && g_orig_grid_lookup) {
    MH_EnableHook(fn);
    log("protocol grid hook armed at %p", fn);
  } else {
    log("protocol grid hook failed: %d", (int)st);
    g_orig_grid_lookup = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 0xA1B470：格子可用性判定
//     ecx = owner+0x10（与上面同一对象）
//     arg1, arg2 -> 返回 byte[ 状态对象 + 0x18 ]
// 这个返回值就是 Flash 用来决定"该格是否可点选/是否显示锁定"的依据。
// ---------------------------------------------------------------------------
static void* g_orig_avail = nullptr;
static int g_avail_logged = 0;
static unsigned char g_avail_seen[3][8];   // [a2][a1] 已记录的返回值
static bool g_avail_have[3][8];

// 取"多数锁定格共用"的那张图作为通用锁图。
// 用多数表决而不是取第一个：被限制的那一格此时 state 可能已经是 1，
// 它自己的协议图标如果排在最前面就会被误当成参照图。
static uint32_t ref_cell = 0;
static bool g_need_btn_refresh = false;
static int g_cells_scanned = 0;
static int g_locked_cells_seen = 0;

static void majority_locked_image(uint32_t* ref_out) {
  // 缓存 800ms：面板每次刷新会调用本函数十几次，全量扫描会明显掉帧。
  static uint32_t cached = 0;
  static DWORD cached_at = 0;
  const DWORD now = GetTickCount();
  if (cached && (int32_t)(now - cached_at) < 800) {
    if (ref_out) *ref_out = cached;
    return;
  }
  if (!is_ptr(g_panel_dialog)) return;
  uint32_t imgs[16] = {};
  uint32_t cells[16] = {};
  int n = 0;
  for (uint32_t i = 0; i < 15 && n < 16; ++i) {
    const uint32_t c = g_panel_dialog + 0x110u + i * 0xBCu;
    uint32_t st = 0, img = 0;
    if (!safe_read_u32(c + 0x60u, &st)) continue;
    if (st != 1) continue;
    if (!safe_read_u32(c + 0x5Cu, &img) || !is_ptr(img)) continue;
    imgs[n] = img;
    cells[n] = c;
    ++n;
  }
  if (n == 0) return;
  int best = 0, best_count = 0;
  for (int i = 0; i < n; ++i) {
    int c = 0;
    for (int k = 0; k < n; ++k) {
      if (imgs[k] == imgs[i]) ++c;
    }
    if (c > best_count) {
      best_count = c;
      best = i;
    }
  }
  ref_cell = cells[best];
  cached = imgs[best];
  cached_at = now;
  if (ref_out) *ref_out = cached;
}

// 把某一格做成"科技锁定 + 锁图"：写引擎自己的锁字节、换通用锁图、
// 再把状态推给 Flash。idx 由调用邻接确定，layout 用实测几何。
static void apply_cell_visual_lock(uint32_t idx) {
  if (idx >= 15) return;
  __try {
    // 1) 状态对象的锁字节（引擎判定"不可用"、悬停显示科技锁定的字段）
    //    注意：g_cell_impl 本身就是 impl（= 网格对象的 [self+4]），
    //    这里绝对不能再解引用一次，否则会把模块静态区当状态对象写坏。
    uint32_t obj = 0;
    if (is_ptr(g_cell_impl) &&
        safe_read_u32(g_cell_impl + 0x20u + idx * 4u, &obj) && is_ptr(obj)) {
      unsigned char before = 0;
      safe_read_u8(obj + 0x18, &before);
      safe_write_u8(obj + 0x18, 1);
      if (before != 1) log("visual LOCKBYTE idx=%u obj=%08X %u -> 1", idx, obj, (unsigned)before);
    }
    // 2) 按钮状态 + 图标
    const uint32_t cell = cell_button_addr(idx);
    if (!is_ptr(cell)) return;
    uint32_t vt = 0, st = 0, img = 0;
    if (!safe_read_u32(cell, &vt) || vt != 0x00C7F8D0u) return;
    if (!safe_read_u32(cell + 0x60u, &st) || !safe_read_u32(cell + 0x5Cu, &img)) return;
    if (g_orig_img[idx] == 0 && is_ptr(img)) g_orig_img[idx] = img;
    uint32_t ref = 0;
    majority_locked_image(&ref);
    bool changed = false;
    if (is_ptr(ref) && img != ref) {
      safe_write_u32(cell + 0x5Cu, ref);
      changed = true;
    }
    if (st != 1) {
      safe_write_u32(cell + 0x60u, 1);
      changed = true;
    }
    if (changed) {
      log("visual PADLOCK idx=%u cell=%08X img %08X -> %08X state %u -> 1", idx, cell, img,
          ref, st);
      using FnRefresh = void(__thiscall*)(uint32_t self);
      FnRefresh refresh = reinterpret_cast<FnRefresh>(g_orig_btn_refresh);
      if (refresh) refresh(cell);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static unsigned char __cdecl panel_avail_probe(uint32_t self, uint32_t a1, uint32_t a2,
                                               uint32_t ret) {
  const unsigned char orig = (unsigned char)(ret & 1);
  __try {
    if (a1 > 7 || a2 > 2) return orig;

    // 0xA1B470 返回的是「该格的锁字节」：
    //     impl = [this + 4]
    //     obj  = [impl + 0x20 + (a1 + 5*a2)*4]
    //     ret  = byte [obj + 0x18]        ← 1 = 锁定，0 = 可用
    // （早期日志 idx=0/1 为 0、idx=2..14 为 1，与此完全一致。）
    const uint32_t idx = a1 + 5u * a2;
    // 记录"刚问过哪一格"，供紧随其后的 hasTech 做调用邻接绑定。
    g_last_avail_idx = (int)idx;
    g_last_avail_tick = GetTickCount();

    uint32_t impl = 0, obj = 0;
    const bool geo_ok = safe_read_u32(self + 4u, &impl) && is_ptr(impl) &&
                        safe_read_u32(impl + 0x20u + idx * 4u, &obj) && is_ptr(obj);

    if (!g_cell_locked[idx]) return orig;   // 只处理被限制的格子，其余一律不碰

    // 科技锁定（锁字节 + 锁图 + 状态）统一交给 apply_cell_visual_lock，
    // 避免同一份逻辑散落在两处、也避免重复做昂贵的锁图统计。
    apply_cell_visual_lock(idx);

    if (g_avail_logged < 60) {
      ++g_avail_logged;
      log("avail LOCKED a1=%u a2=%u idx=%u orig=%u -> 1", a1, a2, idx, (unsigned)orig);
    }
    return 1;   // 强制"锁定"
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return orig;
}

static __declspec(naked) void hk_avail(void) {
  __asm {
    pushfd
    pushad
    mov eax, [esp + 0x2C]        // arg2
    mov edx, [esp + 0x28]        // arg1
    mov ecx, [esp + 0x18]        // this
    push eax
    push edx
    call dword ptr [g_orig_avail]
    movzx eax, al
    mov [esp + 0x1C], eax        // 先写回原返回值
    push eax                     // panel_avail_probe(this, a1, a2, ret)
    push dword ptr [esp + 0x30]
    push dword ptr [esp + 0x30]
    push dword ptr [esp + 0x24]
    call panel_avail_probe
    add esp, 16
    // ★ 用 probe 返回的修正值覆盖真实返回值：
    //   被限制的格子强制返回 0（不可用），引擎的点击/悬停判断都会拒绝。
    movzx eax, al
    mov [esp + 0x1C], eax
    popad
    popfd
    ret 8
  }
}

static void install_protocol_avail_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0xA1B470u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_avail),
                     reinterpret_cast<void**>(&g_orig_avail));
  if (st == MH_OK && g_orig_avail) {
    MH_EnableHook(fn);
    log("protocol avail hook armed at %p", fn);
  } else {
    log("protocol avail hook failed: %d", (int)st);
    g_orig_avail = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 0xA69800：设置单个协议条目的"锁定/可用"状态
//     调用者 0xA6A610 在算完 Player::hasTech 之后，把 !hasTech 传进来。
//     这就是引擎自己的科技锁定入口。把 this 与 locked 打出来，就能把面板上
//     每个格子和"是否显示锁定"对上号。
// ---------------------------------------------------------------------------
static void* g_orig_lock_set = nullptr;
static int g_lock_calls = 0;

static void __cdecl panel_lock_probe(uint32_t thisptr, uint32_t locked, uint32_t arg2) {
  __try {
    if (g_lock_calls >= 60) return;
    ++g_lock_calls;
    log("lock this=%08X locked=%u arg2=%u", thisptr, locked & 0xFF, arg2);
    if (!is_ptr(thisptr)) return;
    // 顺着 +0x48 找到它引用的数据，再往下挖名字串，识别是哪个协议。
    for (int depth = 0; depth < 3; ++depth) {
      uint32_t base = thisptr;
      if (depth > 0) {
        if (!safe_read_u32(thisptr + (depth == 1 ? 0x48u : 0x08u), &base)) break;
        if (!is_ptr(base)) break;
      }
      for (uint32_t off = 0; off < 0x120; off += 4) {
        uint32_t p = 0;
        if (!safe_read_u32(base + off, &p) || !is_ptr(p)) continue;
        char s[64] = {};
        if (!read_name_ascii(p, s, sizeof(s))) continue;
        if (std::strlen(s) < 6) continue;
        log("lock d%d +%03X -> %08X %s", depth, off, p, s);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// 0xA69800(ecx=this, arg1=locked, arg2) ret 8
static __declspec(naked) void hk_lock_set(void) {
  __asm {
    pushfd
    pushad
    // 先转发原函数，保持流程完全不变
    mov eax, [esp + 0x2C]        // arg2
    mov edx, [esp + 0x28]        // arg1 = locked
    mov ecx, [esp + 0x18]        // this
    push eax
    push edx
    call dword ptr [g_orig_lock_set]
    mov [esp + 0x1C], eax
    push eax                     // panel_lock_probe(this, locked, arg2)
    push dword ptr [esp + 0x30]
    push dword ptr [esp + 0x30]
    push dword ptr [esp + 0x24]
    call panel_lock_probe
    add esp, 16
    popad
    popfd
    ret 8
  }
}

static void install_protocol_lock_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0xA69800u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_lock_set),
                     reinterpret_cast<void**>(&g_orig_lock_set));
  if (st == MH_OK && g_orig_lock_set) {
    MH_EnableHook(fn);
    log("protocol lock hook armed at %p", fn);
  } else {
    log("protocol lock hook failed: %d", (int)st);
    g_orig_lock_set = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 0xA25F20：把"该格的可用状态"写到 Flash 元素上，真正决定图标外观。
//     state = [this + 0x60]   0 = available, 1 = locked, 2 = owned
//     0xC78FA8 是状态名表：{ "available", "locked", "owned" }
//     随后 push "SetAvailability" 调用 0x953F60 设置到 Flash 元件。
// 之前改的 impl+0x20 那层只是语义（提示 + 拦截点击），外观不读它，所以
// 图标一直不变。这里直接改 [this+0x60] = 1，锁图和置灰就会由引擎自己画。
// ---------------------------------------------------------------------------
static void* g_orig_avail_set = nullptr;
static int g_avail_set_logged = 0;

// __thiscall(this)，无参数，ret。
static void __cdecl avail_set_pre(uint32_t self) {
  __try {
    if (!is_ptr(self)) return;
    uint32_t state = 0, felem = 0;
    safe_read_u32(self + 0x60u, &state);
    safe_read_u32(self + 8u, &felem);

    bool hit = false;
    uint32_t hit_off = 0, hit_val = 0;
    for (uint32_t off = 0; off < 0x200; off += 4) {
      uint32_t v = 0;
      if (!safe_read_u32(self + off, &v)) continue;
      if (is_ban_ptr(v)) { hit = true; hit_off = off; hit_val = v; break; }
    }

    if (g_avail_set_logged < 100) {
      ++g_avail_set_logged;
      log("avset self=%08X state=%u elem=%08X ban=%d off=%02X val=%08X", self, state, felem,
          hit ? 1 : 0, hit_off, hit_val);
    }

    if (hit && state != 1) {
      safe_write_u32(self + 0x60u, 1);
      log("avset FORCE self=%08X idx=%u state %u -> 1 (locked)", self, hit_off, state);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static __declspec(naked) void hk_avail_set(void) {
  __asm {
    pushfd
    pushad
    mov ecx, [esp + 0x18]        // this
    push ecx
    call avail_set_pre           // 先按需把状态改成 locked
    add esp, 4
    mov ecx, [esp + 0x18]        // 原函数自己用 ecx 取 this
    call dword ptr [g_orig_avail_set]
    popad
    popfd
    ret
  }
}

static void install_protocol_avail_set_hook() {
  static bool tried = false;
  // 该函数从未被协议面板触发，且目标属于另一类对象（退出战役时崩溃）。
  // 保留代码但不再安装，避免影响稳定性。
  if (tried) return;
  tried = true;
  log("protocol avail-set hook disabled (not used by panel)");
  return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0xA25F20u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_avail_set),
                     reinterpret_cast<void**>(&g_orig_avail_set));
  if (st == MH_OK && g_orig_avail_set) {
    MH_EnableHook(fn);
    log("protocol avail-set hook armed at %p", fn);
  } else {
    log("protocol avail-set hook failed: %d", (int)st);
    g_orig_avail_set = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 0x953F60：APT/Flash 的"按属性名+参数设置元件属性"分发器。
//     call 前栈上依次 push: 目标元素, 属性名(cstr*), 参数个数, ...参数
// 协议面板打开时，每格的锁图状态一定从这里流过。只做观察，不改流程。
// ---------------------------------------------------------------------------
static void* g_orig_flash_set = nullptr;
static int g_flash_logged = 0;
static uint32_t g_flash_last[8];
static int g_flash_last_n = 0;

// __cdecl：参数在栈上，返回后由调用者 add esp。
static void __cdecl flash_set_probe(uint32_t elem, uint32_t name_ptr, uint32_t argc) {
  __try {
    if (g_flash_logged >= 200) return;
    char nm[64] = {};
    if (!read_name_ascii(name_ptr, nm, sizeof(nm))) return;
    if (std::strlen(nm) < 4) return;
    // 只关心与可用性/外观有关的属性
    const bool interesting =
        std::strstr(nm, "Availability") || std::strstr(nm, "Revealed") ||
        std::strstr(nm, "Selected") || std::strstr(nm, "Visibility") ||
        std::strstr(nm, "Dependence") || std::strstr(nm, "Color");
    if (!interesting) return;
    ++g_flash_logged;
    // 参数紧跟 argc 之后（argc 是最后一个 push）
    const uint32_t* args = reinterpret_cast<const uint32_t*>(0);
    (void)args;
    char argbuf[96] = {};
    // 用 __try 保护，取最多 3 个参数
    __try {
      const uint32_t* sp = &argc;
      for (int i = 1; i <= 3 && (uint32_t)i <= argc; ++i) {
        uint32_t v = sp[i];
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), " a%d=%08X", i, v);
        std::strncat(argbuf, tmp, sizeof(argbuf) - std::strlen(argbuf) - 1);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    log("flashset elem=%08X %s argc=%u%s", elem, nm, argc, argbuf);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// __thiscall __fastcall(ecx=dispatcher, edx=?)：调用约定未知，用 naked 取栈参数。
// 目标处 call 前连续 push 了 目标/属性名/参数个数，因此 [esp+4]=elem,
// [esp+8]=name, [esp+0xC]=argc（进入函数后含返回地址）。
static __declspec(naked) void hk_flash_set(void) {
  __asm {
    pushfd
    pushad
    push dword ptr [esp + 0x2C]  // argc
    push dword ptr [esp + 0x2C]  // name
    push dword ptr [esp + 0x2C]  // elem
    call flash_set_probe
    add esp, 12
    popad
    popfd
    // 原先的 push 都还在，直接进原函数
    mov eax, dword ptr [g_orig_flash_set]
    jmp eax
  }
}

static void install_protocol_flash_set_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0x953F60u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_flash_set),
                     reinterpret_cast<void**>(&g_orig_flash_set));
  if (st == MH_OK && g_orig_flash_set) {
    MH_EnableHook(fn);
    log("protocol flash-set hook armed at %p", fn);
  } else {
    log("protocol flash-set hook failed: %d", (int)st);
    g_orig_flash_set = nullptr;
  }
}


// ---------------------------------------------------------------------------
// Apt 动态调用器：0x00953F60(this, "方法名", 参数...)
//
// 面板里所有对 Flash 的调用都从这里过（"SetNumberVisibility" / "GetHelpBox" /
// "OnRollOut" 都是它的调用名）。这里只做记录、然后把栈原样交给原函数，
// 因此不需要知道参数个数，也不会改变任何行为。
static void* g_orig_apt_invoke = nullptr;
static uint32_t g_invoke_seen_ptr = 0;
static uint32_t g_invoke_logged = 0;

static bool invoke_interesting(const char* n) {
  static const char* keys[] = {"ock", "vail", "nable", "urcha", "ank", "ech",
                               "oint", "isib", "tate", "itle", "old",  "an"};
  for (const char* k : keys) {
    if (std::strstr(n, k)) return true;
  }
  return false;
}

static void __cdecl invoke_probe(uint32_t self, uint32_t namep, uint32_t a2, uint32_t a3,
                                 uint32_t a4) {
  __try {
    if (!is_ptr(namep)) return;
    char name[64] = {};
    if (!read_name_ascii(namep, name, sizeof(name))) return;
    if (!name[0]) return;
    const bool interesting = invoke_interesting(name);
    if (!interesting && namep == g_invoke_seen_ptr) return;
    g_invoke_seen_ptr = namep;
    if (g_invoke_logged >= 400) return;
    ++g_invoke_logged;
    log("apt call %-28s a2=%08X a3=%08X a4=%08X self=%08X", name, a2, a3, a4, self);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// 只记录、不接管：popad 之后直接跳原函数，栈完全没动过。
static __declspec(naked) void hk_apt_invoke(void) {
  __asm {
    pushfd
    pushad
    mov eax, [esp + 0x34]        // arg4
    mov ecx, [esp + 0x30]        // arg3
    mov edx, [esp + 0x2C]        // arg2
    mov edi, [esp + 0x28]        // arg1 = 方法名
    mov esi, [esp + 0x18]        // this（保存的 ecx）
    push eax
    push ecx
    push edx
    push edi
    push esi
    call invoke_probe
    add esp, 20
    popad
    popfd
    jmp dword ptr [g_orig_apt_invoke]
  }
}

static void install_protocol_invoke_hook() {
  static bool tried = false;
  if (tried || !loaded_module() || uprising_active()) return;
  tried = true;
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
  void* fn = reinterpret_cast<void*>(loaded_module() + (0x953F60u - kModBase));
  st = MH_CreateHook(fn, reinterpret_cast<void*>(&hk_apt_invoke),
                     reinterpret_cast<void**>(&g_orig_apt_invoke));
  if (st == MH_OK && g_orig_apt_invoke) {
    MH_EnableHook(fn);
    log("apt invoke hook armed at %p", fn);
  } else {
    log("apt invoke hook failed: %d", (int)st);
    g_orig_apt_invoke = nullptr;
  }
}

static bool template_has_exact_id(uint32_t tmpl, const char* want) {
  if (!is_ptr(tmpl) || !want || !want[0]) return false;
  const size_t n = std::strlen(want);
  if (n < 4 || n > 80) return false;
  char buf[96] = {};
  auto exact = [&](uint32_t addr) {
    if (!safe_read_bytes(addr, buf, n + 1)) return false;
    return buf[n] == 0 && std::memcmp(buf, want, n) == 0;
  };
  for (uint32_t off = 0; off <= 0x180; off += 4) {
    if (exact(tmpl + off)) return true;
    uint32_t p = 0;
    if (!safe_read_u32(tmpl + off, &p) || !is_ptr(p)) continue;
    if (exact(p)) return true;
    uint32_t p2 = 0;
    if (safe_read_u32(p, &p2) && is_ptr(p2) && exact(p2)) return true;
  }
  return false;
}

static uint32_t call_find_template(uint32_t factory, const char* interned) {
  using Fn = uint32_t(__thiscall*)(uint32_t, const char*);
  uint32_t tmpl = 0;
  __try {
    tmpl = reinterpret_cast<Fn>(va_of(kFnFindTemplate))(factory, interned);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    tmpl = 0;
  }
  return is_ptr(tmpl) ? tmpl : 0;
}

// The finder hashes the exact string pointer it stored at load time.
// Search process memory for that copy and look the pointer up.
// ⚠️ 这是全进程内存扫描：一次可能几十毫秒到几百毫秒，跑在渲染线程上就是卡帧，
//    连续几个不存在的 id 会让游戏看起来像死掉（进而被判定为崩溃）。
//    所以加了 12ms 时间预算 + 每局最多 4 次，超了就直接放弃这次兜底。
static uint32_t find_template_by_interned_name(uint32_t factory, const char* id) {
  const size_t n = std::strlen(id);
  if (n < 4 || n > 80) return 0;
  static int s_scans = 0;
  static DWORD s_last_scan = 0;
  const DWORD now_tick = GetTickCount();
  if (static_cast<int32_t>(now_tick - s_last_scan) > 30000) s_scans = 0;  // 30 秒后重新给额度
  s_last_scan = now_tick;
  if (s_scans >= 4) return 0;
  ++s_scans;
  LARGE_INTEGER freq{}, t0{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  auto out_of_time = [&]() {
    if (!freq.QuadPart) return false;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double ms = (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    return ms > 12.0;
  };
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  auto* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
  auto* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
  int tried = 0;
  while (p < end && tried < 24) {
    if (out_of_time()) {
      log("summon name %s: interned-name scan hit its time budget", id);
      break;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
    auto* base = static_cast<uint8_t*>(mbi.BaseAddress);
    const size_t size = mbi.RegionSize;
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool readable = mbi.State == MEM_COMMIT &&
                          (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                          (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                           prot == PAGE_EXECUTE_WRITECOPY);
    if (readable && size >= n + 1 && size <= 32u * 1024u * 1024u) {
      const uint8_t* lim = base + size - (n + 1);
      for (const uint8_t* q = base; q <= lim && tried < 24;) {
        const void* hit = std::memchr(q, id[0], static_cast<size_t>(lim - q) + 1);
        if (!hit) break;
        const auto* c = static_cast<const uint8_t*>(hit);
        if (std::memcmp(c, id, n) == 0 && c[n] == 0) {
          const uint32_t tmpl = call_find_template(factory, reinterpret_cast<const char*>(c));
          ++tried;
          if (tmpl) return tmpl;
        }
        q = c + 1;
      }
    }
    auto* next = base + size;
    if (next <= p) break;
    p = next;
  }
  if (!tried) log("summon name %s is not loaded", id);
  else log("summon name %s loaded %d times but not registered", id, tried);
  return 0;
}

// The game's finder hashes the string pointer, not the characters. Walk the
// buckets and compare the names it stored.
static uint32_t lookup_template_official(uint32_t factory, const char* id) {
  uint32_t buckets = 0;
  uint32_t count = 0;
  if (!safe_read_u32(factory + 0x2C, &buckets) || !is_ptr(buckets)) return 0;
  if (!safe_read_u32(factory + 0x30, &count) || count == 0 || count > 100000) return 0;
  const size_t n = std::strlen(id);
  if (n < 4 || n > 80) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = 0;
    if (!safe_read_u32(buckets + i * 4, &node)) continue;
    for (int guard = 0; is_ptr(node) && guard < 64; ++guard) {
      uint32_t key = 0;
      uint32_t val = 0;
      uint32_t next = 0;
      if (!safe_read_u32(node, &key)) break;
      if (is_ptr(key)) {
        char buf[96] = {};
        if (safe_read_bytes(key, buf, n + 1) && buf[n] == 0 && std::memcmp(buf, id, n) == 0) {
          if (safe_read_u32(node + 4, &val) && is_ptr(val)) return val;
        }
      }
      if (!safe_read_u32(node + 8, &next) || next == node) break;
      node = next;
    }
  }
  return 0;
}

// 类型名是否真的登记在模板库里（大小写敏感，和 FindTemplate 的键一致）。
// 只在名单够大（>500）时才当"负结论"用：名单是扫 ThingFactory 哈希表得来的，
// 比 find_template_by_id 的兜底路径（遍历 4096 个模板 × 每个 0x180 字节，
// 再全进程内存找驻留字符串）便宜几个数量级。
// 名单是不是从哈希表（权威）读出来的。兜底扫描出来的名字可能残缺，
// 不能拿去当"这个类型不存在"的证据。
static bool g_obj_names_from_hash = false;
static bool type_list_from_hash() { return g_obj_names_from_hash; }

static uint32_t find_template_by_id(const char* id);

// ---------------------------------------------------------------------------
// 模板查找：算出 SAGE 对象类型哈希，再调引擎自己的 FindTemplate。
// 依据 Ra3-Copilot 的 pipeline（units.html#pipeline）：
//   代码 = StringHasher.Hash(模板名.lower())，是 SuperFastHash 变体，不是 CRC32 / FNV。
// 引擎的 FindTemplate(store, edx, hash) 只按哈希查表（那棵哈希树的键其实是名字
// 的哈希，不是字符串指针），所以按字符串扫桶永远配不上——特殊单位（红鬼王、
// 将军刽子手……）召唤不出来的根因就在这里：名字明明在内存里，表里却找不到。
// ---------------------------------------------------------------------------
constexpr uint32_t kFnTemplateFindRva = 0x003E4230;   // FindTemplate  (VA 0x7E4230)
constexpr uint32_t kTemplateStoreRva = 0x008E6C58;    // ThingFactory 全局 (VA 0xCE6C58)

static uint32_t sage_hash_lower(const char* s) {
  if (!s || !s[0]) return 0;
  // 支持直接写十六进制代码
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && std::strlen(s) == 10) {
    uint32_t v = 0;
    for (int i = 2; i < 10; ++i) {
      const char c = s[i];
      uint32_t d;
      if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
      else return 0;
      v = (v << 4) | d;
    }
    return v;
  }
  char low[128] = {};
  size_t len = 0;
  for (; s[len] && len < sizeof(low) - 1; ++len) {
    char c = s[len];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    low[len] = c;
  }
  const unsigned char* b = reinterpret_cast<const unsigned char*>(low);
  const uint32_t n = (uint32_t)len;
  uint32_t h = n;
  uint32_t i = 0;
  for (uint32_t k = 0; k < n / 4; ++k, i += 4) {
    h += (uint32_t)(b[i] | (b[i + 1] << 8));
    const uint32_t tmp = ((uint32_t)(b[i + 2] | (b[i + 3] << 8))) ^ (h << 5);
    h ^= tmp << 11;
    h += h >> 11;
  }
  switch (n & 3) {
    case 3:
      h += (uint32_t)(b[i] | (b[i + 1] << 8));
      h ^= (h ^ ((uint32_t)b[i + 2] << 2)) << 16;
      h += h >> 11;
      break;
    case 2:
      h += (uint32_t)(b[i] | (b[i + 1] << 8));
      h ^= h << 11;
      h += h >> 17;
      break;
    case 1:
      h += b[i];
      h ^= h << 10;
      h += h >> 1;
      break;
    default:
      break;
  }
  h ^= h << 3;
  h += h >> 5;
  h ^= h << 2;
  h += h >> 15;
  h ^= h << 10;
  return h;
}

static bool template_plausible(uint32_t tmpl) {
  if (!is_ptr(tmpl)) return false;
  uint32_t vt = 0;
  const uint32_t base = module_base();
  if (!safe_read_u32(tmpl, &vt) || !is_ptr(vt)) return false;
  return base && vt >= base && vt < base + 0x940000u;
}

static uint32_t module_image_size() {
  const uint32_t base = module_base();
  if (!base) return 0;
  __try {
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// ---- 自动定位 FindTemplate 与 ThingFactory 全局 ----
// 照 Copilot（symres.cpp）的做法：认函数用字节签名，再由
// `mov ecx,[imm32]; call FindTemplate` 这类代码里取出现次数最多的 imm32
// 当 ThingFactory 全局。这样不依赖某个版本的具体 RVA（本机读 0xCE6C58 是 0，
// 说明那个地址对这个构建不成立）。
static constexpr uint8_t kSigFindTemplate[] = {0x83, 0xEC, 0x08, 0x56, 0x8D, 0x71, 0x28,
                                               0x8D, 0x44, 0x24, 0x10, 0x50, 0x8D, 0x4C,
                                               0x24, 0x08, 0x51, 0x8B, 0xCE, 0xE8};

static uint32_t g_tmpl_find_fn = 0;      // FindTemplate 的绝对地址
static uint32_t g_tmpl_factory_slot = 0; // ThingFactory 全局变量的地址
static uint32_t g_tmpl_store = 0;        // 上面那个全局里存的对象
static int g_tmpl_hits = 0;
static bool g_tmpl_discovered = false;

// 一个裸调用 + SEH 的小壳（__try 里不能有需要析构的局部对象，所以别把 lambda 塞进来）
static uint32_t call_find_template(uint32_t fn, uint32_t store, uint32_t hash) {
  using Fn = uint32_t(__fastcall*)(void*, void*, uint32_t);
  uint32_t tmpl = 0;
  __try {
    tmpl = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(store), nullptr, hash);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    tmpl = 0;
  }
  return tmpl;
}

// 在已提交可读的区域里扫一段字节，避免踩到未提交页（直接读会 AV）。
static bool image_region_readable(const MEMORY_BASIC_INFORMATION& mbi) {
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & PAGE_NOACCESS) return false;
  if (mbi.Protect & (PAGE_GUARD)) return false;
  return true;
}

static int scan_image_signature(uint32_t base, uint32_t size, const uint8_t* sig, int siglen,
                                uint32_t* out, int maxout) {
  int n = 0;
  uint32_t p = base;
  const uint32_t end = base + size;
  while (p < end && n < maxout) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
    uint32_t rs = reinterpret_cast<uint32_t>(mbi.BaseAddress);
    uint32_t rsz = static_cast<uint32_t>(mbi.RegionSize);
    if (!rsz) break;
    uint32_t re = rs + rsz;
    if (re > end) re = end;
    if (image_region_readable(mbi) && re > rs + (uint32_t)siglen) {
      const uint32_t limit = re - (uint32_t)siglen;
      __try {
        for (uint32_t i = rs; i < limit && n < maxout; ++i) {
          if (*reinterpret_cast<const uint8_t*>(i) != sig[0]) continue;
          if (std::memcmp(reinterpret_cast<const void*>(i), sig, siglen) == 0) out[n++] = i;
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    p = re;
  }
  return n;
}

// 找 `8B 0D <imm32> E8 <rel32→fn>` 里出现最多的 imm32（Copilot 用的同一招）
static int scan_factory_slots(uint32_t base, uint32_t size, uint32_t fn, uint32_t* slots,
                              int* counts, int maxout) {
  int nc = 0;
  uint32_t p = base;
  const uint32_t end = base + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
    uint32_t rs = reinterpret_cast<uint32_t>(mbi.BaseAddress);
    uint32_t rsz = static_cast<uint32_t>(mbi.RegionSize);
    if (!rsz) break;
    uint32_t re = rs + rsz;
    if (re > end) re = end;
    if (image_region_readable(mbi) && re > rs + 11) {
      const uint32_t limit = re - 11;
      __try {
        for (uint32_t i = rs; i < limit; ++i) {
          if (*reinterpret_cast<const uint8_t*>(i) != 0x8B) continue;
          if (*reinterpret_cast<const uint8_t*>(i + 1) != 0x0D) continue;
          if (*reinterpret_cast<const uint8_t*>(i + 6) != 0xE8) continue;
          int32_t rel = 0;
          std::memcpy(&rel, reinterpret_cast<const void*>(i + 7), 4);
          if (i + 11 + static_cast<uint32_t>(rel) != fn) continue;
          uint32_t imm = 0;
          std::memcpy(&imm, reinterpret_cast<const void*>(i + 2), 4);
          if (imm < base || imm >= end) continue;
          int k = 0;
          for (; k < nc; ++k) {
            if (slots[k] == imm) {
              ++counts[k];
              break;
            }
          }
          if (k == nc && nc < maxout) {
            slots[nc] = imm;
            counts[nc] = 1;
            ++nc;
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    p = re;
  }
  return nc;
}

static void discover_template_lookup() {
  if (g_tmpl_discovered) return;
  if (!module_base()) return;
  g_tmpl_discovered = true;
  const uint32_t base = module_base();
  const uint32_t size = module_image_size();
  if (!size) {
    log("template: cannot read module headers, hash lookup disabled");
    return;
  }
  // ① 认函数：先看 RVA，再看 Copilot 的字节签名
  const uint32_t rva = base + kFnTemplateFindRva;
  uint8_t probe[sizeof(kSigFindTemplate)] = {};
  bool rva_ok = safe_read_bytes(rva, probe, sizeof(probe)) &&
                std::memcmp(probe, kSigFindTemplate, sizeof(probe)) == 0;
  uint32_t fn = 0;
  if (rva_ok) {
    fn = rva;
    log("template: FindTemplate at RVA 0x%X (signature ok)", kFnTemplateFindRva);
  } else {
    uint32_t hits[8] = {};
    const int nh = scan_image_signature(base, size, kSigFindTemplate,
                                        (int)sizeof(kSigFindTemplate), hits, 8);
    if (nh >= 1) {
      fn = hits[0];
      if (nh > 1) {
        // 同形的有几个：挑被最多 `mov ecx,[imm]` 指向的那个
        uint32_t slots[32] = {};
        int counts[32] = {};
        int best = -1;
        for (int k = 0; k < nh; ++k) {
          const int nc = scan_factory_slots(base, size, hits[k], slots, counts, 32);
          int total = 0;
          for (int i = 0; i < nc; ++i) total += counts[i];
          if (best < 0 || total > best) {
            best = total;
            fn = hits[k];
          }
        }
        log("template: %d signature hits, picked %08X", nh, fn);
      }
    } else {
      log("template: FindTemplate signature not found, hash lookup disabled");
      return;
    }
  }
  if (!fn) return;
  g_tmpl_find_fn = fn;

  // ② 反推 ThingFactory 全局
  uint32_t slots[32] = {};
  int counts[32] = {};
  const int nc = scan_factory_slots(base, size, fn, slots, counts, 32);
  int best = -1;
  for (int i = 0; i < nc; ++i) {
    if (best < 0 || counts[i] > counts[best]) best = i;
  }
  if (best >= 0 && counts[best] >= 2) {
    g_tmpl_factory_slot = slots[best];
    g_tmpl_hits = counts[best];
    uint32_t store = 0;
    if (safe_read_u32(g_tmpl_factory_slot, &store) && is_ptr(store)) g_tmpl_store = store;
  }

  // ③ 验证：拿 JapanMCV（一定存在）的哈希去查；查不到就试另两个已知全局
  const uint32_t probe_hash = sage_hash_lower("JapanMCV");
  const uint32_t probe_hash2 = sage_hash_lower("AlliedBarracks");
  uint32_t got = 0;
  if (g_tmpl_store) {
    const uint32_t t = call_find_template(fn, g_tmpl_store, probe_hash);
    if (template_plausible(t)) got = t;
  }
  if (got) {
    log("template: find=%08X factory slot=%08X store=%08X (hits=%d) probe tmpl=%08X", fn,
        g_tmpl_factory_slot, g_tmpl_store, g_tmpl_hits, got);
  } else {
    const uint32_t cand_slots[2] = {base + kTemplateStoreRva, base + kThingFactoryRva};
    uint32_t chosen = 0, chosen_store = 0;
    for (int i = 0; i < 2 && !chosen; ++i) {
      uint32_t s = 0;
      if (!safe_read_u32(cand_slots[i], &s) || !is_ptr(s)) continue;
      const uint32_t t = call_find_template(fn, s, probe_hash);
      if (template_plausible(t)) {
        chosen = cand_slots[i];
        chosen_store = s;
        got = t;
      }
    }
    if (chosen) {
      g_tmpl_factory_slot = chosen;
      g_tmpl_store = chosen_store;
      log("template: find=%08X fallback slot=%08X store=%08X probe tmpl=%08X", fn, chosen,
          chosen_store, got);
    } else {
      log("template: find=%08X but no working store (derived slot=%08X store=%08X)", fn,
          g_tmpl_factory_slot, g_tmpl_store);
    }
  }
  if (g_tmpl_store) {
    const uint32_t t2 = call_find_template(fn, g_tmpl_store, probe_hash2);
    log("template: probe AlliedBarracks %08X -> %08X", probe_hash2, t2);
  }
}

// 引擎模板库里的直接查表。hash 不用小写化——调用方先算好。
static uint32_t template_find_by_hash(uint32_t hash) {
  if (!hash || !module_base()) return 0;
  static int s_fail_logged = 0;
  discover_template_lookup();
  if (!g_tmpl_find_fn) return 0;
  uint32_t store = g_tmpl_store;
  if (!is_ptr(store)) {
    if (!safe_read_u32(g_tmpl_factory_slot, &store) || !is_ptr(store)) return 0;
    g_tmpl_store = store;
  }
  const uint32_t tmpl = call_find_template(g_tmpl_find_fn, store, hash);
  if (!template_plausible(tmpl)) {
    if (s_fail_logged < 8) {
      ++s_fail_logged;
      log("template hash %08X -> %08X (not a template)", hash, tmpl);
    }
    return 0;
  }
  return tmpl;
}

static bool type_id_registered(const char* id) {
  if (!id || !id[0]) return false;
  if (!type_list_from_hash()) return true;  // 名单来自兜底扫描，不可信 → 不下结论
  const std::vector<std::string>& all = object_type_names();
  if (all.size() < 500) return true;
  return std::binary_search(all.begin(), all.end(), std::string(id));
}

static uint32_t find_template_by_id(const char* id) {
  if (!module_base() || !id || !id[0]) return 0;
  // ① 哈希直查（权威路径，Copilot 的 pipeline 同款）：不依赖名单、不扫内存，
  //    名字在内存里但扫桶配不上的特殊单位也能查到。
  {
    const uint32_t hash = sage_hash_lower(id);
    if (hash) {
      const uint32_t by_hash = template_find_by_hash(hash);
      if (by_hash) {
        static int s_logged = 0;
        if (s_logged < 12) {
          ++s_logged;
          log("summon %s: hash 0x%08X -> tmpl %08X", id, hash, by_hash);
        }
        return by_hash;
      }
      log("summon %s: hash 0x%08X not in template library", id, hash);
    }
  }

  uint32_t factory = 0;
  if (!safe_read_u32(module_base() + kThingFactoryRva, &factory) || !is_ptr(factory)) return 0;

  if (!type_id_registered(id)) {
    // 名单里没有 → 这个版本确实没有这个模板。**不要**再走兜底：
    // find_template_by_interned_name 会遍历整个进程内存，一次几十毫秒到几秒，
    // 在渲染线程上连续几个不存在的 id 就会把游戏卡死（看起来像崩溃）。
    log("template %s not in factory list", id);
    return 0;
  }
  const uint32_t named = lookup_template_official(factory, id);
  if (is_ptr(named)) {
    // 护栏：表里有些键指向的不是可 CreateUnit 的模板（技能 / OCL / 脚本对象）。
    // 只看 vtable 是否落在游戏模块里；不在就当没找到，绝不能再拿去造对象（会崩）。
    uint32_t vt = 0;
    const uint32_t base = module_base();
    if (safe_read_u32(named, &vt) && is_ptr(vt) && base && vt >= base && vt < base + 0x940000u) {
      return named;
    }
    log("summon %s: table entry %08X does not look like a template (vt=%08X)", id, named, vt);
  }
  uint32_t tmpl = 0;
  if (safe_read_u32(factory + 0x24, &tmpl) && is_ptr(tmpl)) {
    for (int n = 0; n < 4096 && is_ptr(tmpl); ++n) {
      uint32_t vt = 0;
      if (!safe_read_u32(tmpl, &vt) || !is_ptr(vt)) break;
      if (template_has_exact_id(tmpl, id)) return tmpl;
      uint32_t next = 0;
      if (!safe_read_u32(tmpl + 0xC, &next) || next == tmpl) break;
      tmpl = next;
    }
  }
  return find_template_by_interned_name(factory, id);
}

static const char* mcv_type_id(int faction) {
  if (faction == 1) return "SovietMCV";
  if (faction == 2) return "JapanMCV";
  return "AlliedMCV";
}

static const char* mcv_faction_name(int faction) {
  if (faction == 1) return u8"苏联";
  if (faction == 2) return u8"帝国";
  return u8"盟军";
}

static uint32_t g_rank_player = 0;
static uint32_t g_mcv_player = 0;
static uint32_t g_money_player = 0;
static void label_battle_player(uint32_t player, uint32_t player_id, bool is_local, char* out,
                                size_t out_len);

struct SummonSnap {
  uint32_t player = 0;
  int count = 1;
  int n = 0;
  char ids[64][96] = {};
};

static SummonSnap g_summon_snap;

static bool summon_prepare_impl(uint32_t player, int count, const char* const* type_ids,
                                int type_count, std::string* out_msg) {
  if (!module_base()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }
  if (count < 1) count = 1;
  if (count > 20) count = 20;
  if (!type_ids || type_count <= 0) {
    if (out_msg) *out_msg = u8"请先选择要召唤的单位 / 建筑";
    return false;
  }
  if (type_count > 64) type_count = 64;
  g_summon_snap = SummonSnap{};
  g_summon_snap.player = player;
  g_summon_snap.count = count;
  for (int i = 0; i < type_count; ++i) {
    if (!type_ids[i] || !type_ids[i][0]) continue;
    // 建筑也允许召唤（引擎的 CreateUnit 对建筑模板同样有效）。
    std::snprintf(g_summon_snap.ids[g_summon_snap.n], sizeof(g_summon_snap.ids[0]), "%s",
                  type_ids[i]);
    if (++g_summon_snap.n >= 64) break;
  }
  if (g_summon_snap.n <= 0) {
    if (out_msg) *out_msg = u8"请先选择要召唤的单位 / 建筑";
    return false;
  }
  return true;
}

struct PendingSpawn {
  uint32_t tmpl = 0;
  uint32_t owner = 0;
  uint32_t info = 0;
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  float base_z = 0.f;  // 鼠标落点的地形高度（不含抬升），用来判断"这个对象被抬过"
  int tag = 0;         // 属于第几个请求类型（用于回报"哪种没造出来"）
};

static PendingSpawn g_spawn_q[1280];
static volatile LONG g_spawn_pub = 0;
static volatile LONG g_spawn_made = 0;
static volatile LONG g_spawn_busy = 0;
static int g_spawn_type_made[64] = {};
static HANDLE g_spawn_event = nullptr;

static void ensure_spawn_event() {
  if (!g_spawn_event) g_spawn_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

static uint32_t create_unit_on_this_thread(uint32_t tmpl, uint32_t pos, uint32_t owner,
                                           uint32_t info, bool* threw) {
  using Fn = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
  uint32_t ent = 0;
  if (threw) *threw = false;
  __try {
    ent = reinterpret_cast<Fn>(va_of(kFnCreateUnit))(0, tmpl, pos, owner, info);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ent = 0;
    if (threw) *threw = true;
  }
  return ent;
}

// 召唤出来的对象是"隐形 / 未入世"状态：小地图图标透明、引擎不把它当场上单位。
// Copilot 同源经验（实测同 vtable 逐字节对拍）：
//   实心 = {+0xC8:0, +0x28:0, +0x208:0}，刚召唤 = {+0xC8:0x800, +0x28:0x80000000, +0x208:0x80000000}
// 所以创建后置 +0xBC 的 bit31、清掉这三位；不清就会出现"召唤了但场上什么都看不到"。
static bool g_summon_visible_fix = true;

// 找出对象里所有"位置副本"：本体的 Coord3D、以及一层子对象（Drawable / 物理体，
// 变换矩阵的平移分量也是连续三个 float x,y,z）里的坐标。
// 判据：三个连续 float 的 x/y 落在落点附近、z 接近地形高度。
// 之前只取第一个命中（+0x38）→ 改的不是渲染用的那一份，所以"高度不生效"。
struct PosField {
  uint32_t obj = 0;
  uint32_t off = 0;
  float z = 0.f;
};
constexpr int kMaxPosFields = 8;

static int collect_pos_fields(uint32_t obj, float x, float y, float zg, bool check_z,
                              PosField* out, int max);

// CreateUnit 对地面层对象会把 z 贴回地形高度（气球炸弹因此一出生就"落地"）。
// 一个对象里通常有好几份位置副本（本体 Coord3D、子对象、Drawable 变换矩阵的平移），
// 只改其中一份就"高度不生效"。这里把全部匹配到的坐标副本一起抬起来，
// 返回找到的副本数（>0 才算抬过）。
static int post_create_lift(uint32_t obj, float x, float y, float zg, float want_z) {
  if (!is_ptr(obj) || want_z <= 0.f) return 0;
  PosField f[kMaxPosFields];
  const int n = collect_pos_fields(obj, x, y, zg, true, f, kMaxPosFields);
  if (n <= 0) {
    log("summon: no position copy found on %08X (x=%.1f y=%.1f)", obj, x, y);
    return 0;
  }
  int raised = 0;
  for (int i = 0; i < n; ++i) {
    log("summon: %08X pos copy +0x%X z=%.1f want=%.1f", f[i].obj, f[i].off, f[i].z, want_z);
    if (want_z > f[i].z + 5.f) {
      write_f32(f[i].obj + f[i].off + 8, want_z);
      ++raised;
    }
  }
  log("summon: raised %d/%d position copies on %08X to z=%.1f", raised, n, obj, want_z);
  return n;
}

// ---- 召唤出来的空中单位保持高度 ----
// 气球炸弹的模板描述就是 Slow-Falling Ordnance：它落地/接触地形即引爆。
// 光把创建时的 z 抬高只是把它变成"从高处掉下来炸"，所以这里再加一层：把它们的
// 位置每帧按回召唤高度，让它们真的停在空中；要给它们"投放"，用面板上的「放手」清表。
struct AirHold {
  uint32_t obj = 0;
  uint32_t off = 0;   // 该对象的位置字段偏移
  float z = 0.f;
  uint32_t vt = 0;    // 记下 vtable：内存被复用（换了对象）就不认
  float sx = 0.f;     // 召唤点：对象跑远了就不再托（防写错对象）
  float sy = 0.f;
  DWORD born = 0;
  DWORD probe_at = 0; // 下一次打诊断日志的时间
};
// 一个对象可能有好几份位置副本（本体 + 子对象 / Drawable 的变换矩阵平移分量）。
// 全部都要托住，否则改的那一份不是渲染用的那一份，看起来就是"高度不生效"。
static AirHold g_air_hold[96];
static int g_air_hold_n = 0;
static volatile LONG g_air_hold_on = 1;
static volatile LONG g_air_hold_live = 0;   // 当前被托住的个数（给界面显示）
// 托举表被两条线程碰：Present（每帧托住）和 Destroy 钩子（对象销毁时出表）。
// 用自旋锁而不是 CRITICAL_SECTION：钩子跑在游戏线程上，绝不能在这里阻塞等待。
static volatile LONG g_air_lock = 0;

static bool air_lock_take() {
  for (int i = 0; i < 2000; ++i) {
    if (InterlockedCompareExchange(&g_air_lock, 1, 0) == 0) return true;
    YieldProcessor();
  }
  return false;
}

static void air_lock_drop() { InterlockedExchange(&g_air_lock, 0); }

bool air_hold_enabled_impl() { return g_air_hold_on != 0; }

void set_air_hold_enabled_impl(bool on) {
  InterlockedExchange(&g_air_hold_on, on ? 1 : 0);
  if (!on) {
    g_air_hold_n = 0;
    InterlockedExchange(&g_air_hold_live, 0);
  }
}

int air_hold_count_impl() { return (int)InterlockedCompareExchange(&g_air_hold_live, 0, 0); }

// 「放手」：清掉托举表，它们就开始自然下落（该炸就炸）。
void air_hold_release_all_impl() {
  if (!air_lock_take()) return;
  g_air_hold_n = 0;
  InterlockedExchange(&g_air_hold_live, 0);
  air_lock_drop();
}

void air_hold_forget_impl(uint32_t obj) {
  if (!obj) return;
  if (!air_lock_take()) return;   // 拿不到锁就等下一帧的存活校验把它清掉
  if (g_air_hold_n > 0) {
    int w = 0;
    for (int i = 0; i < g_air_hold_n; ++i) {
      if (g_air_hold[i].obj == obj) continue;
      g_air_hold[w++] = g_air_hold[i];
    }
    g_air_hold_n = w;
    InterlockedExchange(&g_air_hold_live, w);
  }
  air_lock_drop();
}

// need_vt = true：本体/子对象（第一字是 vtable，用来判"内存没被别的对象复用"）。
// need_vt = false：堆里的纯数据块（例如单独分配的 Coord3D），第一字就是 float x，
//                 没有 vtable 可对，只能靠坐标还贴着召唤点来判断。
static void air_hold_add_ex(uint32_t obj, uint32_t off, float z, float sx, float sy,
                            bool need_vt) {
  if (!is_ptr(obj)) return;
  if (!air_lock_take()) return;
  bool found = false;
  for (int i = 0; i < g_air_hold_n; ++i) {
    if (g_air_hold[i].obj == obj && g_air_hold[i].off == off) {
      g_air_hold[i].z = z;
      g_air_hold[i].sx = sx;
      g_air_hold[i].sy = sy;
      found = true;
      break;
    }
  }
  if (!found && g_air_hold_n < (int)(sizeof(g_air_hold) / sizeof(g_air_hold[0]))) {
    AirHold& h = g_air_hold[g_air_hold_n++];
    h.obj = obj;
    h.off = off;
    h.z = z;
    h.vt = 0;
    if (need_vt) safe_read_u32(obj, &h.vt);
    h.sx = sx;
    h.sy = sy;
    h.born = GetTickCount();
    h.probe_at = h.born + 1000;
    InterlockedExchange(&g_air_hold_live, g_air_hold_n);
  }
  air_lock_drop();
}

static void air_hold_add(uint32_t obj, uint32_t off, float z, float sx, float sy) {
  air_hold_add_ex(obj, off, z, sx, sy, true);
}

static void air_hold_add_raw(uint32_t obj, uint32_t off, float z, float sx, float sy) {
  air_hold_add_ex(obj, off, z, sx, sy, false);
}

// ---- 追踪扫描 ----
// 创建当帧只有本体几份坐标；真正驱动"下落 + 落地即爆"的那一份往往在**另一个堆块**里，
// 由 Thing 里的指针指向（Coord3D 是纯 12 字节数据、没有 vtable，之前按"子对象必须有
// vtable"筛，正好把它漏掉了）。这里改成沿指针下钻（最多 3 层、只跟堆指针），
// 每隔一段时间再扫一次，把新冒出来的坐标副本也收进托举表。
struct PosHunt {
  uint32_t obj = 0;
  float sx = 0.f;
  float sy = 0.f;
  float sz = 0.f;
  int stage = 0;
  DWORD next_at = 0;
  // 上一次采样看到的坐标副本：用来判"哪一份在往下掉"（真正驱动下落的那个）。
  int prev_n = 0;
  uint32_t prev_obj[8] = {};
  uint32_t prev_off[8] = {};
  float prev_z[8] = {};
};
static PosHunt g_hunt[16];
static int g_hunt_n = 0;
static volatile LONG g_hunt_lock = 0;

// 采样时刻（毫秒，相对召唤）。前密后疏：前 2 秒是对象建子结构的时候。
static const DWORD kHuntDelays[] = {0, 120, 320, 700, 1400, 2400, 3800, 5600};

static void schedule_hunt(uint32_t obj, float x, float y, float z) {
  if (!is_ptr(obj)) return;
  if (InterlockedCompareExchange(&g_hunt_lock, 1, 0) != 0) return;
  for (int i = 0; i < g_hunt_n; ++i) {
    if (g_hunt[i].obj == obj) {
      g_hunt[i].sx = x;
      g_hunt[i].sy = y;
      g_hunt[i].sz = z;
      g_hunt[i].stage = 0;
      g_hunt[i].next_at = GetTickCount();
      InterlockedExchange(&g_hunt_lock, 0);
      return;
    }
  }
  if (g_hunt_n < (int)(sizeof(g_hunt) / sizeof(g_hunt[0]))) {
    PosHunt& h = g_hunt[g_hunt_n++];
    h.obj = obj;
    h.sx = x;
    h.sy = y;
    h.sz = z;
    h.stage = 0;
    h.next_at = GetTickCount();
  }
  InterlockedExchange(&g_hunt_lock, 0);
}

static void run_due_hunts();

// ---- 高频夹持线程 ----
// 只在 Present 里写是不够的：Present 在「物理更新 → 渲染」之后，下一次物理更新
// 会把 z 改回去，渲染用的又是物理那份，看起来就是"高度不生效"。所以再开一根
// 8ms 的线程反复按高度，赶在物理更新和绘制之间把值压回去。
static volatile LONG g_air_thread_started = 0;
static volatile LONG g_air_thread_stop = 0;
static volatile LONG g_air_thread_live = 0;
static HANDLE g_air_thread = nullptr;

static DWORD WINAPI air_hold_thread_proc(LPVOID);

static void ensure_air_hold_thread() {
  if (InterlockedCompareExchange(&g_air_thread_started, 1, 0) != 0) return;
  InterlockedExchange(&g_air_thread_stop, 0);
  g_air_thread = CreateThread(nullptr, 0, air_hold_thread_proc, nullptr, 0, nullptr);
  if (!g_air_thread) {
    InterlockedExchange(&g_air_thread_started, 0);
    log("air hold: pin thread failed to start");
  } else {
    log("air hold: pin thread started (8ms)");
  }
}

// 卸载前必须先把它停掉：DLL 被 FreeLibrary 之后，这根线程还在往游戏内存里写，
// 就是"退出时崩在 dll_unloaded"那一类。
void air_hold_stop_thread() {
  if (InterlockedCompareExchange(&g_air_thread_started, 0, 0) == 0) return;
  InterlockedExchange(&g_air_thread_stop, 1);
  for (int i = 0; i < 100; ++i) {
    if (InterlockedCompareExchange(&g_air_thread_live, 0, 0) == 0) break;
    Sleep(5);
  }
  if (g_air_thread) {
    if (InterlockedCompareExchange(&g_air_thread_live, 0, 0) != 0) {
      TerminateThread(g_air_thread, 0);
    }
    WaitForSingleObject(g_air_thread, 200);
    CloseHandle(g_air_thread);
    g_air_thread = nullptr;
  }
  InterlockedExchange(&g_air_thread_started, 0);
  InterlockedExchange(&g_air_thread_stop, 0);
}


// 找出对象里所有"位置副本"（实现）：本体 + 一层子对象。
static int collect_pos_fields(uint32_t obj, float x, float y, float zg, bool check_z,
                              PosField* out, int max) {
  int n = 0;
  auto scan = [&](uint32_t o, uint32_t limit) {
    for (uint32_t off = 0; off + 12 <= limit && n < max; off += 4) {
      float a = 0.f, b = 0.f, c = 0.f;
      if (!safe_read_bytes(o + off, &a, sizeof(a))) continue;
      if (!(a > -60000.f && a < 60000.f)) continue;
      if (std::fabs(a - x) > 24.f) continue;
      if (!safe_read_bytes(o + off + 4, &b, sizeof(b))) continue;
      if (!(b > -60000.f && b < 60000.f)) continue;
      if (std::fabs(b - y) > 24.f) continue;
      if (!safe_read_bytes(o + off + 8, &c, sizeof(c))) continue;
      if (!(c > -20000.f && c < 20000.f)) continue;
      // 位置副本的 z 就是刚生成时的地形高度。放 3.0 是为了排掉"旁边别的单位"：
      // 上一版放到 400，连 200 米外高度 247 的另一支部队都被当成副本写过。
      // 延迟复查时本体已经被抬到高空了，所以那时不再看 z。
      if (check_z && std::fabs(c - zg) > 3.f) continue;
      // 去重（同一对象同偏移）
      bool dup = false;
      for (int i = 0; i < n; ++i) {
        if (out[i].obj == o && out[i].off == off) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      out[n].obj = o;
      out[n].off = off;
      out[n].z = c;
      ++n;
    }
  };
  scan(obj, 0x600);
  const uint32_t base = module_base();
  // 一层子对象：vtable 落在游戏模块内的才算（排除容器/字符串指针）。
  for (uint32_t p = 0; p + 4 <= 0x600 && n < max; p += 4) {
    uint32_t q = 0;
    if (!safe_read_u32(obj + p, &q) || !is_ptr(q)) continue;
    uint32_t vt = 0;
    if (!safe_read_u32(q, &vt) || !is_ptr(vt)) continue;
    if (!base || vt < base || vt >= base + 0x940000u) continue;
    scan(q, 0x400);
  }
  return n;
}

// 沿指针下钻找坐标副本。堆里那些"纯数据块"（例如单独分配的 Coord3D）没有 vtable，
// 只能靠"块里有没有一组 x/y 对得上召唤点"来认。上限 256 个块、每块看前 0x800 字节，
// 只在采样时刻跑（8ms 线程里不是每帧都跑）。
constexpr int kDeepMaxBlocks = 256;
constexpr int kDeepScanBytes = 0x800;
constexpr int kDeepPtrWords = 0x300;

static bool deep_mark(uint32_t* seen, int* nseen, uint32_t v) {
  for (int i = 0; i < *nseen; ++i) {
    if (seen[i] == v) return false;
  }
  if (*nseen < kDeepMaxBlocks) seen[(*nseen)++] = v;
  return true;
}

// 只看坐标，不看 z 上界：对象可能已经掉下去一截了。
static int collect_pos_fields_deep(uint32_t root, float x, float y, float zlo, float zhi,
                                   PosField* out, int max, int* out_blocks) {
  if (out_blocks) *out_blocks = 0;
  if (!is_ptr(root) || !out || max <= 0) return 0;
  const uint32_t base = module_base();
  const uint32_t size = module_image_size();
  static uint32_t seen[kDeepMaxBlocks];
  int nseen = 0;
  uint32_t queue[kDeepMaxBlocks] = {};
  int qn = 0;
  queue[qn++] = root;
  deep_mark(seen, &nseen, root);
  int n = 0;
  for (int qi = 0; qi < qn; ++qi) {
    const uint32_t o = queue[qi];
    // ① 找三元组
    for (uint32_t off = 0; off + 12 <= kDeepScanBytes && n < max; off += 4) {
      float a = 0.f, b = 0.f, c = 0.f;
      if (!safe_read_bytes(o + off, &a, sizeof(a))) break;
      if (!(a > -60000.f && a < 60000.f) || std::fabs(a - x) > 24.f) continue;
      if (!safe_read_bytes(o + off + 4, &b, sizeof(b))) break;
      if (!(b > -60000.f && b < 60000.f) || std::fabs(b - y) > 24.f) continue;
      if (!safe_read_bytes(o + off + 8, &c, sizeof(c))) break;
      if (!(c > zlo && c < zhi)) continue;
      bool dup = false;
      for (int i = 0; i < n; ++i) {
        if (out[i].obj == o && out[i].off == off) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      out[n].obj = o;
      out[n].off = off;
      out[n].z = c;
      ++n;
    }
    // ② 跟指针（只跟堆里的，跳过模块自身的数据段/代码段）
    for (uint32_t p = 0; p + 4 <= kDeepPtrWords && qn < kDeepMaxBlocks; p += 4) {
      uint32_t v = 0;
      if (!safe_read_u32(o + p, &v)) break;
      if (v < 0x01000000u || v >= 0x7FF00000u || (v & 3u) != 0) continue;
      if (base && size && v >= base && v < base + size) continue;
      if (!deep_mark(seen, &nseen, v)) continue;
      queue[qn++] = v;
    }
  }
  if (out_blocks) *out_blocks = qn;
  return n;
}

// 到点的采样：把这一次看到的坐标副本补进托举表（并立刻按到目标高度）。
static void run_due_hunts() {
  if (g_hunt_n <= 0) return;
  const DWORD now = GetTickCount();
  if (InterlockedCompareExchange(&g_hunt_lock, 1, 0) != 0) return;
  int w = 0;
  constexpr int kStages = (int)(sizeof(kHuntDelays) / sizeof(kHuntDelays[0]));
  for (int i = 0; i < g_hunt_n; ++i) {
    PosHunt h = g_hunt[i];
    if ((int32_t)(now - h.next_at) < 0) {
      g_hunt[w++] = h;
      continue;
    }
    PosField f[kMaxPosFields];
    int blocks = 0;
    const int n = collect_pos_fields_deep(h.obj, h.sx, h.sy, h.sz - 8000.f, h.sz + 400.f, f,
                                          kMaxPosFields, &blocks);
    // 第一遍只观察、不写：和上一遍比 z，往下掉的那几份就是引擎真正在用的位置。
    if (h.stage > 0) {
      for (int k = 0; k < n; ++k) {
        for (int p = 0; p < h.prev_n; ++p) {
          if (h.prev_obj[p] != f[k].obj || h.prev_off[p] != f[k].off) continue;
          const float dz = f[k].z - h.prev_z[p];
          if (dz < -2.f) {
            log("air hold: FALLING %08X+0x%X z %.1f -> %.1f (dz=%.1f)",
                f[k].obj, f[k].off, h.prev_z[p], f[k].z, dz);
          }
          break;
        }
      }
    }
    int added = 0;
    for (int k = 0; k < n; ++k) {
      if (h.stage > 0) {
        if (f[k].z < h.sz - 0.5f) write_f32(f[k].obj + f[k].off + 8, h.sz);
        air_hold_add_raw(f[k].obj, f[k].off, h.sz, h.sx, h.sy);
        ++added;
      }
    }
    h.prev_n = 0;
    for (int k = 0; k < n && h.prev_n < 8; ++k) {
      h.prev_obj[h.prev_n] = f[k].obj;
      h.prev_off[h.prev_n] = f[k].off;
      h.prev_z[h.prev_n] = f[k].z;
      ++h.prev_n;
    }
    if (h.stage == 0 || added) {
      log("air hold: hunt stage %d on %08X -> %d copies seen, %d held (%d blocks walked)",
          h.stage, h.obj, n, added, blocks);
    }
    ++h.stage;
    if (h.stage >= kStages) continue;   // 这个对象的采样做完了
    h.next_at = now + kHuntDelays[h.stage] - (h.stage ? kHuntDelays[h.stage - 1] : 0);
    g_hunt[w++] = h;
  }
  g_hunt_n = w;
  InterlockedExchange(&g_hunt_lock, 0);
}

// 每帧把被托住的对象按回目标高度。
// 存活判据不能再用「+0x33C 是血量结构」：气球炸弹这类是弹药（Ordnance），
// 那儿根本不是血量指针，于是每一条都在下一帧被判死、整张托举表被清空——
// 这才是"高度怎么调都贴地"的真正原因。现在只要求：
//   ① vtable 还是当初那个、且落在游戏模块内（内存没被复用成别的对象）；
//   ② 坐标还在召唤点附近（跑远了就不是它了，宁可放手也不乱写）。
void air_hold_tick_impl() {
  if (g_air_hold_on) run_due_hunts();
  if (g_air_hold_n <= 0) return;
  const uint32_t base = module_base();
  if (!g_air_hold_on) {
    air_hold_release_all();
    return;
  }
  int w = 0;
  int dropped = 0;
  int pulled = 0;
  DWORD stuck = 0;
  if (!air_lock_take()) return;
  const int held = g_air_hold_n;
  const DWORD now = GetTickCount();
  for (int i = 0; i < held; ++i) {
    AirHold h = g_air_hold[i];
    bool keep = false;
    __try {
      uint32_t vt = 0;
      float px = 0.f, py = 0.f, z = 0.f;
      bool alive = true;
      if (h.vt != 0) {
        // 有 vtable 的：内存没被别的东西复用
        alive = safe_read_u32(h.obj, &vt) && is_ptr(vt) && base && vt >= base &&
                vt < base + 0x940000u && vt == h.vt;
      }
      if (alive && safe_read_bytes(h.obj + h.off, &px, sizeof(px)) &&
          safe_read_bytes(h.obj + h.off + 4, &py, sizeof(py)) &&
          safe_read_bytes(h.obj + h.off + 8, &z, sizeof(z)) && z > -20000.f &&
          z < 20000.f && std::fabs(px - h.sx) < 800.f && std::fabs(py - h.sy) < 800.f) {
        // 只往上托，不压：引擎自己送得更高就别管。
        if (z < h.z - 0.5f) {
          write_f32(h.obj + h.off + 8, h.z);
          ++pulled;
        } else if (z > h.z + 1.f) {
          ++stuck;   // 已经比目标高（被别的逻辑抬走了）
        }
        keep = true;
        // 诊断：出生后 8 秒内每秒报一次"引擎现在把它放多高"
        if ((int32_t)(now - h.probe_at) >= 0 && (DWORD)(now - h.born) < 8000) {
          h.probe_at = now + 1000;
          log("air hold probe: %08X+0x%X z=%.1f want=%.1f", h.obj, h.off, z, h.z);
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      keep = false;
    }
    if (keep) {
      g_air_hold[w++] = h;
    } else {
      ++dropped;
    }
  }
  static int s_tick = 0;
  if ((++s_tick % 120) == 0 && pulled) {
    log("air hold: %d held, %d pulled back this frame, %d already higher, %d dropped",
        g_air_hold_n, pulled, (int)stuck, dropped);
  }
  if (w != g_air_hold_n) {
    if (dropped) log("air hold: %d object(s) gone, %d still held", dropped, w);
    g_air_hold_n = w;
    InterlockedExchange(&g_air_hold_live, w);
  }
  air_lock_drop();
}

static DWORD WINAPI air_hold_thread_proc(LPVOID) {
  InterlockedExchange(&g_air_thread_live, 1);
  while (InterlockedCompareExchange(&g_air_thread_stop, 0, 0) == 0) {
    Sleep(8);
    if (InterlockedCompareExchange(&g_air_thread_stop, 0, 0) != 0) break;
    __try {
      air_hold_tick_impl();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  InterlockedExchange(&g_air_thread_live, 0);
  return 0;
}

static void post_create_show(uint32_t obj) {
  if (!is_ptr(obj)) return;
  // 护栏：Thing 的 +0x33C 是 HP 结构指针；不是就别往这块内存写（写错地方会直接崩）。
  uint32_t hp = 0;
  uint32_t hpv = 0;
  if (!safe_read_u32(obj + 0x33C, &hp) || !is_ptr(hp)) return;
  if (!safe_read_u32(hp, &hpv) || hpv == 0) return;
  uint32_t v = 0;
  // Copilot 的 kPostCreateStateWords：把"静息态 / 回盾 / 技能启用"这几个状态字改成
  // 一个正常在场单位该有的值（+0x80/+0x84/+0x88/+0x90/+0x94/+0xD0）。
  write_u32(obj + 0x80, 0x00000000u);
  write_u32(obj + 0x84, 0x00000800u);
  write_u32(obj + 0x88, 0x00000000u);
  write_u32(obj + 0x90, 0x00000002u);
  write_u32(obj + 0x94, 0x00000020u);
  write_u32(obj + 0xD0, 0x00000000u);
  if (safe_read_u32(obj + 0xBC, &v)) write_u32(obj + 0xBC, v | 0x80000000u);
  if (safe_read_u32(obj + 0xC8, &v)) write_u32(obj + 0xC8, v & ~0x00000800u);
  if (safe_read_u32(obj + 0x28, &v)) write_u32(obj + 0x28, v & ~0x80000000u);
  if (safe_read_u32(obj + 0x208, &v)) write_u32(obj + 0x208, v & ~0x80000000u);
}

static void drain_unit_spawns_impl() {
  const int n = (int)InterlockedExchange(&g_spawn_pub, 0);
  if (n <= 0) return;
  const int count = n > (int)(sizeof(g_spawn_q) / sizeof(g_spawn_q[0]))
                        ? (int)(sizeof(g_spawn_q) / sizeof(g_spawn_q[0]))
                        : n;
  uint8_t* mc = mc_base();
  int made = 0;
  if (mc) {
    const uint32_t pos_buf = reinterpret_cast<uint32_t>(mc + 0x10A0);
    for (int i = 0; i < count; ++i) {
      const PendingSpawn& s = g_spawn_q[i];
      if (!is_ptr(s.tmpl) || !is_ptr(s.owner)) continue;
      write_f32(pos_buf, s.x);
      write_f32(pos_buf + 4, s.y);
      write_f32(pos_buf + 8, s.z);
      // 第 4 个 float 填 1.0（和 Copilot / 引擎自己的落点向量一致），
      // 别把这个槽里的旧垃圾交给游戏。
      write_f32(pos_buf + 12, 1.0f);
      bool threw = false;
      const uint32_t ent = create_unit_on_this_thread(s.tmpl, pos_buf, s.owner, s.info, &threw);
      if (is_ptr(ent)) {
        ++made;
        if (s.tag >= 0 && s.tag < 64) ++g_spawn_type_made[s.tag];
        if (s.z > 0.f) {
          int found = 0;
          PosField fields[kMaxPosFields];
          __try {
            found = post_create_lift(ent, s.x, s.y, s.base_z, s.z);
            if (found > 0) {
              collect_pos_fields(ent, s.x, s.y, s.base_z, true, fields, kMaxPosFields);
            }
          } __except (EXCEPTION_EXECUTE_HANDLER) {
            log("post-create lift faulted on %08X", ent);
            found = 0;
          }
          // 会自己往下掉/落地即爆的对象（z 被抬过）每帧都要按回去，
          // 否则抬多高都只是"从高处掉下来炸"。
          if (found > 0 && s.z > s.base_z + 5.f) {
            for (int k = 0; k < found && k < kMaxPosFields; ++k) {
              air_hold_add(fields[k].obj, fields[k].off, s.z, s.x, s.y);
            }
            ensure_air_hold_thread();
            // 真正驱动"下落"的那一份坐标常常在另一个堆块里、晚几百毫秒才建好，
            // 所以按时刻表反复下钻扫描，把新出现的副本也收进托举表。
            schedule_hunt(ent, s.x, s.y, s.z);
            log("summon: holding %08X at z=%.1f (%d position copies)", ent, s.z, found);
          }
        }
        if (g_summon_visible_fix) {
          __try {
            post_create_show(ent);
          } __except (EXCEPTION_EXECUTE_HANDLER) {
            log("post-create state fix faulted on %08X", ent);
          }
        }
      } else if (threw) {
        // CreateUnit 里炸了：引擎的分配器/对象表多半已经半坏，这一批剩下的别再往里塞。
        log("create unit tmpl=%08X threw; aborting the rest of this batch (%d left)", s.tmpl,
            count - i - 1);
        break;
      }
    }
  }
  InterlockedExchange(&g_spawn_made, made);
  if (g_spawn_event) SetEvent(g_spawn_event);
}

static int submit_spawns(int n) {
  if (n <= 0) return 0;
  ensure_spawn_event();
  if (!g_spawn_event) return -1;
  std::memset(g_spawn_type_made, 0, sizeof(g_spawn_type_made));
  InterlockedExchange(&g_spawn_made, 0);
  ResetEvent(g_spawn_event);
  InterlockedExchange(&g_spawn_pub, n);
  if (WaitForSingleObject(g_spawn_event, 2000) != WAIT_OBJECT_0) {
    InterlockedExchange(&g_spawn_pub, 0);
    return -1;
  }
  return (int)g_spawn_made;
}

static void summon_ring_xy(float ox, float oy, int slot, int total, float* x, float* y) {
  if (!x || !y) return;
  if (total <= 1) {
    *x = ox;
    *y = oy;
    return;
  }
  constexpr int kPer = 6;
  const int ring = slot / kPer;
  const int idx = slot - ring * kPer;
  int on_ring = total - ring * kPer;
  if (on_ring > kPer) on_ring = kPer;
  if (on_ring < 1) on_ring = 1;
  const float radius = 32.f + 26.f * (float)ring;
  const float spin = (ring & 1) ? 0.5f : 0.f;
  const float angle = 6.2831853f * ((float)idx + spin) / (float)on_ring;
  *x = ox + radius * std::cos(angle);
  *y = oy + radius * std::sin(angle);
}

// 有些对象贴地生成会立刻"落地即爆"（气球炸弹），要给它们一个起始高度。
// 单位是游戏世界坐标（和 /summon?z= 同一个量纲，飞机巡航大约 300~600）。
// 气球炸弹比飞机高得多：它本身就是"缓慢下落的弹药"（模板描述 Slow-Falling Ordnance），
// 一落地就炸，所以直接放到高空，配合 air_hold_tick() 停在那儿。
static float summon_spawn_lift(const char* type_id) {
  if (!type_id || !type_id[0]) return 0.f;
  if (contains_ic(type_id, "Balloon")) return 3000.f;
  return 0.f;
}

// 用户手动加的抬升（面板上的「召唤高度」），默认 0。
static float g_summon_lift_extra = 0.f;

static bool summon_troops_impl(std::string* out_msg) {
  const SummonSnap snap = g_summon_snap;
  if (snap.n <= 0 || snap.count <= 0) {
    if (out_msg) *out_msg = u8"请先选择要召唤的单位 / 建筑";
    return false;
  }
  if (!mc_base()) {
    if (out_msg) *out_msg = u8"请先点「注入」";
    return false;
  }
  uint32_t owner = snap.player;
  if (!is_ptr(owner)) owner = local_owner_for_ops();
  if (!is_ptr(owner)) {
    if (out_msg) *out_msg = u8"请先选择玩家";
    return false;
  }
  char owner_name[64] = {};
  {
    uint32_t pid = 0;
    safe_read_u32(owner + 0x20, &pid);
    label_battle_player(owner, pid, owner == local_owner_for_ops(), owner_name, sizeof(owner_name));
  }
  uint32_t owner_vt = 0;
  if (!safe_read_u32(owner, &owner_vt) || !is_ptr(owner_vt)) {
    if (out_msg) *out_msg = u8"该玩家当前不能接收部队";
    return false;
  }
  uint32_t owner_info = 0;
  if (!safe_read_u32(owner + 0x10, &owner_info)) {
    if (out_msg) *out_msg = u8"该玩家当前不能接收部队";
    return false;
  }
  if (owner_info) {
    uint32_t info_vt = 0;
    if (!is_ptr(owner_info) || !safe_read_u32(owner_info, &info_vt) || !is_ptr(info_vt)) {
      if (out_msg) *out_msg = u8"该玩家数据不完整，不能召唤";
      return false;
    }
  }
  float spawn[3];
  if (!mouse_world_pos(spawn)) {
    if (out_msg) *out_msg = u8"读不到鼠标地图坐标（请把鼠标移到战场地形上）";
    return false;
  }
  uint32_t tmpls[64] = {};
  float lifts[64] = {};
  int planned = 0;
  int missing = 0;
  const char* first_missing = nullptr;
  for (int i = 0; i < snap.n; ++i) {
    lifts[i] = summon_spawn_lift(snap.ids[i]);
    tmpls[i] = find_template_by_id(snap.ids[i]);
    if (!is_ptr(tmpls[i])) {
      ++missing;
      if (!first_missing) first_missing = snap.ids[i];
      log("summon no template: %s", snap.ids[i]);
    } else {
      planned += snap.count;
      log("summon type %s -> tmpl %08X lift=%.0f x%d", snap.ids[i], tmpls[i], lifts[i],
          snap.count);
    }
  }
  log("summon player=%08X info=%08X types=%d each=%d planned=%d at=%.1f,%.1f,%.1f", owner,
      owner_info, snap.n, snap.count, planned, spawn[0], spawn[1], spawn[2]);
  if (InterlockedCompareExchange(&g_spawn_busy, 1, 0) != 0) {
    if (out_msg) *out_msg = u8"上一次召唤还在进行";
    return false;
  }
  int queued = 0;
  for (int i = 0; i < snap.n && queued < (int)(sizeof(g_spawn_q) / sizeof(g_spawn_q[0])); ++i) {
    if (!is_ptr(tmpls[i])) continue;
    for (int c = 0; c < snap.count && queued < (int)(sizeof(g_spawn_q) / sizeof(g_spawn_q[0]));
         ++c) {
      PendingSpawn& s = g_spawn_q[queued];
      summon_ring_xy(spawn[0], spawn[1], queued, planned, &s.x, &s.y);
      s.z = spawn[2] + lifts[i] + g_summon_lift_extra;
      s.base_z = spawn[2];
      s.tag = i;
      s.tmpl = tmpls[i];
      s.owner = owner;
      s.info = owner_info;
      ++queued;
    }
  }
  const int made = queued > 0 ? submit_spawns(queued) : 0;
  InterlockedExchange(&g_spawn_busy, 0);
  if (made < 0) {
    if (out_msg) *out_msg = u8"召唤没有赶上游戏这一帧";
    return false;
  }
  // 哪种在模板库里、也提交了，但引擎没造出来 → 单独点名，方便判断是"版本没有"
  // 还是"对象本身不让 CreateUnit 造"。
  std::vector<std::string> failed;
  for (int i = 0; i < snap.n; ++i) {
    if (!is_ptr(tmpls[i])) continue;
    if (g_spawn_type_made[i] > 0) continue;
    failed.emplace_back(snap.ids[i]);
    log("summon type %s exists but CreateUnit refused it", snap.ids[i]);
  }
  if (made <= 0) {
    if (out_msg) {
      if (missing) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      u8"本版本没有 %s（%d 种缺失），已跳过", first_missing, missing);
        *out_msg = buf;
      } else if (failed.size()) {
        char buf[220];
        std::snprintf(buf, sizeof(buf), u8"游戏拒绝创建 %s（模板在，但造不出来）",
                      failed[0].c_str());
        *out_msg = buf;
      } else {
        *out_msg = u8"召唤失败（请把鼠标放在可通行的地形上）";
      }
    }
    return false;
  }
  if (out_msg) {
    char buf[260];
    size_t used = 0;
    used += std::snprintf(buf, sizeof(buf), u8"已召唤 %d 个单位（归属%s）", made,
                          owner_name[0] ? owner_name : u8"所选玩家");
    if (missing && used < sizeof(buf)) {
      used += std::snprintf(buf + used, sizeof(buf) - used, u8"，%d 种本版本没有", missing);
    }
    if (!failed.empty() && used < sizeof(buf)) {
      std::snprintf(buf + used, sizeof(buf) - used, u8"，%s 等 %d 种造不出来",
                    failed[0].c_str(), (int)failed.size());
    }
    *out_msg = buf;
  }
  return true;
}

static bool spawn_one_mcv(std::string* out_msg) {
  if (!mc_base()) {
    if (out_msg) *out_msg = u8"请先点「注入」";
    return false;
  }
  const int faction = mcv_faction();
  uint32_t owner = g_mcv_player;
  if (!is_ptr(owner)) owner = local_owner_for_ops();
  if (!is_ptr(owner)) {
    if (out_msg) *out_msg = u8"请先选择玩家";
    return false;
  }
  char owner_name[64] = {};
  {
    uint32_t pid = 0;
    safe_read_u32(owner + 0x20, &pid);
    label_battle_player(owner, pid, owner == local_owner_for_ops(), owner_name, sizeof(owner_name));
  }
  uint32_t owner_info = 0;
  safe_read_u32(owner + 0x10, &owner_info);

  float spawn[3];
  if (!mouse_world_pos(spawn)) {
    if (out_msg) *out_msg = u8"读不到鼠标地图坐标（请把鼠标移到战场地形上）";
    return false;
  }
  const char* type_id = mcv_type_id(faction);
  const uint32_t tmpl = find_template_by_id(type_id);
  if (!is_ptr(tmpl)) {
    if (out_msg) *out_msg = u8"找不到该阵营的基地车（请先进入战局）";
    return false;
  }
  uint32_t pos_buf = reinterpret_cast<uint32_t>(mc_base() + 0x10A0);
  write_f32(pos_buf, spawn[0]);
  write_f32(pos_buf + 4, spawn[1]);
  write_f32(pos_buf + 8, spawn[2]);
  CallJob job{};
  job.kind = CallJob::kCdecl;
  job.fn = va_of(kFnCreateUnit);
  job.nargs = 5;
  job.args[0] = 0;
  job.args[1] = tmpl;
  job.args[2] = pos_buf;
  job.args[3] = owner;
  job.args[4] = owner_info;
  if (!run_job(job) || !is_ptr(job.eax_out)) {
    if (out_msg) *out_msg = u8"召唤失败（请把鼠标放在可通行的地形上）";
    return false;
  }
  if (out_msg) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), u8"已召唤一辆%s基地车（归属%s）", mcv_faction_name(faction),
                  owner_name[0] ? owner_name : u8"所选玩家");
    *out_msg = buf;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 空投（箱子）
// ---------------------------------------------------------------------------
// 箱子模板名从引擎的模板哈希表里现扫，避免把名字写死（零售版/起义时刻/各语言版
// 的箱子集合略有差异）。扫不到才退回内置表。

struct CrateKindEntry {
  char id[96];
  const char* zh;
  uint32_t tmpl = 0;  // 解析成功后缓存，避免每次空投都重新扫内存
};

static CrateKindEntry g_crates[48];
static int g_crate_n = -1;  // -1 = 还没扫过
static bool g_crate_real = false;   // 当前列表是不是真从模板表扫出来的
static DWORD g_crate_scan_tick = 0;

static const char* crate_zh_for(const char* id) {
  auto has = [id](const char* s) { return std::strstr(id, s) != nullptr; };
  if (has("Attribute")) return u8"属性箱";
  if (has("IronCurtain")) return u8"属性箱·铁幕";
  if (has("Bomb") || has("Forzen") || has("Frozen")) return u8"炸弹箱";
  if (has("Veterancy")) return u8"经验箱";
  if (has("Heal")) return u8"治疗箱";
  if (has("Money")) return u8"奖励箱·资金";
  if (has("Salvage")) return u8"回收箱";
  if (has("Random")) return u8"神秘箱";
  if (has("Shroud")) return u8"黑幕箱";
  if (has("Unit")) return u8"部队箱";
  return nullptr;
}

// 只认真正的箱子模板：CrateMoney / CrateHeal / CrateShroud ...
// 排除碎片、投放器、拾取触发器等同样带 Crate 字样的对象。
static bool crate_template_id_ok(const char* s) {
  if (!s || std::strncmp(s, "Crate", 5) != 0) return false;
  static const char* kBanned[] = {"Debris", "Pickup",  "Spawner", "Revealer", "Module",
                                  "Collide", "Nugget", "Command", "Type",     "Data",
                                  "State",   "Update", "Dummy",   "Seed",     "Class"};
  for (const char* b : kBanned) {
    if (std::strstr(s, b)) return false;
  }
  return true;
}

// 兜底内存扫描用的宽松判断：代号必须以 Crate 开头。
// （引擎里还有 VeterancyCrate / CratePickup 这类"名字里带 Crate、但不是箱子模板"
// 的字符串，之前把这类东西扫进列表，空投时自然一个模板都解析不到。）
static bool crate_id_ok(const char* s) {
  const size_t n = std::strlen(s);
  if (n < 5 || n > 80) return false;
  return crate_template_id_ok(s);
}

// 从模板对象上把它的代号扫出来。引擎把 id 存成 C 字符串，可能挂在对象本身，
// 也可能挂在对象里第一层 / 第二层的子对象上（和 template_has_exact_id 同一套偏移）。
static bool template_id_of(uint32_t tmpl, char* out, size_t out_n) {
  if (!is_ptr(tmpl) || !out || out_n < 16) return false;
  auto take = [&](uint32_t addr) -> bool {
    char buf[96] = {};
    if (!safe_read_bytes(addr, buf, sizeof(buf) - 1)) return false;
    buf[sizeof(buf) - 1] = 0;
    const size_t len = std::strlen(buf);
    if (len < 5 || len > 80) return false;
    for (size_t i = 0; i < len; ++i) {
      const char c = buf[i];
      const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') || c == '_';
      if (!ok) return false;
    }
    std::snprintf(out, out_n, "%s", buf);
    return true;
  };
  for (uint32_t off = 0; off <= 0x180; off += 4) {
    if (take(tmpl + off)) return true;
    uint32_t p = 0;
    if (!safe_read_u32(tmpl + off, &p) || !is_ptr(p)) continue;
    if (take(p)) return true;
    uint32_t p2 = 0;
    if (safe_read_u32(p, &p2) && is_ptr(p2) && take(p2)) return true;
  }
  return false;
}

// 列表排序权重：把常用的箱子放前面（默认选中项就是第一个）。
static int crate_sort_rank(const char* id) {
  if (std::strstr(id, "Money")) return 0;
  if (std::strstr(id, "Heal")) return 1;
  if (std::strstr(id, "Veterancy")) return 2;
  if (std::strstr(id, "Random")) return 3;
  if (std::strstr(id, "Bomb")) return 4;
  if (std::strstr(id, "Unit")) return 5;
  if (std::strstr(id, "Salvage")) return 6;
  if (std::strstr(id, "Shroud")) return 7;
  if (std::strstr(id, "Attribute")) return 8;
  return 9;
}

// 返回新加入的条目下标，-1 = 重复/已满。
static int crate_catalog_add(const char* id) {
  if (g_crate_n >= (int)(sizeof(g_crates) / sizeof(g_crates[0]))) return -1;
  for (int i = 0; i < g_crate_n; ++i) {
    if (std::strcmp(g_crates[i].id, id) == 0) return -1;
  }
  const int idx = g_crate_n++;
  CrateKindEntry& e = g_crates[idx];
  std::snprintf(e.id, sizeof(e.id), "%s", id);
  e.zh = crate_zh_for(e.id);
  return idx;
}

static bool is_id_char(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// 引擎把模板代号当 C 字符串驻留在内存里，直接扫一遍最可靠：
// 零售版叫 CrateMoneyMP、日中版 / 起义时刻可能叫别的，写死名字必然踩空。
static int crate_scan_memory() {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  auto* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
  auto* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
  int found = 0;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
    auto* region = static_cast<uint8_t*>(mbi.BaseAddress);
    const size_t size = mbi.RegionSize;
    auto* next = region + size;
    if (is_committed_readable(mbi.State, mbi.Protect) && size > 16) {
      const uint8_t* lim = region + size;
      const uint8_t* q = region;
      while (q < lim) {
        const void* hit = std::memchr(q, 'C', static_cast<size_t>(lim - q));
        if (!hit) break;
        const auto* c = static_cast<const uint8_t*>(hit);
        const uint8_t* start = c;
        while (start > region && is_id_char(start[-1])) --start;
        const uint8_t* e = c;
        while (e < lim && is_id_char(*e)) ++e;
        const size_t n = static_cast<size_t>(e - start);
        if (n >= 5 && n <= 80) {
          char buf[96];
          const size_t take = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
          std::memcpy(buf, start, take);
          buf[take] = 0;
          const bool left_ok = (start == region) || (start[-1] == 0);
          const bool right_ok = (e >= lim) || (*e == 0);
          if (left_ok && right_ok && crate_id_ok(buf)) {
            const int before = g_crate_n;
            crate_catalog_add(buf);
            if (g_crate_n > before) ++found;
          }
        }
        q = (e > c) ? e : c + 1;
      }
    }
    if (next <= p) break;
    p = next;
  }
  return found;
}

static void crate_catalog_build() {
  g_crate_n = 0;
  // 首选：直接走引擎模板表（ThingFactory+0x24 的单向链表，+0xC 是 next）。
  // 这条链就是引擎自己按名字查模板时用的那张表，所以扫出来的名字一定能解析。
  int from_factory = 0;
  uint32_t factory = 0;
  if (module_base() &&
      safe_read_u32(module_base() + kThingFactoryRva, &factory) && is_ptr(factory)) {
    uint32_t tmpl = 0;
    if (safe_read_u32(factory + 0x24, &tmpl) && is_ptr(tmpl)) {
      for (int n = 0; n < 8192 && is_ptr(tmpl); ++n) {
        char id[96] = {};
        if (template_id_of(tmpl, id, sizeof(id)) && crate_template_id_ok(id)) {
          const int idx = crate_catalog_add(id);
          if (idx >= 0) {
            g_crates[idx].tmpl = tmpl;   // 顺手缓存模板，省掉每次空投的查找
            ++from_factory;
          }
        }
        uint32_t next = 0;
        if (!safe_read_u32(tmpl + 0xC, &next) || next == tmpl || !is_ptr(next)) break;
        tmpl = next;
      }
    }
  }
  if (from_factory > 0) {
    log("crate catalog: %d crate templates from factory", from_factory);
  } else if (is_ptr(local_owner_for_ops())) {
    // 兜底：模板表没读到，扫一遍内存里的字符串。这一步要几百毫秒，
    // 只在战局里做（主菜单不需要箱子表，进来了会重新扫）。
    const DWORD t0 = GetTickCount();
    const int n = crate_scan_memory();
    log("crate catalog: memory scan found %d ids in %lu ms", n, GetTickCount() - t0);
  }
  g_crate_real = (g_crate_n > 0);
  if (g_crate_n == 0) {
    // 最后兜底：零售版内置箱子名（模板还解析不到，进战局后会重新扫）。
    static const char* kFallback[] = {"CrateMoney",    "CrateMoneyMP",   "CrateHeal",
                                      "CrateHealMP",   "CrateVeterancy", "CrateVeterancyMP",
                                      "CrateRandom",   "CrateSalvage",   "CrateShroud",
                                      "CrateBomb",     "CrateUnit",      "CrateAttribute"};
    for (const char* id : kFallback) crate_catalog_add(id);
    g_crate_real = false;
    log("crate catalog: still empty, using built-in list");
  }
  {
    // 按常用度排序，默认选中项就是第一个（资金箱）。
    std::stable_sort(g_crates, g_crates + g_crate_n,
                     [](const CrateKindEntry& a, const CrateKindEntry& b) {
                       const int ra = crate_sort_rank(a.id), rb = crate_sort_rank(b.id);
                       if (ra != rb) return ra < rb;
                       return std::strcmp(a.id, b.id) < 0;
                     });
    // 去重：引擎对同一种箱子会给出单机版 / 联机版两个代号
    // （CrateHeal / CrateHealMP 都叫"治疗箱"），UI 里没必要列两遍。
    // 排序后先出现的是非 MP 代号，所以保留它，丢掉落后的同名牌。
    int kept = 0;
    int dropped = 0;
    int out = 0;
    for (int i = 0; i < g_crate_n; ++i) {
      const char* label = g_crates[i].zh ? g_crates[i].zh : g_crates[i].id;
      bool dup = false;
      for (int j = 0; j < out; ++j) {
        const char* prev = g_crates[j].zh ? g_crates[j].zh : g_crates[j].id;
        if (std::strcmp(prev, label) == 0) {
          dup = true;
          break;
        }
      }
      if (dup) {
        ++dropped;
        continue;
      }
      if (out != i) g_crates[out] = g_crates[i];
      ++out;
      ++kept;
    }
    g_crate_n = out;
    log("crate catalog: %d entries (real=%d, deduped %d)", kept, g_crate_real ? 1 : 0, dropped);
    for (int i = 0; i < g_crate_n && i < 24; ++i) {
      log("  crate[%d] = %s (%s)", i, g_crates[i].id,
          g_crates[i].zh ? g_crates[i].zh : "-");
    }
  }
  g_crate_scan_tick = GetTickCount();
}

static void crate_catalog_ensure() {
  if (g_crate_n >= 0 && g_crate_real) return;
  // 兜底表不算数，但要节流：内存扫描要几百毫秒，不能每 3 秒来一次。
  const DWORD now = GetTickCount();
  if (g_crate_n >= 0 && g_crate_scan_tick &&
      static_cast<int32_t>(now - g_crate_scan_tick) < 30000) {
    return;
  }
  // 不在战局里（读不到玩家归属）就不扫，模板表是读地图时才填的。
  if (g_crate_n >= 0 && !is_ptr(local_owner_for_ops())) return;
  crate_catalog_build();
}

static bool spawn_crate_impl(int crate_idx, int count, float height, std::string* out_msg) {
  if (!mc_base()) {
    if (out_msg) *out_msg = u8"请先点「注入」";
    return false;
  }
  crate_catalog_ensure();
  if (g_crate_n <= 0) {
    if (out_msg) *out_msg = u8"箱子表是空的（请先进入战局再点一次）";
    return false;
  }
  if (crate_idx < 0 || crate_idx >= g_crate_n) crate_idx = 0;
  const char* id = g_crates[crate_idx].id;
  if (count < 1) count = 1;
  if (count > 20) count = 20;

  uint32_t tmpl = g_crates[crate_idx].tmpl;
  if (is_ptr(tmpl)) {
    // 跨局缓存要校验：模板对象可能被下一局重建，指针会失效。
    uint32_t vt = 0;
    uint32_t lo = 0, hi = 0;
    const bool vtable_ok = safe_read_u32(tmpl, &vt) && image_range(&lo, &hi) && vt >= lo &&
                           vt < hi;
    if (!vtable_ok || !template_has_exact_id(tmpl, id)) tmpl = 0;
  }
  if (!is_ptr(tmpl)) tmpl = find_template_by_id(id);
  if (!is_ptr(tmpl)) {
    // 表里的名字不一定都能解析（不同版本命名不同），换一个能用的。
    const int try_n = g_crate_n < 4 ? g_crate_n : 4;
    for (int i = 0; i < try_n && !is_ptr(tmpl); ++i) {
      if (i == crate_idx) continue;
      const uint32_t t = find_template_by_id(g_crates[i].id);
      if (is_ptr(t)) {
        log("crate: '%s' not found in this build, using '%s'", id, g_crates[i].id);
        tmpl = t;
        crate_idx = i;
        id = g_crates[i].id;
      }
    }
  }
  if (!is_ptr(tmpl)) {
    log("crate: no template for '%s' (catalog=%d)", id, g_crate_n);
    if (out_msg) {
      char buf[200];
      std::snprintf(buf, sizeof(buf), u8"箱子模板一个都没解析到（表里有 %d 项，请把日志发我）",
                    g_crate_n);
      *out_msg = buf;
    }
    return false;
  }
  float spawn[3];
  if (!mouse_world_pos(spawn)) {
    if (out_msg) *out_msg = u8"读不到鼠标地图坐标（请把鼠标移到战场地形上）";
    return false;
  }
  uint32_t owner = local_owner_for_ops();
  if (!is_ptr(owner)) {
    if (out_msg) *out_msg = u8"请先选择玩家";
    return false;
  }
  uint32_t owner_info = 0;
  safe_read_u32(owner + 0x10, &owner_info);

  uint32_t pos_buf = reinterpret_cast<uint32_t>(mc_base() + 0x10A0);
  int ok_n = 0;
  for (int i = 0; i < count; ++i) {
    const float radius = (count > 1) ? 45.f : 0.f;
    const float angle = (float)i * 2.399963f;
    write_f32(pos_buf, spawn[0] + radius * std::cos(angle));
    write_f32(pos_buf + 4, spawn[1] + radius * std::sin(angle));
    write_f32(pos_buf + 8, spawn[2] + height);
    CallJob job{};
    job.kind = CallJob::kCdecl;
    job.fn = va_of(kFnCreateUnit);
    job.nargs = 5;
    job.args[0] = 0;
    job.args[1] = tmpl;
    job.args[2] = pos_buf;
    job.args[3] = owner;
    job.args[4] = owner_info;
    if (run_job(job) && is_ptr(job.eax_out)) ++ok_n;
    Sleep(40);
  }
  if (ok_n == 0) {
    if (out_msg) *out_msg = u8"空投失败（请把鼠标放在可通行的地形上）";
    log("crate: spawn failed id=%s tmpl=%08X at=%.1f,%.1f", id, (unsigned)tmpl, spawn[0], spawn[1]);
    return false;
  }
  log("crate: dropped %d x %s tmpl=%08X at=%.1f,%.1f,%.1f", ok_n, id, (unsigned)tmpl, spawn[0],
      spawn[1], spawn[2]);
  if (crate_idx >= 0 && crate_idx < g_crate_n) g_crates[crate_idx].tmpl = tmpl;
  if (out_msg) {
    char buf[192];
    const char* zh = g_crates[crate_idx].zh ? g_crates[crate_idx].zh : id;
    std::snprintf(buf, sizeof(buf), u8"已空投 %d 个「%s」", ok_n, zh);
    *out_msg = buf;
  }
  return true;
}

// ---- 当前选中的箱子（UI 记着，真正空投时用） ----
static int g_crate_sel_idx = 0;
static int g_crate_sel_count = 1;

constexpr uint32_t kUpgradeStoreRva = 0x008E0D90;
constexpr uint32_t kFnPlayerSetUpgrade = 0x0088B800;
constexpr uint32_t kFnPlayerClearUpgrade = 0x00888510;

static const char* kSpawnRankUpgrade[3] = {
    "Upgrade_ProductionVeterancy_VETERAN",
    "Upgrade_ProductionVeterancy_ELITE",
    "Upgrade_ProductionVeterancy_HEROIC",
};

static uint32_t g_rank_store = 0;
static uint32_t g_rank_tmpl[3] = {};

static bool thiscall_set_upgrade(uint32_t player, uint32_t tmpl) {
  using Fn = void(__thiscall*)(uint32_t, uint32_t, uint32_t, uint32_t);
  const Fn fn = reinterpret_cast<Fn>(va_of(kFnPlayerSetUpgrade));
  __try {
    fn(player, tmpl, 2, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log("spawn rank grant fault player=%08X tmpl=%08X", player, tmpl);
    return false;
  }
}

static bool thiscall_clear_upgrade(uint32_t player, uint32_t tmpl) {
  using Fn = void(__thiscall*)(uint32_t, uint32_t, uint32_t);
  const Fn fn = reinterpret_cast<Fn>(va_of(kFnPlayerClearUpgrade));
  __try {
    fn(player, tmpl, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log("spawn rank clear fault player=%08X tmpl=%08X", player, tmpl);
    return false;
  }
}

static uint32_t find_upgrade_by_name(uint32_t store, const char* name) {
  if (!is_ptr(store) || !name || !name[0]) return 0;
  alignas(8) uint32_t key[2] = {};
  using Make = uint32_t(__cdecl*)(void*, const char*, int);
  using Find = uint32_t(__thiscall*)(uint32_t, void*);
  const Make make = reinterpret_cast<Make>(va_of(0x00482BC0));
  const Find find = reinterpret_cast<Find>(va_of(0x00586CF0));
  __try {
    make(key, name, 0);
    if (!key[0] && !key[1]) return 0;
    return find(store, key);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log("spawn rank lookup fault name=%s", name);
    return 0;
  }
}

static bool ensure_spawn_rank_templates() {
  if (!module_base()) return false;
  uint32_t store = 0;
  if (!safe_read_u32(module_base() + kUpgradeStoreRva, &store) || !is_ptr(store)) {
    g_rank_store = 0;
    return false;
  }
  if (store == g_rank_store && is_ptr(g_rank_tmpl[0]) && is_ptr(g_rank_tmpl[1]) &&
      is_ptr(g_rank_tmpl[2])) {
    return true;
  }
  g_rank_store = store;
  g_rank_tmpl[0] = g_rank_tmpl[1] = g_rank_tmpl[2] = 0;
  for (int i = 0; i < 3; ++i) {
    g_rank_tmpl[i] = find_upgrade_by_name(store, kSpawnRankUpgrade[i]);
  }
  const bool ok = is_ptr(g_rank_tmpl[0]) && is_ptr(g_rank_tmpl[1]) && is_ptr(g_rank_tmpl[2]);
  if (!ok) {
    log("spawn rank templates missing store=%08X v=%08X e=%08X h=%08X", store, g_rank_tmpl[0],
        g_rank_tmpl[1], g_rank_tmpl[2]);
  }
  return ok;
}

static bool player_has_upgrade(uint32_t player, uint32_t tmpl) {
  uint32_t node = 0;
  if (!safe_read_u32(player + 0x34, &node)) return false;
  for (int n = 0; n < 256 && is_ptr(node); ++n) {
    uint32_t cur = 0;
    uint32_t status = 0;
    uint32_t next = 0;
    if (!safe_read_u32(node + 4, &cur)) break;
    safe_read_u32(node + 8, &status);
    if (cur == tmpl && status == 2) return true;
    if (!safe_read_u32(node + 0xC, &next) || next == node) break;
    node = next;
  }
  return false;
}

static int rank_of_player(uint32_t player) {
  if (!is_ptr(player) || !ensure_spawn_rank_templates()) return -2;
  int level = 0;
  for (int i = 0; i < 3; ++i) {
    if (player_has_upgrade(player, g_rank_tmpl[i])) level = i + 1;
  }
  return level;
}

static int cached_faction_of(uint32_t player) {
  for (int i = 0; i < g_fac_cache_n; ++i) {
    if (g_fac_cache[i].player == player) return g_fac_cache[i].faction;
  }
  return 0;
}

static void label_battle_player(uint32_t player, uint32_t player_id, bool /*is_local*/, char* out,
                                size_t out_len) {
  if (!out || out_len < 2) return;
  const int fac = cached_faction_of(player);
  const char* fac_name =
      fac == 1 ? u8"盟军" : fac == 2 ? u8"苏联" : fac == 3 ? u8"帝国" : nullptr;
  if (fac_name) {
    std::snprintf(out, out_len, "%s", fac_name);
    return;
  }
  std::snprintf(out, out_len, u8"玩家%u", player_id);
}

// Seat list for the player pickers (money / spawn rank / summon / unit scope /
// team / colour). Deliberately avoids the full economy poll: no object-list
// walks, no protocol scans and no unthrottled faction graph search. Those cost
// hundreds of milliseconds each and used to run once per menu frame, so simply
// showing the overlay froze the match.
static bool refresh_battle_seats_light(MatchEconomy* out) {
  if (!out) return false;
  *out = MatchEconomy{};
  out->spectator = is_spectator();
  const uint32_t base = loaded_module();
  if (!base) return false;

  float seconds = 0.f;
  if (!read_battle_clock(nullptr, &seconds)) return false;
  // Read-only on purpose: the match bookkeeping (roster caches, seen seats,
  // battle clock) stays owned by the economy board so opening the pickers can
  // never disturb it.
  out->match_seconds = seconds;

  uint32_t list = 0;
  if (!safe_read_u32(base + player_list_rva(), &list) || !is_ptr(list)) return false;

  uint32_t local = 0;
  safe_read_u32(list + kPlayerListLocalOff, &local);
  if (!is_ptr(local)) local = local_owner_for_ops();

  for (int i = 0; i < (int)kPlayerListSlots && out->player_count < kMaxEconPlayers; ++i) {
    uint32_t player = 0;
    if (!safe_read_u32(list + kPlayerListArrayOff + (uint32_t)i * 4u, &player)) continue;
    if (!is_ptr(player)) continue;
    bool dup = false;
    for (int j = 0; j < out->player_count; ++j) {
      if (out->players[j].player == player) {
        dup = true;
        break;
      }
    }
    if (dup) continue;

    PlayerEconomy pe{};
    fill_player_economy_basic(&pe, player, local);
    // Player index 0 is the neutral / civilian seat (oil derricks, map props).
    if (!pe.is_local && pe.player_id == 0) continue;
    const bool seen = find_seen_seat(player) >= 0;
    if (!pe.has_money && !pe.has_power && !pe.has_score && !pe.is_local && !seen) continue;
    // Same gate the board applies, so the pickers list the same seats the board
    // does: a seat counts once it entered the match, or was shown earlier.
    if (!pe.is_local && !seat_entered_battle(pe) && !seen) continue;
    mark_defeated_flag(&pe);

    // Faction label from the cache; a miss goes through the shared throttled
    // probes (cheap string hop first, then the rate-limited graph walk).
    int fac = cached_faction_of(player);
    if (!fac && fac_probe_due(player)) {
      fac = detect_faction_from_player(player);
      if (fac) remember_faction(player, pe, fac);
    }
    label_battle_player(player, pe.player_id, pe.is_local, pe.name, sizeof(pe.name));
    out->players[out->player_count++] = pe;
  }

  // Board parity: at match start no seat has a record yet, so keep the local
  // seat as the fallback the board also uses.
  if (out->player_count == 0 && is_ptr(local)) {
    PlayerEconomy pe{};
    fill_player_economy_basic(&pe, local, local);
    if (!seat_is_dummy_local(pe)) {
      label_battle_player(local, pe.player_id, pe.is_local, pe.name, sizeof(pe.name));
      out->players[out->player_count++] = pe;
    }
  }

  // Stable order: local first, then player_id, then pointer. Never sort by money.
  auto seat_less = [](const PlayerEconomy& a, const PlayerEconomy& b) {
    if (a.is_local != b.is_local) return a.is_local;
    if (a.player_id != b.player_id) return a.player_id < b.player_id;
    return a.player < b.player;
  };
  for (int i = 0; i < out->player_count; ++i) {
    for (int j = i + 1; j < out->player_count; ++j) {
      if (seat_less(out->players[j], out->players[i])) {
        PlayerEconomy tmp = out->players[i];
        out->players[i] = out->players[j];
        out->players[j] = tmp;
      }
    }
  }

  out->valid = out->player_count > 0;
  return out->valid;
}

static int list_battle_players_impl(BattlePlayer* out, int max_out) {
  if (!out || max_out <= 0) return 0;
  static MatchEconomy econ;
  if (!refresh_battle_seats_light(&econ) || econ.player_count <= 0) return 0;
  int n = 0;
  for (int i = 0; i < econ.player_count && n < max_out; ++i) {
    const PlayerEconomy& pe = econ.players[i];
    if (!is_ptr(pe.player)) continue;
    BattlePlayer& bp = out[n];
    bp = BattlePlayer{};
    bp.player = pe.player;
    bp.player_id = pe.player_id;
    bp.is_local = pe.is_local;
    bp.defeated = pe.defeated;
    bp.has_color = pe.has_color;
    bp.color_r = pe.color_r;
    bp.color_g = pe.color_g;
    bp.color_b = pe.color_b;
    std::snprintf(bp.name, sizeof(bp.name), "%s", pe.name[0] ? pe.name : u8"玩家");
    ++n;
  }
  return n;
}

static int player_spawn_rank_impl(uint32_t player) {
  if (!is_ptr(player)) return -1;
  return rank_of_player(player);
}

static bool set_spawn_rank_impl(int level, std::string* out_msg) {
  if (level < 0) level = 0;
  if (level > 3) level = 3;
  if (!module_base()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }
  const uint32_t player = g_rank_player;
  char name[64] = {};
  if (!is_ptr(player)) {
    if (out_msg) *out_msg = u8"请先选择玩家";
    return false;
  }
  {
    uint32_t pid = 0;
    safe_read_u32(player + 0x20, &pid);
    label_battle_player(player, pid, false, name, sizeof(name));
  }
  if (!ensure_spawn_rank_templates()) {
    if (out_msg) *out_msg = u8"找不到出场等级数据（请先进入战局）";
    return false;
  }
  for (int i = 2; i >= 0; --i) {
    const bool want = i < level;
    if (want || !player_has_upgrade(player, g_rank_tmpl[i])) continue;
    if (!thiscall_clear_upgrade(player, g_rank_tmpl[i])) {
      if (out_msg) *out_msg = u8"取消出场等级失败";
      return false;
    }
  }
  for (int i = 0; i < 3; ++i) {
    const bool want = i < level;
    if (!want || player_has_upgrade(player, g_rank_tmpl[i])) continue;
    if (!thiscall_set_upgrade(player, g_rank_tmpl[i])) {
      if (out_msg) *out_msg = u8"设置出场等级失败";
      return false;
    }
  }
  if (out_msg) {
    const char* who = name[0] ? name : u8"选中阵营";
    char buf[160];
    if (level == 0) {
      std::snprintf(buf, sizeof(buf), u8"已取消「%s」的出场等级", who);
    } else {
      std::snprintf(buf, sizeof(buf), u8"已将「%s」的出场等级设为 %d", who, level);
    }
    *out_msg = buf;
  }
  return true;
}

}  // namespace

// ---- 空中托举的公开入口 ----------------------------------------------------
void air_hold_tick() { air_hold_tick_impl(); }

bool air_hold_enabled() { return air_hold_enabled_impl(); }

void set_air_hold_enabled(bool on) { set_air_hold_enabled_impl(on); }

int air_hold_count() { return air_hold_count_impl(); }

void air_hold_release_all() { air_hold_release_all_impl(); }

void air_hold_forget(uint32_t obj) { air_hold_forget_impl(obj); }

void air_hold_shutdown() { air_hold_stop_thread(); }

// ---- 真无敌 / 空投 的公开入口 ----------------------------------------------
bool invincible_apply(bool on, float seconds, int flavor, std::string* out_msg) {
  return invincible_apply_impl(on, seconds, flavor, out_msg);
}

// 当前是否「已开启无敌」（点过无敌、还没解除）。
bool invincible_lock() { return invincible_lock_impl(); }

const char* invincible_status() { return invincible_status_impl(); }

void invincible_tick() { invincible_tick_impl(); }

int crate_kind_count() {
  crate_catalog_ensure();
  return g_crate_n;
}

const char* crate_kind_name(int idx) {
  crate_catalog_ensure();
  if (idx < 0 || idx >= g_crate_n) return "";
  return g_crates[idx].id;
}

const char* crate_kind_label(int idx) {
  crate_catalog_ensure();
  if (idx < 0 || idx >= g_crate_n) return "";
  return g_crates[idx].zh ? g_crates[idx].zh : g_crates[idx].id;
}

bool spawn_crate(int crate_idx, int count, float height, std::string* out_msg) {
  return spawn_crate_impl(crate_idx, count, height, out_msg);
}

bool crate_auto_active() { return false; }  // 自动空投已移除

void crate_select(int idx, int count) {
  // 箱子表可能还没扫出来（启动时就会调用），所以索引不按表长裁剪，
  // 真正空投时再夹到合法范围，保证恢复回来的选择不丢。
  if (idx >= 0) g_crate_sel_idx = idx;
  if (count > 0) g_crate_sel_count = count > 20 ? 20 : count;
}

int crate_kind_sel() { return g_crate_sel_idx; }

int crate_sel_count() { return g_crate_sel_count; }

void install_roster_hooks() { install_roster_hooks_impl(); }

bool collect_match_economy(MatchEconomy* out) { return collect_match_economy_impl(out); }

bool refresh_match_resources(MatchEconomy* inout) { return refresh_match_resources_impl(inout); }

const BuildLockEntry* build_lock_catalog(int* count) { return build_lock_catalog_impl(count); }

bool build_lock_set(const char* type_id, bool locked, std::string* out_msg) {
  return build_lock_set_impl(type_id, locked, out_msg);
}

bool build_lock_selected(bool locked, std::string* out_msg) {
  return build_lock_selected_impl(locked, out_msg);
}

void build_lock_tick() { build_lock_tick_impl(); }

const ProtocolLockEntry* protocol_lock_catalog(int* count) {
  return protocol_lock_catalog_impl(count);
}

bool protocol_lock_set(const char* type_id, bool locked, std::string* out_msg) {
  return protocol_lock_set_impl(type_id, locked, out_msg);
}

void protocol_lock_reset() { protocol_lock_reset_impl(); }

void roster_set_active(bool on) {
  const bool rising = on && !g_roster_active;
  g_roster_active = on;
  if (!rising || uprising_active()) return;
  // Each time 战况统计 opens: full-table snapshot of player object lists
  // (BFI-style), then Present amortizes TypeId classify. Clear the old live
  // table so mid-match open cannot show a stale / partial army.
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  g_live.clear();
  g_live_drop.clear();
  g_live_retries.clear();
  g_add_n = 0;
  g_forget_n = 0;
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  g_roster_seed_burst = true;
  g_seed_walk_done = false;
  g_seed_queue_overflow = false;
  g_live_seeded = false;
  g_roster_next_light = 0;
  g_roster_next_rediscover = 0;
  log("roster: stats opened — full scan armed");
}

bool roster_active() { return g_roster_active; }

bool roster_seed_pending() { return g_roster_active && (g_roster_seed_burst || !g_live_seeded); }

void roster_tick() {
  if (!g_live_cs_ready || uprising_active()) return;
  // Stats closed: only forget dead objects. Never classify / scan.
  if (!g_roster_active) {
    g_catalog_budget = 0;
    drain_live_queues(0, 24);
    return;
  }
  auto drain_timeboxed = [&](double budget_ms) {
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    g_catalog_budget = 8;
    for (int slice = 0; slice < 4; ++slice) {
      drain_live_queues(4, 8);
      QueryPerformanceCounter(&t1);
      const double ms = (freq.QuadPart > 0)
                            ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
                            : 0.0;
      if (ms >= budget_ms) break;
      int pending_now = 0;
      if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
      pending_now = g_add_n;
      if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
      if (pending_now == 0) break;
      g_catalog_budget = 4;
    }
  };

  if (g_roster_seed_burst || !g_live_seeded) {
    // Time-box classify so a hard template cannot hitch Present. ~2ms/frame.
    drain_timeboxed(2.0);
    int pending = 0;
    int live_n = 0;
    if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
    pending = g_add_n;
    live_n = (int)g_live.size();
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
    if (g_seed_walk_done && pending == 0) {
      if (g_seed_queue_overflow) {
        g_seed_queue_overflow = false;
        g_seed_walk_done = false;
        g_roster_next_light = 0;
        log("roster: seed queue overflow — another walk armed live=%d", live_n);
      } else {
        g_live_seeded = true;
        g_roster_seed_burst = false;
        // First reconcile soon after the open seed settles.
        g_roster_next_light = GetTickCount() + 2000u;
        log("roster: full scan done live=%d", live_n);
      }
    }
    return;
  }
  // After seed: if a reconcile walk enqueued unknowns, keep draining them with
  // the same time box — otherwise counts lag until the next hide/show.
  int pending = 0;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  pending = g_add_n;
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  if (pending > 0) {
    drain_timeboxed(2.0);
    return;
  }
  if (roster_early_load()) {
    g_catalog_budget = 1;
    drain_live_queues(1, 12);
    return;
  }
  // Keep Present light — list walks live in collect_match_economy (2s wall clock).
  g_catalog_budget = 3;
  drain_live_queues(2, 16);
}

void refresh_power_cooldowns() {
  clear_local_superweapon_cooldowns();
  // 协议限制：每帧把被禁止协议的就绪时间顶住（面板关着也要跑，否则关菜单就失效）。
  protocol_ban_tick();
}

bool disable_superweapon_held() { return g_sw_flag_held; }

bool build_lock_world_ready() { return g_lock_world_ready; }

void note_disable_superweapon_toggle(bool enabled) {
  if (!enabled) {
    g_sw_flag_held = false;
    return;
  }
  if (!g_lock_world_ready) {
    set_flag(0x0E, 0);
    g_sw_flag_held = true;
  }
}

void build_lock_sync_disable_superweapon() { sync_disable_superweapon_toggle(); }

int list_battle_players(BattlePlayer* out, int max_out) {
  return list_battle_players_impl(out, max_out);
}

void set_rank_player(uint32_t player) { g_rank_player = player; }

void set_mcv_player(uint32_t player) { g_mcv_player = player; }

bool summon_prepare(uint32_t player, int count, const char* const* type_ids, int type_count,
                    std::string* out_msg) {
  return summon_prepare_impl(player, count, type_ids, type_count, out_msg);
}

float summon_extra_lift() { return g_summon_lift_extra; }

void set_summon_extra_lift(float meters) {
  if (!(meters >= 0.f)) meters = 0.f;
  if (meters > 4000.f) meters = 4000.f;
  g_summon_lift_extra = meters;
}

bool summon_visible_fix() { return g_summon_visible_fix; }

void set_summon_visible_fix(bool on) { g_summon_visible_fix = on; }

void drain_unit_spawns() { drain_unit_spawns_impl(); }

const char* type_display_name(const char* type_id) { return lookup_zh_name(type_id); }

// 对象模板库（ThingFactory）的哈希表：factory+0x2C 是桶数组，factory+0x30 是桶数，
// 每个节点是 {名称指针, 模板指针, 下一个}。这就是引擎 FindTemplate 用的那张表，
// 里面登记了全部对象类型（含战役 / 特殊 / Mod 单位），不只是建造面板那批。
static std::vector<std::string> g_obj_names;
static uint32_t g_obj_names_factory = 0;
static uint32_t g_obj_names_buckets = 0;
static DWORD g_obj_names_last = 0;
static bool g_obj_names_force = false;

// 类型名形状：可见 ASCII、4~80 字符、带常见对象名前缀。
// 用来从模板内存里挖 id 时过滤掉描述文本 / 资源键。
static bool plausible_type_id(const char* s) {
  if (!s || !s[0]) return false;
  size_t len = 0;
  for (; s[len]; ++len) {
    const unsigned char c = (unsigned char)s[len];
    if (c <= 0x20 || c >= 0x7F) return false;
    if (len >= 80) return false;
  }
  if (len < 4) return false;
  static const char* kPrefix[] = {
      "Allied", "Soviet", "Japan", "Celestial", "Neutral", "Civilian",
      "OreNode", "OilDerrick", "Hospital", "Garage", "Observation",
      "ShipYard", "Airport", "Veterancy", "Crate", "Bonus", "Tech",
      "PlayerPower", "SpecialPower", "Generic", "Wall", "Garrison"};
  for (const char* p : kPrefix) {
    if (_strnicmp(s, p, std::strlen(p)) == 0) return true;
  }
  return false;
}

// 兜底：不同版本的 ThingFactory 哈希表布局可能不一样。布局对不上时改为遍历
// 模板链表（factory+0x24），从每个模板的前 0x180 字节里挖「像类型名」的串。
static std::vector<std::string> collect_type_ids_from_templates(uint32_t factory) {
  std::vector<std::string> out;
  uint32_t tmpl = 0;
  if (!safe_read_u32(factory + 0x24, &tmpl) || !is_ptr(tmpl)) return out;
  for (int n = 0; n < 4096 && is_ptr(tmpl); ++n) {
    for (uint32_t off = 0; off <= 0x180; off += 4) {
      uint32_t p = 0;
      if (!safe_read_u32(tmpl + off, &p) || !is_ptr(p)) continue;
      char buf[96] = {};
      if (!safe_read_bytes(p, buf, sizeof(buf) - 1)) continue;
      if (!plausible_type_id(buf)) continue;
      out.emplace_back(buf);
    }
    uint32_t next = 0;
    if (!safe_read_u32(tmpl + 0xC, &next) || next == tmpl) break;
    tmpl = next;
  }
  return out;
}

// 扫一个 store 的哈希桶，收集类型名。布局：+0x2C 桶数组 / +0x30 桶数，
// 节点 {key(名字指针), value(模板), next}。
static void walk_store_names(uint32_t store, std::vector<std::string>* out, uint32_t* out_count) {
  if (out_count) *out_count = 0;
  if (!out || !is_ptr(store)) return;
  uint32_t buckets = 0, count = 0;
  if (!safe_read_u32(store + 0x2C, &buckets) || !is_ptr(buckets) ||
      !safe_read_u32(store + 0x30, &count) || count == 0 || count > 100000) {
    return;
  }
  if (out_count) *out_count = count;
  int budget = 60000;   // 读预算：布局对不上时不至于在渲染线程上扫太久
  for (uint32_t i = 0; i < count && out->size() < 8192; ++i) {
    uint32_t node = 0;
    if (!safe_read_u32(buckets + i * 4u, &node)) continue;
    for (int guard = 0; is_ptr(node) && guard < 64; ++guard) {
      if (--budget <= 0) return;
      uint32_t key = 0, next = 0;
      if (safe_read_u32(node, &key) && is_ptr(key)) {
        char buf[96] = {};
        if (safe_read_bytes(key, buf, sizeof(buf) - 1)) {
          bool ok = true;
          size_t len = 0;
          for (; buf[len] && len < sizeof(buf) - 1; ++len) {
            const unsigned char c = (unsigned char)buf[len];
            if (c <= 0x20 || c >= 0x7F) {
              ok = false;
              break;
            }
          }
          if (ok && len >= 3) out->push_back(std::string(buf, len));
        }
      }
      if (!safe_read_u32(node + 8, &next) || next == node) break;
      node = next;
    }
  }
}

const std::vector<std::string>& object_type_names() {
  if (!module_base()) return g_obj_names;
  const DWORD now = GetTickCount();
  if (g_obj_names_force) g_obj_names_force = false;
  // 成功扫过：60 秒内复用。扫失败（列表还是空）：5 秒内不再重试，
  // 否则每次 UI 引用都会重扫一遍工厂表，进对局前会把帧时间拖垮。
  const bool ok_fresh =
      !g_obj_names.empty() && static_cast<int32_t>(now - g_obj_names_last) < 60000;
  const bool fail_fresh = g_obj_names.empty() &&
                          static_cast<int32_t>(now - g_obj_names_last) < 5000;
  if (ok_fresh || fail_fresh) return g_obj_names;
  g_obj_names_last = now;

  // 两个 store 都试：真 ThingFactory（引擎 FindTemplate 用的那个）里才有全部模板，
  // 另一个只有可建造的几百条——拿它当"名单"会把现成的特殊单位误判成不存在。
  discover_template_lookup();
  uint32_t stores[3] = {};
  stores[0] = g_tmpl_store;
  if (!safe_read_u32(module_base() + kTemplateStoreRva, &stores[1])) stores[1] = 0;
  if (!safe_read_u32(module_base() + kThingFactoryRva, &stores[2])) stores[2] = 0;

  std::vector<std::string> out;
  uint32_t used = 0;
  uint32_t used_count = 0;
  bool from_hash = false;
  for (int i = 0; i < 3; ++i) {
    if (!is_ptr(stores[i]) || stores[i] == used) continue;
    std::vector<std::string> tmp;
    uint32_t cnt = 0;
    walk_store_names(stores[i], &tmp, &cnt);
    std::sort(tmp.begin(), tmp.end());
    tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
    if (tmp.size() > out.size()) {
      out.swap(tmp);
      used = stores[i];
      used_count = cnt;
      from_hash = out.size() >= 16;
    }
  }
  if (!from_hash && is_ptr(used)) {
    // 哈希表没读到东西（这个版本的布局不是 +0x2C/+0x30）：退回扫模板链表。
    std::vector<std::string> alt = collect_type_ids_from_templates(used);
    std::sort(alt.begin(), alt.end());
    alt.erase(std::unique(alt.begin(), alt.end()), alt.end());
    if (alt.size() > out.size()) out.swap(alt);
  }
  const bool changed_layout = used_count != g_obj_names_buckets || g_obj_names.empty();
  g_obj_names.swap(out);
  g_obj_names_from_hash = from_hash;
  g_obj_names_factory = used;
  g_obj_names_buckets = used_count;
  if (changed_layout) {
    log("thing factory: %d object type names (buckets=%u, store=%08X)", (int)g_obj_names.size(),
        used_count, used);
  }
  return g_obj_names;
}

void refresh_object_type_names() {
  g_obj_names_force = true;
  g_obj_names.clear();
  object_type_names();
}

bool object_type_exists(const char* type_id) {
  if (!type_id || !type_id[0]) return false;
  // 首选哈希直查模板库：这才是引擎的判据，名单扫描（尤其退化的兜底扫描）
  // 会把现成可召唤的特殊单位误判成"没有"。
  if (template_find_by_hash(sage_hash_lower(type_id))) return true;
  const std::vector<std::string>& all = object_type_names();
  if (all.empty()) return false;
  // 名单是模板库哈希表里登记的全部类型名；这里只做精确匹配，不碰模板内存。
  return std::binary_search(all.begin(), all.end(), std::string(type_id));
}

bool object_type_list_ready() {
  return g_obj_names_from_hash && object_type_names().size() >= 500;
}

// 对局时钟（秒），供摄像机定时事件等使用。
bool battle_clock(float* seconds_out) {
  if (seconds_out) *seconds_out = 0.f;
  if (!loaded_module()) return false;
  return read_battle_clock(nullptr, seconds_out);
}

// 对局结束代次：离开对局后必须连续 500ms 都不在对局里，才自增一次。
// 结算/退出对局的头几帧 TheGameLogic、ThePlayerList、音频事件都还在拆，
// 这几帧里做任何"恢复"动作（改倍率、写 Lua 全局、遍历玩家列表）都可能踩到已经释放的对象。
uint32_t battle_left_generation() {
  static uint32_t s_epoch = 0;
  static bool s_was_in_battle = false;
  static bool s_fired = false;
  static DWORD s_left_at = 0;
  const bool in_battle = battle_clock(nullptr);
  const DWORD now = GetTickCount();
  if (in_battle) {
    s_was_in_battle = true;
    s_left_at = 0;
    s_fired = false;
    return s_epoch;
  }
  if (!s_was_in_battle || s_fired) return s_epoch;
  if (s_left_at == 0) {
    s_left_at = now ? now : 1;
    return s_epoch;
  }
  if (now - s_left_at < 500u) return s_epoch;
  s_fired = true;
  s_was_in_battle = false;
  s_left_at = 0;
  return ++s_epoch;
}

void set_money_player(uint32_t player) { g_money_player = player; }

int player_spawn_rank(uint32_t player) {
  if (!module_base()) return -2;
  return player_spawn_rank_impl(player);
}

bool set_spawn_rank(int level, std::string* out_msg) {
  return set_spawn_rank_impl(level, out_msg);
}

bool adjust_local_money(int delta, std::string* out_msg) {
  if (!loaded_module()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }
  uint32_t local = 0;
  if (module_base()) {
    local = local_owner_for_ops();
  } else if (uprising_active()) {
    uint32_t list = 0;
    if (safe_read_u32(loaded_module() + player_list_rva(), &list) && is_ptr(list)) {
      safe_read_u32(list + kPlayerListLocalOff, &local);
    }
    if (!is_ptr(local)) local = local_owner_for_ops();
  }
  if (!is_ptr(local)) {
    if (out_msg) *out_msg = u8"未找到己方玩家（请先进入对局）";
    return false;
  }
  return apply_money_delta(local, delta, out_msg);
}

bool adjust_selected_player_money(int delta, std::string* out_msg) {
  if (!loaded_module()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }
  if (!is_ptr(g_money_player)) {
    if (out_msg) *out_msg = u8"请先进入战局并选择玩家";
    return false;
  }
  BattlePlayer seats[16] = {};
  const int n = list_battle_players_impl(seats, 16);
  bool found = false;
  for (int i = 0; i < n; ++i) {
    if (seats[i].player == g_money_player) found = true;
  }
  if (!found) {
    if (out_msg) *out_msg = u8"请先进入战局并选择玩家";
    return false;
  }
  return apply_money_delta(g_money_player, delta, out_msg);
}

bool inspect_first_selected(UnitInspect* out) {
  if (!out) return false;
  *out = UnitInspect{};
  auto ents = selected_entities_stable();
  out->selected_total = (int)ents.size();
  if (ents.empty()) return false;

  uint32_t ent = ents[0];
  if (!is_ptr(ent)) return false;
  out->ent = ent;
  out->valid = true;

  uint32_t unit_data = 0;
  if (safe_read_u32(ent + 4, &unit_data) && is_ptr(unit_data)) {
    out->unit_data = unit_data;
    char sig_key[32] = {};
    if (resolve_type_id_from_template(unit_data, out->type_id, sizeof(out->type_id),
                                      sig_key, sizeof(sig_key))) {
      std::snprintf(out->name_key, sizeof(out->name_key), "%s", out->type_id);
    } else if (sig_key[0]) {
      // Stable fingerprint of template strings (survives restart).
      std::snprintf(out->name_key, sizeof(out->name_key), "%s", sig_key);
    } else {
      // Last resort: heap address — only useful within this session.
      std::snprintf(out->name_key, sizeof(out->name_key), "tpl_%08X", unit_data);
    }
    const char* zh = nullptr;
    if (out->type_id[0]) zh = lookup_zh_name(out->type_id);
    if (!zh && out->name_key[0]) zh = lookup_zh_name(out->name_key);
    if (zh) {
      std::snprintf(out->type_name, sizeof(out->type_name), "%s", zh);
    } else {
      out->type_name[0] = 0;
    }
  }

  uint32_t owner = 0;
  if (safe_read_u32(ent + 0x418, &owner) && is_ptr(owner)) {
    out->owner = owner;
    uint32_t local = local_owner_for_ops();
    out->is_mine = local && owner == local;
  }

  uint32_t hp = 0;
  if (safe_read_u32(ent + 0x33C, &hp) && is_ptr(hp)) {
    out->has_hp = true;
    out->hp_cur = read_f32(hp + 4);
    out->hp_disp = read_f32(hp + 0xC);
    out->hp_max = read_f32(hp + 0x10);
  }

  auto nodes = speed_nodes(ent);
  if (!nodes.empty()) {
    out->has_speed = true;
    out->speed = read_f32(nodes[0] + 8);
  }

  uint32_t tracker = 0;
  if (safe_read_u32(ent + 0x3CC, &tracker) && is_ptr(tracker)) {
    out->has_rank = true;
    uint32_t lvl = 0;
    if (safe_read_u32(tracker + 0x24, &lvl)) out->rank_level = lvl;
    out->xp = read_f32(tracker + 0x0C);
    out->xp_next = read_f32(tracker + 0x10);
    uint32_t sub = 0;
    if (safe_read_u32(tracker + 0x2C, &sub) && is_ptr(sub)) {
      out->damage_mult = read_f32(sub + 0x08);
    }
  }

  // Weapon damage *type* (armor matrix). Per-shot Damage floats live in
  // WeaponTemplate assets and are not mapped on the live entity yet.
  if (resolve_damage_type(out->unit_data, out->type_id, out->damage_type,
                          sizeof(out->damage_type), out->damage_type_zh,
                          sizeof(out->damage_type_zh))) {
    out->has_damage_type = true;
  }
  return true;
}

bool engine_run(const char* key, std::string* out_msg) {
  if (!ready() && std::strcmp(key, "speed_max") != 0) {
    // speed/hp don't need hooks but still need module; allow memory ops without hooks
  }
  if (!module_base()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }

  {
    bool handled = false;
    const bool ok = camera_engine_cmd(key, &handled, out_msg);
    if (handled) return ok;
  }

  {
    bool handled = false;
    const bool ok = time_control_engine_cmd(key, &handled, out_msg);
    if (handled) return ok;
  }

  {
    bool handled = false;
    const bool ok = lua_bridge_engine_cmd(key, &handled, out_msg);
    if (handled) return ok;
  }

  if (std::strcmp(key, "speed_max") == 0) return apply_speed("max", out_msg);
  if (std::strcmp(key, "speed_slow") == 0) return apply_speed("slow", out_msg);
  if (std::strcmp(key, "speed_freeze") == 0) return apply_speed("freeze", out_msg);
  if (std::strcmp(key, "speed_restore") == 0) return apply_speed("restore", out_msg);
  if (std::strcmp(key, "hp_max") == 0) return apply_hp("max", nullptr, out_msg);
  if (std::strcmp(key, "hp_min") == 0) return apply_hp("min", nullptr, out_msg);
  if (std::strcmp(key, "hp_normal") == 0) return apply_hp("normal", nullptr, out_msg);
  if (std::strcmp(key, "invincible") == 0) {
    // 按 `[` 只给当前选中加无敌（累加，不会把之前保护的单位解掉）；
    // 对同一批已锁的单位再按一次才解除这一批。
    return invincible_toggle_impl(out_msg);
  }
  if (std::strcmp(key, "airdrop") == 0) {
    return spawn_crate_impl(g_crate_sel_idx, g_crate_sel_count, 0.f, out_msg);
  }
  if (std::strcmp(key, "unit_kill") == 0) return kill_selected(out_msg);
  if (std::strcmp(key, "unit_rank") == 0) return rank_up(out_msg);
  if (std::strcmp(key, "convert_unit") == 0) return convert_selected(out_msg);
  if (std::strcmp(key, "damage_mult") == 0) return apply_damage_mult(out_msg);
  if (std::strcmp(key, "chaos_mode") == 0) return chaos_selected(out_msg);
  if (std::strcmp(key, "unit_clone") == 0 || std::strcmp(key, "spawn_unit") == 0) {
    return clone_selected(!is_spectator(), 1, out_msg);
  }
  if (std::strcmp(key, "clone_multi") == 0) return clone_selected(!is_spectator(), 5, out_msg);
  if (std::strcmp(key, "ore_convoy") == 0) return clone_selected(true, 8, out_msg);
  if (std::strcmp(key, "full_buff") == 0) {
    std::string a, b;
    bool ok1 = apply_hp("max", nullptr, &a);
    if (ready() && hook_is_installed("UnitAmmo")) {
      uint8_t prev = get_flag(0x12);
      set_flag(0x12, 1);
      Sleep(300);
      if (!prev) set_flag(0x12, 0);
    }
    bool ok2 = rank_up(&b);
    if (out_msg) *out_msg = a + u8"；" + b;
    return ok1 || ok2;
  }
  if (std::strcmp(key, "spawn_mcv") == 0) return spawn_one_mcv(out_msg);
  if (std::strcmp(key, "summon_troops") == 0) {
    std::string msg;
    const bool ok = summon_troops_impl(&msg);
    log("summon_troops %s: %s", ok ? "ok" : "failed", msg.c_str());
    if (out_msg) *out_msg = msg;
    return ok;
  }
  if (out_msg) *out_msg = u8"未知 engine 功能";
  return false;
}

const char* unit_names_store_path() {
  ensure_unit_names_cs();
  EnterCriticalSection(&g_unit_names_cs);
  load_unit_names_unlocked();
  LeaveCriticalSection(&g_unit_names_cs);
  return g_unit_names_path;
}

bool set_unit_display_name(const char* type_id, const char* display_name,
                           std::string* out_msg) {
  if (!type_id || !type_id[0]) {
    if (out_msg) *out_msg = u8"缺少单位代号";
    return false;
  }
  if (!display_name || !display_name[0]) {
    if (out_msg) *out_msg = u8"显示名不能为空";
    return false;
  }
  // trim
  std::string zh = display_name;
  while (!zh.empty() && (zh.back() == ' ' || zh.back() == '\r' || zh.back() == '\n'))
    zh.pop_back();
  while (!zh.empty() && zh.front() == ' ') zh.erase(zh.begin());
  if (zh.empty()) {
    if (out_msg) *out_msg = u8"显示名不能为空";
    return false;
  }
  if (zh.size() >= 90) {
    if (out_msg) *out_msg = u8"显示名过长";
    return false;
  }

  ensure_unit_names_cs();
  EnterCriticalSection(&g_unit_names_cs);
  load_unit_names_unlocked();
  g_unit_names[type_id] = zh;
  g_user_names[type_id] = zh;
  const bool ok = write_unit_names_file(g_user_names, g_unit_names_path);
  const std::string path = g_unit_names_path;
  const int count = (int)g_user_names.size();
  LeaveCriticalSection(&g_unit_names_cs);

  if (!ok) {
    if (out_msg) *out_msg = u8"写入本地种类名文件失败";
    return false;
  }
  log("unit name saved: %s => %s (%s, %d entries)", type_id, zh.c_str(),
      path.c_str(), count);
  if (out_msg) {
    if (std::strncmp(type_id, "tpl_", 4) == 0) {
      *out_msg = std::string(u8"已保存「") + zh +
                 u8"」（注意：tpl_ 地址键重启后失效，优先等解析出正式代号）";
    } else {
      *out_msg = std::string(u8"已保存「") + zh + u8"」到本地种类名表";
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 单位操作扩展（对应 CameraBridge 的「选择哪些单位 / 切换单位的队伍」）
// ---------------------------------------------------------------------------
namespace {

int g_unit_scope = kUnitScopeSelected;
uint32_t g_unit_scope_player = 0;  // 0 表示用本地玩家

// 「切换队伍 + 不启用 AI 操控」时被压成 0 速度的单位。旧实现只压不还原，
// 于是后来勾上「启用 AI 操控」再切换，单位还是不动。
std::unordered_set<uint32_t> g_team_frozen;

// 还原被上面压成 0 的速度。备份值不可信（<=0，例如第一次压之前字段就是 0）时
// 按原版正常速度 1.0 写回，否则单位会永远站在原地。返回是否真的写了。
bool thaw_speed_node(uint32_t node) {
  const float cur = read_f32(node + 8);
  if (std::fabs(cur) > 1e-4f) return false;  // 已经在动，别碰
  float backup = read_f32(node + 0x40);
  if (!(backup > 0.f && backup <= 5000.f)) backup = 1.f;
  return write_f32(node + 8, backup);
}

uint32_t ent_owner_of(uint32_t ent) {
  uint32_t ow = 0;
  if (!safe_read_u32(ent + ent_off(kEntOwner, kEntOwnerEp1), &ow)) return 0;
  return is_ptr(ow) ? ow : 0;
}

bool ent_in_selected(uint32_t ent, const std::vector<uint32_t>& sel) {
  for (uint32_t e : sel) {
    if (e == ent) return true;
  }
  return false;
}

}  // namespace

int unit_scope() { return g_unit_scope; }

void set_unit_scope(int scope) {
  if (scope < kUnitScopeSelected || scope > kUnitScopePlayer) scope = kUnitScopeSelected;
  g_unit_scope = scope;
}

uint32_t unit_scope_player() { return g_unit_scope_player; }

void set_unit_scope_player(uint32_t player) {
  g_unit_scope_player = is_ptr(player) ? player : 0;
}

int unit_collect_scope(uint32_t* out, int max_out) {
  if (!out || max_out <= 0) return 0;
  const std::vector<uint32_t> sel = selected_entities_stable(256);

  if (g_unit_scope == kUnitScopeSelected) {
    int n = 0;
    for (uint32_t e : sel) {
      if (n >= max_out) break;
      out[n++] = e;
    }
    return n;
  }

  // 其余范围都要走一份「全部活动单位」快照（g_live 由 CreateUnit/扫描填充）。
  std::vector<uint32_t> all;
  if (g_live_cs_ready) {
    EnterCriticalSection(&g_live_cs);
    all.reserve(g_live.size());
    for (const auto& kv : g_live) all.push_back(kv.first);
    LeaveCriticalSection(&g_live_cs);
  }

  const uint32_t want_player =
      is_ptr(g_unit_scope_player) ? g_unit_scope_player : local_owner_for_ops();

  int n = 0;
  for (uint32_t ent : all) {
    if (n >= max_out) break;
    if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
    switch (g_unit_scope) {
      case kUnitScopeUnselected:
        if (ent_in_selected(ent, sel)) continue;
        break;
      case kUnitScopePlayer:
        if (!is_ptr(want_player) || ent_owner_of(ent) != want_player) continue;
        break;
      case kUnitScopeAll:
      default:
        break;
    }
    out[n++] = ent;
  }
  // g_live 还没铺开时，「所有的单位」至少把当前选中纳入，便于用户看到反馈。
  if (n == 0 && g_unit_scope == kUnitScopeAll) {
    for (uint32_t e : sel) {
      if (n >= max_out) break;
      out[n++] = e;
    }
  }
  return n;
}

bool unit_change_team(uint32_t target_player, std::string* out_msg) {
  return unit_change_team_ex(target_player, true, out_msg);
}

bool unit_change_team_ex(uint32_t target_player, bool enable_ai, std::string* out_msg) {
  if (uprising_active()) {
    if (out_msg) *out_msg = u8"切换队伍：起义时刻暂未适配";
    return false;
  }
  if (!is_ptr(target_player) || !is_ptr(read_u32(target_player))) {
    if (out_msg) *out_msg = u8"切换队伍：目标玩家无效";
    return false;
  }
  uint32_t ents[512];
  const int n = unit_collect_scope(ents, 512);
  if (n <= 0) {
    if (out_msg) *out_msg = u8"切换队伍：当前范围内没有单位（先选中单位，或换个范围）";
    return false;
  }
  // 走引擎的 setTeam：它会同时改 +0x414(Team) / +0x418(Player)。只写 +0x418
  // 时引擎会按团队里的玩家把它覆盖回去，所以旧实现看起来「没生效」。
  const uint32_t team = player_team_of(target_player);
  if (!team) {
    log("unit team change: no Team found for player %08X, falling back to raw owner write",
        target_player);
  }
  int changed = 0, failed = 0;
  for (int i = 0; i < n; ++i) {
    const uint32_t ent = ents[i];
    if (ent_owner_of(ent) == target_player) continue;
    if (assign_entity_to_player(ent, target_player, team)) {
      ++changed;
    } else {
      ++failed;
    }
  }
  log("unit team change: team=%08X changed=%d failed=%d", team, changed, failed);
  // 勾了「启用 AI 操控」：把之前被压成 0 的速度还原，否则这些单位永远动不了。
  int thawed = 0, frozen = 0;
  if (enable_ai) {
    for (uint32_t ent : g_team_frozen) {
      if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
      for (uint32_t node : speed_nodes(ent)) {
        if (thaw_speed_node(node)) ++thawed;
      }
    }
    // 本次范围里也一起还原：可能上次压过但没记进名单（老版本留下的冻结）。
    for (int i = 0; i < n; ++i) {
      for (uint32_t node : speed_nodes(ents[i])) {
        if (thaw_speed_node(node)) ++thawed;
      }
    }
    g_team_frozen.clear();
  }
  // 不启用 AI 操控：改完归属顺手把速度压成 0，免得 AI 立刻把单位调走。
  // （这不是引擎级的「禁止 AI」，只是原地冻结，勾上「启用 AI 操控」即可还原。）
  if (!enable_ai && changed > 0) {
    for (int i = 0; i < n; ++i) {
      for (uint32_t node : speed_nodes(ents[i])) {
        if (set_speed_node(node, 0.f)) {
          ++frozen;
          g_team_frozen.insert(ents[i]);
        }
      }
    }
  }
  if (changed == 0 && frozen == 0 && thawed == 0) {
    if (out_msg) {
      if (!team) {
        *out_msg = u8"切换队伍：找不到该玩家的队伍（Team），已放弃以免把单位写坏";
      } else {
        *out_msg = u8"切换队伍：范围内单位已经属于该队伍";
      }
    }
    return false;
  }
  if (out_msg) {
    char buf[224];
    if (frozen > 0) {
      std::snprintf(buf, sizeof(buf), u8"已把 %d 个单位切换到目标队伍，并把 %d 处速度冻结为 0",
                    changed, frozen);
    } else if (thawed > 0 && changed > 0) {
      std::snprintf(buf, sizeof(buf), u8"已把 %d 个单位切换到目标队伍，并还原 %d 处速度（AI 可以调动了）",
                    changed, thawed);
    } else if (thawed > 0) {
      std::snprintf(buf, sizeof(buf), u8"已还原 %d 处被冻结的速度（AI 可以调动了）", thawed);
    } else {
      std::snprintf(buf, sizeof(buf), u8"已把 %d 个单位切换到目标队伍", changed);
    }
    if (failed > 0) {
      const size_t used = std::strlen(buf);
      std::snprintf(buf + used, sizeof(buf) - used, u8"（%d 个没改成功）", failed);
    }
    if (!team) {
      const size_t used = std::strlen(buf);
      std::snprintf(buf + used, sizeof(buf) - used,
                    u8"（没找到该玩家的 Team，只改了归属字段，可能被引擎覆盖）");
    }
    *out_msg = buf;
  }
  log("unit team change -> %08X (%d units, frozen %d, scope %d)", target_player, changed,
      frozen, g_unit_scope);
  return true;
}

// ---------------------------------------------------------------------------
// 「启用 AI 操控」勾回来时：把被冻结的速度还原。
// ---------------------------------------------------------------------------
bool unit_thaw_team_freeze(uint32_t target_player, std::string* out_msg) {
  (void)target_player;
  if (uprising_active()) {
    if (out_msg) *out_msg = u8"切换队伍：起义时刻暂未适配";
    return false;
  }
  int thawed = 0;
  for (uint32_t ent : g_team_frozen) {
    if (!looks_like_entity(ent) || !entity_alive(ent)) continue;
    for (uint32_t node : speed_nodes(ent)) {
      if (thaw_speed_node(node)) ++thawed;
    }
  }
  g_team_frozen.clear();
  // 名单里没记到的也顺手解开（老版本只压不还原，留下的冻结没人记得）。
  uint32_t ents[512];
  const int n = unit_collect_scope(ents, 512);
  for (int i = 0; i < n; ++i) {
    for (uint32_t node : speed_nodes(ents[i])) {
      if (thaw_speed_node(node)) ++thawed;
    }
  }
  if (thawed == 0) {
    if (out_msg) *out_msg = u8"没有需要还原的速度（范围内单位本来就能动）";
    return false;
  }
  if (out_msg) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), u8"已还原 %d 处被冻结的速度（AI 可以调动了）", thawed);
    *out_msg = buf;
  }
  log("unit team thaw -> %d nodes", thawed);
  return true;
}

}  // namespace game_api
