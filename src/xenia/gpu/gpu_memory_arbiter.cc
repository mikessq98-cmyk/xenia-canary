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
#include <cmath>
#include <utility>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/xbox_console.h"

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
      // Named for what may be TRIMMED from it. What it reports as held is every
      // render target, including the ones holding EDRAM data that this consumer
      // will never give up - the report used to read "719 MB held by unused
      // render targets" when hardly any of it was unused, which sent the search
      // for the memory in the wrong direction.
      return "render targets (only the ones holding no EDRAM data are trimmed)";
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

const char* GpuMemoryArbiter::GetCeilingName(Ceiling ceiling) {
  switch (ceiling) {
    case Ceiling::kSystemCommit:
      return "commit charge";
    case Ceiling::kAppPartition:
      return "app memory partition";
    case Ceiling::kGpuBudget:
      return "GPU budget";
    default:
      return "unknown";
  }
}

void GpuMemoryArbiter::SetHostGpuBudgetQuery(HostGpuBudgetQuery query) {
  host_gpu_budget_query_ = std::move(query);
}

uint64_t GpuMemoryArbiter::QueryFreeHostBytes() const {
  // Three pools, three ceilings, and running out of ANY of them fails an
  // allocation. The core used to watch only the first, which is why a session
  // could be told there was a gigabyte free right up to the moment the driver
  // refused a 336 MB region: the gigabyte was in a pool the resource was never
  // going to come from.
  uint64_t free_bytes = UINT64_MAX;
  Ceiling binding = Ceiling::kSystemCommit;
  last_system_commit_free_bytes_ = 0;
  last_app_partition_free_bytes_ = 0;
  last_gpu_budget_free_bytes_ = 0;

#if XE_PLATFORM_WIN32
  MEMORYSTATUSEX status = {sizeof(status)};
  if (GlobalMemoryStatusEx(&status)) {
    // Commit, not physical: on the console allocations fail against the commit
    // charge while free physical RAM still looks plentiful.
    last_system_commit_free_bytes_ = status.ullAvailPageFile;
    free_bytes = status.ullAvailPageFile;
  }
#endif  // XE_PLATFORM_WIN32

  // What is left of the partition this PROCESS was granted, which on a console
  // is not the same question as what the system has left.
  uint64_t app_free_bytes = xe::QueryAppMemoryHeadroomBytes();
  if (app_free_bytes != UINT64_MAX) {
    last_app_partition_free_bytes_ = app_free_bytes;
    if (app_free_bytes < free_bytes) {
      free_bytes = app_free_bytes;
      binding = Ceiling::kAppPartition;
    }
  }

  if (host_gpu_budget_query_) {
    uint64_t budget = 0, usage = 0;
    if (host_gpu_budget_query_(budget, usage) && budget) {
      uint64_t gpu_free_bytes = usage >= budget ? 0 : budget - usage;
      last_gpu_budget_free_bytes_ = gpu_free_bytes;
      if (gpu_free_bytes < free_bytes) {
        free_bytes = gpu_free_bytes;
        binding = Ceiling::kGpuBudget;
      }
    }
  }

  binding_ceiling_ = binding;
  return free_bytes;
}

