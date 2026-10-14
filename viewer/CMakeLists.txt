cmake_minimum_required(VERSION 3.24)

project(LookingGlassWindowsLoopback LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lgwl_display_inventory
  src/display_inventory.cpp
)

add_executable(lgwl_viewer WIN32
  src/viewer.cpp
  ../app/window_picker.cpp
  ../app/hotkeys.cpp
  ../app/dialog_follower.cpp
)
target_include_directories(lgwl_viewer PRIVATE ../app)

set(SHAREDVD_RESOURCES "" CACHE PATH "VKomado branding resources (optional)")
if(SHAREDVD_RESOURCES)
  enable_language(RC)
  set(SHAREDVD_ICON "${SHAREDVD_RESOURCES}/sharedvd.ico")
  configure_file("${SHAREDVD_RESOURCES}/sharedvd.rc.in" sharedvd.rc @ONLY)
  target_sources(lgwl_viewer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sharedvd.rc")
  target_compile_definitions(lgwl_viewer PRIVATE SHAREDVD_ICON_AVAILABLE)
endif()

target_compile_definitions(lgwl_display_inventory PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
)

target_compile_definitions(lgwl_viewer PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
)

if(MSVC)
  # Keep the portable viewer independent of the VC++ Redistributable installer.
  set_property(TARGET lgwl_viewer PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(lgwl_display_inventory PRIVATE
    /W4
    /permissive-
    /EHsc
    /utf-8
  )
  target_compile_options(lgwl_viewer PRIVATE
    /W4
    /permissive-
    /EHsc
    /utf-8
  )
endif()

target_link_libraries(lgwl_display_inventory PRIVATE
  d3d11
  d3d12
  dxgi
  user32
)

target_link_libraries(lgwl_viewer PRIVATE
  d3d11
  d3dcompiler
  dxgi
  runtimeobject
  shell32
  ole32
  gdi32
  dwmapi
  user32
  windowsapp
  comctl32
)
