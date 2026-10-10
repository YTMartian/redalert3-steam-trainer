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
constexpr int kMaxProtocols = 16;

struct ProtocolStatus {
  char key[96] = {};
  char name[64] = {};
  int remain_sec = 0;
};

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
  // Protocols and superweapons this seat currently has. remain_sec > 0 is a countdown.
  int protocol_count = 0;
  ProtocolStatus protocols[kMaxProtocols] = {};
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
// 协议无冷却 / 超级武器：清掉己方超级武器计时表里的每一项（含超能波毁灭装置）。
void refresh_power_cooldowns();
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

// ---------------------------------------------------------------------------
// 摄像机（对应 CameraBridge 的「摄像机操作」）
//
// 反汇编结论（ra3_1.12.game / 零售版）：
//   View 对象（W3DView，构造函数 0x7FFFE0，大小 0x4FC，主 vtable 0xC54BB0）
//     +0x08  float[12] 相机矩阵，行主序 3x4，平移在 m[3]/m[7]/m[11]
//     +0x38  float     相机世界坐标 X（由矩阵推导）
//     +0x3C  float     相机世界坐标 Y
//     +0x40  float     相机世界坐标 Z（Z 轴向上）
//     +0x44  float     偏航角（弧度）= atan2(m[4], m[0])
//     +0x260 float     相机距离（缩放）
//   0x7EC240  设置相机矩阵（this = View，第 2 参数 = float[12]），
//             并顺手把矩阵平移写回 +0x38/+0x3C/+0x40、把 angle 写回 +0x44。
//   所以矩阵才是真正的相机状态，其它字段只是缓存。
// 旋转约定：R = Rz(yaw) * Rx(pitch) * Ry(roll)，因此
//   yaw = atan2(R10, R00)，pitch = asin(R21)，roll = atan2(-R20, R22)。
// ---------------------------------------------------------------------------
struct CameraState {
  bool valid = false;
  uint32_t view = 0;                          // View 对象地址
  float x = 0.f, y = 0.f, z = 0.f;            // 世界坐标
  float yaw = 0.f, pitch = 0.f, roll = 0.f;   // 弧度
  float dist = 0.f;                           // 相机距离（0 表示没读到）
};

// 读取当前摄像机（需要先挂上摄像机 hook，或在游戏中出现过相机更新）。
bool camera_read(CameraState* out);
// 一次性把状态写进游戏（直接改 View 内存）。
bool camera_apply(const CameraState* st);
// 是否已经找到可用的 View。
bool camera_ready();
// 0 = 未安装 / 1 = 已安装 / -1 = 安装失败。返回值也写进 out_msg。
int camera_hook_state();
// 持续接管：打开后每次游戏设置相机矩阵时都会被改成 camera 目标值。
void camera_set_takeover(bool on);
bool camera_takeover();
// 接管目标（也用于手动编辑时的即时反馈）。
void camera_set_live(const CameraState* st);
bool camera_live(CameraState* out);
// 安装 0x7EC240 hook（init 时自动调用，可重复调用）。
void camera_install_hook();
// 由 Present 每帧调用：推进动画时间轴、触发定时事件、必要时写内存。
void camera_tick();
struct CameraKey {
  float t = 0.f;                              // 抵达时间（秒）
  float x = 0.f, y = 0.f, z = 0.f;
  float yaw = 0.f, pitch = 0.f, roll = 0.f, dist = 0.f;
  char label[32] = {};
};

struct CameraEvent {
  float t = 0.f;
  int base = 0;    // 0 = 动画时间 1 = 对局时间
  int action = 0;  // 见 kCameraEventAction*
  int param = 0;
  bool fired = false;
};

enum { kCamInterpLinear = 0, kCamInterpBezier = 1, kCamInterpSmooth = 2 };
enum { kCamStopped = 0, kCamPlaying = 1, kCamPaused = 2 };

int camera_key_count();
bool camera_key(int index, CameraKey* out);
int camera_key_add_from_current(const char* label);   // 返回新下标，-1 失败
bool camera_key_update_from_current(int index);
bool camera_key_set(int index, const CameraKey& key);
bool camera_key_remove(int index);
void camera_keys_clear();
float camera_total_time();

