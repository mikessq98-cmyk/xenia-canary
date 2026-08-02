/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <new>  // std::bad_alloc - host OOM containment on the creation threads.
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#if XE_PLATFORM_WINRT
#include <io.h>  // _chsize_s, _fileno - for the crash-journal truncation.
#endif

#include "third_party/dxbc/DXBCChecksum.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/threading.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/dxbc.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/pipeline_util.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include "third_party/fmt/include/fmt/xchar.h"

DEFINE_bool(d3d12_dxbc_disasm, false,
            "Disassemble DXBC shaders after generation.", "D3D12");
DEFINE_bool(
    d3d12_dxbc_disasm_dxilconv, false,
    "Disassemble DXBC shaders after conversion to DXIL, if DXIL shaders are "
    "supported by the OS, and DirectX Shader Compiler DLLs available at "
    "https://github.com/microsoft/DirectXShaderCompiler/releases are present.",
    "D3D12");
DEFINE_int32(
    d3d12_pipeline_creation_threads, -1,
    "Number of threads used for graphics pipeline creation. -1 to calculate "
    "automatically (75% of logical CPU cores), a positive number to specify "
    "the number of threads explicitly (up to the number of logical CPU cores), "
    "0 to disable multithreaded pipeline creation.",
    "D3D12");
DEFINE_bool(d3d12_tessellation_wireframe, false,
            "Display tessellated surfaces as wireframe for debugging.",
            "D3D12");

DEFINE_bool(
    d3d12_no_early_depth_stencil_hint, false,
    "Never mark a pixel shader [earlydepthstencil], so it is translated once "
    "instead of once per alpha-test state.\n"
    "Whether the hint may be applied depends on the alpha test and "
    "alpha-to-coverage state at the time of the draw rather than on the shader "
    "itself, so a title that draws the same shader both ways gets two "
    "translations of it and two pipelines - and the draws needing the second "
    "one are skipped until it has been built. In the guest logs this is the "
    "largest single source of duplicate pixel shader modifications.\n"
    "The cost of turning this on is the early depth rejection the hint would "
    "have enabled, which is GPU time. Worth trying when the GPU has headroom "
    "and the stalls are pipeline creation, not shading.",
    "D3D12");

#if XE_PLATFORM_WINRT
DEFINE_bool(
    d3d12_pipeline_library, true,
    "Keep the driver's COMPILED pipelines in a D3D12 pipeline library on disk, "
    "next to the shader storage, and reuse them on later launches.\n"
    "The shader storage alone only records which pipelines a game needs - the "
    "driver still compiles every one of them from scratch on every launch, "
    "which on a console fills the creation queue with hundreds of entries "
    "while a level streams in and leaves draws without a pipeline to use. With "
    "the library, a second launch of the same game creates them almost "
    "instantly.\n"
    "The library is rejected by the runtime after a driver update (its "
    "contents are driver-specific); that is detected and it is simply rebuilt.",
    "D3D12");

DEFINE_bool(
    d3d12_async_vs_only_pipelines, true,
    "Xbox UWP: build VS-only pipelines (depth pre-pass, shadow maps, clears) "
    "on background threads, skipping their draws until they are ready.\n"
    "These are now queued AHEAD of every colour pipeline, so the wait is "
    "short and the depth buffer fills in quickly. Building them synchronously "
    "instead (false) guarantees complete depth from the very first frame, but "
    "blocks the GPU command processor while the console driver compiles - "
    "measured at up to 1.7 seconds in one stall, which is far worse than the "
    "artefact it prevents.\n"
    "Set to false only if black geometry still appears with the priority "
    "change in place.",
    "D3D12");

DEFINE_string(
    d3d12_substitute_pending_pipelines, "once",
    "Xbox UWP: what to do with a draw whose pipeline the (slow) console driver "
    "is still compiling. Skipping it shows up as black objects in multi-pass "
    "renderers (the depth pass is there, the material pass is not), but "
    "drawing with a stand-in means a briefly WRONG pixel shader - different "
    "lighting or texture slots - so this is a trade between two artefacts.\n"
    "  off    - skip the draw (upstream behaviour): no wrong shading ever, "
    "black objects during a compile burst.\n"
    "  once   - look for a stand-in when the pipeline is first needed and use "
    "it if one already exists (default). Rarely finds one, so it is nearly "
    "artefact-free while still covering the repeated permutations.\n"
    "  always - keep looking every submission until a stand-in is found. "
    "Covers far more draws, at the cost of visibly wrong shading on them.\n"
    "A stand-in always has the same root signature, vertex shader and render "
    "state, and writes the same render targets; only the pixel shader itself "
    "differs. The real pipeline takes over as soon as it is ready.",
    "D3D12");

DEFINE_bool(
    d3d12_toxic_shader_solver, true,
    "Xbox UWP: automatically detect and permanently skip graphics pipelines "
    "whose creation hard-crashes the GPU driver. Each pipeline creation is "
    "bracketed by a per-game crash journal (<cache>/shaders/<title>."
    "d3d12.inflight); a pair left behind by a crash is promoted, on the next "
    "launch, to the per-game skip list (<cache>/shaders/<title>.d3d12.toxic). "
    "Disable to always attempt every pipeline.",
    "D3D12");
#endif  // XE_PLATFORM_WINRT

// Shader translation differs with resolution scaling, so the solver keeps
// scale-specific state (see SolverInitialize).
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);

namespace xe {
namespace gpu {
namespace d3d12 {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/adaptive_triangle_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/float24_round_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/float24_truncate_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/tessellation_adaptive_vs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/tessellation_indexed_vs.h"
}  // namespace shaders

PipelineCache::PipelineCache(D3D12CommandProcessor& command_processor,
                             const RegisterFile& register_file,
                             const D3D12RenderTargetCache& render_target_cache,
                             bool bindless_resources_used)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      bindless_resources_used_(bindless_resources_used) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();

  bool edram_rov_used = render_target_cache.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
      !(edram_rov_used ||
        render_target_cache_.gamma_render_target_as_unorm16()),
      render_target_cache_.msaa_2x_supported(),
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y(),
      provider.GetGraphicsAnalysis() != nullptr);

  if (edram_rov_used) {
    depth_only_pixel_shader_ =
        std::move(shader_translator_->CreateDepthOnlyPixelShader());
  }
}

PipelineCache::~PipelineCache() { Shutdown(); }

#if XE_PLATFORM_WINRT
static PipelineCache::SubstituteMode ParseSubstituteMode(
    const std::string& value) {
  std::string lower;
  lower.reserve(value.size());
  for (char c : value) {
    lower.push_back(char(std::tolower(uint8_t(c))));
  }
  if (lower == "off" || lower == "false" || lower == "0") {
    return PipelineCache::SubstituteMode::kOff;
  }
  if (lower == "always" || lower == "true" || lower == "1") {
    return PipelineCache::SubstituteMode::kAlways;
  }
  return PipelineCache::SubstituteMode::kOnce;
}
#endif  // XE_PLATFORM_WINRT

bool PipelineCache::Initialize() {
#if XE_PLATFORM_WINRT
  substitute_mode_ =
      ParseSubstituteMode(cvars::d3d12_substitute_pending_pipelines);
#endif  // XE_PLATFORM_WINRT
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();

  // Initialize the command processor thread DXIL objects.
  dxbc_converter_ = nullptr;
  dxc_utils_ = nullptr;
  dxc_compiler_ = nullptr;
  if (cvars::d3d12_dxbc_disasm_dxilconv) {
    if (FAILED(provider.DxbcConverterCreateInstance(
            CLSID_DxbcConverter, IID_PPV_ARGS(&dxbc_converter_)))) {
      XELOGE(
          "Failed to create DxbcConverter, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcUtils,
                                          IID_PPV_ARGS(&dxc_utils_)))) {
      XELOGE(
          "Failed to create DxcUtils, converted DXIL disassembly for debugging "
          "will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcCompiler,
                                          IID_PPV_ARGS(&dxc_compiler_)))) {
      XELOGE(
          "Failed to create DxcCompiler, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
  }

  uint32_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }
  // Initialize creation thread synchronization data even if not using creation
  // threads because they may be used anyway to create pipelines from the
  // storage.
  creation_threads_busy_ = 0;
  creation_completion_event_ =
      xe::threading::Event::CreateManualResetEvent(true);
  assert_not_null(creation_completion_event_);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (cvars::d3d12_pipeline_creation_threads != 0) {
    size_t creation_thread_count;
    if (cvars::d3d12_pipeline_creation_threads < 0) {
      creation_thread_count =
          std::max(logical_processor_count * 3 / 4, uint32_t(1));
#if XE_PLATFORM_WINRT
      // Xbox Series: the app sees only ~6-7 cores and the emulator already
      // runs the guest CPU threads, the GPU command processor, audio and the
      // XMA decoder on them. 75% of cores compiling pipelines mid-game is
      // oversubscription that shows up as frame drops whenever a burst of new
      // pipelines arrives, even at below-normal priority. Three below-normal
      // compilers: two could not keep up with permutation-heavy titles (Black
      // Ops: 200+ pipelines backlogged and GROWING, every affected draw
      // skipped = black objects on screen); the blocking storage prewarm
      // spawns its own temporary extra threads anyway (when nothing else is
      // running), so initial load speed is unaffected.
      creation_thread_count = std::min(creation_thread_count, size_t(3));
#endif  // XE_PLATFORM_WINRT
      // While a level streams in, the queue goes hundreds deep and everything
      // in it is a draw waiting to be drawn - see
      // EnsureCreationThreadsForQueueDepth.
      creation_thread_burst_count_ =
          std::max(creation_thread_count,
                   size_t(std::max(logical_processor_count * 3 / 4,
                                   uint32_t(1))));
#if XE_PLATFORM_WINRT
      creation_thread_burst_count_ =
          std::min(creation_thread_burst_count_, size_t(5));
#endif  // XE_PLATFORM_WINRT
    } else {
      creation_thread_count =
          std::min(uint32_t(cvars::d3d12_pipeline_creation_threads),
                   logical_processor_count);
    }
    // What to come back to once a compilation burst is over.
    creation_thread_base_count_ = creation_thread_count;
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<xe::threading::Thread> creation_thread =
          xe::threading::Thread::Create({}, [this, i]() { CreationThread(i); });
      assert_not_null(creation_thread);
      creation_thread->set_name("D3D12 Pipelines");
#if XE_PLATFORM_WINRT
      // THREAD_PRIORITY_BELOW_NORMAL: on Xbox the app has ~6-7 cores for 30+
      // emulator threads, and a burst of driver shader compilation at normal
      // priority starves the guest CPU / GPU-emulation threads - the classic
      // hard stutter when entering a new area.
      creation_thread->set_priority(-1);
#endif  // XE_PLATFORM_WINRT
      creation_threads_.push_back(std::move(creation_thread));
    }
  }
  return true;
}

void PipelineCache::Shutdown() {
  // Shut down all threads, before destroying the pipelines since they may be
  // creating them.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      creation_threads_shutdown_from_ = 0;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      xe::threading::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  creation_completion_event_.reset();

  // Shut down the persistent shader / pipeline storage.
  ShutdownShaderStorage();

#if XE_PLATFORM_WINRT
  // Clean teardown (threads already joined above, so no creation is in flight):
  // discard the crash journal so a normal exit isn't mistaken for a crash.
  SolverShutdown(/*clean_exit=*/true);
#endif  // XE_PLATFORM_WINRT

  // Destroy all pipelines.
  current_pipeline_ = nullptr;
  for (auto it : pipelines_) {
    ID3D12PipelineState* state =
        it.second->state.load(std::memory_order_acquire);
    if (state) {
      state->Release();
    }
    delete it.second;
  }
#if XE_PLATFORM_WINRT
  substitute_index_.clear();
#endif  // XE_PLATFORM_WINRT
  pipelines_.clear();
  COUNT_profile_set("gpu/pipeline_cache/pipelines", 0);

  // Destroy all shaders.
  if (bindless_resources_used_) {
    bindless_sampler_layout_map_.clear();
    bindless_sampler_layouts_.clear();
  }
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();

  // Shut down shader translation.
  ui::d3d12::util::ReleaseAndNull(dxc_compiler_);
  ui::d3d12::util::ReleaseAndNull(dxc_utils_);
  ui::d3d12::util::ReleaseAndNull(dxbc_converter_);
}

void PipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  ShutdownShaderStorage();

  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig
      pipeline_config;
  pipeline_config.file_suffix =
      fmt::format(".{}.d3d12.xpso", edram_rov_used ? "rov" : "rtv");
  pipeline_config.api_magic = edram_rov_used ? 0x4F525844 : 0x54525844;
  pipeline_config.version =
      std::max(PipelineDescription::kVersion,
               DxbcShaderTranslator::Modification::kVersion);

  uint32_t storage_index = storage_writer_.storage_index() + 1;

  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
#if XE_PLATFORM_WINRT
  // The load and translate callbacks below both run synchronously inside this
  // call; while it runs, shaders_ is being populated and translated binaries
  // written, so the memory-pressure release (which iterates shaders_ and frees
  // binaries) must stand down.
  storage_translations_in_progress_.fetch_add(1, std::memory_order_acq_rel);
#endif  // XE_PLATFORM_WINRT
  bool storage_initialized = storage_writer_.InitializeShaderStorage(
      cache_root, title_id, pipeline_config,
      // Shader load callback.
      [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
          uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
        D3D12Shader* shader =
            LoadShader(type, ucode_dwords, ucode_dword_count, ucode_data_hash);
        if (shader->ucode_storage_index() == storage_index) {
          return true;  // Already loaded.
        }
        shader->set_ucode_storage_index(storage_index);
        return true;
      },
      // Shader translate callback.
      [this, edram_rov_used](
          const std::set<std::pair<uint64_t, uint64_t>>& translations_needed) {
        TranslateShadersForStorage(translations_needed, edram_rov_used);
      },
      pipeline_stored_descriptions);
#if XE_PLATFORM_WINRT
  storage_translations_in_progress_.fetch_sub(1, std::memory_order_acq_rel);
#endif  // XE_PLATFORM_WINRT
  if (!storage_initialized) {
    return;
  }
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;

#if XE_PLATFORM_WINRT
  // Load the per-game toxic-shader list and inspect the crash journal from the
  // previous run BEFORE any pipeline is created below, so learned-toxic pairs
  // are skipped during prewarming and safe mode (if needed) is armed.
  if (cvars::d3d12_toxic_shader_solver) {
    SolverInitialize(cache_root, title_id);
  }
#endif  // XE_PLATFORM_WINRT

  // Create the pipelines.
  if (!pipeline_stored_descriptions.empty()) {
    uint64_t pipeline_creation_start_ = xe::Clock::QueryHostTickCount();

    // Launch additional creation threads to use all cores to create
    // pipelines faster. Will also be using the main thread, so minus 1.
    size_t logical_processor_count = xe::threading::logical_processor_count();
    if (!logical_processor_count) {
      logical_processor_count = 6;
    }
    size_t creation_thread_original_count = creation_threads_.size();
    size_t creation_thread_needed_count = std::max(
        std::min(pipeline_stored_descriptions.size(), logical_processor_count) -
            size_t(1),
        creation_thread_original_count);
    while (creation_threads_.size() < creation_thread_needed_count) {
      size_t creation_thread_index = creation_threads_.size();
      std::unique_ptr<xe::threading::Thread> creation_thread =
          xe::threading::Thread::Create({}, [this, creation_thread_index]() {
            CreationThread(creation_thread_index);
          });
      assert_not_null(creation_thread);
      creation_thread->set_name("D3D12 Pipelines");
#if XE_PLATFORM_WINRT
      // THREAD_PRIORITY_BELOW_NORMAL: on Xbox the app has ~6-7 cores for 30+
      // emulator threads, and a burst of driver shader compilation at normal
      // priority starves the guest CPU / GPU-emulation threads - the classic
      // hard stutter when entering a new area.
      creation_thread->set_priority(-1);
#endif  // XE_PLATFORM_WINRT
      creation_threads_.push_back(std::move(creation_thread));
    }

    size_t pipelines_created = 0;
    size_t pipelines_already_exist = 0;
    size_t pipelines_vs_not_found = 0;
    size_t pipelines_vs_translation_missing = 0;
    size_t pipelines_ps_not_found = 0;
    size_t pipelines_ps_translation_missing = 0;
    size_t pipelines_root_sig_failed = 0;
    for (const PipelineStoredDescription& pipeline_stored_description :
         pipeline_stored_descriptions) {
      const PipelineDescription& pipeline_description =
          pipeline_stored_description.description;
      // TODO(Triang3l): On Vulkan, skip pipelines requiring unsupported device
      // features (to keep the cache files mostly shareable across devices).
      // Skip already known pipelines - those have already been enqueued.
      auto found_range =
          pipelines_.equal_range(pipeline_stored_description.description_hash);
      bool pipeline_found = false;
      for (auto it = found_range.first; it != found_range.second; ++it) {
        Pipeline* found_pipeline = it->second;
        if (!std::memcmp(&found_pipeline->description.description,
                         &pipeline_description, sizeof(pipeline_description))) {
          pipeline_found = true;
          break;
        }
      }
      if (pipeline_found) {
        ++pipelines_already_exist;
        continue;
      }

      PipelineRuntimeDescription pipeline_runtime_description;
      auto vertex_shader_it =
          shaders_.find(pipeline_description.vertex_shader_hash);
      if (vertex_shader_it == shaders_.end()) {
        ++pipelines_vs_not_found;
        XELOGW("Pipeline cache: VS {:016X} not found in shader storage",
               pipeline_description.vertex_shader_hash);
        continue;
      }
      D3D12Shader* vertex_shader = vertex_shader_it->second;
      pipeline_runtime_description.vertex_shader =
          static_cast<D3D12Shader::D3D12Translation*>(
              vertex_shader->GetTranslation(
                  pipeline_description.vertex_shader_modification));
      if (!pipeline_runtime_description.vertex_shader ||
          !pipeline_runtime_description.vertex_shader->is_translated() ||
          !pipeline_runtime_description.vertex_shader->is_valid()) {
        ++pipelines_vs_translation_missing;
        XELOGW(
            "Pipeline cache: VS {:016X} mod {:016X} translation "
            "missing/invalid",
            pipeline_description.vertex_shader_hash,
            pipeline_description.vertex_shader_modification);
        continue;
      }
      D3D12Shader* pixel_shader;
      if (pipeline_description.pixel_shader_hash) {
        auto pixel_shader_it =
            shaders_.find(pipeline_description.pixel_shader_hash);
        if (pixel_shader_it == shaders_.end()) {
          ++pipelines_ps_not_found;
          XELOGW("Pipeline cache: PS {:016X} not found in shader storage",
                 pipeline_description.pixel_shader_hash);
          continue;
        }
        pixel_shader = pixel_shader_it->second;
        pipeline_runtime_description.pixel_shader =
            static_cast<D3D12Shader::D3D12Translation*>(
                pixel_shader->GetTranslation(
                    pipeline_description.pixel_shader_modification));
        if (!pipeline_runtime_description.pixel_shader ||
            !pipeline_runtime_description.pixel_shader->is_translated() ||
            !pipeline_runtime_description.pixel_shader->is_valid()) {
          ++pipelines_ps_translation_missing;
          XELOGW(
              "Pipeline cache: PS {:016X} mod {:016X} translation "
              "missing/invalid",
              pipeline_description.pixel_shader_hash,
              pipeline_description.pixel_shader_modification);
          continue;
        }
      } else {
        pixel_shader = nullptr;
        pipeline_runtime_description.pixel_shader = nullptr;
      }
      GeometryShaderKey pipeline_geometry_shader_key;
      pipeline_runtime_description.geometry_shader =
          GetGeometryShaderKey(
              pipeline_description.geometry_shader,
              DxbcShaderTranslator::Modification(
                  pipeline_description.vertex_shader_modification),
              DxbcShaderTranslator::Modification(
                  pipeline_description.pixel_shader_modification),
              pipeline_geometry_shader_key)
              ? &GetGeometryShader(pipeline_geometry_shader_key)
              : nullptr;
      pipeline_runtime_description.root_signature =
          command_processor_.GetRootSignature(
              vertex_shader, pixel_shader,
              Shader::IsHostVertexShaderTypeDomain(
                  DxbcShaderTranslator::Modification(
                      pipeline_description.vertex_shader_modification)
                      .vertex.host_vertex_shader_type));
      if (!pipeline_runtime_description.root_signature) {
        ++pipelines_root_sig_failed;
        XELOGW(
            "Pipeline cache: Root signature failed for VS {:016X} PS {:016X}",
            pipeline_description.vertex_shader_hash,
            pipeline_description.pixel_shader_hash);
        continue;
      }
      std::memcpy(&pipeline_runtime_description.description,
                  &pipeline_description, sizeof(pipeline_description));

      Pipeline* new_pipeline = new Pipeline;
      std::memcpy(&new_pipeline->description, &pipeline_runtime_description,
                  sizeof(pipeline_runtime_description));
#if XE_PLATFORM_WINRT
      if (substitute_mode_ != SubstituteMode::kOff) {
        // Stored pipelines are the best substitutes there are - they are
        // ready before the game asks for anything.
        new_pipeline->substitute_key =
            ComputeSubstituteKey(pipeline_runtime_description);
        substitute_index_.emplace(new_pipeline->substitute_key, new_pipeline);
      }
#endif  // XE_PLATFORM_WINRT
      // Calculate priority based on whether shader writes to visible RTs.
      if (pixel_shader) {
        uint32_t bound_rts =
            (pipeline_description.render_targets[0].used ? 1 : 0) |
            (pipeline_description.render_targets[1].used ? 2 : 0) |
            (pipeline_description.render_targets[2].used ? 4 : 0) |
            (pipeline_description.render_targets[3].used ? 8 : 0);
        new_pipeline->priority = pipeline_util::CalculatePipelinePriority(
            bound_rts, pixel_shader->writes_color_targets(),
            pixel_shader->writes_depth());
      } else {
        // Depth pre-pass / shadow map / z-fill - first in the queue, see the
        // other priority assignment.
        new_pipeline->priority = pipeline_util::kPriorityNoPixelShader;
      }
      pipelines_.emplace(pipeline_stored_description.description_hash,
                         new_pipeline);
      COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());
      if (!creation_threads_.empty()) {
        // Submit the pipeline for creation to any available thread.
        {
          std::lock_guard<xe_mutex> lock(creation_request_lock_);
          AcquirePipelineTranslationsForCreation(new_pipeline);
          creation_queue_.push(new_pipeline);
        }
        creation_request_cond_.notify_one();
      } else {
        new_pipeline->state.store(
            CreateD3D12Pipeline(pipeline_runtime_description),
            std::memory_order_release);
      }
      ++pipelines_created;
    }

    if (!creation_threads_.empty()) {
      if (blocking) {
        // Blocking mode: help drain the queue on this thread, then wait for
        // background threads to finish.
        CreateQueuedPipelinesOnProcessorThread();
        if (creation_threads_.size() > creation_thread_original_count) {
          {
            std::lock_guard<xe_mutex> lock(creation_request_lock_);
            creation_threads_shutdown_from_ = creation_thread_original_count;
            // Assuming the queue is empty because of
            // CreateQueuedPipelinesOnProcessorThread.
          }
          creation_request_cond_.notify_all();
          while (creation_threads_.size() > creation_thread_original_count) {
            xe::threading::Wait(creation_threads_.back().get(), false);
            creation_threads_.pop_back();
          }
          {
            // Cleanup so additional threads can be created later again.
            std::lock_guard<xe_mutex> lock(creation_request_lock_);
            creation_threads_shutdown_from_ = SIZE_MAX;
          }
        }
        // Wait for any background threads (including original ones) to finish
        // creating pipelines they may have popped from the queue. This ensures
        // all cached pipelines are fully created before the game starts,
        // populating the driver's shader cache.
        bool await_creation_completion_event;
        {
          std::lock_guard<xe_mutex> lock(creation_request_lock_);
          await_creation_completion_event = creation_threads_busy_ != 0;
          if (await_creation_completion_event) {
            creation_completion_event_->Reset();
            creation_completion_set_event_ = true;
          }
        }
        if (await_creation_completion_event) {
          creation_request_cond_.notify_one();
          xe::threading::Wait(creation_completion_event_.get(), false);
        }
      } else {
        // Non-blocking mode: let background threads handle all pipeline
        // creation. Store completion callback to be invoked when done.
        std::lock_guard<xe_mutex> lock(creation_request_lock_);
        if (creation_queue_.empty() && creation_threads_busy_ == 0) {
          // No work pending - callback will be invoked at end of function.
        } else {
          creation_completion_callback_ = std::move(completion_callback);
          completion_callback =
              nullptr;  // Prevent invocation at end of function
        }
      }
    }

    XELOGI(
        "Pipeline cache loaded: {} created, {} already exist, {} total stored",
        pipelines_created, pipelines_already_exist,
        pipeline_stored_descriptions.size());
    if (pipelines_vs_not_found || pipelines_vs_translation_missing ||
        pipelines_ps_not_found || pipelines_ps_translation_missing ||
        pipelines_root_sig_failed) {
      XELOGI(
          "Pipeline cache skipped: {} VS not found, {} VS translation missing, "
          "{} PS not found, {} PS translation missing, {} root sig failed",
          pipelines_vs_not_found, pipelines_vs_translation_missing,
          pipelines_ps_not_found, pipelines_ps_translation_missing,
          pipelines_root_sig_failed);
    }
    XELOGI("Pipeline creation took {} milliseconds",
           (xe::Clock::QueryHostTickCount() - pipeline_creation_start_) * 1000 /
               xe::Clock::QueryHostTickFrequency());
  }

  shader_storage_title_id_ = title_id;

  // Invoke completion callback if provided (for blocking mode or when no
  // background work was needed). For non-blocking mode with background work,
  // the callback is stored and invoked by CreationThread when done.
  if (completion_callback) {
    completion_callback();
  }
}

