/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_MENU_ITEM_UWP_H_
#define XENIA_UI_MENU_ITEM_UWP_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

namespace xe {
namespace ui {

class Window;

// Renders the window's main MenuItem tree as an ImGui main menu bar. CoreWindow
// has no native menu, so this is how the existing Xenia menu (File/Open game,
// Install Content, CPU/GPU/Display settings, Profile, patches, ...) becomes
// usable - and gamepad-navigable - on Xbox. Call once per ImGui frame, after
// ImGui::NewFrame().
void DrawMainMenuBarImGui(Window* window);

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT

#endif  // XENIA_UI_MENU_ITEM_UWP_H_
