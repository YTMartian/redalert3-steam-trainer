// 原生 Win32 注入器（第一步）。
//
// 行为对齐 overlay/tools/inject.py 的主干：找游戏进程 → 卸掉旧的覆盖层 →
// 远程 LoadLibraryW 注入 → 弹结果框。目的只有一个：把 PyInstaller onefile
// 那套"启动时把 14 MB 解到 %TEMP%\_MEIxxxxxx"的环节拿掉。
//
// 和 Python 版的差别（这一步刻意不做的事）：
//   * 版本不对时还不弹带截图的引导窗口（第二步做），先用同样的文字弹框；
//   * 不写 ra3_overlay.log（那是游戏内 DLL 写的），只留 injector_startup.log。
//
// 载荷（DLL / 装配程序 / 引导图）全部编在资源段里，运行时只把两个 DLL 写到
// exe 旁边，写不进去时退到 %LOCALAPPDATA%\RA3Overlay\runtime —— 和 Python 版
// 的 extract_bundled_payload / _runtime_dir 一致。全程不碰临时目录。

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "injector_res.h"

#ifndef BCRYPT_SHA256_ALGORITHM
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#endif

namespace {

// Python 版会把这些名字的旧覆盖层逐个 FreeLibrary 卸掉。
const wchar_t* const kOldOverlayModules[] = {
    L"ra3_overlay_v4.dll", L"ra3_overlay_v3.dll", L"ra3_overlay_v2.dll",
    L"ra3_overlay_fix.dll", L"ra3_overlay_feat.dll", L"ra3_overlay_mh.dll",
    L"ra3_overlay_new.dll", L"ra3_overlay.dll",
};

// 令牌属性：被沙箱/降权时这里能一眼看出来（Python 版的 _token_facts 也记这些）。
struct TokenFacts {
  int integrity = -1;
  bool appcontainer = false;
  bool restricted = false;
};

TokenFacts token_facts();
DWORD visible_process_count();

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::wstring format(const wchar_t* fmt, ...) {
  wchar_t buf[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  return std::wstring(buf);
}

std::wstring to_lower(std::wstring s) {
  for (wchar_t& c : s) c = (wchar_t)towlower(c);
  return s;
}

bool ends_with(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool starts_with(const std::wstring& s, const wchar_t* prefix) {
  const size_t n = wcslen(prefix);
  return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

std::wstring exe_path() {
  wchar_t buf[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, buf, (DWORD)_countof(buf));
  return std::wstring(buf);
}

std::wstring dir_of(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring join(const std::wstring& dir, const wchar_t* name) {
  if (dir.empty()) return std::wstring(name);
  std::wstring out = dir;
  if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
  out += name;
  return out;
}

std::wstring get_env(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)_countof(buf));
  return n > 0 ? std::wstring(buf, n) : std::wstring();
}

bool file_exists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 逐级建目录（等价 os.makedirs(exist_ok=True)）。
bool ensure_dir(const std::wstring& path) {
  if (dir_exists(path)) return true;
  for (size_t pos = path.find(L'\\', 3); pos != std::wstring::npos;
       pos = path.find(L'\\', pos + 1)) {
    CreateDirectoryW(path.substr(0, pos).c_str(), nullptr);
  }
  if (CreateDirectoryW(path.c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

// 探针写一个文件，判断目录真的可写（等价 _dir_writable）。
bool dir_writable(const std::wstring& path) {
  const std::wstring probe =
      join(path, format(L".wt_%lu.tmp", (unsigned long)GetCurrentProcessId()).c_str());
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const char ok = 'k';
  const bool good = WriteFile(h, &ok, 1, &written, nullptr) != 0;
  CloseHandle(h);
  DeleteFileW(probe.c_str());
  return good;
}

// 往 exe 旁边的 injector_startup.log 追加文本（UTF-8）。写不进去就静默放弃。
void append_text(const std::wstring& text) {
  const std::wstring dir = dir_of(exe_path());
  if (dir.empty()) return;
  const int need =
      WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return;
  std::string utf8((size_t)need, '\0');  // 含结尾 NUL，写完再截掉
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), need, nullptr, nullptr);
  utf8.resize((size_t)need - 1);

  HANDLE file = CreateFileW(join(dir, L"injector_startup.log").c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
  CloseHandle(file);
}

void log_line(const wchar_t* fmt, ...) {
  wchar_t buf[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  append_text(std::wstring(buf) + L"\n");
}

// ---------------------------------------------------------------------------
// 资源 / 校验 / 落盘
// ---------------------------------------------------------------------------

bool load_resource(int id, std::vector<unsigned char>* out) {
  HRSRC rsrc = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!rsrc) return false;
  const DWORD size = SizeofResource(nullptr, rsrc);
  HGLOBAL handle = LoadResource(nullptr, rsrc);
  if (!handle || size == 0) return false;
  const void* data = LockResource(handle);
  if (!data) return false;
  out->assign(static_cast<const unsigned char*>(data),
              static_cast<const unsigned char*>(data) + size);
  return true;
}

bool sha256(const void* data, size_t len, unsigned char out[32]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    return false;
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = false;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
    ok = BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const UCHAR*>(data)),
                        (ULONG)len, 0) >= 0 &&
         BCryptFinishHash(hash, out, 32, 0) >= 0;
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

bool sha256_file(const std::wstring& path, unsigned char out[32], ULONGLONG* size) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = false;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
      BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
    std::vector<unsigned char> buf(1 << 16);
    DWORD read = 0;
    ULONGLONG total = 0;
    ok = true;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr) && read > 0) {
      total += read;
      if (BCryptHashData(hash, buf.data(), read, 0) < 0) {
        ok = false;
        break;
      }
    }
    if (ok) ok = BCryptFinishHash(hash, out, 32, 0) >= 0;
    if (size) *size = total;
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(h);
  return ok;
}

// 目标文件已经是这份内容就不写（等价 _copy_if_different 的 same 分支）。
bool file_matches(const std::wstring& dst, const std::vector<unsigned char>& data) {
  ULONGLONG size = 0;
  unsigned char want[32] = {};
  unsigned char have[32] = {};
  if (!sha256(data.data(), data.size(), want)) return false;
  if (!sha256_file(dst, have, &size)) return false;
  return size == data.size() && memcmp(want, have, 32) == 0;
}

// 先写 .tmp 再原子替换（等价 shutil.copy2 + os.replace），失败重试 6 次。
bool write_payload(const std::wstring& dst, const std::vector<unsigned char>& data,
                   std::wstring* note) {
  const std::wstring tmp = dst + L".tmp";
  for (int attempt = 0; attempt < 6; ++attempt) {
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      *note = format(L"错误 %lu", GetLastError());
      Sleep(250);
      continue;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) &&
                    written == data.size();
    const DWORD write_error = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
      *note = format(L"错误 %lu", write_error);
      DeleteFileW(tmp.c_str());
      Sleep(250);
      continue;
    }
    if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    *note = format(L"错误 %lu", GetLastError());
    DeleteFileW(tmp.c_str());
    Sleep(250);
  }
  return false;
}

