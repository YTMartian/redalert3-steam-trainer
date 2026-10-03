#include "game_api.h"
#include "game_api_internal.h"
#include "embedded_payload.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace game_api {
namespace {

constexpr uint32_t kModBase = 0x400000;
constexpr uint32_t kMgrRva = 0x8E08DC;
constexpr uint32_t kLocalPlayerRva = 0x8EDE2C;
constexpr size_t kMcSize = 0x3000;
constexpr size_t kMc2Size = 0x1000;
constexpr size_t kFlagsSize = 0x100;
constexpr size_t kIdbSize = 0x100;

char g_status[256] = "waiting...";
CRITICAL_SECTION g_log_cs;
bool g_cs_ready = false;
char g_log_path[MAX_PATH] = {};

uint32_t g_module = 0;
uint8_t* g_mc = nullptr;
uint8_t* g_mc2 = nullptr;
uint8_t* g_flags = nullptr;
uint8_t* g_idb = nullptr;
bool g_hooked = false;
bool g_spectate_mode = false;
bool g_auto_spec = true;

struct InstalledHook {
  const char* name;
  uint32_t va;
  uint8_t aob[16];
  uint8_t aob_len;
};
std::vector<InstalledHook> g_installed;

std::mutex g_api_mu;

void ensure_log_path() {
  if (g_log_path[0]) return;
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  std::snprintf(g_log_path, sizeof(g_log_path), "%sra3_overlay.log", temp);
}

uint32_t va_of(uint32_t va) { return g_module + (va - kModBase); }

bool is_ptr(uint32_t v) {
  return v >= 0x10000 && v < 0x7FFF0000u && (v & 3u) == 0;
}

bool write_code(void* addr, const void* data, size_t len) {
  DWORD old = 0;
  if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(addr, data, len);
  VirtualProtect(addr, len, old, &old);
  FlushInstructionCache(GetCurrentProcess(), addr, len);
  return true;
}

void apply_relocs(uint8_t* blob, size_t blob_len, const OverlayReloc* relocs,
                  int reloc_count, uint32_t bases[4]) {
  for (int i = 0; i < reloc_count; ++i) {
    const auto& r = relocs[i];
    if (r.offset + 4 > blob_len) continue;
    uint32_t value = bases[r.sym] + static_cast<uint32_t>(r.addend);
    std::memcpy(blob + r.offset, &value, 4);
  }
}

bool name_in_list(const char* name, const char* const* list, int n) {
  for (int i = 0; i < n; ++i) {
    if (std::strcmp(name, list[i]) == 0) return true;
  }
  return false;
}

bool hook_installed(const char* name) {
  for (auto& h : g_installed) {
    if (std::strcmp(h.name, name) == 0) return true;
  }
  return false;
}

const OverlayHookDef* find_hook_def(const char* name) {
  for (int i = 0; i < kOverlayHookCount; ++i) {
    if (std::strcmp(kOverlayHooks[i].name, name) == 0) return &kOverlayHooks[i];
  }
  return nullptr;
}

const char* patch_one_hook(const OverlayHookDef& def) {
  uint32_t hook_va = va_of(def.hook_va);
  uint32_t jmp_target = reinterpret_cast<uint32_t>(g_mc) + def.mc_label;
  int32_t rel = static_cast<int32_t>(jmp_target - (hook_va + 5));
  std::vector<uint8_t> patch(def.aob_len, 0x90);
  patch[0] = 0xE9;
  std::memcpy(patch.data() + 1, &rel, 4);
  if (!write_code(reinterpret_cast<void*>(hook_va), patch.data(), patch.size())) {
    return "write_code failed";
  }
  InstalledHook ih{};
  ih.name = def.name;
  ih.va = def.hook_va;
  ih.aob_len = def.aob_len;
  std::memcpy(ih.aob, def.aob, def.aob_len);
  g_installed.push_back(ih);
  return nullptr;
}

void restore_hooks(const char* const* only_names, int only_count) {
  std::vector<InstalledHook> kept;
  for (auto& h : g_installed) {
    bool drop = (only_names == nullptr);
    if (only_names) {
      drop = name_in_list(h.name, only_names, only_count);
    }
    if (drop) {
      write_code(reinterpret_cast<void*>(va_of(h.va)), h.aob, h.aob_len);
    } else {
      kept.push_back(h);
    }
  }
  g_installed.swap(kept);
}

bool probe_module() {
  const OverlayHookDef* probe = find_hook_def("PlayerID");
  if (!probe) return false;
  auto* addr = reinterpret_cast<const uint8_t*>(va_of(probe->hook_va));
  return std::memcmp(addr, probe->aob, probe->aob_len) == 0 || hook_installed("PlayerID");
}

}  // namespace

