/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/window_uwp.h"

#if XE_PLATFORM_WINRT

#include <algorithm>
#include <chrono>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <gamingdeviceinformation.h>
#include <xinput.h>  // XInputGetState / dwPacketNumber for the idle-wake poll.
// xinput.h defines these as macros, and xenia/hid/input.h (reached through
// imgui_drawer.h -> input_system.h below) declares enumerators with the same
// names - the macro expansion turns them into "0x01 = 0x01" and the whole
// header fails to parse. Only these two collide (verified against the
// 10.0.26100 SDK); the values are not used in this file.
#undef XINPUT_DEVTYPE_GAMEPAD
#undef XINPUT_DEVSUBTYPE_GAMEPAD

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/surface_uwp.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/windowed_app_context_uwp.h"

DEFINE_int32(
    uwp_present_max_height, 0,
    "Cap the UWP swap-chain height in physical pixels (0 = use the native HDMI "
    "mode). Set to 1080, 1440 or 2160 to lower GPU/bandwidth cost on Xbox "
    "Series S; width is derived from the native aspect ratio.",
    "GPU");

namespace xe {
namespace ui {

UWPWindow::UWPWindow(WindowedAppContext& app_context, std::string_view title,
                     uint32_t width, uint32_t height)
    : Window(app_context, title, width, height) {}

UWPWindow::~UWPWindow() {
  if (paint_timer_) {
    paint_timer_.Cancel();
    paint_timer_ = nullptr;
  }
  EnterDestructor();
}

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<UWPWindow>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}

uint32_t UWPWindow::GetLatestDpiImpl() const {
  try {
    auto display =
        winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
    if (display) {
      uint32_t dpi = uint32_t(display.LogicalDpi() + 0.5f);
      if (dpi) {
        return dpi;
      }
    }
  } catch (...) {
  }
  return GetMediumDpi();
}

std::unique_ptr<Surface> UWPWindow::CreateSurfaceImpl(
    Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_UWPCore)) {
    return nullptr;
  }
  if (!core_window_) {
    core_window_ =
        winrt::Windows::ApplicationModel::Core::CoreApplication::MainView()
            .CoreWindow();
  }
  ::IUnknown* abi = static_cast<::IUnknown*>(winrt::get_abi(core_window_));
  return std::make_unique<UWPCoreWindowSurface>(abi);
}

bool UWPWindow::OpenImpl() {
  XELOGI("UWPWindow::OpenImpl");
  // Start from the physical size implied by the requested logical size.
  uint32_t width = SizeToPhysical(GetDesiredLogicalWidth());
  uint32_t height = SizeToPhysical(GetDesiredLogicalHeight());

  // On Xbox the HDMI mode is the real output resolution; prefer it.
  GAMING_DEVICE_MODEL_INFORMATION info = {};
  GetGamingDeviceModelInformation(&info);
  if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
    try {
      auto hdmi = winrt::Windows::Graphics::Display::Core::
          HdmiDisplayInformation::GetForCurrentView();
      if (hdmi) {
        if (auto mode = hdmi.GetCurrentDisplayMode()) {
          width = mode.ResolutionWidthInRawPixels();
          height = mode.ResolutionHeightInRawPixels();
        }
      }
    } catch (...) {
    }
  }

  if (width == 0 || height == 0) {
    width = 1920;
    height = 1080;
  }

  // Optional GPU-load cap: scale down to the requested height keeping the
  // native aspect ratio (e.g. render/scan-out at 1080p on a 4K Series S).
  if (cvars::uwp_present_max_height > 0 &&
      height > uint32_t(cvars::uwp_present_max_height)) {
    const uint32_t native_width = width;
    const uint32_t native_height = height;
    const uint32_t capped_height = uint32_t(cvars::uwp_present_max_height);
    const uint32_t capped_width =
        uint32_t(uint64_t(native_width) * capped_height / native_height);
    // Keep dimensions even for chroma/scaler friendliness.
    width = capped_width & ~1u;
    height = capped_height & ~1u;
    XELOGI("UWPWindow: capping swap chain to {}x{} (native {}x{})", width,
           height, native_width, native_height);
  }

  WireCoreWindowInput();
  StartPaintLoop();

  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, WindowResizeAction::kManual,
                     destruction_receiver);
  return !destruction_receiver.IsWindowDestroyed();
}