struct PayloadSpec {
  int resource;
  const wchar_t* name;
  bool dll;  // true：写不进去但旧文件还在时算 dll-locked（Python 里 DLL 可以容忍）
};

// 把载荷写到 dir。返回的错误串和 Python 版一致，方便两边对照。
void ensure_payloads(const std::wstring& dir, std::vector<std::wstring>* errors) {
  const PayloadSpec specs[] = {
      {IDR_PAYLOAD_OVERLAY, L"ra3_overlay_v4.dll", true},
      {IDR_PAYLOAD_ARM, L"arm_mustcode.dll", false},
  };
  for (const PayloadSpec& spec : specs) {
    std::vector<unsigned char> data;
    if (!load_resource(spec.resource, &data)) {
      errors->push_back(format(L"%s 不在注入器里", spec.name));
      continue;
    }
    const std::wstring dst = join(dir, spec.name);
    if (file_exists(dst) && file_matches(dst, data)) continue;  // same

    std::wstring note;
    if (write_payload(dst, data, &note)) continue;

    // 写不进去（游戏占着 / 目录只读 / 令牌被削）但旧文件还在：照样能用它注入。
    if (file_exists(dst)) {
      if (spec.dll) errors->push_back(format(L"dll-locked: %s", note.c_str()));
      continue;
    }
    if (spec.dll) {
      errors->push_back(format(L"dll-locked: %s", note.c_str()));
    } else {
      errors->push_back(format(L"无法写出 %s（%s）", spec.name, note.c_str()));
    }
  }
}

