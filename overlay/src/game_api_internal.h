#pragma once

#include <cstdint>
#include <string>

namespace game_api {

uint8_t* flags_base();
uint8_t* mc_base();
uint8_t* idb_base();
uint32_t module_base();
bool hook_is_installed(const char* name);

bool engine_run(const char* key, std::string* out_msg);

}  // namespace game_api
