#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace game_api {

void init();
void shutdown();

const char* status_text();
void set_status(const char* text);
void log(const char* fmt, ...);
// Writes the faulting address and call stack to ra3_overlay.log beside the DLL. exception_pointers
// is EXCEPTION_POINTERS*; pass nullptr only to note a site with no context.
void log_exception(void* exception_pointers, const char* where);
void install_crash_filter();

void beep(const char* kind);  // on / off / click / error

bool ready();
bool hooks_armed();
bool is_spectator();
const char* mode_label();

// Allocate MustCode + arm hooks in one step.
bool inject(bool spectate_mode);
bool arm_hooks();
bool inject_full(bool spectate_mode);  // inject + arm_hooks
void detach();

uint8_t get_flag(uint32_t offset);
bool set_flag(uint32_t offset, uint8_t value);
bool pulse_flag(uint32_t offset, float hold_sec);

std::vector<uint32_t> selected_entities(int limit = 64);
// Live selection, or last non-empty cache (survives ImGui click deselect).
std::vector<uint32_t> selected_entities_stable(int limit = 64);
void refresh_selection_cache();
int selected_count();

// Snapshot of the first selected unit (multi-select → first only).
struct UnitInspect {
  bool valid = false;
  int selected_total = 0;
  uint32_t ent = 0;
  uint32_t unit_data = 0;
  uint32_t owner = 0;
  bool is_mine = false;
  char type_id[96] = {};
  char type_name[96] = {};
  char name_key[96] = {};  // persistence key (TypeId or tpl_XXXXXXXX)
  bool has_hp = false;
  float hp_cur = 0.f;
  float hp_disp = 0.f;
  float hp_max = 0.f;
  bool has_speed = false;
  float speed = 0.f;
  bool has_rank = false;
  uint32_t rank_level = 0;
  float xp = 0.f;
  float xp_next = 0.f;
  float damage_mult = 0.f;
  // Primary weapon damage class (RA3 armor matrix). Not per-shot raw damage.
  bool has_damage_type = false;
  char damage_type[48] = {};      // e.g. CANNON
  char damage_type_zh[64] = {};   // e.g. 加农/穿甲
};

// ---- Match economy ----
// Money:  [[player+0xE4]]+0x04
// Power:  [player+0x74]+0x08 used / +0x04 total  (MustCode Power hook fields)
constexpr int kMaxEconPlayers = 16;
constexpr int kMaxRosterTypes = 64;

struct RosterType {
  char key[96] = {};   // stable type id (for order / identity)
  char name[64] = {};  // display (zh or type_id)
  int count = 0;
  bool is_building = false;
};

struct PlayerEconomy {
  uint32_t player = 0;
  uint32_t player_id = 0;
  bool is_local = false;
  bool defeated = false;
  char name[64] = {};
  bool has_color = false;
  uint8_t color_r = 210;
  uint8_t color_g = 214;
  uint8_t color_b = 220;
  bool has_money = false;
  uint32_t money = 0;
  bool has_power = false;
  uint32_t power_used = 0;
  uint32_t power_total = 0;
  int unit_total = 0;
  int building_total = 0;
  // Official ScoreKeeper at player+0x1F0. Cumulative for the whole match.
  bool has_score = false;
  uint32_t units_built = 0;
  uint32_t units_lost = 0;
  uint32_t units_destroyed = 0;
  uint32_t buildings_built = 0;
  uint32_t buildings_lost = 0;
  uint32_t buildings_destroyed = 0;
  uint32_t money_earned = 0;
  uint32_t money_spent = 0;
  int roster_count = 0;
  RosterType roster[kMaxRosterTypes] = {};
  // Plain text: "征召兵 5  防空部队 3"
  char roster_text[768] = {};
};

struct MatchEconomy {
  bool valid = false;
  bool spectator = false;
  float match_seconds = 0.f;
  int player_count = 0;
  PlayerEconomy players[kMaxEconPlayers] = {};
  char note[192] = {};
};

bool collect_match_economy(MatchEconomy* out);
bool inspect_first_selected(UnitInspect* out);

