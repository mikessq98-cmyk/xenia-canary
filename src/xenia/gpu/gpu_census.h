/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GPU_CENSUS_H_
#define XENIA_GPU_GPU_CENSUS_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xe {
namespace gpu {

// Per-object record of what the GPU side is doing - shaders, pipelines,
// textures and host texture creation - written out as CSV tables next to the
// shader storage and summarised in the periodic report.
//
// Recording is an atomic increment into an entry found by hash; the tables are
// only walked when a report is asked for.
class GpuCensus {
 public:
  static GpuCensus& Get();

  void Initialize(const std::filesystem::path& cache_root, uint32_t title_id);
  void Shutdown();
  bool enabled() const { return enabled_; }

  // ---- shaders -----------------------------------------------------------
  // Shape of one guest shader, recorded when it is first translated.
  struct ShaderShape {
    bool is_pixel_shader = false;
    uint32_t ucode_instructions = 0;
    uint32_t control_flow_labels = 0;
    uint32_t texture_fetches = 0;
    uint32_t vertex_fetches = 0;
    bool memexport = false;
    bool kills_pixels = false;
    bool writes_depth = false;
  };
  void RecordShaderTranslated(uint64_t ucode_hash, const ShaderShape& shape,
                              uint64_t modification, size_t dxbc_bytes);

  // ---- pipelines ---------------------------------------------------------
  void RecordPipelineCompiled(uint64_t vs_hash, uint64_t ps_hash,
                              uint64_t state_key, double compile_ms);
  void RecordDraw(uint64_t vs_hash, uint64_t ps_hash, uint32_t index_count);
  void RecordDrawSkipped(uint64_t vs_hash, uint64_t ps_hash);
  // A draw that was submitted on its own and waited on, to prove the pair does
  // not hang. Every one of these is a full GPU drain.
  void RecordVerificationSync(uint64_t vs_hash, uint64_t ps_hash,
                              double wait_ms);

  // ---- textures ----------------------------------------------------------
  struct TextureShape {
    uint32_t format = 0;
    uint32_t dimension = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth_or_array_size = 0;
    uint32_t mip_levels = 0;
    bool scaled_resolve = false;
  };
  void RecordTextureCreated(uint64_t key_hash, const TextureShape& shape,
                            uint64_t host_bytes);
  void RecordTextureLoaded(uint64_t key_hash, uint64_t guest_bytes,
                           double load_ms);
  void RecordTextureBound(uint64_t key_hash);
  void RecordTextureEvicted(uint64_t key_hash, bool was_in_working_set);

  // ---- host texture creation ---------------------------------------------
  // State the driver call ran under. Slow ones are also kept individually so
  // a later hypothesis can be tested without another build.
  struct CreationContext {
    uint32_t compilers_busy = 0;
    uint32_t queue_depth = 0;
    bool from_pool = false;
    uint64_t pool_bytes = 0;
    uint64_t free_host_bytes = 0;
    uint64_t host_bytes = 0;
    uint32_t format = 0;
    uint32_t dimension = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 0;
  };
  void RecordTextureCreation(const CreationContext& context, double ms,
                             bool succeeded);
  // One line for the periodic report - the buckets, side by side.
  std::string GetCreationReport();

  // ---- what actually multiplies the pipeline count ------------------------
  // Shader pair, translation modification and render state counted apart, so
  // which of the three multiplies the pipeline count can be read off.
  void RecordPipelineDescription(uint64_t vs_hash, uint64_t vs_modification,
                                 uint64_t ps_hash, uint64_t ps_modification,
                                 uint64_t render_state_hash,
                                 uint64_t description_hash,
                                 const std::string& render_state_text);
  std::string GetPipelineShapeReport();

  // ---- which shaders read which textures -----------------------------------
  // Which shader pairs drew with a texture.
  void RecordTextureUsedByShaders(uint64_t texture_key_hash, uint64_t vs_hash,
                                  uint64_t ps_hash);

  // ---- reporting ---------------------------------------------------------
  // A handful of lines for the periodic log: the worst offender in each
  // category, which is what a human reads first.
  std::string GetHighlights();
  // The full tables, written next to the shader storage.
  void WriteTables();