// %LOCALAPPDATA%\RA3Overlay\runtime：exe 目录写不进去时的落脚点。
std::wstring runtime_dir() {
  std::wstring base = get_env(L"LOCALAPPDATA");
  if (base.empty()) base = get_env(L"APPDATA");
  if (base.empty()) return std::wstring();
  const std::wstring dir = join(base, L"RA3Overlay\\runtime");
  if (!ensure_dir(dir) || !dir_writable(dir)) return std::wstring();
  return dir;
}

// ---------------------------------------------------------------------------
// 进程 / 模块
// ---------------------------------------------------------------------------

struct GameProcess {
  DWORD pid;
  std::wstring name;
};

std::vector<GameProcess> list_game_processes() {
  std::vector<GameProcess> found;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return found;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      const std::wstring name = to_lower(pe.szExeFile);
      if (starts_with(name, L"ra3") && ends_with(name, L".game")) {
        found.push_back({pe.th32ProcessID, pe.szExeFile});
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

bool battlenet_running() {
  static const wchar_t* const kNames[] = {L"ra3.battlenet.client.exe",
                                          L"ra3battlenet.exe"};
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      const std::wstring name = to_lower(pe.szExeFile);
      for (const wchar_t* want : kNames) {
        if (name == want) {
          found = true;
          break;
        }
      }
    } while (!found && Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

// 目标进程里某个模块的基址（等价 find_remote_module_base）。
ULONG_PTR find_module_base(DWORD pid, const wchar_t* module_name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  ULONG_PTR base = 0;
  const std::wstring want = to_lower(module_name);
  if (Module32FirstW(snap, &me)) {
    do {
      if (to_lower(me.szModule) == want ||
          ends_with(to_lower(me.szExePath), want.c_str())) {
        base = (ULONG_PTR)me.modBaseAddr;
        break;
      }
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
  return base;
}

// ---------------------------------------------------------------------------
// PE 导出表（等价 _pe_export_rva）：按 RVA 在磁盘上的 kernel32.dll 里找导出。
// 目标进程的 kernel32 基址 + 这里的 RVA = 目标进程里 LoadLibraryW 的地址。
// ---------------------------------------------------------------------------

bool pe_export_rva(const std::wstring& dll_path, const char* export_name, DWORD* rva_out) {
  HANDLE file = CreateFileW(dll_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const DWORD size = GetFileSize(file, nullptr);
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) {
    CloseHandle(file);
    return false;
  }
  const unsigned char* base =
      static_cast<const unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
  bool ok = false;

  auto u16 = [&](size_t off) -> unsigned short {
    return *(const unsigned short*)(base + off);
  };
  auto u32 = [&](size_t off) -> unsigned int {
    return *(const unsigned int*)(base + off);
  };

  if (base && size > 0x40 && base[0] == 'M' && base[1] == 'Z') {
    const size_t nt = u32(0x3C);
    if (nt + 0x18 < size && memcmp(base + nt, "PE\0\0", 4) == 0) {
      const size_t opt = nt + 0x18;
      const unsigned short magic = u16(opt);
      // DataDirectory[0] 在可选头 +0x60（PE32）/ +0x70（PE32+）。
      const size_t export_dir_rva_off =
          magic == 0x10B ? opt + 0x60 : (magic == 0x20B ? opt + 0x70 : 0);
      if (export_dir_rva_off) {
        const DWORD export_rva = u32(export_dir_rva_off);
        const unsigned short num_sections = u16(nt + 6);
        const unsigned short size_opt = u16(nt + 0x14);
        const size_t section_table = nt + 0x18 + size_opt;
        auto rva_to_off = [&](DWORD rva, size_t* out_off) -> bool {
          for (unsigned short i = 0; i < num_sections; ++i) {
            const size_t s = section_table + (size_t)i * 40;
            const DWORD virt_size = u32(s + 8);
            const DWORD virt_addr = u32(s + 12);
            const DWORD raw_size = u32(s + 16);
            const DWORD raw_ptr = u32(s + 20);
            const DWORD span = virt_size > raw_size ? virt_size : raw_size;
            if (rva >= virt_addr && rva < virt_addr + span) {
              *out_off = raw_ptr + (rva - virt_addr);
              return *out_off < size;
            }
          }
          return false;
        };
        size_t exp_off = 0;
        if (export_rva && rva_to_off(export_rva, &exp_off)) {
          const DWORD num_names = u32(exp_off + 0x18);
          const size_t funcs = u32(exp_off + 0x1C);
          const size_t names = u32(exp_off + 0x20);
          const size_t ords = u32(exp_off + 0x24);
          size_t names_off = 0, ords_off = 0, funcs_off = 0;
          if (rva_to_off((DWORD)names, &names_off) &&
              rva_to_off((DWORD)ords, &ords_off) &&
              rva_to_off((DWORD)funcs, &funcs_off)) {
            for (DWORD i = 0; i < num_names; ++i) {
              size_t name_off = 0;
              if (!rva_to_off(u32(names_off + i * 4), &name_off)) continue;
              if (strcmp((const char*)(base + name_off), export_name) != 0) continue;
              const unsigned short ordinal = u16(ords_off + i * 2);
              *rva_out = u32(funcs_off + ordinal * 4);
              ok = true;
              break;
            }
          }
        }
      }
    }
  }

  if (base) UnmapViewOfFile(base);
  CloseHandle(mapping);
  CloseHandle(file);
  return ok;
}

// 目标进程内某个 kernel32 导出的地址。why 里写清楚失败在哪一步（只进日志，不给玩家看）。
bool remote_export(DWORD pid, const char* export_name, ULONG_PTR* address,
                   std::wstring* why) {
  const ULONG_PTR module_base = find_module_base(pid, L"kernel32.dll");
  if (!module_base) {
    if (why) *why = L"kernel32.dll 不在目标进程的模块列表里";
    return false;
  }

  std::wstring windir = get_env(L"SystemRoot");
  if (windir.empty()) windir = L"C:\\Windows";
  // 64 位注入器注 32 位游戏时要读 SysWOW64 那份；本工程是 Win32，走 System32。
  std::vector<std::wstring> candidates;
  if (sizeof(void*) > 4) candidates.push_back(join(windir, L"SysWOW64\\kernel32.dll"));
  candidates.push_back(join(windir, L"System32\\kernel32.dll"));

  for (const std::wstring& pe_path : candidates) {
    if (!file_exists(pe_path)) continue;
    DWORD rva = 0;
    if (!pe_export_rva(pe_path, export_name, &rva)) continue;
    *address = module_base + rva;
    if (why) {
      *why = format(L"base=0x%lX rva=0x%lX (%s)", (unsigned long)module_base,
                    (unsigned long)rva, pe_path.c_str());
    }
    return true;
  }
  if (why) *why = L"导出表里没有这个符号（读的是 System32/SysWOW64 的 kernel32）";
  return false;
}

// 远程 FreeLibrary 直到模块消失（等价 unload_module_by_name）。
bool unload_module(HANDLE process, DWORD pid, const wchar_t* name, std::wstring* note) {
  ULONG_PTR free_library = 0;
  if (!remote_export(pid, "FreeLibrary", &free_library, nullptr)) {
    *note = L"FreeLibrary unresolved";
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    const ULONG_PTR base = find_module_base(pid, name);
    if (!base) return true;
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(free_library),
        (LPVOID)base, 0, nullptr);
    if (!thread) {
      *note = format(L"FreeLibrary CreateRemoteThread failed err=%lu", GetLastError());
      return false;
    }
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    *note += format(L"FreeLibrary#%d on 0x%lX ", i + 1, (unsigned long)base);
    Sleep(200);
  }
  if (find_module_base(pid, name)) {
    *note += L"still present";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 注入
// ---------------------------------------------------------------------------

struct InjectResult {
  bool ok = false;
  bool needs_admin = false;
  bool wrong_version = false;
  bool wrong_uprising = false;
  std::wstring running_name;
  std::wstring message;  // 已经是给玩家看的正文
};

InjectResult inject(const std::wstring& dll_path) {
  InjectResult result;

  if (battlenet_running()) {
    result.message = L"请关闭战网进程（RA3BattleNet）";
    return result;
  }
  if (!file_exists(dll_path)) {
    result.message = format(L"找不到覆盖层 DLL：\n%s", dll_path.c_str());
    return result;
  }
  if (!ends_with(dll_path, L".dll")) {
    result.message = format(L"目标不是 DLL：\n%s", dll_path.c_str());
    return result;
  }

  // 选进程：起义时刻 1.0 优先，其次原版 1.12；其它 ra3 版本记成"版本不对"。
  DWORD pid = 0;
  bool has_uprising_other = false;
  std::wstring uprising_other_name;
  bool has_retail_other = false;
  std::wstring retail_other_name;
  for (const GameProcess& game : list_game_processes()) {
    const std::wstring low = to_lower(game.name);
    if (low == L"ra3ep1_1.0.game") {
      if (!pid) pid = game.pid;
    } else if (starts_with(low, L"ra3ep1")) {
      has_uprising_other = true;
      uprising_other_name = game.name;
    } else if (low == L"ra3_1.12.game") {
      if (!pid) pid = game.pid;
    } else {
      has_retail_other = true;
      retail_other_name = game.name;
    }
  }
  if (!pid && has_uprising_other) {
    result.wrong_version = true;
    result.wrong_uprising = true;
    result.running_name = uprising_other_name;
    return result;
  }
  if (!pid && has_retail_other) {
    result.wrong_version = true;
    result.running_name = retail_other_name;
    return result;
  }
  if (!pid) {
    // 沙箱/受限令牌下，进程列表是被过滤的（连 explorer、steam 都看不到），
    // 这时报"没开游戏"会把人带偏，直接把真实原因写出来。
    const TokenFacts facts = token_facts();
    const DWORD visible = visible_process_count();
    if (facts.integrity == 0x1000 /* SECURITY_MANDATORY_LOW_RID */ || facts.restricted) {
      result.message = format(
          L"没有找到游戏进程。\n"
          L"当前进程在受限令牌 / 沙箱里运行（只看到 %lu 个进程，看不到游戏）。\n"
          L"请关掉本程序，在资源管理器里直接双击运行，\n"
          L"不要从终端、工具或聊天面板里启动。",
          (unsigned long)visible);
    } else {
      result.message = L"没有找到游戏进程。\n请先启动红警 3，再运行修改器。";
    }
    return result;
  }

  HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!process) {
    result.needs_admin = true;
    result.message = format(L"无法打开游戏进程（错误 %lu）。\n请用管理员身份运行注入器。",
                            GetLastError());
    return result;
  }

  // 先卸掉上一次注入的覆盖层，让 DllMain 重新跑一遍。
  for (const wchar_t* name : kOldOverlayModules) {
    std::wstring note;
    if (!unload_module(process, pid, name, &note) && note.find(L"still present") != std::wstring::npos) {
      CloseHandle(process);
      result.message = format(
          L"旧的覆盖层还在游戏里，卸不掉（%s）。\n请先完全退出红警 3，再重新进游戏后注入。",
          name);
      return result;
    }
  }

  ULONG_PTR load_library = 0;
  std::wstring export_why;
  if (!remote_export(pid, "LoadLibraryW", &load_library, &export_why)) {
    CloseHandle(process);
    log_line(L"inject: LoadLibraryW resolve failed: %s", export_why.c_str());
    result.message = L"无法在游戏进程里找到 LoadLibraryW。";
    return result;
  }
  log_line(L"inject: pid=%lu LoadLibraryW %s", (unsigned long)pid, export_why.c_str());

  const size_t path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, path_bytes,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) {
    const DWORD err = GetLastError();
    CloseHandle(process);
    result.message = format(L"无法在游戏里分配内存（错误 %lu）。", err);
    return result;
  }

  SIZE_T written = 0;
  if (!WriteProcessMemory(process, remote, dll_path.c_str(), path_bytes, &written)) {
    const DWORD err = GetLastError();
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    result.message = format(L"无法把 DLL 路径写入游戏（错误 %lu）。", err);
    return result;
  }

  HANDLE thread = CreateRemoteThread(
      process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
      remote, 0, nullptr);
  if (!thread) {
    const DWORD err = GetLastError();
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    result.message = format(L"无法在游戏里启动加载线程（错误 %lu）。", err);
    return result;
  }

  WaitForSingleObject(thread, 15000);
  DWORD exit_code = 0;
  GetExitCodeThread(thread, &exit_code);
  log_line(L"inject: thread exit=%lu", (unsigned long)exit_code);
  CloseHandle(thread);
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  CloseHandle(process);

  if (exit_code == 0) {
    result.message =
        L"DLL 没有载入成功。\n"
        L"注入器会把 DLL 解到自己旁边。请确认没有被安全软件隔离。\n"
        L"日志：和修改器同一个目录的 ra3_overlay.log";
    return result;
  }

  result.ok = true;
  result.message = format(
      L"覆盖层已载入。\n进程 PID：%lu\n进局后按 Home 打开菜单。", (unsigned long)pid);
  return result;
}

// ---------------------------------------------------------------------------
// 提示框 / 版本引导文字（第二步换成带截图的窗口，这一步先用文字）
// ---------------------------------------------------------------------------

void show_message(bool ok, const std::wstring& detail) {
  const std::wstring title = ok ? L"注入成功" : L"注入失败";
  const std::wstring text = (ok ? L"注入成功。\n\n" : L"注入失败。\n\n") + detail;
  const UINT icon = ok ? MB_ICONINFORMATION : MB_ICONERROR;
  MessageBoxW(nullptr, text.c_str(), title.c_str(),
              icon | MB_TOPMOST | MB_SETFOREGROUND | MB_OK);
}

std::wstring version_guide_text(const std::wstring& running_name, bool uprising) {
  if (uprising) {
    return format(
        L"注入失败。当前运行的是 %s，不是起义时刻 1.0。\n"
        L"请按下面两步把启动版本改成 1.0，然后完全退出并重新启动游戏，再运行本修改器。\n"
        L"\n"
        L"1. 在 Steam 库中右键《命令与征服：红色警戒 3》起义，选择「属性」。\n"
        L"2. 打开「通用」，在启动选项中填入 -runver 1.0。",
        running_name.c_str());
  }
  return format(
      L"注入失败。当前运行的是 %s，不是 1.12。\n"
      L"请按下面两步把启动版本改成 1.12，然后完全退出并重新启动游戏，再运行本修改器。\n"
      L"\n"
      L"1. 在 Steam 库中右键《命令与征服：红色警戒 3》，选择「属性」。\n"
      L"2. 打开「通用」，在启动选项中填入 -runver 1.12。",
      running_name.c_str());
}

// ---------------------------------------------------------------------------
// 权限 / 诊断
// ---------------------------------------------------------------------------

bool is_admin() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID admin_group = nullptr;
  if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                &admin_group)) {
    return false;
  }
  BOOL member = FALSE;
  CheckTokenMembership(nullptr, admin_group, &member);
  FreeSid(admin_group);
  return member != FALSE;
}

// 令牌属性：被沙箱/降权时这里能一眼看出来（Python 版的 _token_facts 也记这些）。
TokenFacts token_facts() {
  TokenFacts facts;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return facts;
  DWORD value = 0, need = 0;
  BYTE buffer[64] = {};
  if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &need)) {
    const PSID sid = ((PTOKEN_MANDATORY_LABEL)buffer)->Label.Sid;
    const DWORD count = *GetSidSubAuthorityCount(sid);
    facts.integrity = (int)*GetSidSubAuthority(sid, count - 1);
  }
  if (GetTokenInformation(token, (TOKEN_INFORMATION_CLASS)29, &value, sizeof(value), &need)) {
    facts.appcontainer = value != 0;
  }
  value = 0;
  if (GetTokenInformation(token, TokenHasRestrictions, &value, sizeof(value), &need)) {
    facts.restricted = value != 0;
  }
  CloseHandle(token);
  return facts;
}

