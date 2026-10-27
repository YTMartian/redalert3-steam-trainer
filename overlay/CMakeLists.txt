cmake_minimum_required(VERSION 3.16)
project(ra3_overlay LANGUAGES C CXX RC)

# RA3 is a 32-bit process — this DLL MUST be Win32.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ra3_overlay must be built as Win32 (x86). Use -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMGUI_PLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-plot)
set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
set(UNIT_RC ${CMAKE_CURRENT_BINARY_DIR}/unit_images.rc)
if(NOT EXISTS "${UNIT_RC}")
  message(FATAL_ERROR "Missing ${UNIT_RC}. build.bat packs unit_images into the DLL first.")
endif()
set(NAME_RC ${CMAKE_CURRENT_BINARY_DIR}/name_tables.rc)
if(NOT EXISTS "${NAME_RC}")
  message(FATAL_ERROR "Missing ${NAME_RC}. build.bat packs the unit name tables into the DLL first.")
endif()

add_library(ra3_overlay SHARED
  src/dllmain.cpp
  src/d3d9_hook.cpp
  src/input.cpp
  src/skin.cpp
  src/skin_gallery.cpp
  src/paint.cpp
  src/shell.cpp
  src/ui.cpp
  src/unit_icons.cpp
  src/game_api.cpp
  src/game_features.cpp
  src/camera.cpp
  src/time_control.cpp
  src/lua_bridge.cpp
  src/render_misc.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_PLOT_DIR}/src/imgui_plot.cpp
  src/stats_minimal.cpp
  ${MINHOOK_DIR}/src/buffer.c
  ${MINHOOK_DIR}/src/hook.c
  ${MINHOOK_DIR}/src/trampoline.c
  ${MINHOOK_DIR}/src/hde/hde32.c
  ${UNIT_RC}
  ${NAME_RC}
)

target_include_directories(ra3_overlay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${IMGUI_PLOT_DIR}/include
  ${MINHOOK_DIR}/include
)

