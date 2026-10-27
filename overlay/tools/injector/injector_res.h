// Resource ids shared by the .rc template and main.cpp.
//
// Keep this file ASCII-only: rc.exe preprocesses it with the ANSI codepage and
// mis-parses multibyte comments.
//
// All payloads are embedded as RCDATA so the exe does not have to unpack
// anything into a temp directory at startup (that is what the PyInstaller
// onefile build did, and what broke).
#ifndef RA3_INJECTOR_RES_H
#define RA3_INJECTOR_RES_H

#define IDR_PAYLOAD_OVERLAY 101  // ra3_overlay_v4.dll
#define IDR_PAYLOAD_ARM 102      // arm_mustcode.dll (assembler helper, dll name on purpose)

// Explorer / 任务栏 / 文件属性里显示的 exe 图标（overlay/assets/ra3_overlay_icon.ico）
#define IDI_APP_ICON 100

// Steam launch-option screenshots for the version-mismatch guide
// (used by the guide window in step 2; embedded already so the layout can be
// exercised without touching the build again).
#define IDR_GUIDE_RETAIL_1 201   // v1.12_change_method/image_1.png
#define IDR_GUIDE_RETAIL_2 202   // v1.12_change_method/image_2.png
#define IDR_GUIDE_UPRISING_1 203 // uprising_v1.0_change_method/image_1.png
#define IDR_GUIDE_UPRISING_2 204 // uprising_v1.0_change_method/image_2.png

#endif  // RA3_INJECTOR_RES_H
