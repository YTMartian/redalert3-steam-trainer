// ---------------------------------------------------------------------------
// 解锁 60 帧（移植自 RayaTrainer 的 FrameRateUnlock）
//
// 补丁是**一次性静态写入**：把引擎的帧时间基从 30fps 换成 60fps，然后就不管了，
// 不是每帧去改 memory 求倍率。
//
// 这里有个很容易踩的坑，也是最初版本失效的原因：
//
//   只改 .data 里那 8 个常量**不够**。RayaTrainer 还会在 .text 末尾的零填充
//   （RVA 0x7C6420）里现场写一段蹦床，然后把 10 处代码里的绝对地址改成指向它
//   自己造的那份副本：
//
//       0x8B409C(updateFPS) -> 0x7C6448 (值 60)
//       0x8E0DFC           -> 0x7C644C (值 0.03)
//       0x31F457 movss     -> jmp 蹦床，蹦床里 mulss ×0.25 再跳回
//
//   也就是说引擎的关键代码早就不读 .data 那几个全局了。我们改原地址是空转，
//   必须把这套地址重定向一起复现，才会真的变 60 帧。
//
// 补丁集来自对活进程的实测：把运行时内存和磁盘上的 ra3_1.12.game 逐字节对比
// （overlay/tools/capture_vs_disk.py），再用 gen_patchset_inc.py 生成
// fps_unlock_patchset.inc。一共抓到 58 处差异，其中 46 处是 RayaTrainer 其它
// 功能（无敌、金钱等）跳到它自己 VirtualAlloc 内存的 hook —— 那些没有它那份
// 代码就无法复现，全部剔除；剩下 12 处落在引擎映像内部，就是帧率相关的全部。
//
// 关键时序事实（决定状态机怎么写）：
//   ra3_1.12.game 的 .data 段 VA=0x8AB000 / vsize=0x85BA0 / rsize=0x2B000。
//   0x8D6000 之后没有磁盘数据，那一段是 BSS：加载时是 0，由引擎初始化时才
//   写入常量。所以主菜单阶段去读会读到 0 —— 这不是「版本不匹配」。
// ---------------------------------------------------------------------------
#include "fps_unlock.h"

#include "game_api.h"
#include "game_api_internal.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>

