#pragma once

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace unit_icons {

void init(IDirect3DDevice9* device);
void shutdown();
void set_device(IDirect3DDevice9* device);

// Resolve icon for a roster type. key = TypeId, disp = Chinese/display name.
// Returns nullptr if missing / failed.
IDirect3DTexture9* get(const char* type_key, const char* disp_name, bool is_building);

}  // namespace unit_icons
