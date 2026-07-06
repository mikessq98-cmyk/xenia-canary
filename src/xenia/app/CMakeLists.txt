# src/xenia/app/CMakeLists.txt - Main xenia_canary executable.

if(WIN32)
  add_executable(xenia-app WIN32)
else()
  add_executable(xenia-app)
endif()

set_target_properties(xenia-app PROPERTIES OUTPUT_NAME "xenia_canary")

# Main source
target_sources(xenia-app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main.cc
)

# Other app sources (emulator_window, profile_dialogs, etc.)
xe_platform_sources(xenia-app ${CMAKE_CURRENT_SOURCE_DIR})

# Platform-specific entry points
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  # UWP / Xbox: CoreApplication IFrameworkView entry + the package manifest.
  # No win32 .rc / app_win32.manifest (UWP uses Package.appxmanifest), and no
  # main_init_win.cc (CRT float/console setup is desktop-only).
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_uwp.cc
    ${PROJECT_SOURCE_DIR}/package/Package.appxmanifest
  )
  set_property(SOURCE ${PROJECT_SOURCE_DIR}/package/Package.appxmanifest
               PROPERTY VS_DEPLOYMENT_CONTENT 1)
  # Tile/splash images referenced by the manifest (APPX0703 if missing from
  # the payload). Deployed to Assets\ in the package, matching the manifest.
  file(GLOB uwp_asset_images ${PROJECT_SOURCE_DIR}/package/Assets/*.png)
  target_sources(xenia-app PRIVATE ${uwp_asset_images})
  set_property(SOURCE ${uwp_asset_images} PROPERTY VS_DEPLOYMENT_CONTENT 1)
  set_property(SOURCE ${uwp_asset_images} PROPERTY VS_DEPLOYMENT_LOCATION
               "Assets")
  set_target_properties(xenia-app PROPERTIES
    VS_GLOBAL_AppContainerApplication true
    VS_GLOBAL_ApplicationType "Windows Store"
    VS_GLOBAL_DefaultLanguage "en-US"
    # Sign the package with the MNKE publisher cert (matches Identity
    # Publisher="CN=MNKE" in package/Package.appxmanifest).
    VS_GLOBAL_AppxPackageSigningEnabled true
    VS_GLOBAL_PackageCertificateKeyFile "${PROJECT_SOURCE_DIR}/package/MNKE.pfx"
    VS_GLOBAL_PackageCertificatePassword "xenia"
  )
elseif(WIN32)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/main_resources.rc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/app_win32.manifest
  )
  if(XE_TARGET_X86_64)
    # main_init_win.cc needs SSE2 only (not AVX)
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
      PROPERTIES COMPILE_OPTIONS "/arch:SSE2"
    )
  endif()
else()
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_posix.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_posix.cc
  )
  if(NOT MSVC AND XE_TARGET_X86_64)
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_posix.cc
      PROPERTIES COMPILE_OPTIONS "-msse2;-mno-avx"
    )
  endif()
endif()

target_compile_definitions(xenia-app PRIVATE
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
)

# Core libraries (all platforms)
target_link_libraries(xenia-app PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-vfs
  # Third-party
  aes_128
  capstone
  fmt
  dxbc
  # discord-rpc is pulled in transitively by xenia-app-discord (and only on
  # non-WindowsStore, where that target links it); not listed here so the UWP
  # build, which excludes discord-rpc, links cleanly.
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
)

# Vulkan/SPIR-V backends (not macOS, not UWP - no Vulkan loader in the App
# Container; UWP is D3D12-only). Guarded by TARGET existence so the link list
# can never get out of sync with the add_subdirectory() exclusions.
if(TARGET xenia-ui-vulkan)
  target_link_libraries(xenia-app PRIVATE
    xenia-gpu-vulkan
    xenia-ui-vulkan
    glslang-spirv
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  # SDL (audio/hid/helper) is not built for UWP. Discord is linked for its
  # stub symbols only (discord_presence.cc compiles to no-ops under WINRT and
  # does not pull in discord-rpc).
  target_link_libraries(xenia-app PRIVATE
    xenia-app-discord
    xenia-debug-ui
  )
else()
  # Non-Android specific (always true for desktop Windows/Linux)
  target_link_libraries(xenia-app PRIVATE
    xenia-app-discord
    xenia-apu-sdl
    xenia-debug-ui
    xenia-helper-sdl
    xenia-hid-sdl
  )
endif()

# Architecture-specific backend
if(XE_TARGET_X86_64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-a64)
endif()

# Platform-specific libraries
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  # UWP / Xbox: D3D12 + XAudio2 + XInput(UAP); no winkey. Link the in-box import
  # libs directly (App Container can't LoadLibrary these).
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-xaudio2
    xenia-gpu-d3d12
    xenia-hid-xinput
    xenia-ui-d3d12
    d3d12
    dxgi
    xaudio2
    xinputuap
    WindowsApp
  )
elseif(WIN32)
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-xaudio2
    xenia-gpu-d3d12
    xenia-hid-winkey
    xenia-hid-xinput
    xenia-ui-d3d12
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-alsa
    X11
    xcb
    X11-xcb
    SDL2
  )
endif()

xe_target_defaults(xenia-app)