void UWPWindow::StartPaintLoop() {
  if (paint_timer_) {
    return;
  }
  namespace wst = winrt::Windows::System::Threading;
  // CoreWindow has no automatic WM_PAINT, so this timer decides when to repaint.
  // Unlike a naive "RequestPaint every tick" loop (which pins a GPU engine at
  // 100% re-rendering a static 4K menu every vsync), this repaints only ON
  // DEMAND, mirroring the old UWP port's on-request model:
  //   - fresh input (gamepad via dwPacketNumber, or keyboard/pointer events),
  //     plus a short tail afterwards so ImGui nav/hover animations settle;
  //   - while a text field is active (imgui_wants_text_input) - the static menu
  //     doesn't self-request repaints, so without this on-screen-keyboard
  //     editing would stall; forcing paints here also keeps
  //     UpdateOnScreenKeyboard (run after each paint) in sync, which is what
  //     makes the keyboard appear immediately instead of only after the user's
  //     first input;
  //   - while the View-hold-to-open-keyboard gesture is timing out.
  // Guest output (in-game) and dialogs/notifications drive their own repaints
  // through the presenter, so a truly idle menu now leaves the GPU alone.
  paint_timer_ = wst::ThreadPoolTimer::CreatePeriodicTimer(
      [this](const wst::ThreadPoolTimer&) {
        bool activity = input_activity_.exchange(false, std::memory_order_acq_rel);
        activity |= PollGamepadActivity();
        if (activity) {
          paint_tail_ticks_ = 12;  // ~200 ms of follow-up frames at 60 Hz.
        }

        const bool wants_text = imgui_wants_text_input();
        const bool holding_view =
            view_hold_start_ms_.load(std::memory_order_relaxed) != 0;
        // While a debug overlay is active, keep painting so the ImGui drawer
        // registers itself and the overlay stays on screen over guest output.
        const bool debug_overlay_active = !GetDebugOverlayLine().empty();

        bool should_paint = wants_text || holding_view || debug_overlay_active;
        if (wants_text && !keyboard_visible_.load(std::memory_order_relaxed)) {
          // Waiting for the system to bring the on-screen keyboard up. Every
          // paint blocks on vsync on the UI thread, and the keyboard needs
          // that same thread to appear - painting flat out here is what makes
          // it come up late or not at all. Ask for a frame only occasionally
          // until it's on screen.
          if (++keyboard_wait_ticks_ < kPaintTicksPerKeyboardWait) {
            should_paint = false;
          } else {
            keyboard_wait_ticks_ = 0;
          }
        } else {
          keyboard_wait_ticks_ = 0;
        }
        if (paint_tail_ticks_ > 0) {
          --paint_tail_ticks_;
          should_paint = true;
        }
        // While painting is parked for the on-screen keyboard (see
        // RequestPaintImpl), don't queue frames that would only be skipped.
        if (should_paint && IsPaintParkedForOnScreenKeyboard()) {
          should_paint = false;
        }
        // In-game the menu is suppressed and the guest drives its own presents;
        // only nudge a paint on the conditions above (e.g. a just-opened
        // keyboard prompt) - never a periodic full-frame repaint.
        if (should_paint) {
          RequestPaint();
        }
      },
      std::chrono::milliseconds(16));
}

// Quantizes an XInput thumb axis to -1/0/+1 with a deadzone so resting analog
// noise doesn't register as movement.
static uint32_t QuantizeThumbAxis(int16_t value) {
  constexpr int16_t kDeadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;  // 7849
  if (value > kDeadzone) {
    return 1;
  }
  if (value < -kDeadzone) {
    return 2;
  }
  return 0;
}

bool UWPWindow::PollGamepadActivity() {
  // Re-probe empty user slots only ~once a second: XInputGetState on an
  // unconnected index is deliberately slow, and hammering it every 16 ms would
  // defeat the point of idling.
  const bool rescan_empty = (gamepad_rescan_counter_++ % 60) == 0;
  bool changed = false;
  for (DWORD user = 0; user < 4; ++user) {
    if (!gamepad_connected_[user] && !rescan_empty) {
      continue;
    }
    XINPUT_STATE state = {};
    if (XInputGetState(user, &state) == ERROR_SUCCESS) {
      if (!gamepad_connected_[user]) {
        gamepad_connected_[user] = true;
        changed = true;
      }
      // Build a signature from only the inputs that drive menu navigation -
      // digital buttons/D-pad plus deadzoned stick and trigger directions.
      // The raw dwPacketNumber is deliberately NOT used: it increments on the
      // tiniest analog jitter, which would wake a repaint every single tick and
      // pin the GPU at a "static" menu forever.
      const XINPUT_GAMEPAD& pad = state.Gamepad;
      constexpr uint8_t kTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;  // 30
      uint32_t sig = pad.wButtons;
      sig |= QuantizeThumbAxis(pad.sThumbLX) << 16;
      sig |= QuantizeThumbAxis(pad.sThumbLY) << 18;
      sig |= QuantizeThumbAxis(pad.sThumbRX) << 20;
      sig |= QuantizeThumbAxis(pad.sThumbRY) << 22;
      sig |= uint32_t(pad.bLeftTrigger > kTrigger ? 1u : 0u) << 24;
      sig |= uint32_t(pad.bRightTrigger > kTrigger ? 1u : 0u) << 25;
      if (sig != gamepad_last_sig_[user]) {
        gamepad_last_sig_[user] = sig;
        changed = true;
      }
    } else {
      gamepad_connected_[user] = false;
    }
  }
  return changed;
}

