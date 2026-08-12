/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/xbox_console.h"

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

// Windows types first: gamingdeviceinformation.h declares its entry point with
// STDAPI, and psapi.h needs the base headers, neither of which the C++/WinRT
// projection brings in on its own.
#include "xenia/base/platform_win.h"

#include <gamingdeviceinformation.h>
#include <psapi.h>  // PROCESS_MEMORY_COUNTERS_EX (process commit charge).

#include <winrt/Windows.System.h>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {

namespace {

// The App partition on every Xbox generation is about a gigabyte; the Game
// partition is several. Anything above this is the Game partition, whatever
// the generation - the point is to tell the two apart, not to know the exact
// grant, which the query reports anyway.
constexpr uint64_t kGamePartitionThresholdBytes = UINT64_C(2) << 30;

XboxModel ModelFromDeviceId(GAMING_DEVICE_DEVICE_ID device_id,
                            bool& is_devkit_out) {
  is_devkit_out = false;
  switch (device_id) {
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
      return XboxModel::kXboxOne;
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
      return XboxModel::kXboxOneS;
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
      return XboxModel::kXboxOneX;
    case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
      is_devkit_out = true;
      return XboxModel::kXboxOneX;
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
      return XboxModel::kXboxSeriesS;
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
      return XboxModel::kXboxSeriesX;
    case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
      is_devkit_out = true;
      return XboxModel::kXboxSeriesX;
    default:
      return XboxModel::kUnknownXbox;
  }
}

// What the presenter should cap the swap chain to when nobody chose. The whole
// post chain (SMAA, then the scaling and sharpening effect) runs at this size
// every presented frame, so on a console that cannot afford 4K it is the
// single largest fixed GPU cost there is - and on a 2x2 resolution scale of a
// 720p title the guest output is 1440p anyway, so capping there discards
// nothing that was rendered.
uint32_t RecommendedPresentMaxHeight(XboxModel model) {
  switch (model) {
    case XboxModel::kXboxOne:
    case XboxModel::kXboxOneS:
      return 1080;
    case XboxModel::kXboxSeriesS:
      return 1440;
    default:
      // Series X, One X and anything newer: let it use the display's own
      // resolution.
      return 0;
  }
}

uint64_t QueryProcessPrivateCommitBytes() {
  // K32GetProcessMemoryInfo is exported from kernel32, which is always linked;
  // resolving it here avoids a dependency on psapi.lib, which the Xbox
  // toolchain does not link (the same trick memory.cc uses for its telemetry).
  using PfnK32GetProcessMemoryInfo =
      BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
  static auto k32_get_process_memory_info =
      reinterpret_cast<PfnK32GetProcessMemoryInfo>(GetProcAddress(
          GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
  if (!k32_get_process_memory_info) {
    return 0;
  }
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  if (!k32_get_process_memory_info(
          GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
          sizeof(pmc))) {
    return 0;
  }
  return uint64_t(pmc.PrivateUsage);
}

XboxConsoleProfile BuildProfile() {
  XboxConsoleProfile profile;

  GAMING_DEVICE_MODEL_INFORMATION info = {};
  if (SUCCEEDED(GetGamingDeviceModelInformation(&info)) &&
      info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
    profile.model = ModelFromDeviceId(info.deviceId, profile.is_devkit);
  } else {
    // A UWP build can also be run on a PC, where none of this applies.
    profile.model = XboxModel::kNotXbox;
  }

  // The platform's own accounting of what THIS process may use. This is the
  // number that says whether the package was launched as a Game (the several
  // gigabyte partition expandedResources asks for) or as an App - a
  // distinction nothing in the port could previously see, although getting it
  // wrong makes every large allocation fail.
  try {
    profile.app_memory_limit_bytes =
        winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
  } catch (...) {
    // Not available (no WinRT apartment on this thread, or an older runtime).
    // The commit charge remains the only ceiling anyone knows about, which is
    // exactly the behaviour there was before.
    profile.app_memory_limit_bytes = 0;
  }
  profile.has_game_memory_partition =
      profile.app_memory_limit_bytes >= kGamePartitionThresholdBytes;

  profile.logical_processors = xe::threading::logical_processor_count();
  profile.recommended_present_max_height =
      RecommendedPresentMaxHeight(profile.model);
  return profile;
}

}  // namespace

const char* XboxConsoleProfile::model_name() const {
  switch (model) {
    case XboxModel::kXboxOne:
      return "Xbox One";
    case XboxModel::kXboxOneS:
      return "Xbox One S";
    case XboxModel::kXboxOneX:
      return is_devkit ? "Xbox One X (devkit)" : "Xbox One X";
    case XboxModel::kXboxSeriesS:
      return "Xbox Series S";
    case XboxModel::kXboxSeriesX:
      return is_devkit ? "Xbox Series X (devkit)" : "Xbox Series X";
    case XboxModel::kUnknownXbox:
      return "an Xbox this build does not recognise";
    default:
      return "not an Xbox";
  }
}

const XboxConsoleProfile& GetXboxConsoleProfile() {
  static const XboxConsoleProfile profile = BuildProfile();
  return profile;
}

void LogXboxConsoleProfile() {
  const XboxConsoleProfile& profile = GetXboxConsoleProfile();
  if (!profile.is_xbox()) {
    XELOGI(
        "Console profile: not running on an Xbox - no console-specific "
        "defaults applied");
    return;
  }
  if (profile.app_memory_limit_bytes) {
    XELOGI(
        "Console profile: {}, {} logical processors, {} MB memory partition "
        "({})",
        profile.model_name(), profile.logical_processors,
        profile.app_memory_limit_bytes >> 20,
        profile.has_game_memory_partition ? "Game" : "App");
  } else {
    XELOGI(
        "Console profile: {}, {} logical processors, memory partition unknown "
        "(the platform did not answer)",
        profile.model_name(), profile.logical_processors);
  }
  if (profile.app_memory_limit_bytes && !profile.has_game_memory_partition) {
    XELOGE(
        "This process was given the {} MB App memory partition, not the Game "
        "one. Emulation needs several gigabytes: render targets, textures and "
        "the guest's own 512 MB will not fit, and allocations will fail from "
        "the first level onwards. Set the application type to \"Game\" in the "
        "console's Dev Home and launch it again - the toggle resets to \"App\" "
        "on every redeploy.",
        profile.app_memory_limit_bytes >> 20);
  }
}

uint64_t QueryAppMemoryHeadroomBytes() {
  const XboxConsoleProfile& profile = GetXboxConsoleProfile();
  if (!profile.app_memory_limit_bytes) {
    return UINT64_MAX;
  }
  // Deliberately NOT MemoryManager::AppMemoryUsage(): this is polled from the
  // command processor thread, which has no WinRT apartment of its own, and a
  // projected static call from there is a hazard for a number the process's
  // own private commit gives just as well.
  uint64_t private_commit = QueryProcessPrivateCommitBytes();
  if (!private_commit) {
    return UINT64_MAX;
  }
  return private_commit >= profile.app_memory_limit_bytes
             ? 0
             : profile.app_memory_limit_bytes - private_commit;
}

}  // namespace xe

#endif  // XE_PLATFORM_WINRT