namespace {

// 读回用的缓冲上限：最长的一段是 cave blob（48 字节）。
constexpr int kMaxPatchBytes = 64;

struct PatchEntry {
  uint32_t rva;
  uint16_t len;
  const uint8_t* original;
  const uint8_t* patched;
};

// ---- .data 常量 -----------------------------------------------------------
// 语义上正好是一套「翻倍/减半」：
//     0x8B4098  logicFPS   15 -> 30      0x8E0DEC  0.015  -> 0.03
//     0x8E0DBC  66.667 -> 33.333         0x8E0DF8  15.0   -> 30.0
//     0x8E0DF0  30.0   -> 60.0           0x8E0DF4  33.333 -> 16.667
//     0x8E0E34  0.0667 -> 0.0333         0x8E0ED4  0.0333 -> 0.01667
const uint8_t kData0Off[4] = {0x0F, 0x00, 0x00, 0x00};
const uint8_t kData0On[4] = {0x1E, 0x00, 0x00, 0x00};
const uint8_t kData1Off[4] = {0x90, 0xC2, 0x75, 0x3C};
const uint8_t kData1On[4] = {0x90, 0xC2, 0xF5, 0x3C};
const uint8_t kData2Off[4] = {0x55, 0x55, 0x85, 0x42};
const uint8_t kData2On[4] = {0x55, 0x55, 0x05, 0x42};
const uint8_t kData3Off[4] = {0x00, 0x00, 0x70, 0x41};
const uint8_t kData3On[4] = {0x00, 0x00, 0xF0, 0x41};
const uint8_t kData4Off[4] = {0x00, 0x00, 0xF0, 0x41};
const uint8_t kData4On[4] = {0x00, 0x00, 0x70, 0x42};
const uint8_t kData5Off[4] = {0x55, 0x55, 0x05, 0x42};
const uint8_t kData5On[4] = {0x55, 0x55, 0x85, 0x41};
const uint8_t kData6Off[4] = {0x89, 0x88, 0x88, 0x3D};
const uint8_t kData6On[4] = {0x89, 0x88, 0x08, 0x3D};
const uint8_t kData7Off[4] = {0x89, 0x88, 0x08, 0x3D};
const uint8_t kData7On[4] = {0x89, 0x88, 0x88, 0x3C};

const PatchEntry kDataPatches[] = {
    {0x8B4098, 4, kData0Off, kData0On},
    {0x8E0DEC, 4, kData1Off, kData1On},
    {0x8E0DBC, 4, kData2Off, kData2On},
    {0x8E0DF8, 4, kData3Off, kData3On},
    {0x8E0DF0, 4, kData4Off, kData4On},
    {0x8E0DF4, 4, kData5Off, kData5On},
    {0x8E0E34, 4, kData6Off, kData6On},
    {0x8E0ED4, 4, kData7Off, kData7On},
};
constexpr int kDataPatchCount =
    (int)(sizeof(kDataPatches) / sizeof(kDataPatches[0]));

// ---- .text 代码位点（由 tools/gen_patchset_inc.py 生成）-------------------
// 第一条是 cave 本体，必须最先写；其余都是地址重定向 / 跳转。
#include "fps_unlock_patchset.inc"

// cave 里嵌着一条绝对地址引用（mulss 的操作数 0x00BC6440），所以要求模块载入
// 在首选基址上。
constexpr uint32_t kRequiredBase = 0x400000;

// ---------------------------------------------------------------------------
// 半时间增量（把「60fps = 2 倍速」真正解耦掉）
//
// 上面这套补丁让更新循环跑 60 次/秒，而推进时间在 tick 函数
// (vtable+0x34 = 0x539B20) 里：
//
//     0x539BD6  fld1                             ; 本帧时间增量 1.0
//     0x539BD8  add dword ptr [edi+0x50], ebp    ; 时钟 +1（整数，15/秒基准）
//     0x539BDB  push ecx / fstp dword [esp]
//     0x539BE1  call 0x66D190                    ; 按增量推进时间轴
//
// 于是 60 次/秒 × 每次 1.0 = 2.00x。实测（overlay/tools/hook_half_time.py）：
// 整块跳过 tick 会让时钟彻底停住（那个函数里还有全局帧计数、动画刷新、模式
// 判定等每帧必做的工作），所以只能改「每次推进多少」：
//
//     dt 1.0 -> 0.5，时钟隔帧 +1
//
// 每秒总量不变（60 × 0.5 = 30 × 1.0），时间分辨率翻倍。于是 60fps 的流畅度
// 拿到了，而动作/动画/水面的速度仍是 1.00x。
//
// fld1(2 字节) 与 add(3 字节) 在 0x539BD6 连续 5 字节，正好放一个跳转。
// 实测结果：60.0fps / 1.00x（25 秒采样，clock 617.13->642.13 = 25.00s）。
// ---------------------------------------------------------------------------
constexpr uint32_t kHalfTimeRva = 0x139BD6;
constexpr uint32_t kHalfTimeResumeRva = 0x139BDB;  // 原 0x539BDB: push ecx
constexpr int kHalfTimeLen = 5;
const uint8_t kHalfTimeOrig[kHalfTimeLen] = {0xD9, 0xE8, 0x01, 0x6F, 0x50};

struct HalfTimeState {
  uint8_t* page = nullptr;   // 桩所在的页
  uint32_t stub = 0;
  uint32_t counter = 0;      // 隔帧计数
  uint32_t half = 0;         // 0.5f 常量
  bool installed = false;
};
HalfTimeState g_half;
// 半时间增量只有在显式要求时才启用：它能让时钟回到 1.00x，但单位动作、建筑动画
// 和水面仍按 tick 次数推进，实测仍是 2 倍。也就是说它产出的是「时钟 1x / 画面
// 2x」的不一致状态，比两个都 2x 更容易误导，所以默认关闭、只作为实验开关。
//   RA3_FPS60_HALFTIME=1  启用
bool g_halftime_enabled = false;

CRITICAL_SECTION g_cs;
bool g_cs_ready = false;

bool g_wanted = false;   // 用户意图（持久化的是这个）
bool g_applied = false;  // 补丁此刻是否在内存里
uint32_t g_base = 0;
DWORD g_next_retry = 0;
fps_unlock::Reason g_reason = fps_unlock::Reason::kNone;

// 诊断用：这套补丁其实是两个互相独立的部分，
//   kDataOnly —— 只翻倍 .data 常量（logicFPS 15->30、若干 tick 长度）
//   kCodeOnly —— 只做 .text 的地址重定向 + 蹦床（把 updateFPS 读到的地方换成 60）
// 分开施加才能判定「速度加倍」到底来自哪一半。默认 kFull，与 RayaTrainer 一致。
enum class Mode { kFull, kDataOnly, kCodeOnly };
Mode g_mode = Mode::kFull;

const char* mode_name(Mode m) {
  switch (m) {
    case Mode::kDataOnly: return "data-only";
    case Mode::kCodeOnly: return "code-only";
    default: return "full";
  }
}

// RA3_FPS60_MODE=full|data|code 覆盖上面的默认值。只在诊断 2 倍速时用。
// RA3_FPS60_HALFTIME=1 启用半时间增量（实验性：时钟 1x，但画面仍是 2x）。
void read_mode_from_env() {
  char hl[8] = {};
  const DWORD hn = GetEnvironmentVariableA("RA3_FPS60_HALFTIME", hl,
                                           (DWORD)sizeof(hl));
  if (hn > 0 && (hl[0] == '1' || hl[0] == 'y' || hl[0] == 'Y')) {
    g_halftime_enabled = true;
  }
  char buf[16] = {};
  const DWORD n = GetEnvironmentVariableA("RA3_FPS60_MODE", buf,
                                          (DWORD)sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return;
  if (lstrcmpiA(buf, "data") == 0 || lstrcmpiA(buf, "data-only") == 0) {
    g_mode = Mode::kDataOnly;
  } else if (lstrcmpiA(buf, "code") == 0 || lstrcmpiA(buf, "code-only") == 0) {
    g_mode = Mode::kCodeOnly;
  } else {
    g_mode = Mode::kFull;
  }
}

int patch_total() {
  // 模式只影响「这一组位点算不算数」，apply/verify/revert 全都走
  // patch_total()/patch_at()，所以不必改动它们。
  const int nd = (g_mode == Mode::kCodeOnly) ? 0 : kDataPatchCount;
  const int nc = (g_mode == Mode::kDataOnly) ? 0 : kCodePatchCount;
  return nd + nc;
}

const PatchEntry* patch_at(int i) {
  const int nd = (g_mode == Mode::kCodeOnly) ? 0 : kDataPatchCount;
  if (i < nd) return &kDataPatches[i];
  i -= nd;
  if (i < ((g_mode == Mode::kDataOnly) ? 0 : kCodePatchCount)) {
    return &kCodePatches[i];
  }
  return nullptr;
}

// 表里的 rva 是**相对模块基址**的偏移，不是已含首选基址的 VA。
// 曾经写成 g_base + (rva - 0x400000)，结果把基址又减掉了：0x8E0ED4 被算成
// 0x8E0ED4（落在 .text 里，读到 pop ebx/pop ebp 这类代码字节），于是 verify
// 全部报 mismatch、UI 显示「版本不匹配」。正确地址是 0xCE0ED4。
uint32_t va_of(const PatchEntry& p) { return g_base + p.rva; }

bool bytes_equal(const uint8_t* a, const uint8_t* b, int len) {
  return std::memcmp(a, b, (size_t)len) == 0;
}

bool all_zero(const uint8_t* p, int len) {
  for (int i = 0; i < len; ++i) {
    if (p[i]) return false;
  }
  return true;
}

bool read_bytes(uint32_t va, uint8_t* out, int len) {
  __try {
    std::memcpy(out, reinterpret_cast<const void*>(va), (size_t)len);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// 改代码时要保证没有别的线程正停在这几字节中间。挂起期间本线程不碰游戏的其它
// 状态，只做内存写入，所以不会死锁（不等待任何被挂起线程持有的锁）。
struct ThreadFreeze {
  HANDLE handles[256];
  int count = 0;

  void freeze() {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
      do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (count >= (int)(sizeof(handles) / sizeof(handles[0]))) break;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
          handles[count++] = h;
        } else {
          CloseHandle(h);
        }
      } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
  }

  void thaw() {
    for (int i = 0; i < count; ++i) {
      ResumeThread(handles[i]);
      CloseHandle(handles[i]);
    }
    count = 0;
  }
};

// 把一段字节写进目标地址，必要时改页保护。返回是否全部成功。
bool write_bytes(uint32_t va, const uint8_t* src, int len) {
  DWORD old_protect = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(va), (SIZE_T)len,
                      PAGE_EXECUTE_READWRITE, &old_protect)) {
    return false;
  }
  bool ok = true;
  __try {
    std::memcpy(reinterpret_cast<void*>(va), src, (size_t)len);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(va),
                          (SIZE_T)len);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void*>(va), (SIZE_T)len, old_protect, &ignored);
  return ok;
}