// 当前进程能看到的进程数。沙箱里这个数字会明显偏小（而且看不到 explorer/steam），
// 记进日志后，"找不到游戏"到底是没开还是被隔离，一眼就能分。
DWORD visible_process_count() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  DWORD count = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      ++count;
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return count;
}

// 父进程链（谁把我拉起来的）。沙箱会把父进程藏起来，所以这里同时记录
// "父进程名能不能查到" —— 查不到就说明父进程不在本进程可见的列表里。
std::wstring parent_chain(bool* explorer_visible) {
  struct Entry {
    DWORD pid, parent;
    std::wstring name;
  };
  std::vector<Entry> entries;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
      do {
        entries.push_back({pe.th32ProcessID, pe.th32ParentProcessID, to_lower(pe.szExeFile)});
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
  }
  if (explorer_visible) {
    *explorer_visible = false;
    for (const Entry& e : entries) {
      if (e.name == L"explorer.exe") *explorer_visible = true;
    }
  }

  std::wstring out;
  DWORD pid = GetCurrentProcessId();
  for (int depth = 0; depth < 4; ++depth) {
    DWORD parent = 0;
    for (const Entry& e : entries) {
      if (e.pid == pid) {
        parent = e.parent;
        break;
      }
    }
    if (!parent) break;
    std::wstring name;
    for (const Entry& e : entries) {
      if (e.pid == parent) {
        name = e.name;
        break;
      }
    }
    if (name.empty()) name = L"<hidden>";
    if (!out.empty()) out += L" <- ";
    out += format(L"%s(%lu)", name.c_str(), (unsigned long)parent);
    pid = parent;
  }
  return out.empty() ? std::wstring(L"<none>") : out;
}

