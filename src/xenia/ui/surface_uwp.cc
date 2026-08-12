/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/surface_uwp.h"

#if XE_PLATFORM_WINRT

#include <winrt/Windows.Graphics.Display.Core.h>

#include <gamingdeviceinformation.h>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/xbox_console.h"

DECLARE_int32(uwp_present_max_height);

namespace xe {
namespace ui {

uint32_t GetUWPPresentMaxHeight() {
  // Resolved once. The console profile does not change during a run, and a cap
  // that changed between the surface and the window would be worse than either
  // value on its own.
  static const uint32_t height = []() -> uint32_t {
    int32_t configured = cvars::uwp_present_max_height;
    if (configured > 0) {
      return uint32_t(configured);
    }
    if (configured == 0) {
      // An explicit "never cap" - the display's own resolution.
      return 0;
    }
    // Automatic. The whole post chain (SMAA, then scaling and sharpening) runs
    // at this size on every presented frame, so on a console that cannot
    // afford 4K it is the largest fixed GPU cost in the session - and at a 2x2
    // resolution scale of a 720p title the guest output is 1440p anyway, so a
    // 1440 cap discards nothing that was rendered.
    const XboxConsoleProfile& profile = GetXboxConsoleProfile();
    uint32_t recommended = profile.recommended_present_max_height;
    if (recommended) {
      XELOGI(
          "Presentation: capping the swap chain to {} rows for {} "
          "(uwp_present_max_height is automatic; set it to 0 for the display's "
          "own resolution, or to a height of your own)",
          recommended, profile.model_name());
    }
    return recommended;
  }();
  return height;
}

bool UWPCoreWindowSurface::GetSizeImpl(uint32_t& width_out,
                                       uint32_t& height_out) const {
  // Default to 1080p; on an actual Xbox the HDMI mode is authoritative (the
  // CoreWindow logical bounds are normalized and not in physical pixels).
  width_out = 1920;
  height_out = 1080;

  GAMING_DEVICE_MODEL_INFORMATION info = {};
  GetGamingDeviceModelInformation(&info);
  if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
    auto hdmi = winrt::Windows::Graphics::Display::Core::
        HdmiDisplayInformation::GetForCurrentView();
    if (hdmi) {
      if (auto mode = hdmi.GetCurrentDisplayMode()) {
        width_out = mode.ResolutionWidthInRawPixels();
        height_out = mode.ResolutionHeightInRawPixels();
      }
    }
  }

  // This size is what the presenter creates the swap chain with, so the cap
  // must be applied here (the window size alone doesn't affect the swap chain).
  const uint32_t max_height = GetUWPPresentMaxHeight();
  if (max_height && height_out > max_height) {
    width_out = uint32_t(uint64_t(width_out) * max_height / height_out);
    height_out = max_height;
  }

  return width_out != 0 && height_out != 0;
}

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