void GpuMemoryArbiter::RegisterConsumer(ConsumerKind kind, UsageFunction usage,
                                        TrimFunction trim,
                                        FreeUnneededFunction free_unneeded,
                                        bool trim_safe_mid_submission) {
  Consumer consumer;
  consumer.kind = kind;
  consumer.usage = std::move(usage);
  consumer.trim = std::move(trim);
  consumer.free_unneeded = std::move(free_unneeded);
  consumer.trim_safe_mid_submission = trim_safe_mid_submission;
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

uint64_t GpuMemoryArbiter::GetReserveBytes() const {
  // Enough for one more allocation of the size this title actually makes, plus
  // a margin for the parts of the process this core does not manage (the JIT's
  // code cache, guest thread stacks, the driver's own allocations). At 1x1 the
  // largest anything allocates is a few megabytes and the reserve sits at its
  // floor; at 3x3 a single scaled-resolve region is 356 MB and the reserve
  // grows to match, without anyone having configured either case.
  uint64_t largest = largest_allocation_bytes_;
  uint64_t reserve = kUnmanagedMarginBytes + largest + largest / 2;
  // Whichever of the two instruments asks for more. A title with one enormous
  // allocation is covered by the first; a title with a hundred thousand small
  // ones is covered by the second, and neither is a special case of the other.
  reserve = std::max(reserve, kUnmanagedMarginBytes + demand_reserve_bytes_);
  return std::clamp(reserve, kMinReserveBytes, kMaxReserveBytes);
}

void GpuMemoryArbiter::UpdateConsumerDemand(uint64_t now_ms) {
  uint64_t usage = GetTotalUsage();
  if (last_consumer_usage_bytes_ == UINT64_MAX ||
      now_ms <= last_consumer_usage_time_ms_) {
    last_consumer_usage_bytes_ = usage;
    last_consumer_usage_time_ms_ = now_ms;
    return;
  }
  double elapsed_seconds =
      double(now_ms - last_consumer_usage_time_ms_) / 1000.0;
  if (elapsed_seconds < kTrendWindowSeconds) {
    // Same baseline as the trend, and for the same reason: caches are trimmed
    // and refilled constantly, so anything shorter measures the wobble.
    return;
  }
  double delta = double(int64_t(usage) - int64_t(last_consumer_usage_bytes_));
  last_consumer_usage_bytes_ = usage;
  last_consumer_usage_time_ms_ = now_ms;
  double rate = delta / elapsed_seconds;
  consumer_growth_bytes_per_second_ =
      consumer_growth_bytes_per_second_ * (1.0 - kRateSmoothing) +
      rate * kRateSmoothing;
  if (consumer_growth_bytes_per_second_ <= 0.0) {
    // Caches shrinking or flat - they are asking for nothing.
    demand_reserve_bytes_ = 0;
    return;
  }
  double wanted = consumer_growth_bytes_per_second_ * kDemandCoverSeconds;
  demand_reserve_bytes_ =
      std::min(uint64_t(wanted), kMaxDemandReserveBytes);
}

void GpuMemoryArbiter::NoteAllocationSize(uint64_t bytes, uint64_t now_ms) {
  if (bytes >= largest_allocation_bytes_) {
    largest_allocation_bytes_ = bytes;
    largest_allocation_time_ms_ = now_ms;
    return;
  }
  // Decay: a size that one level needed and nothing since should stop holding
  // the reserve up, but only slowly - the title may simply not have reached
  // that surface again yet.
  if (now_ms - largest_allocation_time_ms_ >= kAllocationObservationMs) {
    largest_allocation_bytes_ = std::max(bytes, largest_allocation_bytes_ / 2);
    largest_allocation_time_ms_ = now_ms;
  }
}

void GpuMemoryArbiter::UpdateSettledLevel(uint64_t free_bytes,
                                          uint64_t now_ms) {
  if (settled_free_bytes_ == UINT64_MAX || now_ms <= settled_time_ms_) {
    settled_free_bytes_ = free_bytes;
    settled_time_ms_ = now_ms;
    return;
  }
  double elapsed_seconds = double(now_ms - settled_time_ms_) / 1000.0;
  settled_time_ms_ = now_ms;
  // One time constant per kSettledLevelTimeConstantSeconds, expressed against
  // real elapsed time rather than per poll - the poll interval changes by a
  // factor of eight depending on how fast memory is moving, and a per-poll
  // factor would make the settled level follow eight times faster exactly when
  // a slide is happening, which is when it must not follow.
  double alpha =
      1.0 - std::exp(-elapsed_seconds / kSettledLevelTimeConstantSeconds);
  double settled = double(settled_free_bytes_);
  settled += (double(free_bytes) - settled) * alpha;
  settled_free_bytes_ = uint64_t(std::max(0.0, settled));
}

void GpuMemoryArbiter::LogStatistics() const {
  uint64_t free_bytes = last_free_bytes_;
  XELOGI(
      "[MEM] gpu memory core: pressure {}, {} MB free (settles at {} MB, "
      "reserving {} MB for allocations of up to {} MB and {} MB/s of cache "
      "demand), using {} MB/s | "
      "released on sight {} MB, forced trims {} freeing {} MB, {} passes "
      "skipped as flat, {} allocation failures reclaimed {} MB",
      GetPressureName(pressure()),
      free_bytes == UINT64_MAX ? 0 : (free_bytes >> 20),
      settled_free_bytes_ == UINT64_MAX ? 0 : (settled_free_bytes_ >> 20),
      GetReserveBytes() >> 20, largest_allocation_bytes_ >> 20,
      // Divided, not shifted: a right shift on a negative value rounds toward
      // negative infinity, so any rate between -1 MB/s and zero - memory being
      // slowly RELEASED, which is a perfectly normal state - printed as
      // "-1 MB/s" and read like a defect. The policy below has always treated
      // a negative rate as zero; only the report was lying about it.
      int64_t(consumer_growth_bytes_per_second_) / (1 << 20),
      int64_t(consumption_bytes_per_second_) / (1 << 20),
      total_freed_immediately_bytes_ >> 20, total_trims_,
      total_released_bytes_ >> 20, total_flat_skips_,
      total_allocation_failures_, total_reserved_for_allocations_bytes_ >> 20);
  // Which pool the free figure above came from, and what the others said. The
  // three disagree by more than a gigabyte in practice, so "free" without this
  // is not an answer to any question worth asking.
  XELOGI(
      "[MEM]   nearest ceiling is the {}: commit {} MB, partition {} MB, GPU "
      "budget {} MB",
      GetCeilingName(binding_ceiling_), last_system_commit_free_bytes_ >> 20,
      last_app_partition_free_bytes_ >> 20, last_gpu_budget_free_bytes_ >> 20);
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

double GpuMemoryArbiter::UpdateTrendAndGetSecondsToReserve(
    uint64_t free_bytes) {
  constexpr double kInfinite = 1.0e9;
  uint64_t reserve = GetReserveBytes();
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (last_trend_free_bytes_ == UINT64_MAX || now_ms <= last_trend_time_ms_) {
    last_trend_free_bytes_ = free_bytes;
    last_trend_time_ms_ = now_ms;
    return kInfinite;
  }
  double elapsed_seconds = double(now_ms - last_trend_time_ms_) / 1000.0;
  // Measure over a long baseline, not between consecutive polls. A Dark Souls
  // II session sat between 1345 and 1388 MB free for twenty minutes - not
  // falling at all - while consecutive samples read 56 MB/s and put the core
  // in elevated almost permanently: 58 trims where none were needed. Memory
  // that goes down and back up is not consumption, and only a window long
  // enough to contain both halves of that can tell the difference.
  if (elapsed_seconds < kTrendWindowSeconds) {
    return consumption_bytes_per_second_ > 0.0
               ? double(free_bytes > reserve ? free_bytes - reserve : 0) /
                     consumption_bytes_per_second_
               : kInfinite;
  }
  // Positive when free memory is genuinely lower than a whole window ago.
  double delta = double(int64_t(last_trend_free_bytes_) - int64_t(free_bytes));
  double window_rate = delta / elapsed_seconds;
  last_trend_free_bytes_ = free_bytes;
  last_trend_time_ms_ = now_ms;
  consumption_bytes_per_second_ =
      consumption_bytes_per_second_ * (1.0 - kRateSmoothing) +
      window_rate * kRateSmoothing;
  if (consumption_bytes_per_second_ <= 0.0) {
    // Not falling - memory is being returned faster than taken.
    return kInfinite;
  }
  uint64_t above_reserve = free_bytes > reserve ? free_bytes - reserve : 0;
  return double(above_reserve) / consumption_bytes_per_second_;
}

uint64_t GpuMemoryArbiter::TrimConsumers(uint64_t bytes_to_free,
                                         ConsumerKind skip,
                                         bool mid_submission) {
  uint64_t released_total = 0;
  for (const Consumer& consumer : consumers_) {
    if (released_total >= bytes_to_free) {
      break;
    }
    if (consumer.kind == skip) {
      continue;
    }
    if (mid_submission && !consumer.trim_safe_mid_submission) {
      continue;
    }
    if (!consumer.trim) {
      continue;
    }
    if (consumer.usage && !consumer.usage()) {
      continue;
    }
    uint64_t released = consumer.trim(bytes_to_free - released_total);
    if (!released) {
      continue;
    }
    released_total += released;
    // Sub-megabyte releases used to produce a line reading "released 0 MB",
    // 86 of them in one session - noise that says nothing.
    if (released >= (UINT64_C(1) << 20)) {
      XELOGI("GPU memory: released {} MB of {}", released >> 20,
             GetConsumerName(consumer.kind));
    }
  }
  return released_total;
}

bool GpuMemoryArbiter::TryReserveAllocation(ConsumerKind requester,
                                            uint64_t bytes) {
  if (!bytes) {
    return true;
  }
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  // Even if this fits without help, the core has now learned how big this
  // title's allocations are, which is what the reserve is derived from.
  NoteAllocationSize(bytes, now_ms);

  uint64_t free_bytes = QueryFreeHostBytes();
  if (free_bytes == UINT64_MAX) {
    return true;
  }
  last_free_bytes_ = free_bytes;

  // Only this allocation has to fit, plus the margin for the parts of the
  // process the caches do not own. The forward-looking part of the reserve is
  // for the steady-state pressure signal; demanding it here as well would mean
  // refusing an allocation there is room for.
  uint64_t needed = bytes + kUnmanagedMarginBytes;
  if (free_bytes >= needed) {
    return true;
  }

  // The requester's own kind is never asked. It is about to use this memory,
  // and releasing what it already holds to make room for what it is allocating
  // is how a cache ends up destroying and recreating the same thing - the
  // caches have their own idle policies for that, run before they get here.
  //
  // This runs wherever the allocator happens to be, which is in the middle of a
  // submission, so only the consumers that hold nothing the GPU can reach are
  // asked - see RegisterConsumer. Whatever that leaves short is remembered and
  // taken at the next submission boundary, where the render targets and
  // textures can safely be released.
  uint64_t deficit = needed - free_bytes;
  uint64_t released = TrimConsumers(deficit, requester,
                                    /*mid_submission=*/true);
  total_reserved_for_allocations_bytes_ += released;
  if (released < deficit) {
    uint64_t remaining = deficit - released;
    deferred_deficit_bytes_ = std::max(deferred_deficit_bytes_, remaining);
    total_deferred_deficit_bytes_ += remaining;
  }
  if (!released) {
    return false;
  }
  free_bytes = QueryFreeHostBytes();
  if (free_bytes != UINT64_MAX) {
    last_free_bytes_ = free_bytes;
  }
  XELOGI(
      "GPU memory: freed {} MB so {} could allocate {} MB ({} MB free now)",
      released >> 20, GetConsumerName(requester), bytes >> 20,
      free_bytes == UINT64_MAX ? 0 : (free_bytes >> 20));
  return free_bytes == UINT64_MAX || free_bytes >= needed;
}

uint64_t GpuMemoryArbiter::ReportAllocationFailure(ConsumerKind requester,
                                                   uint64_t bytes) {
  ++total_allocation_failures_;
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  NoteAllocationSize(bytes, now_ms);
  allocation_failure_bytes_ = std::max(allocation_failure_bytes_, bytes);
  allocation_failure_polls_ = kFailureEscalationPolls;
  // Straight to critical, without waiting for the next poll: the host just
  // demonstrated that what it reports as free cannot all be used, so every
  // cache should stop growing into it immediately.
  pressure_.store(Pressure::kCritical, std::memory_order_relaxed);
  pressure_relief_polls_ = 0;

  // Free what the retry needs, not a capped bite - a bite smaller than the
  // allocation cannot make it succeed, and failing the same allocation again
  // costs another full driver attempt. Restricted to what is safe to release
  // in the middle of a submission; the rest is taken at the next boundary and
  // the title's next attempt at the same resolve gets it.
  uint64_t released = TrimConsumers(bytes, requester,
                                    /*mid_submission=*/true);
  total_reserved_for_allocations_bytes_ += released;
  if (released < bytes) {
    uint64_t remaining = bytes - released;
    deferred_deficit_bytes_ = std::max(deferred_deficit_bytes_, remaining);
    total_deferred_deficit_bytes_ += remaining;
  }
  uint64_t free_bytes = QueryFreeHostBytes();
  if (free_bytes != UINT64_MAX) {
    last_free_bytes_ = free_bytes;
  }
  if (released) {
    XELOGW(
        "GPU memory: {} could not allocate {} MB with {} MB reported free - "
        "released {} MB from the other caches for a retry (failure {})",
        GetConsumerName(requester), bytes >> 20,
        free_bytes == UINT64_MAX ? 0 : (free_bytes >> 20), released >> 20,
        total_allocation_failures_);
  } else if (total_allocation_failures_ <= 4 ||
             (total_allocation_failures_ % 256) == 0) {
    XELOGW(
        "GPU memory: {} could not allocate {} MB with {} MB reported free and "
        "nothing could be released - the caches hold {} MB, all of it still in "
        "use (failure {})",
        GetConsumerName(requester), bytes >> 20,
        free_bytes == UINT64_MAX ? 0 : (free_bytes >> 20),
        GetTotalUsage() >> 20, total_allocation_failures_);
  }
  return released;
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
  uint64_t reserve = GetReserveBytes();
  if (consumption_bytes_per_second_ > double(kFastConsumptionBytesPerSecond) ||
      allocation_failure_polls_ ||
      (last_free_bytes_ != UINT64_MAX &&
       last_free_bytes_ < reserve + largest_allocation_bytes_)) {
    poll_interval = kFastPollIntervalSubmissions;
  }
  if (submission_index - last_poll_submission_ < poll_interval) {
    return;
  }
  last_poll_submission_ = submission_index;

  uint64_t free_bytes = QueryFreeHostBytes();
  last_free_bytes_ = free_bytes;
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  // Nothing allocated this poll is still an observation: it lets a size the
  // title has stopped needing decay out of the reserve.
  NoteAllocationSize(0, now_ms);
  // And how fast the caches themselves are filling, which is what the reserve
  // has to cover for a title that never makes a large allocation.
  UpdateConsumerDemand(now_ms);

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

  // Where this title lives, and how far the current reading is from it.
  UpdateSettledLevel(free_bytes, now_ms);

  // How long the current rate of consumption leaves before allocations start
  // failing. Reacting to the slope means releases are early, gradual and cheap
  // instead of an emergency.
  double seconds_to_reserve = UpdateTrendAndGetSecondsToReserve(free_bytes);
  // The reserve may have moved with the observation above.
  reserve = GetReserveBytes();

  // Three readings, and the worst of them wins.
  //
  // The trend sees a slow slide coming while releasing is still cheap. The
  // absolute level sees what no rate can - the next single allocation, which at
  // a draw resolution scale is worth many seconds of the average rate, so a
  // comfortable extrapolation is not evidence that the memory is there. And a
  // failed allocation is the only one of the three that is not a prediction at
  // all: it already happened.
  Pressure by_trend;
  if (seconds_to_reserve <= kCriticalSecondsToReserve) {
    by_trend = Pressure::kCritical;
  } else if (seconds_to_reserve <= kElevatedSecondsToReserve) {
    by_trend = Pressure::kElevated;
  } else {
    by_trend = Pressure::kNone;
  }
  // Room for one more allocation of the observed size, but not for two, is what
  // "elevated" means here. Where the title makes no large allocations at all
  // the band falls back to the unmanaged margin, so the level signal never
  // disappears entirely.
  uint64_t elevated_band =
      std::max(std::max(largest_allocation_bytes_, kUnmanagedMarginBytes),
               demand_reserve_bytes_);
  Pressure by_level;
  if (free_bytes <= reserve) {
    by_level = Pressure::kCritical;
  } else if (free_bytes <= reserve + elevated_band) {
    by_level = Pressure::kElevated;
  } else {
    by_level = Pressure::kNone;
  }
  Pressure by_failure = Pressure::kNone;
  if (allocation_failure_polls_) {
    --allocation_failure_polls_;
    by_failure = Pressure::kCritical;
  } else {
    allocation_failure_bytes_ = 0;
  }
  // An allocator that could not be served in the middle of a submission left
  // its shortfall here. This is the safe point it was waiting for.
  Pressure by_deferred =
      deferred_deficit_bytes_ ? Pressure::kCritical : Pressure::kNone;
  Pressure new_pressure;
  if (free_bytes == UINT64_MAX) {
    new_pressure = Pressure::kNone;
  } else {
    new_pressure = std::max(std::max(by_trend, by_level),
                            std::max(by_failure, by_deferred));
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
        "GPU memory: pressure {} -> {} ({} MB free, settles at {} MB, "
        "reserving {} MB, using {} MB/s, {} s of headroom by trend; trend says "
        "{}, level says {}, failures say {})",
        GetPressureName(old_pressure), GetPressureName(new_pressure),
        free_bytes == UINT64_MAX ? 0 : (free_bytes >> 20),
        settled_free_bytes_ == UINT64_MAX ? 0 : (settled_free_bytes_ >> 20),
        reserve >> 20, int64_t(consumption_bytes_per_second_) / (1 << 20),
        seconds_to_reserve >= 1.0e8 ? -1 : int64_t(seconds_to_reserve),
        GetPressureName(by_trend), GetPressureName(by_level),
        GetPressureName(by_failure));
  }

  if (new_pressure == Pressure::kNone) {
    return;
  }

  // MEMORY SITTING FLAT IS NOT A SHORTAGE, however low it sits.
  //
  // A title fills its caches and settles, and where it settles is where its
  // working set fits. Taking memory from a cache in that state does not free
  // anything for long - the game draws the same frame again and loads it all
  // back. This is what the previous core did for twenty minutes in Dark Souls
  // II: 170 forced trims, 5534 MB released, of which 2718 MB were textures and
  // 2216 MB the resource reuse pool, and the cache sizes at the end were within
  // ten megabytes of where they started. The whole 5.4 GB was reloaded.
  //
  // So under elevated pressure - where nothing is urgent - a level that has not
  // moved off where it settles means there is nothing a forced trim can fix.
  // Critical is different: there the reserve is already gone, and something is
  // about to fail whether the level is flat or not.
  if (new_pressure == Pressure::kElevated &&
      settled_free_bytes_ != UINT64_MAX &&
      free_bytes + kFlatBandBytes >= settled_free_bytes_) {
    ++total_flat_skips_;
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
  if (free_bytes < reserve) {
    bytes_to_free += reserve - free_bytes;
  }
  // A recent failure overrides the estimate: that allocation is what the title
  // is actually waiting for, and anything less than its size cannot help. So
  // does a shortfall an allocator could not be served with mid-submission.
  bytes_to_free = std::max(bytes_to_free, allocation_failure_bytes_);
  bytes_to_free = std::max(bytes_to_free, deferred_deficit_bytes_);
  deferred_deficit_bytes_ = 0;
  if (new_pressure == Pressure::kElevated) {
    // Elevated is the smoothing band: give back a little on every pass, well
    // before anything is urgent, so the shortage is met by a series of releases
    // nobody can feel rather than by one that stalls a frame.
    //
    // But "a little" has to be at least what the title is taking, or the band
    // does nothing at all. Black Ops consumed 17 MB/s while passes released a
    // flat 16 MB each, so every pass lost ground and the run went from elevated
    // straight through to running out of memory - the log reads "freed 16 MB,
    // buying back about 0 s at the current rate (caches held 2088 MB, host had
    // 561 MB free)". Zero seconds is the whole story. The floor is a fixed
    // small bite; above it the pass frees what the observed rate will consume
    // before the next one, so the level stops moving instead of merely moving
    // more slowly.
    uint64_t keep_up_bytes = uint64_t(std::max(
        0.0, consumption_bytes_per_second_ * kElevatedCatchUpSeconds));
    bytes_to_free = std::min(
        bytes_to_free, std::max(kElevatedTrimPerPassBytes, keep_up_bytes));
  }
  bytes_to_free = std::clamp(bytes_to_free, kMinTrimBytes, kMaxTrimBytes);
  // Take it in bites, sized by urgency. Reaching the target over several
  // passes a few submissions apart is invisible; reaching it in one is a stall
  // the length of destroying hundreds of resources, and it overshoots - the
  // game reloads what was thrown away.
  //
  // But a bite that is too small for the shortage is worse than either: Black
  // Ops spent a whole session with its texture cache pegged at the hard limit
  // while passes released 16 MB each, so the caches scraped along their
  // ceilings continuously and the game hitched the entire time. Elevated stays
  // gentle - there is time for many small bites. Critical is allowed to
  // actually end the shortage, because recovering properly is what stops the
  // hitching.
  uint64_t per_pass_cap = new_pressure == Pressure::kCritical
                              ? kMaxTrimPerPassCriticalBytes
                              : kMaxTrimPerPassBytes;
  bytes_to_free = std::min(bytes_to_free, per_pass_cap);
  uint64_t total_before = GetTotalUsage();

  // Every consumer may be asked here: nothing has been recorded into the
  // command list yet, so releasing a host resource cannot leave a recorded
  // command pointing at it.
  uint64_t released_total = TrimConsumers(bytes_to_free, ConsumerKind::kCount,
                                          /*mid_submission=*/false);

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