struct VerifyResult {
  bool ok = false;           // 全部位点都是「原值或补丁值」
  bool any_original = false; // 至少还有一个位点仍是原值（需要写）
  bool any_zero = false;     // 有非零以外的位点：还没被引擎初始化
  int mismatches = 0;
};

// 校验整套补丁的落点。
// 判定规则：等于补丁值 = 已打；等于原值 = 待打；全 0 = 引擎还没初始化
// （.data 是 BSS，见文件头）；其余非零内容才说明版本/构建不匹配。
//
// 注意 cave 那一条的原值本来就是 48 个 0，所以「原值」必须先于「全 0」判断，
// 否则会被误判成未初始化而永远打不上。
VerifyResult verify_all(bool log_details) {
  VerifyResult r;
  r.ok = true;
  for (int i = 0; i < patch_total(); ++i) {
    const PatchEntry* p = patch_at(i);
    if (!p) continue;
    uint8_t cur[kMaxPatchBytes] = {};
    if (!read_bytes(va_of(*p), cur, p->len)) {
      r.ok = false;
      ++r.mismatches;
      if (log_details) {
        game_api::log("fps60: site 0x%X unreadable", p->rva);
      }
      continue;
    }
    if (bytes_equal(cur, p->patched, p->len)) continue;
    if (bytes_equal(cur, p->original, p->len)) {
      r.any_original = true;
      continue;
    }
    if (all_zero(cur, p->len)) {
      r.any_zero = true;
      r.ok = false;
      ++r.mismatches;
      if (log_details) {
        game_api::log("fps60: site 0x%X is still zero (engine not initialised yet)",
                      p->rva);
      }
      continue;
    }
    r.ok = false;
    ++r.mismatches;
    if (log_details) {
      uint32_t got = 0;
      uint32_t want = 0;
      std::memcpy(&got, cur, 4);
      std::memcpy(&want, p->original, 4);
      game_api::log("fps60: site 0x%X mismatch: memory=%08X expected=%08X",
                    p->rva, got, want);
    }
  }
  return r;
}

