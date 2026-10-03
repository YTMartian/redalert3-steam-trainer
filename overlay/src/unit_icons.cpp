#include "unit_icons.h"

#include <Windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace unit_icons {
namespace {

IDirect3DDevice9* g_device = nullptr;
bool g_indexed = false;

struct FileRef {
  std::wstring path;
  std::string faction;  // 苏联 / 盟军 / 帝国 / 中立单位建筑 / 战役建筑 / 特殊单位
  bool building = false;
  std::string name;  // basename without .png
};

std::vector<FileRef> g_files;
// name(utf8) -> indices into g_files
std::map<std::string, std::vector<int>> g_by_name;

struct TexCache {
  IDirect3DTexture9* tex = nullptr;
  bool tried = false;
};
std::map<std::string, TexCache> g_tex;  // cache key = faction|B/U|name

std::string narrow_utf8(const wchar_t* w) {
  if (!w || !w[0]) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring widen_utf8(const char* u8) {
  if (!u8 || !u8[0]) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring w((size_t)n - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, u8, -1, w.data(), n);
  return w;
}

std::wstring module_dir_w() {
  wchar_t path[MAX_PATH] = {};
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&module_dir_w), &self) ||
      !self) {
    return {};
  }
  GetModuleFileNameW(self, path, MAX_PATH);
  wchar_t* slash = wcsrchr(path, L'\\');
  if (slash) *slash = 0;
  return path;
}

bool dir_exists(const std::wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring find_images_root() {
  const std::wstring mod = module_dir_w();
  const std::wstring candidates[] = {
      mod + L"\\unit_images",
      mod + L"\\..\\unit_images",
      mod + L"\\..\\..\\unit_images",
  };
  for (const auto& c : candidates) {
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr) && dir_exists(full)) {
      return full;
    }
  }
  return {};
}