bool UWPWindow::IsPaintParkedForOnScreenKeyboard() const {
  if (!keyboard_visible_.load(std::memory_order_relaxed)) {
    return false;
  }
  int64_t shown_ms = keyboard_shown_uptime_ms_.load(std::memory_order_relaxed);
  if (!shown_ms) {
    return false;
  }
  // The typing session is confirmed alive by the show call and by every typed
  // character. If neither has happened for a while, resume painting: on this
  // runtime BOTH CoreInputView visibility events are known to never arrive,
  // and the window-reactivation signal may be missing too - a lost session
  // end must never freeze the UI for long.
  int64_t last_activity_ms =
      std::max(shown_ms, int64_t(last_typed_character_uptime_ms()));
  int64_t now_ms = int64_t(Clock::QueryHostUptimeMillis());
  return now_ms - last_activity_ms <= kKeyboardPaintParkMaxMs;
}

void UWPWindow::RequestPaintImpl() {
  // The Win32 backend does `InvalidateRect`: thread-safe, coalescing, and the
  // real paint happens later on the UI thread via WM_PAINT. RequestPaint() may
  // be called from the guest-output refresh (GPU) thread, so we must never call
  // OnPaint() inline here. Mirror Win32 by marshaling a coalesced paint onto the
  // UI thread through the windowed-app context's pending-function queue.
  // While waiting for the system to put the on-screen keyboard up, thin the
  // paints out. Every paint occupies the UI thread until the next vsync, and
  // the keyboard needs that same thread to appear - a dialog repainting every
  // frame (dialogs drive their own repaints through the presenter, so the
  // paint-driver timer's own backoff doesn't cover them) is exactly why the
  // keyboard used to arrive only once the dialog had closed and the UI thread
  // was free again.
  if (keyboard_wanted_.load(std::memory_order_relaxed) &&
      !keyboard_visible_.load(std::memory_order_relaxed) &&
      !keyboard_dismissed_by_user_.load(std::memory_order_relaxed)) {
    int64_t now_ms =
        int64_t(winrt::clock::now().time_since_epoch().count() / 10000);
    if (now_ms - last_paint_ms_.load(std::memory_order_relaxed) <
        kKeyboardWaitPaintIntervalMs) {
      return;
    }
  }
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (!app_context().CallInUIThreadDeferred([this]() {
        paint_pending_.store(false, std::memory_order_release);
        last_paint_ms_.store(
            int64_t(winrt::clock::now().time_since_epoch().count() / 10000),
            std::memory_order_relaxed);
        if (keyboard_visible_.load(std::memory_order_relaxed)) {
          paints_since_keyboard_shown_.fetch_add(1, std::memory_order_relaxed);
        }
        // While the system on-screen keyboard overlay is up, do NOT paint:
        // under the fullscreen overlay presents stop completing (the window is
        // occluded), so OnPaint wedges the UI thread for the whole typing
        // session - and a wedged UI thread cannot dispatch CharacterReceived,
        // which is why the typed text only ever arrived a session late ("the
        // previous input appears in the next window"). Nothing is visible
        // behind the overlay anyway; the skipped frame is repainted the moment
        // the keyboard hides.
        if (IsPaintParkedForOnScreenKeyboard()) {
          if (!paint_parked_for_keyboard_.exchange(
                  true, std::memory_order_relaxed)) {
            XELOGI(
                "UWPWindow: painting parked while the on-screen keyboard is "
                "up - the UI thread stays free to receive typed characters");
          }
          CheckKeyboardHoldGesture();
          return;
        }
        int64_t paint_start_ms =
            int64_t(winrt::clock::now().time_since_epoch().count() / 10000);
        try {
          OnPaint();
        } catch (const winrt::hresult_error& e) {
          XELOGE("UWPWindow paint hresult 0x{:08X}: {}",
                 uint32_t(e.code().value), winrt::to_string(e.message()));
        } catch (const std::exception& e) {
          XELOGE("UWPWindow paint exception: {}", e.what());
        } catch (...) {
          XELOGE("UWPWindow paint unknown exception");
        }
        {
          // Premise probe: a paint that took hundreds of ms (or seconds) means
          // the present blocked while the window was occluded - the mechanism
          // the parking above exists to avoid.
          int64_t paint_took_ms =
              int64_t(winrt::clock::now().time_since_epoch().count() / 10000) -
              paint_start_ms;
          if (paint_took_ms > 500) {
            XELOGW("UWPWindow: OnPaint blocked the UI thread for {} ms",
                   paint_took_ms);
          }
        }
        // Let the ImGui drawer re-evaluate whether it must be registered (e.g.
        // for a debug overlay set from another thread).
        RunUIThreadPaintTickCallback();
        // After ImGui has updated its IO during OnPaint, reflect its text-input
        // need on the system on-screen keyboard.
        UpdateOnScreenKeyboard();
        CheckKeyboardHoldGesture();
      })) {
    // The loop has already stopped accepting functions (shutting down).
    paint_pending_.store(false, std::memory_order_release);
  }
}

