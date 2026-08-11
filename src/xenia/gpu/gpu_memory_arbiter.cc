/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/gpu_memory_arbiter.h"

#include <algorithm>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace gpu {

const char* GpuMemoryArbiter::GetConsumerName(ConsumerKind kind) {
  switch (kind) {
    case ConsumerKind::kShaderBytecode:
      return "shader bytecode";
    case ConsumerKind::kUploadPools:
      return "upload pools";
    case ConsumerKind::kResourceReusePool:
      return "resource reuse pool";
    case ConsumerKind::kIdleScaledResolve:
      return "idle scaled resolve regions";
    case ConsumerKind::kUnusedRenderTargets:
      return "unused render targets";
    case ConsumerKind::kTextures:
      return "textures";
    case ConsumerKind::kOwningRenderTargets:
      return "render targets holding EDRAM data";
    default:
      return "unknown";
  }
}

const char* GpuMemoryArbiter::GetPressureName(Pressure pressure) {
  switch (pressure) {
    case Pressure::kNone:
      return "none";
    case Pressure::kElevated:
      return "elevated";
    case Pressure::kCritical:
      return "critical";
    default:
      return "unknown";
  }
}

uint64_t GpuMemoryArbiter::QueryFreeHostBytes() {
#if XE_PLATFORM_WIN32
  MEMORYSTATUSEX status = {sizeof(status)};
  if (!GlobalMemoryStatusEx(&status)) {
    return UINT64_MAX;
  }
  // Commit, not physical: on the console allocations fail against the commit
  // charge while free physical RAM still looks plentiful.
  return status.ullAvailPageFile;
#else
  return UINT64_MAX;
#endif  // XE_PLATFORM_WIN32
}

void GpuMemoryArbiter::RegisterConsumer(ConsumerKind kind, UsageFunction usage,
                                        TrimFunction trim,
                                        FreeUnneededFunction free_unneeded) {
  Consumer consumer;
  consumer.kind = kind;
  consumer.usage = std::move(usage);
  consumer.trim = std::move(trim);
  consumer.free_unneeded = std::move(free_unneeded);
  consumers_.push_back(std::move(consumer));
  // Keep the list in eviction order so Update just walks it.
  std::stable_sort(consumers_.begin(), consumers_.end(),
                   [](const Consumer& a, const Consumer& b) {
                     return uint32_t(a.kind) < uint32_t(b.kind);
                   });
}

void GpuMemoryArbiter::UnregisterConsumers() { consumers_.clear(); }

uint64_t GpuMemoryArbiter::GetTotalUsage() const {
  uint64_t total = 0;
  for (const Consumer& consumer : consumers_) {
    if (consumer.usage) {
      total += consumer.usage();
    }
  }
  return total;
}

void GpuMemoryArbiter::LogStatistics() const {
  uint64_t free_bytes = last_free_bytes_;
  XELOGI(
      "[MEM] gpu memory core: pressure {}, {} MB free, using {} MB/s | "
      "released on sight {} MB, forced trims {} freeing {} MB",
      GetPressureName(pressure()),
      free_bytes == UINT64_MAX ? 0 : (free_bytes >> 20),
      int64_t(consumption_bytes_per_second_) >> 20,
      total_freed_immediately_bytes_ >> 20, total_trims_,
      total_released_bytes_ >> 20);
  for (const Consumer& consumer : consumers_) {
    if (!consumer.usage) {
      continue;
    }
    uint64_t usage = consumer.usage();
    if (!usage) {
      continue;
    }
    XELOGI("[MEM]   {} MB held by {}", usage >> 20,
           GetConsumerName(consumer.kind));
  }
}

