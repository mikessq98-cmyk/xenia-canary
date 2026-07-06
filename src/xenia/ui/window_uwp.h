/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOW_UWP_H_
#define XENIA_UI_WINDOW_UWP_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.UI.Core.h>

#include "xenia/ui/window.h"

namespace xe {
namespace ui {

// CoreWindow-backed window. There is exactly one CoreWindow per app view on
// UWP, so this is effectively a singleton wrapper: sizing comes from the HDMI
// display mode (or a configurable cap), and painting is driven by the presenter
// requesting repaints, pumped from the IFrameworkView Run loop.
class UWPWindow final : public Window {
 public:
  UWPWindow(WindowedAppContext& app_context, std::string_view title,
            uint32_t width, uint32_t height);
  ~UWPWindow() override;

  uint32_t GetMediumDpi() const override { return 96; }

 protected:
  uint32_t GetLatestDpiImpl() const override;

  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags allowed_types) override;

  bool OpenImpl() override;
  void RequestCloseImpl() override;
  void RequestPaintImpl() override;
  // Mouse capture is implicit on CoreWindow; focus just forces a repaint.
  void FocusImpl() override;

 private:
  // Subscribes CoreWindow keyboard/pointer events and routes them to the
  // cross-platform Window event handlers (OnKeyChar/OnKeyDown/OnMouse*).
  void WireCoreWindowInput();
  // Shows/hides the system on-screen keyboard (InputPane) to match ImGui's
  // current text-input need - this is the "pop-up keyboard" on Xbox.
  void UpdateOnScreenKeyboard();
  // Force-opens the system keyboard when the View button is held (fallback).
  void CheckKeyboardHoldGesture();
  // CoreWindow has no automatic WM_PAINT, so a periodic timer drives continuous
  // repaints of the UI (menu/ImGui) - without this the UI never redraws (black
  // screen at the menu when no game is presenting guest output).
  void StartPaintLoop();

  winrt::Windows::UI::Core::CoreWindow core_window_{nullptr};
  winrt::Windows::System::Threading::ThreadPoolTimer paint_timer_{nullptr};
  // Coalesces repaint requests (which may arrive from the GPU refresh thread)
  // so only one OnPaint is marshaled to the UI thread at a time, mirroring how
  // Win32 InvalidateRect collapses into a single WM_PAINT.
  std::atomic<bool> paint_pending_{false};
  bool input_wired_ = false;
  bool keyboard_visible_ = false;
  // Manual on-screen keyboard trigger: holding the gamepad View button (the
  // "two windows" button) for ~2 seconds force-opens the system keyboard, as a
  // fallback for dialogs where ImGui's WantTextInput isn't picked up.
  std::atomic<int64_t> view_hold_start_ms_{0};
  bool view_hold_fired_ = false;
};

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT

#endif  // XENIA_UI_WINDOW_UWP_H_