void install_roster_hooks();

void init() {
  if (!g_cs_ready) {
    InitializeCriticalSection(&g_log_cs);
    g_cs_ready = true;
  }
  ensure_log_path();
  DeleteFileA(g_log_path);
  log("DLL attached, log=%s", g_log_path);
  g_module = reinterpret_cast<uint32_t>(GetModuleHandleW(L"ra3_1.12.game"));
  if (!g_module) {
    g_module = kModBase;
    log("GetModuleHandle(ra3_1.12.game) failed — assuming 0x%X", kModBase);
  } else {
    log("module base 0x%X", g_module);
  }
  install_roster_hooks();
}

void shutdown() {
  detach();
  log("DLL detaching");
  if (g_cs_ready) {
    DeleteCriticalSection(&g_log_cs);
    g_cs_ready = false;
  }
}

const char* status_text() { return g_status; }

void set_status(const char* text) {
  if (!text) {
    g_status[0] = '\0';
    return;
  }
  std::strncpy(g_status, text, sizeof(g_status) - 1);
  g_status[sizeof(g_status) - 1] = '\0';
}

void log(const char* fmt, ...) {
  ensure_log_path();
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  set_status(line);
  if (g_cs_ready) EnterCriticalSection(&g_log_cs);
  FILE* f = std::fopen(g_log_path, "a");
  if (f) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(f, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, line);
    std::fclose(f);
  }
  if (g_cs_ready) LeaveCriticalSection(&g_log_cs);
}

bool ready() { return g_flags != nullptr && g_mc != nullptr; }
bool hooks_armed() { return g_hooked; }

bool is_spectator() {
  if (!g_module) return true;
  uint32_t player = *reinterpret_cast<uint32_t*>(g_module + kLocalPlayerRva);
  if (!is_ptr(player)) return true;
  uint32_t tmpl = *reinterpret_cast<uint32_t*>(player + 0x28);
  if (!is_ptr(tmpl)) return true;
  if (*reinterpret_cast<uint8_t*>(tmpl + 0x106) != 0) return true;
  if (*reinterpret_cast<uint8_t*>(tmpl + 0x123C) != 0) return true;
  return false;
}

const char* mode_label() {
  if (!ready()) return u8"未分配";
  if (!hooks_armed()) return u8"已分配(未启用Hook)";
  return g_spectate_mode ? u8"观战Hook" : u8"正常Hook";
}

static bool find_tool_path(char* out, size_t out_len, const wchar_t* leaf_name) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&find_tool_path), &self) ||
      !self) {
    return false;
  }
  wchar_t wpath[MAX_PATH] = {};
  if (!GetModuleFileNameW(self, wpath, MAX_PATH)) return false;
  wchar_t* slash = wcsrchr(wpath, L'\\');
  if (!slash) return false;
  *slash = 0;  // .../bin
  wchar_t candidate[MAX_PATH] = {};
  // Prefer beside DLL: overlay/bin/arm_mustcode.exe
  _snwprintf_s(candidate, _TRUNCATE, L"%s\\%s", wpath, leaf_name);
  if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
    WideCharToMultiByte(CP_UTF8, 0, candidate, -1, out, (int)out_len, nullptr, nullptr);
    return true;
  }
  // overlay/tools/arm_mustcode.exe or .py
  slash = wcsrchr(wpath, L'\\');
  if (!slash) return false;
  *slash = 0;
  _snwprintf_s(candidate, _TRUNCATE, L"%s\\tools\\%s", wpath, leaf_name);
  if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
    WideCharToMultiByte(CP_UTF8, 0, candidate, -1, out, (int)out_len, nullptr, nullptr);
    return true;
  }
  return false;
}

