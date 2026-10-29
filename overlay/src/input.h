#pragma once

namespace input {

void poll();  // menu toggle + feature hotkeys
bool menu_visible();
void set_menu_visible(bool v);
void toggle_menu();

// 「禁用键盘快捷键」：开启后只在菜单里点按钮，不再响应功能热键。
// 菜单显隐热键（Home / Insert / F8）不受影响——它是唯一能重新打开菜单、
// 再把这个开关关掉的口子，跟着一起屏蔽会让用户彻底出不来。
void set_hotkeys_disabled(bool v);
bool hotkeys_disabled();

}  // namespace input
