/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SHADER_STORAGE_H_
#define XENIA_GPU_SHADER_STORAGE_H_

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Shader storage file format (.xsh) - shared between D3D12 and Vulkan backends.
// Stores guest shader microcode (Xbox 360 bytecode) for persistent caching.

// File magic: 'XESH'
constexpr uint32_t kShaderStorageMagic = 0x48534558;

// Header for each stored shader entry.
XEPACKEDSTRUCT(ShaderStoredHeader, {
  uint64_t ucode_data_hash;

  uint32_t ucode_dword_count : 31;
  xenos::ShaderType type : 1;

  // Increment when the format changes in an incompatible way.
  static constexpr uint32_t kVersion = 0x20201219;
});

// Pipeline storage file format (.xpso) - API-specific but with shared header.
// File magic: 'XEPS'
constexpr uint32_t kPipelineStorageMagic = 0x53504558;

// Storage directory structure:
//   <cache_root>/shaders/
//     shareable/           - Files shared between different machines/drivers
//       {TITLE_ID}.xsh     - Shader microcode (shared between D3D12/Vulkan)
//       {TITLE_ID}.*.xpso  - Pipeline descriptions (API-specific)
//     local/               - Files specific to this machine/driver
//       {TITLE_ID}.*.bin   - Driver-specific pipeline cache

inline std::filesystem::path GetShaderStorageRoot(
    const std::filesystem::path& cache_root) {
  return cache_root / "shaders";
}

inline std::filesystem::path GetShaderStorageShareableRoot(
    const std::filesystem::path& cache_root) {
  return GetShaderStorageRoot(cache_root) / "shareable";
}

inline std::filesystem::path GetShaderStorageLocalRoot(
    const std::filesystem::path& cache_root) {
  return GetShaderStorageRoot(cache_root) / "local";
}

inline std::filesystem::path GetShaderStorageFilePath(
    const std::filesystem::path& cache_root, uint32_t title_id) {
  return GetShaderStorageShareableRoot(cache_root) /
         fmt::format("{:08X}.xsh", title_id);
}

// Ensures shader storage directories exist. Returns true on success.
inline bool EnsureShaderStorageDirectoriesExist(
    const std::filesystem::path& cache_root) {
  std::error_code error_code;
  auto shareable_root = GetShaderStorageShareableRoot(cache_root);
  if (!std::filesystem::exists(shareable_root)) {
    if (!std::filesystem::create_directories(shareable_root, error_code)) {
      XELOGE(
          "Failed to create shader storage directory, persistent shader "
          "storage will be disabled: {}",
          xe::path_to_utf8(shareable_root));
      return false;
    }
  }
  return true;
}

// File header for shader storage (.xsh).
XEPACKEDSTRUCT(ShaderStorageFileHeader, {
  uint32_t magic;
  uint32_t version_swapped;
});

// Validates the shader storage file header. Returns true if valid.
inline bool ValidateShaderStorageHeader(FILE* file,
                                        ShaderStorageFileHeader& header_out) {
  if (!fread(&header_out, sizeof(header_out), 1, file)) {
    return false;
  }
  return header_out.magic == kShaderStorageMagic &&
         xe::byte_swap(header_out.version_swapped) ==
             ShaderStoredHeader::kVersion;
}

// Writes a new shader storage file header.
inline bool WriteShaderStorageHeader(FILE* file) {
  ShaderStorageFileHeader header;
  header.magic = kShaderStorageMagic;
  header.version_swapped = xe::byte_swap(ShaderStoredHeader::kVersion);
  return fwrite(&header, sizeof(header), 1, file) == 1;
}

