#include "d3d9_hook.h"
#include "game_api.h"

#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
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
      // reserved != nullptr：进程正在退出，其它线程已经被系统随机终止。
      // 这时候在 loader lock 里等线程（stop() 最多等 5 秒）、卸载 MinHook、
      // 删界面资源，都是「对着已经死掉的线程/已释放的 D3D 设备干活」——
      // 表现就是 WER 里 `ra3_overlay_v4.dll_unloaded` + c0000005 的退出崩溃。
      // 进程退出时什么都不用做，系统会回收；只有 FreeLibrary 显式卸载才做清理。
      if (reserved != nullptr) {
        break;
      }
      d3d9_hook::stop();
      game_api::shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
