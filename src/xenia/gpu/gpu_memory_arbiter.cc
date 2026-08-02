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
                                        TrimFunction trim) {
  Consumer consumer;
  consumer.kind = kind;
  consumer.usage = std::move(usage);
  consumer.trim = std::move(trim);
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

void GpuMemoryArbiter::Update(uint64_t submission_index) {
  if (consumers_.empty()) {
    return;
  }
  if (submission_index - last_poll_submission_ < kPollIntervalSubmissions) {
    return;
  }
  last_poll_submission_ = submission_index;

  uint64_t free_bytes = QueryFreeHostBytes();
  last_free_bytes_ = free_bytes;
  if (free_bytes == UINT64_MAX || free_bytes >= kTargetFreeBytes) {
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

  uint64_t bytes_to_free = kTargetFreeBytes - free_bytes + kTrimHeadroomBytes;
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
  if (released_total) {
    XELOGI(
        "GPU memory: freed {} MB total to get back to {} MB of headroom "
        "(caches held {} MB, host had {} MB free)",
        released_total >> 20, kTargetFreeBytes >> 20, total_before >> 20,
        free_bytes >> 20);
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
