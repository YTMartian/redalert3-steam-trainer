#pragma once

struct IDirect3DDevice9;

namespace ui {

bool init(IDirect3DDevice9* device, void* hwnd);
void shutdown();
void begin_frame();
void draw();
void end_frame(IDirect3DDevice9* device);
bool want_capture_mouse();
bool want_capture_keyboard();

}  // namespace ui
