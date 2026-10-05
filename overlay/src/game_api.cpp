#include "game_api.h"
#include "game_api_internal.h"
#include "embedded_payload.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// 0x416740 returns whatever the game pool allocated. On a failed grow that
// pointer is NULL, and 0xA01B70 then reads [NULL-4]. Substitute a process-heap
// block so the caller still receives a real pointer. Hooks stay armed.
extern "C" void* g_ra3_pool_resume = nullptr;
extern "C" void* g_ra3_pool_hdr = nullptr;
extern "C" void* g_ra3_free_resume = nullptr;
extern "C" void* g_ra3_free_skip = nullptr;

struct PoolFb {
  uint32_t user;
  PoolFb* next;
};
static PoolFb* g_pool_fb[4096] = {};
static CRITICAL_SECTION g_pool_fb_cs;
static bool g_pool_fb_ready = false;

static int pool_fb_slot(uint32_t user) { return static_cast<int>((user >> 4) & 4095u); }

extern "C" uint32_t ra3_pool_fallback(uint32_t size) {
  if (!g_pool_fb_ready || size == 0 || size > 0x1000000u) return 0;
  uint32_t bytes = (size + 4u + 7u) & ~7u;
  if (bytes < 16u) bytes = 16u;
  auto* raw = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
  auto* node = static_cast<PoolFb*>(HeapAlloc(GetProcessHeap(), 0, sizeof(PoolFb)));
  if (!raw || !node) {
    if (raw) HeapFree(GetProcessHeap(), 0, raw);
    if (node) HeapFree(GetProcessHeap(), 0, node);
    return 0;
  }
  *reinterpret_cast<uint32_t*>(raw) = bytes & 0x7ffffff8u;
  const uint32_t user = reinterpret_cast<uint32_t>(raw + 4);
  node->user = user;
  EnterCriticalSection(&g_pool_fb_cs);
  const int slot = pool_fb_slot(user);
  node->next = g_pool_fb[slot];
  g_pool_fb[slot] = node;
  LeaveCriticalSection(&g_pool_fb_cs);
  static volatile LONG reported = 0;
  if (InterlockedIncrement(&reported) <= 6) {
    game_api::log("pool returned null, substituted %u bytes", size);
  }
  return user;
}

extern "C" int ra3_pool_take_fallback(uint32_t user) {
  if (!g_pool_fb_ready || user == 0) return 0;
  EnterCriticalSection(&g_pool_fb_cs);
  const int slot = pool_fb_slot(user);
  PoolFb** link = &g_pool_fb[slot];
  while (*link) {
    if ((*link)->user == user) {
      PoolFb* dead = *link;
      *link = dead->next;
      LeaveCriticalSection(&g_pool_fb_cs);
      HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(user - 4));
      HeapFree(GetProcessHeap(), 0, dead);
      return 1;
    }
    link = &(*link)->next;
  }
  LeaveCriticalSection(&g_pool_fb_cs);
  return 0;
}

extern "C" void __declspec(naked) ra3_pool_null_stub() {
  __asm {
    test eax, eax
    jz pool_fallback_path
    mov esi, eax
    push 1
    push esi
    call dword ptr [g_ra3_pool_hdr]
    jmp dword ptr [g_ra3_pool_resume]
  pool_fallback_path:
    push ecx
    mov eax, dword ptr [esp + 10h]
    push eax
    call ra3_pool_fallback
    add esp, 4
    pop ecx
    test eax, eax
    jz pool_give_up
    mov esi, eax
    push 1
    push esi
    mov ecx, dword ptr [0x00CD7550]
    call dword ptr [g_ra3_pool_hdr]
    jmp dword ptr [g_ra3_pool_resume]
  pool_give_up:
    xor esi, esi
    xor eax, eax
    jmp dword ptr [g_ra3_pool_resume]
  }
}

extern "C" void __declspec(naked) ra3_pool_free_stub() {
  __asm {
    mov esi, dword ptr [esp + 0x0c]
    test esi, esi
    jz pool_free_skip
    push esi
    call ra3_pool_take_fallback
    add esp, 4
    test eax, eax
    jnz pool_free_skip
    jmp dword ptr [g_ra3_free_resume]
  pool_free_skip:
    jmp dword ptr [g_ra3_free_skip]
  }
}

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
  HMODULE self = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&ensure_log_path), &self) &&
      self) {
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(self, path, MAX_PATH)) {
      char* slash = std::strrchr(path, '\\');
      if (slash) {
        slash[1] = 0;
        std::snprintf(g_log_path, sizeof(g_log_path), "%sra3_overlay.log", path);
        return;
      }
    }
  }
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