// Per-player production lock. The game keeps a disabled-type list on each
// Player (script: cannot build this object type). Bans apply to every seat
// in the current match and are reapplied after a new match starts.
struct BuildLockEntry {
  char type_id[96] = {};
  char name[64] = {};
  char group[96] = {};
  char icon_id[96] = {};
  char icon_name[64] = {};
  int group_pref = 0;
  bool building = false;
  bool banned = false;
  bool superweapon = false;
  bool paired = false;
};

const BuildLockEntry* build_lock_catalog(int* count);
bool build_lock_set(const char* type_id, bool locked, std::string* out_msg);
bool build_lock_selected(bool locked, std::string* out_msg);
void build_lock_tick();
void build_lock_sync_disable_superweapon();
// 消散战争迷雾：未探索格子按观战的方式画成可见，关掉后按原探索结果恢复。
void sync_dispel_shroud();
// True while 禁用超武 is checked but the flag byte is held off until the match is running.
bool disable_superweapon_held();
bool build_lock_world_ready();
void note_disable_superweapon_toggle(bool enabled);

// Direct money write (no MustCode hook). Works in match / spectator.
int money_self_step();
int money_sel_step();
void set_money_self_step(int v);
void set_money_sel_step(int v);
// 0 盟军 / 1 苏联 / 2 帝国。召唤基地车每次只生成这一家的一辆。
int mcv_faction();
void set_mcv_faction(int faction);
bool adjust_local_money(int delta, std::string* out_msg);
bool adjust_selected_player_money(int delta, std::string* out_msg);

// Persistent unit display-name store (local file). File wins over builtins.
bool set_unit_display_name(const char* type_id, const char* display_name,
                           std::string* out_msg);
const char* unit_names_store_path();  // UTF-8 path, valid until next call

bool toggle_feature(const char* key, bool enabled, std::string* out_msg);
bool pulse_feature(const char* key, std::string* out_msg);
bool set_danger(int level, std::string* out_msg);
struct BattlePlayer {
  uint32_t player = 0;
  uint32_t player_id = 0;
  bool is_local = false;
  bool defeated = false;
  bool has_color = false;
  uint8_t color_r = 180;
  uint8_t color_g = 190;
  uint8_t color_b = 200;
  char name[64] = {};
};

int list_battle_players(BattlePlayer* out, int max_out);
void set_rank_player(uint32_t player);
void set_mcv_player(uint32_t player);
void set_money_player(uint32_t player);
// 召唤部队：先记下玩家、每种数量和选中的兵种，再由 run_engine("summon_troops") 生成。
bool summon_prepare(uint32_t player, int count, const char* const* type_ids, int type_count,
                    std::string* out_msg);
// 在 EndScene/Present（游戏线程）上把排队的 CreateUnit 做完，避免和模拟线程抢单位。
void drain_unit_spawns();
// 该玩家的出场等级。-1 没有这个玩家，-2 还没读到升级数据。0..3 为等级。
int player_spawn_rank(uint32_t player);
bool set_spawn_rank(int level, std::string* out_msg);
bool run_engine(const char* key, std::string* out_msg);
// Hotkey entry: toggles/pulses/engines by feature key (incl. danger_max/min/norm).
bool trigger_hotkey(const char* key, std::string* out_msg);

void sync_spectator_hooks();

struct FeatureInfo {
  const char* key;
  const char* label;
  const char* type;
  uint32_t flag;
};

struct GroupInfo {
  const char* name;
  const char* const* keys;
  int key_count;
  const char* subtitle;
};

const FeatureInfo* features(int* count);
const FeatureInfo* find_feature(const char* key);
const GroupInfo* groups(int* count);
bool feature_enabled(const char* key);
const char* hotkey_hint(const char* key);  // live binding, e.g. "Ctrl+F1"; null if none

int hotkey_slot_count();
const char* hotkey_slot_id(int index);
const char* hotkey_slot_feature(int index);
void hotkey_slot_mods(int index, int* vk, bool* ctrl, bool* alt, bool* shift);
bool set_hotkey_slot(int index, int vk, bool ctrl, bool alt, bool shift, std::string* err);
bool reset_hotkey_slot(int index);
void format_hotkey_slot(int index, char* buf, size_t buf_len);
int hotkey_slots_for_feature(const char* feature, int* indices, int cap);
bool apply_hotkey_text(const char* slot_id, const char* text);
bool hotkey_capture_active();
void set_hotkey_capture(bool on);

}  // namespace game_api