void UWPWindow::RequestCloseImpl() { app_context().RequestDeferredQuit(); }

void UWPWindow::FocusImpl() { RequestPaintImpl(); }

void UWPWindow::WireCoreWindowInput() {
  if (input_wired_) {
    return;
  }
  if (!core_window_) {
    try {
      core_window_ =
          winrt::Windows::ApplicationModel::Core::CoreApplication::MainView()
              .CoreWindow();
    } catch (...) {
    }
  }
  if (!core_window_) {
    return;
  }

  namespace wuc = winrt::Windows::UI::Core;
  namespace wsy = winrt::Windows::System;

  // xe::ui::VirtualKey, Win32 VK_*, and winrt VirtualKey share numeric values,
  // so the cast is a straight pass-through (same as window_win.cc does for WM_*).
  auto is_down = [](const wuc::CoreWindow& w, wsy::VirtualKey vk) {
    return (w.GetKeyState(vk) & wuc::CoreVirtualKeyStates::Down) ==
           wuc::CoreVirtualKeyStates::Down;
  };

  // The PRIMARY end-of-typing-session signal: when the system keyboard
  // overlay dismisses, this window is reactivated. On this runtime BOTH
  // CoreInputView visibility events (Showing and Hiding) never arrive, so
  // without this the paint parking (see IsPaintParkedForOnScreenKeyboard)
  // would only ever end through the activity time cap - a frozen UI after
  // every keyboard use.
  core_window_.Activated([this](const wuc::CoreWindow&,
                                const wuc::WindowActivatedEventArgs& e) {
    if (e.WindowActivationState() ==
        wuc::CoreWindowActivationState::Deactivated) {
      return;
    }
    NoteInputActivity();
    if (!keyboard_shown_uptime_ms_.load(std::memory_order_relaxed)) {
      // No typing session in progress - reactivation from something else
      // (e.g. the guide overlay closing).
      return;
    }
    XELOGI(
        "UWPWindow: window reactivated - ending the on-screen keyboard "
        "typing session ({} characters arrived)",
        keyboard_char_count_.load(std::memory_order_relaxed));
    keyboard_shown_uptime_ms_.store(0, std::memory_order_relaxed);
    // Same semantics as the (never-arriving) Hiding event: the keyboard went
    // away while something still wanted it - the user closed it, and that
    // stands until the dialog/field gives the keyboard up.
    keyboard_visible_ = false;
    if (keyboard_wanted_.load(std::memory_order_relaxed)) {
      keyboard_dismissed_by_user_ = true;
    }
    paint_parked_for_keyboard_.store(false, std::memory_order_relaxed);
    // Repaint immediately so the collected characters land on screen now.
    RequestPaint();
  });

  // Typed characters - delivered for physical keys and for the on-screen
  // keyboard alike. This is what feeds ImGui text fields.
  core_window_.CharacterReceived(
      [this](const wuc::CoreWindow&, const wuc::CharacterReceivedEventArgs& e) {
        NoteInputActivity();
        // Typed text only reaches an ImGui field if a frame runs while the
        // field is active - and the system keyboard is an overlay that may
        // take the UI thread for itself. Report the character together with
        // how many frames have been painted since the keyboard came up: zero
        // means the characters are piling up in ImGui's queue to be applied
        // whenever a frame finally runs, which is what "the text appears the
        // next time the field is opened" looks like.
        uint32_t char_count =
            keyboard_char_count_.fetch_add(1, std::memory_order_relaxed);
        if (char_count < 16) {
          // The ms-after-show number is the decisive timing: characters
          // arriving with small increasing values are LIVE (the UI thread is
          // dispatching during the typing session); a burst of large identical
          // values means they were queued and only delivered later.
          int64_t shown_ms =
              keyboard_shown_uptime_ms_.load(std::memory_order_relaxed);
          int64_t since_show_ms =
              shown_ms ? int64_t(Clock::QueryHostUptimeMillis()) - shown_ms
                       : -1;
          XELOGI(
              "UWPWindow: character 0x{:04X} received - {} frames painted "
              "since the keyboard came up, {} ms after show, ImGui wants text "
              "input: {}",
              uint32_t(e.KeyCode()),
              paints_since_keyboard_shown_.load(std::memory_order_relaxed),
              since_show_ms, imgui_wants_text_input() ? "yes" : "no");
        }
        KeyEvent ke(this, static_cast<VirtualKey>(e.KeyCode()),
                    int(e.KeyStatus().RepeatCount), e.KeyStatus().WasKeyDown,
                    false, false, false, false);
        WindowDestructionReceiver r(this);
        OnKeyChar(ke, r);
      });

  core_window_.KeyDown([this, is_down](const wuc::CoreWindow& w,
                                       const wuc::KeyEventArgs& e) {
    NoteInputActivity();
    // Track the View ("two windows") button for the hold-to-open-keyboard
    // gesture; 0xD0 == Windows.System.VirtualKey.GamepadView.
    if (uint32_t(e.VirtualKey()) == 0xD0 &&
        view_hold_start_ms_.load(std::memory_order_relaxed) == 0) {
      view_hold_start_ms_.store(
          int64_t(winrt::clock::now().time_since_epoch().count() / 10000),
          std::memory_order_relaxed);
    }
    KeyEvent ke(this, static_cast<VirtualKey>(uint32_t(e.VirtualKey())),
                int(e.KeyStatus().RepeatCount), e.KeyStatus().WasKeyDown,
                is_down(w, wsy::VirtualKey::Shift),
                is_down(w, wsy::VirtualKey::Control),
                is_down(w, wsy::VirtualKey::Menu), false);
    WindowDestructionReceiver r(this);
    OnKeyDown(ke, r);
  });

  core_window_.KeyUp([this, is_down](const wuc::CoreWindow& w,
                                     const wuc::KeyEventArgs& e) {
    NoteInputActivity();
    if (uint32_t(e.VirtualKey()) == 0xD0) {
      view_hold_start_ms_.store(0, std::memory_order_relaxed);
      view_hold_fired_ = false;
    }
    KeyEvent ke(this, static_cast<VirtualKey>(uint32_t(e.VirtualKey())),
                int(e.KeyStatus().RepeatCount), e.KeyStatus().WasKeyDown,
                is_down(w, wsy::VirtualKey::Shift),
                is_down(w, wsy::VirtualKey::Control),
                is_down(w, wsy::VirtualKey::Menu), false);
    WindowDestructionReceiver r(this);
    OnKeyUp(ke, r);
  });

  // Pointer is mostly for dev/desktop; on Xbox the gamepad drives ImGui nav.
  // CoreWindow coordinates are in DIPs - scale to physical pixels.
  core_window_.PointerMoved(
      [this](const wuc::CoreWindow&, const wuc::PointerEventArgs& e) {
        NoteInputActivity();
        auto p = e.CurrentPoint().Position();
        MouseEvent me(this, MouseEvent::Button::kNone,
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.X)))),
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.Y)))));
        WindowDestructionReceiver r(this);
        OnMouseMove(me, r);
      });
  core_window_.PointerPressed([this](const wuc::CoreWindow&,
                                     const wuc::PointerEventArgs& e) {
    NoteInputActivity();
    auto props = e.CurrentPoint().Properties();
    auto p = e.CurrentPoint().Position();
    MouseEvent::Button button = props.IsRightButtonPressed()
                                    ? MouseEvent::Button::kRight
                                    : (props.IsMiddleButtonPressed()
                                           ? MouseEvent::Button::kMiddle
                                           : MouseEvent::Button::kLeft);
    MouseEvent me(this, button,
                  int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.X)))),
                  int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.Y)))));
    WindowDestructionReceiver r(this);
    OnMouseDown(me, r);
  });
  core_window_.PointerReleased(
      [this](const wuc::CoreWindow&, const wuc::PointerEventArgs& e) {
        NoteInputActivity();
        auto p = e.CurrentPoint().Position();
        MouseEvent me(this, MouseEvent::Button::kLeft,
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.X)))),
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.Y)))));
        WindowDestructionReceiver r(this);
        OnMouseUp(me, r);
      });
  core_window_.PointerWheelChanged(
      [this](const wuc::CoreWindow&, const wuc::PointerEventArgs& e) {
        NoteInputActivity();
        auto p = e.CurrentPoint().Position();
        int32_t delta = e.CurrentPoint().Properties().MouseWheelDelta();
        MouseEvent me(this, MouseEvent::Button::kNone,
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.X)))),
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.Y)))), 0,
                      delta);
        WindowDestructionReceiver r(this);
        OnMouseWheel(me, r);
      });

  // Visibility/activation: on Xbox the CoreWindow swap chain is only composited
  // to the screen while the window is visible and activated. Log the state and
  // force a repaint on every transition so a stale/black frame can't persist.
  core_window_.VisibilityChanged(
      [this](const wuc::CoreWindow&, const wuc::VisibilityChangedEventArgs& e) {
        if (e.Visible()) {
          // A burst of frames so ImGui layout settles after (re)becoming visible.
          NoteInputActivity();
          RequestPaint();
        }
      });
  core_window_.Activated(
      [this](const wuc::CoreWindow&, const wuc::WindowActivatedEventArgs&) {
        NoteInputActivity();
        RequestPaint();
      });

  WireKeyboardVisibilityTracking();

  input_wired_ = true;
}