void PipelineCache::ShutdownShaderStorage() {
  // Persist the driver's compiled pipelines before anything else - this is
  // what spares the next launch the whole compilation.
  ShutdownPipelineLibrary();

  // Shut down the storage writer (closes files, stops write thread).
  storage_writer_.ShutdownShaderStorage();
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = 0;
}

#if XE_PLATFORM_WINRT
namespace {
// Parses one "<vs_hex> <ps_hex>" line of a solver file. Blank/`#` lines fail.
bool ParseSolverLine(const std::string& line, uint64_t& vs, uint64_t& ps) {
  if (line.empty() || line[0] == '#') {
    return false;
  }
  std::istringstream stream(line);
  stream >> std::hex >> vs >> ps;
  return !stream.fail();
}
}  // namespace

void PipelineCache::SolverInitialize(const std::filesystem::path& cache_root,
                                     uint32_t title_id) {
  // Re-entrant safe: close any journal handle left open by a prior game.
  {
    std::lock_guard<std::mutex> lock(solver_journal_mutex_);
    if (solver_journal_file_) {
      std::fclose(solver_journal_file_);
      solver_journal_file_ = nullptr;
    }
    solver_inflight_.clear();
  }
  solver_enabled_ = false;
  solver_journaling_ = false;
  solver_safe_mode_ = false;
  solver_device_lost_.store(false, std::memory_order_release);
  solver_oom_seen_.store(false, std::memory_order_release);
  solver_toxic_shaders_.clear();

  std::filesystem::path root = GetShaderStorageRoot(cache_root);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  // The solver state is per resolution scale: shaders are TRANSLATED
  // differently when scaling is active, so a pair whose translation crashes
  // the driver's compiler at 2x2 is usually perfectly fine at 1x1 (and vice
  // versa) - quarantines must not leak between scales.
  std::string solver_scale_suffix;
  if (cvars::draw_resolution_scale_x > 1 || cvars::draw_resolution_scale_y > 1) {
    solver_scale_suffix = fmt::format(".{}x{}", cvars::draw_resolution_scale_x,
                                      cvars::draw_resolution_scale_y);
  }
  InitializePipelineLibrary(root, title_id);

  solver_toxic_path_ =
      root / fmt::format("{:08X}.d3d12{}.toxic", title_id, solver_scale_suffix);
  solver_journal_path_ = root / fmt::format("{:08X}.d3d12{}.inflight", title_id,
                                            solver_scale_suffix);
  solver_running_path_ = root / fmt::format("{:08X}.d3d12{}.running", title_id,
                                            solver_scale_suffix);

  // Load the persistent per-game skip list (confirmed-toxic pairs).
  {
    std::ifstream toxic_file(solver_toxic_path_);
    std::string line;
    while (std::getline(toxic_file, line)) {
      uint64_t vs = 0, ps = 0;
      if (ParseSolverLine(line, vs, ps)) {
        solver_toxic_shaders_.emplace(vs, ps);
      }
    }
  }

  // A leftover ".running" marker means the previous run did NOT exit cleanly -
  // i.e. it crashed. We only pay for the per-creation crash journal while
  // recovering from such a crash; a normal run does no journaling at all (no
  // per-pipeline file I/O under a lock, so no contention on the creation
  // threads), only the cheap in-memory toxic lookup.
  const bool crashed_last_run = std::filesystem::exists(solver_running_path_, ec);
  solver_journaling_ = crashed_last_run;

  // Read any journal left behind by that crash (only meaningful if we crashed).
  std::vector<std::pair<uint64_t, uint64_t>> suspects;
  if (crashed_last_run) {
    std::ifstream journal_file(solver_journal_path_);
    std::string line;
    while (std::getline(journal_file, line)) {
      uint64_t vs = 0, ps = 0;
      if (ParseSolverLine(line, vs, ps)) {
        suspects.emplace_back(vs, ps);
      }
    }
  }

  // Promote ALL leftover suspects to the skip list. With creation serialized
  // in recovery runs there is exactly one; even in a fully parallel run there
  // can only be as many as there are creation threads (2 on the Xbox build).
  // Quarantining a possibly-innocent shader (invisible geometry for one
  // material) is a far better deal than another crash-restart cycle per
  // culprit - games with several toxic shaders were taking many restarts to
  // converge one-at-a-time. The .toxic file is plain text and can be edited to
  // un-quarantine a pair.
  if (!suspects.empty()) {
    XELOGW(
        "Toxic-shader solver: {} pipeline(s) were mid-creation when the "
        "device/process died last run - quarantining all of them in {}:",
        suspects.size(), xe::path_to_utf8(solver_toxic_path_));
    for (const std::pair<uint64_t, uint64_t>& s : suspects) {
      if (solver_toxic_shaders_.emplace(s.first, s.second).second) {
        SolverAppendToxic(s.first, s.second);
      }
      XELOGW("  quarantined VS {:016X}, PS {:016X}", s.first, s.second);
    }
  } else if (crashed_last_run) {
    // Crashed, but that run wasn't journaling yet (the first crash) - nothing
    // was recorded; this run will journal every creation.
    XELOGW(
        "Toxic-shader solver: previous run crashed with no journal - "
        "journaling and serializing pipeline creation this run to catch the "
        "culprit.");
  }
  // Any recovery run runs fully serialized, not just the ambiguous case: a
  // game can have SEVERAL toxic shaders, and serialization guarantees the next
  // death leaves exactly one suspect (no innocent bystanders), converging one
  // culprit per crash instead of needing extra runs to disambiguate.
  solver_safe_mode_ = solver_journaling_;

  // Mark this run as in progress; deleted on clean shutdown, so its presence at
  // the next launch is what signals a crash.
  { std::ofstream running_marker(solver_running_path_, std::ios::trunc); }

  // Only open the crash journal (which costs per-creation file I/O under a lock)
  // while actually recovering from a crash - a normal run journals nothing.
  if (solver_journaling_) {
    std::lock_guard<std::mutex> lock(solver_journal_mutex_);
    solver_journal_file_ = xe::filesystem::OpenFile(solver_journal_path_, "wb+");
    if (!solver_journal_file_) {
      XELOGW("Toxic-shader solver: couldn't open crash journal {}; detection "
             "disabled this run (known-toxic skipping still active)",
             xe::path_to_utf8(solver_journal_path_));
      solver_journaling_ = false;
    }
  }

  solver_enabled_ = true;
  XELOGI(
      "Toxic-shader solver active: {} known-toxic pair(s) will be skipped{}{}",
      solver_toxic_shaders_.size(),
      solver_journaling_ ? ", crash journaling ON (recovering)" : "",
      solver_safe_mode_ ? ", serialized safe mode ON" : "");
}

void PipelineCache::SolverOnDeviceLost() {
  if (!solver_device_lost_.exchange(true, std::memory_order_acq_rel)) {
    XELOGW(
        "Toxic-shader solver: device loss reported - the crash journal will "
        "be kept on shutdown");
  }
}

void PipelineCache::SolverQuarantineExecutionSuspect(
    uint64_t vertex_shader_hash, uint64_t pixel_shader_hash) {
  if (!solver_enabled_) {
    return;
  }
  SolverAppendToxic(vertex_shader_hash, pixel_shader_hash);
  XELOGW(
      "Toxic-shader solver: quarantined EXECUTION hang suspect VS {:016X}, "
      "PS {:016X} (the most recently bound pipeline when the device hung) - "
      "it will be skipped from the next launch; if the hang persists, the "
      "next suspect will be quarantined on the next death. Remove the pair "
      "from {} if it turns out innocent.",
      vertex_shader_hash, pixel_shader_hash,
      xe::path_to_utf8(solver_toxic_path_));
}

void PipelineCache::SolverShutdown(bool clean_exit) {
  std::lock_guard<std::mutex> lock(solver_journal_mutex_);
  if (solver_journal_file_) {
    std::fclose(solver_journal_file_);
    solver_journal_file_ = nullptr;
  }
  // A GRACEFUL device removal (process survives, the user quits via the
  // "device lost" message box) must still count as a crash for the solver:
  // the culprit's journal entry - possibly from a creation call that HUNG the
  // driver's shader compiler and never returned - is the only evidence, and a
  // "clean" shutdown would destroy it. Observed with dxbc_switch=true: a
  // specific in-game shader hangs newbe_xs.dll, the device is eventually
  // removed, and without this the suspect was erased on exit.
  if (clean_exit && solver_device_lost_.load(std::memory_order_acquire)) {
    XELOGW(
        "Toxic-shader solver: shutdown after device loss - keeping the crash "
        "journal so the culprit can be confirmed on the next launch");
    clean_exit = false;
  }
  if (clean_exit) {
    // No crash occurred - drop the in-progress marker and the journal so this
    // normal shutdown isn't treated as a crash next launch.
    std::error_code ec;
    if (!solver_running_path_.empty()) {
      std::filesystem::remove(solver_running_path_, ec);
    }
    if (!solver_journal_path_.empty()) {
      std::filesystem::remove(solver_journal_path_, ec);
    }
  }
  solver_inflight_.clear();
  solver_enabled_ = false;
  solver_journaling_ = false;
  solver_safe_mode_ = false;
}

bool PipelineCache::IsShaderToxic(uint64_t vertex_shader_hash,
                                  uint64_t pixel_shader_hash) const {
  if (solver_toxic_shaders_.empty()) {
    return false;
  }
  return solver_toxic_shaders_.find(
             std::make_pair(vertex_shader_hash, pixel_shader_hash)) !=
         solver_toxic_shaders_.end();
}

void PipelineCache::SolverRewriteJournalLocked() {
  if (!solver_journal_file_) {
    return;
  }
  std::string buffer;
  for (const std::pair<uint64_t, uint64_t>& s : solver_inflight_) {
    buffer += fmt::format("{:016X} {:016X}\n", s.first, s.second);
  }
  std::rewind(solver_journal_file_);
  if (!buffer.empty()) {
    std::fwrite(buffer.data(), 1, buffer.size(), solver_journal_file_);
  }
  // Push the CRT buffer to the OS so the data survives a process crash (the OS
  // still writes its cache to disk even though our process died); then shrink
  // the file to exactly what we wrote so stale trailing bytes aren't parsed.
  std::fflush(solver_journal_file_);
  _chsize_s(_fileno(solver_journal_file_),
            static_cast<__int64>(buffer.size()));
}

void PipelineCache::SolverJournalBegin(uint64_t vertex_shader_hash,
                                       uint64_t pixel_shader_hash) {
  std::lock_guard<std::mutex> lock(solver_journal_mutex_);
  solver_inflight_.emplace(vertex_shader_hash, pixel_shader_hash);
  SolverRewriteJournalLocked();
}

void PipelineCache::SolverJournalEnd(uint64_t vertex_shader_hash,
                                     uint64_t pixel_shader_hash) {
  std::lock_guard<std::mutex> lock(solver_journal_mutex_);
  solver_inflight_.erase(
      std::make_pair(vertex_shader_hash, pixel_shader_hash));
  SolverRewriteJournalLocked();
}

void PipelineCache::SolverAppendToxic(uint64_t vertex_shader_hash,
                                      uint64_t pixel_shader_hash) {
  // May be called from multiple creation threads (crash-catch path).
  std::lock_guard<std::mutex> lock(solver_journal_mutex_);
  std::ofstream toxic_file(solver_toxic_path_, std::ios::app);
  if (!toxic_file) {
    return;
  }
  toxic_file << fmt::format("{:016X} {:016X}\n", vertex_shader_hash,
                            pixel_shader_hash);
}

void PipelineCache::SolverRetractThisRunToxic() {
  // Out-of-memory was detected: compiler crashes quarantined EARLIER in this
  // run (before the first observed E_OUTOFMEMORY) were most likely also
  // out-of-memory victims. Rewrite the skip list with only the entries known
  // at startup, dropping everything appended during this session.
  std::lock_guard<std::mutex> lock(solver_journal_mutex_);
  std::ofstream toxic_file(solver_toxic_path_, std::ios::trunc);
  if (!toxic_file) {
    return;
  }
  for (const auto& pair : solver_toxic_shaders_) {
    toxic_file << fmt::format("{:016X} {:016X}\n", pair.first, pair.second);
  }
  XELOGW(
      "Toxic-shader solver: dropped the pairs quarantined during this "
      "out-of-memory session from {} (kept the {} known at startup)",
      xe::path_to_utf8(solver_toxic_path_), solver_toxic_shaders_.size());
}
#endif  // XE_PLATFORM_WINRT

void PipelineCache::EndSubmission() {
  // Periodically persist the pipeline library. A console application is often
  // terminated by the system rather than shut down cleanly, and everything
  // compiled since the last save would be lost - which would put the next
  // launch right back to compiling hundreds of pipelines. Serialization is not
  // cheap, so this is rare and only happens when something new was stored.
  if (pipeline_library_dirty_) {
    static constexpr uint32_t kSubmissionsPerLibrarySave = 600;
    if (++submissions_since_library_save_ >= kSubmissionsPerLibrarySave) {
      submissions_since_library_save_ = 0;
      SavePipelineLibrary();
    }
  }

  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_file_flush_needed_,
                                 pipeline_storage_file_flush_needed_);
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }
  // Releasing translated bytecode under memory pressure used to be decided
  // here, against this cache's own threshold. It is now the memory arbiter's
  // call (see GpuMemoryArbiter): five caches each polling the host and each
  // reacting at a different threshold is what made them take turns releasing
  // and re-claiming memory, and this one - the cheapest to rebuild - has to
  // be asked FIRST, which only something that sees all of them can do.
  if (!creation_threads_.empty()) {
    // Add compilers while the backlog is deep - everything in that queue is a
    // draw that gets skipped until it is built. Also decides how many threads
    // to wake: waking all of them on every submission is a thundering herd
    // when there is nothing much to build.
    bool backlog_deep = EnsureCreationThreadsForQueueDepth();
    // Don't wait for pipeline creation - let background threads work
    // asynchronously. Draws will be skipped until pipelines are ready.
    // This avoids frame-time spikes from blocking on pipeline creation.
    if (backlog_deep) {
      creation_request_cond_.notify_all();
    } else {
      creation_request_cond_.notify_one();
    }
  }
}

bool PipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<xe_mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

void PipelineCache::AwaitPipelineCompletion() {
  if (creation_threads_.empty()) {
    return;
  }

  bool await_creation_completion_event;
  {
    std::lock_guard<xe_mutex> lock(creation_request_lock_);
    await_creation_completion_event =
        !creation_queue_.empty() || creation_threads_busy_ != 0;
    if (await_creation_completion_event) {
      creation_completion_event_->Reset();
      creation_completion_set_event_ = true;
    }
  }

  if (await_creation_completion_event) {
    creation_request_cond_.notify_one();
#if XE_PLATFORM_WINRT
    // Bounded on the console. This waits for the WHOLE creation queue to
    // drain, not just for the pipeline the caller needs, and that queue
    // regularly holds a couple of hundred pipelines while a level streams in;
    // with the driver's compiler as slow as it is, an unbounded wait blocked
    // the GPU command processor for up to 1.7 seconds - measured - which is
    // what showed up as the picture freezing while the game itself kept
    // running, and as stale frames appearing over the new ones. The callers
    // (occlusion query readback, EndSubmission) only need this to reduce
    // racing against compilation, and all of them cope with it not having
    // finished - so give up after a frame's worth of time and let the
    // emulator keep presenting.
    xe::threading::Wait(creation_completion_event_.get(), false,
                        std::chrono::milliseconds(16));
#else
    xe::threading::Wait(creation_completion_event_.get(), false);
#endif  // XE_PLATFORM_WINRT
  }
}

ID3D12PipelineState* PipelineCache::AwaitD3D12PipelineByHandle(void* handle) {
  ID3D12PipelineState* pipeline = GetD3D12PipelineByHandle(handle);
  if (pipeline != nullptr) {
    return pipeline;
  }
  AwaitPipelineCompletion();
  return GetD3D12PipelineByHandle(handle);
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                       const uint32_t* host_address,
                                       uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(uint32_t)));
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                       const uint32_t* host_address,
                                       uint32_t dword_count,
                                       uint64_t data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  D3D12Shader* shader =
      new D3D12Shader(shader_type, data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

DxbcShaderTranslator::Modification
PipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                   xenos::PrimitiveType::kPointList);

  return modification;
}

DxbcShaderTranslator::Modification
PipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    // Whether this draw is native res due to a scale threshold. (RTV only)
    modification.pixel.resolution_scale_native =
        uint32_t(render_target_cache_.IsDrawScaleNative());

    using DepthStencilMode =
        DxbcShaderTranslator::Modification::DepthStencilMode;
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        normalized_depth_control.z_enable &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
            xenos::DepthRenderTargetFormat::kD24FS8) {
      modification.pixel.depth_stencil_mode =
          render_target_cache_.depth_float24_round()
              ? DepthStencilMode::kFloat24Rounding
              : DepthStencilMode::kFloat24Truncating;
    } else {
      if (!cvars::d3d12_no_early_depth_stencil_hint &&
          shader.implicit_early_z_write_allowed() &&
          (!shader.writes_color_target(0) ||
           !draw_util::DoesCoverageDependOnAlpha(
               regs.Get<reg::RB_COLORCONTROL>()))) {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
      } else {
        // Whether this hint applies depends on the alpha test state at the
        // time of the draw, not on the shader - so a title that draws the same
        // shader with the test on and off gets two translations of it and two
        // pipelines, and the draws using the second one are skipped until it
        // is built. Measured on the guest logs this is the single largest
        // source of duplicate pixel shader modifications: 8 of the 10 shaders
        // that had more than one in Dark Souls, and both of them in Black Ops.
        // Dropping the hint collapses those pairs into one pipeline, at the
        // cost of the early depth rejection it would have enabled - see the
        // cvar.
        modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }

    // Check if MIN/MAX blend is used with non-trivial source factors.
    // D3D12 fixed-function blend ignores factors for MIN/MAX, but Xbox 360
    // applies them. If the destination factor is ONE (or ZERO), we can
    // pre-multiply the shader output by the source factor to emulate this.
    // Only RT0 is supported for now.
    modification.pixel.rt0_blend_rgb_factor_for_premult =
        xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult =
        xenos::BlendFactor::kOne;

    if (shader.writes_color_target(0)) {
      auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
          reg::RB_BLENDCONTROL::rt_register_indices[0]);

      // Pre-multiply by kSrcAlpha for MIN/MAX blend ops when dstFactor is ONE.
      if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.color_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_rgb_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }

      if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_a_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }
    }
  }

  return modification;
}

