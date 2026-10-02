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

void beep(const char* kind);  // on / off / click / error

bool ready();
bool hooks_armed();
bool is_spectator();
const char* mode_label();

// Allocate MustCode + arm hooks (trainer.build) in one step.
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

bool toggle_feature(const char* key, bool enabled, std::string* out_msg);
bool pulse_feature(const char* key, std::string* out_msg);
bool set_danger(int level, std::string* out_msg);
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
const char* hotkey_hint(const char* key);  // e.g. "Ctrl+F1", may be null

}  // namespace game_api