void install_pool_null_guard() {
  if (!g_module) return;
  if (!g_pool_fb_ready) {
    InitializeCriticalSection(&g_pool_fb_cs);
    g_pool_fb_ready = true;
  }
  auto* site = reinterpret_cast<uint8_t*>(g_module + (0x416797u - kModBase));
  const uint8_t expect[] = {0x8B, 0xF0, 0x6A, 0x01, 0x56, 0xE8};
  if (std::memcmp(site, expect, sizeof(expect)) != 0) {
    log("pool guard skipped, allocator bytes differ");
    return;
  }
  auto* free_site = reinterpret_cast<uint8_t*>(g_module + (0x416A10u - kModBase));
  const uint8_t free_expect[] = {0x8B, 0x74, 0x24, 0x0C, 0x85, 0xF6, 0x74, 0x26};
  if (std::memcmp(free_site, free_expect, sizeof(free_expect)) != 0) {
    log("pool guard skipped, free bytes differ");
    return;
  }
  g_ra3_pool_resume = reinterpret_cast<void*>(g_module + (0x4167A1u - kModBase));
  g_ra3_pool_hdr = reinterpret_cast<void*>(g_module + (0xA01B70u - kModBase));
  g_ra3_free_resume = reinterpret_cast<void*>(g_module + (0x416A18u - kModBase));
  g_ra3_free_skip = reinterpret_cast<void*>(g_module + (0x416A3Eu - kModBase));

  const auto stub = reinterpret_cast<uintptr_t>(&ra3_pool_null_stub);
  const int32_t rel = static_cast<int32_t>(stub - (reinterpret_cast<uintptr_t>(site) + 5));
  uint8_t patch[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
  std::memcpy(patch + 1, &rel, 4);
  if (!write_code(site, patch, sizeof(patch))) {
    log("pool guard write failed");
    return;
  }

  const auto free_stub = reinterpret_cast<uintptr_t>(&ra3_pool_free_stub);
  const int32_t free_rel =
      static_cast<int32_t>(free_stub - (reinterpret_cast<uintptr_t>(free_site) + 5));
  uint8_t free_patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
  std::memcpy(free_patch + 1, &free_rel, 4);
  if (!write_code(free_site, free_patch, sizeof(free_patch))) {
    log("pool free guard write failed");
    return;
  }
  log("pool null guard at %p", site);
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
  install_crash_filter();
  g_module = reinterpret_cast<uint32_t>(GetModuleHandleW(L"ra3_1.12.game"));
  if (!g_module) {
    g_module = kModBase;
    log("GetModuleHandle(ra3_1.12.game) failed — assuming 0x%X", kModBase);
  } else {
    log("module base 0x%X", g_module);
  }
  install_pool_null_guard();
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

namespace {

volatile LONG g_crash_reports = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_crash_filter = nullptr;

bool crash_read_u32(uint32_t addr, uint32_t* out) {
  if (!out || addr < 0x10000u) return false;
  __try {
    *out = *reinterpret_cast<const volatile uint32_t*>(addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void format_code_addr(uint32_t addr, char* buf, size_t n) {
  if (!buf || n < 12) return;
  buf[0] = 0;
  HMODULE mod = nullptr;
  if (!addr ||
      !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(addr)), &mod) ||
      !mod) {
    std::snprintf(buf, n, "%08X", addr);
    return;
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(mod, path, MAX_PATH);
  const char* base = path;
  for (const char* p = path; *p; ++p) {
    if (*p == '\\' || *p == '/') base = p + 1;
  }
  const auto base_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mod));
  std::snprintf(buf, n, "%s+%08X", base[0] ? base : "mod", addr - base_addr);
}

const char* exception_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:
      return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "DIVIDE_BY_ZERO";
    case 0xC0000374:
      return "HEAP_CORRUPTION";
    case 0xE06D7363:
      return "CXX_EXCEPTION";
    default:
      return "EXCEPTION";
  }
}

void crash_file_write(const char* text) {
  if (!text || !text[0]) return;
  ensure_log_path();
  HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &wrote, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
}

LONG WINAPI unhandled_crash_filter(EXCEPTION_POINTERS* ep) {
  log_exception(ep, "unhandled");
  if (g_prev_crash_filter && g_prev_crash_filter != unhandled_crash_filter) {
    return g_prev_crash_filter(ep);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_filter() {
  LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(unhandled_crash_filter);
  if (cur && cur != unhandled_crash_filter) g_prev_crash_filter = cur;
}

void log_exception(void* ep_void, const char* where) {
  if (InterlockedIncrement(&g_crash_reports) > 6) return;
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char block[4096];
  size_t used = 0;
  auto add = [&](const char* line) {
    if (!line || used + 2 >= sizeof(block)) return;
    const size_t len = std::strlen(line);
    const size_t room = sizeof(block) - 1 - used;
    const size_t n = len < room ? len : room;
    std::memcpy(block + used, line, n);
    used += n;
    if (used + 1 < sizeof(block)) block[used++] = '\n';
    block[used] = 0;
  };
  char line[320];
  std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] CRASH %s", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, where && where[0] ? where : "unknown");
  add(line);

  auto* ep = static_cast<EXCEPTION_POINTERS*>(ep_void);
  if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
    add("  (no exception context)");
    crash_file_write(block);
    return;
  }
  const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
  const CONTEXT* ctx = ep->ContextRecord;
  char at[160];
  format_code_addr(static_cast<uint32_t>(ctx->Eip), at, sizeof(at));
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const char* rw = rec->ExceptionInformation[0] ? "write" : "read";
    char fault[160];
    format_code_addr(static_cast<uint32_t>(rec->ExceptionInformation[1]), fault, sizeof(fault));
    std::snprintf(line, sizeof(line), "  code=%08X %s %s addr=%s eip=%s esp=%08X ebp=%08X",
                  rec->ExceptionCode, exception_name(rec->ExceptionCode), rw, fault, at, ctx->Esp,
                  ctx->Ebp);
  } else {
    std::snprintf(line, sizeof(line), "  code=%08X %s eip=%s esp=%08X ebp=%08X", rec->ExceptionCode,
                  exception_name(rec->ExceptionCode), at, ctx->Esp, ctx->Ebp);
  }
  add(line);

  int frame = 0;
  uint32_t ebp = ctx->Ebp;
  uint32_t eip = ctx->Eip;
  for (int guard = 0; guard < 24 && eip >= 0x10000u; ++guard) {
    format_code_addr(eip, at, sizeof(at));
    std::snprintf(line, sizeof(line), "  #%02d %s", frame++, at);
    add(line);
    uint32_t next = 0;
    uint32_t ret = 0;
    if (!crash_read_u32(ebp, &next) || !crash_read_u32(ebp + 4, &ret)) break;
    if (next <= ebp || next > ebp + 0x10000u) break;
    ebp = next;
    eip = ret;
  }

  int slots = 0;
  for (int i = 0; i < 48 && slots < 12; ++i) {
    uint32_t word = 0;
    if (!crash_read_u32(ctx->Esp + static_cast<uint32_t>(i) * 4u, &word)) break;
    HMODULE mod = nullptr;
    if (!word ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(word)), &mod) ||
        !mod) {
      continue;
    }
    format_code_addr(word, at, sizeof(at));
    std::snprintf(line, sizeof(line), "  esp+%02X %s", i * 4, at);
    add(line);
    ++slots;
  }
  crash_file_write(block);
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