 private:
  struct ShaderEntry {
    ShaderShape shape;
    std::atomic<uint32_t> modifications{0};
    std::atomic<uint64_t> dxbc_bytes{0};
    std::atomic<uint64_t> draws{0};
    // Distinct modification values seen, so the count above is variants and
    // not re-translations of the same one.
    std::mutex modification_lock;
    std::unordered_map<uint64_t, uint64_t> modification_dxbc_bytes;
  };
  struct PipelineEntry {
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
    std::atomic<uint32_t> variants{0};
    std::atomic<uint64_t> compile_us{0};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> indices{0};
    std::atomic<uint64_t> draws_skipped{0};
    std::atomic<uint32_t> verification_syncs{0};
    std::atomic<uint64_t> verification_wait_us{0};
  };
  struct TextureEntry {
    TextureShape shape;
    uint64_t host_bytes = 0;
    std::atomic<uint32_t> creations{0};
    std::atomic<uint32_t> loads{0};
    std::atomic<uint64_t> loaded_guest_bytes{0};
    std::atomic<uint64_t> load_us{0};
    std::atomic<uint64_t> binds{0};
    std::atomic<uint32_t> evictions{0};
    std::atomic<uint32_t> evictions_from_working_set{0};
  };

  ShaderEntry* FindShader(uint64_t ucode_hash);
  PipelineEntry* FindPipeline(uint64_t vs_hash, uint64_t ps_hash);
  TextureEntry* FindTexture(uint64_t key_hash);

  bool enabled_ = false;
  std::filesystem::path table_root_;
  uint32_t title_id_ = 0;

  // The maps are only ever grown, and entries are never removed while the
  // title runs, so a pointer handed out stays valid - that is what lets the
  // recording paths touch an entry without holding a lock.
  std::mutex shaders_lock_;
  std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> shaders_;
  std::mutex pipelines_lock_;
  std::unordered_map<uint64_t, std::unique_ptr<PipelineEntry>> pipelines_;
  std::mutex textures_lock_;
  std::unordered_map<uint64_t, std::unique_ptr<TextureEntry>> textures_;

  std::atomic<uint64_t> total_verification_syncs_{0};
  std::atomic<uint64_t> total_verification_wait_us_{0};

  // More buckets than one hypothesis needs, so the next one costs no build.
  struct CreationBucket {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> us{0};
    void Add(double ms) {
      count.fetch_add(1, std::memory_order_relaxed);
      us.fetch_add(uint64_t(ms * 1000.0), std::memory_order_relaxed);
    }
    double average_ms() const {
      uint64_t n = count.load(std::memory_order_relaxed);
      return n ? double(us.load(std::memory_order_relaxed)) / 1000.0 / double(n)
               : 0.0;
    }
  };
  CreationBucket creation_while_compiling_;
  CreationBucket creation_while_idle_;
  CreationBucket creation_from_pool_;
  CreationBucket creation_from_driver_;
  CreationBucket creation_low_memory_;   // under 512 MB free
  CreationBucket creation_ample_memory_;
  CreationBucket creation_small_;  // under 1 MB of texture
  CreationBucket creation_large_;
  std::atomic<uint64_t> creation_failures_{0};

  // Individual slow creations, so a hypothesis nobody has had yet can still be
  // tested against what actually happened. Capped - this is evidence, not a
  // trace.
  struct CreationEvent {
    double ms;
    CreationContext context;
  };
  static constexpr size_t kMaxCreationEvents = 8192;
  static constexpr double kSlowCreationMs = 4.0;
  std::mutex creation_events_lock_;
  std::vector<CreationEvent> creation_events_;

  // The three ways a pipeline description can differ, counted apart.
  std::mutex shape_lock_;
  std::unordered_set<uint64_t> distinct_descriptions_;
  std::unordered_set<uint64_t> distinct_shader_pairs_;
  std::unordered_set<uint64_t> distinct_shader_combinations_;  // pair + mods
  // Render state -> how many pipelines use it, and what it is in words. If
  // this map is short, the state is not what multiplies.
  std::unordered_map<uint64_t, std::pair<uint64_t, std::string>>
      distinct_render_states_;

  // texture -> the shader pairs that drew with it, and the reverse count.
  std::mutex texture_shader_lock_;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>>
      texture_shader_pairs_;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>>
      shader_pair_textures_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GPU_CENSUS_H_
