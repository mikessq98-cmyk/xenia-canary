/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_SETTINGS_EDITOR_DIALOG_H_
#define XENIA_APP_SETTINGS_EDITOR_DIALOG_H_

#include <map>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace app {

class EmulatorWindow;

// In-app editor for every non-transient config variable (what is stored in
// xenia-canary.config.toml). Made for platforms where the toml can't just be
// opened in a text editor (Xbox): fully ImGui-based, gamepad-navigable, text
// entry via the system on-screen keyboard, with an explicit save button that
// writes the file through config::SaveConfig().
class SettingsEditorDialog final : public ui::ImGuiDialog {
 public:
  SettingsEditorDialog(ui::ImGuiDrawer* imgui_drawer,
                       EmulatorWindow* emulator_window);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawVariableEditor(const std::string& name, cvar::IConfigVar* var);

  EmulatorWindow* emulator_window_;
  char filter_[128] = {0};
  // Per-variable text buffers for string editors (keyed by cvar name).
  std::map<std::string, std::string> text_buffers_;
  bool dirty_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_SETTINGS_EDITOR_DIALOG_H_
