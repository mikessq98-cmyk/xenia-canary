/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_XBOX_CONSOLE_H_
#define XENIA_BASE_XBOX_CONSOLE_H_

#include <cstdint>

namespace xe {

// What the emulator is actually running on, and what that console granted it.
//
// Everything in the UWP port used to guess this. The memory budget came from
// GlobalMemoryStatusEx, which reports the partition the process happens to be
// in without saying whether that is the partition it was supposed to get; the
// core count came from hardware_concurrency; and nothing at all distinguished
// a Series S from a Series X, although they differ by roughly three times in
// GPU throughput and by a large fraction of the memory partition. So the same
// defaults were handed to both.
//
// This is queried once, at startup, and is the single place anything asks.
enum class XboxModel {
  // Not an Xbox at all - the desktop build, or a UWP build running on a PC.
  kNotXbox,
  // An Xbox the SDK reported but this build does not have a name for. Treated
  // as the most capable known console, because under-serving a newer console
  // is the safer error than over-serving an older one.
  kUnknownXbox,
  kXboxOne,
  kXboxOneS,
  kXboxOneX,
  kXboxSeriesS,
  kXboxSeriesX,
};

struct XboxConsoleProfile {
  XboxModel model = XboxModel::kNotXbox;
  // Development kits report their own device IDs and have a larger memory
  // partition than the retail console of the same generation.
  bool is_devkit = false;

  // The memory limit the OS granted THIS process, from the platform's own
  // accounting rather than from the system-wide commit charge. Zero when the
  // query is unavailable (every non-UWP build).
  uint64_t app_memory_limit_bytes = 0;
  // True when that limit is large enough to be the Game partition rather than
  // the ~1 GB App one. A package that asks for expandedResources but is
  // launched as an "App" gets the small partition, every large allocation
  // fails, and nothing in the logs used to say why - so this is checked and
  // reported explicitly.
  bool has_game_memory_partition = false;

  uint32_t logical_processors = 0;

  // The height the presenter should cap the swap chain to when the user has
  // not chosen one, in guest-output rows. Zero means "do not cap" - the
  // console can afford the display's native resolution.
  uint32_t recommended_present_max_height = 0;

  bool is_xbox() const { return model != XboxModel::kNotXbox; }
  // Series S and Series X share a CPU but not a GPU; the wide gap is in fill
  // rate and in the memory partition, which is what the presenter and the
  // resolution scale care about.
  bool is_series() const {
    return model == XboxModel::kXboxSeriesS || model == XboxModel::kXboxSeriesX;
  }
  const char* model_name() const;
};

// Computed on the first call and cached. Safe from any thread.
const XboxConsoleProfile& GetXboxConsoleProfile();

// Writes the profile to the log, once, along with a warning when the process
// did not get the memory partition it asked for. Called from emulator setup.
void LogXboxConsoleProfile();

// How much of the app's own memory budget is left, live, in bytes.
// UINT64_MAX when the platform does not account for this - callers take the
// minimum of this and whatever else they know, so an unavailable answer
// correctly constrains nothing.
uint64_t QueryAppMemoryHeadroomBytes();

}  // namespace xe

#endif  // XENIA_BASE_XBOX_CONSOLE_H_