void UWPWindow::WireKeyboardVisibilityTracking() {
  // The system opens and closes the on-screen keyboard on its own too - the
  // user dismisses it with B, and it closes itself when the text is committed.
  // Without listening for that, keyboard_visible_ only ever reflects what this
  // code asked for: after the first system-side dismissal it stays stuck at
  // "shown", every later show request is skipped as redundant, and the
  // keyboard never appears again for the rest of the session (in the settings
  // editor, which relies on the automatic path, it may then never appear at
  // all).
  try {
    auto input_view = winrt::Windows::UI::ViewManagement::Core::CoreInputView::
        GetForCurrentView();
    if (input_view) {
      input_view.PrimaryViewShowing(
          [this](const winrt::Windows::UI::ViewManagement::Core::CoreInputView&,
                 const winrt::Windows::UI::ViewManagement::Core::
                     CoreInputViewShowingEventArgs&) {
            keyboard_visible_ = true;
          });
      input_view.PrimaryViewHiding(
          [this](const winrt::Windows::UI::ViewManagement::Core::CoreInputView&,
                 const winrt::Windows::UI::ViewManagement::Core::
                     CoreInputViewHidingEventArgs&) {
            keyboard_visible_ = false;
            // Confirms the runtime does report hides - the gamepad is handed
            // back to the UI here (see ImGuiDrawer::UpdateGamepads), so if
            // this never appeared, that would be why the pad stopped working
            // after using the keyboard.
            static uint32_t hiding_log_count = 0;
            if (hiding_log_count < 4) {
              ++hiding_log_count;
              XELOGI("UWPWindow: the system reports the on-screen keyboard hidden");
            }
            // If the keyboard went away while this code still wanted it on
            // screen, the user closed it themselves - and that has to win.
            // Asking for it again here is what made it impossible to close:
            // every dismissal was answered with another show request.
            if (keyboard_wanted_) {
              keyboard_dismissed_by_user_ = true;
            }
            // End the paint parking for the typing session and repaint, so the
            // characters received during it land on screen right away.
            keyboard_shown_uptime_ms_.store(0, std::memory_order_relaxed);
            if (paint_parked_for_keyboard_.exchange(
                    false, std::memory_order_relaxed)) {
              RequestPaint();
            }
          });
      keyboard_visibility_tracked_ = true;
      XELOGI(
          "UWPWindow: on-screen keyboard visibility tracked through "
          "CoreInputView");
      return;
    }
  } catch (const winrt::hresult_error& e) {
    XELOGW(
        "UWPWindow: CoreInputView visibility events unavailable (0x{:08X}) - "
        "falling back to InputPane",
        uint32_t(e.code().value));
  } catch (...) {
  }
  // Older runtimes without ICoreInputView4 - the input pane's own events cover
  // the same keyboard.
  try {
    auto input_pane =
        winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView();
    if (input_pane) {
      input_pane.Showing(
          [this](const winrt::Windows::UI::ViewManagement::InputPane&,
                 const winrt::Windows::UI::ViewManagement::
                     InputPaneVisibilityEventArgs&) {
            keyboard_visible_ = true;
          });
      input_pane.Hiding(
          [this](const winrt::Windows::UI::ViewManagement::InputPane&,
                 const winrt::Windows::UI::ViewManagement::
                     InputPaneVisibilityEventArgs&) {
            keyboard_visible_ = false;
            // Same as in the CoreInputView path: a keyboard that goes away
            // while this code still wants it was closed by the user, and must
            // not be asked for again until the reason to show it is gone.
            if (keyboard_wanted_) {
              keyboard_dismissed_by_user_ = true;
            }
            // End the paint parking and repaint (see the CoreInputView path).
            keyboard_shown_uptime_ms_.store(0, std::memory_order_relaxed);
            if (paint_parked_for_keyboard_.exchange(
                    false, std::memory_order_relaxed)) {
              RequestPaint();
            }
          });
      keyboard_visibility_tracked_ = true;
      XELOGI(
          "UWPWindow: on-screen keyboard visibility tracked through InputPane");
    }
  } catch (...) {
  }
  if (!keyboard_visibility_tracked_) {
    XELOGW(
        "UWPWindow: no on-screen keyboard visibility events available - the "
        "keyboard state is tracked blindly (it can't be told that the user "
        "closed it)");
  }
}

