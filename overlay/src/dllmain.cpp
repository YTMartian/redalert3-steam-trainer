#include "d3d9_hook.h"
#include "game_api.h"

#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(module);
      game_api::init();
      game_api::set_status("DLL loaded");
      if (!d3d9_hook::start()) {
        game_api::set_status("failed to start hook thread");
      }
      break;
    case DLL_PROCESS_DETACH:
      d3d9_hook::stop();
      game_api::shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