// Reads shader entries from storage file and calls the callback for each.
// Returns the number of valid bytes read (for truncation on corruption).
// The callback signature is: bool(xenos::ShaderType type,
//                                  const uint32_t* ucode_dwords,
//                                  uint32_t ucode_dword_count,
//                                  uint64_t ucode_data_hash)
// Callback should return true to continue reading, false to stop.
template <typename Callback>
inline uint64_t ReadShaderEntries(FILE* file, Callback&& callback) {
  uint64_t valid_bytes = sizeof(ShaderStorageFileHeader);

  ShaderStoredHeader shader_header;
  std::vector<uint32_t> ucode_dwords;
  ucode_dwords.reserve(0xFFFF);

  while (true) {
    if (!fread(&shader_header, sizeof(shader_header), 1, file)) {
      break;
    }
    size_t ucode_byte_count =
        shader_header.ucode_dword_count * sizeof(uint32_t);
    ucode_dwords.resize(shader_header.ucode_dword_count);
    if (shader_header.ucode_dword_count &&
        !fread(ucode_dwords.data(), ucode_byte_count, 1, file)) {
      break;
    }
    uint64_t ucode_data_hash =
        XXH3_64bits(ucode_dwords.data(), ucode_byte_count);
    if (shader_header.ucode_data_hash != ucode_data_hash) {
      // Validation failed - corrupted entry.
      break;
    }
    valid_bytes += sizeof(shader_header) + ucode_byte_count;

    if (!callback(shader_header.type, ucode_dwords.data(),
                  shader_header.ucode_dword_count, ucode_data_hash)) {
      break;
    }
  }

  return valid_bytes;
}

// File header for pipeline storage (.xpso).
XEPACKEDSTRUCT(PipelineStorageFileHeader, {
  uint32_t magic;
  uint32_t magic_api;
  uint32_t version_swapped;
});

// Template class for shader and pipeline storage management.
// TPipelineStoredDescription is the API-specific pipeline description type.
template <typename TPipelineStoredDescription>
class ShaderStorageWriter {
 public:
  // Configuration for pipeline storage files.
  struct PipelineStorageConfig {
    std::string file_suffix;  // e.g., ".fsi.vk.xpso" or ".rov.d3d12.xpso"
    uint32_t api_magic;       // e.g., 'VKPS' or 'DXRO'
    uint32_t version;         // Pipeline description version
  };

  // Callback for loading a shader from ucode data.
  // Return true to continue reading, false to stop.
  using ShaderLoadCallback =
      std::function<bool(xenos::ShaderType type, const uint32_t* ucode_dwords,
                         uint32_t ucode_dword_count, uint64_t ucode_data_hash)>;

  // Callback for translating shaders. Called with the set of
  // (ucode_hash, modification) pairs that need translation.
  // Implementation should handle parallel translation internally.
  using TranslateCallback = std::function<void(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed)>;

  ShaderStorageWriter() = default;
  ~ShaderStorageWriter() { ShutdownShaderStorage(); }

  // Non-copyable.
  ShaderStorageWriter(const ShaderStorageWriter&) = delete;
  ShaderStorageWriter& operator=(const ShaderStorageWriter&) = delete;