void UWPWindow::CheckKeyboardHoldGesture() {
  int64_t start_ms = view_hold_start_ms_.load(std::memory_order_relaxed);
  if (!start_ms || view_hold_fired_) {
    return;
  }
  int64_t now_ms =
      int64_t(winrt::clock::now().time_since_epoch().count() / 10000);
  if (now_ms - start_ms < 2000) {
    return;
  }
  bool shown = false;
  try {
    auto input_view = winrt::Windows::UI::ViewManagement::Core::CoreInputView::
        GetForCurrentView();
    if (input_view) {
      shown = input_view.TryShowPrimaryView();
      if (!shown) {
        XELOGW("UWPWindow: CoreInputView.TryShowPrimaryView returned false");
      }
    } else {
      XELOGW("UWPWindow: CoreInputView.GetForCurrentView returned null");
    }
  } catch (const winrt::hresult_error& e) {
    XELOGE("UWPWindow: CoreInputView failed: 0x{:08X} {}",
           uint32_t(e.code().value), winrt::to_string(e.message()));
  }
  if (shown) {
    XELOGI("UWPWindow: View button held - on-screen keyboard opened");
    view_hold_fired_ = true;
  } else {
    // The system sometimes refuses transiently (e.g. focus transitions);
    // retry roughly twice a second while the button is still held.
    view_hold_start_ms_.store(now_ms - 1500, std::memory_order_relaxed);
  }
}