bool PipelineCache::ConfigurePipeline(
    D3D12Shader::D3D12Translation* vertex_shader,
    D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    void** pipeline_handle_out, ID3D12RootSignature** root_signature_out) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  assert_not_null(pipeline_handle_out);
  assert_not_null(root_signature_out);

  // Ensure shaders are translated - needed now for GetCurrentStateDescription.
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);

  // Check if we should use async pipeline creation.
  // Normally VS-only pipelines (depth pre-pass, shadow maps, clears) are created
  // synchronously because they're cheap to compile on a desktop driver.
  bool background_vs_only = false;
#if XE_PLATFORM_WINRT
  background_vs_only = cvars::d3d12_async_vs_only_pipelines;
#endif  // XE_PLATFORM_WINRT
  bool use_async = cvars::async_shader_compilation && !creation_threads_.empty() &&
                   (pixel_shader != nullptr || background_vs_only);

  // Ensure VS ucode is analyzed (needed for description hash).
  if (!vertex_shader->shader().is_ucode_analyzed()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
  }

  // For async mode, defer VS translation to background thread.
  // For sync mode, translate VS now on main thread.
  if (!vertex_shader->is_translated() && !use_async) {
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader,
                                 dxbc_converter_, dxc_utils_, dxc_compiler_)) {
      XELOGE("Failed to translate the vertex shader!");
      return false;
    }
    if (storage_writer_.is_active() &&
        vertex_shader->shader().try_set_ucode_storage_index(
            storage_writer_.storage_index())) {
      shader_storage_file_flush_needed_ = true;
      storage_writer_.QueueShaderWrite(&vertex_shader->shader());
    }
  }
  if (!use_async && !vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid (sync mode only).
    return false;
  }

  if (pixel_shader != nullptr && !use_async) {
    // Sync mode - must translate PS now on main thread.
    // No mutex needed - main thread translator is not shared with background
    // threads (they have their own translators).
    if (!pixel_shader->is_translated()) {
      if (!pixel_shader->shader().is_ucode_analyzed()) {
        pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      }
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader,
                                   dxbc_converter_, dxc_utils_,
                                   dxc_compiler_)) {
        XELOGE("Failed to translate the pixel shader!");
        return false;
      }
      if (storage_writer_.is_active() &&
          pixel_shader->shader().try_set_ucode_storage_index(
              storage_writer_.storage_index())) {
        shader_storage_file_flush_needed_ = true;
        storage_writer_.QueueShaderWrite(&pixel_shader->shader());
      }
    }
    if (!pixel_shader->is_valid()) {
      // Translation attempted previously, but not valid.
      return false;
    }
  }

  PipelineRuntimeDescription runtime_description;
  if (!GetCurrentStateDescription(
          vertex_shader, pixel_shader, primitive_processing_result,
          normalized_depth_control, normalized_color_mask,
          bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, runtime_description,
          use_async)) {
    return false;
  }
  PipelineDescription& description = runtime_description.description;

  if (current_pipeline_ != nullptr &&
      current_pipeline_->description.description == description) {
    *pipeline_handle_out = current_pipeline_;
    *root_signature_out = current_pipeline_->description.root_signature;
    return true;
  }

  // Find an existing pipeline in the cache.
  uint64_t hash = XXH3_64bits(&description, sizeof(description));
  auto found_range = pipelines_.equal_range(hash);
  for (auto it = found_range.first; it != found_range.second; ++it) {
    Pipeline* found_pipeline = it->second;
    if (found_pipeline->description.description == description) {
      current_pipeline_ = found_pipeline;
      *pipeline_handle_out = found_pipeline;
      *root_signature_out = found_pipeline->description.root_signature;
      return true;
    }
  }

  Pipeline* new_pipeline = new Pipeline;
  std::memcpy(&new_pipeline->description, &runtime_description,
              sizeof(runtime_description));
  pipelines_.emplace(hash, new_pipeline);
  COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());

#if XE_PLATFORM_WINRT
  if (substitute_mode_ != SubstituteMode::kOff) {
    // Index every pipeline by what a stand-in must match, so a pipeline that
    // is still compiling can find a ready one to draw with (and so this one
    // can serve others once it is ready) in constant time per lookup.
    new_pipeline->substitute_key = ComputeSubstituteKey(runtime_description);
    substitute_index_.emplace(new_pipeline->substitute_key, new_pipeline);
    // Look now, while the pipeline is being queued: in "once" mode this is
    // the only search there will be.
    if (use_async) {
      auto range = substitute_index_.equal_range(new_pipeline->substitute_key);
      for (auto it = range.first; it != range.second; ++it) {
        Pipeline* candidate = it->second;
        if (candidate != new_pipeline &&
            candidate->state.load(std::memory_order_acquire) &&
            AreSubstitutable(candidate->description, runtime_description)) {
          new_pipeline->substitute.store(candidate, std::memory_order_release);
          break;
        }
      }
    }
  }
#endif  // XE_PLATFORM_WINRT

  if (use_async) {
    // Queue for background thread.
    new_pipeline->pending_vertex_shader = vertex_shader;
    new_pipeline->pending_pixel_shader = pixel_shader;
    // Calculate priority based on whether shader writes to visible RTs.
    if (pixel_shader) {
      uint32_t bound_rts = pipeline_util::GetBoundRTMaskFromNormalizedColorMask(
          normalized_color_mask);
      new_pipeline->priority = pipeline_util::CalculatePipelinePriority(
          bound_rts, pixel_shader->shader().writes_color_targets(),
          pixel_shader->shader().writes_depth());
    } else {
      // A pipeline with no pixel shader is a depth pre-pass, shadow map or
      // z-fill. It used to keep the default priority of zero, which put it
      // BEHIND every colour pipeline in the queue - so in a title that keeps
      // producing new colour permutations it was never reached, its draws
      // were skipped indefinitely, and the incomplete depth buffer turned the
      // geometry of later colour passes black. It goes first now.
      new_pipeline->priority = pipeline_util::kPriorityNoPixelShader;
    }
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      AcquirePipelineTranslationsForCreation(new_pipeline);
      creation_queue_.push(new_pipeline);
    }
    creation_request_cond_.notify_one();
  } else {
    // Sync mode or no creation threads: create synchronously.
    new_pipeline->state.store(CreateD3D12Pipeline(runtime_description),
                              std::memory_order_release);
  }

  if (storage_writer_.is_active()) {
    pipeline_storage_file_flush_needed_ = true;
    PipelineStoredDescription stored_description;
    stored_description.description_hash = hash;
    std::memcpy(&stored_description.description, &description,
                sizeof(description));
    storage_writer_.QueuePipelineWrite(stored_description);
  }

  current_pipeline_ = new_pipeline;
  *pipeline_handle_out = new_pipeline;
  *root_signature_out = runtime_description.root_signature;
  return true;
}

void PipelineCache::InitializePipelineLibrary(
    const std::filesystem::path& root, uint32_t title_id) {
  ShutdownPipelineLibrary();
  if (!cvars::d3d12_pipeline_library) {
    return;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  // Ask whether the driver supports pipeline libraries AT ALL before touching
  // one. Calling CreatePipelineLibrary on a driver that does not took the
  // console's device down with DXGI_ERROR_DRIVER_INTERNAL_ERROR before a
  // single draw - every later call, including the library creation itself,
  // then returned DXGI_ERROR_DEVICE_REMOVED. A capability query cannot do
  // that.
  D3D12_FEATURE_DATA_SHADER_CACHE shader_cache_support = {};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE,
                                         &shader_cache_support,
                                         sizeof(shader_cache_support))) ||
      !(shader_cache_support.SupportFlags &
        D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
    XELOGI(
        "Pipeline library: not supported by this driver (shader cache "
        "support 0x{:X}) - compiled pipelines will not be cached between "
        "launches",
        uint32_t(shader_cache_support.SupportFlags));
    return;
  }

  Microsoft::WRL::ComPtr<ID3D12Device1> device1;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
    XELOGW(
        "Pipeline library: ID3D12Device1 unavailable - compiled pipelines "
        "will not be cached between launches");
    return;
  }

  // Driver-compiled contents are specific to the shader translation, so the
  // resolution scale (which changes it) gets its own library, exactly like the
  // toxic-shader solver's state.
  std::string scale_suffix;
  if (cvars::draw_resolution_scale_x > 1 || cvars::draw_resolution_scale_y > 1) {
    scale_suffix = fmt::format(".{}x{}", cvars::draw_resolution_scale_x,
                               cvars::draw_resolution_scale_y);
  }
  pipeline_library_path_ =
      root / fmt::format("{:08X}.d3d12{}.pso_library", title_id, scale_suffix);

  // The blob must outlive the library - the runtime reads from it lazily, so
  // it is kept in pipeline_library_blob_ untouched until shutdown.
  {
    std::ifstream library_file(pipeline_library_path_,
                               std::ios::binary | std::ios::ate);
    if (library_file) {
      std::streamsize size = library_file.tellg();
      if (size > 0) {
        pipeline_library_blob_.resize(size_t(size));
        library_file.seekg(0);
        if (!library_file.read(
                reinterpret_cast<char*>(pipeline_library_blob_.data()), size)) {
          pipeline_library_blob_.clear();
        }
      }
    }
  }

  HRESULT hr = device1->CreatePipelineLibrary(
      pipeline_library_blob_.data(), pipeline_library_blob_.size(),
      IID_PPV_ARGS(&pipeline_library_));
  if (FAILED(hr)) {
    // E_INVALIDARG / D3D12_ERROR_DRIVER_VERSION_MISMATCH / ADAPTER_NOT_FOUND
    // all mean the same thing in practice: this blob was written by a
    // different driver or adapter and cannot be used. Start over rather than
    // giving up on caching.
    if (!pipeline_library_blob_.empty()) {
      XELOGI(
          "Pipeline library: the stored library is not usable by this driver "
          "(0x{:08X}) - rebuilding it",
          uint32_t(hr));
      pipeline_library_blob_.clear();
      hr = device1->CreatePipelineLibrary(nullptr, 0,
                                          IID_PPV_ARGS(&pipeline_library_));
    }
    if (FAILED(hr)) {
      XELOGW(
          "Pipeline library: unavailable (0x{:08X}) - the driver will compile "
          "every pipeline on every launch",
          uint32_t(hr));
      pipeline_library_.Reset();
      pipeline_library_blob_.clear();
      return;
    }
  }
  XELOGI("Pipeline library: {} ({} KB restored from {})",
         pipeline_library_blob_.empty() ? "started empty" : "loaded",
         pipeline_library_blob_.size() >> 10,
         xe::path_to_utf8(pipeline_library_path_));
}

void PipelineCache::SavePipelineLibrary() {
  std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
  if (!pipeline_library_ || !pipeline_library_dirty_ ||
      pipeline_library_path_.empty()) {
    return;
  }
  pipeline_library_dirty_ = false;
  size_t serialized_size = pipeline_library_->GetSerializedSize();
  if (!serialized_size) {
    return;
  }
  std::vector<uint8_t> serialized(serialized_size);
  if (FAILED(pipeline_library_->Serialize(serialized.data(),
                                          serialized_size))) {
    XELOGW("Pipeline library: serialization failed - not saving");
    return;
  }
  // Written through a temporary and moved into place: a partially written
  // library would be rejected on the next launch, throwing away everything.
  std::filesystem::path temp_path = pipeline_library_path_;
  temp_path += ".tmp";
  {
    std::ofstream library_file(temp_path, std::ios::binary | std::ios::trunc);
    if (!library_file ||
        !library_file.write(reinterpret_cast<const char*>(serialized.data()),
                            std::streamsize(serialized_size))) {
      XELOGW("Pipeline library: could not write {}",
             xe::path_to_utf8(temp_path));
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp_path, pipeline_library_path_, ec);
  if (ec) {
    std::filesystem::remove(pipeline_library_path_, ec);
    std::filesystem::rename(temp_path, pipeline_library_path_, ec);
  }
  XELOGI(
      "Pipeline library: saved {} KB ({} pipeline(s) served from it this run, "
      "{} compiled by the driver)",
      serialized_size >> 10,
      pipeline_library_hits_.load(std::memory_order_relaxed),
      pipeline_library_misses_.load(std::memory_order_relaxed));
}

void PipelineCache::ShutdownPipelineLibrary() {
  SavePipelineLibrary();
  {
    std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
    pipeline_library_.Reset();
    // Only safe to release once the library is gone.
    pipeline_library_blob_.clear();
    pipeline_library_blob_.shrink_to_fit();
    pipeline_library_path_.clear();
    pipeline_library_dirty_ = false;
  }
  pipeline_library_hits_.store(0, std::memory_order_relaxed);
  pipeline_library_misses_.store(0, std::memory_order_relaxed);
}

#if XE_PLATFORM_WINRT
uint64_t PipelineCache::ComputeSubstituteKey(
    const PipelineRuntimeDescription& runtime_description) {
  struct {
    ID3D12RootSignature* root_signature;
    D3D12Shader::D3D12Translation* vertex_shader;
    const std::vector<uint32_t>* geometry_shader;
    PipelineDescription description;
  } key_data;
  std::memset(&key_data, 0, sizeof(key_data));
  key_data.root_signature = runtime_description.root_signature;
  key_data.vertex_shader = runtime_description.vertex_shader;
  key_data.geometry_shader = runtime_description.geometry_shader;
  key_data.description = runtime_description.description;
  // The pixel shader identity is the one thing a substitute may differ in.
  key_data.description.pixel_shader_hash = 0;
  key_data.description.pixel_shader_modification = 0;
  return XXH3_64bits(&key_data, sizeof(key_data));
}

bool PipelineCache::AreSubstitutable(const PipelineRuntimeDescription& a,
                                     const PipelineRuntimeDescription& b) {
  if (a.root_signature != b.root_signature ||
      a.vertex_shader != b.vertex_shader ||
      a.geometry_shader != b.geometry_shader) {
    return false;
  }
  PipelineDescription da = a.description;
  PipelineDescription db = b.description;
  da.pixel_shader_hash = db.pixel_shader_hash = 0;
  da.pixel_shader_modification = db.pixel_shader_modification = 0;
  if (std::memcmp(&da, &db, sizeof(da)) != 0) {
    return false;
  }
  // The stand-in must also WRITE the same things. Two pixel shaders can share
  // a root signature and render state and still differ in whether they output
  // colour or depth at all - substituting across that turns a subtle "wrong
  // shading" into geometry that is missing or wrongly occluding.
  if (!a.pixel_shader != !b.pixel_shader) {
    return false;
  }
  if (a.pixel_shader && b.pixel_shader) {
    const Shader& sa = a.pixel_shader->shader();
    const Shader& sb = b.pixel_shader->shader();
    if (!sa.is_ucode_analyzed() || !sb.is_ucode_analyzed()) {
      // Without the analysis their outputs are unknown - don't guess.
      return false;
    }
    if (sa.writes_color_targets() != sb.writes_color_targets() ||
        sa.writes_depth() != sb.writes_depth()) {
      return false;
    }
    // And it must READ the same things. This is not a quality question - the
    // texture and sampler descriptors written for a draw are the ones the real
    // pixel shader asked for, via RequestTextures and the binding update. A
    // stand-in that samples anything else reads a descriptor that was never
    // filled in, which on this driver is a GPU page fault at VA 0 and takes
    // the device down (DXGI_ERROR_DEVICE_HUNG, seen in Black Ops).
    // It is also what turns objects into flat purple/green/red: a shader that
    // samples nothing standing in for a textured one shades with whatever its
    // own constants say.
    const D3D12Shader& da12 = static_cast<const D3D12Shader&>(sa);
    const D3D12Shader& db12 = static_cast<const D3D12Shader&>(sb);
    if (da12.GetUsedTextureMaskAfterTranslation() !=
        db12.GetUsedTextureMaskAfterTranslation()) {
      return false;
    }
    const auto& ta = da12.GetTextureBindingsAfterTranslation();
    const auto& tb = db12.GetTextureBindingsAfterTranslation();
    if (ta.size() != tb.size() ||
        (!ta.empty() && std::memcmp(ta.data(), tb.data(),
                                    ta.size() * sizeof(ta[0])) != 0)) {
      return false;
    }
    const auto& ssa = da12.GetSamplerBindingsAfterTranslation();
    const auto& ssb = db12.GetSamplerBindingsAfterTranslation();
    if (ssa.size() != ssb.size() ||
        (!ssa.empty() && std::memcmp(ssa.data(), ssb.data(),
                                     ssa.size() * sizeof(ssa[0])) != 0)) {
      return false;
    }
  }
  return true;
}

void* PipelineCache::GetReadySubstituteByHandle(void* handle) {
  if (substitute_mode_ == SubstituteMode::kOff) {
    return nullptr;
  }
  Pipeline* pipeline = reinterpret_cast<Pipeline*>(handle);
  Pipeline* substitute = pipeline->substitute.load(std::memory_order_acquire);
  if (substitute && substitute->state.load(std::memory_order_acquire)) {
    return substitute;
  }
  if (substitute_mode_ != SubstituteMode::kAlways) {
    // "once": only the stand-in found when this pipeline was first needed is
    // used. Retrying finds one for far more draws, but every one of those
    // draws is then shaded by the wrong pixel shader - which is the more
    // visible artefact of the two in practice.
    return nullptr;
  }
  // No substitute yet - retry, but at most once per submission per pipeline
  // so a burst of skipped draws can't turn into a search storm.
  uint64_t submission = command_processor_.GetCurrentSubmission();
  if (pipeline->substitute_search_submission == submission) {
    return nullptr;
  }
  pipeline->substitute_search_submission = submission;
  auto range = substitute_index_.equal_range(pipeline->substitute_key);
  bool rejected_any = false;
  for (auto it = range.first; it != range.second; ++it) {
    Pipeline* candidate = it->second;
    if (candidate == pipeline ||
        !candidate->state.load(std::memory_order_acquire)) {
      continue;
    }
    if (!AreSubstitutable(candidate->description, pipeline->description)) {
      rejected_any = true;
      continue;
    }
    pipeline->substitute.store(candidate, std::memory_order_release);
    return candidate;
  }
  if (rejected_any) {
    // Worth knowing how often the safety rules turn a stand-in down: every one
    // of these is a draw that gets skipped instead of being drawn wrongly (or
    // taking the device down). If this dwarfs the substitution count, the
    // feature is not earning its keep for that title.
    uint32_t n = substitutes_rejected_.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || ((n + 1) % 10000) == 0) {
      XELOGI(
          "Pipeline substitution: {} candidates rejected as unsafe (they read "
          "or write different resources than the draw bound)",
          n + 1);
    }
  }
  return nullptr;
}
#endif  // XE_PLATFORM_WINRT

