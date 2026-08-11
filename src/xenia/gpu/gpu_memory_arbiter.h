/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GPU_MEMORY_ARBITER_H_
#define XENIA_GPU_GPU_MEMORY_ARBITER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace xe {
namespace gpu {

// The single owner of "how much host memory may the GPU caches hold, and what
// is given back first when they may not".
//
// Before this existed, five caches each polled the OS and each reacted at its
// own threshold - upload pools at 768 MB free, render targets at 640, shader
// translations at 512, scaled resolve regions at 384, while the texture cache
// watched only its own size and never looked at the host at all. None of them
// knew what the others were doing, so under pressure they took turns: one
// released memory and the next immediately claimed it, which is what made a
// title that ran well for a minute judder after half an hour.
//
// Consolidating the TRIMMING was only half of it. The thresholds stayed
// scattered - the arbiter trimmed below one number, the texture cache was told
// it was "under pressure" below another, its own soft and hard limits were two
// more, and the resource reuse pool had a fifth - so the caches still
// contradicted each other: 438 evictions in one session while 1.3-2.8 GB sat
// free, because a cache hit its own ceiling long before the host was short of
// anything.
//
// So this owns the thresholds too. A cache never compares against a number of
// its own; it asks what the pressure level is, and that answer comes from one
// poll of the host against one set of limits.
class GpuMemoryArbiter {
 public:
  // How badly the HOST needs memory back - which is a different question from
  // whether any one cache happens to be large.
  enum class Pressure {
    // Plenty free. Caches keep everything they hold, except what has gone
    // unused long enough to be worth nothing (their own idle policy).
    kNone,
    // Getting tight. Caches honour their soft size limits: give back what has
    // aged out, gradually, before anything has to be taken by force.
    kElevated,
    // Short. Everything that can be released is released, oldest first, down
    // to what the current frame is actually drawing with.
    kCritical,
  };

  // Ordered cheapest-to-restore first: this IS the eviction order.
  enum class ConsumerKind {
    // Translated shader bytecode - regenerated from the guest microcode.
    kShaderBytecode,
    // Upload/descriptor pool pages - pure scratch.
    kUploadPools,
    // Host resources kept only so a future texture can reuse them instead of
    // paying the driver's creation cost - by definition not in use.
    kResourceReusePool,
    // Scaled resolve regions the game has stopped resolving into.
    kIdleScaledResolve,
    // Render targets that hold no EDRAM data.
    kUnusedRenderTargets,
    // Cached guest textures, least recently used first.
    kTextures,
    // Render targets that still own EDRAM data - a surface may come back
    // empty for a frame, so this is the last resort.
    kOwningRenderTargets,

    kCount,
  };

  // Asked to release at least `bytes_to_free`; returns what it actually
  // released. Returning less than asked is normal and simply moves the
  // arbiter on to the next consumer.
  using TrimFunction = std::function<uint64_t(uint64_t bytes_to_free)>;
  // Current host memory held by this consumer, for reporting and for deciding
  // whether asking it is worthwhile at all.
  using UsageFunction = std::function<uint64_t()>;
  // Releases what this consumer KNOWS is no longer needed - not "least
  // recently used", but genuinely dead: shader bytecode whose pipelines are
  // built, resources nothing can reuse, regions the game stopped resolving
  // into. Called on every poll regardless of pressure, because keeping
  // something that cannot be needed buys nothing and only makes the eventual
  // shortage arrive sooner. Returns bytes freed; may be null.
  using FreeUnneededFunction = std::function<uint64_t()>;

  void RegisterConsumer(ConsumerKind kind, UsageFunction usage,
                        TrimFunction trim,
                        FreeUnneededFunction free_unneeded = nullptr);
  void UnregisterConsumers();

  // Called once per submission from the command processor. Polls the host at
  // most every kPollIntervalSubmissions submissions (the OS query is not free
  // and its answer does not change meaningfully between draws), updates the
  // pressure level, and trims when below the target.
  void Update(uint64_t submission_index);

  // The current answer to "does the host need memory back". Safe to read from
  // any thread; caches consult this instead of thresholds of their own.
  Pressure pressure() const {
    return pressure_.load(std::memory_order_relaxed);
  }
  // Convenience for the common check - a cache's soft size limit only applies
  // once the host is actually short.
  bool IsUnderPressure() const { return pressure() != Pressure::kNone; }

  // Total host memory currently held by all registered consumers.
  uint64_t GetTotalUsage() const;

  // Free host commit as of the last poll, or UINT64_MAX if never polled.
  uint64_t last_free_bytes() const { return last_free_bytes_; }

  // One line per consumer: what each holds and what the pressure is. Called
  // from the periodic memory report, so the split is visible without having to
  // correlate several separate log lines.
  void LogStatistics() const;

  static const char* GetPressureName(Pressure pressure);