int camera_interp();
void camera_set_interp(int mode);

int camera_play_state();
float camera_playhead();
void camera_set_playhead(float t);
float camera_play_rate();
void camera_set_play_rate(float rate);
bool camera_loop();
void camera_set_loop(bool on);
void camera_play_start();
void camera_play_pause();
void camera_play_stop();

int camera_event_count();
bool camera_event(int index, CameraEvent* out);
bool camera_event_set(int index, const CameraEvent& ev);
int camera_event_add(const CameraEvent& ev);
bool camera_event_remove(int index);
void camera_events_clear();
void camera_events_reset();
int camera_events_fired();
int camera_event_feature_count();
const char* camera_event_feature_key(int index);
const char* camera_event_feature_label(int index);

int camera_config_count();
const char* camera_config_name(int index);
const char* camera_config_note(int index);
void camera_config_set_note(int index, const char* note);
int camera_config_active();
bool camera_config_select(int index);
int camera_config_add(const char* name);
int camera_config_clone(int index, const char* name);
bool camera_config_remove(int index);
bool camera_config_rename(int index, const char* name);
bool camera_config_save();
bool camera_config_load();
const char* camera_file_path();

// engine/hotkey 入口：摄像机相关的功能键。handled=false 表示不是本模块的键。
bool camera_engine_cmd(const char* key, bool* handled, std::string* out_msg);

// ---------------------------------------------------------------------------
// 游戏时间控制（对应 CameraBridge 的 RA3TimePausePlugin）
//
// 全局变量 0x00CE8138 指向全局数据对象：
//   +0x30  GameSpeedFactor（展示用）
//   +0x54  游戏时间倍率（0.6 最慢 / 1.0 正常 / 2.0 最快；0 相当于暂停）
// 改 +0x54 即可控制游戏速度。写前会严格校验指针与当前值，绝不盲写。
// ---------------------------------------------------------------------------
void time_control_install();
void time_control_tick();  // 由 Present 每帧调用
// 读取当前倍率 / GameSpeedFactor。返回是否读到了合法值。
bool time_control_read(float* scale_out, float* factor_out, bool* valid_out);
// 启用/停用持续覆盖。
void time_control_set_enabled(bool on);
bool time_control_enabled();
// 设置目标倍率并启用覆盖（0.0 = 暂停逻辑，1.0 = 常速）。
void time_control_set_scale(float scale);
float time_control_scale();
// 恢复原始倍率并停用。
void time_control_restore();
// 最近一次实际读到的值（不必启用）。
float time_control_seen_scale();
float time_control_seen_factor();
bool time_control_valid();
bool time_control_engine_cmd(const char* key, bool* handled, std::string* out_msg);

// ---- 快进（对应 CameraBridge 的「快进功能」）---------------------------------
// 目标时间是对局时钟（battle_clock）的秒数。快进期间把游戏倍率拉到 ff_speed，
// 一旦对局时钟到达目标时间就自动把时间停下（倍率 0），并清除快进状态。
void time_control_ff_start(float target_seconds, float speed);
// 取消快进（不改变当前倍率，只停止快进推进）。
void time_control_ff_cancel();
bool time_control_ff_active();
float time_control_ff_target();
float time_control_ff_speed();
// 是否在到达目标时间后自动开始快进（「自动快进至目标时间」勾选项）。
void time_control_set_ff_auto(bool on);
bool time_control_ff_auto();

// ---- 禁用游戏中的鼠标输入（对应 CameraBridge 的「在游戏中禁用输入」）--------
// 勾选后 overlay 吞掉发往游戏窗口的鼠标消息：游戏不再响应悬停/点击
// （不会出现鼠标悬停单位时的边框），但 overlay 自己的操作照常。
void set_game_input_disabled(bool on);
bool game_input_disabled();

