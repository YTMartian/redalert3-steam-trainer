#pragma once

namespace d3d9_hook {

// Spawns a worker that finds D3D9 vtable and installs EndScene/Reset hooks.
bool start();
void stop();

}  // namespace d3d9_hook