static bool run_arm_python(std::string* err_out) {
  char tool[MAX_PATH] = {};
  const bool is_exe = find_tool_path(tool, sizeof(tool), L"arm_mustcode.exe");
  if (!is_exe && !find_tool_path(tool, sizeof(tool), L"arm_mustcode.py")) {
    if (err_out) *err_out = "arm_mustcode.exe/.py not found in overlay/bin or tools";
    return false;
  }

  char result_path[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, result_path);
  std::strncat(result_path, "ra3_overlay_arm_result.txt",
               sizeof(result_path) - std::strlen(result_path) - 1);
  DeleteFileA(result_path);

  char cmd[1280];
  if (is_exe) {
    std::snprintf(cmd, sizeof(cmd),
                  "\"%s\" %lu 0x%lX 0x%lX 0x%lX 0x%lX %d", tool,
                  GetCurrentProcessId(),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_mc),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_mc2),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_flags),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_idb),
                  g_spectate_mode ? 1 : 0);
  } else {
    std::snprintf(cmd, sizeof(cmd),
                  "py -3 \"%s\" %lu 0x%lX 0x%lX 0x%lX 0x%lX %d", tool,
                  GetCurrentProcessId(),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_mc),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_mc2),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_flags),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_idb),
                  g_spectate_mode ? 1 : 0);
  }

  log("running: %s", cmd);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  BOOL created = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!created && !is_exe) {
    std::snprintf(cmd, sizeof(cmd),
                  "python \"%s\" %lu 0x%lX 0x%lX 0x%lX 0x%lX %d", tool,
                  GetCurrentProcessId(),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_mc),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_mc2),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_flags),
                  (unsigned long)reinterpret_cast<uintptr_t>(g_idb),
                  g_spectate_mode ? 1 : 0);
    created = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  }
  if (!created) {
    if (err_out) *err_out = "CreateProcess arm_mustcode failed";
    return false;
  }
  WaitForSingleObject(pi.hProcess, 90000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  char line[512] = {};
  FILE* f = std::fopen(result_path, "r");
  if (f) {
    if (std::fgets(line, sizeof(line), f)) {
      size_t n = std::strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    }
    std::fclose(f);
  }
  if (code != 0 || std::strncmp(line, "OK:", 3) != 0) {
    if (err_out) *err_out = line[0] ? line : "arm_mustcode failed";
    return false;
  }
  log("%s", line);
  return true;
}

bool inject(bool spectate_mode) {
  std::lock_guard<std::mutex> lock(g_api_mu);
  g_spectate_mode = spectate_mode;
  g_auto_spec = false;

  if (!g_module) {
    g_module = reinterpret_cast<uint32_t>(GetModuleHandleW(L"ra3_1.12.game"));
    if (!g_module) g_module = kModBase;
  }

  if (g_hooked) {
    restore_hooks(nullptr, 0);
    g_hooked = false;
  }

  if (!g_mc) {
    g_mc = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kMcSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    g_mc2 = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kMc2Size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    g_flags = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kFlagsSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_idb = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kIdbSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_mc || !g_mc2 || !g_flags || !g_idb) {
      log("VirtualAlloc failed for MustCode segments");
      return false;
    }
    log("alloc MC=%p MC2=%p FLAGS=%p IDB=%p", g_mc, g_mc2, g_flags, g_idb);
  }

  // Do NOT write the broken relocatable embedded blob. trainer.build() will
  // fill these pages when arm_hooks() runs (same path as trainer.py).
  std::memset(g_mc, 0xCC, kMcSize);
  std::memset(g_mc2, 0xCC, kMc2Size);
  std::memset(g_flags, 0, kFlagsSize);
  std::memset(g_idb, 0, kIdbSize);
  *reinterpret_cast<uint32_t*>(g_flags + 0x24) = 0x6586A5A0u;
  *reinterpret_cast<uint32_t*>(g_flags + 0x20) = 0;

  if (!probe_module()) {
    auto* probe = find_hook_def("PlayerID");
    uint32_t addr = va_of(probe->hook_va);
    log("module probe failed @ 0x%X — wrong game version?", addr);
    return false;
  }

  g_installed.clear();
  g_hooked = false;
  log("MustCode pages allocated (empty). Use arm_hooks via trainer.build().");
  return true;
}