target_compile_definitions(ra3_overlay PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(ra3_overlay PRIVATE /utf-8 /Oy-)

target_link_libraries(ra3_overlay PRIVATE
  d3d9
  user32
  gdi32
  dwmapi
  psapi
  imm32
  ole32
  windowscodecs
)

# Primary output uses a distinct name so builds succeed while the game still
# holds an older ra3_overlay.dll open. inject.py prefers *_new.dll when present.
set_target_properties(ra3_overlay PROPERTIES
  OUTPUT_NAME "ra3_overlay_v4"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin
  LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin
  LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin
  ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin
  PREFIX ""
)

# ---------------------------------------------------------------------------
# 皮肤离线预览工具（开发期用）。
# 独立 Win32 + D3D9 窗口，只链接 skin / paint 层，不依赖游戏与 game_api，
# 用来在没有 RA3 对局的情况下核对配色与控件观感。
# EXCLUDE_FROM_ALL：默认编译（build.bat / build_injector.bat）不再产出
# bin\preview，发布目录里不会多出一个开发工具。需要时单独指定目标：
#   cmake --build build --config Release --target ra3_skin_preview
# ---------------------------------------------------------------------------
add_executable(ra3_skin_preview WIN32 EXCLUDE_FROM_ALL
  tools/skin_preview/skin_preview.cpp
  src/skin.cpp
  src/skin_gallery.cpp
  src/paint.cpp
  src/shell.cpp
  src/stats_minimal.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(ra3_skin_preview PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)

target_compile_definitions(ra3_skin_preview PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(ra3_skin_preview PRIVATE /utf-8)
target_link_libraries(ra3_skin_preview PRIVATE d3d9 user32 gdi32 dwmapi imm32)

set_target_properties(ra3_skin_preview PROPERTIES
  OUTPUT_NAME "ra3_skin_preview"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin/preview
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin/preview
)

# ---------------------------------------------------------------------------
# 原生注入器（overlay\bin\ra3_injector.exe）。
#
# 目的：把 PyInstaller onefile 那套"启动时解包到 %TEMP%\_MEIxxxxxx"换掉。
# 载荷（ra3_overlay_v4.dll / arm_mustcode.dll / 四张引导图）在配置时编成
# RCDATA 塞进 exe，运行时只把两个 DLL 写到 exe 旁边，不碰任何临时目录。
#
# EXCLUDE_FROM_ALL：默认编译（build.bat / build_injector.bat）行为完全不变，
# 这个 exe 只在需要时单独编：
#   cmake --build build --config Release --target ra3_injector
# 或者直接 overlay\build_injector_native.bat。
#
# 载荷不在（干净 clone / 还没编过 DLL）时不注册这个 target，避免把配置步骤
# 变成硬依赖；编过 DLL 之后再配置一次就会出现。
# ---------------------------------------------------------------------------
set(INJECTOR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tools/injector)
set(INJECTOR_PAYLOAD_OVERLAY ${CMAKE_CURRENT_SOURCE_DIR}/bin/ra3_overlay_v4.dll)
set(INJECTOR_PAYLOAD_ARM ${CMAKE_CURRENT_SOURCE_DIR}/bin/arm_mustcode.dll)
set(INJECTOR_PNG_RETAIL_1 ${CMAKE_CURRENT_SOURCE_DIR}/../v1.12_change_method/image_1.png)
set(INJECTOR_PNG_RETAIL_2 ${CMAKE_CURRENT_SOURCE_DIR}/../v1.12_change_method/image_2.png)
set(INJECTOR_PNG_UPRISING_1 ${CMAKE_CURRENT_SOURCE_DIR}/../uprising_v1.0_change_method/image_1.png)
set(INJECTOR_PNG_UPRISING_2 ${CMAKE_CURRENT_SOURCE_DIR}/../uprising_v1.0_change_method/image_2.png)

set(INJECTOR_PAYLOADS
  ${INJECTOR_PAYLOAD_OVERLAY}
  ${INJECTOR_PAYLOAD_ARM}
  ${INJECTOR_PNG_RETAIL_1}
  ${INJECTOR_PNG_RETAIL_2}
  ${INJECTOR_PNG_UPRISING_1}
  ${INJECTOR_PNG_UPRISING_2}
)
set(INJECTOR_PAYLOADS_OK TRUE)
foreach(_payload ${INJECTOR_PAYLOADS})
  if(NOT EXISTS "${_payload}")
    set(INJECTOR_PAYLOADS_OK FALSE)
  endif()
endforeach()

if(INJECTOR_PAYLOADS_OK)
  file(TO_CMAKE_PATH "${INJECTOR_PAYLOAD_OVERLAY}" OVERLAY_DLL)
  file(TO_CMAKE_PATH "${INJECTOR_PAYLOAD_ARM}" ARM_DLL)
  file(TO_CMAKE_PATH "${INJECTOR_PNG_RETAIL_1}" PNG_RETAIL_1)
  file(TO_CMAKE_PATH "${INJECTOR_PNG_RETAIL_2}" PNG_RETAIL_2)
  file(TO_CMAKE_PATH "${INJECTOR_PNG_UPRISING_1}" PNG_UPRISING_1)
  file(TO_CMAKE_PATH "${INJECTOR_PNG_UPRISING_2}" PNG_UPRISING_2)
  configure_file(
    ${INJECTOR_SRC}/injector.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/injector_payload.rc
    @ONLY
  )

  add_executable(ra3_injector WIN32 EXCLUDE_FROM_ALL
    ${INJECTOR_SRC}/main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/injector_payload.rc
  )

  target_include_directories(ra3_injector PRIVATE ${INJECTOR_SRC})

  target_compile_definitions(ra3_injector PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
  )

  target_compile_options(ra3_injector PRIVATE /utf-8)
  target_link_libraries(ra3_injector PRIVATE user32 bcrypt shell32 advapi32)

  # 默认请求管理员权限：普通权限启动时，这台机器会把进程放进低完整性 / 受限令牌，
  # 进程列表被过滤，看不到游戏。写进 manifest 后双击就弹 UAC，点"是"即可，
  # 不用再右键"以管理员身份运行"（旧 PyInstaller 版也是 --uac-admin）。
  target_link_options(ra3_injector PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

  # 载荷换了内容要重新嵌进 exe，否则注入器会一直带着旧 DLL。
  set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/injector_payload.rc APPEND
    PROPERTY OBJECT_DEPENDS "${INJECTOR_PAYLOAD_OVERLAY};${INJECTOR_PAYLOAD_ARM}")

  set_target_properties(ra3_injector PROPERTIES
    OUTPUT_NAME "ra3_injector"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin
  )
else()
  message(STATUS "ra3_injector skipped: overlay/bin/*.dll not built yet")
endif()

