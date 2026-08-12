/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/pipeline_blob_cache.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/xxhash.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

DEFINE_bool(
    d3d12_pipeline_blob_cache, true,
    "Keep what the driver produced when it compiled each pipeline, next to the "
    "shader storage, and hand it back on the next launch so it does not have "
    "to compile it again.\n"
    "The shader storage already knows WHICH pipelines a title needs and the "
    "prewarm creates all of them before the game starts - what that costs is "
    "the compilation, measured at 6-8 pipelines per second on this driver, so "
    "a title with 587 of them pays 75-90 seconds of startup every single "
    "launch for work identical to the last one's.\n"
    "This is D3D12's per-pipeline cached blob, NOT ID3D12PipelineLibrary (see "
    "d3d12_pipeline_library, which crashes this console's driver): there is no "
    "library object, one bad blob affects one pipeline instead of all of them, "
    "and a blob the runtime refuses simply makes that pipeline compile "
    "normally. A run that dies while feeding blobs throws the whole file away "
    "on the next launch.",
    "D3D12");
DEFINE_int32(
    d3d12_pipeline_blob_cache_max_mb, 256,
    "Ceiling on the pipeline blob cache file. Reached, no further blobs are "
    "kept - the pipelines that already have one still load instantly.",
    "D3D12");

DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);