// 把 0.5f 的时间增量桩装到 0x539BD6。成功返回 true；失败只影响速度（会回到
// 2 倍速），不影响 60fps 本身，所以调用方只记日志不中止。
bool half_time_install() {
  if (!g_halftime_enabled) return true;
  if (g_mode != Mode::kFull) return true;
  if (g_base != kRequiredBase) return false;

  const uint32_t site = g_base + kHalfTimeRva;
  uint8_t cur[kHalfTimeLen] = {};
  if (!read_bytes(site, cur, kHalfTimeLen)) return false;
  if (cur[0] == 0xE9) {           // 已经是我们的跳转
    g_half.installed = true;
    return true;
  }
  if (!bytes_equal(cur, kHalfTimeOrig, kHalfTimeLen)) {
    game_api::log("fps60: half-time site 0x%X unexpected bytes %02X %02X %02X "
                  "%02X %02X", kHalfTimeRva, cur[0], cur[1], cur[2], cur[3],
                  cur[4]);
    return false;
  }

  if (!g_half.page) {
    void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (!p) {
      game_api::log("fps60: half-time VirtualAlloc failed (%lu)", GetLastError());
      return false;
    }
    g_half.page = reinterpret_cast<uint8_t*>(p);
    g_half.stub = (uint32_t)(uintptr_t)p + 0x100;
    g_half.counter = (uint32_t)(uintptr_t)p + 0x800;
    g_half.half = (uint32_t)(uintptr_t)p + 0x900;
    *reinterpret_cast<uint32_t*>((uintptr_t)g_half.counter) = 0;
    *reinterpret_cast<float*>((uintptr_t)g_half.half) = 0.5f;
  }

  // 桩：fld 0.5 / 计数 / 隔帧再加时钟 / 回到原流程的 push ecx。
  //   D9 05 <0.5f>        fld  dword ptr [half]
  //   A1 <counter>        mov  eax, [counter]
  //   40                  inc  eax
  //   A3 <counter>        mov  [counter], eax
  //   A8 01               test al, 1
  //   75 04               jnz  +4        (跳过下面 4 字节)
  //   83 47 50 01         add  dword ptr [edi+0x50], 1
  //   E9 <rel32>          jmp  0x539BDB
  uint8_t code[32] = {};
  int n = 0;
  auto put32 = [&code](int at, uint32_t v) {
    std::memcpy(&code[at], &v, 4);
  };
  code[n++] = 0xD9; code[n++] = 0x05; put32(n, g_half.half); n += 4;
  code[n++] = 0xA1; put32(n, g_half.counter); n += 4;
  code[n++] = 0x40;
  code[n++] = 0xA3; put32(n, g_half.counter); n += 4;
  code[n++] = 0xA8; code[n++] = 0x01;
  code[n++] = 0x75; code[n++] = 0x04;
  code[n++] = 0x83; code[n++] = 0x47; code[n++] = 0x50; code[n++] = 0x01;
  code[n++] = 0xE9;
  const int32_t back = (int32_t)(g_base + kHalfTimeResumeRva) -
                       (int32_t)(g_half.stub + (uint32_t)n + 4);
  put32(n, (uint32_t)back); n += 4;

  if (!write_bytes(g_half.stub, code, n)) {
    game_api::log("fps60: half-time stub write failed");
    return false;
  }

  uint8_t patch[kHalfTimeLen] = {0xE9, 0, 0, 0, 0};
  const int32_t rel = (int32_t)g_half.stub - (int32_t)(site + kHalfTimeLen);
  std::memcpy(&patch[1], &rel, 4);

  ThreadFreeze freeze;
  freeze.freeze();
  const bool ok = write_bytes(site, patch, kHalfTimeLen);
  freeze.thaw();
  g_half.installed = ok;
  if (ok) {
    game_api::log("fps60: half-time decouple installed (dt 1.0 -> 0.5, clock "
                  "every other frame) at %08X -> stub %08X",
                  site, g_half.stub);
  } else {
    game_api::log("fps60: half-time detour write failed");
  }
  return ok;
}

