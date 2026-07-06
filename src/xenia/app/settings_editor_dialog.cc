/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/settings_editor_dialog.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "third_party/imgui/imgui.h"

#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"

namespace xe {
namespace app {

namespace {

// Case-insensitive substring match for the filter box.
bool MatchesFilter(const std::string& haystack, const char* needle) {
  if (!needle || !needle[0]) {
    return true;
  }
  auto it = std::search(haystack.begin(), haystack.end(), needle,
                        needle + std::strlen(needle), [](char a, char b) {
                          return std::tolower(uint8_t(a)) ==
                                 std::tolower(uint8_t(b));
                        });
  return it != haystack.end();
}

}  // namespace

SettingsEditorDialog::SettingsEditorDialog(ui::ImGuiDrawer* imgui_drawer,
                                           EmulatorWindow* emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

void SettingsEditorDialog::DrawVariableEditor(const std::string& name,
                                              cvar::IConfigVar* var) {
  ImGui::PushID(name.c_str());

  if (auto* bool_var = dynamic_cast<cvar::ConfigVar<bool>*>(var)) {
    bool value = *bool_var->current_value();
    if (ImGui::Checkbox(name.c_str(), &value)) {
      bool_var->SetConfigValue(value);
      dirty_ = true;
    }
  } else if (auto* i32_var = dynamic_cast<cvar::ConfigVar<int32_t>*>(var)) {
    int value = int(*i32_var->current_value());
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputInt(name.c_str(), &value, 1, 100,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
      i32_var->SetConfigValue(int32_t(value));
      dirty_ = true;
    }
  } else if (auto* u32_var = dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) {
    int value = int(*u32_var->current_value());
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputInt(name.c_str(), &value, 1, 100,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
      u32_var->SetConfigValue(uint32_t(std::max(0, value)));
      dirty_ = true;
    }
  } else if (auto* i64_var = dynamic_cast<cvar::ConfigVar<int64_t>*>(var)) {
    int64_t value = *i64_var->current_value();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputScalar(name.c_str(), ImGuiDataType_S64, &value, nullptr,
                           nullptr, nullptr,
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
      i64_var->SetConfigValue(value);
      dirty_ = true;
    }
  } else if (auto* u64_var = dynamic_cast<cvar::ConfigVar<uint64_t>*>(var)) {
    uint64_t value = *u64_var->current_value();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputScalar(name.c_str(), ImGuiDataType_U64, &value, nullptr,
                           nullptr, nullptr,
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
      u64_var->SetConfigValue(value);
      dirty_ = true;
    }
  } else if (auto* dbl_var = dynamic_cast<cvar::ConfigVar<double>*>(var)) {
    double value = *dbl_var->current_value();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputDouble(name.c_str(), &value, 0.0, 0.0, "%.6g",
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
      dbl_var->SetConfigValue(value);
      dirty_ = true;
    }
  } else if (auto* str_var =
                 dynamic_cast<cvar::ConfigVar<std::string>*>(var)) {
    std::string& buffer = text_buffers_[name];
    if (buffer.capacity() < 512) {
      buffer.reserve(512);
      buffer = *str_var->current_value();
    }
    buffer.resize(511);
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::InputText(name.c_str(), buffer.data(), buffer.capacity(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      str_var->SetConfigValue(std::string(buffer.c_str()));
      dirty_ = true;
    }
    buffer.resize(std::strlen(buffer.c_str()));
  } else if (auto* path_var =
                 dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
    std::string& buffer = text_buffers_[name];
    if (buffer.capacity() < 512) {
      buffer.reserve(512);
      buffer = xe::path_to_utf8(*path_var->current_value());
    }
    buffer.resize(511);
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::InputText(name.c_str(), buffer.data(), buffer.capacity(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      path_var->SetConfigValue(xe::to_path(std::string(buffer.c_str())));
      dirty_ = true;
    }
    buffer.resize(std::strlen(buffer.c_str()));
  } else {
    // Unknown cvar type - show read-only.
    ImGui::TextDisabled("%s = %s", name.c_str(), var->config_value().c_str());
  }

  // Description as a tooltip and, for gamepad users (no hover), as dimmed text.
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", var->description().c_str());
  }

  ImGui::PopID();
}

void SettingsEditorDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 60.0f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(
      ImVec2(std::min(io.DisplaySize.x * 0.8f, 900.0f),
             std::min(io.DisplaySize.y * 0.8f, 700.0f)),
      ImGuiCond_FirstUseEver);

  bool open = true;
  if (ImGui::Begin("Settings (config.toml)", &open)) {
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputText("Filter", filter_, sizeof(filter_));
    ImGui::SameLine();
    if (ImGui::Button(dirty_ ? "Save to config.toml*" : "Save to config.toml")) {
      config::SaveConfig();
      dirty_ = false;
      XELOGI("SettingsEditor: config saved");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      open = false;
    }
    ImGui::Separator();

    ImGui::BeginChild("##settings_scroll", ImVec2(0, 0), false);

    // Group by category (ConfigVars is sorted by variable name, so categories
    // interleave without this).
    std::map<std::string, std::vector<std::pair<std::string, cvar::IConfigVar*>>>
        categories;
    if (cvar::ConfigVars) {
      for (const auto& [name, var] : *cvar::ConfigVars) {
        if (var->is_transient()) {
          continue;
        }
        if (!MatchesFilter(name, filter_) &&
            !MatchesFilter(var->category(), filter_)) {
          continue;
        }
        categories[var->category()].emplace_back(name, var);
      }
    }
    for (const auto& [category, vars] : categories) {
      if (ImGui::CollapsingHeader(
              category.empty() ? "(no category)" : category.c_str(),
              filter_[0] ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        for (const auto& [name, var] : vars) {
          DrawVariableEditor(name, var);
        }
      }
    }

    ImGui::EndChild();
  }
  ImGui::End();

  if (!open) {
    Close();
  }
}

}  // namespace app
}  // namespace xe