bool PipelineCache::TranslateAnalyzedShader(
    DxbcShaderTranslator& translator,
    D3D12Shader::D3D12Translation& translation, IDxbcConverter* dxbc_converter,
    IDxcUtils* dxc_utils, IDxcCompiler* dxc_compiler) {
  D3D12Shader& shader = static_cast<D3D12Shader&>(translation.shader());

  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!translator.TranslateAnalyzedShader(translation)) {
    XELOGE("Shader {:016X} translation failed; marking as ignored",
           shader.ucode_data_hash());
    return false;
  }

  const char* host_shader_type;
  if (shader.type() == xenos::ShaderType::kVertex) {
    DxbcShaderTranslator::Modification modification(translation.modification());
    switch (modification.vertex.host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kLineDomainCPIndexed:
        host_shader_type = "control-point-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
        host_shader_type = "patch-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
        host_shader_type = "control-point-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        host_shader_type = "patch-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
        host_shader_type = "control-point-indexed quad domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        host_shader_type = "patch-indexed quad domain";
        break;
      default:
        assert(modification.vertex.host_vertex_shader_type ==
               Shader::HostVertexShaderType::kVertex);
        host_shader_type = "vertex";
    }
  } else {
    host_shader_type = "pixel";
  }
  XELOGGPU("Generated {} shader ({}b) - hash {:016X}:\n{}\n", host_shader_type,
           shader.ucode_dword_count() * sizeof(uint32_t),
           shader.ucode_data_hash(), shader.ucode_disassembly().c_str());

  // Set up texture and sampler binding layouts.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    const std::vector<D3D12Shader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    const std::vector<D3D12Shader::SamplerBinding>& sampler_bindings =
        shader.GetSamplerBindingsAfterTranslation();
    size_t sampler_binding_count = sampler_bindings.size();
    assert_false(bindless_resources_used_ &&
                 texture_binding_count + sampler_binding_count >
                     D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4);
    size_t texture_binding_layout_bytes =
        texture_binding_count * sizeof(*texture_bindings.data());
    uint64_t texture_binding_layout_hash = 0;
    if (texture_binding_count) {
      texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
    }
    size_t bindless_sampler_count =
        bindless_resources_used_ ? sampler_binding_count : 0;
    uint64_t bindless_sampler_layout_hash = 0;
    if (bindless_sampler_count) {
      XXH3_state_t hash_state;
      XXH3_64bits_reset(&hash_state);
      for (size_t i = 0; i < bindless_sampler_count; ++i) {
        XXH3_64bits_update(
            &hash_state, &sampler_bindings[i].bindless_descriptor_index,
            sizeof(sampler_bindings[i].bindless_descriptor_index));
      }
      bindless_sampler_layout_hash = XXH3_64bits_digest(&hash_state);
    }
    // Obtain the unique IDs of binding layouts if there are any texture
    // bindings or bindless samplers, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    // Use sampler count for the bindful case because it's the only thing that
    // must be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for actual
    // samplers).
    static_assert(
        kLayoutUIDEmpty == 0,
        "Empty layout UID is assumed to be 0 because for bindful samplers, the "
        "UID is their count");
    size_t sampler_binding_layout_uid =
        bindless_resources_used_ ? kLayoutUIDEmpty : sampler_binding_count;
    if (texture_binding_count || bindless_sampler_count) {
      std::lock_guard<std::mutex> layouts_lock(layouts_mutex_);
      if (texture_binding_count) {
        auto found_range = texture_binding_layout_map_.equal_range(
            texture_binding_layout_hash);
        for (auto it = found_range.first; it != found_range.second; ++it) {
          if (it->second.vector_span_length == texture_binding_count &&
              !std::memcmp(texture_binding_layouts_.data() +
                               it->second.vector_span_offset,
                           texture_bindings.data(),
                           texture_binding_layout_bytes)) {
            texture_binding_layout_uid = it->second.uid;
            break;
          }
        }
        if (texture_binding_layout_uid == kLayoutUIDEmpty) {
          static_assert(
              kLayoutUIDEmpty == 0,
              "Layout UID is size + 1 because it's assumed that 0 is the UID "
              "for an empty layout");
          texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
          LayoutUID new_uid;
          new_uid.uid = texture_binding_layout_uid;
          new_uid.vector_span_offset = texture_binding_layouts_.size();
          new_uid.vector_span_length = texture_binding_count;
          texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                          texture_binding_count);
          std::memcpy(
              texture_binding_layouts_.data() + new_uid.vector_span_offset,
              texture_bindings.data(), texture_binding_layout_bytes);
          texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                              new_uid);
        }
      }
      if (bindless_sampler_count) {
        auto found_range = bindless_sampler_layout_map_.equal_range(
            sampler_binding_layout_uid);
        for (auto it = found_range.first; it != found_range.second; ++it) {
          if (it->second.vector_span_length != bindless_sampler_count) {
            continue;
          }
          sampler_binding_layout_uid = it->second.uid;
          const uint32_t* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + it->second.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            if (vector_bindless_sampler_layout[i] !=
                sampler_bindings[i].bindless_descriptor_index) {
              sampler_binding_layout_uid = kLayoutUIDEmpty;
              break;
            }
          }
          if (sampler_binding_layout_uid != kLayoutUIDEmpty) {
            break;
          }
        }
        if (sampler_binding_layout_uid == kLayoutUIDEmpty) {
          sampler_binding_layout_uid = bindless_sampler_layout_map_.size();
          LayoutUID new_uid;
          static_assert(
              kLayoutUIDEmpty == 0,
              "Layout UID is size + 1 because it's assumed that 0 is the UID "
              "for an empty layout");
          new_uid.uid = sampler_binding_layout_uid + 1;
          new_uid.vector_span_offset = bindless_sampler_layouts_.size();
          new_uid.vector_span_length = sampler_binding_count;
          bindless_sampler_layouts_.resize(new_uid.vector_span_offset +
                                           sampler_binding_count);
          uint32_t* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + new_uid.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            vector_bindless_sampler_layout[i] =
                sampler_bindings[i].bindless_descriptor_index;
          }
          bindless_sampler_layout_map_.emplace(bindless_sampler_layout_hash,
                                               new_uid);
        }
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);
    shader.SetSamplerBindingLayoutUserUID(sampler_binding_layout_uid);
  }

  // Disassemble the shader for dumping.
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  if (cvars::d3d12_dxbc_disasm_dxilconv) {
    translation.DisassembleDxbcAndDxil(provider, cvars::d3d12_dxbc_disasm,
                                       dxbc_converter, dxc_utils, dxc_compiler);
  } else {
    translation.DisassembleDxbcAndDxil(provider, cvars::d3d12_dxbc_disasm);
  }

  // Dump shader files if desired.
  if (!cvars::dump_shaders.empty()) {
    bool edram_rov_used = render_target_cache_.GetPath() ==
                          RenderTargetCache::Path::kPixelShaderInterlock;
    translation.Dump(cvars::dump_shaders,
                     (shader.type() == xenos::ShaderType::kPixel)
                         ? (edram_rov_used ? "d3d12_rov" : "d3d12_rtv")
                         : "d3d12");
  }

  if (translation.is_valid()) {
    // Track resident translated bytecode for telemetry / memory pressure.
    translated_shader_bytes_.fetch_add(translation.translated_binary().size(),
                                       std::memory_order_relaxed);
  }
  return translation.is_valid();
}

uint32_t PipelineCache::GetSharedInterpolatorMask(
    uint64_t vertex_shader_ucode_hash, uint32_t vertex_shader_writes,
    uint32_t pixel_shader_reads) {
  // Never export what the vertex shader doesn't produce.
  uint32_t wanted = vertex_shader_writes & pixel_shader_reads;

  // Most draws in a row use the same shaders, so answer those without going
  // near the map at all - this runs per draw.
  if (vertex_shader_ucode_hash == last_shared_interpolator_vs_hash_ &&
      (wanted & ~last_shared_interpolator_mask_) == 0) {
    return last_shared_interpolator_mask_;
  }

  auto it = shared_interpolator_masks_.find(vertex_shader_ucode_hash);
  if (it == shared_interpolator_masks_.end()) {
    it = shared_interpolator_masks_.emplace(vertex_shader_ucode_hash,
                                            SharedInterpolatorMask{wanted, 0})
             .first;
  } else if ((wanted & ~it->second.mask) != 0) {
    // Widening costs far more than a normal miss: it changes the vertex
    // shader's modification, so every pipeline already built with this vertex
    // shader has to be rebuilt, while just answering `wanted` (what stock
    // Xenia does, and equally correct) costs the single pipeline this draw
    // needs. Never pay the multiplied cost while a backlog is being compiled -
    // that is precisely when the frames are already late. The widening is not
    // lost, only deferred: the next draw that needs those interpolators asks
    // again, and once the queue is quiet it goes through.
    if (creation_queue_depth_hint_.load(std::memory_order_relaxed) >=
        kSharedInterpolatorDeferWidenQueueDepth) {
      ++shared_interpolator_widenings_deferred_;
      return wanted;
    }
    // The price is paid in pipelines: every one already built with this vertex
    // shader was built against the old modification and has to be built again,
    // and its draws are skipped until it is. The saving is the translations of
    // this vertex shader that the union avoids later. So count what it would
    // cost before agreeing to it - past a handful of pipelines, living with a
    // separate modification for this pixel shader is the cheaper answer.
    size_t pipelines_with_vs = 0;
    for (const auto& pipeline_pair : pipelines_) {
      const PipelineRuntimeDescription& desc = pipeline_pair.second->description;
      if (desc.vertex_shader &&
          desc.vertex_shader->shader().ucode_data_hash() ==
              vertex_shader_ucode_hash) {
        ++pipelines_with_vs;
      }
    }
    if (pipelines_with_vs > kSharedInterpolatorMaxPipelinesToRebuild) {
      ++shared_interpolator_widenings_refused_;
      if (shared_interpolator_widenings_refused_ <= 8 ||
          (shared_interpolator_widenings_refused_ % 100) == 0) {
        XELOGI(
            "Interpolators: not widening VS {:016X} from {:08X} to {:08X} - it "
            "would rebuild {} pipelines ({} refusals so far)",
            vertex_shader_ucode_hash, it->second.mask,
            it->second.mask | wanted, pipelines_with_vs,
            shared_interpolator_widenings_refused_);
      }
      return wanted;
    }
    XELOGI(
        "Interpolators: widening VS {:016X} from {:08X} to {:08X} (widening "
        "{} of {}), rebuilding {} pipelines; {} widenings deferred so far for "
        "a busy queue",
        vertex_shader_ucode_hash, it->second.mask,
        (it->second.widen_count + 1 >= kSharedInterpolatorMaxWidenings)
            ? vertex_shader_writes
            : (it->second.mask | wanted),
        it->second.widen_count + 1, kSharedInterpolatorMaxWidenings,
        pipelines_with_vs, shared_interpolator_widenings_deferred_);
    // Something reads an interpolator not exported yet, so the mask has to
    // widen - and widening RETRANSLATES the vertex shader and rebuilds every
    // pipeline using it. Doing that repeatedly, a few bits at a time, would
    // put a burst of compilation right where the scene is already streaming.
    // After a couple of widenings, settle on everything the vertex shader
    // writes instead: it exports a few interpolators nobody reads, but this
    // shader has demonstrably no stable layout, and it will never be
    // retranslated for interpolators again.
    if (++it->second.widen_count >= kSharedInterpolatorMaxWidenings) {
      it->second.mask = vertex_shader_writes;
    } else {
      it->second.mask |= wanted;
    }
  }

  last_shared_interpolator_vs_hash_ = vertex_shader_ucode_hash;
  last_shared_interpolator_mask_ = it->second.mask;
  return it->second.mask;
}

bool PipelineCache::EnsureCreationThreadsForQueueDepth() {
  // Steady state uses few threads on purpose: the console has ~6-7 usable
  // cores carrying 30+ emulator threads, and compilation competing with them
  // is felt as stutter. But when a level streams in, the queue goes hundreds
  // deep and every entry in it is a draw that will be skipped until it is
  // built - there, finishing sooner matters more, and the threads run at
  // below-normal priority so they still yield to the guest.
  size_t queue_depth;
  {
    std::lock_guard<xe_mutex> lock(creation_request_lock_);
    queue_depth = creation_queue_.size();
  }
  creation_queue_depth_hint_.store(uint32_t(std::min<size_t>(queue_depth, ~0u)),
                                   std::memory_order_relaxed);
  bool backlog_deep = queue_depth >= kCreationQueueBurstDepth;
  if (cvars::d3d12_pipeline_creation_threads >= 0) {
    // An explicit count is the user's decision - don't override it.
    return backlog_deep;
  }
  size_t wanted_threads = creation_threads_.size();
  if (backlog_deep) {
    wanted_threads = creation_thread_burst_count_;
    creation_threads_idle_submissions_ = 0;
  } else if (creation_threads_.size() > creation_thread_base_count_) {
    // The burst is over. Threads spawned for it must not stay: they are extra
    // runnable threads on a console with ~6-7 usable cores already carrying
    // 30+ emulator threads, and keeping them costs the guest even while they
    // have nothing to build. Wait a while first so a stuttering backlog
    // doesn't make them come and go every few submissions, and require a
    // completely empty queue - joining a thread that is inside the driver's
    // compiler would block the submission for the whole compilation.
    if (queue_depth != 0) {
      creation_threads_idle_submissions_ = 0;
      return false;
    }
    if (++creation_threads_idle_submissions_ >=
        kCreationThreadIdleSubmissions) {
      creation_threads_idle_submissions_ = 0;
      size_t shut_down_from = creation_thread_base_count_;
      {
        std::lock_guard<xe_mutex> lock(creation_request_lock_);
        creation_threads_shutdown_from_ = shut_down_from;
      }
      creation_request_cond_.notify_all();
      for (size_t i = shut_down_from; i < creation_threads_.size(); ++i) {
        // They are waiting on the condition variable with nothing queued, so
        // this returns as soon as each observes the shutdown index.
        xe::threading::Wait(creation_threads_[i].get(), false);
      }
      creation_threads_.resize(shut_down_from);
      {
        std::lock_guard<xe_mutex> lock(creation_request_lock_);
        creation_threads_shutdown_from_ = SIZE_MAX;
      }
      XELOGI(
          "Pipeline cache: back to {} creation threads, the backlog is gone",
          creation_threads_.size());
    }
    return false;
  }
  if (wanted_threads <= creation_threads_.size()) {
    return backlog_deep;
  }
  while (creation_threads_.size() < wanted_threads) {
    size_t creation_thread_index = creation_threads_.size();
    std::unique_ptr<xe::threading::Thread> creation_thread =
        xe::threading::Thread::Create({}, [this, creation_thread_index]() {
          CreationThread(creation_thread_index);
        });
    if (!creation_thread) {
      break;
    }
    creation_thread->set_name("D3D12 Pipelines");
#if XE_PLATFORM_WINRT
    creation_thread->set_priority(-1);
#endif  // XE_PLATFORM_WINRT
    creation_threads_.push_back(std::move(creation_thread));
  }
  XELOGI(
      "Pipeline cache: {} creation threads while {} pipelines are queued",
      creation_threads_.size(), queue_depth);
  return backlog_deep;
}

void PipelineCache::PrioritizePipelineForPendingDraw(void* handle) {
  Pipeline* pipeline = reinterpret_cast<Pipeline*>(handle);
  if (pipeline->state.load(std::memory_order_acquire)) {
    return;
  }
  if (pipeline->priority >= pipeline_util::kPriorityPendingDraw) {
    // Already at the front - re-queueing it every skipped draw would fill the
    // queue with duplicates of the same pipeline.
    return;
  }
  {
    std::lock_guard<xe_mutex> lock(creation_request_lock_);
    if (pipeline->state.load(std::memory_order_acquire) ||
        pipeline->priority >= pipeline_util::kPriorityPendingDraw) {
      return;
    }
    pipeline->priority = pipeline_util::kPriorityPendingDraw;
    // std::priority_queue cannot re-sort in place, so the pipeline is pushed
    // again with its new priority. The duplicate is harmless: whichever copy
    // is popped first builds it, and the creation path skips a pipeline that
    // already has a state object.
    AcquirePipelineTranslationsForCreation(pipeline);
    creation_queue_.push(pipeline);
  }
  creation_request_cond_.notify_one();
}

uint64_t PipelineCache::ReleaseTranslationsForArbiter(uint64_t bytes_to_free) {
  // The storage loader translates without going through the creation queue, so
  // its translations can't be tracked - stay out of its way entirely.
  if (storage_translations_in_progress_.load(std::memory_order_acquire)) {
    return 0;
  }
  // Everything else is safe with creation_request_lock_ held: translations
  // queued for or being used by a creation hold a reference (taken and dropped
  // under this lock), and every other translation has no reader. This used to
  // bail out whenever any creation was in flight, which meant it did nothing
  // during a compilation storm - the one moment the memory is needed.
  std::lock_guard<xe_mutex> lock(creation_request_lock_);
  size_t released_translations = 0;
  size_t released_bytes = 0;
  size_t kept_in_creation = 0;
  for (auto& shader_pair : shaders_) {
    for (const auto& translation_pair : shader_pair.second->translations()) {
      auto translation = static_cast<D3D12Shader::D3D12Translation*>(
          translation_pair.second);
      if (!translation->is_translated()) {
        continue;
      }
      size_t binary_size = translation->translated_binary().size();
      if (!binary_size) {
        continue;
      }
      if (translation->is_referenced_by_creation()) {
        ++kept_in_creation;
        continue;
      }
      translation->ReleaseTranslationForMemoryPressure();
      ++released_translations;
      released_bytes += binary_size;
      if (released_bytes >= bytes_to_free) {
        // Enough. Dropping everything - which this used to do - is what makes
        // the trim a loop: the title retranslates every shader it still draws
        // with over the next frames, memory climbs back to where it was, and
        // the next trim drops it all again. Free the shortfall and no more.
        break;
      }
    }
    if (released_bytes >= bytes_to_free) {
      break;
    }
  }
  if (released_translations) {
    translated_shader_bytes_.fetch_sub(released_bytes,
                                       std::memory_order_relaxed);
    XELOGI(
        "Pipeline cache: released {} KB of translated shader bytecode ({} "
        "translations, {} kept as they are being compiled); shaders will be "
        "retranslated on demand (expect brief pop-in)",
        released_bytes >> 10, released_translations, kept_in_creation);
  }
  return released_bytes;
}

void PipelineCache::AcquirePipelineTranslationsForCreation(Pipeline* pipeline) {
  if (pipeline->description.vertex_shader) {
    pipeline->description.vertex_shader->AcquireForCreation();
  }
  if (pipeline->description.pixel_shader) {
    pipeline->description.pixel_shader->AcquireForCreation();
  }
}

void PipelineCache::ReleasePipelineTranslationsFromCreation(
    Pipeline* pipeline) {
  if (pipeline->description.vertex_shader) {
    pipeline->description.vertex_shader->ReleaseFromCreation();
  }
  if (pipeline->description.pixel_shader) {
    pipeline->description.pixel_shader->ReleaseFromCreation();
  }
}

void PipelineCache::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
    bool edram_rov_used) {
  uint64_t translation_start = xe::Clock::QueryHostTickCount();

  std::vector<D3D12Shader*> shaders_to_translate_list;
  shaders_to_translate_list.reserve(shaders_.size());
  for (auto& shader_pair : shaders_) {
    D3D12Shader* shader = shader_pair.second;
    uint64_t ucode_data_hash = shader->ucode_data_hash();
    if (translations_needed.lower_bound(
            std::make_pair(ucode_data_hash, uint64_t(0))) !=
        translations_needed.upper_bound(
            std::make_pair(ucode_data_hash, UINT64_MAX))) {
      shaders_to_translate_list.push_back(shader);
    }
  }

  if (shaders_to_translate_list.empty()) {
    return;
  }

  std::atomic<size_t> translation_index{0};
  std::mutex shaders_failed_to_translate_mutex;
  std::vector<D3D12Shader::D3D12Translation*> shaders_failed_to_translate;

  auto translate_function = [&, this]() {
    const ui::d3d12::D3D12Provider& provider =
        command_processor_.GetD3D12Provider();
    StringBuffer ucode_disasm_buffer;
    DxbcShaderTranslator translator(
        provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
        !(edram_rov_used ||
          render_target_cache_.gamma_render_target_as_unorm16()),
        render_target_cache_.msaa_2x_supported(),
        render_target_cache_.draw_resolution_scale_x(),
        render_target_cache_.draw_resolution_scale_y(),
        provider.GetGraphicsAnalysis() != nullptr);
    // DXIL conversion objects.
    IDxbcConverter* dxbc_converter = nullptr;
    IDxcUtils* dxc_utils = nullptr;
    IDxcCompiler* dxc_compiler = nullptr;
    if (cvars::d3d12_dxbc_disasm_dxilconv && dxbc_converter_ && dxc_utils_ &&
        dxc_compiler_) {
      provider.DxbcConverterCreateInstance(CLSID_DxbcConverter,
                                           IID_PPV_ARGS(&dxbc_converter));
      provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils));
      provider.DxcCreateInstance(CLSID_DxcCompiler,
                                 IID_PPV_ARGS(&dxc_compiler));
    }

    while (true) {
      size_t index = translation_index.fetch_add(1);
      if (index >= shaders_to_translate_list.size()) {
        break;
      }
      D3D12Shader* shader = shaders_to_translate_list[index];
      // Translation allocates (ucode analysis, the DXBC buffers); an OOM here
      // must not escape the thread function and terminate the process - the
      // shader simply stays untranslated and is retried on demand later.
      try {
        if (!shader->is_ucode_analyzed()) {
          shader->AnalyzeUcode(ucode_disasm_buffer);
        }
        uint64_t ucode_data_hash = shader->ucode_data_hash();
        for (auto modification_it = translations_needed.lower_bound(
                 std::make_pair(ucode_data_hash, uint64_t(0)));
             modification_it != translations_needed.end() &&
             modification_it->first == ucode_data_hash;
             ++modification_it) {
          D3D12Shader::D3D12Translation* translation =
              static_cast<D3D12Shader::D3D12Translation*>(
                  shader->GetOrCreateTranslation(modification_it->second));
          if (!translation->is_translated() &&
              !TranslateAnalyzedShader(translator, *translation, dxbc_converter,
                                       dxc_utils, dxc_compiler)) {
            std::lock_guard<std::mutex> lock(shaders_failed_to_translate_mutex);
            shaders_failed_to_translate.push_back(translation);
          }
        }
      } catch (const std::bad_alloc&) {
        static std::atomic<uint32_t> storage_oom_log_count{0};
        uint32_t n = storage_oom_log_count.fetch_add(1);
        if (n < 4) {
          XELOGE(
              "Shader storage translation ran OUT OF MEMORY on shader "
              "{:016X} - leaving it untranslated (it will be retried on "
              "demand)",
              shader->ucode_data_hash());
        }
      }
    }

    if (dxc_compiler) {
      dxc_compiler->Release();
    }
    if (dxc_utils) {
      dxc_utils->Release();
    }
    if (dxbc_converter) {
      dxbc_converter->Release();
    }
  };

  size_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }
  size_t thread_count = std::min(logical_processor_count - size_t(1),
                                 shaders_to_translate_list.size());
  std::vector<std::unique_ptr<xe::threading::Thread>> translation_threads;
  for (size_t i = 0; i < thread_count; ++i) {
    auto thread = xe::threading::Thread::Create({}, translate_function);
    if (thread) {
      thread->set_name("Shader Translation");
      translation_threads.push_back(std::move(thread));
    }
  }

  // Main thread also participates.
  translate_function();

  for (auto& thread : translation_threads) {
    xe::threading::Wait(thread.get(), false);
  }

  for (D3D12Shader::D3D12Translation* translation :
       shaders_failed_to_translate) {
    D3D12Shader* shader = static_cast<D3D12Shader*>(&translation->shader());
    shader->DestroyTranslation(translation->modification());
    if (shader->translations().empty()) {
      shaders_.erase(shader->ucode_data_hash());
      delete shader;
    }
  }

  XELOGI("Translated {} shaders in {} ms",
         shaders_to_translate_list.size() - shaders_failed_to_translate.size(),
         (xe::Clock::QueryHostTickCount() - translation_start) * 1000 /
             xe::Clock::QueryHostTickFrequency());
}