bool arm_hooks() {
  std::lock_guard<std::mutex> lock(g_api_mu);
  if (!g_mc || !g_flags) {
    log("arm_hooks: allocate MustCode first");
    return false;
  }
  if (g_hooked) {
    restore_hooks(nullptr, 0);
    g_hooked = false;
  }

  std::string err;
  if (!run_arm_python(&err)) {
    log("arm_hooks failed: %s", err.c_str());
    return false;
  }

  // Track installed hooks for detach/restore (same set Python patched).
  g_installed.clear();
  for (int i = 0; i < kOverlayHookCount; ++i) {
    const auto& def = kOverlayHooks[i];
    if (g_spectate_mode &&
        name_in_list(def.name, kPlayerHookNames, kPlayerHookNameCount)) {
      continue;
    }
    InstalledHook ih{};
    ih.name = def.name;
    ih.va = def.hook_va;
    ih.aob_len = def.aob_len;
    std::memcpy(ih.aob, def.aob, def.aob_len);
    g_installed.push_back(ih);
  }
  g_hooked = true;
  log("hooks armed via trainer.build() count=%d", (int)g_installed.size());
  return true;
}

void detach() {
  std::lock_guard<std::mutex> lock(g_api_mu);
  if (g_hooked) {
    restore_hooks(nullptr, 0);
    g_hooked = false;
    log("hooks restored");
  }
}

uint8_t get_flag(uint32_t offset) {
  if (!g_flags || offset >= kFlagsSize) return 0;
  return g_flags[offset];
}

bool set_flag(uint32_t offset, uint8_t value) {
  if (!g_flags || offset >= kFlagsSize) return false;
  g_flags[offset] = value;
  return true;
}

bool pulse_flag(uint32_t offset, float hold_sec) {
  if (!g_flags) return false;
  uint8_t prev = g_flags[offset];
  g_flags[offset] = 1;
  Sleep(static_cast<DWORD>(hold_sec * 1000.0f));
  if (!prev) g_flags[offset] = 0;
  return true;
}

std::vector<uint32_t> selected_entities(int limit) {
  std::vector<uint32_t> out;
  if (!g_module) return out;
  uint32_t mgr = *reinterpret_cast<uint32_t*>(g_module + kMgrRva);
  if (!is_ptr(mgr)) return out;
  uint32_t count = *reinterpret_cast<uint32_t*>(mgr + 0x5C);
  uint32_t node = *reinterpret_cast<uint32_t*>(mgr + 0x50);
  int steps = (count > 0 && count <= (uint32_t)limit) ? (int)count : limit;
  uint32_t head = node;
  for (int i = 0; i < steps; ++i) {
    if (!is_ptr(node)) break;
    uint32_t obj = *reinterpret_cast<uint32_t*>(node + 8);
    if (is_ptr(obj)) {
      uint32_t ent = *reinterpret_cast<uint32_t*>(obj + 0x138);
      if (is_ptr(ent)) out.push_back(ent);
    }
    uint32_t nxt = *reinterpret_cast<uint32_t*>(node);
    if (!is_ptr(nxt) || nxt == head || nxt == node) break;
    node = nxt;
  }
  return out;
}

static std::vector<uint32_t> g_sel_cache;

void refresh_selection_cache() {
  auto live = selected_entities();
  if (!live.empty()) g_sel_cache = std::move(live);
}

std::vector<uint32_t> selected_entities_stable(int limit) {
  auto live = selected_entities(limit);
  if (!live.empty()) {
    g_sel_cache = live;
    return live;
  }
  return g_sel_cache;
}

int selected_count() {
  auto live = selected_entities();
  if (!live.empty()) return (int)live.size();
  return (int)g_sel_cache.size();
}

