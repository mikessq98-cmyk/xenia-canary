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

#include <cstdint>
#include <functional>
#include <vector>

namespace xe {
namespace gpu {

// Decides, for the whole GPU backend, what to give back when host memory runs
// short.
//
// Before this existed, five caches each polled the OS and each reacted at its
// own threshold - upload pools at 768 MB free, render targets at 640, shader
// translations at 512, scaled resolve regions at 384, while the texture cache
// watched only its own size and never looked at the host at all. None of them
// knew what the others were doing, so under pressure they took turns: one
// released memory and the next immediately claimed it, which is what made a
// title that ran well for a minute judder after half an hour.
//
// The arbiter replaces that with one poll, one budget and one eviction order.
// Consumers register with the cost of getting the memory back if it is taken
// away, and are asked to trim from cheapest to most expensive - so shader
// bytecode (retranslatable in milliseconds) goes long before textures (a guest
// memory read plus format conversion) or render targets holding EDRAM data.
class GpuMemoryArbiter {
 public:
  // Ordered cheapest-to-restore first: this IS the eviction order.
  enum class ConsumerKind {
    // Translated shader bytecode - regenerated from the guest microcode.
    kShaderBytecode,
    // Upload/descriptor pool pages - pure scratch.
    kUploadPools,
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

  void RegisterConsumer(ConsumerKind kind, UsageFunction usage,
                        TrimFunction trim);
  void UnregisterConsumers();

  // Called once per submission from the command processor. Polls the host at
  // most every kPollIntervalSubmissions submissions (the OS query is not free
  // and its answer does not change meaningfully between draws) and trims when
  // below the target.
  void Update(uint64_t submission_index);

  // Total host memory currently held by all registered consumers.
  uint64_t GetTotalUsage() const;

  // Free host commit as of the last poll, or UINT64_MAX if never polled.
  uint64_t last_free_bytes() const { return last_free_bytes_; }

 private:
  struct Consumer {
    ConsumerKind kind = ConsumerKind::kCount;
    UsageFunction usage;
    TrimFunction trim;
  };

  static const char* GetConsumerName(ConsumerKind kind);
  // Reads free host commit (the binding limit on the console - allocations
  // fail on the commit charge, not on free physical RAM).
  static uint64_t QueryFreeHostBytes();

  std::vector<Consumer> consumers_;

  // How often the host is polled. Everything below is in submissions.
  static constexpr uint64_t kPollIntervalSubmissions = 32;
  // After a trim, growth is left alone for this long. Without the pause the
  // caches simply re-fill and the arbiter trims again a few submissions later,
  // which is the oscillation this class exists to stop.
  static constexpr uint64_t kTrimCooldownSubmissions = 240;

  // Trim when free host commit falls below this, and free enough to get back
  // above it plus the headroom below - a target rather than a threshold, so a
  // single pass ends the pressure instead of nibbling at it every poll.
  static constexpr uint64_t kTargetFreeBytes = UINT64_C(768) << 20;
  static constexpr uint64_t kTrimHeadroomBytes = UINT64_C(192) << 20;

  uint64_t last_poll_submission_ = 0;
  uint64_t last_trim_submission_ = 0;
  uint64_t last_free_bytes_ = UINT64_MAX;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GPU_MEMORY_ARBITER_H_