 private:
  struct Consumer {
    ConsumerKind kind = ConsumerKind::kCount;
    UsageFunction usage;
    TrimFunction trim;
    FreeUnneededFunction free_unneeded;
  };

  // Updates consumption_bytes_per_second_ and returns how long the current
  // rate leaves before the floor, or infinity when memory is not falling.
  double UpdateTrendAndGetSecondsToFloor(uint64_t free_bytes);

  static const char* GetConsumerName(ConsumerKind kind);
  // Reads free host commit (the binding limit on the console - allocations
  // fail on the commit charge, not on free physical RAM).
  static uint64_t QueryFreeHostBytes();

  std::vector<Consumer> consumers_;

  // How often the host is polled. Everything below is in submissions.
  static constexpr uint64_t kPollIntervalSubmissions = 32;
  // Used while memory is moving fast or headroom is already small - see
  // Update. A level load can take gigabytes between two ordinary polls.
  static constexpr uint64_t kFastPollIntervalSubmissions = 4;
  static constexpr uint64_t kFastConsumptionBytesPerSecond = UINT64_C(64) << 20;
  // After a trim, growth is left alone for this long. Without the pause the
  // caches simply re-fill and the arbiter trims again a few submissions later,
  // which is the oscillation this class exists to stop.
  static constexpr uint64_t kTrimCooldownSubmissions = 240;

  // Fixed thresholds are the wrong instrument and were removed. A number like
  // "trim below 768 MB free" is either too early - throwing away work while
  // gigabytes sit unused, which is what produced 438 evictions in one session -
  // or too late, because what matters is not how much is free right now but
  // whether it will still be enough by the time the scene finishes streaming.
  //
  // What is used instead:
  //  - Anything that is genuinely not needed is released as soon as that is
  //    known, at no threshold at all (see kFreeImmediately consumers). Nothing
  //    is kept just because there is room for it.
  //  - Everything else is judged by TREND: how fast free memory is falling and
  //    how long that leaves. Reacting to the slope means the release happens
  //    early enough to be gradual and cheap, rather than as an emergency.
  //
  // Trend alone is NOT enough, and a Dark Souls II session at 3x3 proved it:
  // pressure went to none at 784 MB free because consumption had slowed to
  // 16 MB/s, which extrapolated to 24 seconds of headroom - and then a single
  // scaled-resolve region took the lot at once. The log after that is 1010
  // failed upload buffers, 1290 dropped draws, and the shader compiler
  // crashing out of memory. A rate says nothing about an allocation that has
  // not happened yet, and at a draw resolution scale the individual
  // allocations are enormous.
  //
  // So the level is the WORSE of two readings: what the trend predicts, and
  // where the free memory is in absolute terms. The trend catches a slow
  // slide early, when releasing is cheap; the absolute levels catch a step
  // that no rate could have foreseen. Neither replaces the other.
  static constexpr uint64_t kElevatedFreeBytes = UINT64_C(1024) << 20;
  static constexpr uint64_t kCriticalFreeBytes = UINT64_C(640) << 20;
  // Below this allocations are already failing - nothing left to predict.
  static constexpr uint64_t kFloorFreeBytes = UINT64_C(384) << 20;
  // Start giving memory back when the current rate of consumption would reach
  // the floor within this long. Chosen so that a release is spread over many
  // frames instead of landing in one.
  static constexpr double kElevatedSecondsToFloor = 20.0;
  static constexpr double kCriticalSecondsToFloor = 6.0;
  // Free enough to buy back this much time at the observed rate, rather than a
  // fixed number of megabytes - the same shortfall means something very
  // different when memory is falling at 5 MB/s and at 200 MB/s.
  static constexpr double kTrimTargetSecondsOfHeadroom = 45.0;
  // Sanity bounds on any single trim, so a wild rate estimate (a one-off
  // allocation spike, a stalled poll) cannot ask for absurd amounts.
  static constexpr uint64_t kMinTrimBytes = UINT64_C(64) << 20;
  static constexpr uint64_t kMaxTrimBytes = UINT64_C(1024) << 20;

  std::atomic<Pressure> pressure_{Pressure::kNone};
  uint64_t last_poll_submission_ = 0;
  uint64_t last_trim_submission_ = 0;
  uint64_t last_free_bytes_ = UINT64_MAX;
  // For the trend. The rate is smoothed because a single poll straddling a
  // level load reads as hundreds of MB/s and would trigger a panic trim.
  uint64_t last_trend_free_bytes_ = UINT64_MAX;
  uint64_t last_trend_time_ms_ = 0;
  double consumption_bytes_per_second_ = 0.0;
  static constexpr double kRateSmoothing = 0.25;
  // Totals since startup, for the periodic report - a trim that keeps
  // happening is a different problem from one that happened once.
  uint64_t total_trims_ = 0;
  uint64_t total_released_bytes_ = 0;
  uint64_t total_freed_immediately_bytes_ = 0;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GPU_MEMORY_ARBITER_H_