void half_time_remove() {
  if (!g_half.installed) return;
  const uint32_t site = g_base + kHalfTimeRva;
  ThreadFreeze freeze;
  freeze.freeze();
  const bool ok = write_bytes(site, kHalfTimeOrig, kHalfTimeLen);
  freeze.thaw();
  g_half.installed = false;
  if (ok) {
    game_api::log("fps60: half-time decouple removed");
  } else {
    game_api::log("fps60: half-time restore failed");
  }
}

bool apply_locked(bool log_details) {
  if (g_base != kRequiredBase) {
    // cave 里有绝对地址操作数，模块不在首选基址上就不能照搬。
    g_reason = fps_unlock::Reason::kVersionMismatch;
    if (log_details) {
      game_api::log("fps60: module base 0x%X != 0x%X, code patch set needs the "
                    "preferred base", g_base, kRequiredBase);
    }
    return false;
  }

  const VerifyResult v = verify_all(log_details);
  if (!v.ok) {
    if (v.any_zero) {
      g_reason = fps_unlock::Reason::kNotReady;
    } else {
      g_reason = fps_unlock::Reason::kVersionMismatch;
    }
    return false;
  }

  ThreadFreeze freeze;
  freeze.freeze();
  bool ok = true;
  const bool any_orig = v.any_original;
  // 表里 cave 排第一，先写它，避免有线程跳进半成品。
  for (int i = 0; i < patch_total() && ok; ++i) {
    const PatchEntry* p = patch_at(i);
    if (!p) continue;
    uint32_t va = va_of(*p);
    uint8_t cur[kMaxPatchBytes] = {};
    if (!read_bytes(va, cur, p->len)) {
      ok = false;
      break;
    }
    if (bytes_equal(cur, p->patched, p->len)) continue;  // 已就位
    ok = write_bytes(va, p->patched, p->len);
  }
  freeze.thaw();

  if (!ok) {
    g_reason = fps_unlock::Reason::kVersionMismatch;
    game_api::log("fps60: writing patches failed");
    return false;
  }
  g_applied = true;
  g_reason = fps_unlock::Reason::kNone;
  if (log_details || any_orig) {
    game_api::log("fps60: %d patch sites applied (%d data + %d code)",
                  patch_total(), kDataPatchCount, kCodePatchCount);
  }
  // 没有这一步，60fps 必然伴随 2 倍速（更新循环频率翻倍）。失败不撤销补丁：
  // 帧率仍然是对的，只是速度会回到 2x，日志里能看出原因。
  half_time_install();
  return true;
}

