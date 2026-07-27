/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_util.h"
#include "xenia/emulator.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

void CreateProfileUI::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup("Create Profile");
    has_opened_ = true;
  }

  auto profile_manager =
      emulator_->kernel_state()->xam_state()->profile_manager();

  bool dialog_open = true;
  if (!ImGui::BeginPopupModal("Create Profile", &dialog_open,
                              ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
#if XE_PLATFORM_WINRT
    // The popup can go away through this path too - never leak the explicit
    // keyboard hold (it would suppress the keyboard everywhere afterwards).
    imgui_drawer()->window()->HideOnScreenKeyboard();
#endif
    Close();
    return;
  }

  // Focus the gamertag field only while NOTHING is focused (dialog just
  // opened, or focus was lost entirely). Re-focusing while the user has
  // navigated to another item would steal gamepad navigation every frame,
  // locking the cursor on the field.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      !ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused() &&
      !ImGui::IsMouseClicked(0)) {
    ImGui::SetKeyboardFocusHere();
  }

  ImGui::TextUnformatted("Gamertag:");
  const bool enter_pressed =
      ImGui::InputText("##Gamertag", gamertag_, sizeof(gamertag_),
                       ImGuiInputTextFlags_EnterReturnsTrue);
  valid_gamertag_ = profile_manager->IsGamertagValid(std::string(gamertag_));

#if XE_PLATFORM_WINRT
  // Our own keyboard - on a console the gamepad is the only text input device,
  // and the system keyboard is unusable here (see ImGuiVirtualKeyboard).
  ImGui::Spacing();
  std::string keyboard_text(gamertag_);
  auto keyboard_result =
      virtual_keyboard_.Draw(keyboard_text, sizeof(gamertag_));
  xe::string_util::copy_truncating(gamertag_, keyboard_text,
                                   sizeof(gamertag_));
  valid_gamertag_ = profile_manager->IsGamertagValid(std::string(gamertag_));
  if (keyboard_result == xe::ui::ImGuiVirtualKeyboard::Result::kCancel) {
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
    ImGui::CloseCurrentPopup();
    Close();
    ImGui::EndPopup();
    return;
  }
  const bool keyboard_accepted =
      keyboard_result == xe::ui::ImGuiVirtualKeyboard::Result::kAccept;
  ImGui::Spacing();
#else
  const bool keyboard_accepted = false;
#endif  // XE_PLATFORM_WINRT

  ImGui::BeginDisabled(!valid_gamertag_);
  if (ImGui::Button("Create") ||
      ((enter_pressed || keyboard_accepted) && valid_gamertag_)) {
    bool autologin = (profile_manager->GetAccountCount() == 0);
    if (profile_manager->CreateProfile(std::string(gamertag_), autologin,
                                       migration_) &&
        migration_) {
      emulator_->DataMigration(0xB13EBABEBABEBABE);
    }
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
    dialog_open = false;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  if (ImGui::Button("Cancel")) {
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
    dialog_open = false;
  }

  if (!dialog_open) {
#if XE_PLATFORM_WINRT
    imgui_drawer()->window()->HideOnScreenKeyboard();
#endif
    ImGui::CloseCurrentPopup();
    Close();
    ImGui::EndPopup();
    return;
  }
  ImGui::EndPopup();
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
