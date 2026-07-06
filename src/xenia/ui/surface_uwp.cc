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

DECLARE_int32(uwp_present_max_height);

namespace xe {
namespace ui {

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
  if (cvars::uwp_present_max_height > 0 &&
      height_out > uint32_t(cvars::uwp_present_max_height)) {
    const uint32_t capped_height = uint32_t(cvars::uwp_present_max_height);
    width_out = uint32_t(uint64_t(width_out) * capped_height / height_out);
    height_out = capped_height;
  }

  return width_out != 0 && height_out != 0;
}

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