bool battlenet_client_running() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"Ra3.BattleNet.Client.exe") == 0 ||
          _wcsicmp(pe.szExeFile, L"RA3BattleNet.exe") == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

bool inject(bool spectate_mode) {
  std::lock_guard<std::mutex> lock(g_api_mu);
  if (battlenet_client_running()) {
    set_status(u8"请关闭战网进程（RA3BattleNet）");
    log("inject refused: RA3BattleNet is running");
    return false;
  }
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

  // Do NOT write the broken relocatable embedded blob. arm_mustcode fills
  // these pages when arm_hooks() runs.
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
  log("MustCode pages allocated (empty). Use arm_hooks.");
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
  log("hooks armed count=%d", (int)g_installed.size());
  return true;
}

void detach() {
  std::lock_guard<std::mutex> lock(g_api_mu);
  if (g_hooked) {
    restore_hooks(nullptr, 0);
    g_hooked = false;
    log("hooks restored");
  }
  sync_dispel_shroud();
}

// Gameplay visibility reads the per-cell counter, but the fog graphic only
// updates when a cell transitions out of fog inside 0xB21C20. Writing the
// counter straight to "already visible" shows units and then swallows that
// transition, so the graphic stays and walking no longer clears it. Force
// every cell through the real reveal once. The circle painter also rejects
// cliffs (0xB21AB9) and a height scale can zero the radius (0x73AEFD /
// 0x73AFBD); skip both while the toggle is on. Saved counters are restored
// on the way out, and the linked shroud chunks are marked dirty so the game
// repaints fog from those counters.
static void patch_site(uint32_t va, const uint8_t* orig, const uint8_t* repl, int n, bool on) {
  auto* site = reinterpret_cast<uint8_t*>(va_of(va));
  const uint8_t* want = on ? repl : orig;
  if (std::memcmp(site, want, n) == 0) return;
  if (std::memcmp(site, orig, n) != 0 && std::memcmp(site, repl, n) != 0) return;
  write_code(site, want, n);
}

