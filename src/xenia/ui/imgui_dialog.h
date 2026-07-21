/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_DIALOG_H_
#define XENIA_UI_IMGUI_DIALOG_H_

#include <functional>
#include <memory>
#include <utility>

#include "xenia/base/threading.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window_listener.h"

namespace xe {
namespace ui {

class ImGuiDialog {
 public:
  virtual ~ImGuiDialog();

  // Shows a simple message box containing a text message.
  // Callers can want for the dialog to close with Wait().
  // Dialogs retain themselves and will delete themselves when closed.
  static ImGuiDialog* ShowMessageBox(ImGuiDrawer* imgui_drawer,
                                     std::string title, std::string body);

  // A fence to signal when the dialog is closed.
  void Then(xe::threading::Fence* fence);

  void Draw();

  bool IsClosing() const { return has_close_pending_; }

  // Dialogs delete themselves once closed (see Draw), so anything keeping a
  // pointer to one must be told when that happens - otherwise the next use of
  // that pointer touches freed memory. Opening the settings editor a second
  // time crashed exactly this way: the owning unique_ptr still held the
  // already-deleted dialog and destroyed it again.
  void SetDestroyedCallback(std::function<void()> callback) {
    destroyed_callback_ = std::move(callback);
  }

 protected:
  ImGuiDialog(ImGuiDrawer* imgui_drawer);

  ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }
  ImGuiIO& GetIO();

  uint64_t GetWindowId() const { return next_window_id_; }
  // Closes the dialog and returns to any waiters.
  void Close();

  virtual void OnShow() {}
  virtual void OnClose() {}
  virtual void OnDraw(ImGuiIO& io) {}

 private:
  static std::atomic<uint64_t> next_window_id_;

  ImGuiDrawer* imgui_drawer_ = nullptr;
  bool has_close_pending_ = false;
  // Invoked from the destructor - see SetDestroyedCallback.
  std::function<void()> destroyed_callback_;
  std::vector<xe::threading::Fence*> waiting_fences_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_DIALOG_H_