void UWPWindow::RequestOnScreenKeyboardState(bool show) {
  keyboard_wanted_ = show;
  if (show == keyboard_visible_) {
    // Already in the requested state (as reported by the system itself when
    // visibility tracking is available).
    keyboard_request_pending_ = false;
    return;
  }
  // Don't queue a request per frame - the callers ask every frame on purpose
  // (so a transient refusal is retried), but the UI thread only needs one
  // outstanding request at a time.
  int64_t now_ms =
      int64_t(winrt::clock::now().time_since_epoch().count() / 10000);
  if (keyboard_request_pending_ &&
      now_ms - keyboard_request_ms_ < kKeyboardRetryIntervalMs) {
    return;
  }
  keyboard_request_pending_ = true;
  keyboard_request_ms_ = now_ms;

  // Apply it as its own HIGH-priority UI work item. Called from a dialog's
  // draw, this used to run inside the paint work item itself - which is the
  // worst possible moment: the paint has just blocked on vsync, another paint
  // is already queued behind it, and the system's own keyboard view competes
  // for the same UI thread. Jumping the queue is what makes the keyboard come
  // up promptly instead of "sometimes, eventually".
  auto& uwp_context = static_cast<UWPWindowedAppContext&>(app_context());
  if (!uwp_context.CallInUIThreadAtHighPriority(
          [this, show]() { ApplyOnScreenKeyboardState(show); })) {
    // No dispatcher (shutting down) - fall back to applying inline if this
    // already is the UI thread.
    if (app_context().IsInUIThread()) {
      ApplyOnScreenKeyboardState(show);
    }
  }
}

void UWPWindow::ApplyOnScreenKeyboardState(bool show) {
  keyboard_request_pending_ = false;
  if (show != keyboard_wanted_) {
    // The dialog closed (or opened) between queueing this and running it.
    return;
  }
  if (show == keyboard_visible_) {
    return;
  }
  static uint32_t show_fail_count = 0;
  bool applied = false;
  try {
    auto input_view = winrt::Windows::UI::ViewManagement::Core::CoreInputView::
        GetForCurrentView();
    if (input_view) {
      applied = show ? input_view.TryShowPrimaryView()
                     : input_view.TryHidePrimaryView();
      if (!applied && show &&
          (show_fail_count < 5 || (show_fail_count % 300) == 0)) {
        XELOGW(
            "UWPWindow: CoreInputView.TryShowPrimaryView returned false "
            "(attempt {})",
            show_fail_count + 1);
      }
    } else if (show_fail_count < 5) {
      XELOGW("UWPWindow: CoreInputView.GetForCurrentView returned null");
    }
  } catch (const winrt::hresult_error& e) {
    if (show_fail_count < 5) {
      XELOGE("UWPWindow: on-screen keyboard {} failed: 0x{:08X} {}",
             show ? "show" : "hide", uint32_t(e.code().value),
             winrt::to_string(e.message()));
    }
  } catch (...) {
    if (show_fail_count < 5) {
      XELOGE("UWPWindow: on-screen keyboard {} failed: unknown exception",
             show ? "show" : "hide");
    }
  }
  if (applied) {
    if (show) {
      XELOGI("UWPWindow: on-screen keyboard shown (after {} refusals)",
             show_fail_count);
      show_fail_count = 0;
    }
    // The call returning true means the system took the request - believe it
    // rather than waiting for a confirming event. On this runtime the
    // PrimaryViewShowing event never arrives, so waiting for it left the
    // keyboard permanently "not visible" here, and every field or dialog that
    // still wanted it asked again - the keyboard reopening itself over and
    // over. The hiding event, which does arrive, is what clears this again.
    keyboard_visible_ = show;
    if (show) {
      paints_since_keyboard_shown_.store(0, std::memory_order_relaxed);
      keyboard_char_count_.store(0, std::memory_order_relaxed);
      // Park painting for the typing session - under the fullscreen overlay
      // presents stop completing and would wedge the UI thread, which must
      // stay free to dispatch the typed characters.
      keyboard_shown_uptime_ms_.store(int64_t(Clock::QueryHostUptimeMillis()),
                                      std::memory_order_relaxed);
    } else {
      keyboard_shown_uptime_ms_.store(0, std::memory_order_relaxed);
      if (paint_parked_for_keyboard_.exchange(false,
                                              std::memory_order_relaxed)) {
        RequestPaint();
      }
    }
  } else if (show) {
    ++show_fail_count;
  }
}