static void patch_shroud_filters(bool on) {
  const uint8_t jbe_cell[] = {0x76, 0x08};
  const uint8_t nop_cell[] = {0x90, 0x90};
  const uint8_t jbe_lo[] = {0x76, 0x49};
  const uint8_t jmp_lo[] = {0xEB, 0x49};
  const uint8_t jbe_hi[] = {0x76, 0x4B};
  const uint8_t jmp_hi[] = {0xEB, 0x4B};
  patch_site(0xB21AB9, jbe_cell, nop_cell, 2, on);
  patch_site(0x73AEFD, jbe_lo, jmp_lo, 2, on);
  patch_site(0x73AFBD, jbe_hi, jmp_hi, 2, on);
}

static int viewed_shroud_player(uint32_t inner) {
  int player = *reinterpret_cast<int*>(inner + 0x80);
  if (player >= 0 && player < 0x14) return player;
  uint32_t list = *reinterpret_cast<uint32_t*>(va_of(0xCEDE2C));
  if (!is_ptr(list)) return -1;
  uint32_t local = *reinterpret_cast<uint32_t*>(list + 0x28);
  if (!is_ptr(local)) return -1;
  player = *reinterpret_cast<int*>(local + 0x20);
  if (player < 0 || player >= 0x14) return -1;
  return player;
}

static uint16_t* shroud_word(uint8_t* cells, uint32_t index, int player) {
  return reinterpret_cast<uint16_t*>(cells + index * 0x34u + 4 + player * 2);
}

