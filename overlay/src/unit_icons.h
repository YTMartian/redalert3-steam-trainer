#pragma once

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace unit_icons {

void init(IDirect3DDevice9* device);
void shutdown();
void set_device(IDirect3DDevice9* device);
// Call once per Present before drawing — resets the PNG decode budget.
void begin_frame();

// Resolve icon for a roster type. key = TypeId, disp = Chinese/display name.
// Returns nullptr if missing / failed / decode deferred this frame.
IDirect3DTexture9* get(const char* type_key, const char* disp_name, bool is_building);

}  // namespace unit_icons
