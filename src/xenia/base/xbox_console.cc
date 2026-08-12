/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/xbox_console.h"

#include "xenia/base/platform.h"

// The console profile is only knowable from inside an Xbox app container; the
// UWP build carries the real implementation in xbox_console_uwp.cc. Everywhere
// else this answers "not an Xbox", so callers can consult it unconditionally
// instead of guarding every use site.
#if !XE_PLATFORM_WINRT

namespace xe {

const char* XboxConsoleProfile::model_name() const { return "not an Xbox"; }

const XboxConsoleProfile& GetXboxConsoleProfile() {
  static const XboxConsoleProfile profile;
  return profile;
}

void LogXboxConsoleProfile() {}

uint64_t QueryAppMemoryHeadroomBytes() { return UINT64_MAX; }

}  // namespace xe

#endif  // !XE_PLATFORM_WINRT
