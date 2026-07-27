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
  bool IsOnScreenKeyboardVisible() const override {
    return keyboard_visible_.load(std::memory_order_relaxed);
  }
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
  // Starts a fresh reason to show the keyboard: forgets an earlier dismissal
  // and opens the window during which transient refusals are retried.
  void BeginOnScreenKeyboardRequest();
  // Whether asking the system again is still appropriate - false once it is on
  // screen, once the user has closed it, and once the retry window has passed.
  bool ShouldKeepAskingForOnScreenKeyboard() const;
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
  // Set when the system hid the keyboard while this code still wanted it -
  // i.e. the user closed it. Until whatever wanted it gives up (the text field
  // is deactivated, or the dialog closes), it is not asked for again; without
  // this the dismissal was immediately answered with another show request and
  // the keyboard could not be closed at all.
  std::atomic<bool> keyboard_dismissed_by_user_{false};
  // Until when transient refusals from the system are retried, after something
  // asked for the keyboard. Past it the request stands down rather than
  // occupying the UI thread the system needs to show it.
  std::atomic<int64_t> keyboard_request_deadline_ms_{0};
  static constexpr int64_t kKeyboardRequestWindowMs = 3000;
  // Whether ImGui wanted text input at the previous update - the keyboard is
  // asked for when a field BECOMES active, not for as long as one is.
  bool keyboard_want_previous_ = false;
  // Owner-based lifecycle for the automatic (WantTextInput-driven) keyboard: a
  // text field becoming active opens the keyboard ONCE and owns it as a
  // "session"; WantTextInput toggling during that session does not open a
  // second one. The session ends when the field is really gone (WantTextInput
  // stays false for kKeyboardSessionEndFrames updates, not a one-frame blip),
  // at which point the keyboard is forced down once, or when the user closes
  // it. Touched only on the UI thread (UpdateOnScreenKeyboard, after paint).
  bool keyboard_session_active_ = false;
  uint32_t keyboard_want_false_frames_ = 0;
  static constexpr uint32_t kKeyboardSessionEndFrames = 30;
  // Frames painted, and characters received, since the keyboard came up. A
  // character arriving with no frames painted means nothing can consume it
  // yet - see the CharacterReceived handler.
  std::atomic<uint32_t> paints_since_keyboard_shown_{0};
  std::atomic<uint32_t> keyboard_char_count_{0};
  // Host-uptime ms when the system on-screen keyboard was (successfully)
  // shown; 0 while it is not up. While set, painting is PARKED: under the
  // fullscreen keyboard overlay presents stop completing (the window is
  // occluded), which wedges the UI thread inside OnPaint for the whole typing
  // session - and a wedged UI thread cannot dispatch CharacterReceived, so the
  // typed text only ever arrived a session late. Nothing is visible behind the
  // overlay anyway. Ended by the window-reactivation event (the reliable
  // signal - BOTH CoreInputView visibility events never arrive on this
  // runtime), the hide events if they ever do arrive, the applied hide call,
  // or the activity time cap: painting resumes when neither the show call nor
  // a typed character has confirmed the session within
  // kKeyboardPaintParkMaxMs, so a lost session end can only freeze the UI
  // briefly. Host-uptime ms (Clock::QueryHostUptimeMillis).
  std::atomic<int64_t> keyboard_shown_uptime_ms_{0};
  // A paint was skipped while parked - repaint as soon as the keyboard hides.
  std::atomic<bool> paint_parked_for_keyboard_{false};
  static constexpr int64_t kKeyboardPaintParkMaxMs = 20000;
  // Layered end-of-typing-session detection. On this runtime EVERY system
  // notification that could end the session is dead (CoreInputView's Showing
  // and Hiding never fire, and neither does CoreWindow.Activated when the
  // overlay dismisses), so the end is detected from signals that provably
  // exist: the pad itself - XInput sees it even under the overlay, and the
  // press that CLOSES the keyboard is a B that no character follows (every
  // typing press produces a character within tens of ms) - and the characters
  // (Enter types 0x0D and closes the keyboard). A pending probe holds its
  // deadline and the character count at arming; any character cancels it.
  std::atomic<int64_t> keyboard_close_probe_deadline_ms_{0};
  std::atomic<uint32_t> keyboard_close_probe_char_count_{0};
  // When a probe last ended the session: a character arriving shortly after
  // proves the guess wrong (the keyboard is still up) and the session is
  // re-parked. The window gate keeps physical-keyboard typing, which also
  // lands in CharacterReceived, from ever parking anything.
  std::atomic<int64_t> keyboard_probe_end_ms_{0};
  static constexpr int64_t kKeyboardCloseProbeMs = 400;
  static constexpr int64_t kKeyboardEnterProbeMs = 600;
  static constexpr int64_t kKeyboardReparkWindowMs = 5000;
  // Nothing may end a session before this long after the show: the overlay
  // takes a moment to actually come up, and every trigger fires spuriously in
  // that window - the reactivation event arrives when the overlay APPEARS
  // (the window loses and regains focus), and a B still held from whatever
  // opened the dialog looks like a close press. Ending the session there
  // unparks painting, so the UI thread wedges in the present again and the
  // characters pile up to be delivered in a burst later.
  static constexpr int64_t kKeyboardSessionGraceMs = 800;
  // Previous B-button state seen by the pad poll (timer thread only).
  bool keyboard_pad_b_down_prev_ = false;
  // Direct polling of the real keyboard state, which is what the heuristics
  // above only approximate. Runs as a light high-priority UI-thread work item
  // (the UI thread is free precisely because painting is parked), reading
  // CoreWindow's activation mode and the input pane's occluded rect. Each
  // signal is calibrated on the first poll of a session: it is only trusted
  // if it actually reports the overlay as up at that point, so a signal that
  // is dead on this runtime can never end the session immediately.
  void PollOnScreenKeyboardState();
  std::atomic<bool> keyboard_poll_pending_{false};
  std::atomic<int64_t> keyboard_last_poll_ms_{0};
  static constexpr int64_t kKeyboardPollIntervalMs = 120;
  // Touched only on the UI thread (PollOnScreenKeyboardState / the applied
  // show), so plain bools.
  bool keyboard_poll_baseline_taken_ = false;
  bool keyboard_poll_activation_usable_ = false;
  bool keyboard_poll_pane_usable_ = false;
  // Ends the typing session from any trigger: resumes painting, marks the
  // keyboard hidden and dismissed-while-wanted. Safe from any thread; no-op
  // when no session is active.
  void EndOnScreenKeyboardSession(const char* reason);
  // Whether painting is currently parked for the on-screen keyboard (visible,
  // shown timestamp set, safety cap not yet exceeded).
  bool IsPaintParkedForOnScreenKeyboard() const;
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
  // When the last paint ran, and how far apart paints are kept while waiting
  // for the on-screen keyboard to appear (see RequestPaintImpl).
  std::atomic<int64_t> last_paint_ms_{0};
  static constexpr int64_t kKeyboardWaitPaintIntervalMs = 50;
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
