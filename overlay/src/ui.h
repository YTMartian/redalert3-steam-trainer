#pragma once

struct IDirect3DDevice9;

namespace ui {

bool init(IDirect3DDevice9* device, void* hwnd);
void shutdown();
void begin_frame();
void draw();
// 恢复 overlay_ui.ini 里保存的杂项设置（需要杂项模块初始化完成；没完成就下一帧再试）。
void pump_pending_settings();
void end_frame(IDirect3DDevice9* device);
bool overlay_visible();  // main menu, or pinned stats window
bool stats_roster_wanted();  // 战况统计 open/pinned — drives roster scan cost
bool want_capture_mouse();
bool want_capture_keyboard();

}  // namespace ui