void index_dir(const std::wstring& root) {
  g_files.clear();
  g_by_name.clear();
  g_indexed = false;
  if (root.empty()) return;

  const std::wstring pattern = root + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;

  auto add_png = [&](const std::wstring& dir, const wchar_t* file, const std::string& faction,
                     bool building) {
    std::wstring base = file;
    const size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    FileRef ref;
    ref.path = dir + L"\\" + file;
    ref.faction = faction;
    ref.building = building;
    ref.name = narrow_utf8(base.c_str());
    if (ref.name.empty()) return;
    const int idx = (int)g_files.size();
    g_files.push_back(ref);
    g_by_name[ref.name].push_back(idx);
  };

  // unit_images/{faction}/{单位|建筑}/*.png
  // unit_images/{中立单位建筑|战役建筑|特殊单位}/*.png  (flat folders)
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (fd.cFileName[0] == L'.') continue;
    const std::string faction = narrow_utf8(fd.cFileName);

    if (faction == u8"中立单位建筑" || faction == u8"战役建筑" || faction == u8"特殊单位") {
      const std::wstring kind_dir = root + L"\\" + fd.cFileName;
      const std::wstring glob = kind_dir + L"\\*.png";
      WIN32_FIND_DATAW ff{};
      HANDLE hf = FindFirstFileW(glob.c_str(), &ff);
      if (hf == INVALID_HANDLE_VALUE) continue;
      do {
        if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        add_png(kind_dir, ff.cFileName, faction, faction != u8"特殊单位");
      } while (FindNextFileW(hf, &ff));
      FindClose(hf);
      continue;
    }

    if (faction != u8"苏联" && faction != u8"盟军" && faction != u8"帝国") continue;

    const wchar_t* kinds[] = {L"单位", L"建筑"};
    for (const wchar_t* kind : kinds) {
      const bool building = (wcscmp(kind, L"建筑") == 0);
      const std::wstring kind_dir = root + L"\\" + fd.cFileName + L"\\" + kind;
      const std::wstring glob = kind_dir + L"\\*.png";
      WIN32_FIND_DATAW ff{};
      HANDLE hf = FindFirstFileW(glob.c_str(), &ff);
      if (hf == INVALID_HANDLE_VALUE) continue;
      do {
        if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        add_png(kind_dir, ff.cFileName, faction, building);
      } while (FindNextFileW(hf, &ff));
      FindClose(hf);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  g_indexed = true;
}

const char* faction_from_key(const char* key) {
  if (!key) return "";
  if (_strnicmp(key, "Soviet", 6) == 0) return u8"苏联";
  if (_strnicmp(key, "Allied", 6) == 0) return u8"盟军";
  if (_strnicmp(key, "Japan", 5) == 0) return u8"帝国";
  return "";
}

static bool looks_like_neutral_key(const char* key) {
  if (!key || !key[0]) return false;
  if (faction_from_key(key)[0]) return false;
  return std::strstr(key, "Hospital") || std::strstr(key, "Garage") ||
         std::strstr(key, "Observation") || std::strstr(key, "OilDerrick") ||
         std::strstr(key, "OreNode") || std::strstr(key, "TechBuilding") ||
         std::strstr(key, "ShipYardTech") || std::strstr(key, "AirportTech") ||
         std::strstr(key, "Veterancy") || std::strstr(key, "DefensiveStructure") ||
         std::strstr(key, "Bridge") || std::strstr(key, "Civilian");
}

static bool looks_like_japan_nano_core(const char* type_key, const char* disp) {
  // Packaged building cores ("道場核心" / Japan*Egg) share one generic icon.
  if (type_key && type_key[0]) {
    if (std::strncmp(type_key, "Japan", 5) == 0 && std::strstr(type_key, "Egg")) {
      return true;
    }
    if (std::strcmp(type_key, "JapanOutpost") == 0) return true;
  }
  if (disp && disp[0] && std::strstr(disp, u8"核心")) {
    // Keep finished structures that aren't pack cores on their own art.
    if (std::strstr(disp, u8"纳米主机") || std::strstr(disp, u8"奈米主機")) return false;
    if (std::strstr(disp, u8"电力核心") || std::strstr(disp, u8"電力核心")) return false;
    return true;
  }
  return false;
}

// Our display / TypeId names → image basename (Chinese filename without .png).
const char* alias_image_name(const char* type_key, const char* disp) {
  if (looks_like_japan_nano_core(type_key, disp)) {
    return u8"纳米核心（通用）";
  }
  if (type_key && type_key[0]) {
    struct Pair {
      const char* key;
      const char* img;
    };
    static const Pair kKey[] = {
        {"SovietSurveyor", u8"史普尼克勘探车"},
        {"SovietSurveyor_Naval", u8"史普尼克勘探车"},
        {"SovietAntiStructureVehicle", u8"V4导弹发射车"},
        {"SovietAntiStructureShip", u8"无畏战列舰"},
        {"SovietMiner", u8"苏联矿车"},
        {"AlliedMiner", u8"勘探者"},
        {"AlliedMiner_Naval", u8"勘探者"},
        {"JapanMiner", u8"采矿车"},
        {"SovietConstructionYard", u8"建筑工厂"},
        {"SovietConYard", u8"建筑工厂"},
        {"SovietBaseDefenseGround", u8"哨兵枪"},
        {"SovietTechStructure", u8"作战实验室"},
        {"SovietOutpost", u8"前线基地"},
        {"SovietVacuumImploder", u8"真空内爆弹"},
        {"SovietSuperWeapon", u8"真空内爆弹"},
        {"SovietSuperWeaponAdvanced", u8"真空内爆弹"},
        {"SOVIETSUPERWEAPONADVANCED", u8"真空内爆弹"},
        {"SovietNavalYard", u8"海军造船厂"},
        {"SovietShipyard", u8"海军造船厂"},
        {"SovietAntiVehicleInfantry", u8"防空步兵"},
        {"SovietAntiAirInfantry", u8"防空步兵"},
        {"SovietFighterAircraft", u8"米格战斗机"},
        {"SovietAntiAirVehicleTech1", u8"牛蛙载具"},
        {"SovietAntiAirShip", u8"牛蛙载具"},
        {"SovietAntiAirShip_Ground", u8"牛蛙载具"},
        {"SovietHeavyAntiVehicleInfantry", u8"磁暴步兵"},
        {"AlliedConstructionYard", u8"建筑工厂"},
        {"AlliedConYard", u8"建筑工厂"},
        {"AlliedBaseDefense", u8"多功能炮塔"},
        {"AlliedInfantryVehicle", u8"多功能步兵战车"},
        {"AlliedAntiAirVehicleTech1", u8"多功能步兵战车"},
        {"AlliedTechStructure", u8"防卫局"},
        {"AlliedOutpost", u8"指挥中心"},
        {"AlliedProtonCollider", u8"质子撞击炮"},
        {"AlliedAntiVehicleVehicleTech3", u8"幻影坦克"},
        {"JapanConstructionYard", u8"建筑工厂"},
        {"JapanConYard", u8"建筑工厂"},
        {"JapanPowerPlant", u8"瞬息发电厂"},
        {"JapanAntiStructureVehicle", u8"波能炮"},
        {"JapanTechStructure", u8"纳米主机"},
        {"JapanNanotechMainframe", u8"纳米主机"},
        {"JapanPsionicDecimator", u8"超能波毁灭装置"},
        {"JapanSuperWeaponAdvanced", u8"超能波毁灭装置"},
        {"JAPANSUPERWEAPONADVANCED", u8"超能波毁灭装置"},
        {"JapanLightTransportVehicle", u8"迅雷运输艇"},
        {"JapanNavyScoutShip", u8"长枪迷你潜艇"},
        {"JapanAntiVehicleVehicle", u8"鬼王"},
        {"JapanBaseDefenseAdv", u8"波能塔"},
        {"JapanOutpost", u8"纳米核心（通用）"},
        // Neutral / tech map props → 中立单位建筑/
        {"HospitalTechBuilding", u8"医院"},
        {"HospitalTechStructure", u8"医院"},
        {"GarageTechStructure", u8"车库"},
        {"ObservationPostTechStructure", u8"监视站"},
        {"ObservationPostTechBuilding", u8"监视站"},
        {"OilDerrick", u8"油井"},
        {"OilDerrick_OnWater", u8"油井"},
        {"TechBuildingOilDerrick", u8"油井"},
        {"ShipYardTechStructure", u8"船坞"},
        {"AirportTechStructure", u8"机场"},
        {"VeterancyTechStructure", u8"精兵学院"},
        {"DefensiveStructureTechStructure", u8"前哨战"},
        {"OreNode", u8"矿脉"},
        {"TechBuildingOreNode", u8"矿脉"},
        {"TechBuildingOreNode2", u8"矿脉"},
        {"TechBuildingOreNode4", u8"矿脉"},
        {"OreNode2a", u8"矿脉"},
        {"OreNode2b", u8"矿脉"},
        {"OreNode4a", u8"矿脉"},
        {"OreNode4b", u8"矿脉"},
        {"OreNode4c", u8"矿脉"},
        {"OreNode4d", u8"矿脉"},
    };
    for (const auto& p : kKey) {
      if (std::strcmp(type_key, p.key) == 0) return p.img;
    }
  }
  if (disp && disp[0]) {
    struct Pair {
      const char* from;
      const char* to;
    };
    static const Pair kDisp[] = {
        {u8"史普尼克勘察车", u8"史普尼克勘探车"},
        {u8"V4火箭发射车", u8"V4导弹发射车"},
        {u8"无畏战舰", u8"无畏战列舰"},
        {u8"建造工厂", u8"建筑工厂"},
        {u8"建造场", u8"建筑工厂"},
        {u8"哨枪", u8"哨兵枪"},
        {u8"战斗研究所", u8"作战实验室"},
        {u8"前哨站", u8"前线基地"},
        {u8"真空内爆器", u8"真空内爆弹"},
        {u8"真空內爆彈", u8"真空内爆弹"},
        {u8"多功能步兵炮塔", u8"多功能炮塔"},
        {u8"多功能步兵车", u8"多功能步兵战车"},
        {u8"瞬息发电机", u8"瞬息发电厂"},
        {u8"波能坦克", u8"波能炮"},
        {u8"纳米科技电脑主机", u8"纳米主机"},
        {u8"心灵毁灭者", u8"超能波毁灭装置"},
        {u8"超能波毀滅裝置", u8"超能波毁灭装置"},
        {u8"质子碰撞器", u8"质子撞击炮"},
        {u8"質子撞擊炮", u8"质子撞击炮"},  // CSF traditional
        {u8"質子碰撞器", u8"质子撞击炮"},
        {u8"质子撞击跑", u8"质子撞击炮"},  // old typo
        {u8"海军船坞", u8"海军造船厂"},
        {u8"科技中心", u8"防卫局"},
        {u8"牛蛙步行机甲", u8"牛蛙载具"},
        {u8"米格战机", u8"米格战斗机"},
        {u8"防空部队", u8"防空步兵"},
        {u8"迅雷运输车", u8"迅雷运输艇"},
        {u8"迷你潜艇", u8"长枪迷你潜艇"},
        {u8"干船坞", u8"船坞"},
        {u8"乾船坞", u8"船坞"},
        {u8"中立机场", u8"机场"},
        {u8"机场控制塔", u8"机场"},
        {u8"监视哨", u8"监视站"},
        {u8"中立防御基地", u8"前哨战"},
        {u8"矿井高塔", u8"油井"},
        {u8"钻井高塔", u8"油井"},
        {u8"盟军V.I.P.碉堡", u8"盟军VIP碉堡"},
        {u8"帝国V.I.P.碉堡", u8"帝国VIP碉堡"},
        {u8"俄军VIP碉堡", u8"苏联VIP碉堡"},
        {u8"苏联母亲的雕像", u8"苏联母亲雕像"},
        {u8"远距离雷达", u8"远程雷达"},
        {u8"天西机械公司", u8"天西机械实验室"},
        {u8"天皇的宫殿", u8"天皇芳郎的宫殿"},
        {u8"克里姆林的中心", u8"克里姆林宫"},
        {u8"布莱顿沿岸炮台", u8"布莱顿海岸炮"},
        {u8"帝国港口", u8"帝国港口建筑"},
        {u8"港口管理处", u8"港口管理中心"},
        {u8"白田港口", u8"白田港"},
        {u8"纽约证交所", u8"纽约证券交易所"},
        {u8"科技抑制装置", u8"科技抑制器"},
        {u8"要塞电缆", u8"要塞电力核心"},
        {u8"傲德萨教堂", u8"敖德萨教堂"},
        {u8"傲德萨歌剧院", u8"敖德萨歌剧院"},
        {u8"傲德萨要塞", u8"敖德萨城堡"},
        {u8"未来科技总部", u8"未来科技公司"},
        {u8"名椎道馆", u8"长间道场"},
        {u8"东山高阶指挥", u8"帝国指挥总部"},
        {u8"苏联总部", u8"海德堡总部"},
        {u8"科学设施", u8"科技实验室"},
        {u8"补给港务局", u8"补给中心"},
        {u8"桥梁警卫室", u8"桥梁"},
        {u8"华盛顿胸像", u8"华盛顿"},
        {u8"林肯胸像", u8"林肯"},
        {u8"罗斯福胸像", u8"罗斯福"},
        {u8"杰佛逊胸像", u8"杰斐逊"},
        {u8"末日杰佛逊", u8"末日杰斐逊"},
        {u8"华盛顿石像控制", u8"机器头像控制中心"},
        {u8"林肯石像控制", u8"机器头像控制中心"},
        {u8"杰斐逊头像控制", u8"机器头像控制中心"},
        {u8"杰佛逊头像控制", u8"机器头像控制中心"},
        {u8"拉什莫尔发射基地", u8"总统山发射塔"},
        {u8"拉什莫尔通讯塔", u8"总统山发射塔"},
        {u8"双矿脉", u8"矿脉"},
        {u8"四矿脉", u8"矿脉"},
        {u8"探矿车", u8"勘探者"},
        {u8"探礦車", u8"勘探者"},
    };
    for (const auto& p : kDisp) {
      if (std::strcmp(disp, p.from) == 0) return p.to;
    }
    return disp;
  }
  return nullptr;
}

// Fold a few common traditional glyphs so CSF names match simplified PNG filenames.
static std::string fold_zh_icon_name(const char* u8) {
  if (!u8 || !u8[0]) return {};
  std::string s = u8;
  struct Pair {
    const char* from;
    const char* to;
  };
  static const Pair kFold[] = {
      {u8"質", u8"质"}, {u8"擊", u8"击"}, {u8"車", u8"车"}, {u8"採", u8"采"},
      {u8"礦", u8"矿"}, {u8"戰", u8"战"}, {u8"國", u8"国"}, {u8"機", u8"机"},
      {u8"毀", u8"毁"}, {u8"滅", u8"灭"}, {u8"裝", u8"装"}, {u8"內", u8"内"},
      {u8"動", u8"动"}, {u8"衛", u8"卫"}, {u8"達", u8"达"}, {u8"艦", u8"舰"},
      {u8"彈", u8"弹"}, {u8"導", u8"导"}, {u8"擊", u8"击"}, {u8"擊", u8"击"},
      {u8"廠", u8"厂"}, {u8"場", u8"场"}, {u8"營", u8"营"}, {u8"術", u8"术"},
      {u8"無", u8"无"}, {u8"電", u8"电"}, {u8"氣", u8"气"}, {u8"飛", u8"飞"},
      {u8"轉", u8"转"}, {u8"傳", u8"传"}, {u8"層", u8"层"}, {u8"衝", u8"冲"},
      {u8"鍾", u8"钟"}, {u8"鐵", u8"铁"}, {u8"鋼", u8"钢"}, {u8"總", u8"总"},
      {u8"協", u8"协"}, {u8"聯", u8"联"}, {u8"軍", u8"军"}, {u8"禦", u8"御"},
      {u8"擊", u8"击"},
  };
  for (const auto& p : kFold) {
    for (;;) {
      size_t pos = s.find(p.from);
      if (pos == std::string::npos) break;
      s.replace(pos, std::strlen(p.from), p.to);
    }
  }
  return s;
}

static std::string simplify_zh(const char* u8) {
  std::wstring w = widen_utf8(u8);
  if (w.empty()) return {};
  const int n = LCMapStringW(0x0804, LCMAP_SIMPLIFIED_CHINESE, w.c_str(), (int)w.size(),
                             nullptr, 0);
  if (n <= 0) return narrow_utf8(w.c_str());
  std::wstring out((size_t)n, L'\0');
  LCMapStringW(0x0804, LCMAP_SIMPLIFIED_CHINESE, w.c_str(), (int)w.size(), out.data(), n);
  return narrow_utf8(out.c_str());
}

const FileRef* pick_file(const char* type_key, const char* disp, bool building) {
  if (!g_indexed) {
    index_dir(find_images_root());
  }
  const char* faction = faction_from_key(type_key);
  const char* names_try[8] = {};
  std::string folded;
  std::string simplified;
  std::string folded_simple;
  int ntry = 0;
  auto push_name = [&](const char* s) {
    if (!s || !s[0] || ntry >= 8) return;
    for (int i = 0; i < ntry; ++i) {
      if (names_try[i] && std::strcmp(names_try[i], s) == 0) return;
    }
    names_try[ntry++] = s;
  };
  const char* aliased = alias_image_name(type_key, disp);
  push_name(aliased);
  push_name(disp);
  if (disp && disp[0]) {
    folded = fold_zh_icon_name(disp);
    push_name(folded.c_str());
    simplified = simplify_zh(disp);
    push_name(simplified.c_str());
    if (!simplified.empty()) {
      push_name(alias_image_name(nullptr, simplified.c_str()));
      folded_simple = fold_zh_icon_name(simplified.c_str());
      push_name(folded_simple.c_str());
    }
  }

  // Miners: never cross-faction (Soviet/Japan/Allied arts differ).
  if (type_key && std::strncmp(type_key, "SovietMiner", 11) == 0) {
    names_try[0] = u8"苏联矿车";
    ntry = 1;
  } else if (type_key && std::strncmp(type_key, "JapanMiner", 10) == 0) {
    names_try[0] = u8"采矿车";
    ntry = 1;
  } else if (type_key && std::strncmp(type_key, "AlliedMiner", 11) == 0) {
    // Allied ore collector uses the same art/name as Surveyor (勘探者).
    names_try[0] = u8"勘探者";
    ntry = 1;
  }

  const bool want_neutral = looks_like_neutral_key(type_key) || !faction[0];
  auto score = [&](const FileRef& f) -> int {
    const bool is_neutral =
        (f.faction == u8"中立单位建筑" || f.faction == u8"战役建筑");
    // Known faction → only own folder (or neutral). Never steal another faction's art.
    if (faction[0] && f.faction != faction && !is_neutral) return -1;
    int s = 0;
    if (faction[0] && f.faction == faction) s += 100;
    if (is_neutral) {
      if (want_neutral) s += 90;
      else s += 15;  // weak fallback only when own-faction art missing
    }
    if (f.building == building) s += 20;
    else if (is_neutral) s += 10;
    return s;
  };

  const FileRef* best = nullptr;
  int best_sc = -1;
  for (int t = 0; t < ntry; ++t) {
    auto it = g_by_name.find(names_try[t]);
    if (it == g_by_name.end()) continue;
    for (int idx : it->second) {
      const int sc = score(g_files[idx]);
      if (sc < 0) continue;
      if (sc > best_sc) {
        best_sc = sc;
        best = &g_files[idx];
      }
    }
    if (best && best_sc >= 100) break;
  }
  // "火山要塞" / "基洛夫发射台" → the numbered file (…1, …2) when there is no exact png.
  if (!best) {
    int num_ord = 9999;
    for (int t = 0; t < ntry; ++t) {
      const std::string q = names_try[t] ? names_try[t] : "";
      if (q.empty()) continue;
      for (const auto& kv : g_by_name) {
        const std::string& fn = kv.first;
        if (fn.size() <= q.size() || fn.compare(0, q.size(), q) != 0) continue;
        bool digits = true;
        int ord = 0;
        for (size_t i = q.size(); i < fn.size(); ++i) {
          if (fn[i] < '0' || fn[i] > '9') {
            digits = false;
            break;
          }
          ord = ord * 10 + (fn[i] - '0');
        }
        if (!digits) continue;
        for (int idx : kv.second) {
          const int sc = score(g_files[idx]);
          if (sc < 0) continue;
          if (sc > best_sc || (sc == best_sc && ord < num_ord)) {
            best_sc = sc;
            num_ord = ord;
            best = &g_files[idx];
          }
        }
      }
    }
  }
  return best;
}

IDirect3DTexture9* load_png(const std::wstring& path) {
  if (!g_device || path.empty()) return nullptr;

  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))) ||
      !factory) {
    // OLE may not be inited in game thread.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        !factory) {
      return nullptr;
    }
  }

  IWICBitmapDecoder* decoder = nullptr;
  HRESULT hr = factory->CreateDecoderFromFilename(
      path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(hr) || !decoder) {
    factory->Release();
    return nullptr;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr) || !frame) {
    decoder->Release();
    factory->Release();
    return nullptr;
  }

  IWICFormatConverter* conv = nullptr;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv) {
    frame->Release();
    decoder->Release();
    factory->Release();
    return nullptr;
  }

  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                        nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    return nullptr;
  }

  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 2048 || h > 2048) {
    conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    return nullptr;
  }

  std::vector<BYTE> pixels((size_t)w * h * 4);
  hr = conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
  conv->Release();
  frame->Release();
  decoder->Release();
  factory->Release();
  if (FAILED(hr)) return nullptr;

  IDirect3DTexture9* tex = nullptr;
  hr = g_device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex,
                               nullptr);
  if (FAILED(hr) || !tex) return nullptr;

  D3DLOCKED_RECT lr{};
  if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
    tex->Release();
    return nullptr;
  }
  for (UINT y = 0; y < h; ++y) {
    BYTE* dst = static_cast<BYTE*>(lr.pBits) + y * lr.Pitch;
    const BYTE* src = pixels.data() + (size_t)y * w * 4;
    // WIC BGRA -> D3D A8R8G8B8 (same byte order on LE)
    std::memcpy(dst, src, (size_t)w * 4);
  }
  tex->UnlockRect(0);
  return tex;
}

std::string cache_key_for(const FileRef& f) {
  return f.faction + "|" + (f.building ? "B" : "U") + "|" + f.name;
}

}  // namespace

void set_device(IDirect3DDevice9* device) { g_device = device; }

void init(IDirect3DDevice9* device) {
  g_device = device;
  g_indexed = false;
  g_files.clear();
  g_by_name.clear();
  index_dir(find_images_root());
}

void shutdown() {
  for (auto& kv : g_tex) {
    if (kv.second.tex) {
      kv.second.tex->Release();
      kv.second.tex = nullptr;
    }
  }
  g_tex.clear();
  g_files.clear();
  g_by_name.clear();
  g_indexed = false;
  g_device = nullptr;
}

IDirect3DTexture9* get(const char* type_key, const char* disp_name, bool is_building) {
  if (!g_device) return nullptr;
  const FileRef* ref = pick_file(type_key, disp_name, is_building);
  if (!ref) return nullptr;

  const std::string ck = cache_key_for(*ref);
  TexCache& c = g_tex[ck];
  if (c.tried) return c.tex;
  c.tried = true;
  c.tex = load_png(ref->path);
  return c.tex;
}

}  // namespace unit_icons