static DWORD WINAPI beep_thread(LPVOID param) {
  const char* kind = static_cast<const char*>(param);
  if (std::strcmp(kind, "on") == 0) {
    Beep(523, 70);
    Beep(784, 90);
  } else if (std::strcmp(kind, "off") == 0) {
    Beep(784, 70);
    Beep(523, 90);
  } else if (std::strcmp(kind, "error") == 0) {
    Beep(220, 140);
  } else {
    Beep(1000, 60);
  }
  return 0;
}

void beep(const char* kind) {
  // Copy kind string for thread lifetime (static literals are fine).
  const char* k = "click";
  if (kind) {
    if (std::strcmp(kind, "on") == 0) k = "on";
    else if (std::strcmp(kind, "off") == 0) k = "off";
    else if (std::strcmp(kind, "error") == 0) k = "error";
  }
  HANDLE th = CreateThread(nullptr, 0, beep_thread, (LPVOID)k, 0, nullptr);
  if (th) CloseHandle(th);
}

bool inject_full(bool spectate_mode) {
  if (!inject(spectate_mode)) return false;
  if (!arm_hooks()) return false;
  return true;
}

bool trigger_hotkey(const char* key, std::string* out_msg) {
  if (!key) return false;
  if (std::strcmp(key, "danger_max") == 0) {
    bool ok = set_danger(1, out_msg);
    beep(ok ? "click" : "error");
    return ok;
  }
  if (std::strcmp(key, "danger_min") == 0) {
    bool ok = set_danger(2, out_msg);
    beep(ok ? "click" : "error");
    return ok;
  }
  if (std::strcmp(key, "danger_norm") == 0) {
    bool ok = set_danger(0, out_msg);
    beep(ok ? "click" : "error");
    return ok;
  }
  const FeatureInfo* f = find_feature(key);
  if (!f) {
    if (out_msg) *out_msg = u8"未知热键";
    beep("error");
    return false;
  }
  if (std::strcmp(f->type, "toggle") == 0) {
    bool on = !feature_enabled(key);
    bool ok = toggle_feature(key, on, out_msg);
    beep(ok ? (on ? "on" : "off") : "error");
    return ok;
  }
  if (std::strcmp(f->type, "pulse") == 0) {
    bool ok = pulse_feature(key, out_msg);
    beep(ok ? "click" : "error");
    return ok;
  }
  if (std::strcmp(f->type, "money") == 0) {
    bool ok = adjust_local_money(money_self_step(), out_msg);
    beep(ok ? "click" : "error");
    return ok;
  }
  if (std::strcmp(f->type, "money_sel") == 0) {
    bool ok = adjust_selected_player_money(money_sel_step(), out_msg);
    beep(ok ? "click" : "error");
    return ok;
  }
  if (std::strcmp(f->type, "engine") == 0) {
    bool ok = run_engine(key, out_msg);
    beep(ok ? "click" : "error");
    return ok;
  }
  if (out_msg) *out_msg = u8"不支持的热键类型";
  beep("error");
  return false;
}

const char* hotkey_hint(const char* key) {
  if (!key) return nullptr;
  struct Hint { const char* key; const char* text; };
  static const Hint kHints[] = {
      {"money", "Ctrl+F1"}, {"power", "Ctrl+F2"}, {"scpoint", "Ctrl+F3"},
      {"haveallsc", "Ctrl+F4"}, {"fastbuild", "Ctrl+F5"}, {"superpower", "Ctrl+F6"},
      {"disableallsp", "Ctrl+F7"}, {"map", "Ctrl+F9"}, {"nocbuild", "Ctrl+F10"},
      {"ammo", ";"}, {"oremine", "'"}, {"danger", ", . /"},
      {"speed_max", "-"}, {"speed_slow", "="}, {"speed_freeze", "PgUp"},
      {"speed_restore", "PgDn"}, {"hp_max", "["}, {"hp_min", "]"},
      {"hp_normal", "\\"}, {"unit_rank", "P"}, {"unit_kill", "Del"},
      {"unit_clone", "I"},
  };
  for (auto& h : kHints) {
    if (std::strcmp(h.key, key) == 0) return h.text;
  }
  return nullptr;
}

void sync_spectator_hooks() {
  // Disabled: auto install/remove based on is_spectator() is unsafe because the
  // main menu can look like a valid local player, which armed hooks too early
  // and crashed on match start. Hooks are armed only via arm_hooks().
}