// ---------------------------------------------------------------------------
// 单位操作扩展（对应 CameraBridge 的「选择哪些单位 / 切换单位的队伍」）
// ---------------------------------------------------------------------------
// 作用范围（「选择哪些单位」）
enum {
  kUnitScopeSelected = 0,    // 选中的单位
  kUnitScopeUnselected = 1,  // 未选中的单位
  kUnitScopeAll = 2,         // 所有的单位
  kUnitScopePlayer = 3,      // 指定玩家的单位
};
int unit_scope();
void set_unit_scope(int scope);
uint32_t unit_scope_player();     // kUnitScopePlayer 用；0 = 用本地玩家
void set_unit_scope_player(uint32_t player);
// 按当前作用范围收集单位实体。返回个数（最多 max_out）。
int unit_collect_scope(uint32_t* out, int max_out);

// 「切换单位的队伍」：把作用范围内的单位改到 target_player 名下（写归属 0x418）。
bool unit_change_team(uint32_t target_player, std::string* out_msg);
// 同上的扩展版。enable_ai = false 时改完归属再把单位速度压成 0（原地冻结），
// 对应 CameraBridge 的「启用 AI 操控」未勾选的情况。
bool unit_change_team_ex(uint32_t target_player, bool enable_ai, std::string* out_msg);

// 「切换单位的颜色」：零售版 1.12 里单位颜色来自所属玩家对象，颜色以 ARGB
// 存在玩家对象 +0x80（个别版本在 +0x84）。读写这个字段就等于给该玩家的
// 部队整体换色；「恢复原来的颜色」写回第一次改色前缓存的原值。
bool unit_color_read(uint32_t player, uint8_t* r, uint8_t* g, uint8_t* b);
bool unit_color_write(uint32_t player, uint8_t r, uint8_t g, uint8_t b, std::string* out_msg);
bool unit_color_restore(uint32_t player, std::string* out_msg);
bool unit_color_has_backup(uint32_t player);

// ---------------------------------------------------------------------------
// 杂项操作（对应 CameraBridge 的「杂项操作」）
//
// 反汇编结论（ra3_1.12.game / Steam 1.12，均为绝对 VA）：
//   0x00CE08F0  渲染对象全局指针（单例，[obj] 是虚表）。指向的对象：
//     +0x138  byte  是否显示建筑背后的单位剪影（1 显示 / 0 隐藏）
//     +0x139  byte  是否显示血条与维修标志（1 显示 / 0 隐藏）
//     游戏里 0xA32DE8 处 `mov eax,[0xCE08F0]; cmp byte [eax+0x139],0` 就是画血条前的判断。
//   0x00CE0D78  光照配置对象全局指针。真正的光照结构在 [[0xCE0D78]+0x24]：
//     +0x00 / +0x04 / +0x08   float 环境光 RGB
//     +0x0C / +0x10 / +0x14   float 主光源 RGB
//     +0x30 / +0x34 / +0x38   float 调色颜色 #1 RGB
//     +0x54 / +0x58 / +0x5C   float 调色颜色 #2 RGB
//     游戏里 0x61156D 处把 主光源*0.5 + 环境光 再乘 255 得到实际光色，可见分量是 0..1 的浮点。
//   0x00CEEDB4  int    阴影贴图大小。游戏 0x93BBE6 初始化成 0x400（1024），
//                      0x928A4F 的 setter 会写入可选值并在画质>=4 时下限抬到 2048。
//   0x00CEEDB8  float  最大阴影距离。游戏 0x93BBD8 从常量 1000.0 初始化。
//
// 与 CameraBridge 的零售版地址对应关系：渲染对象/光照是 +0x51A0，阴影这一组是 +0x5190
//（两处 .data 之间有 0x10 的收缩），本文件直接使用已经验证过的 Steam 地址。
//
// 安全性：所有读写都先校验指针与当前值，任何一条不满足就拒绝并写日志，绝不盲写。
// ---------------------------------------------------------------------------
struct MiscLightState {
  bool ok = false;
  float ambient[3] = {0.f, 0.f, 0.f};
  float main[3] = {0.f, 0.f, 0.f};
  float tint1[3] = {0.f, 0.f, 0.f};
  float tint2[3] = {0.f, 0.f, 0.f};
};