// 记一行启动痕迹（对应 Python 的 _startup_log）。写入失败就静默跳过。
void startup_log(int argc, wchar_t** argv) {
  const std::wstring dir = dir_of(exe_path());
  if (dir.empty()) return;

  std::wstring argv_text;
  for (int i = 1; i < argc; ++i) {
    if (!argv_text.empty()) argv_text += L' ';
    argv_text += argv[i];
  }

  const TokenFacts facts = token_facts();
  bool explorer_visible = false;
  const std::wstring parents = parent_chain(&explorer_visible);
  const std::wstring facts_text = format(
      L"integrity=0x%X appcontainer=%d restricted=%d procs=%lu explorer_visible=%d parents=%s",
      facts.integrity, facts.appcontainer ? 1 : 0, facts.restricted ? 1 : 0,
      visible_process_count(), explorer_visible ? 1 : 0, parents.c_str());

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  const std::wstring text =
      format(L"--- %04d-%02d-%02d %02d:%02d:%02d ---\n", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond) +
      format(L"native=1 exe=%s\n", dir.c_str()) +
      format(L"meipass= (none) cwd=%s\n", std::wstring([] {
               wchar_t cwd[MAX_PATH * 2] = {};
               GetCurrentDirectoryW((DWORD)_countof(cwd), cwd);
               return std::wstring(cwd);
             }()).c_str()) +
      format(L"TEMP=%s TMP=%s\n", get_env(L"TEMP").c_str(), get_env(L"TMP").c_str()) +
      format(L"admin=%d argv=%s\n", is_admin() ? 1 : 0, argv_text.c_str()) +
      facts_text + L"\n";

  append_text(text);
}