// engine_run is implemented in game_features.cpp

static const FeatureInfo kFeatures[] = {
    {"money", u8"己方资金", "money", 0},
    {"money_sel", u8"选中玩家资金", "money_sel", 0},
    {"power", u8"电力无限", "toggle", 0x09},
    {"scpoint", u8"科技点无限", "toggle", 0x0A},
    {"haveallsc", u8"全科技", "toggle", 0x0B},
    {"fastbuild", u8"快速建造", "toggle", 0x0C},
    {"oremine", u8"恢复矿场", "pulse", 0x14},
    {"superpower", u8"超级武器", "toggle", 0x0D},
    {"disableallsp", u8"禁用超武", "toggle", 0x0E},
    {"map", u8"全地图", "toggle", 0x11},
    {"nocbuild", u8"敌人无法建造", "toggle", 0x15},
    {"protocol_ready", u8"协议无冷却", "engine", 0},
    {"unit_skill_ready", u8"单位技能无冷却", "engine", 0},
    {"disable_protocol", u8"禁用敌方协议", "engine", 0},
    {"ammo", u8"弹药无限", "toggle", 0x12},
    {"danger", u8"危险等级", "danger", 0x13},
    {"speed_max", u8"超速 ×500", "engine", 0},
    {"speed_slow", u8"慢速 ×10", "engine", 0},
    {"speed_freeze", u8"冻结", "engine", 0},
    {"speed_restore", u8"恢复速度", "engine", 0},
    {"hp_max", u8"无敌", "engine", 0},
    {"hp_min", u8"残血(1点)", "engine", 0},
    {"hp_normal", u8"恢复血量", "engine", 0},
    {"unit_rank", u8"满级(3星)", "engine", 0},
    {"unit_kill", u8"摧毁选中", "engine", 0},
    {"unit_clone", u8"复制选中", "engine", 0},
    {"convert_unit", u8"收编敌方", "engine", 0},
    {"spawn_unit", u8"复制到己方", "engine", 0},
    {"clone_multi", u8"批量复制", "engine", 0},
    {"damage_mult", u8"伤害×5", "engine", 0},
    {"full_buff", u8"一键满状态", "engine", 0},
    {"enemy_weaken", u8"敌方残血", "engine", 0},
    {"ally_god", u8"友军无敌", "engine", 0},
    {"fog_toggle", u8"迷雾开关", "toggle", 0x11},
    {"spec_gift", u8"观战赠送单位", "engine", 0},
    {"chaos_mode", u8"混乱模式", "engine", 0},
    {"ore_convoy", u8"刷矿车车队", "engine", 0},
    {"spawn_mcv", u8"召唤基地车", "engine", 0},
};

static const char* kGroupRes[] = {"money", "money_sel", "power", "scpoint", "haveallsc", "fastbuild", "oremine"};
static const char* kGroupSw[] = {"superpower", "disableallsp", "map", "nocbuild",
                                 "protocol_ready", "unit_skill_ready", "disable_protocol"};
static const char* kGroupAmmo[] = {"ammo", "danger"};
static const char* kGroupUnit[] = {
    "speed_max", "speed_slow", "speed_freeze", "speed_restore", "hp_max", "hp_min", "hp_normal",
    "unit_rank", "unit_kill", "unit_clone", "convert_unit", "spawn_unit", "clone_multi",
    "damage_mult", "full_buff"};
static const char* kGroupBattle[] = {"enemy_weaken", "ally_god"};
static const char* kGroupIntel[] = {"fog_toggle"};
static const char* kGroupSpec[] = {"spec_gift"};
static const char* kGroupFun[] = {"chaos_mode", "ore_convoy", "spawn_mcv"};

static const GroupInfo kGroups[] = {
    {u8"资源", kGroupRes, 7, u8"己方默认 +10万；选中玩家单位后可加减其资金（默认 1万）"},
    {u8"超武 / 地图", kGroupSw, 7, nullptr},
    {u8"弹药 / 危险", kGroupAmmo, 2, nullptr},
    {u8"单位操作", kGroupUnit, 15, u8"需先在游戏里选中单位"},
    {u8"战场", kGroupBattle, 2, nullptr},
    {u8"情报", kGroupIntel, 1, nullptr},
    {u8"观战", kGroupSpec, 1, u8"建议使用观战模式注入"},
    {u8"趣味", kGroupFun, 3, u8"娱乐向；召唤基地车可能仍不稳定"},
};

