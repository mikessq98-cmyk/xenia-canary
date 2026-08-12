/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_PIPELINE_BLOB_CACHE_H_
#define XENIA_GPU_D3D12_PIPELINE_BLOB_CACHE_H_

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace ui {
namespace d3d12 {
class D3D12Provider;
}  // namespace d3d12
}  // namespace ui

namespace gpu {
namespace d3d12 {

// Keeps what the DRIVER produced when it compiled a pipeline, so the next
// launch does not have to compile it again.
//
// The shader storage already records which pipelines a title needs, and the
// blocking prewarm creates all of them before the game starts - that part
// works. What it costs is the compilation itself: measured on the Xbox UWP
// driver at 6-8 pipelines per second, so a title with 587 of them pays 75-90
// SECONDS of startup on every single launch, every time, for work that is
// bit-for-bit identical to the last launch's.
//
// D3D12 offers two ways to keep that work. ID3D12PipelineLibrary is the
// obvious one and it CRASHES this console's driver, even behind the
// D3D12_FEATURE_SHADER_CACHE support query - which is why it is off. This is
// the other one: D3D12_GRAPHICS_PIPELINE_STATE_DESC::CachedPSO, a blob per
// pipeline that the driver hands back from
// ID3D12PipelineState::GetCachedBlob(). It is a different code path with no
// library container object at all, and its failure model is defined rather
// than fatal - a blob the runtime rejects makes creation return an error, and
// the caller simply creates the pipeline again without it.
//
// Everything here is per title AND per resolution scale, like the toxic
// solver: shaders are translated differently when scaling is active, so the
// pipelines - and therefore the blobs - are not the same objects at all.
class PipelineBlobCache {
 public:
  PipelineBlobCache() = default;
  PipelineBlobCache(const PipelineBlobCache&) = delete;
  PipelineBlobCache& operator=(const PipelineBlobCache&) = delete;
  ~PipelineBlobCache();

  // `storage_root` is the shader storage directory; the blobs go in its
  // `local` subdirectory, which the storage layout already reserves for
  // "files specific to this machine/driver" and which nothing wrote until now.
  void Initialize(const std::filesystem::path& storage_root, uint32_t title_id,
                  const ui::d3d12::D3D12Provider& provider);
  void Shutdown(bool clean_exit);

  bool enabled() const { return enabled_; }

  // Points desc.CachedPSO at a stored blob for this pipeline, if there is one
  // and it has not already been rejected. Returns whether it did - the caller
  // needs to know, because a creation that used a blob must be retried without
  // one if it fails, and must not have its own output stored.
  bool ApplyTo(uint64_t description_hash,
               D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
  // Creation failed with the blob applied. The blob is dropped for the rest of
  // the run and rewritten out of the file at shutdown.
  void Discard(uint64_t description_hash);
  // A pipeline the driver compiled from scratch. Asks it for the blob and
  // appends it. Cheap enough to do inline: the alternative is compiling the
  // same pipeline again on the next launch.
  void Store(uint64_t description_hash, ID3D12PipelineState* state);
  // The blobs are only useful while pipelines are being created. Once the
  // prewarm is over they are tens of megabytes of nothing - dropped, and any
  // pipeline created later simply compiles as it always did.
  void ReleaseLoadedBlobs();

  // One line for the periodic memory report.
  std::string GetReport() const;

 private:
  // 'XEPB'. Bumped when the entry layout changes.
  static constexpr uint32_t kMagic = 0x42504558;
  static constexpr uint32_t kVersion = 1;

  struct FileHeader {
    uint32_t magic;
    uint32_t version;
    // The blobs are only valid for the adapter and driver that produced them.
    // D3D12 checks this itself and returns D3D12_ERROR_ADAPTER_NOT_FOUND or
    // D3D12_ERROR_DRIVER_VERSION_MISMATCH, but per pipeline - checking it once
    // here throws the whole stale file away in one go instead of paying a
    // failed creation and a retry for every one of several hundred.
    uint64_t adapter_luid;
    uint64_t driver_version;
    uint64_t reserved;
  };
  struct EntryHeader {
    uint64_t description_hash;
    uint32_t blob_size;
    uint32_t reserved;
  };

  // Reads the file, discarding it whole if the header does not match this
  // adapter and driver. Returns whether anything usable was loaded.
  bool LoadFile();
  void RewriteFileWithoutDiscarded();

  bool enabled_ = false;
  std::filesystem::path blob_path_;
  // Written before the first blob is fed to the driver and deleted once the
  // prewarm is through. Present at startup it means the last run died while
  // feeding blobs - so the blobs are what killed it, and the file goes. The
  // toxic solver learned this shape first and it is the right one for anything
  // that hands the driver data it might not survive.
  std::filesystem::path feeding_marker_path_;
  bool marker_written_ = false;

  uint64_t adapter_luid_ = 0;
  uint64_t driver_version_ = 0;

  mutable std::mutex mutex_;
  // Loaded from disk; emptied by ReleaseLoadedBlobs.
  std::unordered_map<uint64_t, std::vector<uint8_t>> loaded_blobs_;
  // Hashes whose stored blob the runtime refused. Never fed again, and dropped
  // from the file at shutdown - by reading it back, not by keeping every blob
  // in memory all session for the sake of one rewrite that usually never
  // happens.
  std::unordered_set<uint64_t> discarded_;
  std::FILE* file_ = nullptr;

  uint64_t loaded_count_ = 0;
  uint64_t loaded_bytes_ = 0;
  std::atomic<uint64_t> hit_count_{0};
  std::atomic<uint64_t> miss_count_{0};
  std::atomic<uint64_t> rejected_count_{0};
  std::atomic<uint64_t> stored_count_{0};
  std::atomic<uint64_t> stored_bytes_{0};
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_PIPELINE_BLOB_CACHE_H_