double GpuMemoryArbiter::UpdateTrendAndGetSecondsToFloor(uint64_t free_bytes) {
  constexpr double kInfinite = 1.0e9;
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (last_trend_free_bytes_ == UINT64_MAX || now_ms <= last_trend_time_ms_) {
    last_trend_free_bytes_ = free_bytes;
    last_trend_time_ms_ = now_ms;
    return kInfinite;
  }
  double elapsed_seconds = double(now_ms - last_trend_time_ms_) / 1000.0;
  if (elapsed_seconds < 0.05) {
    return consumption_bytes_per_second_ > 0.0
               ? double(free_bytes > kFloorFreeBytes
                            ? free_bytes - kFloorFreeBytes
                            : 0) /
                     consumption_bytes_per_second_
               : kInfinite;
  }
  // Positive when free memory is falling.
  double delta = double(int64_t(last_trend_free_bytes_) - int64_t(free_bytes));
  double instant_rate = delta / elapsed_seconds;
  last_trend_free_bytes_ = free_bytes;
  last_trend_time_ms_ = now_ms;
  // Smoothed, because one poll that happens to straddle a level load reads as
  // hundreds of MB/s and would trigger a panic trim over nothing.
  consumption_bytes_per_second_ =
      consumption_bytes_per_second_ * (1.0 - kRateSmoothing) +
      instant_rate * kRateSmoothing;
  if (consumption_bytes_per_second_ <= 0.0) {
    // Not falling - memory is being returned faster than taken.
    return kInfinite;
  }
  uint64_t above_floor =
      free_bytes > kFloorFreeBytes ? free_bytes - kFloorFreeBytes : 0;
  return double(above_floor) / consumption_bytes_per_second_;
}