const FeatureInfo* features(int* count) {
  if (count) *count = (int)(sizeof(kFeatures) / sizeof(kFeatures[0]));
  return kFeatures;
}

const FeatureInfo* find_feature(const char* key) {
  int n = 0;
  const FeatureInfo* all = features(&n);
  for (int i = 0; i < n; ++i) {
    if (std::strcmp(all[i].key, key) == 0) return &all[i];
  }
  return nullptr;
}

const GroupInfo* groups(int* count) {
  if (count) *count = (int)(sizeof(kGroups) / sizeof(kGroups[0]));
  return kGroups;
}

bool feature_enabled(const char* key) {
  const FeatureInfo* f = find_feature(key);
  if (!f || std::strcmp(f->type, "toggle") != 0) return false;
  return get_flag(f->flag) != 0;
}

bool toggle_feature(const char* key, bool enabled, std::string* out_msg) {
  const FeatureInfo* f = find_feature(key);
  if (!f || std::strcmp(f->type, "toggle") != 0) {
    if (out_msg) *out_msg = u8"不是开关类功能";
    return false;
  }
  if (!ready() || !hooks_armed()) {
    if (out_msg) *out_msg = u8"请先点击「注入」";
    return false;
  }
  if (!set_flag(f->flag, enabled ? 1 : 0)) {
    if (out_msg) *out_msg = u8"写 flag 失败";
    return false;
  }
  if (std::strcmp(key, "disableallsp") == 0) {
    build_lock_sync_disable_superweapon();
  }
  if (out_msg) {
    *out_msg = std::string(f->label) + (enabled ? u8"：开" : u8"：关");
  }
  return true;
}

bool pulse_feature(const char* key, std::string* out_msg) {
  const FeatureInfo* f = find_feature(key);
  if (!f || std::strcmp(f->type, "pulse") != 0) {
    if (out_msg) *out_msg = u8"不是脉冲类功能";
    return false;
  }
  if (!ready() || !hooks_armed()) {
    if (out_msg) *out_msg = u8"请先点击「注入」";
    return false;
  }
  if (!set_flag(f->flag, 1)) {
    if (out_msg) *out_msg = u8"写 flag 失败";
    return false;
  }
  if (out_msg) *out_msg = std::string(f->label) + u8"：已触发";
  return true;
}

bool set_danger(int level, std::string* out_msg) {
  if (!ready() || !hooks_armed()) {
    if (out_msg) *out_msg = u8"请先点击「注入」";
    return false;
  }
  if (level < 0) level = 0;
  if (level > 2) level = 2;
  if (!set_flag(0x13, (uint8_t)level)) {
    if (out_msg) *out_msg = u8"写危险等级失败";
    return false;
  }
  static const char* names[] = {u8"正常", u8"高", u8"最高"};
  if (out_msg) *out_msg = std::string(u8"危险等级：") + names[level];
  return true;
}

bool run_engine(const char* key, std::string* out_msg) {
  return engine_run(key, out_msg);
}

// Accessors used by game_features.cpp
static int clamp_money_step(int v) {
  if (v < 1000) return 1000;
  if (v > 50000000) return 50000000;
  return v;
}

static int g_money_self_step = 100000;
static int g_money_sel_step = 10000;

int money_self_step() { return g_money_self_step; }
int money_sel_step() { return g_money_sel_step; }
void set_money_self_step(int v) { g_money_self_step = clamp_money_step(v); }
void set_money_sel_step(int v) { g_money_sel_step = clamp_money_step(v); }

uint8_t* flags_base() { return g_flags; }
uint8_t* mc_base() { return g_mc; }
uint8_t* idb_base() { return g_idb; }
uint32_t module_base() { return g_module; }
bool hook_is_installed(const char* name) { return hook_installed(name); }

}  // namespace game_api
