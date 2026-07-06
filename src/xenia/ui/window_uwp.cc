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


#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/surface_uwp.h"
#include "xenia/ui/ui_event.h"

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
  // CoreWindow has no automatic WM_PAINT. Drive repaints at ~120 Hz; RequestPaint
  // coalesces (paint_pending_) and marshals to the UI thread, and the swap-chain
  // present provides the real vsync throttle, so this never runs ahead of the
  // display. Without it the menu/ImGui never redraws -> black screen.
  //
  // While a game is running (menu suppressed) the guest output itself requests
  // repaints through the presenter, so the timer only needs a slow keep-alive
  // tick for dialogs/notifications - constantly repainting 4K UI frames at
  // 120 Hz would keep a GPU engine at 100% for nothing.
  paint_timer_ = wst::ThreadPoolTimer::CreatePeriodicTimer(
      [this](const wst::ThreadPoolTimer&) {
        if (uwp_menu_suppressed()) {
          static uint32_t slow_tick = 0;
          if ((slow_tick++ % 12) != 0) {  // ~10 Hz keep-alive in-game.
            return;
          }
        }
        RequestPaint();
      },
      std::chrono::milliseconds(8));
}

void UWPWindow::RequestPaintImpl() {
  // The Win32 backend does `InvalidateRect`: thread-safe, coalescing, and the
  // real paint happens later on the UI thread via WM_PAINT. RequestPaint() may
  // be called from the guest-output refresh (GPU) thread, so we must never call
  // OnPaint() inline here. Mirror Win32 by marshaling a coalesced paint onto the
  // UI thread through the windowed-app context's pending-function queue.
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (!app_context().CallInUIThreadDeferred([this]() {
        paint_pending_.store(false, std::memory_order_release);
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

  // Typed characters - delivered for physical keys and for the on-screen
  // keyboard alike. This is what feeds ImGui text fields.
  core_window_.CharacterReceived(
      [this](const wuc::CoreWindow&, const wuc::CharacterReceivedEventArgs& e) {
        KeyEvent ke(this, static_cast<VirtualKey>(e.KeyCode()),
                    int(e.KeyStatus().RepeatCount), e.KeyStatus().WasKeyDown,
                    false, false, false, false);
        WindowDestructionReceiver r(this);
        OnKeyChar(ke, r);
      });

  core_window_.KeyDown([this, is_down](const wuc::CoreWindow& w,
                                       const wuc::KeyEventArgs& e) {
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
        auto p = e.CurrentPoint().Position();
        MouseEvent me(this, MouseEvent::Button::kNone,
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.X)))),
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.Y)))));
        WindowDestructionReceiver r(this);
        OnMouseMove(me, r);
      });
  core_window_.PointerPressed([this](const wuc::CoreWindow&,
                                     const wuc::PointerEventArgs& e) {
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
        auto p = e.CurrentPoint().Position();
        MouseEvent me(this, MouseEvent::Button::kLeft,
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.X)))),
                      int32_t(SizeToPhysical(uint32_t(std::max(0.f, p.Y)))));
        WindowDestructionReceiver r(this);
        OnMouseUp(me, r);
      });
  core_window_.PointerWheelChanged(
      [this](const wuc::CoreWindow&, const wuc::PointerEventArgs& e) {
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
          RequestPaint();
        }
      });
  core_window_.Activated(
      [this](const wuc::CoreWindow&, const wuc::WindowActivatedEventArgs&) {
        RequestPaint();
      });

  input_wired_ = true;
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

void UWPWindow::UpdateOnScreenKeyboard() {
  // Set by the ImGui drawer at the end of each drawn frame; does not depend on
  // which ImGui context is current here.
  const bool want = imgui_wants_text_input();
  if (want == keyboard_visible_) {
    return;
  }
  bool applied = false;
  try {
    // On Xbox there is no mouse/keyboard - only the gamepad. The gamepad-
    // navigable system on-screen keyboard for a custom CoreWindow app is shown
    // via CoreInputView::TryShowPrimaryView(), NOT InputPane::TryShow() (which
    // is unreliable outside XAML text controls). Matches the proven UWP port.
    auto input_view = winrt::Windows::UI::ViewManagement::Core::CoreInputView::
        GetForCurrentView();
    if (input_view) {
      applied = want ? input_view.TryShowPrimaryView()
                     : input_view.TryHidePrimaryView();
      if (!applied) {
        XELOGW("UWPWindow: CoreInputView.Try{}PrimaryView returned false",
               want ? "Show" : "Hide");
      }
    }
  } catch (...) {
  }
  // Only latch the new state on success; on transient refusals (e.g. during
  // focus transitions) this runs again on the next painted frame - this was
  // the cause of the keyboard appearing only ~50% of the time.
  if (applied) {
    keyboard_visible_ = want;
  }
}

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