bool PipelineCache::GetCurrentStateDescription(
    D3D12Shader::D3D12Translation* vertex_shader,
    D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    PipelineRuntimeDescription& runtime_description_out, bool for_placeholder) {
  // Translated shaders needed at least for the root signature.
  // Exception: for_placeholder mode (async pipeline creation) allows
  // untranslated shaders - root signature uses VS bindings only initially,
  // updated after background translation.
  assert_true(for_placeholder ||
              (vertex_shader->is_translated() && vertex_shader->is_valid()));
  assert_true(!pixel_shader || for_placeholder ||
              (pixel_shader->is_translated() && pixel_shader->is_valid()));

  PipelineDescription& description_out = runtime_description_out.description;

  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  // Initialize all unused fields to zero for comparison/hashing.
  std::memset(&runtime_description_out, 0, sizeof(runtime_description_out));

  assert_true(DxbcShaderTranslator::Modification(vertex_shader->modification())
                  .vertex.host_vertex_shader_type ==
              primitive_processing_result.host_vertex_shader_type);
  bool tessellated = primitive_processing_result.IsTessellated();
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  // In Direct3D, rasterization (along with pixel counting) is disabled by
  // disabling the pixel shader and depth / stencil. However, if rasterization
  // should be disabled, the pixel shader must be disabled externally, to ensure
  // things like texture binding layout is correct for the shader actually being
  // used (don't replace anything here).
  if (!rasterization_enabled) {
    assert_null(pixel_shader);
    if (pixel_shader) {
      return false;
    }
  }

  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  // Root signature.
  // For placeholder mode, pass nullptr for pixel_shader since placeholder PS
  // has no texture/sampler bindings - root signature only needs VS bindings.
  runtime_description_out.root_signature = command_processor_.GetRootSignature(
      static_cast<const DxbcShader*>(&vertex_shader->shader()),
      (pixel_shader && !for_placeholder)
          ? static_cast<const DxbcShader*>(&pixel_shader->shader())
          : nullptr,
      tessellated);
  if (runtime_description_out.root_signature == nullptr) {
    return false;
  }

  // Vertex shader.
  runtime_description_out.vertex_shader = vertex_shader;
  description_out.vertex_shader_hash =
      vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();

  // Index buffer strip cut value.
  if (primitive_processing_result.host_primitive_reset_enabled) {
    description_out.strip_cut_index =
        primitive_processing_result.host_index_format ==
                xenos::IndexFormat::kInt16
            ? PipelineStripCutIndex::kFFFF
            : PipelineStripCutIndex::kFFFFFFFF;
  } else {
    description_out.strip_cut_index = PipelineStripCutIndex::kNone;
  }

  // Host vertex shader type and primitive topology.
  if (tessellated) {
    description_out.primitive_topology_type_or_tessellation_mode =
        uint32_t(primitive_processing_result.tessellation_mode);
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kPoint);
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      // Quads are emulated as line lists with adjacency.
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::k2DLineStrip:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kLine);
        break;
      default:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kTriangle);
        break;
    }
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.geometry_shader = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        description_out.geometry_shader =
            PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        description_out.geometry_shader = PipelineGeometryShader::kQuadList;
        break;
      default:
        description_out.geometry_shader = PipelineGeometryShader::kNone;
        break;
    }
  }
  GeometryShaderKey geometry_shader_key;
  runtime_description_out.geometry_shader =
      GetGeometryShaderKey(
          description_out.geometry_shader,
          DxbcShaderTranslator::Modification(vertex_shader->modification()),
          DxbcShaderTranslator::Modification(
              pixel_shader ? pixel_shader->modification() : 0),
          geometry_shader_key)
          ? &GetGeometryShader(geometry_shader_key)
          : nullptr;

  // The rest doesn't matter when rasterization is disabled (thus no writing to
  // anywhere from post-geometry stages and no samples are counted).
  if (!rasterization_enabled) {
    description_out.cull_mode = PipelineCullMode::kDisableRasterization;
    return true;
  }

  // Pixel shader.
  if (pixel_shader) {
    runtime_description_out.pixel_shader = pixel_shader;
    description_out.pixel_shader_hash =
        pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
  }

  // Rasterizer state.
  // Because Direct3D 12 doesn't support per-side fill mode and depth bias, the
  // values to use depends on the current culling state.
  // If front faces are culled, use the ones for back faces.
  // If back faces are culled, it's the other way around.
  // If culling is not enabled, assume the developer wanted to draw things in a
  // more special way - so if one side is wireframe or has a depth bias, then
  // that's intentional (if both sides have a depth bias, the one for the front
  // faces is used, though it's unlikely that they will ever be different -
  // SetRenderState sets the same offset for both sides).
  // Points fill mode (0) also isn't supported in Direct3D 12, but assume the
  // developer didn't want to fill the whole primitive and use wireframe (like
  // Xenos fill mode 1).
  // Here we also assume that only one side is culled - if two sides are culled,
  // rasterization will be disabled externally, or the draw call will be dropped
  // early if the vertex shader doesn't export to memory.
  bool cull_front, cull_back;
  if (primitive_polygonal) {
    description_out.front_counter_clockwise = pa_su_sc_mode_cntl.face == 0;
    cull_front = pa_su_sc_mode_cntl.cull_front != 0;
    cull_back = pa_su_sc_mode_cntl.cull_back != 0;
    if (cull_front) {
      // The case when both faces are culled should be handled by disabling
      // rasterization.
      assert_false(cull_back);
      description_out.cull_mode = PipelineCullMode::kFront;
    } else if (cull_back) {
      description_out.cull_mode = PipelineCullMode::kBack;
    } else {
      description_out.cull_mode = PipelineCullMode::kNone;
    }
    // With ROV, the depth bias is applied in the pixel shader because
    // per-sample depth is needed for MSAA.
    if (!cull_front) {
      // Front faces aren't culled.
      // Direct3D 12, unfortunately, doesn't support point fill mode.
      if (pa_su_sc_mode_cntl.polymode_front_ptype !=
          xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (!cull_back) {
      // Back faces aren't culled.
      if (pa_su_sc_mode_cntl.polymode_back_ptype !=
          xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
      description_out.fill_mode_wireframe = 0;
    }
  } else {
    // Filled front faces only, without culling.
    cull_front = false;
    cull_back = false;
  }
  if (!edram_rov_used) {
    float polygon_offset, polygon_offset_scale;
    draw_util::GetPreferredFacePolygonOffset(
        regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
    description_out.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
    description_out.depth_bias_slope_scaled =
        polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
    // The slope-scaled depth bias multiplier depends on this at pipeline
    // creation.
    description_out.resolution_scale_native =
        uint32_t(render_target_cache_.IsDrawScaleNative());
  }
  if (tessellated && cvars::d3d12_tessellation_wireframe) {
    description_out.fill_mode_wireframe = 1;
  }
  description_out.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  bool depth_stencil_bound_and_used = false;
  if (!edram_rov_used) {
    // Depth/stencil. No stencil, always passing depth test and no depth writing
    // means depth disabled.
    if (bound_depth_and_color_render_target_bits & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_func = normalized_depth_control.zfunc;
        description_out.depth_write = normalized_depth_control.z_write_enable;
      } else {
        description_out.depth_func = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_enable = 1;
        bool stencil_backface_enable =
            primitive_polygonal && normalized_depth_control.backface_enable;
        // Per-face masks not supported by Direct3D 12, choose the back face
        // ones only if drawing only back faces.
        Register stencil_ref_mask_reg;
        if (stencil_backface_enable && cull_front) {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
        }
        auto stencil_ref_mask =
            regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
        description_out.stencil_read_mask = stencil_ref_mask.stencilmask;
        description_out.stencil_write_mask = stencil_ref_mask.stencilwritemask;
        description_out.stencil_front_fail_op =
            normalized_depth_control.stencilfail;
        description_out.stencil_front_depth_fail_op =
            normalized_depth_control.stencilzfail;
        description_out.stencil_front_pass_op =
            normalized_depth_control.stencilzpass;
        description_out.stencil_front_func =
            normalized_depth_control.stencilfunc;
        if (stencil_backface_enable) {
          description_out.stencil_back_fail_op =
              normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_depth_fail_op =
              normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_pass_op =
              normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_func =
              normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op =
              description_out.stencil_front_fail_op;
          description_out.stencil_back_depth_fail_op =
              description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_pass_op =
              description_out.stencil_front_pass_op;
          description_out.stencil_back_func =
              description_out.stencil_front_func;
        }
      }
      // If not binding the DSV, ignore the format in the hash.
      if (description_out.depth_func != xenos::CompareFunction::kAlways ||
          description_out.depth_write || description_out.stencil_enable) {
        description_out.depth_format = xenos::DepthRenderTargetFormat(
            bound_depth_and_color_render_target_formats[0]);
        depth_stencil_bound_and_used = true;
      }
    } else {
      description_out.depth_func = xenos::CompareFunction::kAlways;
    }

    // Render targets and blending state. 32 because of 0x1F mask, for safety
    // (all unknown to zero).
    static constexpr PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kInvSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestColor,
        /*  9 */ PipelineBlendFactor::kInvDestColor,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        // CONSTANT_COLOR
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // Like kBlendFactorMap, but with color modes changed to alpha. Some
    // pipelines aren't created in 545407E0 because a color mode is used for
    // alpha.
    static constexpr PipelineBlendFactor kBlendFactorAlphaMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcAlpha,
        /*  5 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestAlpha,
        /*  9 */ PipelineBlendFactor::kInvDestAlpha,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // While it's okay to specify fewer render targets in the pipeline state
    // (even fewer than written by the shader) than actually bound to the
    // command list (though this kind of truncation may only happen at the end -
    // DXGI_FORMAT_UNKNOWN *requires* a null RTV descriptor to be bound), not
    // doing that because sample counts of all render targets bound via
    // OMSetRenderTargets, even those beyond NumRenderTargets, apparently must
    // have their sample count matching the one set in the pipeline - however if
    // we set NumRenderTargets to 0 and also disable depth / stencil, the sample
    // count must be set to 1 - while the command list may still have
    // multisampled render targets bound (happens in 4D5307E6 main menu).
    // TODO(Triang3l): Investigate interaction of OMSetRenderTargets with
    // non-null depth and DSVFormat DXGI_FORMAT_UNKNOWN in the same case.
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(bound_depth_and_color_render_target_bits &
            (uint32_t(1) << (1 + i)))) {
        continue;
      }
      PipelineRenderTarget& rt = description_out.render_targets[i];
      rt.used = 1;
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(
          reg::RB_COLOR_INFO::rt_register_indices[i]);
      rt.format = xenos::ColorRenderTargetFormat(
          bound_depth_and_color_render_target_formats[1 + i]);
      rt.write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
      if (rt.write_mask) {
        auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
            reg::RB_BLENDCONTROL::rt_register_indices[i]);
        rt.src_blend = kBlendFactorMap[uint32_t(blendcontrol.color_srcblend)];
        rt.dest_blend = kBlendFactorMap[uint32_t(blendcontrol.color_destblend)];
        rt.blend_op = blendcontrol.color_comb_fcn;
        rt.src_blend_alpha =
            kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_srcblend)];
        rt.dest_blend_alpha =
            kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_destblend)];
        rt.blend_op_alpha = blendcontrol.alpha_comb_fcn;
      } else {
        rt.src_blend = PipelineBlendFactor::kOne;
        rt.dest_blend = PipelineBlendFactor::kZero;
        rt.blend_op = xenos::BlendOp::kAdd;
        rt.src_blend_alpha = PipelineBlendFactor::kOne;
        rt.dest_blend_alpha = PipelineBlendFactor::kZero;
        rt.blend_op_alpha = xenos::BlendOp::kAdd;
      }
    }
  }
  xenos::MsaaSamples host_msaa_samples =
      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  if (edram_rov_used) {
    if (host_msaa_samples == xenos::MsaaSamples::k2X) {
      // 2 is not supported in ForcedSampleCount on Nvidia.
      host_msaa_samples = xenos::MsaaSamples::k4X;
    }
  } else {
    if (!(bound_depth_and_color_render_target_bits & ~uint32_t(1)) &&
        !depth_stencil_bound_and_used) {
      // Direct3D 12 requires the sample count to be 1 when no color or depth /
      // stencil render targets are bound.
      // FIXME(Triang3l): Use ForcedSampleCount or some other fallback for
      // sample counting when needed, though with 2x it will be as incorrect as
      // with 1x / 4x anyway; or bind a dummy depth / stencil buffer if really
      // needed.
      host_msaa_samples = xenos::MsaaSamples::k1X;
    }
    // TODO(Triang3l): 4x MSAA fallback when 2x isn't supported.
  }
  description_out.host_msaa_samples = host_msaa_samples;

  return true;
}

bool PipelineCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    DxbcShaderTranslator::Modification vertex_shader_modification,
    DxbcShaderTranslator::Modification pixel_shader_modification,
    GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  assert_true(vertex_shader_modification.vertex.interpolator_mask ==
              pixel_shader_modification.pixel.interpolator_mask);
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count =
      xe::bit_count(vertex_shader_modification.vertex.interpolator_mask);
  key.user_clip_plane_count =
      vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull =
      vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_size;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key_out = key;
  return true;
}

