/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/gpu_census.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/xxhash.h"

DEFINE_bool(
    gpu_census, true,
    "Keep a per-object record of what the GPU side is doing - every guest "
    "shader with its shape and how many DXBC variants it was compiled into, "
    "every shader pair with what it cost to compile and how much it drew, "
    "every texture with how many times it was loaded, bound and thrown out.\n"
    "Written next to the shader storage as three tables, and summarised in the "
    "periodic report. This is what turns \"the title stutters here\" into "
    "\"this shader has 31 variants and each one is half a second of driver "
    "compiler\", which is the difference between guessing at a fix and "
    "choosing one.",
    "GPU");

namespace xe {
namespace gpu {

GpuCensus& GpuCensus::Get() {
  static GpuCensus census;
  return census;
}

void GpuCensus::Initialize(const std::filesystem::path& cache_root,
                           uint32_t title_id) {
  Shutdown();
  if (!cvars::gpu_census) {
    return;
  }
  table_root_ = cache_root;
  title_id_ = title_id;
  std::error_code ec;
  std::filesystem::create_directories(table_root_, ec);
  enabled_ = true;
}

void GpuCensus::Shutdown() {
  if (enabled_) {
    WriteTables();
  }
  enabled_ = false;
  {
    std::lock_guard<std::mutex> lock(shaders_lock_);
    shaders_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(pipelines_lock_);
    pipelines_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(textures_lock_);
    textures_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(objects_lock_);
    objects_.clear();
  }
  total_verification_syncs_.store(0, std::memory_order_relaxed);
  total_verification_wait_us_.store(0, std::memory_order_relaxed);
}

GpuCensus::ShaderEntry* GpuCensus::FindShader(uint64_t ucode_hash) {
  std::lock_guard<std::mutex> lock(shaders_lock_);
  auto it = shaders_.find(ucode_hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto entry = std::make_unique<ShaderEntry>();
  ShaderEntry* raw = entry.get();
  shaders_.emplace(ucode_hash, std::move(entry));
  return raw;
}

GpuCensus::PipelineEntry* GpuCensus::FindPipeline(uint64_t vs_hash,
                                                  uint64_t ps_hash) {
  uint64_t pair[2] = {vs_hash, ps_hash};
  uint64_t key = XXH3_64bits(pair, sizeof(pair));
  std::lock_guard<std::mutex> lock(pipelines_lock_);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second.get();
  }
  auto entry = std::make_unique<PipelineEntry>();
  entry->vs_hash = vs_hash;
  entry->ps_hash = ps_hash;
  PipelineEntry* raw = entry.get();
  pipelines_.emplace(key, std::move(entry));
  return raw;
}

GpuCensus::TextureEntry* GpuCensus::FindTexture(uint64_t key_hash) {
  std::lock_guard<std::mutex> lock(textures_lock_);
  auto it = textures_.find(key_hash);
  if (it != textures_.end()) {
    return it->second.get();
  }
  auto entry = std::make_unique<TextureEntry>();
  TextureEntry* raw = entry.get();
  textures_.emplace(key_hash, std::move(entry));
  return raw;
}

void GpuCensus::RecordShaderTranslated(uint64_t ucode_hash,
                                       const ShaderShape& shape,
                                       uint64_t modification,
                                       size_t dxbc_bytes) {
  if (!enabled_) {
    return;
  }
  ShaderEntry* entry = FindShader(ucode_hash);
  entry->shape = shape;
  std::lock_guard<std::mutex> lock(entry->modification_lock);
  // Counted per DISTINCT modification: the same variant can be translated
  // again after the bytecode has been released under memory pressure, and
  // counting those would report a permutation explosion that is not there.
  auto inserted = entry->modification_dxbc_bytes.emplace(modification,
                                                         uint64_t(dxbc_bytes));
  if (inserted.second) {
    entry->modifications.fetch_add(1, std::memory_order_relaxed);
    entry->dxbc_bytes.fetch_add(dxbc_bytes, std::memory_order_relaxed);
  }
}

void GpuCensus::RecordPipelineCompiled(uint64_t vs_hash, uint64_t ps_hash,
                                       uint64_t state_key, double compile_ms) {
  if (!enabled_) {
    return;
  }
  PipelineEntry* entry = FindPipeline(vs_hash, ps_hash);
  entry->variants.fetch_add(1, std::memory_order_relaxed);
  entry->compile_us.fetch_add(uint64_t(compile_ms * 1000.0),
                              std::memory_order_relaxed);
}

void GpuCensus::RecordDraw(uint64_t vs_hash, uint64_t ps_hash,
                           uint32_t index_count) {
  if (!enabled_) {
    return;
  }
  PipelineEntry* entry = FindPipeline(vs_hash, ps_hash);
  entry->draws.fetch_add(1, std::memory_order_relaxed);
  entry->indices.fetch_add(index_count, std::memory_order_relaxed);
  ShaderEntry* vs = FindShader(vs_hash);
  vs->draws.fetch_add(1, std::memory_order_relaxed);
  if (ps_hash) {
    FindShader(ps_hash)->draws.fetch_add(1, std::memory_order_relaxed);
  }
}

void GpuCensus::RecordDrawSkipped(uint64_t vs_hash, uint64_t ps_hash) {
  if (!enabled_) {
    return;
  }
  FindPipeline(vs_hash, ps_hash)
      ->draws_skipped.fetch_add(1, std::memory_order_relaxed);
}

void GpuCensus::RecordVerificationSync(uint64_t vs_hash, uint64_t ps_hash,
                                       double wait_ms) {
  if (!enabled_) {
    return;
  }
  uint64_t wait_us = uint64_t(wait_ms * 1000.0);
  PipelineEntry* entry = FindPipeline(vs_hash, ps_hash);
  entry->verification_syncs.fetch_add(1, std::memory_order_relaxed);
  entry->verification_wait_us.fetch_add(wait_us, std::memory_order_relaxed);
  total_verification_syncs_.fetch_add(1, std::memory_order_relaxed);
  total_verification_wait_us_.fetch_add(wait_us, std::memory_order_relaxed);
}

void GpuCensus::RecordTextureCreated(uint64_t key_hash,
                                     const TextureShape& shape,
                                     uint64_t host_bytes) {
  if (!enabled_) {
    return;
  }
  TextureEntry* entry = FindTexture(key_hash);
  entry->shape = shape;
  entry->host_bytes = host_bytes;
  entry->creations.fetch_add(1, std::memory_order_relaxed);
}

void GpuCensus::RecordTextureLoaded(uint64_t key_hash, uint64_t guest_bytes,
                                    double load_ms) {
  if (!enabled_) {
    return;
  }
  TextureEntry* entry = FindTexture(key_hash);
  entry->loads.fetch_add(1, std::memory_order_relaxed);
  entry->loaded_guest_bytes.fetch_add(guest_bytes, std::memory_order_relaxed);
  entry->load_us.fetch_add(uint64_t(load_ms * 1000.0),
                           std::memory_order_relaxed);
}

void GpuCensus::RecordTextureBound(uint64_t key_hash) {
  if (!enabled_) {
    return;
  }
  FindTexture(key_hash)->binds.fetch_add(1, std::memory_order_relaxed);
}

void GpuCensus::RecordTextureEvicted(uint64_t key_hash,
                                     bool was_in_working_set) {
  if (!enabled_) {
    return;
  }
  TextureEntry* entry = FindTexture(key_hash);
  entry->evictions.fetch_add(1, std::memory_order_relaxed);
  if (was_in_working_set) {
    entry->evictions_from_working_set.fetch_add(1, std::memory_order_relaxed);
  }
}

void GpuCensus::RecordTextureCreation(const CreationContext& context, double ms,
                                      bool succeeded) {
  if (!enabled_) {
    return;
  }
  if (!succeeded) {
    creation_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  // Every creation lands in one bucket of each pair, so the pairs can be read
  // independently - "slow while compiling" and "slow when large" are different
  // claims and the same creation can support both.
  (context.compilers_busy ? creation_while_compiling_ : creation_while_idle_)
      .Add(ms);
  (context.from_pool ? creation_from_pool_ : creation_from_driver_).Add(ms);
  (context.free_host_bytes && context.free_host_bytes < (512ull << 20)
       ? creation_low_memory_
       : creation_ample_memory_)
      .Add(ms);
  (context.host_bytes < (1ull << 20) ? creation_small_ : creation_large_)
      .Add(ms);

  if (ms >= kSlowCreationMs) {
    std::lock_guard<std::mutex> lock(creation_events_lock_);
    if (creation_events_.size() < kMaxCreationEvents) {
      creation_events_.push_back(CreationEvent{ms, context});
    }
  }
}

std::string GpuCensus::GetCreationReport() {
  if (!enabled_) {
    return std::string();
  }
  uint64_t compiling = creation_while_compiling_.count.load(
      std::memory_order_relaxed);
  uint64_t idle = creation_while_idle_.count.load(std::memory_order_relaxed);
  if (!compiling && !idle) {
    return std::string();
  }
  return fmt::format(
      "{} host textures created | while the driver was COMPILING: {} at {:.1f} "
      "ms average; while it was not: {} at {:.1f} ms | from the reuse pool: {} "
      "at {:.1f} ms; from the driver: {} at {:.1f} ms | under 512 MB free: {} "
      "at {:.1f} ms; with room: {} at {:.1f} ms | under 1 MB: {} at {:.1f} ms; "
      "larger: {} at {:.1f} ms | {} failed",
      compiling + idle, compiling, creation_while_compiling_.average_ms(), idle,
      creation_while_idle_.average_ms(),
      creation_from_pool_.count.load(std::memory_order_relaxed),
      creation_from_pool_.average_ms(),
      creation_from_driver_.count.load(std::memory_order_relaxed),
      creation_from_driver_.average_ms(),
      creation_low_memory_.count.load(std::memory_order_relaxed),
      creation_low_memory_.average_ms(),
      creation_ample_memory_.count.load(std::memory_order_relaxed),
      creation_ample_memory_.average_ms(),
      creation_small_.count.load(std::memory_order_relaxed),
      creation_small_.average_ms(),
      creation_large_.count.load(std::memory_order_relaxed),
      creation_large_.average_ms(),
      creation_failures_.load(std::memory_order_relaxed));
}

void GpuCensus::RecordPipelineDescription(uint64_t vs_hash,
                                          uint64_t vs_modification,
                                          uint64_t ps_hash,
                                          uint64_t ps_modification,
                                          uint64_t render_state_hash,
                                          uint64_t description_hash,
                                          const std::string& render_state_text) {
  if (!enabled_) {
    return;
  }
  uint64_t pair_key[2] = {vs_hash, ps_hash};
  uint64_t combo_key[4] = {vs_hash, vs_modification, ps_hash, ps_modification};
  std::lock_guard<std::mutex> lock(shape_lock_);
  distinct_descriptions_.insert(description_hash);
  distinct_shader_pairs_.insert(XXH3_64bits(pair_key, sizeof(pair_key)));
  distinct_shader_combinations_.insert(
      XXH3_64bits(combo_key, sizeof(combo_key)));
  auto& entry = distinct_render_states_[render_state_hash];
  ++entry.first;
  if (entry.second.empty()) {
    entry.second = render_state_text;
  }
}

std::string GpuCensus::GetPipelineShapeReport() {
  if (!enabled_) {
    return std::string();
  }
  std::lock_guard<std::mutex> lock(shape_lock_);
  if (distinct_descriptions_.empty()) {
    return std::string();
  }
  return fmt::format(
      "{} distinct pipelines = {} shader pair(s) x {} pair+modification "
      "combination(s) x {} distinct render state(s)",
      distinct_descriptions_.size(), distinct_shader_pairs_.size(),
      distinct_shader_combinations_.size(), distinct_render_states_.size());
}

void GpuCensus::RecordTextureUsedByShaders(uint64_t texture_key_hash,
                                           uint64_t vs_hash,
                                           uint64_t ps_hash) {
  if (!enabled_ || !vs_hash) {
    return;
  }
  uint64_t pair[2] = {vs_hash, ps_hash};
  uint64_t pair_key = XXH3_64bits(pair, sizeof(pair));
  std::lock_guard<std::mutex> lock(texture_shader_lock_);
  // Bounded: a texture every shader in the game touches would otherwise grow a
  // set the size of the shader list, per texture.
  auto& pairs = texture_shader_pairs_[texture_key_hash];
  if (pairs.size() < 64) {
    pairs.insert(pair_key);
  }
  auto& textures = shader_pair_textures_[pair_key];
  if (textures.size() < 256) {
    textures.insert(texture_key_hash);
  }
}

GpuCensus::ObjectEntry* GpuCensus::FindObject(uint64_t object_key) {
  std::lock_guard<std::mutex> lock(objects_lock_);
  auto it = objects_.find(object_key);
  if (it != objects_.end()) {
    return it->second.get();
  }
  // Bounded: a title that streams geometry through a ring buffer produces a
  // new key per frame, and one Black Ops session reached 387467 of them - of
  // which 208311 were drawn once or twice and accounted for 1.5% of the draws.
  // Past the cap the tail is simply not tracked; the 7% of keys carrying 98.5%
  // of the work are all long-lived and already in.
  if (objects_.size() >= kMaxTrackedObjects) {
    return nullptr;
  }
  auto entry = std::make_unique<ObjectEntry>();
  ObjectEntry* raw = entry.get();
  objects_.emplace(object_key, std::move(entry));
  return raw;
}

void GpuCensus::RecordObjectDraw(uint64_t object_key, uint32_t vertex_base,
                                 uint32_t index_base, uint32_t index_count,
                                 uint64_t vs_hash, uint64_t ps_hash,
                                 uint64_t state_key, bool pipeline_ready) {
  if (!enabled_ || !object_key) {
    return;
  }
  ObjectEntry* entry = FindObject(object_key);
  if (!entry) {
    return;
  }
  (pipeline_ready ? entry->draws : entry->draws_skipped)
      .fetch_add(1, std::memory_order_relaxed);
  uint64_t pair[2] = {vs_hash, ps_hash};
  std::lock_guard<std::mutex> lock(entry->lock);
  entry->vertex_base = vertex_base;
  entry->index_base = index_base;
  entry->index_count = index_count;
  if (entry->shader_pairs.size() < 64) {
    entry->shader_pairs.insert(XXH3_64bits(pair, sizeof(pair)));
  }
  if (entry->states_wanted.size() < 64) {
    entry->states_wanted.insert(state_key);
  }
  if (pipeline_ready && entry->states_ready.size() < 64) {
    entry->states_ready.insert(state_key);
  }
}

bool GpuCensus::IsObjectOneStateShort(uint64_t object_key) {
  if (!enabled_ || !object_key) {
    return false;
  }
  ObjectEntry* entry = nullptr;
  {
    std::lock_guard<std::mutex> lock(objects_lock_);
    auto it = objects_.find(object_key);
    if (it == objects_.end()) {
      return false;
    }
    entry = it->second.get();
  }
  std::lock_guard<std::mutex> entry_lock(entry->lock);
  // Drawn enough times to be a mesh rather than a slot in a ring buffer: the
  // arenas produce keys drawn once or twice, real geometry is drawn hundreds
  // of times. 98.5% of all draws come from keys past this line.
  if (entry->draws.load(std::memory_order_relaxed) +
          entry->draws_skipped.load(std::memory_order_relaxed) <
      kObjectEstablishedDraws) {
    return false;
  }
  size_t wanted = entry->states_wanted.size();
  return wanted > 1 && entry->states_ready.size() + 1 == wanted;
}

std::string GpuCensus::GetObjectReport() {
  if (!enabled_) {
    return std::string();
  }
  size_t total = 0, incomplete = 0, worst_missing = 0, multi_pass = 0;
  {
    std::lock_guard<std::mutex> lock(objects_lock_);
    total = objects_.size();
    for (const auto& pair : objects_) {
      std::lock_guard<std::mutex> entry_lock(pair.second->lock);
      size_t wanted = pair.second->states_wanted.size();
      size_t ready = pair.second->states_ready.size();
      if (wanted > 1) {
        ++multi_pass;
      }
      if (ready < wanted) {
        ++incomplete;
        worst_missing = std::max(worst_missing, wanted - ready);
      }
    }
  }
  if (!total) {
    return std::string();
  }
  return fmt::format(
      "{} distinct meshes, {} drawn in more than one pipeline state, {} still "
      "missing at least one of theirs (worst is short by {})",
      total, multi_pass, incomplete, worst_missing);
}

std::string GpuCensus::GetHighlights() {
  if (!enabled_) {
    return std::string();
  }
  // The single worst entry in each category. Anything more belongs in the
  // tables; this is the line a human reads while the session is running.
  uint64_t worst_variant_shader = 0;
  uint32_t worst_variants = 0;
  {
    std::lock_guard<std::mutex> lock(shaders_lock_);
    for (const auto& pair : shaders_) {
      uint32_t variants =
          pair.second->modifications.load(std::memory_order_relaxed);
      if (variants > worst_variants) {
        worst_variants = variants;
        worst_variant_shader = pair.first;
      }
    }
  }
  uint64_t worst_compile_vs = 0, worst_compile_ps = 0;
  uint64_t worst_compile_us = 0;
  {
    std::lock_guard<std::mutex> lock(pipelines_lock_);
    for (const auto& pair : pipelines_) {
      uint64_t us = pair.second->compile_us.load(std::memory_order_relaxed);
      if (us > worst_compile_us) {
        worst_compile_us = us;
        worst_compile_vs = pair.second->vs_hash;
        worst_compile_ps = pair.second->ps_hash;
      }
    }
  }
  uint64_t worst_reload_texture = 0;
  uint32_t worst_reloads = 0;
  uint64_t reloaded_textures = 0;
  {
    std::lock_guard<std::mutex> lock(textures_lock_);
    for (const auto& pair : textures_) {
      uint32_t evictions =
          pair.second->evictions.load(std::memory_order_relaxed);
      // A texture loaded more times than it was created is one the title keeps
      // coming back to after it was thrown out - the churn this port has spent
      // the most time on.
      uint32_t loads = pair.second->loads.load(std::memory_order_relaxed);
      if (evictions && loads > 1) {
        ++reloaded_textures;
      }
      if (evictions > worst_reloads) {
        worst_reloads = evictions;
        worst_reload_texture = pair.first;
      }
    }
  }
  uint64_t syncs = total_verification_syncs_.load(std::memory_order_relaxed);
  double sync_ms =
      double(total_verification_wait_us_.load(std::memory_order_relaxed)) /
      1000.0;
  return fmt::format(
      "worst shader {:016X} compiled into {} DXBC variants | costliest pair VS "
      "{:016X} PS {:016X} took {:.0f} ms of driver compiler | {} textures came "
      "back after being evicted, worst one {} times | draw verification: {} "
      "GPU drains costing {:.0f} ms",
      worst_variant_shader, worst_variants, worst_compile_vs, worst_compile_ps,
      double(worst_compile_us) / 1000.0, reloaded_textures, worst_reloads,
      syncs, sync_ms);
}

void GpuCensus::WriteTables() {
  if (!enabled_ || table_root_.empty()) {
    return;
  }
  {
    std::ofstream file(table_root_ /
                       fmt::format("{:08X}.shaders.csv", title_id_));
    if (file) {
      file << "ucode_hash,type,dxbc_variants,ucode_instructions,"
              "control_flow_labels,texture_fetches,vertex_fetches,memexport,"
              "kills_pixels,writes_depth,dxbc_bytes,draws,"
              "dxbc_prologue_and_control,dxbc_alu,dxbc_texture_fetch,"
              "dxbc_vertex_fetch,dxbc_epilogue\n";
      std::lock_guard<std::mutex> lock(shaders_lock_);
      for (const auto& pair : shaders_) {
        const ShaderEntry& e = *pair.second;
        file << fmt::format(
            "{:016X},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
            pair.first, e.shape.is_pixel_shader ? "PS" : "VS",
            e.modifications.load(std::memory_order_relaxed),
            e.shape.ucode_instructions, e.shape.control_flow_labels,
            e.shape.texture_fetches, e.shape.vertex_fetches,
            e.shape.memexport ? 1 : 0, e.shape.kills_pixels ? 1 : 0,
            e.shape.writes_depth ? 1 : 0,
            e.dxbc_bytes.load(std::memory_order_relaxed),
            e.draws.load(std::memory_order_relaxed),
            e.shape.dxbc_prologue_and_control, e.shape.dxbc_alu,
            e.shape.dxbc_texture_fetch, e.shape.dxbc_vertex_fetch,
            e.shape.dxbc_epilogue);
      }
    }
  }
  {
    std::ofstream file(table_root_ /
                       fmt::format("{:08X}.pipelines.csv", title_id_));
    if (file) {
      file << "vs_hash,ps_hash,variants,compile_ms,draws,indices,draws_skipped,"
              "verification_syncs,verification_wait_ms\n";
      std::lock_guard<std::mutex> lock(pipelines_lock_);
      for (const auto& pair : pipelines_) {
        const PipelineEntry& e = *pair.second;
        file << fmt::format(
            "{:016X},{:016X},{},{:.1f},{},{},{},{},{:.1f}\n", e.vs_hash,
            e.ps_hash, e.variants.load(std::memory_order_relaxed),
            double(e.compile_us.load(std::memory_order_relaxed)) / 1000.0,
            e.draws.load(std::memory_order_relaxed),
            e.indices.load(std::memory_order_relaxed),
            e.draws_skipped.load(std::memory_order_relaxed),
            e.verification_syncs.load(std::memory_order_relaxed),
            double(e.verification_wait_us.load(std::memory_order_relaxed)) /
                1000.0);
      }
    }
  }
  {
    std::ofstream file(table_root_ /
                       fmt::format("{:08X}.textures.csv", title_id_));
    if (file) {
      file << "key_hash,format,dimension,width,height,depth_or_array,mips,"
              "scaled,host_bytes,creations,loads,loaded_guest_bytes,load_ms,"
              "binds,evictions,evictions_from_working_set\n";
      std::lock_guard<std::mutex> lock(textures_lock_);
      for (const auto& pair : textures_) {
        const TextureEntry& e = *pair.second;
        file << fmt::format(
            "{:016X},{},{},{},{},{},{},{},{},{},{},{},{:.1f},{},{},{}\n",
            pair.first, e.shape.format, e.shape.dimension, e.shape.width,
            e.shape.height, e.shape.depth_or_array_size, e.shape.mip_levels,
            e.shape.scaled_resolve ? 1 : 0, e.host_bytes,
            e.creations.load(std::memory_order_relaxed),
            e.loads.load(std::memory_order_relaxed),
            e.loaded_guest_bytes.load(std::memory_order_relaxed),
            double(e.load_us.load(std::memory_order_relaxed)) / 1000.0,
            e.binds.load(std::memory_order_relaxed),
            e.evictions.load(std::memory_order_relaxed),
            e.evictions_from_working_set.load(std::memory_order_relaxed));
      }
    }
  }
  {
    std::ofstream file(table_root_ /
                       fmt::format("{:08X}.creations.csv", title_id_));
    if (file) {
      file << "ms,compilers_busy,pipeline_queue_depth,from_pool,pool_bytes,"
              "free_host_bytes,host_bytes,format,dimension,width,height,mips\n";
      std::lock_guard<std::mutex> lock(creation_events_lock_);
      for (const CreationEvent& e : creation_events_) {
        file << fmt::format("{:.2f},{},{},{},{},{},{},{},{},{},{},{}\n", e.ms,
                            e.context.compilers_busy, e.context.queue_depth,
                            e.context.from_pool ? 1 : 0, e.context.pool_bytes,
                            e.context.free_host_bytes, e.context.host_bytes,
                            e.context.format, e.context.dimension,
                            e.context.width, e.context.height,
                            e.context.mip_levels);
      }
    }
  }
  {
    // The question this file exists for: how many DIFFERENT fixed-function
    // states does the title actually draw in. If the list is short, the
    // pipeline count is driven by shader combinations alone, and only a
    // different way of shading brings it down.
    std::ofstream file(table_root_ /
                       fmt::format("{:08X}.states.csv", title_id_));
    if (file) {
      file << "render_state_hash,pipelines_using_it,state\n";
      std::lock_guard<std::mutex> lock(shape_lock_);
      for (const auto& pair : distinct_render_states_) {
        file << fmt::format("{:016X},{},\"{}\"\n", pair.first,
                            pair.second.first, pair.second.second);
      }
    }
  }
  {
    std::ofstream file(table_root_ /
                       fmt::format("{:08X}.texture_shaders.csv", title_id_));
    if (file) {
      file << "texture_key_hash,distinct_shader_pairs\n";
      std::lock_guard<std::mutex> lock(texture_shader_lock_);
      for (const auto& pair : texture_shader_pairs_) {
        file << fmt::format("{:016X},{}\n", pair.first, pair.second.size());
      }
    }
  }
  {
    std::ofstream file(table_root_ /
                       fmt::format("{:08X}.objects.csv", title_id_));
    if (file) {
      file << "object_key,vertex_base,index_base,index_count,shader_pairs,"
              "states_wanted,states_ready,draws,draws_skipped\n";
      std::lock_guard<std::mutex> lock(objects_lock_);
      for (const auto& pair : objects_) {
        ObjectEntry& e = *pair.second;
        std::lock_guard<std::mutex> entry_lock(e.lock);
        file << fmt::format("{:016X},{:08X},{:08X},{},{},{},{},{},{}\n",
                            pair.first, e.vertex_base, e.index_base,
                            e.index_count, e.shader_pairs.size(),
                            e.states_wanted.size(), e.states_ready.size(),
                            e.draws.load(std::memory_order_relaxed),
                            e.draws_skipped.load(std::memory_order_relaxed));
      }
    }
  }
  XELOGI("GPU census: tables written to {}", xe::path_to_utf8(table_root_));
}

}  // namespace gpu
}  // namespace xe
