/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/menu_item_uwp.h"

#if XE_PLATFORM_WINRT

#include <string>

#include "third_party/imgui/imgui.h"

#include "xenia/ui/menu_item.h"
#include "xenia/ui/window.h"

namespace xe {
namespace ui {

// CoreWindow has no native menu bar, so MenuItem on UWP is a lightweight node
// that just holds the type/text/hotkey/callback/children (all in the base) and
// is rendered with ImGui by DrawMainMenuBarImGui below.
class ImGuiMenuItem final : public MenuItem {
 public:
  ImGuiMenuItem(Type type, const std::string& text, const std::string& hotkey,
                std::function<void()> callback)
      : MenuItem(type, text, hotkey, callback) {}

  void SetEnabled(bool enabled) override { enabled_ = enabled; }
  bool enabled() const { return enabled_; }

  // Exposes the protected OnSelected() to the renderer.
  void Activate() { OnSelected(); }

 private:
  bool enabled_ = true;
};

std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           const std::string& hotkey,
                                           std::function<void()> callback) {
  return std::make_unique<ImGuiMenuItem>(type, text, hotkey, callback);
}

namespace {

// Win32 menu labels carry '&' mnemonics ("&File", "E&xit"); strip them for ImGui.
std::string StripMnemonics(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '&' && i + 1 < text.size()) {
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

void DrawMenuNode(MenuItem* item) {
  if (!item) {
    return;
  }
  switch (item->type()) {
    case MenuItem::Type::kSeparator:
      ImGui::Separator();
      break;
    case MenuItem::Type::kPopup: {
      const std::string label = StripMnemonics(item->text());
      if (ImGui::BeginMenu(label.c_str())) {
        for (size_t i = 0; i < item->child_count(); ++i) {
          DrawMenuNode(item->child(i));
        }
        ImGui::EndMenu();
      }
    } break;
    case MenuItem::Type::kString:
    case MenuItem::Type::kNormal: {
      const std::string label = StripMnemonics(item->text());
      if (item->child_count() > 0) {
        if (ImGui::BeginMenu(label.c_str())) {
          for (size_t i = 0; i < item->child_count(); ++i) {
            DrawMenuNode(item->child(i));
          }
          ImGui::EndMenu();
        }
      } else {
        auto* imgui_item = static_cast<ImGuiMenuItem*>(item);
        const char* shortcut =
            item->hotkey().empty() ? nullptr : item->hotkey().c_str();
        if (ImGui::MenuItem(label.c_str(), shortcut, false,
                            imgui_item->enabled())) {
          imgui_item->Activate();
        }
      }
    } break;
    default:
      break;
  }
}

}  // namespace

void DrawMainMenuBarImGui(Window* window) {
  if (!window) {
    return;
  }
  MenuItem* root = window->main_menu();
  if (!root || root->child_count() == 0) {
    return;
  }
  if (ImGui::BeginMainMenuBar()) {
    for (size_t i = 0; i < root->child_count(); ++i) {
      DrawMenuNode(root->child(i));
    }
    ImGui::EndMainMenuBar();
  }
}

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
