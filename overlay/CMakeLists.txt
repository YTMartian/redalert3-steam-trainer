cmake_minimum_required(VERSION 3.16)
project(ra3_overlay LANGUAGES C CXX)

# RA3 is a 32-bit process — this DLL MUST be Win32.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ra3_overlay must be built as Win32 (x86). Use -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)

add_library(ra3_overlay SHARED
  src/dllmain.cpp
  src/d3d9_hook.cpp
  src/input.cpp
  src/ui.cpp
  src/game_api.cpp
  src/game_features.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${MINHOOK_DIR}/src/buffer.c
  ${MINHOOK_DIR}/src/hook.c
  ${MINHOOK_DIR}/src/trampoline.c
  ${MINHOOK_DIR}/src/hde/hde32.c
)

target_include_directories(ra3_overlay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${MINHOOK_DIR}/include
)

target_compile_definitions(ra3_overlay PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(ra3_overlay PRIVATE /utf-8)

target_link_libraries(ra3_overlay PRIVATE
  d3d9
  user32
  gdi32
  dwmapi
  psapi
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
