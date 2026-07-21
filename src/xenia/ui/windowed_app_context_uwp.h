/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOWED_APP_CONTEXT_UWP_H_
#define XENIA_UI_WINDOWED_APP_CONTEXT_UWP_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

#include <atomic>
#include <functional>
#include <memory>

#include <winrt/Windows.UI.Core.h>

#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace ui {

// UWP windowed-app context. The Win32 backend uses a hidden message-only window
// + PostMessage to wake the UI loop; the CoreWindow equivalent is dispatching a
// low-priority callback onto the CoreDispatcher of the UI thread. We coalesce
// notifications (dispatch_queued_) so a burst of CallInUIThreadDeferred calls
// only schedules one drain, and we guard against the dispatcher firing after
// this context is destroyed with a shared liveness token.
class UWPWindowedAppContext final : public WindowedAppContext {
 public:
  // Must be constructed on the CoreWindow UI thread (its dispatcher is captured
  // here, and WindowedAppContext records this thread as the UI thread).
  UWPWindowedAppContext();
  ~UWPWindowedAppContext();

  const winrt::Windows::UI::Core::CoreDispatcher& dispatcher() const {
    return dispatcher_;
  }

  // Runs a callback on the UI thread AHEAD of the queued normal-priority work.
  // Everything else - painting included - is drained by one coalesced
  // normal-priority item, and a paint blocks on vsync, so while frames are
  // being requested continuously (which is exactly what happens while a text
  // field is active) a normal-priority request can wait behind a full frame.
  // System UI operations that must not be stuck behind rendering - showing the
  // on-screen keyboard - go through here instead.
  // Returns false if the request couldn't be posted.
  bool CallInUIThreadAtHighPriority(std::function<void()> fn);

 protected:
  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

 private:
  winrt::Windows::UI::Core::CoreDispatcher dispatcher_{nullptr};
  std::atomic<bool> dispatch_queued_{false};
  // Lets an in-flight RunAsync callback detect that the context has already
  // been destroyed and bail out instead of touching freed memory.
  std::shared_ptr<std::atomic<bool>> alive_;
};

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT

#endif  // XENIA_UI_WINDOWED_APP_CONTEXT_UWP_H_
