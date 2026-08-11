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
  //
  // MUST only release resources the GPU has finished with - Update runs before
  // anything is recorded into the command list, but TryReserveAllocation is
  // called from an allocator mid-submission, where a resource referenced by
  // recorded commands would be replayed after being destroyed. Every consumer
  // registered today already checks the completed submission for its own
  // reasons; a new one has to as well.
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

  // Called by a consumer that is about to make a large host allocation, before
  // it makes it. This is the only way the core can know about an allocation
  // that has not happened yet - a rate of consumption cannot predict one, which
  // is what a Dark Souls II session proved when pressure read "none" at 784 MB
  // free with 24 seconds of extrapolated headroom and a single scaled-resolve
  // region then took the lot.
  //
  // Trims other consumers (never the requester's own kind - it is about to use
  // that memory) until the allocation fits alongside the reserve, and returns
  // whether it is now expected to. A false return means the caller should skip
  // the allocation rather than let the driver fail it.
  bool TryReserveAllocation(ConsumerKind requester, uint64_t bytes);

  // Reports that an allocation of this size failed anyway, and releases memory
  // so the caller can retry. This is the single most valuable observation the
  // core can get: the OS reported enough free commit and the allocation still
  // did not fit, so the real ceiling is lower than any query says. One Dark
  // Souls II session at 3x3 failed to create the same 336 MB scaled-resolve
  // region 1287 times while the core, reading ~900 MB free, was busy evicting
  // textures - nothing ever told it that the memory it was looking at could not
  // actually be used. Escalates to critical for the next few polls so the
  // caches stop growing into it as well.
  //
  // Returns how much it freed; zero means there is nothing left to give and the
  // caller should stop asking rather than retry.
  uint64_t ReportAllocationFailure(ConsumerKind requester, uint64_t bytes);

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
  // rate leaves before the reserve, or infinity when memory is not falling.
  double UpdateTrendAndGetSecondsToReserve(uint64_t free_bytes);
  // Moves the settled level towards the current reading - see the member.
  void UpdateSettledLevel(uint64_t free_bytes, uint64_t now_ms);
  // Records a single allocation's size into the decaying maximum the reserve
  // is derived from.
  void NoteAllocationSize(uint64_t bytes, uint64_t now_ms);
  // Walks the consumers in eviction order asking for memory. Returns what was
  // actually released. `skip` is not asked (the caller is about to use that
  // memory itself); pass kCount to ask everyone.
  uint64_t TrimConsumers(uint64_t bytes_to_free, ConsumerKind skip);

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
  // After a trim, growth is left alone for this long. Short because a pass no
  // longer releases much: many small releases spread over time cost nothing
  // visible, while one large one is a stall. The long pause that used to be
  // here existed only because a pass could free hundreds of megabytes at once.
  static constexpr uint64_t kTrimCooldownSubmissions = 16;
  // The most one pass may release. Freeing 563 MB in a single pass - measured
  // in GTA IV, 404 MB of it textures - destroys hundreds of resources on the
  // command processor thread and resets every texture binding, which is felt
  // as a jerk. It also overshoots: the same session then showed memory coming
  // straight back (a NEGATIVE consumption rate), meaning the game reloaded
  // what had just been thrown away. Small bites, taken more often, reach the
  // same place without either.
  static constexpr uint64_t kMaxTrimPerPassBytes = UINT64_C(64) << 20;
  // Under critical pressure a 64 MB bite is too weak to end the shortage, so
  // the caches stay pegged against their ceilings and hitch continuously -
  // worse than one honest pause. Recovering properly is the gentler outcome.
  static constexpr uint64_t kMaxTrimPerPassCriticalBytes = UINT64_C(320) << 20;
  // Even smaller in the elevated band, where nothing is urgent yet - the point
  // there is to meet the shortage with releases nobody can feel.
  static constexpr uint64_t kElevatedTrimPerPassBytes = UINT64_C(16) << 20;

  // WHERE THE DANGER LINE COMES FROM.
  //
  // It is not a constant, and picking a better constant is not the fix. That
  // was tried across three builds on 2026-08-11 and every one of them solved
  // the previous symptom and created the next, because the number has to be
  // two different things at once: Dark Souls II settles at ~900 MB free and is
  // perfectly healthy there, so a 1024 MB threshold left it permanently
  // "elevated" and the core trimmed 170 times, 5534 MB, in a single session -
  // taking textures and the resource reuse pool, the two most expensive things
  // to rebuild, and the game simply loaded them again. Black Ops with the same
  // constant never triggered at all and grew its texture cache to 1836 MB.
  //
  // What actually decides whether free memory is enough is the size of the
  // NEXT allocation, and that is a property of the title (and of the resolution
  // scale), discovered by watching it: at 3x3 a single scaled-resolve region is
  // 356 MB, at 1x1 the largest thing anyone allocates is a few MB. So the
  // reserve is derived from the largest single allocation actually observed,
  // and the only constants left are bounds on that derivation and a margin for
  // the parts of the process this core does not manage at all (the JIT's code
  // cache, guest threads, the driver's own allocations).
  static constexpr uint64_t kUnmanagedMarginBytes = UINT64_C(192) << 20;
  static constexpr uint64_t kMinReserveBytes = UINT64_C(256) << 20;
  // A title that once allocated something enormous must not be left permanently
  // convinced it is out of memory - past this, the trend and the allocation
  // requests carry the load instead.
  static constexpr uint64_t kMaxReserveBytes = UINT64_C(1024) << 20;
  // The observed maximum decays, so a size that one level needed and no other
  // does stops holding the reserve up. Long enough to span a level load.
  static constexpr uint64_t kAllocationObservationMs = 120 * 1000;
  // Start giving memory back when the current rate of consumption would reach
  // the reserve within this long. Chosen so that a release is spread over many
  // frames instead of landing in one.
  static constexpr double kElevatedSecondsToReserve = 20.0;
  static constexpr double kCriticalSecondsToReserve = 6.0;
  // Free enough to buy back this much time at the observed rate, rather than a
  // fixed number of megabytes - the same shortfall means something very
  // different when memory is falling at 5 MB/s and at 200 MB/s.
  static constexpr double kTrimTargetSecondsOfHeadroom = 45.0;
  // Sanity bounds on what a trim may ask for before the per-pass cap applies.
  static constexpr uint64_t kMinTrimBytes = UINT64_C(16) << 20;
  static constexpr uint64_t kMaxTrimBytes = UINT64_C(1024) << 20;

  // Room for the next allocation of the size this title actually makes.
  uint64_t GetReserveBytes() const;
  // The largest single allocation seen recently, which is what the reserve has
  // to cover. Zero until anything reports one.
  uint64_t largest_allocation_bytes_ = 0;
  uint64_t largest_allocation_time_ms_ = 0;

  std::atomic<Pressure> pressure_{Pressure::kNone};
  // Consecutive polls that wanted a lower level - see the hysteresis in
  // Update. Rising is immediate; falling has to be sustained.
  uint32_t pressure_relief_polls_ = 0;
  static constexpr uint32_t kPollsBeforeRelief = 4;
  uint64_t last_poll_submission_ = 0;
  uint64_t last_trim_submission_ = 0;
  uint64_t last_free_bytes_ = UINT64_MAX;

  // THE SETTLED LEVEL: where this title lives.
  //
  // Every title finds an equilibrium - it fills its caches, the host settles at
  // some amount free, and it stays there. That level is not a shortage however
  // low it is; the caches hold exactly what the game is drawing with, and
  // taking any of it away just makes them load it again. What IS a shortage is
  // memory going down and not coming back.
  //
  // So this tracks the level free memory keeps returning to, with a time
  // constant of tens of seconds: a slide leaves the settled level behind (the
  // gap is the evidence), while a title that simply lives low is matched within
  // a minute and the gap closes. Under elevated pressure - where nothing is
  // urgent yet - a closed gap means there is nothing a forced trim can fix, and
  // the pass is skipped. This is what stops the 170-trim session: Dark Souls II
  // sat between 901 and 963 MB free for twenty minutes, which is flat.
  uint64_t settled_free_bytes_ = UINT64_MAX;
  uint64_t settled_time_ms_ = 0;
  // How fast the settled level follows the reading. One time constant per this
  // many seconds - long enough that a real slide registers as a gap, short
  // enough that a title which has genuinely moved to a new level is accepted
  // before the next trim decision matters.
  static constexpr double kSettledLevelTimeConstantSeconds = 45.0;
  // Free memory wobbles by tens of megabytes with ordinary streaming. Anything
  // inside this of the settled level is flat.
  static constexpr uint64_t kFlatBandBytes = UINT64_C(64) << 20;

  // ALLOCATION FAILURE FEEDBACK.
  //
  // When an allocation fails while the OS still reports room for it, that
  // report is wrong for the purpose - fragmentation, the GPU-visible budget, or
  // driver overhead put the real ceiling lower. Nothing else the core can
  // measure says this. So a failure escalates to critical for a few polls and
  // sets the next trim's target to the size that failed, which is the amount
  // the caller's retry needs.
  uint64_t allocation_failure_bytes_ = 0;
  uint32_t allocation_failure_polls_ = 0;
  static constexpr uint32_t kFailureEscalationPolls = 8;
  uint64_t total_allocation_failures_ = 0;

  // For the trend. The rate is smoothed because a single poll straddling a
  // level load reads as hundreds of MB/s and would trigger a panic trim.
  uint64_t last_trend_free_bytes_ = UINT64_MAX;
  uint64_t last_trend_time_ms_ = 0;
  double consumption_bytes_per_second_ = 0.0;
  static constexpr double kRateSmoothing = 0.25;
  // The baseline the rate is measured over. Streaming makes free memory rise
  // and fall constantly; anything shorter than this measures the wobble rather
  // than the direction, which read as 56 MB/s of consumption in a session
  // where memory ended where it started.
  static constexpr double kTrendWindowSeconds = 10.0;
  // Totals since startup, for the periodic report - a trim that keeps
  // happening is a different problem from one that happened once.
  uint64_t total_trims_ = 0;
  uint64_t total_released_bytes_ = 0;
  uint64_t total_freed_immediately_bytes_ = 0;
  uint64_t total_reserved_for_allocations_bytes_ = 0;
  // Passes skipped because the level was flat - the difference between "the
  // core has nothing to do" and "the core is not being called".
  uint64_t total_flat_skips_ = 0;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GPU_MEMORY_ARBITER_H_