struct MiscState {
  bool flags_ok = false;         // 读到了渲染对象
  bool show_health = true;       // +0x139
  bool show_silhouette = true;   // +0x138
  bool shadow_size_ok = false;
  int shadow_size = 0;           // 1024 / 2048 / 4096 / 8192
  bool shadow_dist_ok = false;
  float shadow_dist = 0.f;       // 最大阴影距离
  MiscLightState light;
};

// 一次性读取杂项状态。各项各自带 ok 标记，读不到不会乱写。
void misc_read(MiscState* out);
// 由 Present 每帧调用：把用户启用的项持续写回（游戏换局/改画质会重置）。
void misc_tick();
// 安装（init 时调用一次）。
void misc_install();

// 不显示血条和维修标志 / 不显示建筑背后的单位剪影。hide=true 即隐藏。
bool misc_set_hide_health(bool hide, bool take_over, std::string* out_msg);
bool misc_set_hide_silhouette(bool hide, bool take_over, std::string* out_msg);
// 阴影贴图大小（1024/2048/4096/8192）。重新开始一局之后生效。
bool misc_set_shadow_size(int size, std::string* out_msg);
bool misc_set_shadow_distance(float dist, std::string* out_msg);
void misc_set_shadow_takeover(bool on);
// which: 0 环境光 / 1 主光源 / 2 调色 #1 / 3 调色 #2。分量是 0..1（可以更大/更小）。
bool misc_set_light(int which, const float rgb[3], std::string* out_msg);
void misc_set_light_lock(bool on);
bool misc_light_lock();
// 把 4 组光照恢复成本 DLL 第一次读到的原始值，并停用锁定。
void misc_restore_lights();
bool misc_engine_cmd(const char* key, bool* handled, std::string* out_msg);

// ---- Lua 桥（对应 CameraBridge 的 RA3Lua / RA3LuaBridge） -------------------
// 一个 Lua 值的快照。type = -1 表示没读到；否则就是 Lua 的类型标签。
struct LuaGlobal {
  int type = -1;
  double number = 0.0;
  char text[256] = {};
};

// 动画相关的 Lua 全局变量快照（对应原版插件的动画跟踪）。
struct LuaAnimInfo {
  bool valid = false;
  LuaGlobal game_object_address;      // AnimationLuaGetCurrentGameObjectAddress
  LuaGlobal game_object_id;           // AnimationLuaGetCurrentGameObjectId
  LuaGlobal will_switch_animation;    // willSwitchAnimation
  LuaGlobal will_switch_pause;        // willSwitchPause
  LuaGlobal game_time;                // gameTime
  LuaGlobal relative_time;            // relativeTime
};

// 安装 Lua API hook（init 时自动调用，可重复调用）。
void lua_bridge_install();
// 由 Present 每帧调用：重试安装、刷新动画全局变量缓存。
void lua_bridge_tick();
bool lua_bridge_ready();
uint32_t lua_bridge_state();   // 捕获到的 lua_State*（0 = 还没有）
int lua_bridge_hook_state();   // -1 = 未装好，1 = 已装好
int lua_bridge_api_hits();
const char* lua_type_name(int type);

// 读 / 写 Lua 全局变量。全部做了地址与栈校验，失败返回 false 而不是乱写。
bool lua_global_read(const char* name, LuaGlobal* out);
bool lua_global_read_text(const char* name, char* out, size_t out_len);
bool lua_global_read_number(const char* name, double* out);
bool lua_global_write_string(const char* name, const char* value);
bool lua_global_write_number(const char* name, double value);
bool lua_global_write_nil(const char* name);

// 动画探针
bool lua_anim_read(LuaAnimInfo* out);
int lua_anim_probe_count();
const char* lua_anim_probe_name(int index);
const char* lua_anim_probe_label(int index);

bool lua_bridge_engine_cmd(const char* key, bool* handled, std::string* out_msg);

// 对局时钟（秒）。返回 false 表示当前没有进行中的对局。
bool battle_clock(float* seconds_out);

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
