#include "game_api.h"
#include "game_api_internal.h"

#include <Windows.h>

#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace game_api {
namespace {

constexpr uint32_t kModBase = 0x400000;
constexpr uint32_t kLocalPlayerRva = 0x8EDE2C;
constexpr uint32_t kFnAddXp = 0x005173F0;
constexpr uint32_t kFnDestroy = 0x007DCDF0;
constexpr uint32_t kFnCreateUnit = 0x006440F0;
constexpr uint32_t kFnGetMouseXyz = 0x0062C500;

uint32_t va_of(uint32_t va) { return module_base() + (va - kModBase); }

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

uint32_t local_owner() {
  uint32_t player = read_u32(module_base() + kLocalPlayerRva);
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

struct SpeedNode {
  uint32_t addr;
};

std::vector<uint32_t> speed_nodes(uint32_t ent) {
  std::vector<uint32_t> nodes;
  uint32_t vec = read_u32(ent + 0x374);
  if (!is_ptr(vec)) return nodes;
  uint32_t ctrl = read_u32(vec + 0x200);
  if (!is_ptr(ctrl)) return nodes;
  uint32_t first = read_u32(ctrl);
  if (is_ptr(first)) nodes.push_back(first);
  uint32_t second = read_u32(ctrl + 4);
  if (is_ptr(second)) {
    float flag = read_f32(second + 0x18);
    if (std::fabs(flag - 1.0f) < 1e-6f) nodes.push_back(second);
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
  uint32_t hp = read_u32(ent + 0x33C);
  if (!is_ptr(hp)) return false;
  if (std::strcmp(mode, "max") == 0) {
    return write_f32(hp + 4, 9999999.f) && write_f32(hp + 0xC, 9999999.f);
  }
  if (std::strcmp(mode, "min") == 0) {
    return write_f32(hp + 4, 1.f);
  }
  if (std::strcmp(mode, "normal") == 0) {
    float mx = read_f32(hp + 0x10);
    return write_f32(hp + 4, mx) && write_f32(hp + 0xC, mx);
  }
  return false;
}

std::vector<uint32_t> filter_by_relation(const std::vector<uint32_t>& ents,
                                         const char* relation) {
  uint32_t local = local_owner_for_ops();
  std::vector<uint32_t> out;
  for (uint32_t ent : ents) {
    uint32_t ow = read_u32(ent + 0x418);
    if (std::strcmp(relation, "ally") == 0) {
      if (local) {
        if (is_ptr(ow) && ow == local) out.push_back(ent);
      } else {
        out.push_back(ent);
      }
    } else if (std::strcmp(relation, "enemy") == 0) {
      if (local) {
        if (is_ptr(ow) && ow != local) out.push_back(ent);
      } else if (is_ptr(ow)) {
        out.push_back(ent);
      }
    }
  }
  return out;
}

// ---- in-process game calls on a worker thread ----

struct CallJob {
  enum Kind { kCdecl, kThiscallDestroy } kind;
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
        if (out_msg) *out_msg = u8"读不到本地玩家归属（观战请用观战赠送）";
        return false;
      }
    } else {
      if (!safe_read_u32(ent + 0x418, &owner) || !is_ptr(owner)) {
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
  int n = 0;
  for (uint32_t ent : ents) {
    if (write_entity_hp(ent, mode)) ++n;
  }
  if (n == 0) {
    if (out_msg) *out_msg = u8"选中对象没有可写的血量组件";
    return false;
  }
  if (out_msg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), u8"已对 %d 个单位改血量", n);
    *out_msg = buf;
  }
  return true;
}

bool kill_selected(std::string* out_msg) {
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
  for (uint32_t ent : ents) {
    if (read_u32(ent + 0x418) == owner) continue;
    if (write_u32(ent + 0x418, owner)) ++n;
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

}  // namespace

bool engine_run(const char* key, std::string* out_msg) {
  if (!ready() && std::strcmp(key, "speed_max") != 0) {
    // speed/hp don't need hooks but still need module; allow memory ops without hooks
  }
  if (!module_base()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }

  if (std::strcmp(key, "protocol_ready") == 0) {
    if (!ready() || !hook_is_installed("SuperPower")) {
      if (out_msg) *out_msg = u8"需要已注入且含 SuperPower hook";
      return false;
    }
    pulse_flag(0x0D, 1.0f);
    if (out_msg) *out_msg = u8"已脉冲协议/超武就绪约 1 秒";
    return true;
  }
  if (std::strcmp(key, "unit_skill_ready") == 0) {
    if (!ready() || !hook_is_installed("SuperPower")) {
      if (out_msg) *out_msg = u8"需要已注入且含 SuperPower hook";
      return false;
    }
    pulse_flag(0x0D, 1.2f);
    if (out_msg) *out_msg = u8"已脉冲单位技能就绪约 1.2 秒";
    return true;
  }
  if (std::strcmp(key, "disable_protocol") == 0) {
    if (!ready() || !hook_is_installed("DisableAllSP")) {
      if (out_msg) *out_msg = u8"需要已注入且含 DisableAllSP hook";
      return false;
    }
    pulse_flag(0x0E, 2.0f);
    if (out_msg) *out_msg = u8"已脉冲禁用敌方超武/协议约 2 秒";
    return true;
  }
  if (std::strcmp(key, "fog_toggle") == 0) {
    if (!ready() || !hook_is_installed("Map")) {
      if (out_msg) *out_msg = u8"需要已注入且含 Map hook";
      return false;
    }
    uint8_t cur = get_flag(0x11);
    set_flag(0x11, cur ? 0 : 1);
    if (out_msg) *out_msg = cur ? u8"已恢复战争迷雾" : u8"已关闭战争迷雾";
    return true;
  }
  if (std::strcmp(key, "speed_max") == 0) return apply_speed("max", out_msg);
  if (std::strcmp(key, "speed_slow") == 0) return apply_speed("slow", out_msg);
  if (std::strcmp(key, "speed_freeze") == 0) return apply_speed("freeze", out_msg);
  if (std::strcmp(key, "speed_restore") == 0) return apply_speed("restore", out_msg);
  if (std::strcmp(key, "hp_max") == 0) return apply_hp("max", nullptr, out_msg);
  if (std::strcmp(key, "hp_min") == 0) return apply_hp("min", nullptr, out_msg);
  if (std::strcmp(key, "hp_normal") == 0) return apply_hp("normal", nullptr, out_msg);
  if (std::strcmp(key, "enemy_weaken") == 0) {
    auto ents = filter_by_relation(selected_entities_stable(), "enemy");
    return apply_hp("min", &ents, out_msg);
  }
  if (std::strcmp(key, "ally_god") == 0) {
    auto ents = filter_by_relation(selected_entities_stable(), "ally");
    return apply_hp("max", &ents, out_msg);
  }
  if (std::strcmp(key, "unit_kill") == 0) return kill_selected(out_msg);
  if (std::strcmp(key, "unit_rank") == 0) return rank_up(out_msg);
  if (std::strcmp(key, "convert_unit") == 0) return convert_selected(out_msg);
  if (std::strcmp(key, "damage_mult") == 0) return apply_damage_mult(out_msg);
  if (std::strcmp(key, "chaos_mode") == 0) return chaos_selected(out_msg);
  if (std::strcmp(key, "unit_clone") == 0) return clone_selected(true, 1, out_msg);
  if (std::strcmp(key, "spawn_unit") == 0) return clone_selected(true, 1, out_msg);
  if (std::strcmp(key, "spec_gift") == 0) return clone_selected(false, 1, out_msg);
  if (std::strcmp(key, "clone_multi") == 0) return clone_selected(true, 5, out_msg);
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
  if (std::strcmp(key, "spawn_mcv") == 0) {
    if (out_msg)
      *out_msg = u8"召唤基地车仍不稳定，请暂用 Python 版或选中基地车后「复制到己方」";
    return false;
  }
  if (out_msg) *out_msg = u8"未知 engine 功能";
  return false;
}

}  // namespace game_api
