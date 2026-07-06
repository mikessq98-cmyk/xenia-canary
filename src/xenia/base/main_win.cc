/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/main_win.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

#include "version.h"

// For RequestWin32MMCSS.
#include <dwmapi.h>
// For RequestWin32HighResolutionTimer.
#include <winternl.h>

DEFINE_bool(win32_high_resolution_timer, true,
            "Requests high-resolution timer from the NT kernel", "Win32");
DEFINE_bool(
    win32_mmcss, true,
    "Opt in the Multimedia Class Scheduler Service (MMCSS) scheduling for "
    "prioritized access to CPU resources",
    "Win32");

namespace xe {

static void RequestWin32HighResolutionTimer() {
#if XE_PLATFORM_WINRT
  // Timer-resolution tuning via ntdll is not available to the App Container.
  return;
#else
  HMODULE ntdll_module = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll_module) {
    return;
  }

  // clang-format off
  NTSTATUS (NTAPI* nt_query_timer_resolution)(OUT PULONG MinimumResolution,
                                              OUT PULONG MaximumResolution,
                                              OUT PULONG CurrentResolution);
  NTSTATUS (NTAPI* nt_set_timer_resolution)(IN ULONG DesiredResolution,
                                            IN BOOLEAN SetResolution,
                                            OUT PULONG CurrentResolution);
  // clang-format on
  nt_query_timer_resolution =
      reinterpret_cast<decltype(nt_query_timer_resolution)>(
          GetProcAddress(ntdll_module, "NtQueryTimerResolution"));
  nt_set_timer_resolution = reinterpret_cast<decltype(nt_set_timer_resolution)>(
      GetProcAddress(ntdll_module, "NtSetTimerResolution"));
  if (!nt_query_timer_resolution || !nt_set_timer_resolution) {
    return;
  }

  ULONG minimum_resolution, maximum_resolution, current_resolution;
  nt_query_timer_resolution(&minimum_resolution, &maximum_resolution,
                            &current_resolution);
  nt_set_timer_resolution(maximum_resolution, true, &current_resolution);
#endif  // XE_PLATFORM_WINRT
}

static void RequestWin32MMCSS() {
#if XE_PLATFORM_WINRT
  // dwmapi.dll (DwmEnableMMCSS) is not available in the App Container.
  return;
#else
  HMODULE dwmapi_module = LoadLibraryW(L"dwmapi.dll");
  if (!dwmapi_module) {
    return;
  }
  // clang-format off
  HRESULT (STDAPICALLTYPE* dwm_enable_mmcss)(BOOL fEnableMMCSS);
  // clang-format on
  dwm_enable_mmcss = reinterpret_cast<decltype(dwm_enable_mmcss)>(
      GetProcAddress(dwmapi_module, "DwmEnableMMCSS"));
  if (dwm_enable_mmcss) {
    dwm_enable_mmcss(true);
  }
  FreeLibrary(dwmapi_module);
#endif  // XE_PLATFORM_WINRT
}

bool ParseWin32LaunchArguments(
    bool transparent_options, const std::string_view positional_usage,
    const std::vector<std::string>& positional_options,
    std::vector<std::string>* args_out) {
#if XE_PLATFORM_WINRT
  // CommandLineToArgvW (shellapi) is desktop-only, and the App Container has no
  // conventional command line. Initialize cvars from config with no positional
  // args (program name only). Activation args are handled by the entry point.
  if (!transparent_options) {
    char program_name[] = "xenia";
    char* argv_storage[1] = {program_name};
    int argc = 1;
    char** argv = argv_storage;
    cvar::ParseLaunchArguments(argc, argv, positional_usage,
                               positional_options);
  }
  if (args_out) {
    args_out->clear();
  }
  return true;
#else
  auto command_line = GetCommandLineW();

  int wargc;
  wchar_t** wargv = CommandLineToArgvW(command_line, &wargc);
  if (!wargv) {
    return false;
  }

  // Convert all args to narrow, as cxxopts doesn't support wchar.
  int argc = wargc;
  char** argv = reinterpret_cast<char**>(alloca(sizeof(char*) * argc));
  for (int n = 0; n < argc; n++) {
    size_t len = std::wcstombs(nullptr, wargv[n], 0);
    argv[n] = reinterpret_cast<char*>(alloca(sizeof(char) * (len + 1)));
    std::wcstombs(argv[n], wargv[n], len + 1);
  }

  LocalFree(wargv);

  if (!transparent_options) {
    cvar::ParseLaunchArguments(argc, argv, positional_usage,
                               positional_options);
  }

  if (args_out) {
    args_out->clear();
    for (int n = 0; n < argc; n++) {
      args_out->push_back(std::string(argv[n]));
    }
  }

  return true;
#endif  // XE_PLATFORM_WINRT
}

int InitializeWin32App(const std::string_view app_name) {
  // Initialize logging. Needs parsed FLAGS.
  xe::InitializeLogging(app_name);

  // Print version info.
  XELOGI(
      "Build: "
#ifdef XE_BUILD_IS_PR
      "PR#" XE_BUILD_PR_NUMBER " - "
#endif
      XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE);

#if XE_PLATFORM_WINRT
  // Make the memory profile visible in every log: on Xbox an "App" gets ~1GB
  // and a "Game" ~5GB - most memory-related failures (err 8/1455) trace back
  // to accidentally running under the App profile.
  {
    MEMORYSTATUSEX mem_status = {sizeof(mem_status)};
    if (GlobalMemoryStatusEx(&mem_status)) {
      XELOGI("Memory profile: {} MB total, {} MB available{}",
             mem_status.ullTotalPhys >> 20, mem_status.ullAvailPhys >> 20,
             (mem_status.ullTotalPhys >> 20) < 2048
                 ? " - APP PROFILE (~1GB)! The Game profile (~5GB) is required"
                 : "");
    }
  }
#endif  // XE_PLATFORM_WINRT

  // Request high-performance timing and scheduling.
  if (cvars::win32_high_resolution_timer) {
    RequestWin32HighResolutionTimer();
  }
  if (cvars::win32_mmcss) {
    RequestWin32MMCSS();
  }

  return 0;
}

void ShutdownWin32App() { xe::ShutdownLogging(); }

}  // namespace xe
