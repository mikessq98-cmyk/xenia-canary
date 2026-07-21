/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_context_uwp.h"

#include <utility>

#if XE_PLATFORM_WINRT

#include <winrt/Windows.ApplicationModel.Core.h>

namespace xe {
namespace ui {

UWPWindowedAppContext::UWPWindowedAppContext()
    : alive_(std::make_shared<std::atomic<bool>>(true)) {
  // Constructed on the UI thread; capturing the dispatcher here is what lets
  // any thread marshal work back onto it later.
  try {
    dispatcher_ =
        winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
  } catch (...) {
    // Left null; NotifyUILoopOfPendingFunctions becomes a no-op, and pending
    // functions will only run if the loop drains them explicitly.
  }
}

UWPWindowedAppContext::~UWPWindowedAppContext() {
  if (alive_) {
    alive_->store(false, std::memory_order_release);
  }
}

void UWPWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Snapshot the shared state by value so the lambda never dereferences `this`
  // without first confirming the context is still alive.
  const auto dispatcher = dispatcher_;
  const auto alive = alive_;
  if (!dispatcher || !alive) {
    return;
  }
  // Coalesce: if a drain is already scheduled, the queued callback will pick up
  // everything that has been enqueued since.
  if (dispatch_queued_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  try {
    // Normal, not Low: since presents were moved off the GPU-emulation thread,
    // in-game guest frames are painted through this queue - at Low priority
    // they'd be starved by any burst of input/window events on the UI thread,
    // showing up as frame-pacing hiccups. Input is still dispatched by
    // ProcessEvents ahead of Normal-priority work items.
    dispatcher.RunAsync(
        winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
        [this, alive]() {
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          // Reset before draining so notifications raised *during* the drain
          // reliably schedule another pass.
          dispatch_queued_.store(false, std::memory_order_release);
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          ExecutePendingFunctionsFromUIThread();
        });
  } catch (...) {
    dispatch_queued_.store(false, std::memory_order_release);
  }
}

bool UWPWindowedAppContext::CallInUIThreadAtHighPriority(
    std::function<void()> fn) {
  const auto dispatcher = dispatcher_;
  const auto alive = alive_;
  if (!dispatcher || !alive) {
    return false;
  }
  try {
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        [alive, fn = std::move(fn)]() {
                          if (!alive->load(std::memory_order_acquire)) {
                            return;
                          }
                          fn();
                        });
  } catch (...) {
    return false;
  }
  return true;
}

void UWPWindowedAppContext::PlatformQuitFromUIThread() {
  try {
    winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
  } catch (...) {
  }
}

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