void UWPWindow::ShowOnScreenKeyboard() {
  // Called every frame from a dialog's draw for as long as its text field
  // should be editable. Asking the system every frame is what fights the user:
  // the keyboard is asked for when the dialog TAKES the keyboard, and then
  // only until it actually appears - after that, and after the user closes it,
  // the request stands down.
  if (!explicit_keyboard_hold_.exchange(true)) {
    BeginOnScreenKeyboardRequest();
  }
  if (!ShouldKeepAskingForOnScreenKeyboard()) {
    return;
  }
  RequestOnScreenKeyboardState(true);
}

void UWPWindow::HideOnScreenKeyboard() {
  bool had_hold = explicit_keyboard_hold_.exchange(false);
  if (had_hold) {
    // The dialog is done with the keyboard - the next one starts from scratch,
    // including forgetting that the user closed this one. Only on a real
    // release: the ImGui drawer calls this every frame when no dialog is open,
    // and clearing the dismissal there would re-open the keyboard the user
    // just closed on the very next frame.
    keyboard_dismissed_by_user_ = false;
  }
  if (!had_hold && !keyboard_visible_ && !keyboard_wanted_) {
    return;
  }
  RequestOnScreenKeyboardState(false);
}

void UWPWindow::BeginOnScreenKeyboardRequest() {
  // A fresh reason to show the keyboard - an earlier dismissal doesn't apply
  // to it, and the system gets a window in which transient refusals are
  // retried.
  keyboard_dismissed_by_user_ = false;
  // Don't trust what this code believes about the keyboard being up: the
  // runtime may not report a hide either, and a stale "it's already there"
  // would mean never asking again. Asking for a keyboard that happens to be
  // on screen already costs nothing.
  keyboard_visible_ = false;
  keyboard_request_deadline_ms_.store(
      int64_t(winrt::clock::now().time_since_epoch().count() / 10000) +
          kKeyboardRequestWindowMs,
      std::memory_order_relaxed);
}

bool UWPWindow::ShouldKeepAskingForOnScreenKeyboard() const {
  if (keyboard_dismissed_by_user_ || keyboard_visible_) {
    // The user closed it, or it's already up - either way, stop asking.
    return false;
  }
  // Retry only for a while after the request was made. Beyond that the system
  // is refusing for a reason of its own, and hammering it just keeps the UI
  // thread busy.
  int64_t now_ms =
      int64_t(winrt::clock::now().time_since_epoch().count() / 10000);
  return now_ms <=
         keyboard_request_deadline_ms_.load(std::memory_order_relaxed);
}

void UWPWindow::UpdateOnScreenKeyboard() {
  // A dialog is driving the keyboard explicitly - don't let the WantTextInput
  // automation hide (or re-show) it out from under the dialog.
  if (explicit_keyboard_hold_) {
    return;
  }
  // Set by the ImGui drawer at the end of each drawn frame; does not depend on
  // which ImGui context is current here. This is what serves ImGui text fields
  // that don't manage the keyboard themselves - the settings editor above all.
  const bool want = imgui_wants_text_input();
  const bool want_became_true = want && !keyboard_want_previous_;
  keyboard_want_previous_ = want;

  if (!keyboard_session_active_) {
    // Nothing owns the keyboard. A text field BECOMING active opens it once
    // and takes ownership of it for as long as that field is used - one
    // appearance per activation, no matter how WantTextInput toggles after.
    if (!want_became_true) {
      return;
    }
    keyboard_session_active_ = true;
    keyboard_want_false_frames_ = 0;
    BeginOnScreenKeyboardRequest();
  }

  // A session is active.
  if (keyboard_dismissed_by_user_) {
    // The user closed the keyboard. Don't fight them - release the session so
    // it stays closed; the next field activation opens a fresh one.
    keyboard_session_active_ = false;
    return;
  }
  if (want) {
    keyboard_want_false_frames_ = 0;
  } else if (++keyboard_want_false_frames_ >= kKeyboardSessionEndFrames) {
    // The field is really gone (not a one-frame blip while the overlay has
    // focus) - force the keyboard down once and end the session.
    keyboard_session_active_ = false;
    RequestOnScreenKeyboardState(false);
    return;
  }
  // Retry the single show only until it is actually up or the window passes -
  // ShouldKeepAsking goes false once keyboard_visible_ is set.
  if (want && ShouldKeepAskingForOnScreenKeyboard()) {
    RequestOnScreenKeyboardState(true);
  }
}

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
