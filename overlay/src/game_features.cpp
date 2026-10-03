#include "game_api.h"
#include "game_api_internal.h"
#include "unit_names_zh.h"
#include "MinHook.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
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

  // Prefer beside the DLL (packaged release layout: overlay\bin\*.dll + name files).
  {
    wchar_t wdir[MAX_PATH] = {};
    std::wcsncpy(wdir, wpath, MAX_PATH - 1);
    wchar_t* slash = wcsrchr(wdir, L'\\');
    if (slash) {
      *slash = 0;
      wchar_t wfile[MAX_PATH] = {};
      _snwprintf_s(wfile, _TRUNCATE, L"%s\\unit_names.txt", wdir);
      if (WideCharToMultiByte(CP_UTF8, 0, wfile, -1, out, (int)out_len, nullptr,
                              nullptr) > 0) {
        return true;
      }
    }
  }

  // Dev fallback: <repo>/overlay/bin/<dll> → <repo>/unit_names.txt
  {
    wchar_t wdir[MAX_PATH] = {};
    std::wcsncpy(wdir, wpath, MAX_PATH - 1);
    for (int i = 0; i < 3; ++i) {
      wchar_t* slash = wcsrchr(wdir, L'\\');
      if (!slash) break;
      *slash = 0;
    }
    wchar_t wfile[MAX_PATH] = {};
    _snwprintf_s(wfile, _TRUNCATE, L"%s\\unit_names.txt", wdir);
    if (WideCharToMultiByte(CP_UTF8, 0, wfile, -1, out, (int)out_len, nullptr,
                            nullptr) > 0) {
      return true;
    }
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

bool load_names_file_into(NameMap* m, const char* path, bool overwrite) {
  if (!m || !path || !path[0]) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::string line;
  char bom[3] = {};
  f.read(bom, 3);
  if (!(bom[0] == (char)0xEF && bom[1] == (char)0xBB && bom[2] == (char)0xBF)) {
    f.seekg(0);
  }
  while (std::getline(f, line)) {
    std::string id, zh;
    if (!parse_name_line(line, &id, &zh)) continue;
    if (overwrite || m->find(id) == m->end()) (*m)[id] = zh;
  }
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
  f << u8"# 与 DLL 同目录（overlay\\bin\\unit_names.txt）\r\n";
  f << u8"# 格式: TypeId=显示名\r\n";
  f << u8"#       sig_XXXXXXXX=显示名（模板字符串指纹，跨局可用）\r\n";
  f << u8"#       tpl_XXXXXXXX=显示名（旧版内存地址键，重启后失效，可删）\r\n";
  f << u8"# CSF 基底: 同目录 unit_names_csf.txt；本文件覆盖优先\r\n";
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

  // 1) Official CSF export (Name:<TypeId>).
  char csf_path[MAX_PATH] = {};
  if (g_unit_names_path[0] &&
      sibling_names_path(g_unit_names_path, "unit_names_csf.txt", csf_path,
                         sizeof(csf_path))) {
    if (load_names_file_into(&g_unit_names, csf_path, true)) {
      log("unit names CSF loaded: %s (%d entries)", csf_path,
          (int)g_unit_names.size());
    }
  }

  // 2) Builtins fill gaps only.
  seed_builtins(&g_unit_names);

  // 3) User file overrides (persisted separately so saves don't dump CSF).
  bool file_ok = false;
  if (g_unit_names_path[0]) {
    if (load_names_file_into(&g_user_names, g_unit_names_path, true)) {
      file_ok = true;
      for (const auto& kv : g_user_names) {
        g_unit_names[kv.first] = kv.second;
      }
    }
  }

  // First run: create an empty-ish user file with comment header only if missing.
  if (!file_ok && g_unit_names_path[0]) {
    write_unit_names_file(g_user_names, g_unit_names_path);
    log("unit names store created: %s", g_unit_names_path);
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

// ThePlayerList (RVA 0x8EDE2C): +0x28 local, +0x2C count, +0x30 Player*[0x14].
// Money: resource table at player+0xE4, first entry is money obj (+0x04 funds).
// Power: player+0x74 -> obj with +0x04 total / +0x08 used (MustCode Power).
constexpr uint32_t kPlayerListLocalOff = 0x28;
constexpr uint32_t kPlayerListCountOff = 0x2C;
constexpr uint32_t kPlayerListArrayOff = 0x30;
constexpr uint32_t kPlayerListSlots = 0x14;
constexpr uint32_t kPlayerMoneyTableOff = 0xE4;
constexpr uint32_t kPlayerPowerObjOff = 0x74;

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

static bool type_id_is_building(const char* id) {
  if (!id || !id[0]) return false;
  static const char* kBuild[] = {
      "Barracks", "WarFactory", "Factory", "PowerPlant", "Refinery",
      "ConstructionYard", "ConYard", "NavalYard", "Shipyard", "Airfield",
      "Outpost", "Crane", "BaseDefense", "WallHub", "TeslaWallHub",
      "TechStructure", "Chronosphere", "ProtonCollider", "IronCurtain",
      "VacuumImploder", "SuperWeapon", "Nanotech", "Psionic", "Bunker",
      "Hospital", "Garage", "Observation", "OilDerrick", "OreNode",
      "TechBuilding", "ShipYardTech", "AirportTech", "Veterancy",
      "DefensiveStructure", "CommandHub", "Egg"};
  for (const char* b : kBuild) {
    if (std::strstr(id, b)) return true;
  }
  return false;
}

static bool looks_like_entity(uint32_t ent) {
  if (!is_ptr(ent)) return false;
  uint32_t ow = 0, unit_data = 0;
  if (!safe_read_u32(ent + 0x418, &ow) || !is_ptr(ow)) return false;
  if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) return false;
  return true;
}

static bool entity_alive(uint32_t ent) {
  uint32_t hp = 0;
  if (!safe_read_u32(ent + 0x33C, &hp) || !is_ptr(hp)) return true;
  float cur = 0.f, mx = 0.f;
  if (!safe_read_bytes(hp + 4, &cur, sizeof(cur))) return true;
  safe_read_bytes(hp + 0x10, &mx, sizeof(mx));
  if (!(cur == cur) || cur > 1.0e8f || cur < -1.f) return true;  // NaN / garbage
  // Only drop clearly-dead objects (max HP known and current is ~0).
  if (mx > 1.f && cur <= 0.5f) return false;
  return true;
}

static bool entity_belongs_to(uint32_t ent, uint32_t player, uint32_t /*player_id*/) {
  if (!looks_like_entity(ent) || !is_ptr(player)) return false;
  uint32_t ow = 0;
  if (!safe_read_u32(ent + 0x418, &ow) || !is_ptr(ow)) return false;
  // Owner must be this Player object. player+0x20 is shared by faction/team
  // and would dump another seat's army onto the first matching player.
  return ow == player;
}

// Cached player-relative entity vector offsets (units + buildings often separate).
// High bit set => vector lives on [player+0x40] + (off&~hi).
constexpr int kMaxEntVecOffs = 8;
static uint32_t g_ent_vec_offs[kMaxEntVecOffs] = {};
static int g_ent_vec_off_count = 0;
static uint32_t g_roster_local_id = 0;
static uint32_t g_logic_list_off = 0;
constexpr int kMaxLogicLists = 8;
static uint32_t g_logic_offs[kMaxLogicLists] = {};
static int g_logic_off_n = 0;
static int g_logic_refresh = 0;

static bool valid_ent_vector_range(uint32_t begin, uint32_t end, uint32_t* out_n) {
  if (!is_ptr(begin) || !is_ptr(end) || end <= begin) return false;
  const uint32_t bytes = end - begin;
  if (bytes & 3u) return false;
  const uint32_t n = bytes / 4u;
  if (n < 1 || n > 4096) return false;
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
    safe_read_u32(ent + 0x418, &ow);
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
  char key[96] = {};
  char name[64] = {};
  bool building = false;
};
static std::map<uint32_t, LiveEnt> g_live;
static std::vector<uint32_t> g_pending;
static CRITICAL_SECTION g_live_cs;
static bool g_live_cs_ready = false;
static bool g_live_seeded = false;
static int g_live_scan_tick = 0;

struct FacCache {
  uint32_t player = 0;
  int faction = 0;  // 1 allied, 2 soviet, 3 empire
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
  g_pending.clear();
  g_live_seeded = false;
  g_live_scan_tick = 0;
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
}

static void clear_roster_caches() {
  g_ent_vec_off_count = 0;
  g_player_vec_cache_n = 0;
  g_roster_order_n = 0;
  g_roster_local_id = 0;
  g_logic_list_off = 0;
  g_logic_off_n = 0;
  g_logic_refresh = 0;
  ensure_tpl_cs();
  EnterCriticalSection(&g_tpl_cs);
  g_tpl_type_cache.clear();
  LeaveCriticalSection(&g_tpl_cs);
  g_ent_ident.clear();
  g_fac_cache_n = 0;
  g_seen_n = 0;
  std::memset(g_fac_cache, 0, sizeof(g_fac_cache));
  std::memset(g_seen, 0, sizeof(g_seen));
  std::memset(g_ent_vec_offs, 0, sizeof(g_ent_vec_offs));
  std::memset(g_player_vec_cache, 0, sizeof(g_player_vec_cache));
  std::memset(g_roster_order, 0, sizeof(g_roster_order));
  clear_live_ents();
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

static void add_roster_count(PlayerEconomy* pe, const char* key, const char* disp,
                             bool building) {
  if (!pe || !key || !key[0] || !disp || !disp[0]) return;
  // Prefer merging by display name so TypeId jitter doesn't split one unit type.
  for (int i = 0; i < pe->roster_count; ++i) {
    const bool key_hit = (std::strcmp(pe->roster[i].key, key) == 0);
    const bool name_hit =
        pe->roster[i].name[0] && disp[0] && std::strcmp(pe->roster[i].name, disp) == 0 &&
        pe->roster[i].is_building == building;
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
  if (std::strstr(key, "AttackDron")) return true;  // AttackDrone / AttackDron
  if (std::strcmp(key, "AlliedAttackDrone") == 0) return true;
  // Top-secret chronosphere / satellite sweep helper objects.
  if (std::strstr(key, "SatelliteSweep")) return true;
  if (std::strcmp(key, "AlliedSatelliteSweepRevealObject") == 0) return true;
  if (std::strcmp(key, "AlliedSatelliteSweepShroudRevealer") == 0) return true;
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

// The template's own GameObject id is the nearest catalog string.
// Nested weapon / spawn names sit further out and were being picked before,
// which labeled a unit as another faction.
static bool tpl_cached_reject(uint32_t unit_data) {
  if (!is_ptr(unit_data) || g_tpl_cs_state != 2) return false;
  EnterCriticalSection(&g_tpl_cs);
  auto it = g_tpl_type_cache.find(unit_data);
  const bool reject = it != g_tpl_type_cache.end() && (it->second.empty() || it->second == "-");
  LeaveCriticalSection(&g_tpl_cs);
  return reject;
}

static bool resolve_primary_catalog_id(uint32_t unit_data, char* out, size_t out_len) {
  if (!is_ptr(unit_data) || !out || out_len < 2) return false;
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
    return lookup_zh_name(out) != nullptr;
  }
  LeaveCriticalSection(&g_tpl_cs);
  char best[96] = {};
  for (uint32_t off = 0; off <= 0x180 && !best[0]; off += 4) {
    if (catalog_id_at(unit_data + off, best, sizeof(best))) break;
    uint32_t p = 0;
    if (!safe_read_u32(unit_data + off, &p) || !is_ptr(p)) continue;
    if (catalog_id_at(p, best, sizeof(best))) break;
    uint32_t p2 = 0;
    if (safe_read_u32(p, &p2) && is_ptr(p2)) catalog_id_at(p2, best, sizeof(best));
  }
  ensure_tpl_cs();
  EnterCriticalSection(&g_tpl_cs);
  if (!best[0] || should_hide_roster_type(best)) {
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
  if (!safe_read_u32(ent + 0x418, &ow)) return false;
  const int s = seat_for_owner(out, ow);
  if (s < 0) return false;
  uint32_t unit_data = 0;
  if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) return false;
  if (!resolve_primary_catalog_id(unit_data, type_id, type_len)) return false;
  if (seat) *seat = s;
  return true;
}

static int score_catalog_vector(uint32_t begin, uint32_t end, const MatchEconomy* out) {
  uint32_t n = 0;
  if (!valid_ent_vector_range(begin, end, &n)) return 0;
  int catalog = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t ent = 0;
    if (!safe_read_u32(begin + i * 4u, &ent)) continue;
    char type_id[96] = {};
    if (entity_catalog_for_roster(ent, out, nullptr, type_id, sizeof(type_id))) ++catalog;
  }
  return catalog;
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
    if (safe_read_u32(player + 0x84, &argb)) take_color(argb);
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

static LiveNote note_live_ent(uint32_t ent, uint32_t owner_hint) {
  if (!is_ptr(ent)) return LiveNote::kDrop;
  uint32_t unit_data = 0;
  if (!safe_read_u32(ent + 4, &unit_data) || !is_ptr(unit_data)) return LiveNote::kRetry;
  char type_id[96] = {};
  if (!resolve_primary_catalog_id(unit_data, type_id, sizeof(type_id))) return LiveNote::kDrop;
  const char* zh = lookup_zh_name(type_id);
  if (!zh || !zh[0]) return LiveNote::kDrop;
  uint32_t ow = 0;
  safe_read_u32(ent + 0x418, &ow);
  if (!is_ptr(ow)) ow = owner_hint;
  if (!is_ptr(ow)) return LiveNote::kRetry;
  LiveEnt rec{};
  rec.owner = ow;
  std::snprintf(rec.key, sizeof(rec.key), "%s", type_id);
  std::snprintf(rec.name, sizeof(rec.name), "%s", zh);
  rec.building = type_id_is_building(type_id);
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  g_live[ent] = rec;
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  return LiveNote::kOk;
}

static void forget_live_ent(uint32_t ent) {
  if (!is_ptr(ent)) return;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  g_live.erase(ent);
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
}

static void publish_live_roster(MatchEconomy* out) {
  if (!out) return;
  std::vector<uint32_t> pending;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  pending.swap(g_pending);
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  // A 6-player battle spawns far more projectiles than units. Classify a
  // bounded batch per refresh so the render thread cannot stall the match.
  constexpr size_t kBatch = 64;
  std::vector<uint32_t> later;
  if (pending.size() > kBatch) {
    later.assign(pending.begin() + kBatch, pending.end());
    pending.resize(kBatch);
  }
  std::vector<uint32_t> again;
  for (uint32_t ent : pending) {
    if (note_live_ent(ent, 0) == LiveNote::kRetry && again.size() < 24) again.push_back(ent);
  }
  if (!later.empty() || !again.empty()) {
    if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
    if (g_pending.size() < 512) {
      const size_t room = 512 - g_pending.size();
      size_t n = later.size();
      if (n > room) n = room;
      g_pending.insert(g_pending.end(), later.begin(), later.begin() + n);
    }
    if (g_pending.size() < 512) {
      const size_t room = 512 - g_pending.size();
      size_t n = again.size();
      if (n > room) n = room;
      g_pending.insert(g_pending.end(), again.begin(), again.begin() + n);
    }
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  }

  std::vector<std::pair<uint32_t, LiveEnt>> copy;
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  copy.reserve(g_live.size());
  for (auto it = g_live.begin(); it != g_live.end();) {
    if (!looks_like_entity(it->first)) {
      it = g_live.erase(it);
      continue;
    }
    // Capture / ownership change writes +0x418 in place (oil derrick, etc.).
    uint32_t ow = 0;
    if (safe_read_u32(it->first + 0x418, &ow) && is_ptr(ow)) it->second.owner = ow;
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
  if (is_ptr(ent)) note_live_ent(ent, owner);
  return ent;
}

// 0x51A210 allocates every object (production, buildings, projectiles).
// Owner is often written by the caller after this returns, so remember the
// pointer and classify it on the next stats refresh.
static uint32_t __cdecl hk_spawn_object(void* req) {
  uint32_t ent = g_orig_spawn ? g_orig_spawn(req) : 0;
  if (is_ptr(ent)) {
    uint32_t unit_data = 0;
    safe_read_u32(ent + 4, &unit_data);
    if (tpl_cached_reject(unit_data)) return ent;
    if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
    if (g_pending.size() < 512) g_pending.push_back(ent);
    if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  }
  return ent;
}

static void __cdecl forget_live_from_hook(uint32_t ent) { forget_live_ent(ent); }

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

// Gather entities from every player's lists (and global multi-owner lists),
// then bucket into each PlayerEconomy by owner / player_id.
static void fill_all_rosters(MatchEconomy* out) {
  if (!out || out->player_count <= 0) return;

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

  // One scan when the match starts (units already on the field never hit the
  // create hook). After that, only CreateUnit / Destroy change the roster.
  if (!g_live_seeded) {
  if (g_live_cs_ready) EnterCriticalSection(&g_live_cs);
  g_live.clear();
  g_pending.clear();
  if (g_live_cs_ready) LeaveCriticalSection(&g_live_cs);
  g_live_scan_tick = 0;

  constexpr uint32_t kGameLogicRva = 0x8DDE84;  // VA 0xCDDE84
  uint32_t logic = 0;
  safe_read_u32(module_base() + kGameLogicRva, &logic);

  auto read_logic_vec = [&](uint32_t off, uint32_t* begin, uint32_t* end) -> bool {
    if (!is_ptr(logic) || !begin || !end) return false;
    if (!safe_read_u32(logic + off, begin) || !safe_read_u32(logic + off + 4, end)) return false;
    return valid_ent_vector_range(*begin, *end, nullptr);
  };

  // Units and buildings live in different lists. Keep several, and refresh
  // the set periodically instead of locking onto whichever list was largest once.
  if (is_ptr(logic) && (g_logic_off_n == 0 || ++g_logic_refresh >= 8)) {
    g_logic_refresh = 0;
    g_player_vec_cache_n = 0;
    struct Cand {
      uint32_t off = 0;
      int n = 0;
    };
    Cand cands[80] = {};
    int nc = 0;
    uint32_t seen_begin[80] = {};
    int nseen = 0;
    for (uint32_t off = 0x20; off <= 0x600; off += 4) {
      uint32_t begin = 0, end = 0;
      if (!read_logic_vec(off, &begin, &end)) continue;
      bool dup = false;
      for (int k = 0; k < nseen; ++k) {
        if (seen_begin[k] == begin) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      if (nseen >= 80) continue;
      seen_begin[nseen++] = begin;
      const int n = score_catalog_vector(begin, end, out);
      if (n < 1) continue;
      cands[nc++] = Cand{off, n};
    }
    std::sort(cands, cands + nc, [](const Cand& a, const Cand& b) { return a.n > b.n; });
    g_logic_off_n = 0;
    for (int i = 0; i < nc && g_logic_off_n < kMaxLogicLists; ++i) {
      if (cands[i].n < 1) break;
      g_logic_offs[g_logic_off_n++] = cands[i].off;
    }
  }

  std::vector<uint32_t> ents;
  ents.reserve(512);
  auto push_vec = [&](uint32_t begin, uint32_t end) {
    uint32_t n = 0;
    if (!valid_ent_vector_range(begin, end, &n)) return;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t ent = 0;
      if (!safe_read_u32(begin + i * 4u, &ent) || !is_ptr(ent)) continue;
      ents.push_back(ent);
    }
  };

  for (int i = 0; i < g_logic_off_n; ++i) {
    uint32_t begin = 0, end = 0;
    if (read_logic_vec(g_logic_offs[i], &begin, &end)) push_vec(begin, end);
  }
  for (int i = 0; i < out->player_count; ++i) {
    uint32_t offs[kMaxEntVecOffs] = {};
    const int no =
        cached_offs_for_player(out->players[i].player, out->players[i].player_id, offs,
                               kMaxEntVecOffs);
    for (int k = 0; k < no; ++k) {
      uint32_t begin = 0, end = 0;
      if (!resolve_vector_off(out->players[i].player, offs[k], &begin, &end)) continue;
      push_vec(begin, end);
    }
  }

  std::sort(ents.begin(), ents.end());
  ents.erase(std::unique(ents.begin(), ents.end()), ents.end());
  for (uint32_t ent : ents) note_live_ent(ent, 0);
  g_live_seeded = true;
  }

  publish_live_roster(out);
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
  return best;
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
      return detected;
    }
    // A new match reuses the same Player*. Replace the label when this
    // seat's own anchors (MCV / yard / barracks) now belong to another side.
    if (detected && detected != g_fac_cache[i].faction &&
        anchor_count_for(pe, detected) > anchor_count_for(pe, g_fac_cache[i].faction)) {
      g_fac_cache[i].faction = detected;
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

static void apply_seat_labels(MatchEconomy* out) {
  if (!out) return;
  for (int i = 0; i < out->player_count; ++i) {
    PlayerEconomy& pe = out->players[i];
    const int faction = remember_faction(pe.player, pe, detect_faction(pe));
    const char* fac = faction == 1 ? u8"盟军" : faction == 2 ? u8"苏联" : faction == 3 ? u8"帝国" : u8"玩家";
    std::snprintf(pe.name, sizeof(pe.name), "%s", fac);
  }
}

// GameLogic+0x148. 1/2/4 are live matches; 6/7 are replay variants of those.
// 9 is the freshly constructed / shell value. Anything else is not a battle.
static uint32_t g_battle_logic = 0;
static float g_battle_seconds = -1.f;

static bool read_battle_clock(uint32_t* logic_out, float* seconds_out) {
  if (logic_out) *logic_out = 0;
  if (seconds_out) *seconds_out = 0.f;
  uint32_t logic = 0;
  if (!safe_read_u32(module_base() + 0x8DDE84, &logic) || !is_ptr(logic)) return false;
  uint32_t mode = 0;
  if (!safe_read_u32(logic + 0x148, &mode)) return false;
  const bool active = mode == 1 || mode == 2 || mode == 4 || mode == 6 || mode == 7;
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
  *out = MatchEconomy{};
  if (!module_base()) {
    std::snprintf(out->note, sizeof(out->note), u8"未找到游戏模块");
    return false;
  }

  out->spectator = is_spectator();

  uint32_t battle_logic = 0;
  float battle_seconds = 0.f;
  if (!read_battle_clock(&battle_logic, &battle_seconds)) {
    clear_roster_caches();
    g_battle_logic = 0;
    g_battle_seconds = -1.f;
    std::snprintf(out->note, sizeof(out->note), u8"未进入对局");
    return false;
  }
  if (battle_logic != g_battle_logic ||
      (g_battle_seconds >= 0.f && battle_seconds + 2.f < g_battle_seconds)) {
    clear_roster_caches();
  }
  g_battle_logic = battle_logic;
  g_battle_seconds = battle_seconds;
  out->match_seconds = battle_seconds;

  uint32_t list = 0;
  if (!safe_read_u32(module_base() + kLocalPlayerRva, &list) || !is_ptr(list)) {
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
    fill_player_economy_basic(&out->players[0], local, local);
    out->player_count = 1;
  }

  // Unit/building tallies for every seat (incl. other players).
  fill_all_rosters(out);

  // Drop spectators and seats that have not entered. Keep anyone already
  // shown this match, including after defeat.
  {
    int kept = 0;
    for (int i = 0; i < out->player_count; ++i) {
      PlayerEconomy& pe = out->players[i];
      const int seen_i = find_seen_seat(pe.player);
      if (!seat_entered_battle(pe) && seen_i < 0) continue;
      mark_defeated_flag(&pe);
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

  {
    bool present[kMaxEconPlayers] = {};
    for (int i = 0; i < out->player_count; ++i) {
      PlayerEconomy& pe = out->players[i];
      int seen_i = find_seen_seat(pe.player);
      if (seen_i < 0 && g_seen_n < kMaxEconPlayers &&
          (seat_entered_battle(pe) || pe.defeated)) {
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
static bool g_sw_toggle_latched = false;
static int g_lock_tick_depth = 0;
static bool g_lock_syncing = false;
static void sync_disable_superweapon_toggle();

static bool thiscall_build_lock(uint32_t player, uint32_t tmpl, bool locked) {
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

static void apply_ban_to_players(BuildBan* ban, const uint32_t* players, int pn) {
  if (!ban || ban->tmpl_n <= 0 || pn <= 0) return;
  int kept = 0;
  for (int i = 0; i < ban->applied_n; ++i) {
    bool live = false;
    for (int p = 0; p < pn; ++p) {
      if (players[p] == ban->applied[i]) live = true;
    }
    if (live) ban->applied[kept++] = ban->applied[i];
  }
  ban->applied_n = kept;
  for (int p = 0; p < pn; ++p) {
    bool done = false;
    for (int i = 0; i < ban->applied_n; ++i) {
      if (ban->applied[i] == players[p]) done = true;
    }
    if (done) continue;
    bool any = false;
    for (int t = 0; t < ban->tmpl_n; ++t) {
      if (thiscall_build_lock(players[p], ban->tmpls[t], true)) any = true;
    }
    if (any && ban->applied_n < kMaxEconPlayers) {
      ban->applied[ban->applied_n++] = players[p];
    }
  }
}

static void forget_lock_targets() {
  g_build_catalog_ready = false;
  for (BuildBan& ban : g_build_bans) {
    ban.applied_n = 0;
    ban.tmpl_n = 0;
  }
  ensure_tpl_cs();
  EnterCriticalSection(&g_tpl_cs);
  g_tpl_type_cache.clear();
  LeaveCriticalSection(&g_tpl_cs);
}

static void build_lock_tick_impl() {
  if (g_lock_tick_depth) return;
  const DWORD now = GetTickCount();
  if (g_build_lock_tick != 0 && now - g_build_lock_tick < 400) return;
  g_build_lock_tick = now;
  ++g_lock_tick_depth;
  uint32_t battle_logic = 0;
  float battle_seconds = 0.f;
  const bool in_battle = read_battle_clock(&battle_logic, &battle_seconds);
  if (!in_battle) {
    if (g_lock_battle_logic != 0 || g_lock_battle_seconds >= 0.f) {
      for (BuildBan& ban : g_build_bans) {
        ban.applied_n = 0;
        ban.tmpl_n = 0;
      }
      g_build_catalog_ready = false;
    }
    g_lock_battle_logic = 0;
    g_lock_battle_seconds = -1.f;
  } else {
    const bool restarted = battle_logic != g_lock_battle_logic ||
                           (g_lock_battle_seconds >= 0.f &&
                            battle_seconds + 2.f < g_lock_battle_seconds);
    if (restarted && (g_lock_battle_logic != 0 || g_lock_battle_seconds >= 0.f)) {
      forget_lock_targets();
    }
    g_lock_battle_logic = battle_logic;
    g_lock_battle_seconds = battle_seconds;
  }
  sync_disable_superweapon_toggle();
  if (!g_build_bans.empty() && module_base() && in_battle) {
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
      face_set(out, "sw_jp_nano", u8"纳米虫群核心", "JapanSuperWeapon", u8"纳米虫群", egg ? 0 : 1,
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

}  // namespace

void install_roster_hooks() { install_roster_hooks_impl(); }

bool collect_match_economy(MatchEconomy* out) { return collect_match_economy_impl(out); }

const BuildLockEntry* build_lock_catalog(int* count) { return build_lock_catalog_impl(count); }

bool build_lock_set(const char* type_id, bool locked, std::string* out_msg) {
  return build_lock_set_impl(type_id, locked, out_msg);
}

bool build_lock_selected(bool locked, std::string* out_msg) {
  return build_lock_selected_impl(locked, out_msg);
}

void build_lock_tick() { build_lock_tick_impl(); }

void build_lock_sync_disable_superweapon() { sync_disable_superweapon_toggle(); }

bool adjust_local_money(int delta, std::string* out_msg) {
  if (!module_base()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }
  uint32_t local = local_owner_for_ops();
  if (!is_ptr(local)) {
    if (out_msg) *out_msg = u8"未找到己方玩家（请先进入对局）";
    return false;
  }
  return apply_money_delta(local, delta, out_msg);
}

bool adjust_selected_player_money(int delta, std::string* out_msg) {
  if (!module_base()) {
    if (out_msg) *out_msg = u8"未找到游戏模块";
    return false;
  }
  auto ents = selected_entities_stable();
  if (ents.empty()) {
    if (out_msg) *out_msg = u8"请先选中该玩家的一个单位或建筑";
    return false;
  }
  uint32_t owner = 0;
  if (!safe_read_u32(ents[0] + 0x418, &owner) || !is_ptr(owner)) {
    if (out_msg) *out_msg = u8"读不到选中单位的所属玩家";
    return false;
  }
  return apply_money_delta(owner, delta, out_msg);
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
  if (std::strcmp(key, "unit_clone") == 0 || std::strcmp(key, "spawn_unit") == 0) {
    return clone_selected(!is_spectator(), 1, out_msg);
  }
  if (std::strcmp(key, "spec_gift") == 0) return clone_selected(false, 1, out_msg);
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
  if (std::strcmp(key, "spawn_mcv") == 0) {
    if (out_msg)
      *out_msg = u8"召唤基地车仍不稳定，请暂用 Python 版或选中基地车后「复制到己方」";
    return false;
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

}  // namespace game_api