void PipelineCache::CreateDxbcGeometryShader(
    GeometryShaderKey key, std::vector<uint32_t>& shader_out) {
  shader_out.clear();

  // RDEF, ISGN, OSG5, SHEX, STAT.
  constexpr uint32_t kBlobCount = 5;

  // Allocate space for the container header and the blob offsets.
  shader_out.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) +
                    kBlobCount);
  uint32_t blob_offset_position_dwords =
      sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(shader_out.size());
  constexpr uint32_t kBlobHeaderSizeDwords =
      sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // shader_out.resize(rdef_position_dwords);

  // RDEF header - the actual definitions will be written if needed.
  shader_out.resize(rdef_position_dwords +
                    sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(shader_out, "Xenia");
  {
    auto& rdef_header = *reinterpret_cast<dxbc::RdefHeader*>(
        shader_out.data() + rdef_position_dwords);
    rdef_header.shader_model = dxbc::RdefShaderModel::kGeometryShader5_1;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    // Generator name is right after the header.
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k5_1;
    rdef_header.InitializeSizes();
  }

  uint32_t system_cbuffer_size_vector_aligned_bytes = 0;

  if (key.type == PipelineGeometryShader::kPointList) {
    // Need point parameters from the system constants.

    // Constant types - float2 only.
    // Names.
    name_ptr =
        uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_float2 = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "float2");
    // Types.
    uint32_t rdef_type_float2_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_type_float2_ptr =
        uint32_t((rdef_type_float2_position_dwords - rdef_position_dwords) *
                 sizeof(uint32_t));
    shader_out.resize(rdef_type_float2_position_dwords +
                      sizeof(dxbc::RdefType) / sizeof(uint32_t));
    {
      auto& rdef_type_float2 = *reinterpret_cast<dxbc::RdefType*>(
          shader_out.data() + rdef_type_float2_position_dwords);
      rdef_type_float2.variable_class = dxbc::RdefVariableClass::kVector;
      rdef_type_float2.variable_type = dxbc::RdefVariableType::kFloat;
      rdef_type_float2.row_count = 1;
      rdef_type_float2.column_count = 2;
      rdef_type_float2.name_ptr = rdef_name_ptr_float2;
    }

    // Constants:
    // - float2 xe_point_constant_diameter
    // - float2 xe_point_screen_diameter_to_ndc_radius
    enum PointConstant : uint32_t {
      kPointConstantConstantDiameter,
      kPointConstantScreenDiameterToNDCRadius,
      kPointConstantCount,
    };
    // Names.
    name_ptr =
        uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_point_constant_diameter = name_ptr;
    name_ptr +=
        dxbc::AppendAlignedString(shader_out, "xe_point_constant_diameter");
    uint32_t rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius = name_ptr;
    name_ptr += dxbc::AppendAlignedString(
        shader_out, "xe_point_screen_diameter_to_ndc_radius");
    // Constants.
    uint32_t rdef_constants_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_constants_ptr =
        uint32_t((rdef_constants_position_dwords - rdef_position_dwords) *
                 sizeof(uint32_t));
    shader_out.resize(rdef_constants_position_dwords +
                      sizeof(dxbc::RdefVariable) / sizeof(uint32_t) *
                          kPointConstantCount);
    {
      auto rdef_constants = reinterpret_cast<dxbc::RdefVariable*>(
          shader_out.data() + rdef_constants_position_dwords);
      // float2 xe_point_constant_diameter
      static_assert(
          sizeof(DxbcShaderTranslator::SystemConstants ::
                     point_constant_diameter) == sizeof(float) * 2,
          "DxbcShaderTranslator point_constant_diameter system constant size "
          "differs between the shader translator and geometry shader "
          "generation");
      static_assert_size(
          DxbcShaderTranslator::SystemConstants::point_constant_diameter,
          sizeof(float) * 2);
      dxbc::RdefVariable& rdef_constant_point_constant_diameter =
          rdef_constants[kPointConstantConstantDiameter];
      rdef_constant_point_constant_diameter.name_ptr =
          rdef_name_ptr_xe_point_constant_diameter;
      rdef_constant_point_constant_diameter.start_offset_bytes = offsetof(
          DxbcShaderTranslator::SystemConstants, point_constant_diameter);
      rdef_constant_point_constant_diameter.size_bytes = sizeof(float) * 2;
      rdef_constant_point_constant_diameter.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_constant_diameter.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_constant_diameter.start_texture = UINT32_MAX;
      rdef_constant_point_constant_diameter.start_sampler = UINT32_MAX;
      // float2 xe_point_screen_diameter_to_ndc_radius
      static_assert(
          sizeof(DxbcShaderTranslator::SystemConstants ::
                     point_screen_diameter_to_ndc_radius) == sizeof(float) * 2,
          "DxbcShaderTranslator point_screen_diameter_to_ndc_radius system "
          "constant size differs between the shader translator and geometry "
          "shader generation");
      dxbc::RdefVariable& rdef_constant_point_screen_diameter_to_ndc_radius =
          rdef_constants[kPointConstantScreenDiameterToNDCRadius];
      rdef_constant_point_screen_diameter_to_ndc_radius.name_ptr =
          rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_offset_bytes =
          offsetof(DxbcShaderTranslator::SystemConstants,
                   point_screen_diameter_to_ndc_radius);
      rdef_constant_point_screen_diameter_to_ndc_radius.size_bytes =
          sizeof(float) * 2;
      rdef_constant_point_screen_diameter_to_ndc_radius.flags =
          dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_screen_diameter_to_ndc_radius.type_ptr =
          rdef_type_float2_ptr;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_texture =
          UINT32_MAX;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_sampler =
          UINT32_MAX;
    }

    // Constant buffers - xe_system_cbuffer only.

    // Names.
    name_ptr =
        uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_system_cbuffer = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_system_cbuffer");
    // Constant buffers.
    uint32_t rdef_cbuffer_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_cbuffer_position_dwords +
                      sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t));
    {
      auto& rdef_cbuffer_system = *reinterpret_cast<dxbc::RdefCbuffer*>(
          shader_out.data() + rdef_cbuffer_position_dwords);
      rdef_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_cbuffer_system.variable_count = kPointConstantCount;
      rdef_cbuffer_system.variables_ptr = rdef_constants_ptr;
      auto rdef_constants = reinterpret_cast<const dxbc::RdefVariable*>(
          shader_out.data() + rdef_constants_position_dwords);
      for (uint32_t i = 0; i < kPointConstantCount; ++i) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     rdef_constants[i].start_offset_bytes +
                         rdef_constants[i].size_bytes);
      }
      system_cbuffer_size_vector_aligned_bytes =
          xe::align(system_cbuffer_size_vector_aligned_bytes,
                    uint32_t(sizeof(uint32_t) * 4));
      rdef_cbuffer_system.size_vector_aligned_bytes =
          system_cbuffer_size_vector_aligned_bytes;
    }

    // Bindings - xe_system_cbuffer only.
    uint32_t rdef_binding_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_binding_position_dwords +
                      sizeof(dxbc::RdefInputBind) / sizeof(uint32_t));
    {
      auto& rdef_binding_cbuffer_system =
          *reinterpret_cast<dxbc::RdefInputBind*>(shader_out.data() +
                                                  rdef_binding_position_dwords);
      rdef_binding_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_binding_cbuffer_system.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_cbuffer_system.bind_point =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      rdef_binding_cbuffer_system.bind_count = 1;
      rdef_binding_cbuffer_system.flags = dxbc::kRdefInputFlagUserPacked;
    }

    // Pointers in the header.
    {
      auto& rdef_header = *reinterpret_cast<dxbc::RdefHeader*>(
          shader_out.data() + rdef_position_dwords);
      rdef_header.cbuffer_count = 1;
      rdef_header.cbuffers_ptr =
          uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) *
                   sizeof(uint32_t));
      rdef_header.input_bind_count = 1;
      rdef_header.input_binds_ptr =
          uint32_t((rdef_binding_position_dwords - rdef_position_dwords) *
                   sizeof(uint32_t));
    }
  }

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input signature
  // ***************************************************************************

  // Clip and cull distances are tightly packed together into registers, but
  // have separate signature parameters with each being a vec4-aligned window.
  uint32_t input_clip_distance_count =
      key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t input_cull_distance_count =
      (key.user_clip_plane_cull ? key.user_clip_plane_count : 0) +
      key.has_vertex_kill_and;
  uint32_t input_clip_and_cull_distance_count =
      input_clip_distance_count + input_cull_distance_count;

  // Interpolators, position, clip and cull distances (parameters containing
  // only clip or cull distances, and also one parameter containing both if
  // present), point size.
  uint32_t isgn_parameter_count =
      key.interpolator_count + 1 +
      ((input_clip_and_cull_distance_count + 3) / 4) +
      uint32_t(input_cull_distance_count &&
               (input_clip_distance_count & 3) != 0) +
      key.has_point_size;

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t isgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(isgn_position_dwords +
                    sizeof(dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(dxbc::SignatureParameter) / sizeof(uint32_t) *
                        isgn_parameter_count);

  // Names (after the parameters).
  name_ptr =
      uint32_t((shader_out.size() - isgn_position_dwords) * sizeof(uint32_t));
  uint32_t isgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t isgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t isgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }
  uint32_t isgn_name_ptr_sv_cull_distance = name_ptr;
  if (input_cull_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_CullDistance");
  }
  uint32_t isgn_name_ptr_xepsize = name_ptr;
  if (key.has_point_size) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XEPSIZE");
  }

  // Header and parameters.
  uint32_t input_register_interpolators = UINT32_MAX;
  uint32_t input_register_position;
  uint32_t input_register_clip_and_cull_distances = UINT32_MAX;
  uint32_t input_register_point_size = UINT32_MAX;
  {
    // Header.
    auto& isgn_header = *reinterpret_cast<dxbc::Signature*>(
        shader_out.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    // Parameters.
    auto isgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_out.data() + isgn_position_dwords +
        sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t isgn_parameter_index = 0;
    uint32_t input_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      input_register_interpolators = input_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(isgn_parameter_index < isgn_parameter_count);
        dxbc::SignatureParameter& isgn_interpolator =
            isgn_parameters[isgn_parameter_index++];
        isgn_interpolator.semantic_name_ptr = isgn_name_ptr_texcoord;
        isgn_interpolator.semantic_index = i;
        isgn_interpolator.component_type =
            dxbc::SignatureRegisterComponentType::kFloat32;
        isgn_interpolator.register_index = input_register_index++;
        isgn_interpolator.mask = 0b1111;
        isgn_interpolator.always_reads_mask = 0b1111;
      }
    }

    // Position (SV_Position).
    input_register_position = input_register_index;
    assert_true(isgn_parameter_index < isgn_parameter_count);
    dxbc::SignatureParameter& isgn_sv_position =
        isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_name_ptr_sv_position;
    isgn_sv_position.system_value = dxbc::Name::kPosition;
    isgn_sv_position.component_type =
        dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = input_register_index++;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b1111;

    // Clip and cull distances (SV_ClipDistance#, SV_CullDistance#).
    if (input_clip_and_cull_distance_count) {
      input_register_clip_and_cull_distances = input_register_index;
      uint32_t isgn_cull_distance_semantic_index = 0;
      for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
        if (i < input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_clip_distance =
              isgn_parameters[isgn_parameter_index++];
          isgn_sv_clip_distance.semantic_name_ptr =
              isgn_name_ptr_sv_clip_distance;
          isgn_sv_clip_distance.semantic_index = i / 4;
          isgn_sv_clip_distance.system_value = dxbc::Name::kClipDistance;
          isgn_sv_clip_distance.component_type =
              dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_clip_distance.register_index = input_register_index;
          uint8_t isgn_sv_clip_distance_mask =
              (UINT8_C(1) << std::min(input_clip_distance_count - i,
                                      UINT32_C(4))) -
              1;
          isgn_sv_clip_distance.mask = isgn_sv_clip_distance_mask;
          isgn_sv_clip_distance.always_reads_mask = isgn_sv_clip_distance_mask;
        }
        if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_cull_distance =
              isgn_parameters[isgn_parameter_index++];
          isgn_sv_cull_distance.semantic_name_ptr =
              isgn_name_ptr_sv_cull_distance;
          isgn_sv_cull_distance.semantic_index =
              isgn_cull_distance_semantic_index++;
          isgn_sv_cull_distance.system_value = dxbc::Name::kCullDistance;
          isgn_sv_cull_distance.component_type =
              dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_cull_distance.register_index = input_register_index;
          uint8_t isgn_sv_cull_distance_mask =
              (UINT8_C(1) << std::min(input_clip_and_cull_distance_count - i,
                                      UINT32_C(4))) -
              1;
          if (i < input_clip_distance_count) {
            isgn_sv_cull_distance_mask &=
                ~((UINT8_C(1) << (input_clip_distance_count - i)) - 1);
          }
          isgn_sv_cull_distance.mask = isgn_sv_cull_distance_mask;
          isgn_sv_cull_distance.always_reads_mask = isgn_sv_cull_distance_mask;
        }
        ++input_register_index;
      }
    }

    // Point size (XEPSIZE).
    if (key.has_point_size) {
      input_register_point_size = input_register_index;
      assert_true(isgn_parameter_index < isgn_parameter_count);
      dxbc::SignatureParameter& isgn_point_size =
          isgn_parameters[isgn_parameter_index++];
      isgn_point_size.semantic_name_ptr = isgn_name_ptr_xepsize;
      isgn_point_size.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      isgn_point_size.register_index = input_register_index++;
      isgn_point_size.mask = 0b0001;
      isgn_point_size.always_reads_mask =
          key.type == PipelineGeometryShader::kPointList ? 0b0001 : 0;
    }

    assert_true(isgn_parameter_index == isgn_parameter_count);
  }

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Output signature
  // ***************************************************************************

  // Interpolators, point coordinates, position, clip distances.
  uint32_t osgn_parameter_count = key.interpolator_count +
                                  key.has_point_coordinates + 1 +
                                  ((input_clip_distance_count + 3) / 4);

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t osgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(osgn_position_dwords +
                    sizeof(dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(dxbc::SignatureParameterForGS) / sizeof(uint32_t) *
                        osgn_parameter_count);

  // Names (after the parameters).
  name_ptr =
      uint32_t((shader_out.size() - osgn_position_dwords) * sizeof(uint32_t));
  uint32_t osgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t osgn_name_ptr_xespritetexcoord = name_ptr;
  if (key.has_point_coordinates) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XESPRITETEXCOORD");
  }
  uint32_t osgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t osgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }

  // Header and parameters.
  uint32_t output_register_interpolators = UINT32_MAX;
  uint32_t output_register_point_coordinates = UINT32_MAX;
  uint32_t output_register_position;
  uint32_t output_register_clip_distances = UINT32_MAX;
  {
    // Header.
    auto& osgn_header = *reinterpret_cast<dxbc::Signature*>(
        shader_out.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    // Parameters.
    auto osgn_parameters = reinterpret_cast<dxbc::SignatureParameterForGS*>(
        shader_out.data() + osgn_position_dwords +
        sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t osgn_parameter_index = 0;
    uint32_t output_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      output_register_interpolators = output_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(osgn_parameter_index < osgn_parameter_count);
        dxbc::SignatureParameterForGS& osgn_interpolator =
            osgn_parameters[osgn_parameter_index++];
        osgn_interpolator.semantic_name_ptr = osgn_name_ptr_texcoord;
        osgn_interpolator.semantic_index = i;
        osgn_interpolator.component_type =
            dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_interpolator.register_index = output_register_index++;
        osgn_interpolator.mask = 0b1111;
      }
    }

    // Point coordinates (XESPRITETEXCOORD).
    if (key.has_point_coordinates) {
      output_register_point_coordinates = output_register_index;
      assert_true(osgn_parameter_index < osgn_parameter_count);
      dxbc::SignatureParameterForGS& osgn_point_coordinates =
          osgn_parameters[osgn_parameter_index++];
      osgn_point_coordinates.semantic_name_ptr = osgn_name_ptr_xespritetexcoord;
      osgn_point_coordinates.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_point_coordinates.register_index = output_register_index++;
      osgn_point_coordinates.mask = 0b0011;
      osgn_point_coordinates.never_writes_mask = 0b1100;
    }

    // Position (SV_Position).
    output_register_position = output_register_index;
    assert_true(osgn_parameter_index < osgn_parameter_count);
    dxbc::SignatureParameterForGS& osgn_sv_position =
        osgn_parameters[osgn_parameter_index++];
    osgn_sv_position.semantic_name_ptr = osgn_name_ptr_sv_position;
    osgn_sv_position.system_value = dxbc::Name::kPosition;
    osgn_sv_position.component_type =
        dxbc::SignatureRegisterComponentType::kFloat32;
    osgn_sv_position.register_index = output_register_index++;
    osgn_sv_position.mask = 0b1111;

    // Clip distances (SV_ClipDistance#).
    if (input_clip_distance_count) {
      output_register_clip_distances = output_register_index;
      for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
        dxbc::SignatureParameterForGS& osgn_sv_clip_distance =
            osgn_parameters[osgn_parameter_index++];
        osgn_sv_clip_distance.semantic_name_ptr =
            osgn_name_ptr_sv_clip_distance;
        osgn_sv_clip_distance.semantic_index = i / 4;
        osgn_sv_clip_distance.system_value = dxbc::Name::kClipDistance;
        osgn_sv_clip_distance.component_type =
            dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_sv_clip_distance.register_index = output_register_index++;
        uint8_t osgn_sv_clip_distance_mask =
            (UINT8_C(1) << std::min(input_clip_distance_count - i,
                                    UINT32_C(4))) -
            1;
        osgn_sv_clip_distance.mask = osgn_sv_clip_distance_mask;
        osgn_sv_clip_distance.never_writes_mask =
            osgn_sv_clip_distance_mask ^ 0b1111;
      }
    }

    assert_true(osgn_parameter_index == osgn_parameter_count);
  }

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignatureForGS;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(shex_position_dwords);

  shader_out.push_back(
      dxbc::VersionToken(dxbc::ProgramType::kGeometryShader, 5, 1));
  // Reserve space for the length token.
  shader_out.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  dxbc::Assembler a(shader_out, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);

  if (system_cbuffer_size_vector_aligned_bytes) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(
            dxbc::Src::Dcl, 0,
            uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants)),
        system_cbuffer_size_vector_aligned_bytes / (sizeof(uint32_t) * 4));
  }

  dxbc::Primitive input_primitive = dxbc::Primitive::kUndefined;
  uint32_t input_primitive_vertex_count = 0;
  dxbc::PrimitiveTopology output_primitive_topology =
      dxbc::PrimitiveTopology::kUndefined;
  uint32_t max_output_vertex_count = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kPoint;
      input_primitive_vertex_count = 1;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kTriangle;
      input_primitive_vertex_count = 3;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      // 4 vertices passed via kLineWithAdjacency to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kLineWithAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    default:
      assert_unhandled_case(key.type);
  }

  assert_false(key.interpolator_count &&
               input_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count,
                                 input_register_interpolators + i));
  }
  a.OpDclInputSIV(
      dxbc::Dest::V2D(input_primitive_vertex_count, input_register_position),
      dxbc::Name::kPosition);
  // Clip and cull plane declarations are separate in FXC-generated code even
  // for a single register.
  assert_false(input_clip_and_cull_distance_count &&
               input_register_clip_and_cull_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
    if (i < input_clip_distance_count) {
      a.OpDclInput(
          dxbc::Dest::V2D(input_primitive_vertex_count,
                          input_register_clip_and_cull_distances + (i >> 2),
                          (UINT32_C(1) << std::min(
                               input_clip_distance_count - i, UINT32_C(4))) -
                              1));
    }
    if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
      uint32_t cull_distance_mask =
          (UINT32_C(1) << std::min(input_clip_and_cull_distance_count - i,
                                   UINT32_C(4))) -
          1;
      if (i < input_clip_distance_count) {
        cull_distance_mask &=
            ~((UINT32_C(1) << (input_clip_distance_count - i)) - 1);
      }
      a.OpDclInput(
          dxbc::Dest::V2D(input_primitive_vertex_count,
                          input_register_clip_and_cull_distances + (i >> 2),
                          cull_distance_mask));
    }
  }
  if (key.has_point_size && key.type == PipelineGeometryShader::kPointList) {
    assert_true(input_register_point_size != UINT32_MAX);
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count,
                                 input_register_point_size, 0b0001));
  }

  // At least 1 temporary register needed to discard primitives with NaN
  // position.
  size_t dcl_temps_count_position_dwords = a.OpDclTemps(1);

  a.OpDclInputPrimitive(input_primitive);
  dxbc::Dest stream(dxbc::Dest::M(0));
  a.OpDclStream(stream);
  a.OpDclOutputTopology(output_primitive_topology);

  assert_false(key.interpolator_count &&
               output_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclOutput(dxbc::Dest::O(output_register_interpolators + i));
  }
  if (key.has_point_coordinates) {
    assert_true(output_register_point_coordinates != UINT32_MAX);
    a.OpDclOutput(dxbc::Dest::O(output_register_point_coordinates, 0b0011));
  }
  a.OpDclOutputSIV(dxbc::Dest::O(output_register_position),
                   dxbc::Name::kPosition);
  assert_false(input_clip_distance_count &&
               output_register_clip_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
    a.OpDclOutputSIV(
        dxbc::Dest::O(output_register_clip_distances + (i >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - i,
                                               UINT32_C(4))) -
                          1),
        dxbc::Name::kClipDistance);
  }

  a.OpDclMaxOutputVertexCount(max_output_vertex_count);

  // Note that after every emit, all o# become initialized and must be written
  // to again.
  // Also, FXC generates only movs (from statically or dynamically indexed
  // v[#][#], from r#, or from a literal) to o# for some reason.
  // emit_then_cut_stream must not be used - it crashes the shader compiler of
  // AMD Software: Adrenalin Edition 23.3.2 on RDNA 3 if it's conditional (after
  // a `retc` or inside an `if`), and it doesn't seem to be generated by FXC or
  // DXC at all.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    a.OpNE(dxbc::Dest::R(0), dxbc::Src::V2D(i, input_register_position),
           dxbc::Src::V2D(i, input_register_position));
    a.OpOr(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, 0b0100),
           dxbc::Src::R(0, 0b1110));
    a.OpOr(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
           dxbc::Src::R(0, dxbc::Src::kYYYY));
    a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // TODO(Triang3l): For points, handle ps_ucp_mode (transform the host clip
  // space to the guest one, calculate the distances to the user clip planes,
  // cull using the distance from the center for modes 0, 1 and 2, cull and clip
  // per-vertex for modes 2 and 3) - except for the vertex kill flag.
  if (input_cull_distance_count) {
    for (uint32_t i = 0; i < input_cull_distance_count; ++i) {
      uint32_t cull_distance_register = input_register_clip_and_cull_distances +
                                        ((input_clip_distance_count + i) >> 2);
      uint32_t cull_distance_component = (input_clip_distance_count + i) & 3;
      a.OpLT(dxbc::Dest::R(0, 0b0001),
             dxbc::Src::V2D(0, cull_distance_register)
                 .Select(cull_distance_component),
             dxbc::Src::LF(0.0f));
      for (uint32_t j = 1; j < input_primitive_vertex_count; ++j) {
        a.OpLT(dxbc::Dest::R(0, 0b0010),
               dxbc::Src::V2D(j, cull_distance_register)
                   .Select(cull_distance_component),
               dxbc::Src::LF(0.0f));
        a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
                dxbc::Src::R(0, dxbc::Src::kYYYY));
      }
      a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
    }
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.
      dxbc::Src point_size_src(dxbc::Src::CB(
          0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
          offsetof(DxbcShaderTranslator::SystemConstants,
                   point_constant_diameter) >>
              4,
          ((offsetof(DxbcShaderTranslator::SystemConstants,
                     point_constant_diameter[0]) >>
            2) &
           3) |
              (((offsetof(DxbcShaderTranslator::SystemConstants,
                          point_constant_diameter[1]) >>
                 2) &
                3)
               << 2)));
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        a.OpGE(dxbc::Dest::R(0, 0b0001),
               dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX),
               dxbc::Src::LF(0.0f));
        a.OpMovC(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX),
                 point_size_src);
        point_size_src = dxbc::Src::R(0, 0b0100);
      }
      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      // XY may contain the point size with the per-vertex override applied, use
      // Z as temporary.
      for (uint32_t i = 0; i < 2; ++i) {
        a.OpLT(dxbc::Dest::R(0, 0b0100), dxbc::Src::LF(0.0f),
               point_size_src.SelectFromSwizzled(i));
        a.OpRetC(false, dxbc::Src::R(0, dxbc::Src::kZZZZ));
      }
      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      a.OpMul(
          dxbc::Dest::R(0, 0b0011), point_size_src,
          dxbc::Src::CB(
              0,
              uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants,
                       point_screen_diameter_to_ndc_radius) >>
                  4,
              ((offsetof(DxbcShaderTranslator::SystemConstants,
                         point_screen_diameter_to_ndc_radius[0]) >>
                2) &
               3) |
                  (((offsetof(DxbcShaderTranslator::SystemConstants,
                              point_screen_diameter_to_ndc_radius[1]) >>
                     2) &
                    3)
                   << 2)));
      point_size_src = dxbc::Src::R(0, 0b0100);
      a.OpMul(dxbc::Dest::R(0, 0b0011), point_size_src,
              dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
      dxbc::Src point_radius_x_src(point_size_src.SelectFromSwizzled(0));
      dxbc::Src point_radius_y_src(point_size_src.SelectFromSwizzled(1));

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(0, input_register_interpolators + j));
        }
        // Top-left, top-right, bottom-left, bottom-right order (chosen
        // arbitrarily, simply based on clockwise meaning front with
        // FrontCounterClockwise = FALSE, but faceness is ignored for
        // non-polygon primitive types).
        // Bottom is -Y in Direct3D NDC, +V in point sprite coordinates.
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  dxbc::Src::LF(float(i & 1), float(i >> 1), 0.0f, 0.0f));
        }
        // FXC generates only `mov`s for o#, use temporary registers (r0.zw, as
        // r0.xy already used for the point size) for calculations.
        a.OpAdd(dxbc::Dest::R(0, 0b0100),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kXXXX),
                (i & 1) ? point_radius_x_src : -point_radius_x_src);
        a.OpAdd(dxbc::Dest::R(0, 0b1000),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kYYYY),
                (i >> 1) ? -point_radius_y_src : point_radius_y_src);
        a.OpMov(dxbc::Dest::O(output_register_position, 0b0011),
                dxbc::Src::R(0, 0b1110));
        a.OpMov(dxbc::Dest::O(output_register_position, 0b1100),
                dxbc::Src::V2D(0, input_register_position));
        // TODO(Triang3l): Handle ps_ucp_mode properly, clip expanded points if
        // needed.
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(
              dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                            (UINT32_C(1) << std::min(
                                 input_clip_distance_count - j, UINT32_C(4))) -
                                1),
              dxbc::Src::V2D(
                  0, input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]
      //
      // Input vertices are implicitly indexable, dcl_indexRange is not needed
      // for the first dimension of a v[#][#] index.

      // Get squares of edge lengths into r0.xyz to choose the longest edge.
      // r0.x = ||12||^2
      a.OpAdd(dxbc::Dest::R(0, 0b0011),
              dxbc::Src::V2D(2, input_register_position, 0b0100),
              -dxbc::Src::V2D(1, input_register_position, 0b0100));
      a.OpDP2(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, 0b0100),
              dxbc::Src::R(0, 0b0100));
      // r0.y = ||20||^2
      a.OpAdd(dxbc::Dest::R(0, 0b0110),
              dxbc::Src::V2D(0, input_register_position, 0b0100 << 2),
              -dxbc::Src::V2D(2, input_register_position, 0b0100 << 2));
      a.OpDP2(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, 0b1001),
              dxbc::Src::R(0, 0b1001));
      // r0.z = ||01||^2
      a.OpAdd(dxbc::Dest::R(0, 0b1100),
              dxbc::Src::V2D(1, input_register_position, 0b0100 << 4),
              -dxbc::Src::V2D(0, input_register_position, 0b0100 << 4));
      a.OpDP2(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, 0b1110),
              dxbc::Src::R(0, 0b1110));

      // Find the longest edge, and select the strip vertex indices into r0.xyz.
      // r0.w = 12 > 20
      a.OpLT(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r0.x = 12 > 01
      a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r0.x = 12 > 20 && 12 > 01
      a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW),
              dxbc::Src::R(0, dxbc::Src::kXXXX));
      a.OpIf(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
      {
        // 12 is the longest edge, the first triangle in the strip is 012.
        a.OpMov(dxbc::Dest::R(0, 0b0111), dxbc::Src::LU(0, 1, 2, 0));
      }
      a.OpElse();
      {
        // r0.x = 20 > 01
        a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
               dxbc::Src::R(0, dxbc::Src::kYYYY));
        // If 20 is the longest edge, the first triangle in the strip is 120.
        // Otherwise, it's 201.
        a.OpMovC(dxbc::Dest::R(0, 0b0111), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::LU(1, 2, 0, 0), dxbc::Src::LU(2, 0, 1, 0));
      }
      a.OpEndIf();

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        dxbc::Index input_vertex_index(0, i);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(
              dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                            (UINT32_C(1) << std::min(
                                 input_clip_distance_count - j, UINT32_C(4))) -
                                1),
              dxbc::Src::V2D(
                  input_vertex_index,
                  input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }

      // Construct the fourth vertex using r1 as temporary storage, including
      // for the final operation as FXC generates only `mov`s for o#.
      stat.temp_register_count =
          std::max(UINT32_C(2), stat.temp_register_count);
      for (uint32_t j = 0; j < key.interpolator_count; ++j) {
        uint32_t input_register_interpolator = input_register_interpolators + j;
        a.OpAdd(dxbc::Dest::R(1),
                -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_interpolator),
                dxbc::Src::V2D(dxbc::Index(0, 1), input_register_interpolator));
        a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
                dxbc::Src::V2D(dxbc::Index(0, 2), input_register_interpolator));
        a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                dxbc::Src::R(1));
      }
      if (key.has_point_coordinates) {
        a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                dxbc::Src::LF(0.0f));
      }
      a.OpAdd(dxbc::Dest::R(1),
              -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_position),
              dxbc::Src::V2D(dxbc::Index(0, 1), input_register_position));
      a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
              dxbc::Src::V2D(dxbc::Index(0, 2), input_register_position));
      a.OpMov(dxbc::Dest::O(output_register_position), dxbc::Src::R(1));
      for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
        uint32_t clip_distance_mask =
            (UINT32_C(1) << std::min(input_clip_distance_count - j,
                                     UINT32_C(4))) -
            1;
        uint32_t input_register_clip_distance =
            input_register_clip_and_cull_distances + (j >> 2);
        a.OpAdd(
            dxbc::Dest::R(1, clip_distance_mask),
            -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_clip_distance),
            dxbc::Src::V2D(dxbc::Index(0, 1), input_register_clip_distance));
        a.OpAdd(
            dxbc::Dest::R(1, clip_distance_mask), dxbc::Src::R(1),
            dxbc::Src::V2D(dxbc::Index(0, 2), input_register_clip_distance));
        a.OpMov(dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                              clip_distance_mask),
                dxbc::Src::R(1));
      }
      a.OpEmitStream(stream);
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t input_vertex_index = i ^ (i >> 1);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(
              dxbc::Dest::O(output_register_clip_distances + (j >> 2),
                            (UINT32_C(1) << std::min(
                                 input_clip_distance_count - j, UINT32_C(4))) -
                                1),
              dxbc::Src::V2D(
                  input_vertex_index,
                  input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    default:
      assert_unhandled_case(key.type);
  }

  a.OpRet();

  // Write the actual number of temporary registers used.
  shader_out[dcl_temps_count_position_dwords] = stat.temp_register_count;

  // Write the shader program length in dwords.
  shader_out[shex_position_dwords + 1] =
      uint32_t(shader_out.size()) - shex_position_dwords;

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(stat_position_dwords +
                    sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(shader_out.data() + stat_position_dwords, &stat,
              sizeof(dxbc::Statistics));

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t shader_size_bytes = uint32_t(shader_out.size() * sizeof(uint32_t));
  {
    auto& container_header =
        *reinterpret_cast<dxbc::ContainerHeader*>(shader_out.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = shader_size_bytes;
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(
        reinterpret_cast<unsigned char*>(shader_out.data()),
        static_cast<unsigned int>(shader_size_bytes),
        reinterpret_cast<unsigned int*>(&container_header.hash));
  }
}

const std::vector<uint32_t>& PipelineCache::GetGeometryShader(
    GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }
  std::vector<uint32_t> shader;
  CreateDxbcGeometryShader(key, shader);
  return geometry_shaders_.emplace(key, std::move(shader)).first->second;
}

void PipelineCache::EnsurePipelineShadersTranslated(
    Pipeline* pipeline, DxbcShaderTranslator& translator,
    StringBuffer& ucode_disasm_buffer, IDxbcConverter* dxbc_converter,
    IDxcUtils* dxc_utils, IDxcCompiler* dxc_compiler, bool use_try_claim,
    bool handle_non_placeholder) {
  D3D12Shader::D3D12Translation* pending_vs = pipeline->pending_vertex_shader;
  D3D12Shader::D3D12Translation* pending_ps = pipeline->pending_pixel_shader;

  // Helper lambda to translate a shader, optionally using TryClaimTranslation.
  auto translate_shader = [&](D3D12Shader::D3D12Translation* translation,
                              const char* shader_type) {
    if (!translation->is_translated()) {
      bool should_translate = true;
      if (use_try_claim) {
        should_translate = translation->TryClaimTranslation();
        if (!should_translate) {
          // Another thread is translating - wait for it. Not forever, though:
          // if that translation fails, or its binary is released under memory
          // pressure, is_translated() never becomes true and this creation
          // thread would spin here for the rest of the run - burning a core
          // and, worse, never building another pipeline, so every draw that
          // needs one is skipped from then on.
          uint64_t wait_start_ticks = Clock::QueryHostTickCount();
          uint64_t wait_warn_ticks = Clock::QueryHostTickFrequency() * 5;
          bool wait_reported = false;
          while (!translation->is_translated()) {
            if (!wait_reported &&
                Clock::QueryHostTickCount() - wait_start_ticks >
                    wait_warn_ticks) {
              wait_reported = true;
              XELOGW(
                  "Pipeline cache: still waiting for another thread to "
                  "translate shader {:016X} after 5 seconds - if this repeats, "
                  "that translation never completed and pipeline creation is "
                  "stuck behind it",
                  translation->shader().ucode_data_hash());
            }
            // Sleep rather than yield. These threads run at below-normal
            // priority on a console with ~6-7 usable cores and 30+ emulator
            // threads: a spinning yield() only ever offers the core to
            // threads of equal or higher priority, so the below-normal thread
            // that actually holds the translation may not get scheduled at
            // all while its peers burn their quanta waiting for it - a
            // priority inversion that stalls pipeline creation completely
            // (observed as multi-second waits, and as "async compilation that
            // stutters like sync"). Sleeping takes the waiter off the core.
            xe::threading::Sleep(std::chrono::milliseconds(1));
          }
        }
      }
      if (should_translate) {
        DxbcShader& shader = static_cast<DxbcShader&>(translation->shader());
        if (!shader.is_ucode_analyzed()) {
          shader.AnalyzeUcode(ucode_disasm_buffer);
        }
        if (!TranslateAnalyzedShader(translator, *translation, dxbc_converter,
                                     dxc_utils, dxc_compiler)) {
          XELOGE("Failed to translate {} shader {:016X}", shader_type,
                 shader.ucode_data_hash());
        } else {
          if (storage_writer_.is_active() &&
              shader.try_set_ucode_storage_index(
                  storage_writer_.storage_index())) {
            shader_storage_file_flush_needed_ = true;
            storage_writer_.QueueShaderWrite(&shader);
          }
        }
      }
    }
  };

  // Translate pending VS if present.
  if (pending_vs != nullptr) {
    translate_shader(pending_vs, "vertex");
    pipeline->pending_vertex_shader = nullptr;
  }

  // Translate pending PS if present and update root signature.
  if (pending_ps != nullptr) {
    translate_shader(pending_ps, "pixel");
    // Update root signature now that PS is translated.
    if (pending_ps->is_valid()) {
      PipelineRuntimeDescription& desc = pipeline->description;
      bool tessellated = Shader::IsHostVertexShaderTypeDomain(
          DxbcShaderTranslator::Modification(desc.vertex_shader->modification())
              .vertex.host_vertex_shader_type);
      desc.root_signature = command_processor_.GetRootSignature(
          static_cast<const DxbcShader*>(&desc.vertex_shader->shader()),
          static_cast<const DxbcShader*>(&pending_ps->shader()), tessellated);
    }
    pipeline->pending_pixel_shader = nullptr;
  } else if (handle_non_placeholder && pending_vs == nullptr) {
    // Non-placeholder mode: translate desc.pixel_shader if needed (for
    // pipelines loaded from cache).
    D3D12Shader::D3D12Translation* ps = pipeline->description.pixel_shader;
    if (ps != nullptr) {
      translate_shader(ps, "pixel");
    }
  }
}

#if XE_PLATFORM_WINRT
static DWORD SolverStoreExceptionCode(DWORD code, DWORD* code_out) {
  *code_out = code;
  return EXCEPTION_EXECUTE_HANDLER;
}

// Calls CreateGraphicsPipelineState under a structured-exception guard: the
// Xbox UWP driver's shader compiler (newbe_xs.dll) hard-crashes (access
// violation) on some pipelines, which would otherwise kill the whole process.
// The crash is a user-mode bug in the compiler DLL - caught here, the process
// (and usually the D3D12 device itself) survives, the pipeline is reported as
// failed, and the caller quarantines the shader pair. Must be a separate
// function without C++ objects needing unwinding for __try to be legal.
static HRESULT CreateGraphicsPipelineStateGuarded(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid, void** state_out, DWORD* exception_code_out) {
  *exception_code_out = 0;
  __try {
    return device->CreateGraphicsPipelineState(desc, riid, state_out);
  } __except (SolverStoreExceptionCode(GetExceptionCode(),
                                       exception_code_out)) {
    return E_FAIL;
  }
}
#endif  // XE_PLATFORM_WINRT

ID3D12PipelineState* PipelineCache::CreateD3D12Pipeline(
    const PipelineRuntimeDescription& runtime_description) {
  const PipelineDescription& description = runtime_description.description;

  const uint64_t vs_hash =
      runtime_description.vertex_shader->shader().ucode_data_hash();
  const uint64_t ps_hash =
      runtime_description.pixel_shader
          ? runtime_description.pixel_shader->shader().ucode_data_hash()
          : 0;

  // Toxic-shader workaround (Xbox UWP): never create a pipeline for a shader
  // combination on the skip list. Doing this here (not only at draw time)
  // means the persistent shader-storage prewarming - which re-creates every
  // stored pipeline on load - won't re-create a driver-hanging pipeline and
  // crash before the game even runs. The draw is skipped as "pipeline not
  // ready" (the geometry is invisible, but the GPU survives). Two sources: the
  // manually configured cvars::d3d12_skip_shaders list, and the per-game list
  // the crash solver has learned automatically.
  bool skip_pipeline = command_processor_.IsShaderSkipped(vs_hash, ps_hash);
#if XE_PLATFORM_WINRT
  skip_pipeline = skip_pipeline || IsShaderToxic(vs_hash, ps_hash);
#endif  // XE_PLATFORM_WINRT
  if (skip_pipeline) {
    return nullptr;
  }

  if (runtime_description.pixel_shader != nullptr) {
    XELOGGPU("Creating graphics pipeline with VS {:016X}, PS {:016X}",
             runtime_description.vertex_shader->shader().ucode_data_hash(),
             runtime_description.pixel_shader->shader().ucode_data_hash());
  } else {
    XELOGGPU("Creating graphics pipeline with VS {:016X}",
             runtime_description.vertex_shader->shader().ucode_data_hash());
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC state_desc;
  std::memset(&state_desc, 0, sizeof(state_desc));

  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  // Root signature.
  state_desc.pRootSignature = runtime_description.root_signature;

  // Index buffer strip cut value.
  switch (description.strip_cut_index) {
    case PipelineStripCutIndex::kFFFF:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      break;
    case PipelineStripCutIndex::kFFFFFFFF:
      state_desc.IBStripCutValue =
          D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      break;
    default:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      break;
  }

  // Primitive topology, vertex, hull, domain and geometry shaders.
  if (!runtime_description.vertex_shader->is_translated()) {
    XELOGE("Vertex shader {:016X} not translated",
           runtime_description.vertex_shader->shader().ucode_data_hash());
    assert_always();
    return nullptr;
  }
  Shader::HostVertexShaderType host_vertex_shader_type =
      DxbcShaderTranslator::Modification(
          runtime_description.vertex_shader->modification())
          .vertex.host_vertex_shader_type;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    xenos::TessellationMode tessellation_mode = xenos::TessellationMode(
        description.primitive_topology_type_or_tessellation_mode);
    if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
      state_desc.VS.pShaderBytecode = shaders::tessellation_adaptive_vs;
      state_desc.VS.BytecodeLength = sizeof(shaders::tessellation_adaptive_vs);
    } else {
      state_desc.VS.pShaderBytecode = shaders::tessellation_indexed_vs;
      state_desc.VS.BytecodeLength = sizeof(shaders::tessellation_indexed_vs);
    }
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_triangle_3cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_triangle_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_quad_4cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_quad_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_quad_1cp_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_triangle_3cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_triangle_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_quad_4cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_quad_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_quad_1cp_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::adaptive_triangle_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::adaptive_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::adaptive_quad_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::adaptive_quad_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      default:
        assert_unhandled_case(tessellation_mode);
        return nullptr;
    }
    state_desc.DS.pShaderBytecode =
        runtime_description.vertex_shader->translated_binary().data();
    state_desc.DS.BytecodeLength =
        runtime_description.vertex_shader->translated_binary().size();
  } else {
    assert_true(host_vertex_shader_type ==
                Shader::HostVertexShaderType::kVertex);
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
      // Fallback vertex shaders are not needed on Direct3D 12.
      return nullptr;
    }
    state_desc.VS.pShaderBytecode =
        runtime_description.vertex_shader->translated_binary().data();
    state_desc.VS.BytecodeLength =
        runtime_description.vertex_shader->translated_binary().size();
    PipelinePrimitiveTopologyType primitive_topology_type =
        PipelinePrimitiveTopologyType(
            description.primitive_topology_type_or_tessellation_mode);
    switch (primitive_topology_type) {
      case PipelinePrimitiveTopologyType::kPoint:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
      case PipelinePrimitiveTopologyType::kLine:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
      case PipelinePrimitiveTopologyType::kTriangle:
        state_desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
      default:
        assert_unhandled_case(primitive_topology_type);
        return nullptr;
    }
  }

  // Pixel shader.
  if (runtime_description.pixel_shader != nullptr) {
    if (!runtime_description.pixel_shader->is_translated()) {
      XELOGE("Pixel shader {:016X} not translated",
             runtime_description.pixel_shader->shader().ucode_data_hash());
      assert_always();
      return nullptr;
    }
    state_desc.PS.pShaderBytecode =
        runtime_description.pixel_shader->translated_binary().data();
    state_desc.PS.BytecodeLength =
        runtime_description.pixel_shader->translated_binary().size();
  } else if (edram_rov_used) {
    state_desc.PS.pShaderBytecode = depth_only_pixel_shader_.data();
    state_desc.PS.BytecodeLength = depth_only_pixel_shader_.size();
  } else {
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        (description.depth_func != xenos::CompareFunction::kAlways ||
         description.depth_write) &&
        description.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      if (render_target_cache_.depth_float24_round()) {
        state_desc.PS.pShaderBytecode = shaders::float24_round_ps;
        state_desc.PS.BytecodeLength = sizeof(shaders::float24_round_ps);
      } else {
        state_desc.PS.pShaderBytecode = shaders::float24_truncate_ps;
        state_desc.PS.BytecodeLength = sizeof(shaders::float24_truncate_ps);
      }
    }
  }

  // Geometry shader.
  if (runtime_description.geometry_shader != nullptr) {
    state_desc.GS.pShaderBytecode = runtime_description.geometry_shader->data();
    state_desc.GS.BytecodeLength =
        sizeof(*runtime_description.geometry_shader->data()) *
        runtime_description.geometry_shader->size();
  }

  // Rasterizer state.
  state_desc.RasterizerState.FillMode = description.fill_mode_wireframe
                                            ? D3D12_FILL_MODE_WIREFRAME
                                            : D3D12_FILL_MODE_SOLID;
  switch (description.cull_mode) {
    case PipelineCullMode::kFront:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
      break;
    case PipelineCullMode::kBack:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      break;
    default:
      assert_true(description.cull_mode == PipelineCullMode::kNone ||
                  description.cull_mode ==
                      PipelineCullMode::kDisableRasterization);
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      break;
  }
  state_desc.RasterizerState.FrontCounterClockwise =
      description.front_counter_clockwise ? true : false;
  state_desc.RasterizerState.DepthBias = description.depth_bias;
  state_desc.RasterizerState.DepthBiasClamp = 0.0f;
  // With non-square resolution scaling, make sure the worst-case impact is
  // reverted (slope only along the scaled axis), thus max. More bias is better
  // than less bias, because less bias means Z fighting with the background is
  // more likely. Native draws get the guest bias as is.
  state_desc.RasterizerState.SlopeScaledDepthBias =
      description.depth_bias_slope_scaled *
      (description.resolution_scale_native
           ? 1.0f
           : float(std::max(render_target_cache_.draw_resolution_scale_x(),
                            render_target_cache_.draw_resolution_scale_y())));
  state_desc.RasterizerState.DepthClipEnable =
      description.depth_clip ? true : false;
  uint32_t msaa_sample_count = uint32_t(1)
                               << uint32_t(description.host_msaa_samples);
  if (edram_rov_used) {
    // Only 1, 4, 8 and (not on all GPUs) 16 are allowed, using sample 0 as 0
    // and 3 as 1 for 2x instead (not exactly the same sample positions, but
    // still top-left and bottom-right - however, this can be adjusted with
    // programmable sample positions).
    assert_true(msaa_sample_count == 1 || msaa_sample_count == 4);
    if (msaa_sample_count != 1 && msaa_sample_count != 4) {
      return nullptr;
    }
    state_desc.RasterizerState.ForcedSampleCount =
        uint32_t(1) << uint32_t(description.host_msaa_samples);
  }

  // Sample mask and description.
  state_desc.SampleMask = UINT_MAX;
  // TODO(Triang3l): 4x MSAA fallback when 2x isn't supported without ROV.
  if (edram_rov_used) {
    state_desc.SampleDesc.Count = 1;
  } else {
    assert_true(msaa_sample_count <= 4);
    if (msaa_sample_count > 4) {
      return nullptr;
    }
    if (msaa_sample_count == 2 && !render_target_cache_.msaa_2x_supported()) {
      // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
      // sample positions, but still top-left and bottom-right - however, this
      // can be adjusted with programmable sample positions).
      state_desc.SampleMask = 0b1001;
      state_desc.SampleDesc.Count = 4;
    } else {
      state_desc.SampleDesc.Count = msaa_sample_count;
    }
  }

  if (!edram_rov_used) {
    // Depth/stencil.
    if (description.depth_func != xenos::CompareFunction::kAlways ||
        description.depth_write) {
      state_desc.DepthStencilState.DepthEnable = true;
      state_desc.DepthStencilState.DepthWriteMask =
          description.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                  : D3D12_DEPTH_WRITE_MASK_ZERO;
      // Comparison functions are the same in Direct3D 12 but plus one (minus
      // one, bit 0 for less, bit 1 for equal, bit 2 for greater).
      state_desc.DepthStencilState.DepthFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.depth_func));
    }
    if (description.stencil_enable) {
      state_desc.DepthStencilState.StencilEnable = true;
      state_desc.DepthStencilState.StencilReadMask =
          description.stencil_read_mask;
      state_desc.DepthStencilState.StencilWriteMask =
          description.stencil_write_mask;
      // Stencil operations are the same in Direct3D 12 too but plus one.
      state_desc.DepthStencilState.FrontFace.StencilFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_depth_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilPassOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_pass_op));
      state_desc.DepthStencilState.FrontFace.StencilFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.stencil_front_func));
      state_desc.DepthStencilState.BackFace.StencilFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_fail_op));
      state_desc.DepthStencilState.BackFace.StencilDepthFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_depth_fail_op));
      state_desc.DepthStencilState.BackFace.StencilPassOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_pass_op));
      state_desc.DepthStencilState.BackFace.StencilFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.stencil_back_func));
    }
    if (state_desc.DepthStencilState.DepthEnable ||
        state_desc.DepthStencilState.StencilEnable) {
      state_desc.DSVFormat = D3D12RenderTargetCache::GetDepthDSVDXGIFormat(
          description.depth_format);
    }

    // Render targets and blending.
    state_desc.BlendState.IndependentBlendEnable = true;
    static constexpr D3D12_BLEND kBlendFactorMap[] = {
        D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
        D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_SRC_ALPHA_SAT,
    };
    // 8 entries for safety since 3 bits from the guest are passed directly.
    static constexpr D3D12_BLEND_OP kBlendOpMap[] = {
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,     D3D12_BLEND_OP_MIN,
        D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const PipelineRenderTarget& rt = description.render_targets[i];
      if (!rt.used) {
        // Null RTV descriptors can be used for slots with DXGI_FORMAT_UNKNOWN
        // in the pipeline state.
        state_desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        continue;
      }
      state_desc.NumRenderTargets = i + 1;
      state_desc.RTVFormats[i] =
          render_target_cache_.GetColorDrawDXGIFormat(rt.format);
      if (state_desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN) {
        assert_always();
        return nullptr;
      }
      D3D12_RENDER_TARGET_BLEND_DESC& blend_desc =
          state_desc.BlendState.RenderTarget[i];
      if (rt.src_blend != PipelineBlendFactor::kOne ||
          rt.dest_blend != PipelineBlendFactor::kZero ||
          rt.blend_op != xenos::BlendOp::kAdd ||
          rt.src_blend_alpha != PipelineBlendFactor::kOne ||
          rt.dest_blend_alpha != PipelineBlendFactor::kZero ||
          rt.blend_op_alpha != xenos::BlendOp::kAdd) {
        blend_desc.BlendEnable = true;
        blend_desc.BlendOp = kBlendOpMap[uint32_t(rt.blend_op)];
        if (blend_desc.BlendOp == D3D12_BLEND_OP_MIN ||
            blend_desc.BlendOp == D3D12_BLEND_OP_MAX) {
          blend_desc.SrcBlend = D3D12_BLEND_ONE;
          blend_desc.DestBlend = D3D12_BLEND_ONE;
          blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;
          blend_desc.DestBlendAlpha = D3D12_BLEND_ONE;
        } else {
          blend_desc.SrcBlend = kBlendFactorMap[uint32_t(rt.src_blend)];
          blend_desc.DestBlend = kBlendFactorMap[uint32_t(rt.dest_blend)];
          blend_desc.SrcBlendAlpha =
              kBlendFactorMap[uint32_t(rt.src_blend_alpha)];
          blend_desc.DestBlendAlpha =
              kBlendFactorMap[uint32_t(rt.dest_blend_alpha)];
        }
        blend_desc.BlendOpAlpha = kBlendOpMap[uint32_t(rt.blend_op_alpha)];
      }
      blend_desc.RenderTargetWriteMask = rt.write_mask;
    }
  }

  // Disable rasterization if needed (parameter combinations that make no
  // difference when rasterization is disabled have already been handled in
  // GetCurrentStateDescription) the way it's disabled in Direct3D by design
  // (disabling a pixel shader and depth / stencil).
  // TODO(Triang3l): When it happens to be that a combination of parameters
  // (no host pixel shader and depth / stencil without ROV) would disable
  // rasterization when it's still needed (for occlusion query sample counting),
  // ensure rasterization happens (by binding an empty pixel shader, or maybe
  // via ForcedSampleCount when not using 2x MSAA - its requirements for
  // OMSetRenderTargets need some investigation though).
  if (description.cull_mode == PipelineCullMode::kDisableRasterization) {
    state_desc.PS.pShaderBytecode = nullptr;
    state_desc.PS.BytecodeLength = 0;
    state_desc.DepthStencilState.DepthEnable = false;
    state_desc.DepthStencilState.StencilEnable = false;
  }

  // Create the D3D12 pipeline state object.
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  ID3D12PipelineState* state;
#if XE_PLATFORM_WINRT
  // Crash-journal probe (toxic-shader solver): record this pair as in-flight on
  // disk before the (potentially driver-crashing) creation call, and remove it
  // on any normal return via the guard's destructor. If the driver hard-crashes
  // the process inside CreateGraphicsPipelineState the destructor never runs,
  // so the pair is left in the journal and confirmed toxic on the next launch.
  // In safe mode the serialize lock guarantees exactly one pipeline is ever in
  // flight, so a crash pinpoints the culprit unambiguously.
  struct SolverProbe {
    PipelineCache& cache;
    std::pair<uint64_t, uint64_t> key;
    std::unique_lock<std::mutex> serialize_lock;
    bool active = false;
    ~SolverProbe() {
      // Leave the entry in the journal if the device was lost - it stays a
      // suspect for the next launch. Otherwise a normal return clears it.
      if (active && !cache.solver_device_lost_.load(std::memory_order_acquire)) {
        cache.SolverJournalEnd(key.first, key.second);
      }
    }
  } solver_probe{*this, std::make_pair(vs_hash, ps_hash)};
  // Only bracket the creation with the crash journal while recovering from a
  // crash (solver_journaling_); a normal run does zero per-creation file I/O.
  if (solver_enabled_ && solver_journaling_) {
    if (solver_safe_mode_) {
      // Safe mode serializes every creation to pin down which pipeline kills
      // the driver - which also means the game gets its pipelines one at a
      // time and renders almost nothing until they arrive. That's a price
      // worth paying for the first pipelines of a run, not for the whole
      // session: if this many have been created without a crash, whatever
      // killed the previous run isn't reproducing (a run killed by running out
      // of memory leaves the same "crashed" marker as a toxic shader), so stop
      // isolating and let creation go wide again.
      if (solver_safe_mode_creations_.fetch_add(1, std::memory_order_relaxed) >=
          kSolverSafeModeMaxCreations) {
        solver_safe_mode_ = false;
        XELOGW(
            "Toxic-shader solver: {} pipelines created in safe mode without a "
            "crash - the previous run's death isn't reproducing, resuming "
            "parallel creation (journaling stays on)",
            kSolverSafeModeMaxCreations);
      } else {
        solver_probe.serialize_lock =
            std::unique_lock<std::mutex>(solver_serialize_mutex_);
      }
    }
    SolverJournalBegin(vs_hash, ps_hash);
    solver_probe.active = true;
  }
  // Ask the pipeline library first - a hit skips the driver's compiler
  // entirely, which is the whole point of keeping it.
  std::wstring library_name;
  if (pipeline_library_) {
    library_name =
        fmt::format(L"{:016X}", XXH3_64bits(&description, sizeof(description)));
    std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
    if (SUCCEEDED(pipeline_library_->LoadGraphicsPipeline(
            library_name.c_str(), &state_desc, IID_PPV_ARGS(&state)))) {
      pipeline_library_hits_.fetch_add(1, std::memory_order_relaxed);
      return state;
    }
  }

  DWORD creation_exception_code = 0;
  HRESULT hr = CreateGraphicsPipelineStateGuarded(
      device, &state_desc, IID_PPV_ARGS(&state), &creation_exception_code);
  if (creation_exception_code != 0) {
    // With the process out of memory, the driver's shader compiler crashes on
    // its own failed allocations - on perfectly valid shaders (confirmed in
    // GTA IV at 2x2: E_OUTOFMEMORY failures interleaved with these AVs, and
    // the same pairs compile fine at 1x1). The crash may even PRECEDE the
    // first E_OUTOFMEMORY pipeline failure (the compiler allocates a lot), so
    // also check the actual memory headroom right now - if the title is
    // nearly out of its budget, this is an OOM victim, not a toxic shader.
    if (!solver_oom_seen_.load(std::memory_order_acquire)) {
      MEMORYSTATUSEX oom_check_status = {sizeof(oom_check_status)};
      // Watch COMMIT headroom (ullAvailPageFile), not physical - the compiler
      // fails its allocations when the commit charge, not physical RAM, is
      // exhausted (see the memory-pressure note in EndSubmission).
      if (GlobalMemoryStatusEx(&oom_check_status) &&
          oom_check_status.ullAvailPageFile < (UINT64_C(768) << 20)) {
        if (!solver_oom_seen_.exchange(true, std::memory_order_acq_rel)) {
          XELOGW(
              "Toxic-shader solver: the driver's shader compiler crashed with "
              "only {} MB of commit left - treating this and further crashes "
              "as out-of-memory victims, not toxic shaders (lower the memory "
              "usage - e.g. the resolution scale - instead)",
              uint64_t(oom_check_status.ullAvailPageFile) >> 20);
          std::error_code oom_ec;
          std::filesystem::remove(solver_running_path_, oom_ec);
          SolverRetractThisRunToxic();
        }
      }
    }
    // Skip the draws this run, but don't brand the pair toxic.
    if (solver_oom_seen_.load(std::memory_order_acquire)) {
      XELOGW(
          "Toxic-shader solver: the driver's shader compiler crashed "
          "(exception 0x{:08X}) on VS {:016X}, PS {:016X} while the process "
          "is OUT OF MEMORY - treating as an out-of-memory victim, skipping "
          "its draws this run WITHOUT quarantining",
          uint32_t(creation_exception_code), vs_hash, ps_hash);
      xe::FlushLog();
      return nullptr;
    }
    // The driver's shader compiler crashed on this pipeline and the exception
    // was contained. Quarantine the pair immediately - both for the next runs
    // (the .toxic file) and effectively for this run (this Pipeline object
    // keeps a null state, so its draws are skipped and it's never re-queued).
    XELOGE(
        "Toxic-shader solver: the driver's shader compiler CRASHED (exception "
        "0x{:08X}) creating the pipeline with VS {:016X}, PS {:016X} - the "
        "crash was contained, quarantining the pair and skipping its draws",
        uint32_t(creation_exception_code), vs_hash, ps_hash);
    if (solver_enabled_) {
      SolverAppendToxic(vs_hash, ps_hash);
      // Dump the exact DXBC the compiler crashed on next to the .toxic list
      // for offline analysis of the construct the driver chokes on (the
      // hashes identify the guest ucode; the DXBC differs per translation,
      // e.g. per resolution scale).
      if (!solver_toxic_path_.empty()) {
        auto dump_stage_dxbc = [this](const char* stage, uint64_t hash,
                                      const void* code, size_t code_size) {
          if (!code || !code_size) {
            return;
          }
          std::filesystem::path dump_path = solver_toxic_path_;
          dump_path += fmt::format(".{}_{:016X}.dxbc", stage, hash);
          std::error_code dump_ec;
          if (std::filesystem::exists(dump_path, dump_ec)) {
            return;
          }
          FILE* dump_file = xe::filesystem::OpenFile(dump_path, "wb");
          if (dump_file) {
            std::fwrite(code, 1, code_size, dump_file);
            std::fclose(dump_file);
            XELOGI("Toxic-shader solver: dumped crashing {} DXBC to {}", stage,
                   xe::path_to_utf8(dump_path));
          }
        };
        dump_stage_dxbc("vs", vs_hash, state_desc.VS.pShaderBytecode,
                        state_desc.VS.BytecodeLength);
        dump_stage_dxbc("ps", ps_hash, state_desc.PS.pShaderBytecode,
                        state_desc.PS.BytecodeLength);
      }
    }
    xe::FlushLog();
    return nullptr;
  }
