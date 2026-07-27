/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_virtual_keyboard.h"

#include "third_party/imgui/imgui.h"

namespace xe {
namespace ui {

namespace {

// Four rows per page, laid out like a phone keyboard. The letter page's first
// row doubles as the digits so numbers never need a page switch.
constexpr const char* kLetterRowsLower[] = {"1234567890", "qwertyuiop",
                                            "asdfghjkl", "zxcvbnm"};
constexpr const char* kLetterRowsUpper[] = {"1234567890", "QWERTYUIOP",
                                            "ASDFGHJKL", "ZXCVBNM"};
constexpr const char* kSymbolRows[] = {"1234567890", "-_=+[]{}\\|",
                                       ";:'\",.<>/?", "!@#$%^&*()"};

// Drops the last UTF-8 code point (continuation bytes first).
void PopBackUtf8(std::string& text) {
  while (!text.empty() && (uint8_t(text.back()) & 0xC0) == 0x80) {
    text.pop_back();
  }
  if (!text.empty()) {
    text.pop_back();
  }
}

}  // namespace

ImGuiVirtualKeyboard::Result ImGuiVirtualKeyboard::Draw(std::string& text,
                                                        size_t max_length) {
  Result result = Result::kNone;

  const float font_size = ImGui::GetFontSize();
  const ImVec2 key_size(font_size * 2.4f, font_size * 2.0f);
  const float spacing = ImGui::GetStyle().ItemSpacing.x;

  const char* const* rows = symbols_ ? kSymbolRows
                                     : (shift_ ? kLetterRowsUpper
                                               : kLetterRowsLower);

  ImGui::PushID("xe_virtual_keyboard");
  for (int row_index = 0; row_index < 4; ++row_index) {
    const char* row = rows[row_index];
    // Indent the shorter rows so the block stays centred under the first one.
    float row_width = 0.0f;
    for (const char* p = row; *p; ++p) {
      row_width += key_size.x + spacing;
    }
    float full_width = 10.0f * (key_size.x + spacing);
    if (row_width < full_width) {
      ImGui::Dummy(ImVec2((full_width - row_width) * 0.5f, 0.0f));
      ImGui::SameLine();
    }
    for (const char* p = row; *p; ++p) {
      const char key_label[2] = {*p, '\0'};
      ImGui::PushID(int(p - row) + row_index * 100);
      if (ImGui::Button(key_label, key_size)) {
        if (text.size() + 1 < max_length) {
          text.push_back(*p);
        }
      }
      if (!default_focus_set_) {
        // Land the gamepad on the keyboard the first time it is drawn, so it
        // is usable without hunting for focus.
        default_focus_set_ = true;
        ImGui::SetItemDefaultFocus();
      }
      ImGui::PopID();
      if (*(p + 1)) {
        ImGui::SameLine();
      }
    }
  }

  // Modifier and action row.
  const ImVec2 wide_key_size(key_size.x * 2.0f, key_size.y);
  if (ImGui::Button(shift_ ? "shift on" : "shift", wide_key_size)) {
    shift_ = !shift_;
  }
  ImGui::SameLine();
  if (ImGui::Button(symbols_ ? "abc" : "#+=", wide_key_size)) {
    symbols_ = !symbols_;
  }
  ImGui::SameLine();
  if (ImGui::Button("space", ImVec2(key_size.x * 3.0f, key_size.y))) {
    if (text.size() + 1 < max_length) {
      text.push_back(' ');
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("back", wide_key_size)) {
    PopBackUtf8(text);
  }

  if (ImGui::Button("Done", wide_key_size)) {
    result = Result::kAccept;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", wide_key_size)) {
    result = Result::kCancel;
  }
  ImGui::PopID();

  return result;
}

}  // namespace ui
}  // namespace xe
