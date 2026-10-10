#pragma once

#include <cstdint>
#include <string>

namespace game_api {

uint8_t* flags_base();
uint8_t* mc_base();
uint8_t* idb_base();
uint32_t module_base();
// g_module even on Uprising. module_base() stays 0 there so retail walks do not run.
uint32_t loaded_module();
bool uprising_active();
bool hook_is_installed(const char* name);

// Re-derives the derived power flags (0x0D / 0x0E) from the toggle flags so a
// restored or externally written toggle always takes effect.
void sync_power_flags_from_toggles();

bool engine_run(const char* key, std::string* out_msg);

}  // namespace game_api