#else
  HRESULT hr =
      device->CreateGraphicsPipelineState(&state_desc, IID_PPV_ARGS(&state));
#endif  // XE_PLATFORM_WINRT
  if (FAILED(hr)) {
    // If the device has been removed, every subsequent pipeline creation fails
    // too - report the removal reason once instead of flooding the log with
    // thousands of identical errors (the failures are a symptom, not a cause).
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
#if XE_PLATFORM_WINRT
      // The device is gone. Suppress the crash journal's normal removal for
      // every pipeline still in flight (the guard checks this flag) so they
      // remain suspects for the next launch even though the process survived a
      // *graceful* removal - a hard crash would have left them anyway. In
      // serialized safe mode exactly one pipeline was in flight, so this pair
      // is the confirmed culprit: append it to the per-game skip list now (the
      // in-memory set stays immutable; the file is reloaded next launch).
      if (solver_enabled_) {
        solver_device_lost_.store(true, std::memory_order_release);
      }
#endif  // XE_PLATFORM_WINRT
      static std::atomic<bool> device_removed_logged{false};
      if (!device_removed_logged.exchange(true)) {
        HRESULT reason = device->GetDeviceRemovedReason();
#if XE_PLATFORM_WINRT
        if (solver_enabled_ && solver_safe_mode_) {
          SolverAppendToxic(vs_hash, ps_hash);
          XELOGW(
              "Toxic-shader solver: confirmed VS {:016X}, PS {:016X} as toxic "
              "(caused device removal while isolated in safe mode); it will be "
              "skipped from the next launch",
              vs_hash, ps_hash);
        }
#endif  // XE_PLATFORM_WINRT
        XELOGE(
            "Failed to create graphics pipeline: THE GPU DEVICE WAS "
            "REMOVED/RESET (hr=0x{:08X}, GetDeviceRemovedReason=0x{:08X}). "
            "All further pipeline creations will fail; suppressing their "
            "logging. First failing pipeline: VS {:016X}, PS {:016X}",
            uint32_t(hr), uint32_t(reason),
            runtime_description.vertex_shader->shader().ucode_data_hash(),
            runtime_description.pixel_shader
                ? runtime_description.pixel_shader->shader().ucode_data_hash()
                : 0);
#if XE_PLATFORM_WINRT
        // Dump the DRED auto-breadcrumbs (enabled in the provider): the last
        // in-flight command list operations point at what killed the driver.
        {
          Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
          if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
            if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
              const D3D12_AUTO_BREADCRUMB_NODE* node =
                  breadcrumbs.pHeadAutoBreadcrumbNode;
              uint32_t node_index = 0;
              while (node && node_index < 16) {
                uint32_t executed =
                    node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue
                                               : 0;
                XELOGE(
                    "DRED node {}: cmdlist='{}' queue='{}' completed {}/{} "
                    "ops; last ops:",
                    node_index,
                    node->pCommandListDebugNameA ? node->pCommandListDebugNameA
                                                 : "?",
                    node->pCommandQueueDebugNameA
                        ? node->pCommandQueueDebugNameA
                        : "?",
                    executed, node->BreadcrumbCount);
                // Log the operations around the last executed one.
                uint32_t begin = executed > 8 ? executed - 8 : 0;
                uint32_t end = std::min(node->BreadcrumbCount, executed + 2);
                for (uint32_t i = begin; i < end; ++i) {
                  XELOGE("  op[{}]{} = {}", i, i == executed ? " <== DIED" : "",
                         uint32_t(node->pCommandHistory[i]));
                }
                node = node->pNext;
                ++node_index;
              }
            } else {
              XELOGW("DRED: GetAutoBreadcrumbsOutput failed");
            }
            D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
            if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault))) {
              XELOGE("DRED: page fault VA=0x{:016X}",
                     uint64_t(page_fault.PageFaultVA));
            }
          } else {
            XELOGW("DRED: ID3D12DeviceRemovedExtendedData unavailable");
          }
        }