namespace xe {
namespace gpu {
namespace d3d12 {

PipelineBlobCache::~PipelineBlobCache() {
  if (enabled_) {
    // Reaching the destructor first means nothing shut this down normally.
    Shutdown(/*clean_exit=*/false);
  }
}

void PipelineBlobCache::Initialize(const std::filesystem::path& storage_root,
                                   uint32_t title_id,
                                   const ui::d3d12::D3D12Provider& provider) {
  enabled_ = false;
  marker_written_ = false;
  loaded_blobs_.clear();
  discarded_.clear();
  loaded_count_ = 0;
  loaded_bytes_ = 0;
  hit_count_.store(0, std::memory_order_relaxed);
  miss_count_.store(0, std::memory_order_relaxed);
  rejected_count_.store(0, std::memory_order_relaxed);
  stored_count_.store(0, std::memory_order_relaxed);
  stored_bytes_.store(0, std::memory_order_relaxed);
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  if (!cvars::d3d12_pipeline_blob_cache) {
    return;
  }

  // Identity of what produced the blobs, as far as the provider exposes it:
  // the adapter description string and its vendor. This is only a shortcut -
  // D3D12 validates a blob against the real adapter and driver itself and
  // returns D3D12_ERROR_ADAPTER_NOT_FOUND / D3D12_ERROR_DRIVER_VERSION_MISMATCH,
  // which the creation path already handles by retrying without it. Checking
  // here first just throws a whole stale file away in one go instead of paying
  // a failed creation and a retry for each of several hundred pipelines.
  const std::string& adapter_description = provider.GetAdapterDescription();
  adapter_luid_ =
      XXH3_64bits(adapter_description.data(), adapter_description.size());
  driver_version_ = uint64_t(provider.GetAdapterVendorID());

  std::error_code ec;
  // The storage layout already reserves this directory for exactly this -
  // "files specific to this machine/driver" - and nothing has ever written it.
  std::filesystem::path local_root = storage_root / "local";
  std::filesystem::create_directories(local_root, ec);

  std::string scale_suffix;
  if (cvars::draw_resolution_scale_x > 1 || cvars::draw_resolution_scale_y > 1) {
    scale_suffix = fmt::format(".{}x{}", cvars::draw_resolution_scale_x,
                               cvars::draw_resolution_scale_y);
  }
  blob_path_ = local_root / fmt::format("{:08X}.d3d12{}.psoblob", title_id,
                                        scale_suffix);
  feeding_marker_path_ = local_root / fmt::format("{:08X}.d3d12{}.psoblob.using",
                                                  title_id, scale_suffix);

  // A marker left behind means the last run died with blobs in flight. They
  // are the prime suspect for that, and there is no way to tell which one, so
  // the file goes as a whole. The pipelines simply compile this run.
  if (std::filesystem::exists(feeding_marker_path_, ec)) {
    std::filesystem::remove(blob_path_, ec);
    std::filesystem::remove(feeding_marker_path_, ec);
    XELOGW(
        "Pipeline blob cache: the last run died while handing blobs to the "
        "driver - the cache has been thrown away and this run will compile "
        "pipelines normally. If this repeats, set d3d12_pipeline_blob_cache "
        "to false; the driver does not tolerate them.");
  } else {
    LoadFile();
  }

  file_ = xe::filesystem::OpenFile(blob_path_, "a+b");
  if (!file_) {
    XELOGW("Pipeline blob cache: couldn't open {} - blobs will not be kept",
           xe::path_to_utf8(blob_path_));
  } else if (!loaded_count_) {
    // A brand new file needs its header before anything is appended to it.
    xe::filesystem::Seek(file_, 0, SEEK_END);
    if (xe::filesystem::Tell(file_) == 0) {
      FileHeader header = {};
      header.magic = kMagic;
      header.version = kVersion;
      header.adapter_luid = adapter_luid_;
      header.driver_version = driver_version_;
      std::fwrite(&header, sizeof(header), 1, file_);
    }
  }

  enabled_ = true;
  XELOGI(
      "Pipeline blob cache: {} pipeline(s) already compiled, {} MB - those "
      "will not be compiled again this launch",
      loaded_count_, loaded_bytes_ >> 20);
}

bool PipelineBlobCache::LoadFile() {
  std::FILE* file = xe::filesystem::OpenFile(blob_path_, "rb");
  if (!file) {
    return false;
  }
  FileHeader header = {};
  bool usable = std::fread(&header, sizeof(header), 1, file) == 1 &&
                header.magic == kMagic && header.version == kVersion &&
                header.adapter_luid == adapter_luid_ &&
                header.driver_version == driver_version_;
  if (!usable) {
    std::fclose(file);
    // Not an error - a driver update invalidates every blob in it, which is
    // exactly what the header is for. Throwing it away once beats paying a
    // failed creation and a retry for each of several hundred pipelines.
    std::error_code ec;
    std::filesystem::remove(blob_path_, ec);
    XELOGI(
        "Pipeline blob cache: the stored blobs were made by a different "
        "adapter or driver - starting a new cache");
    return false;
  }
  uint64_t total_bytes = 0;
  for (;;) {
    EntryHeader entry = {};
    if (std::fread(&entry, sizeof(entry), 1, file) != 1) {
      break;
    }
    if (!entry.blob_size ||
        entry.blob_size > (uint32_t(64) << 20)) {
      // Truncated or corrupt - everything after this point is unreadable too.
      break;
    }
    std::vector<uint8_t> blob(entry.blob_size);
    if (std::fread(blob.data(), 1, blob.size(), file) != blob.size()) {
      break;
    }
    total_bytes += blob.size();
    // A later entry for the same pipeline wins - the file is append-only, so
    // the last one written is the most recent.
    loaded_blobs_[entry.description_hash] = std::move(blob);
  }
  std::fclose(file);
  loaded_count_ = loaded_blobs_.size();
  loaded_bytes_ = total_bytes;
  return loaded_count_ != 0;
}

bool PipelineBlobCache::ApplyTo(uint64_t description_hash,
                                D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
  if (!enabled_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (loaded_blobs_.empty() || discarded_.count(description_hash)) {
    miss_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  auto it = loaded_blobs_.find(description_hash);
  if (it == loaded_blobs_.end()) {
    miss_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Arm the marker the first time a blob actually goes to the driver, so a
  // death from here on is attributed to the blobs and the file is dropped next
  // launch. Deleted again by ReleaseLoadedBlobs once the prewarm survived.
  if (!marker_written_) {
    marker_written_ = true;
    std::FILE* marker = xe::filesystem::OpenFile(feeding_marker_path_, "wb");
    if (marker) {
      std::fclose(marker);
    }
  }
  desc.CachedPSO.pCachedBlob = it->second.data();
  desc.CachedPSO.CachedBlobSizeInBytes = it->second.size();
  hit_count_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void PipelineBlobCache::Discard(uint64_t description_hash) {
  if (!enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_blobs_.erase(description_hash);
  discarded_.insert(description_hash);
  rejected_count_.fetch_add(1, std::memory_order_relaxed);
}

void PipelineBlobCache::Store(uint64_t description_hash,
                              ID3D12PipelineState* state) {
  if (!enabled_ || !state || !file_) {
    return;
  }
  if (uint64_t(loaded_bytes_) + stored_bytes_.load(std::memory_order_relaxed) >=
      (uint64_t(std::max(cvars::d3d12_pipeline_blob_cache_max_mb, 0)) << 20)) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  if (FAILED(state->GetCachedBlob(&blob)) || !blob || !blob->GetBufferSize()) {
    return;
  }
  EntryHeader entry = {};
  entry.description_hash = description_hash;
  entry.blob_size = uint32_t(blob->GetBufferSize());
  std::lock_guard<std::mutex> lock(mutex_);
  // Appended as it is learned, not written at shutdown: a run that ends in a
  // device loss or an out-of-memory death must still keep everything it
  // compiled beforehand, or the next launch compiles the whole game again.
  std::fwrite(&entry, sizeof(entry), 1, file_);
  std::fwrite(blob->GetBufferPointer(), 1, entry.blob_size, file_);
  std::fflush(file_);
  stored_count_.fetch_add(1, std::memory_order_relaxed);
  stored_bytes_.fetch_add(entry.blob_size, std::memory_order_relaxed);
  // Deliberately NOT kept in memory. Holding every blob for a possible rewrite
  // at shutdown would mean carrying up to the whole cache - hundreds of
  // megabytes - for the entire session, on a budget where that is the thing
  // this project has spent its time defending. The rewrite reads the file back
  // instead; it happens once, at exit, and only if something was rejected.
}

void PipelineBlobCache::ReleaseLoadedBlobs() {
  if (!enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_blobs_.clear();
  // The blobs survived the prewarm, so they did not kill the run and the
  // marker must not make the next launch throw them away.
  if (marker_written_) {
    marker_written_ = false;
    std::error_code ec;
    std::filesystem::remove(feeding_marker_path_, ec);
  }
}

void PipelineBlobCache::RewriteFileWithoutDiscarded() {
  // Only worth the rewrite when something was actually rejected - otherwise
  // the append-only file is already exactly what it should be. Reads the file
  // back rather than keeping every blob in memory all session for this.
  if (discarded_.empty()) {
    return;
  }
  std::FILE* source = xe::filesystem::OpenFile(blob_path_, "rb");
  if (!source) {
    return;
  }
  std::filesystem::path temp_path = blob_path_;
  temp_path += ".rewrite";
  std::FILE* target = xe::filesystem::OpenFile(temp_path, "wb");
  if (!target) {
    std::fclose(source);
    return;
  }
  FileHeader header = {};
  bool ok = std::fread(&header, sizeof(header), 1, source) == 1 &&
            header.magic == kMagic && header.version == kVersion;
  if (ok) {
    std::fwrite(&header, sizeof(header), 1, target);
    std::vector<uint8_t> blob;
    for (;;) {
      EntryHeader entry = {};
      if (std::fread(&entry, sizeof(entry), 1, source) != 1) {
        break;
      }
      if (!entry.blob_size || entry.blob_size > (uint32_t(64) << 20)) {
        break;
      }
      blob.resize(entry.blob_size);
      if (std::fread(blob.data(), 1, blob.size(), source) != blob.size()) {
        break;
      }
      if (discarded_.count(entry.description_hash)) {
        continue;
      }
      std::fwrite(&entry, sizeof(entry), 1, target);
      std::fwrite(blob.data(), 1, blob.size(), target);
    }
  }
  std::fclose(source);
  std::fclose(target);
  std::error_code ec;
  if (ok) {
    std::filesystem::rename(temp_path, blob_path_, ec);
  }
  std::filesystem::remove(temp_path, ec);
}

void PipelineBlobCache::Shutdown(bool clean_exit) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  if (clean_exit) {
    RewriteFileWithoutDiscarded();
  }
  // Whether the exit was clean or not, surviving to shutdown at all means the
  // blobs did not take the process down.
  if (marker_written_) {
    marker_written_ = false;
    std::error_code ec;
    std::filesystem::remove(feeding_marker_path_, ec);
  }
  loaded_blobs_.clear();
  discarded_.clear();
  enabled_ = false;
}

std::string PipelineBlobCache::GetReport() const {
  if (!cvars::d3d12_pipeline_blob_cache) {
    return std::string();
  }
  uint64_t hits = hit_count_.load(std::memory_order_relaxed);
  uint64_t misses = miss_count_.load(std::memory_order_relaxed);
  uint64_t rejected = rejected_count_.load(std::memory_order_relaxed);
  uint64_t stored = stored_count_.load(std::memory_order_relaxed);
  if (!hits && !misses && !stored) {
    return std::string();
  }
  return fmt::format(
      "{} pipelines loaded from the driver's own output ({} rejected), {} "
      "compiled and kept ({} MB), {} had nothing stored",
      hits - rejected, rejected, stored,
      stored_bytes_.load(std::memory_order_relaxed) >> 20, misses);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
