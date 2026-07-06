/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SURFACE_UWP_H_
#define XENIA_UI_SURFACE_UWP_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

#include <cstdint>
#include <unknwn.h>

#include "xenia/ui/surface.h"

namespace xe {
namespace ui {

// A presentation surface backed by the application's CoreWindow. The graphics
// provider turns this into a swap chain via IDXGIFactory2::
// CreateSwapChainForCoreWindow. We only store the raw IUnknown ABI pointer of
// the CoreWindow (owned for the lifetime of the app by CoreApplication), so
// this header pulls in no WinRT projection headers.
class UWPCoreWindowSurface final : public Surface {
 public:
  explicit UWPCoreWindowSurface(::IUnknown* core_window_abi)
      : core_window_abi_(core_window_abi) {}

  TypeIndex GetType() const override { return kTypeIndex_UWPCore; }

  ::IUnknown* core_window_abi() const { return core_window_abi_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ::IUnknown* core_window_abi_ = nullptr;
};

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT

#endif  // XENIA_UI_SURFACE_UWP_H_