#endif  // XE_PLATFORM_WINRT
        command_processor_.GetD3D12Provider().LogD3D12DebugMessages();
        xe::FlushLog();
      }
      return nullptr;
    }
#if XE_PLATFORM_WINRT
    if (hr == E_OUTOFMEMORY) {
      // The system is out of memory - from this point the driver's shader
      // compiler may CRASH (access violation on its own failed allocations)
      // on perfectly valid shaders. Those are OOM victims, not toxic
      // pipelines: stop quarantining, and drop the crash marker so a
      // subsequent out-of-memory process death doesn't promote the in-flight
      // pairs to the per-game skip list on the next launch.
      if (solver_enabled_ &&
          !solver_oom_seen_.exchange(true, std::memory_order_acq_rel)) {
        XELOGW(
            "Toxic-shader solver: pipeline creation failed with "
            "E_OUTOFMEMORY - the process is out of memory; driver shader "
            "compiler crashes from now on will NOT be treated as toxic "
            "shaders (lower the memory usage - e.g. the resolution scale or "
            "the post-processing output resolution - instead)");
        std::error_code oom_ec;
        std::filesystem::remove(solver_running_path_, oom_ec);
        // Pairs quarantined earlier in this run were likely OOM victims too.
        SolverRetractThisRunToxic();
      }
    }
#endif  // XE_PLATFORM_WINRT
    if (runtime_description.pixel_shader != nullptr) {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X}, PS {:016X}: "
          "HRESULT 0x{:08X}",
          runtime_description.vertex_shader->shader().ucode_data_hash(),
          runtime_description.pixel_shader->shader().ucode_data_hash(),
          uint32_t(hr));
    } else {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X}: HRESULT "
          "0x{:08X}",
          runtime_description.vertex_shader->shader().ucode_data_hash(),
          uint32_t(hr));
    }
    // Log D3D12 debug messages for all pipeline failures.
    command_processor_.GetD3D12Provider().LogD3D12DebugMessages();
    return nullptr;
  }
  std::wstring name;
  if (runtime_description.pixel_shader != nullptr) {
    name = fmt::format(
        L"VS {:016X}, PS {:016X}",
        runtime_description.vertex_shader->shader().ucode_data_hash(),
        runtime_description.pixel_shader->shader().ucode_data_hash());
  } else {
    name = fmt::format(
        L"VS {:016X}",
        runtime_description.vertex_shader->shader().ucode_data_hash());
  }
  state->SetName(name.c_str());

  // Hand the freshly compiled pipeline to the library so the next launch does
  // not have to compile it again.
  if (pipeline_library_ && !library_name.empty()) {
    pipeline_library_misses_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
    HRESULT store_hr =
        pipeline_library_->StorePipeline(library_name.c_str(), state);
    if (SUCCEEDED(store_hr)) {
      pipeline_library_dirty_ = true;
    } else if (store_hr != E_INVALIDARG) {
      // E_INVALIDARG just means this name is already stored (a pipeline whose
      // description hash collides with an existing entry, or a re-creation
      // after a cache clear) - not worth reporting.
      static std::atomic<bool> store_failure_logged{false};
      if (!store_failure_logged.exchange(true)) {
        XELOGW("Pipeline library: StorePipeline failed (0x{:08X})",
               uint32_t(store_hr));
      }
    }
  }
  return state;
}

void PipelineCache::CreationThread(size_t thread_index) {
  // Create thread-local translator to avoid contention with main thread.
  // This mirrors what the shader storage loading threads do.
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;
  StringBuffer ucode_disasm_buffer;
  DxbcShaderTranslator translator(
      provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
      !(edram_rov_used ||
        render_target_cache_.gamma_render_target_as_unorm16()),
      render_target_cache_.msaa_2x_supported(),
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y(),
      provider.GetGraphicsAnalysis() != nullptr);
  // Create thread-local DXIL conversion objects if needed.
  IDxbcConverter* dxbc_converter = nullptr;
  IDxcUtils* dxc_utils = nullptr;
  IDxcCompiler* dxc_compiler = nullptr;
  if (cvars::d3d12_dxbc_disasm_dxilconv && dxbc_converter_ && dxc_utils_ &&
      dxc_compiler_) {
    provider.DxbcConverterCreateInstance(CLSID_DxbcConverter,
                                         IID_PPV_ARGS(&dxbc_converter));
    provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils));
    provider.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
  }

  while (true) {
    Pipeline* pipeline_to_create = nullptr;

    // Check if need to shut down or set the completion event and dequeue the
    // pipeline if there is any.
    {
      std::unique_lock<xe_mutex> lock(creation_request_lock_);
      if (thread_index >= creation_threads_shutdown_from_ ||
          creation_queue_.empty()) {
        if (creation_threads_busy_ == 0) {
          // Last pipeline in the queue created.
          if (creation_completion_set_event_) {
            // Signal the event if requested (blocking mode).
            creation_completion_set_event_ = false;
            creation_completion_event_->Set();
          }
          if (creation_completion_callback_) {
            // Invoke completion callback (non-blocking mode).
            auto callback = std::move(creation_completion_callback_);
            creation_completion_callback_ = nullptr;
            lock.unlock();
            callback();
            lock.lock();
          }
        }
        if (thread_index >= creation_threads_shutdown_from_) {
          // Cleanup thread-local resources.
          if (dxc_compiler) {
            dxc_compiler->Release();
          }
          if (dxc_utils) {
            dxc_utils->Release();
          }
          if (dxbc_converter) {
            dxbc_converter->Release();
          }
          return;
        }
        creation_request_cond_.wait(lock);
        continue;
      }
      // Take the pipeline from the queue and increment the busy thread count
      // until the pipeline is created - other threads must be able to dequeue
      // requests, but can't set the completion event until the pipelines are
      // fully created (rather than just started creating).
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
      if (pipeline_to_create->state.load(std::memory_order_acquire)) {
        // Already built. A pipeline can legitimately be in the queue twice:
        // PrioritizePipelineForPendingDraw re-queues it with a higher priority
        // because std::priority_queue cannot re-sort in place. Drop the
        // duplicate, releasing the reference that queueing took.
        ReleasePipelineTranslationsFromCreation(pipeline_to_create);
        continue;
      }
      ++creation_threads_busy_;
    }

    // Translation and pipeline creation allocate freely (the DXBC buffers, the
    // translator's containers, the driver's own allocations). On the
    // memory-constrained Xbox target those allocations do run out, and a
    // std::bad_alloc escaping this thread function terminates the whole
    // process - the emulator died from an unhandled 0xE06D7363 on the creation
    // threads while the game itself was still perfectly alive. Treat it like
    // any other creation failure instead: the pipeline stays null, its draws
    // are skipped, and the run continues (degraded) until memory frees up.
    ID3D12PipelineState* new_state = nullptr;
    bool creation_out_of_memory = false;
    try {
      // Translate pending shaders and update root signature.
      EnsurePipelineShadersTranslated(pipeline_to_create, translator,
                                      ucode_disasm_buffer, dxbc_converter,
                                      dxc_utils, dxc_compiler,
                                      /*use_try_claim=*/true,
                                      /*handle_non_placeholder=*/true);

      // Create the D3D12 pipeline state object.
      new_state = CreateD3D12Pipeline(pipeline_to_create->description);
    } catch (const std::bad_alloc&) {
      creation_out_of_memory = true;
    }

    // Store the pipeline. If creation failed, state stays nullptr and draws
    // will be skipped.
    if (new_state != nullptr) {
      pipeline_to_create->state.store(new_state, std::memory_order_release);
    } else {
      // The detailed reason (including device removal) is already logged by
      // CreateD3D12Pipeline; keep this summary heavily throttled.
      static std::atomic<uint32_t> creation_failed_log_count{0};
      uint32_t n = creation_failed_log_count.fetch_add(1);
      if (n < 5 || (n % 500) == 0) {
        XELOGE(
            "Pipeline creation failed{} (VS {:016X}, PS {:016X}) ({})",
            creation_out_of_memory ? " - OUT OF MEMORY (host allocation threw; "
                                     "the pipeline is skipped, not quarantined "
                                     "- lower the memory usage, e.g. the "
                                     "resolution scale)"
                                   : "",
            pipeline_to_create->description.vertex_shader
                ? pipeline_to_create->description.vertex_shader->shader()
                      .ucode_data_hash()
                : 0,
            pipeline_to_create->description.pixel_shader
                ? pipeline_to_create->description.pixel_shader->shader()
                      .ucode_data_hash()
                : 0,
            n + 1);
      }
    }

    // Pipeline created - the thread is not busy anymore, safe to set the
    // completion event if needed (at the next iteration, or in some other
    // thread). The translations are no longer being read, so the
    // memory-pressure release may free them from now on.
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      ReleasePipelineTranslationsFromCreation(pipeline_to_create);
      --creation_threads_busy_;
    }
  }
}

void PipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  assert_false(creation_threads_.empty());
  while (true) {
    Pipeline* pipeline_to_create;
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      if (creation_queue_.empty()) {
        break;
      }
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
      if (pipeline_to_create->state.load(std::memory_order_acquire)) {
        // Duplicate left by a priority bump - see the creation thread.
        ReleasePipelineTranslationsFromCreation(pipeline_to_create);
        continue;
      }
    }

    // Same host-OOM containment as on the creation threads - this runs on the
    // command processor thread, where an escaping std::bad_alloc would take
    // the whole emulator down instead of just skipping a pipeline.
    ID3D12PipelineState* new_state = nullptr;
    bool creation_out_of_memory = false;
    try {
      // Translate pending shaders and update root signature.
      EnsurePipelineShadersTranslated(pipeline_to_create, *shader_translator_,
                                      ucode_disasm_buffer_, dxbc_converter_,
                                      dxc_utils_, dxc_compiler_,
                                      /*use_try_claim=*/true,
                                      /*handle_non_placeholder=*/true);

      new_state = CreateD3D12Pipeline(pipeline_to_create->description);
    } catch (const std::bad_alloc&) {
      creation_out_of_memory = true;
    }

    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      ReleasePipelineTranslationsFromCreation(pipeline_to_create);
    }

    // Store the pipeline. If creation failed, state stays nullptr.
    if (new_state != nullptr) {
      pipeline_to_create->state.store(new_state, std::memory_order_release);
    } else {
      XELOGW("ProcessorThread: Pipeline creation failed{}",
             creation_out_of_memory ? " - OUT OF MEMORY" : "");
    }
  }
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