static void dirty_cell_shroud(uint8_t* cell, int player) {
  __try {
    uint32_t node = *reinterpret_cast<uint32_t*>(cell);
    for (int n = 0; n < 32 && is_ptr(node); ++n) {
      uint32_t obj = *reinterpret_cast<uint32_t*>(node + 4);
      if (is_ptr(obj))
        *reinterpret_cast<uint32_t*>(obj + static_cast<uint32_t>(player) * 4u + 0x24) = 0;
      uint32_t next = *reinterpret_cast<uint32_t*>(node + 0xC);
      if (next == node) break;
      node = next;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static int copy_shroud_words(uint8_t* cells, uint32_t count, int player, uint16_t* snap) {
  __try {
    for (uint32_t i = 0; i < count; ++i) snap[i] = *shroud_word(cells, i, player);
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// 0xB21C20 updates the fog graphic only when the cell was not already visible.
// Drop the counter to fog first, then let the game reveal the whole grid.
static int force_reveal_grid(uint8_t* cells, uint32_t count, uint32_t inner, int player) {
  __try {
    for (uint32_t i = 0; i < count; ++i) *shroud_word(cells, i, player) = 0;
    auto fn = reinterpret_cast<void(__thiscall*)(void*, int)>(va_of(0xB22500));
    fn(reinterpret_cast<void*>(inner), player);
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

static int top_up_reveal(uint8_t* cells, uint32_t count, uint32_t inner, int player) {
  auto fn = reinterpret_cast<void(__thiscall*)(void*, void*, int)>(va_of(0xB21C20));
  __try {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t* word = shroud_word(cells, i, player);
      if (*word != 0 && *word != 0xFFFF) continue;
      *word = 0;
      fn(cells + i * 0x34u, reinterpret_cast<void*>(inner), player);
    }
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

static int restore_shroud_cells(uint8_t* cells, uint32_t count, int player,
                                const uint16_t* snap) {
  __try {
    for (uint32_t i = 0; i < count; ++i) {
      *shroud_word(cells, i, player) = snap[i];
      dirty_cell_shroud(cells + i * 0x34u, player);
    }
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

void sync_dispel_shroud() {
  if (!g_module) return;
  const bool on = g_hooked && g_flags && g_flags[0x11] != 0;
  patch_shroud_filters(on);

  static bool applied = false;
  static uint32_t snap_cells = 0;
  static int snap_player = -1;
  static uint32_t next_pass = 0;
  static std::vector<uint16_t> snap;

  uint32_t mgr = *reinterpret_cast<uint32_t*>(va_of(0xCE8130));
  uint32_t inner = 0;
  uint32_t cells = 0;
  uint32_t count = 0;
  int player = -1;
  if (is_ptr(mgr)) {
    inner = *reinterpret_cast<uint32_t*>(mgr + 0x28);
    if (is_ptr(inner)) {
      uint32_t w = *reinterpret_cast<uint32_t*>(inner + 0x3C);
      uint32_t h = *reinterpret_cast<uint32_t*>(inner + 0x40);
      cells = *reinterpret_cast<uint32_t*>(inner + 0x44);
      if (is_ptr(cells) && w > 0 && h > 0 && w <= 2048 && h <= 2048)
        count = w * h;
      else
        cells = 0;
      if (cells) player = viewed_shroud_player(inner);
    }
  }

  if (!on) {
    if (!applied) return;
    if (cells && cells == snap_cells && player == snap_player && snap.size() == count)
      restore_shroud_cells(reinterpret_cast<uint8_t*>(cells), count, player, snap.data());
    snap.clear();
    snap_cells = 0;
    snap_player = -1;
    applied = false;
    return;
  }
  if (!cells || player < 0) return;

  const bool same = applied && cells == snap_cells && player == snap_player && snap.size() == count;
  if (applied && !same) {
    if (cells == snap_cells && snap_player >= 0 && snap.size() == count)
      restore_shroud_cells(reinterpret_cast<uint8_t*>(cells), count, snap_player, snap.data());
    snap.clear();
    applied = false;
  }
  const uint32_t now = GetTickCount();
  auto* base = reinterpret_cast<uint8_t*>(cells);
  if (!applied) {
    snap.assign(count, 0);
    if (!copy_shroud_words(base, count, player, snap.data())) {
      snap.clear();
      return;
    }
    if (!force_reveal_grid(base, count, inner, player)) {
      restore_shroud_cells(base, count, player, snap.data());
      snap.clear();
      return;
    }
    snap_cells = cells;
    snap_player = player;
    applied = true;
    next_pass = now;
    return;
  }
  if (now - next_pass >= 300) {
    top_up_reveal(base, count, inner, player);
    next_pass = now;
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

struct HotkeySlot {
  const char* id;
  const char* feature;
  int vk;
  bool ctrl;
  bool alt;
  bool shift;
};

static const HotkeySlot kDefaultHotkeys[] = {
    {"money", "money", VK_F1, true, false, false},
    {"power", "power", VK_F2, true, false, false},
    {"scpoint", "scpoint", VK_F3, true, false, false},
    {"haveallsc", "haveallsc", VK_F4, true, false, false},
    {"fastbuild", "fastbuild", VK_F5, true, false, false},
    {"superpower", "superpower", VK_F6, true, false, false},
    {"disableallsp", "disableallsp", VK_F7, true, false, false},
    {"map", "map", VK_F9, true, false, false},
    {"nocbuild", "nocbuild", VK_F10, true, false, false},
    {"ammo", "ammo", 0xBA, false, false, false},
    {"oremine", "oremine", 0xDE, false, false, false},
    {"danger_max", "danger", 0xBC, false, false, false},
    {"danger_min", "danger", 0xBE, false, false, false},
    {"danger_norm", "danger", 0xBF, false, false, false},
    {"speed_max", "speed_max", 0xBD, false, false, false},
    {"speed_slow", "speed_slow", 0xBB, false, false, false},
    {"speed_freeze", "speed_freeze", VK_PRIOR, false, false, false},
    {"speed_restore", "speed_restore", VK_NEXT, false, false, false},
    {"hp_max", "hp_max", 0xDB, false, false, false},
    {"hp_min", "hp_min", 0xDD, false, false, false},
    {"hp_normal", "hp_normal", 0xDC, false, false, false},
    {"unit_rank", "unit_rank", 'P', false, false, false},
    {"unit_kill", "unit_kill", VK_DELETE, false, false, false},
    {"unit_clone", "unit_clone", 'I', false, false, false},
};

static HotkeySlot g_hotkeys[32];
static int g_hotkey_n = 0;
static bool g_hotkey_capture = false;

static void ensure_hotkeys() {
  if (g_hotkey_n > 0) return;
  g_hotkey_n = (int)(sizeof(kDefaultHotkeys) / sizeof(kDefaultHotkeys[0]));
  if (g_hotkey_n > (int)(sizeof(g_hotkeys) / sizeof(g_hotkeys[0]))) {
    g_hotkey_n = (int)(sizeof(g_hotkeys) / sizeof(g_hotkeys[0]));
  }
  for (int i = 0; i < g_hotkey_n; ++i) g_hotkeys[i] = kDefaultHotkeys[i];
}

static bool vk_name(int vk, char* buf, size_t n) {
  if (!buf || n < 2) return false;
  buf[0] = 0;
  if (vk >= VK_F1 && vk <= VK_F24) {
    std::snprintf(buf, n, "F%d", vk - VK_F1 + 1);
    return true;
  }
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
    std::snprintf(buf, n, "%c", (char)vk);
    return true;
  }
  switch (vk) {
    case VK_PRIOR: std::snprintf(buf, n, "PgUp"); return true;
    case VK_NEXT: std::snprintf(buf, n, "PgDn"); return true;
    case VK_DELETE: std::snprintf(buf, n, "Del"); return true;
    case VK_INSERT: std::snprintf(buf, n, "Ins"); return true;
    case VK_HOME: std::snprintf(buf, n, "Home"); return true;
    case VK_END: std::snprintf(buf, n, "End"); return true;
    case VK_SPACE: std::snprintf(buf, n, "Space"); return true;
    case VK_TAB: std::snprintf(buf, n, "Tab"); return true;
    case VK_UP: std::snprintf(buf, n, "Up"); return true;
    case VK_DOWN: std::snprintf(buf, n, "Down"); return true;
    case VK_LEFT: std::snprintf(buf, n, "Left"); return true;
    case VK_RIGHT: std::snprintf(buf, n, "Right"); return true;
    case 0xBA: std::snprintf(buf, n, ";"); return true;
    case 0xBB: std::snprintf(buf, n, "="); return true;
    case 0xBC: std::snprintf(buf, n, ","); return true;
    case 0xBD: std::snprintf(buf, n, "-"); return true;
    case 0xBE: std::snprintf(buf, n, "."); return true;
    case 0xBF: std::snprintf(buf, n, "/"); return true;
    case 0xC0: std::snprintf(buf, n, "`"); return true;
    case 0xDB: std::snprintf(buf, n, "["); return true;
    case 0xDC: std::snprintf(buf, n, "\\"); return true;
    case 0xDD: std::snprintf(buf, n, "]"); return true;
    case 0xDE: std::snprintf(buf, n, "'"); return true;
    default: break;
  }
  if (vk > 0 && vk < 256) {
    std::snprintf(buf, n, "%d", vk);
    return true;
  }
  return false;
}

static bool vk_from_name(const char* s, int* vk) {
  if (!s || !s[0] || !vk) return false;
  if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '0' && s[1] <= '9') {
    const int n = std::atoi(s + 1);
    if (n >= 1 && n <= 24) {
      *vk = VK_F1 + n - 1;
      return true;
    }
  }
  if (!s[1]) {
    const char ch = s[0];
    if (ch >= 'a' && ch <= 'z') {
      *vk = ch - 'a' + 'A';
      return true;
    }
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
      *vk = (unsigned char)ch;
      return true;
    }
    switch (ch) {
      case ';': *vk = 0xBA; return true;
      case '=': *vk = 0xBB; return true;
      case ',': *vk = 0xBC; return true;
      case '-': *vk = 0xBD; return true;
      case '.': *vk = 0xBE; return true;
      case '/': *vk = 0xBF; return true;
      case '`': *vk = 0xC0; return true;
      case '[': *vk = 0xDB; return true;
      case '\\': *vk = 0xDC; return true;
      case ']': *vk = 0xDD; return true;
      case '\'': *vk = 0xDE; return true;
      default: break;
    }
  }
  if (_stricmp(s, "PgUp") == 0) { *vk = VK_PRIOR; return true; }
  if (_stricmp(s, "PgDn") == 0) { *vk = VK_NEXT; return true; }
  if (_stricmp(s, "Del") == 0 || _stricmp(s, "Delete") == 0) { *vk = VK_DELETE; return true; }
  if (_stricmp(s, "Ins") == 0) { *vk = VK_INSERT; return true; }
  if (_stricmp(s, "Home") == 0) { *vk = VK_HOME; return true; }
  if (_stricmp(s, "End") == 0) { *vk = VK_END; return true; }
  if (_stricmp(s, "Space") == 0) { *vk = VK_SPACE; return true; }
  if (_stricmp(s, "Tab") == 0) { *vk = VK_TAB; return true; }
  if (_stricmp(s, "Up") == 0) { *vk = VK_UP; return true; }
  if (_stricmp(s, "Down") == 0) { *vk = VK_DOWN; return true; }
  if (_stricmp(s, "Left") == 0) { *vk = VK_LEFT; return true; }
  if (_stricmp(s, "Right") == 0) { *vk = VK_RIGHT; return true; }
  if (s[0] >= '0' && s[0] <= '9') {
    const int n = std::atoi(s);
    if (n > 0 && n < 256) {
      *vk = n;
      return true;
    }
  }
  return false;
}

static void format_combo(int vk, bool ctrl, bool alt, bool shift, char* buf, size_t n) {
  if (!buf || n < 2) return;
  buf[0] = 0;
  char key[16] = {};
  if (!vk_name(vk, key, sizeof(key))) {
    std::snprintf(buf, n, "?");
    return;
  }
  std::snprintf(buf, n, "%s%s%s%s", ctrl ? "Ctrl+" : "", alt ? "Alt+" : "", shift ? "Shift+" : "",
                key);
}

static bool same_combo(const HotkeySlot& s, int vk, bool ctrl, bool alt, bool shift) {
  return s.vk == vk && s.ctrl == ctrl && s.alt == alt && s.shift == shift;
}

static bool reserved_hotkey(int vk) {
  return vk == VK_HOME || vk == VK_INSERT || vk == VK_F8 || vk == VK_ESCAPE ||
         vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
         vk == VK_XBUTTON2 || vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
         vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT ||
         vk == VK_LMENU || vk == VK_RMENU;
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
    {"map", u8"消散战争迷雾", "toggle", 0x11},
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
    {"chaos_mode", u8"混乱模式", "engine", 0},
    {"ore_convoy", u8"刷矿车车队", "engine", 0},
    {"spawn_mcv", u8"召唤基地车", "engine", 0},
    {"spawn_rank", u8"出场等级", "spawn_rank", 0},
};

static const char* kGroupRes[] = {"money_sel", "money", "power", "scpoint", "haveallsc", "fastbuild", "oremine"};
static const char* kGroupSw[] = {"superpower", "disableallsp", "map", "nocbuild",
                                 "protocol_ready", "unit_skill_ready", "disable_protocol"};
static const char* kGroupAmmo[] = {"ammo", "danger"};
static const char* kGroupUnit[] = {
    "speed_max", "speed_slow", "speed_freeze", "speed_restore", "hp_max", "hp_min", "hp_normal",
    "unit_rank", "unit_kill", "unit_clone", "convert_unit", "spawn_unit", "clone_multi",
    "damage_mult", "full_buff", "spawn_rank", "spawn_mcv"};
static const char* kGroupBattle[] = {"enemy_weaken", "ally_god"};

static const GroupInfo kGroups[] = {
    {u8"资源", kGroupRes, 7, u8"己方默认 +10万；玩家资金从列表选择阵营后加减（默认 1万）"},
    {u8"超武 / 地图", kGroupSw, 7, nullptr},
    {u8"弹药 / 危险", kGroupAmmo, 2, nullptr},
    {u8"单位操作", kGroupUnit, 17, u8"需先在游戏里选中单位"},
    {u8"战场", kGroupBattle, 2, nullptr},
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

int hotkey_slot_count() {
  ensure_hotkeys();
  return g_hotkey_n;
}

const char* hotkey_slot_id(int index) {
  ensure_hotkeys();
  if (index < 0 || index >= g_hotkey_n) return "";
  return g_hotkeys[index].id;
}

const char* hotkey_slot_feature(int index) {
  ensure_hotkeys();
  if (index < 0 || index >= g_hotkey_n) return "";
  return g_hotkeys[index].feature;
}

void hotkey_slot_mods(int index, int* vk, bool* ctrl, bool* alt, bool* shift) {
  ensure_hotkeys();
  if (index < 0 || index >= g_hotkey_n) {
    if (vk) *vk = 0;
    if (ctrl) *ctrl = false;
    if (alt) *alt = false;
    if (shift) *shift = false;
    return;
  }
  if (vk) *vk = g_hotkeys[index].vk;
  if (ctrl) *ctrl = g_hotkeys[index].ctrl;
  if (alt) *alt = g_hotkeys[index].alt;
  if (shift) *shift = g_hotkeys[index].shift;
}

void format_hotkey_slot(int index, char* buf, size_t buf_len) {
  ensure_hotkeys();
  if (!buf || buf_len == 0) return;
  buf[0] = 0;
  if (index < 0 || index >= g_hotkey_n || g_hotkeys[index].vk <= 0) return;
  format_combo(g_hotkeys[index].vk, g_hotkeys[index].ctrl, g_hotkeys[index].alt,
               g_hotkeys[index].shift, buf, buf_len);
}

static int find_hotkey_index(const char* id) {
  ensure_hotkeys();
  if (!id) return -1;
  for (int i = 0; i < g_hotkey_n; ++i) {
    if (std::strcmp(g_hotkeys[i].id, id) == 0) return i;
  }
  return -1;
}

static bool assign_hotkey(int index, int vk, bool ctrl, bool alt, bool shift, std::string* err) {
  ensure_hotkeys();
  if (index < 0 || index >= g_hotkey_n) {
    if (err) *err = u8"没有这个快捷键";
    return false;
  }
  if (vk <= 0 || vk >= 256 || reserved_hotkey(vk)) {
    if (err) *err = u8"这个键不能当作快捷键";
    return false;
  }
  for (int i = 0; i < g_hotkey_n; ++i) {
    if (i == index || g_hotkeys[i].vk <= 0) continue;
    if (!same_combo(g_hotkeys[i], vk, ctrl, alt, shift)) continue;
    const FeatureInfo* other = find_feature(g_hotkeys[i].feature);
    if (err) {
      *err = std::string(u8"和「") + (other ? other->label : g_hotkeys[i].id) + u8"」冲突";
    }
    return false;
  }
  g_hotkeys[index].vk = vk;
  g_hotkeys[index].ctrl = ctrl;
  g_hotkeys[index].alt = alt;
  g_hotkeys[index].shift = shift;
  return true;
}

bool set_hotkey_slot(int index, int vk, bool ctrl, bool alt, bool shift, std::string* err) {
  return assign_hotkey(index, vk, ctrl, alt, shift, err);
}

bool reset_hotkey_slot(int index) {
  ensure_hotkeys();
  if (index < 0 || index >= g_hotkey_n) return false;
  const int n = (int)(sizeof(kDefaultHotkeys) / sizeof(kDefaultHotkeys[0]));
  for (int i = 0; i < n; ++i) {
    if (std::strcmp(kDefaultHotkeys[i].id, g_hotkeys[index].id) != 0) continue;
    g_hotkeys[index].vk = kDefaultHotkeys[i].vk;
    g_hotkeys[index].ctrl = kDefaultHotkeys[i].ctrl;
    g_hotkeys[index].alt = kDefaultHotkeys[i].alt;
    g_hotkeys[index].shift = kDefaultHotkeys[i].shift;
    return true;
  }
  return false;
}

int hotkey_slots_for_feature(const char* feature, int* indices, int cap) {
  ensure_hotkeys();
  int n = 0;
  if (!feature || !indices || cap <= 0) return 0;
  for (int i = 0; i < g_hotkey_n && n < cap; ++i) {
    if (std::strcmp(g_hotkeys[i].feature, feature) == 0) indices[n++] = i;
  }
  return n;
}

bool apply_hotkey_text(const char* slot_id, const char* text) {
  const int index = find_hotkey_index(slot_id);
  if (index < 0 || !text) return false;
  bool ctrl = false, alt = false, shift = false;
  const char* key = text;
  while (*key) {
    const char* plus = std::strchr(key, '+');
    char tok[32] = {};
    const size_t len = plus ? (size_t)(plus - key) : std::strlen(key);
    if (len == 0 || len >= sizeof(tok)) return false;
    std::memcpy(tok, key, len);
    tok[len] = 0;
    if (!plus) {
      int vk = 0;
      if (!vk_from_name(tok, &vk) || vk <= 0 || vk >= 256) return false;
      g_hotkeys[index].vk = vk;
      g_hotkeys[index].ctrl = ctrl;
      g_hotkeys[index].alt = alt;
      g_hotkeys[index].shift = shift;
      return true;
    }
    if (_stricmp(tok, "Ctrl") == 0 || _stricmp(tok, "Control") == 0) ctrl = true;
    else if (_stricmp(tok, "Alt") == 0) alt = true;
    else if (_stricmp(tok, "Shift") == 0) shift = true;
    else return false;
    key = plus + 1;
  }
  return false;
}

bool hotkey_capture_active() { return g_hotkey_capture; }

void set_hotkey_capture(bool on) { g_hotkey_capture = on; }

const char* hotkey_hint(const char* key) {
  ensure_hotkeys();
  if (!key) return nullptr;
  thread_local char buf[128];
  buf[0] = 0;
  size_t used = 0;
  for (int i = 0; i < g_hotkey_n; ++i) {
    if (std::strcmp(g_hotkeys[i].feature, key) != 0 || g_hotkeys[i].vk <= 0) continue;
    char one[48] = {};
    format_combo(g_hotkeys[i].vk, g_hotkeys[i].ctrl, g_hotkeys[i].alt, g_hotkeys[i].shift, one,
                 sizeof(one));
    if (!one[0]) continue;
    const size_t need = std::strlen(one) + (used ? 1 : 0);
    if (used + need + 1 >= sizeof(buf)) break;
    if (used) buf[used++] = ' ';
    std::memcpy(buf + used, one, std::strlen(one) + 1);
    used += std::strlen(one);
  }
  return buf[0] ? buf : nullptr;
}

bool feature_enabled(const char* key) {
  const FeatureInfo* f = find_feature(key);
  if (!f || std::strcmp(f->type, "toggle") != 0) return false;
  if (std::strcmp(key, "disableallsp") == 0 && disable_superweapon_held()) return true;
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
    note_disable_superweapon_toggle(enabled);
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

static int g_mcv_faction = 0;

int mcv_faction() { return g_mcv_faction; }

void set_mcv_faction(int faction) {
  if (faction < 0) faction = 0;
  if (faction > 2) faction = 2;
  g_mcv_faction = faction;
}

uint8_t* flags_base() { return g_flags; }
uint8_t* mc_base() { return g_mc; }
uint8_t* idb_base() { return g_idb; }
uint32_t module_base() { return g_module; }
bool hook_is_installed(const char* name) { return hook_installed(name); }

}  // namespace game_api