void GpuMemoryArbiter::Update(uint64_t submission_index) {
  if (consumers_.empty()) {
    return;
  }
  // How often to look depends on how fast memory is moving. A GTA IV session
  // at 3x3 lost 1700 MB between two polls 32 submissions apart - by the time
  // the next reading arrived there was nothing left to do about it. When
  // consumption is high, or the remaining headroom is small, the OS query is
  // cheap compared to being blind through a level load.
  uint64_t poll_interval = kPollIntervalSubmissions;
  if (consumption_bytes_per_second_ > double(kFastConsumptionBytesPerSecond) ||
      (last_free_bytes_ != UINT64_MAX &&
       last_free_bytes_ < kElevatedFreeBytes)) {
    poll_interval = kFastPollIntervalSubmissions;
  }
  if (submission_index - last_poll_submission_ < poll_interval) {
    return;
  }
  last_poll_submission_ = submission_index;

  uint64_t free_bytes = QueryFreeHostBytes();
  last_free_bytes_ = free_bytes;

  // Whatever is genuinely dead goes back now, at any pressure. Keeping it
  // costs the emulation nothing to lose and brings the shortage closer - and
  // doing it continuously is what keeps the trend gentle enough that the
  // predictive path below rarely has to do anything at all.
  for (const Consumer& consumer : consumers_) {
    if (!consumer.free_unneeded) {
      continue;
    }
    uint64_t freed = consumer.free_unneeded();
    if (freed) {
      total_freed_immediately_bytes_ += freed;
    }
  }

  // How long the current rate of consumption leaves before allocations start
  // failing. This replaces fixed thresholds: what matters is not how much is
  // free at this instant but whether it will still be enough by the time the
  // scene finishes streaming. Reacting to the slope means releases are early,
  // gradual and cheap instead of an emergency.
  double seconds_to_floor = UpdateTrendAndGetSecondsToFloor(free_bytes);

  // The worse of the two readings. The trend sees a slow slide coming while
  // releasing is still cheap; the absolute level sees what no rate can - the
  // next single allocation. At a draw resolution scale one scaled-resolve
  // region is worth many seconds of the average rate, so a comfortable
  // extrapolation is not evidence that the memory is there.
  Pressure by_trend;
  if (seconds_to_floor <= kCriticalSecondsToFloor) {
    by_trend = Pressure::kCritical;
  } else if (seconds_to_floor <= kElevatedSecondsToFloor) {
    by_trend = Pressure::kElevated;
  } else {
    by_trend = Pressure::kNone;
  }
  Pressure by_level;
  if (free_bytes <= kCriticalFreeBytes) {
    by_level = Pressure::kCritical;
  } else if (free_bytes <= kElevatedFreeBytes) {
    by_level = Pressure::kElevated;
  } else {
    by_level = Pressure::kNone;
  }
  Pressure new_pressure;
  if (free_bytes == UINT64_MAX) {
    new_pressure = Pressure::kNone;
  } else {
    new_pressure = std::max(by_trend, by_level);
  }
  // Hysteresis. A Dark Souls II session sat at 727-735 MB free while the rate
  // wobbled between 45 and 58 MB/s, and the level flapped
  // critical->elevated->critical->elevated - each visit to critical starting
  // another trim over a few megabytes of difference. Rising is immediate,
  // because a shortage must be acted on at once; falling waits for the
  // improvement to hold, so noise around a boundary cannot drive the caches.
  Pressure previous = pressure_.load(std::memory_order_relaxed);
  if (new_pressure < previous) {
    if (++pressure_relief_polls_ < kPollsBeforeRelief) {
      new_pressure = previous;
    } else {
      pressure_relief_polls_ = 0;
    }
  } else {
    pressure_relief_polls_ = 0;
  }
  Pressure old_pressure = pressure_.exchange(new_pressure,
                                             std::memory_order_relaxed);
  if (new_pressure != old_pressure) {
    XELOGI(
        "GPU memory: pressure {} -> {} ({} MB free, using {} MB/s, {} s of "
        "headroom by trend; trend says {}, level says {})",
        GetPressureName(old_pressure), GetPressureName(new_pressure),
        free_bytes == UINT64_MAX ? 0 : (free_bytes >> 20),
        int64_t(consumption_bytes_per_second_) >> 20,
        seconds_to_floor >= 1.0e8 ? -1 : int64_t(seconds_to_floor),
        GetPressureName(by_trend), GetPressureName(by_level));
  }

  if (new_pressure == Pressure::kNone) {
    return;
  }

  // Let a previous trim take effect before deciding it was not enough - the
  // memory it released may not have been reflected yet (buffers are only
  // freed once the GPU is done with them), and trimming again immediately is
  // how this turns into a feedback loop.
  if (last_trim_submission_ &&
      submission_index - last_trim_submission_ < kTrimCooldownSubmissions) {
    return;
  }

  // Ask for enough to buy back a comfortable amount of TIME at the observed
  // rate, not a fixed number of megabytes: the same shortfall means something
  // very different at 5 MB/s and at 200 MB/s.
  uint64_t bytes_to_free = uint64_t(std::max(
      0.0, consumption_bytes_per_second_ * kTrimTargetSecondsOfHeadroom));
  if (free_bytes < kFloorFreeBytes) {
    bytes_to_free += kFloorFreeBytes - free_bytes;
  }
  if (new_pressure == Pressure::kElevated) {
    // Elevated is the smoothing band. Give back a little on every pass, well
    // before anything is urgent, so the shortage is met by a series of
    // releases nobody can feel rather than by one that stalls a frame. The
    // caches also age out their own idle contents here; this is the part that
    // keeps up when they cannot.
    bytes_to_free = std::min(bytes_to_free, kElevatedTrimPerPassBytes);
  }
  bytes_to_free = std::clamp(bytes_to_free, kMinTrimBytes, kMaxTrimBytes);
  // Take it in bites. Reaching the target over several passes a few
  // submissions apart is invisible; reaching it in one is a stall the length
  // of destroying hundreds of resources, and it overshoots - the game reloads
  // what was thrown away, which is worse than not having released it.
  bytes_to_free = std::min(bytes_to_free, kMaxTrimPerPassBytes);
  uint64_t total_before = GetTotalUsage();
  uint64_t released_total = 0;

  for (const Consumer& consumer : consumers_) {
    if (released_total >= bytes_to_free) {
      break;
    }
    if (!consumer.trim) {
      continue;
    }
    if (consumer.usage && !consumer.usage()) {
      continue;
    }
    uint64_t released = consumer.trim(bytes_to_free - released_total);
    if (released) {
      XELOGI("GPU memory: released {} MB of {}", released >> 20,
             GetConsumerName(consumer.kind));
      released_total += released;
    }
  }

  last_trim_submission_ = submission_index;
  ++total_trims_;
  total_released_bytes_ += released_total;
  if (released_total) {
    XELOGI(
        "GPU memory: freed {} MB, buying back about {} s at the current rate "
        "(caches held {} MB, host had {} MB free)",
        released_total >> 20,
        consumption_bytes_per_second_ > 0.0
            ? int64_t(double(released_total) / consumption_bytes_per_second_)
            : 0,
        total_before >> 20, free_bytes >> 20);
  } else {
    // Nothing could be given up: everything still in use. Reporting it is
    // what tells a memory problem apart from a leak.
    XELOGW(
        "GPU memory: {} MB free and nothing could be released - the caches "
        "hold {} MB, all of it still in use",
        free_bytes >> 20, total_before >> 20);
  }
}

}  // namespace gpu
}  // namespace xe