bool revert_locked() {
  // 先摘掉半时间增量的跳转，再撤补丁：桩页本身不释放，重新开启时复用。
  half_time_remove();
  ThreadFreeze freeze;
  freeze.freeze();
  // 反向恢复：先撤掉跳转，最后再清 cave，避免有线程跳进被清空的蹦床。
  bool ok = true;
  for (int i = patch_total() - 1; i >= 0 && ok; --i) {
    const PatchEntry* p = patch_at(i);
    if (!p) continue;
    uint32_t va = va_of(*p);
    uint8_t cur[kMaxPatchBytes] = {};
    if (!read_bytes(va, cur, p->len)) {
      ok = false;
      break;
    }
    if (bytes_equal(cur, p->original, p->len)) continue;  // 已是原值
    ok = write_bytes(va, p->original, p->len);
  }
  freeze.thaw();
  g_applied = false;
  if (!ok) game_api::log("fps60: restoring original bytes failed");
  return ok;
}

}  // namespace

namespace fps_unlock {

bool available() { return patch_total() > 0; }

bool ready() {
  // module_base() 在起义时刻恒为 0 —— 那边布局不同，这套补丁不适用。
  return game_api::module_base() != 0;
}

bool enabled() {
  if (!g_cs_ready) return false;
  EnterCriticalSection(&g_cs);
  const bool v = g_wanted;
  LeaveCriticalSection(&g_cs);
  return v;
}

bool active() {
  if (!g_cs_ready) return false;
  EnterCriticalSection(&g_cs);
  const bool v = g_applied;
  LeaveCriticalSection(&g_cs);
  return v;
}

Reason pending_reason() {
  if (!g_cs_ready) return Reason::kNoModule;
  EnterCriticalSection(&g_cs);
  const Reason r = g_reason;
  LeaveCriticalSection(&g_cs);
  return r;
}

// 由 game_api::init 在模块确定后调用一次。
void init() {
  if (!g_cs_ready) {
    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
  }
  g_base = game_api::module_base();
  g_applied = false;
  g_wanted = false;
  g_next_retry = 0;
  g_reason = Reason::kNone;
  read_mode_from_env();
  if (!g_base) {
    g_reason = Reason::kNoModule;
    game_api::log("fps60: game module missing, 60fps unlock unavailable");
    return;
  }
  if (game_api::uprising_active()) {
    g_reason = Reason::kNoModule;
    game_api::log("fps60: uprising build has a different layout, unlock disabled");
    return;
  }
  game_api::log("fps60: ready, %d patch sites (%d data + %d code), mode %s, "
                "base 0x%X",
                patch_total(), kDataPatchCount, kCodePatchCount,
                mode_name(g_mode), g_base);
}

bool set_enabled(bool on) {
  if (!g_cs_ready) return false;
  EnterCriticalSection(&g_cs);
  g_wanted = on;
  if (!on) {
    if (g_applied) revert_locked();
    g_reason = Reason::kNone;
    LeaveCriticalSection(&g_cs);
    return true;
  }
  g_base = game_api::module_base();
  if (!g_base) {
    g_reason = Reason::kNoModule;
    g_wanted = false;
    LeaveCriticalSection(&g_cs);
    return false;
  }
  // 引擎可能还没初始化那几个 BSS 常量。失败也保留意图，交给 tick 重试。
  const bool ok = g_applied ? true : apply_locked(true);
  g_next_retry = GetTickCount() + 500;
  LeaveCriticalSection(&g_cs);
  return ok;
}

void tick() {
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  const bool want = g_wanted;
  LeaveCriticalSection(&g_cs);
  if (!want) return;

  const DWORD now = GetTickCount();
  if (now < g_next_retry && (g_next_retry - now) < 0x40000000u) return;

  EnterCriticalSection(&g_cs);
  g_next_retry = now + 500;
  if (!g_applied) {
    // 还没打上：引擎初始化完成后就能成功。只在真正进对局后重试，避免在主菜单
    // 每隔 500ms 白读一次内存。
    if (game_api::battle_clock(nullptr)) {
      apply_locked(false);
    }
  } else {
    // 已打上：进/退对局时游戏可能把常量写回原值，漂移就重打。
    const VerifyResult v = verify_all(false);
    if (v.ok && !g_half.installed && g_halftime_enabled &&
        g_mode == Mode::kFull) {
      // 补丁在位但半时间增量的跳转被人改掉了（换图/其它工具）：补回去，
      // 否则会出现「60fps 但是 2 倍速」。
      half_time_install();
    }
    if (!v.ok && v.any_zero) {
      // 读回 0 不代表被别的东西改坏了：0x8D6000 之后是 BSS，换地图/重新开局时
      // 引擎会先清零再重新写入。这时应该等它写好再补，不能把用户的开关关掉。
      g_reason = Reason::kNotReady;
      apply_locked(false);
    } else if (!v.ok) {
      // 非零但既不是原值也不是补丁值：确实被别的东西改写了（其它修改器 /
      // 版本差异）。不强改，直接停用并把原因显示出来。
      g_wanted = false;
      g_applied = false;
      g_reason = Reason::kVersionMismatch;
      game_api::log("fps60: patch sites changed underneath us, unlock turned off");
    } else if (v.any_original) {
      apply_locked(false);
    }
  }
  LeaveCriticalSection(&g_cs);
}

void restore_all() {
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  if (g_applied) revert_locked();
  g_wanted = false;
  g_applied = false;
  LeaveCriticalSection(&g_cs);
}

}  // namespace fps_unlock
