#include "input.h"
#include "game_api.h"

#include <Windows.h>

#include <string>

namespace input {
namespace {

bool g_show = true;
bool g_toggle_was_down = false;

bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool any_menu_toggle_down() {
  return key_down(VK_HOME) || key_down(VK_INSERT) || key_down(VK_F8);
}

void poll_feature_hotkeys() {
  static bool pressed[64] = {};
  if (game_api::hotkey_capture_active()) return;
  const bool ctrl = key_down(VK_CONTROL);
  const bool alt = key_down(VK_MENU);
  const bool shift = key_down(VK_SHIFT);
  const int n = game_api::hotkey_slot_count();
  for (int i = 0; i < n && i < 64; ++i) {
    int vk = 0;
    bool want_ctrl = false, want_alt = false, want_shift = false;
    game_api::hotkey_slot_mods(i, &vk, &want_ctrl, &want_alt, &want_shift);
    if (vk <= 0) {
      pressed[i] = false;
      continue;
    }
    const bool down = key_down(vk);
    const bool was = pressed[i];
    if (down && !was && ctrl == want_ctrl && alt == want_alt && shift == want_shift) {
      std::string msg;
      game_api::trigger_hotkey(game_api::hotkey_slot_id(i), &msg);
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
