#include "input.h"
#include "game_api.h"

#include <Windows.h>

#include <string>

namespace input {
namespace {

bool g_show = true;
bool g_toggle_was_down = false;

struct HotkeyDef {
  const char* key;
  int vk;
  bool need_ctrl;
};

const HotkeyDef kHotkeys[] = {
    {"money", 0x70, true},
    {"power", 0x71, true},
    {"scpoint", 0x72, true},
    {"haveallsc", 0x73, true},
    {"fastbuild", 0x74, true},
    {"superpower", 0x75, true},
    {"disableallsp", 0x76, true},
    {"map", 0x78, true},
    {"nocbuild", 0x79, true},
    {"ammo", 0xBA, false},
    {"oremine", 0xDE, false},
    {"danger_max", 0xBC, false},
    {"danger_min", 0xBE, false},
    {"danger_norm", 0xBF, false},
    {"speed_max", 0xBD, false},
    {"speed_slow", 0xBB, false},
    {"speed_freeze", 0x21, false},
    {"speed_restore", 0x22, false},
    {"hp_max", 0xDB, false},
    {"hp_min", 0xDD, false},
    {"hp_normal", 0xDC, false},
    {"unit_rank", 0x50, false},
    {"unit_kill", 0x2E, false},
    {"unit_clone", 0x49, false},
};

bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool any_menu_toggle_down() {
  return key_down(VK_HOME) || key_down(VK_INSERT) || key_down(VK_F8);
}

void poll_feature_hotkeys() {
  static bool pressed[64] = {};
  const bool ctrl = key_down(VK_CONTROL);
  const int n = (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0]));
  for (int i = 0; i < n; ++i) {
    const auto& hk = kHotkeys[i];
    const bool down = key_down(hk.vk);
    const bool was = pressed[i];
    if (down && !was && (hk.need_ctrl == ctrl)) {
      std::string msg;
      game_api::trigger_hotkey(hk.key, &msg);
      if (!msg.empty()) game_api::set_status(msg.c_str());
    }
    pressed[i] = down;
  }
}

}  // namespace

void poll() {
  game_api::refresh_selection_cache();

  const bool down = any_menu_toggle_down();
  if (down && !g_toggle_was_down) {
    g_show = !g_show;
  }
  g_toggle_was_down = down;

  poll_feature_hotkeys();
}

bool menu_visible() { return g_show; }

void set_menu_visible(bool v) { g_show = v; }

void toggle_menu() { g_show = !g_show; }

}  // namespace input
