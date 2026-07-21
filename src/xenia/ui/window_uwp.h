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

  // Explicit keyboard control (called from dialog draw code on the UI thread).
  // Mirrors the proven old-port behavior: the dialog calls Show every frame
  // while its text field should be editable (naturally retrying transient
  // CoreInputView refusals), and Hide when it's done. Takes an "explicit hold"
  // that suspends the automatic WantTextInput-driven show/hide.
  void ShowOnScreenKeyboard() override;
  void HideOnScreenKeyboard() override;

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
  // Subscribes to the system's own on-screen keyboard show/hide events so
  // keyboard_visible_ reflects reality rather than only what was requested.
  void WireKeyboardVisibilityTracking();
  // Queues the show/hide as a high-priority UI work item (see
  // UWPWindowedAppContext::CallInUIThreadAtHighPriority) and rate-limits the
  // retries; safe to call every frame from any thread that draws.
  void RequestOnScreenKeyboardState(bool show);
  // The actual CoreInputView call - UI thread only.
  void ApplyOnScreenKeyboardState(bool show);
  // Force-opens the system keyboard when the View button is held (fallback).
  void CheckKeyboardHoldGesture();
  // CoreWindow has no automatic WM_PAINT. A periodic timer drives repaints of
  // the UI, but ON DEMAND: it only requests a paint when there's actually
  // something to redraw (fresh input, a short tail after it, an active text
  // field, or the View-hold gesture). A static menu therefore idles the GPU
  // instead of re-rendering a full-screen 4K frame every vsync. Guest output
  // and dialogs/notifications drive their own repaints through the presenter.
  void StartPaintLoop();
  // Polls the gamepads on the timer thread (independent of painting, because
  // ImGui's gamepad nav is otherwise only sampled inside a paint) and reports
  // whether any pad's input changed since the last tick, via XInput's
  // dwPacketNumber. This is what lets the idle menu wake on gamepad input.
  bool PollGamepadActivity();

 public:
  // Marks that user input arrived (called from the CoreWindow input handlers on
  // the UI thread); makes the paint-driver timer wake the UI for a short burst.
  void NoteInputActivity() {
    input_activity_.store(true, std::memory_order_release);
  }

 private:
  winrt::Windows::UI::Core::CoreWindow core_window_{nullptr};
  winrt::Windows::System::Threading::ThreadPoolTimer paint_timer_{nullptr};
  // While true, a dialog manages the keyboard explicitly (ShowOnScreenKeyboard
  // was called and HideOnScreenKeyboard hasn't been yet) - the automatic
  // WantTextInput path must not interfere. Set from wherever the dialogs are
  // drawn, read on the UI thread.
  std::atomic<bool> explicit_keyboard_hold_{false};
  // Set by input handlers, consumed by the paint-driver timer.
  std::atomic<bool> input_activity_{false};
  // The following are touched only on the timer thread.
  // Number of upcoming ticks to keep painting after the last input (lets ImGui
  // nav/hover animations settle before the menu goes idle again).
  uint32_t paint_tail_ticks_ = 0;
  // Ticks since the last frame requested while waiting for the system to bring
  // the on-screen keyboard up, and how many to skip between those frames.
  uint32_t keyboard_wait_ticks_ = 0;
  static constexpr uint32_t kPaintTicksPerKeyboardWait = 4;  // ~64 ms.
  // Last meaningful-input signature (buttons + deadzoned stick/trigger
  // directions, NOT the raw packet number, which ticks on analog jitter and
  // would keep the menu awake forever) and connection state per user;
  // disconnected pads are only re-probed occasionally to avoid the well-known
  // slow-XInputGetState-on-empty-slot cost.
  uint32_t gamepad_last_sig_[4] = {0, 0, 0, 0};
  bool gamepad_connected_[4] = {false, false, false, false};
  uint32_t gamepad_rescan_counter_ = 0;
  // Coalesces repaint requests (which may arrive from the GPU refresh thread)
  // so only one OnPaint is marshaled to the UI thread at a time, mirroring how
  // Win32 InvalidateRect collapses into a single WM_PAINT.
  std::atomic<bool> paint_pending_{false};
  bool input_wired_ = false;
  // Written on the UI thread (system visibility events / the apply call), read
  // by the paint-driver timer thread as well.
  std::atomic<bool> keyboard_visible_{false};
  // Whether the system reports keyboard visibility changes (see
  // WireKeyboardVisibilityTracking); without it, a system-side dismissal can't
  // be observed and shows have to be retried more defensively.
  bool keyboard_visibility_tracked_ = false;
  // The state the UI asked for, whether a request is in flight on the UI
  // thread, and when it was posted - callers ask every frame, the system only
  // needs one outstanding request.
  std::atomic<bool> keyboard_wanted_{false};
  std::atomic<bool> keyboard_request_pending_{false};
  std::atomic<int64_t> keyboard_request_ms_{0};
  static constexpr int64_t kKeyboardRetryIntervalMs = 250;
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
