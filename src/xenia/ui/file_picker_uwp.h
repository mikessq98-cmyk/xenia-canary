/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_FILE_PICKER_UWP_H_
#define XENIA_UI_FILE_PICKER_UWP_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace xe {
namespace ui {

class Window;

// Shows the WinRT file-open picker asynchronously and invokes `on_picked` on the
// UI thread with the selected paths (empty vector if cancelled/failed).
//
// The synchronous xe::ui::FilePicker::Show() cannot be used on UWP: the picker
// is async and UI-thread-affine, and blocking the UI thread (or pumping a nested
// ProcessEvents) is illegal while the app's main ProcessUntilQuit loop runs
// ("Nested calls to ProcessEvents method is not allowed", 0x8000FFFF). So the
// game-open / install flows call this async entry point instead.
//
// Picked items are added to the FutureAccessList so the *FromApp file APIs in
// filesystem_win.cc can reopen them by path afterwards.
void ShowFileOpenPickerAsyncUWP(
    Window* window,
    std::vector<std::pair<std::string, std::string>> extensions,
    bool multi_selection,
    std::function<void(std::vector<std::filesystem::path>)> on_picked);

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
#endif  // XENIA_UI_FILE_PICKER_UWP_H_
