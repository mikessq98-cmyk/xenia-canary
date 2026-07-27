/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_VIRTUAL_KEYBOARD_H_
#define XENIA_UI_IMGUI_VIRTUAL_KEYBOARD_H_

#include <cstddef>
#include <string>

namespace xe {
namespace ui {

// An on-screen keyboard drawn with ImGui, for hosts where text can only be
// entered with a gamepad.
//
// On Xbox this replaces the SYSTEM on-screen keyboard, which cannot be made to
// work here: it is a fullscreen overlay, so presenting from the UI thread
// stops completing while it is up and wedges that thread (typed characters
// then arrive a whole session late), and every event that would say when it
// opened or closed - CoreInputView's Showing and Hiding, CoreWindow's
// Activated - is never raised on the console runtime, so its lifetime can only
// be guessed at. Drawing the keyboard ourselves removes all of that: frames
// keep running, the field stays visible while typing, each press lands
// immediately, and ImGui's gamepad navigation already moves between the keys.
class ImGuiVirtualKeyboard {
 public:
  enum class Result {
    kNone,
    kAccept,
    kCancel,
  };

  // Draws the keyboard into the current ImGui window and applies presses to
  // `text` (UTF-8; never grows past max_length bytes). Returns what the user
  // asked for, if anything.
  Result Draw(std::string& text, size_t max_length);

  // Forgets the layout state (shift, symbol page, initial focus) - call when
  // the owning dialog opens.
  void Reset() {
    shift_ = false;
    symbols_ = false;
    default_focus_set_ = false;
  }

 private:
  bool shift_ = false;
  bool symbols_ = false;
  bool default_focus_set_ = false;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_VIRTUAL_KEYBOARD_H_