// 以管理员身份重启自己（对应 _relaunch_elevated）。
bool relaunch_elevated(int argc, wchar_t** argv) {
  std::wstring params;
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"--elevated") == 0) continue;
    if (!params.empty()) params += L' ';
    params += L'"';
    params += argv[i];
    params += L'"';
  }
  if (!params.empty()) params += L' ';
  params += L"--elevated";

  const HINSTANCE rc = ShellExecuteW(nullptr, L"runas", exe_path().c_str(), params.c_str(),
                                     nullptr, SW_SHOWNORMAL);
  return (INT_PTR)rc > 32;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) argc = 0;

  startup_log(argc, argv);

  // --selftest：只用本进程验证 PE 导出解析（含 WOW64 下读哪份 kernel32），不碰游戏。
  if (argc > 1 && _wcsicmp(argv[1], L"--selftest") == 0) {
    ULONG_PTR addr = 0;
    std::wstring why;
    const bool ok = remote_export(GetCurrentProcessId(), "LoadLibraryW", &addr, &why);
    const ULONG_PTR real = reinterpret_cast<ULONG_PTR>(&LoadLibraryW);
    const bool same = ok && addr == real;
    log_line(L"selftest: ok=%d same=%d addr=0x%lX real=0x%lX why=%s", ok ? 1 : 0,
             same ? 1 : 0, (unsigned long)addr, (unsigned long)real, why.c_str());
    show_message(same,
                 format(L"自检：LoadLibraryW 解析%s\naddr=0x%lX\nreal=0x%lX\n%s",
                        same ? L"正确" : L"失败", (unsigned long)addr,
                        (unsigned long)real, why.c_str()));
    LocalFree(argv);
    return same ? 0 : 1;
  }

  const std::wstring here = dir_of(exe_path());

  // 1. 把两个 DLL 落到 exe 旁边（内容一致就跳过）。
  std::vector<std::wstring> errors;
  ensure_payloads(here, &errors);

  // 只保留真正的错误：dll-locked 表示文件还在、可以继续注入。
  std::vector<std::wstring> fatal;
  for (const std::wstring& e : errors) {
    if (!starts_with(e, L"dll-locked:")) fatal.push_back(e);
  }
  if (!fatal.empty()) {
    std::wstring text = L"无法释出修改器文件：\n";
    for (const std::wstring& e : fatal) text += e + L"\n";
    show_message(false, text);
    LocalFree(argv);
    return 1;
  }

  // 2. exe 目录写不进去时，退到 %LOCALAPPDATA%\RA3Overlay\runtime。
  std::wstring work_dir = here;
  if (!file_exists(join(here, L"ra3_overlay_v4.dll"))) {
    const std::wstring alt = runtime_dir();
    if (!alt.empty() && alt != here) {
      std::vector<std::wstring> alt_errors;
      ensure_payloads(alt, &alt_errors);
      if (file_exists(join(alt, L"ra3_overlay_v4.dll"))) {
        work_dir = alt;
      } else {
        std::wstring text = L"无法释出修改器文件：\n";
        for (const std::wstring& e : alt_errors) text += e + L"\n";
        show_message(false, text);
        LocalFree(argv);
        return 1;
      }
    }
  }

  // 3. 选 DLL：命令行里第一个非 -- 参数优先，否则用刚释出的那份。
  std::wstring dll = join(work_dir, L"ra3_overlay_v4.dll");
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != L'-' || argv[i][1] != L'-') {
      dll = argv[i];
      break;
    }
  }

  // 注的就是刚释出的那份时，再确认一次它是最新的（对应 inject() 里的 only_dll 分支）。
  if (_wcsicmp(dll.c_str(), join(here, L"ra3_overlay_v4.dll").c_str()) == 0 ||
      _wcsicmp(dll.c_str(), join(work_dir, L"ra3_overlay_v4.dll").c_str()) == 0) {
    std::vector<std::wstring> dll_errors;
    ensure_payloads(work_dir, &dll_errors);
    for (const std::wstring& e : dll_errors) {
      if (!starts_with(e, L"dll-locked:") && e.find(L"ra3_overlay_v4.dll") != std::wstring::npos) {
        show_message(false,
                     L"无法更新 DLL，文件仍被占用。\n请先完全退出红警 3，再运行修改器。");
        LocalFree(argv);
        return 1;
      }
    }
  }

  InjectResult result = inject(dll);

  // 只有真的被系统挡回来（进程打不开）才提权重来一次。
  const bool elevated_flag = [&] {
    for (int i = 1; i < argc; ++i) {
      if (_wcsicmp(argv[i], L"--elevated") == 0) return true;
    }
    return false;
  }();
  if (!result.ok && result.needs_admin && !elevated_flag && !is_admin()) {
    if (relaunch_elevated(argc, argv)) {
      LocalFree(argv);
      return 0;
    }
  }

  if (result.wrong_version) {
    // 第二步会换成带两张截图的引导窗口；这里先给同样的文字。
    show_message(false, version_guide_text(result.running_name, result.wrong_uprising));
    LocalFree(argv);
    return 1;
  }

  show_message(result.ok, result.message);
  LocalFree(argv);
  return result.ok ? 0 : 1;
}
