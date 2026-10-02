#pragma once

namespace input {

void poll();  // menu toggle + feature hotkeys
bool menu_visible();
void set_menu_visible(bool v);
void toggle_menu();

}  // namespace input