  // Opens files, loads shaders, triggers translation, starts write thread.
  // Caller must call ShutdownShaderStorage() first if re-initializing.
  bool InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id,
      const PipelineStorageConfig& pipeline_config,
      ShaderLoadCallback load_shader, TranslateCallback translate_shaders,
      std::vector<TPipelineStoredDescription>& pipeline_descriptions_out) {
    cache_root_ = cache_root;
    title_id_ = title_id;

    if (!EnsureShaderStorageDirectoriesExist(cache_root)) {
      return false;
    }

    auto shader_storage_shareable_root =
        GetShaderStorageShareableRoot(cache_root);
    ++storage_index_;

    // Open pipeline storage file.
    auto pipeline_storage_file_path =
        shader_storage_shareable_root /
        fmt::format("{:08X}{}", title_id, pipeline_config.file_suffix);
    pipeline_storage_file_ =
        xe::filesystem::OpenFile(pipeline_storage_file_path, "a+b");
    if (!pipeline_storage_file_) {
      XELOGE(
          "Failed to open the pipeline storage file for writing, persistent "
          "shader storage will be disabled: {}",
          xe::path_to_utf8(pipeline_storage_file_path));
      return false;
    }

    // Read pipeline descriptions.
    const uint32_t pipeline_storage_version_swapped =
        xe::byte_swap(pipeline_config.version);
    int64_t pipeline_storage_size_before = 0;
    if (xe::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END)) {
      pipeline_storage_size_before =
          xe::filesystem::Tell(pipeline_storage_file_);
    }
    xe::filesystem::Seek(pipeline_storage_file_, 0, SEEK_SET);
    PipelineStorageFileHeader pipeline_header = {};
    bool pipeline_header_read =
        fread(&pipeline_header, sizeof(pipeline_header), 1,
              pipeline_storage_file_) == 1;
    if (pipeline_header_read &&
        pipeline_header.magic == kPipelineStorageMagic &&
        pipeline_header.magic_api == pipeline_config.api_magic &&
        pipeline_header.version_swapped == pipeline_storage_version_swapped) {
      // Valid header, read pipeline descriptions.
      xe::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);
      int64_t pipeline_storage_told_end =
          xe::filesystem::Tell(pipeline_storage_file_);
      size_t pipeline_storage_told_count =
          size_t(pipeline_storage_told_end >= int64_t(sizeof(pipeline_header))
                     ? (uint64_t(pipeline_storage_told_end) -
                        sizeof(pipeline_header)) /
                           sizeof(TPipelineStoredDescription)
                     : 0);
      if (pipeline_storage_told_count) {
        xe::filesystem::Seek(pipeline_storage_file_,
                             int64_t(sizeof(pipeline_header)), SEEK_SET);
        pipeline_descriptions_out.resize(pipeline_storage_told_count);
        pipeline_descriptions_out.resize(
            fread(pipeline_descriptions_out.data(),
                  sizeof(TPipelineStoredDescription),
                  pipeline_storage_told_count, pipeline_storage_file_));
        // Validate each description's hash.
        size_t valid_count = 0;
        for (size_t i = 0; i < pipeline_descriptions_out.size(); ++i) {
          const TPipelineStoredDescription& desc = pipeline_descriptions_out[i];
          if (XXH3_64bits(&desc.description, sizeof(desc.description)) !=
              desc.description_hash) {
            break;
          }
          ++valid_count;
        }
        if (valid_count < pipeline_storage_told_count) {
          XELOGW(
              "Pipeline storage: the file holds {} description(s) but only {} "
              "of them pass their own hash - discarding the remaining {}. The "
              "run that wrote them was interrupted mid-write, or the entries "
              "were written by a build with a different description layout.",
              pipeline_storage_told_count, valid_count,
              pipeline_storage_told_count - valid_count);
        }
        pipeline_descriptions_out.resize(valid_count);
      }
      // Truncate to last valid description.
      xe::filesystem::TruncateStdioFile(
          pipeline_storage_file_,
          uint64_t(sizeof(pipeline_header) +
                   sizeof(TPipelineStoredDescription) *
                       pipeline_descriptions_out.size()));
    } else {
      if (pipeline_storage_size_before > 0) {
        XELOGW(
            "Pipeline storage: REJECTING an existing {} byte file - header {} "
            "(magic {:08X}/{:08X} wanted {:08X}/{:08X}, version {} wanted {}). "
            "Every pipeline the last run compiled is being thrown away.",
            pipeline_storage_size_before,
            pipeline_header_read ? "mismatched" : "unreadable",
            pipeline_header.magic, pipeline_header.magic_api,
            kPipelineStorageMagic, pipeline_config.api_magic,
            xe::byte_swap(pipeline_header.version_swapped),
            pipeline_config.version);
      }
      // Write new header.
      xe::filesystem::TruncateStdioFile(pipeline_storage_file_, 0);
      pipeline_header.magic = kPipelineStorageMagic;
      pipeline_header.magic_api = pipeline_config.api_magic;
      pipeline_header.version_swapped = pipeline_storage_version_swapped;
      if (fwrite(&pipeline_header, sizeof(pipeline_header), 1,
                 pipeline_storage_file_) != 1) {
        XELOGE("Failed to write pipeline storage header");
        fclose(pipeline_storage_file_);
        pipeline_storage_file_ = nullptr;
        return false;
      }
    }

    // Open shader storage file.
    auto shader_storage_file_path =
        GetShaderStorageFilePath(cache_root, title_id);
    shader_storage_file_ =
        xe::filesystem::OpenFile(shader_storage_file_path, "a+b");
    if (!shader_storage_file_) {
      XELOGE(
          "Failed to open the guest shader storage file for writing, "
          "persistent shader storage will be disabled: {}",
          xe::path_to_utf8(shader_storage_file_path));
      fclose(pipeline_storage_file_);
      pipeline_storage_file_ = nullptr;
      return false;
    }

    // Load shaders from storage.
    size_t shaders_loaded = 0;
    ShaderStorageFileHeader shader_header;
    // How big the file was BEFORE anything here touched it. Three consecutive
    // sessions read 0, 3 and 0 shaders while one of them created 598
    // pipelines, and there was no way to tell an empty file from a rejected
    // header from a file whose first entry was corrupt - all three print
    // "Loaded 0 shaders" and then truncate, which destroys the evidence.
    int64_t shader_storage_size_before = 0;
    if (xe::filesystem::Seek(shader_storage_file_, 0, SEEK_END)) {
      shader_storage_size_before = xe::filesystem::Tell(shader_storage_file_);
    }
    xe::filesystem::Seek(shader_storage_file_, 0, SEEK_SET);
    if (ValidateShaderStorageHeader(shader_storage_file_, shader_header)) {
      uint64_t shader_storage_valid_bytes = ReadShaderEntries(
          shader_storage_file_,
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            if (load_shader(type, ucode_dwords, ucode_dword_count,
                            ucode_data_hash)) {
              ++shaders_loaded;
              return true;
            }
            return false;
          });
      if (int64_t(shader_storage_valid_bytes) < shader_storage_size_before) {
        XELOGW(
            "Shader storage: the file is {} bytes but only {} of them are "
            "readable - {} bytes are being discarded after {} shader(s). The "
            "run that wrote it did not finish writing, or wrote something this "
            "build cannot read.",
            shader_storage_size_before, shader_storage_valid_bytes,
            shader_storage_size_before - int64_t(shader_storage_valid_bytes),
            shaders_loaded);
      }
      xe::filesystem::TruncateStdioFile(shader_storage_file_,
                                        shader_storage_valid_bytes);
    } else {
      if (shader_storage_size_before > 0) {
        XELOGW(
            "Shader storage: REJECTING an existing {} byte file - its header "
            "is not this build's (magic {:08X} wanted {:08X}, version {} "
            "wanted {}). Everything it held is being thrown away and the game "
            "will compile from scratch.",
            shader_storage_size_before, shader_header.magic,
            kShaderStorageMagic, xe::byte_swap(shader_header.version_swapped),
            ShaderStoredHeader::kVersion);
      }
      // Write new header.
      xe::filesystem::TruncateStdioFile(shader_storage_file_, 0);
      if (!WriteShaderStorageHeader(shader_storage_file_)) {
        XELOGE("Failed to write shader storage header");
        fclose(shader_storage_file_);
        shader_storage_file_ = nullptr;
        fclose(pipeline_storage_file_);
        pipeline_storage_file_ = nullptr;
        return false;
      }
    }
    XELOGI("Loaded {} shaders from storage", shaders_loaded);

    // Collect shader translations needed from pipeline descriptions.
    std::set<std::pair<uint64_t, uint64_t>> translations_needed;
    for (const TPipelineStoredDescription& desc : pipeline_descriptions_out) {
      translations_needed.emplace(desc.description.vertex_shader_hash,
                                  desc.description.vertex_shader_modification);
      if (desc.description.pixel_shader_hash) {
        translations_needed.emplace(desc.description.pixel_shader_hash,
                                    desc.description.pixel_shader_modification);
      }
    }
    XELOGI("Loaded {} pipeline descriptions, {} shader translations needed",
           pipeline_descriptions_out.size(), translations_needed.size());

    // Translate shaders (callback handles parallel translation).
    if (!translations_needed.empty() && translate_shaders) {
      translate_shaders(translations_needed);
    }

    // Start the write thread.
    storage_write_thread_shutdown_ = false;
    storage_write_thread_ =
        xe::threading::Thread::Create({}, [this]() { WriteThread(); });
    storage_write_thread_->set_name("Shader Storage Writer");

    return true;
  }

  // Shutdown storage: stops write thread, closes files.
  void ShutdownShaderStorage() {
    if (storage_write_thread_) {
      {
        std::lock_guard<std::mutex> lock(storage_write_request_lock_);
        size_t shaders_pending = storage_write_shader_queue_.size();
        size_t pipelines_pending = storage_write_pipeline_queue_.size();
        if (shaders_pending || pipelines_pending) {
          XELOGGPU(
              "Shader storage: writing out {} shader(s) and {} pipeline "
              "description(s) still queued at shutdown",
              shaders_pending, pipelines_pending);
        }
        storage_write_thread_shutdown_ = true;
      }
      storage_write_request_cond_.notify_one();
      xe::threading::Wait(storage_write_thread_.get(), false);
      storage_write_thread_.reset();
    }

    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.clear();
      storage_write_pipeline_queue_.clear();
      storage_write_flush_shaders_ = false;
      storage_write_flush_pipelines_ = false;
    }

    if (pipeline_storage_file_) {
      fclose(pipeline_storage_file_);
      pipeline_storage_file_ = nullptr;
    }
    if (shader_storage_file_) {
      fclose(shader_storage_file_);
      shader_storage_file_ = nullptr;
    }

    cache_root_.clear();
    title_id_ = 0;
  }

  uint32_t storage_index() const { return storage_index_; }
  FILE* shader_storage_file() const { return shader_storage_file_; }
  FILE* pipeline_storage_file() const { return pipeline_storage_file_; }
  bool is_active() const { return shader_storage_file_ != nullptr; }

  // What has actually reached the files this session, and how big they are
  // right now. Reported periodically rather than at shutdown: a console
  // session usually ends with a device loss or the system terminating the
  // app, and a number that is only printed on the way out is a number that is
  // never printed.
  std::string GetWriteReport() {
    int64_t shader_bytes = 0, pipeline_bytes = 0;
    // The write thread owns the file positions; only look while it is parked.
    std::lock_guard<std::mutex> lock(storage_write_request_lock_);
    if (shader_storage_file_ &&
        xe::filesystem::Seek(shader_storage_file_, 0, SEEK_END)) {
      shader_bytes = xe::filesystem::Tell(shader_storage_file_);
    }
    if (pipeline_storage_file_ &&
        xe::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END)) {
      pipeline_bytes = xe::filesystem::Tell(pipeline_storage_file_);
    }
    return fmt::format(
        "{} shader(s) and {} pipeline description(s) written this session, "
        "files are {} and {} bytes, {} and {} still queued",
        shaders_written_, pipelines_written_, shader_bytes, pipeline_bytes,
        storage_write_shader_queue_.size(),
        storage_write_pipeline_queue_.size());
  }
  const std::filesystem::path& cache_root() const { return cache_root_; }
  uint32_t title_id() const { return title_id_; }

  void QueueShaderWrite(const Shader* shader) {
    if (!shader_storage_file_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.push_back(shader);
    }
    storage_write_request_cond_.notify_one();
  }

  void QueuePipelineWrite(const TPipelineStoredDescription& description) {
    if (!pipeline_storage_file_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_pipeline_queue_.push_back(description);
    }
    storage_write_request_cond_.notify_one();
  }

  void RequestFlush(bool flush_shaders, bool flush_pipelines) {
    bool need_notify = false;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (flush_shaders) {
        storage_write_flush_shaders_ = true;
        need_notify = true;
      }
      if (flush_pipelines) {
        storage_write_flush_pipelines_ = true;
        need_notify = true;
      }
    }
    if (need_notify) {
      storage_write_request_cond_.notify_one();
    }
  }

 private:
  void WriteThread() {
    ShaderStoredHeader shader_header;
    std::memset(&shader_header, 0, sizeof(shader_header));

    std::vector<uint32_t> ucode_guest_endian;
    ucode_guest_endian.reserve(0xFFFF);

    bool flush_shaders = false;
    bool flush_pipelines = false;

    while (true) {
      if (flush_shaders) {
        flush_shaders = false;
        assert_not_null(shader_storage_file_);
        fflush(shader_storage_file_);
      }
      if (flush_pipelines) {
        flush_pipelines = false;
        assert_not_null(pipeline_storage_file_);
        fflush(pipeline_storage_file_);
      }

      const Shader* shader = nullptr;
      TPipelineStoredDescription pipeline_description;
      bool write_pipeline = false;
      {
        std::unique_lock<std::mutex> lock(storage_write_request_lock_);
        if (storage_write_thread_shutdown_ &&
            storage_write_shader_queue_.empty() &&
            storage_write_pipeline_queue_.empty()) {
          return;
        }
        // Note that shutdown does NOT return while anything is still queued.
        // Returning immediately (and then clearing the queues in
        // ShutdownShaderStorage) threw away everything written since the last
        // time this thread got to run: on a console, where the queue keeps
        // filling while a level streams in, a session that created hundreds
        // of pipelines persisted a handful of them. Every launch then had to
        // compile almost everything again, the creation queue grew to
        // hundreds of entries, draws whose pipeline was not ready yet were
        // skipped, and the render target kept the previous frame's contents
        // where they should have been - the "old frames drawn over the new
        // ones" the user was seeing.
        if (!storage_write_shader_queue_.empty()) {
          shader = storage_write_shader_queue_.front();
          storage_write_shader_queue_.pop_front();
        } else if (storage_write_flush_shaders_) {
          storage_write_flush_shaders_ = false;
          flush_shaders = true;
        }
        if (!storage_write_pipeline_queue_.empty()) {
          std::memcpy(&pipeline_description,
                      &storage_write_pipeline_queue_.front(),
                      sizeof(pipeline_description));
          storage_write_pipeline_queue_.pop_front();
          write_pipeline = true;
        } else if (storage_write_flush_pipelines_) {
          storage_write_flush_pipelines_ = false;
          flush_pipelines = true;
        }
        if (!shader && !write_pipeline && !flush_shaders && !flush_pipelines) {
          storage_write_request_cond_.wait(lock);
          continue;
        }
      }

      if (shader) {
        shader_header.ucode_data_hash = shader->ucode_data_hash();
        shader_header.ucode_dword_count = shader->ucode_dword_count();
        shader_header.type = shader->type();
        assert_not_null(shader_storage_file_);
        if (fwrite(&shader_header, sizeof(shader_header), 1,
                   shader_storage_file_) != 1) {
          XELOGE("Failed to write shader header to storage");
          shader_header.ucode_dword_count = 0;
        } else {
          ++shaders_written_;
        }
        if (shader_header.ucode_dword_count) {
          ucode_guest_endian.resize(shader_header.ucode_dword_count);
          // Need to swap because the hash is calculated for the shader with
          // guest endianness.
          xe::copy_and_swap(ucode_guest_endian.data(), shader->ucode_dwords(),
                            shader_header.ucode_dword_count);
          if (fwrite(ucode_guest_endian.data(),
                     shader_header.ucode_dword_count * sizeof(uint32_t), 1,
                     shader_storage_file_) != 1) {
            XELOGE("Failed to write shader ucode to storage");
          }
        }
      }

      if (write_pipeline) {
        assert_not_null(pipeline_storage_file_);
        if (fwrite(&pipeline_description, sizeof(pipeline_description), 1,
                   pipeline_storage_file_) != 1) {
          XELOGE("Failed to write pipeline description to storage");
        } else {
          ++pipelines_written_;
        }
      }
    }
  }

  std::filesystem::path cache_root_;
  uint32_t title_id_ = 0;
  uint32_t storage_index_ = 0;

  FILE* shader_storage_file_ = nullptr;
  FILE* pipeline_storage_file_ = nullptr;
  // Written this session, counted on the write thread and reported at
  // shutdown against the resulting file sizes.
  uint64_t shaders_written_ = 0;
  uint64_t pipelines_written_ = 0;

  std::unique_ptr<xe::threading::Thread> storage_write_thread_;
  std::mutex storage_write_request_lock_;
  std::condition_variable storage_write_request_cond_;
  std::deque<const Shader*> storage_write_shader_queue_;
  std::deque<TPipelineStoredDescription> storage_write_pipeline_queue_;
  bool storage_write_flush_shaders_ = false;
  bool storage_write_flush_pipelines_ = false;
  bool storage_write_thread_shutdown_ = false;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SHADER_STORAGE_H_
