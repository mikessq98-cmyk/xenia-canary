/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_PIPELINE_CACHE_H_
#define XENIA_GPU_D3D12_PIPELINE_CACHE_H_

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/hash.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"
#include "xenia/gpu/d3d12/d3d12_shader.h"
#include "xenia/gpu/d3d12/toxic_shader_solver.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/primitive_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace gpu {
namespace d3d12 {

class D3D12CommandProcessor;

class PipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  PipelineCache(D3D12CommandProcessor& command_processor,
                const RegisterFile& register_file,
                const D3D12RenderTargetCache& render_target_cache,
                bool bindless_resources_used);
  ~PipelineCache();

  bool Initialize();
  void Shutdown();
  // No ClearCache because it's undesirable with the persistent shader storage
  // (if the storage is reloaded, effectively nothing is cleared, while the call
  // takes a long time, and if it's not, there will be heavy stuttering for the
  // rest of the execution of the guest).

  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr);
  void ShutdownShaderStorage();

#if XE_PLATFORM_WINRT
  // The toxic-shader solver itself - see toxic_shader_solver.h. Thin
  // forwarders below for the calls the command processor makes, so the device
  // loss and draw paths do not have to reach through the pipeline cache into
  // another object.
  ToxicShaderSolver& solver() { return solver_; }
  // Marks that the host GPU device has been lost (callable from any thread) so
  // the solver keeps its crash journal on shutdown even though the process
  // exits gracefully - covers device loss detected outside pipeline creation
  // (present/submission), including a creation call that hung the driver's
  // shader compiler and never returned.
  void SolverOnDeviceLost() { solver_.OnDeviceLost(); }
  // Records that the device died while EXECUTING draws rather than while
  // creating a pipeline. The next launch then runs draws one at a time,
  // journalling each, so the hang leaves exactly one suspect behind.
  void SolverMarkExecutionHang() { solver_.MarkExecutionHang(); }
  bool solver_execution_safe_mode() const {
    return solver_.execution_safe_mode();
  }
  // Called just before a draw is submitted in execution safe mode.
  void SolverExecutionJournalDraw(uint64_t vertex_shader_hash,
                                  uint64_t pixel_shader_hash) {
    solver_.ExecutionJournalDraw(vertex_shader_hash, pixel_shader_hash);
  }
  // Whether this pair, in this pipeline state, has never been seen to survive
  // execution. `state_key` is the pipeline's description hash - see
  // GetPipelineStateKeyByHandle.
  bool SolverNeedsExecutionVerification(uint64_t vertex_shader_hash,
                                        uint64_t pixel_shader_hash,
                                        uint64_t state_key) {
    return solver_.NeedsExecutionVerification(vertex_shader_hash,
                                              pixel_shader_hash, state_key);
  }
  // The pair drew without hanging the GPU. Remembered across launches.
  void SolverMarkExecutionVerified(uint64_t vertex_shader_hash,
                                   uint64_t pixel_shader_hash,
                                   uint64_t state_key) {
    solver_.MarkExecutionVerified(vertex_shader_hash, pixel_shader_hash,
                                  state_key);
  }
  // The identity of the exact pipeline STATE behind a handle. Zero when the
  // handle is not a guest pipeline. The same guest shaders are translated into
  // several modifications - different DXBC out of our own translator - so a
  // pair proven safe in one of them has not been proven in another.
  uint64_t GetPipelineStateKeyByHandle(void* handle) const {
    return handle ? reinterpret_cast<const Pipeline*>(handle)->description_hash
                  : 0;
  }
  void SolverQuarantineExecutionSuspect(uint64_t vertex_shader_hash,
                                        uint64_t pixel_shader_hash) {
    solver_.QuarantineExecutionSuspect(vertex_shader_hash, pixel_shader_hash);
  }
  // An isolated draw hung the GPU - exact attribution, not a suspect.
  void SolverQuarantineIsolatedExecutionHang(uint64_t vertex_shader_hash,
                                             uint64_t pixel_shader_hash) {
    solver_.QuarantineIsolatedExecutionHang(vertex_shader_hash,
                                            pixel_shader_hash);
  }
  // Quarantined at startup or at any point since - the draw path asks per draw,
  // so a pair quarantined mid-run stops being submitted immediately.
  bool SolverIsShaderToxic(uint64_t vertex_shader_hash,
                           uint64_t pixel_shader_hash) const {
    return solver_.IsShaderToxic(vertex_shader_hash, pixel_shader_hash);
  }
  size_t solver_verified_at_startup() const {
    return solver_.verified_at_startup();
  }
  size_t solver_verified_this_run() const {
    return solver_.verified_this_run();
  }
#endif  // XE_PLATFORM_WINRT

  // What the creation side has cost the CPU this session: thread time, the
  // split between our translator and the driver's compiler, and how long
  // pipelines wait in the queue. One line for the periodic memory report.
  std::string GetCreationCostReport() const;
  // CPU consumed by the compilers so far, for the process-wide CPU split.
  uint64_t creation_cpu_100ns() const {
    return creation_cpu_100ns_.load(std::memory_order_relaxed);
  }
  // What the shader storage has committed to disk this session.
  std::string GetShaderStorageWriteReport() {
    return storage_writer_.GetWriteReport();
  }

  // The guest finished a frame - compilation may pay down its debt.
  void NoteGuestFrameForCreationBudget() {
#if XE_PLATFORM_WINRT
    RefillCreationBudget();
#endif  // XE_PLATFORM_WINRT
  }
  // One line for the periodic report: what the governor is doing and why.
  std::string GetCreationGovernorReport() const;

  void EndSubmission();
  bool IsCreatingPipelines();
  // How many compilers are inside the driver right now. Sampled by the texture
  // cache around its own driver calls, to find out whether the two contend.
  uint32_t GetPipelinesBeingCreated() {
    std::lock_guard<xe_mutex> lock(creation_request_lock_);
    return uint32_t(creation_threads_busy_);
  }
  // Waits for any pipeline creation needed by the current draw path to finish
  // before state is consumed. This was added so strict ZPD query paths stop
  // racing pipeline compilation and then blocking work on incomplete state.
  void AwaitPipelineCompletion();

  D3D12Shader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) {
    if (!shader.is_ucode_analyzed()) {
      shader.AnalyzeUcode(ucode_disasm_buffer_);
    }
  }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  DxbcShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  DxbcShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      bool apply_polygon_offset_in_shader) const;

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this!
  bool ConfigurePipeline(
      D3D12Shader::D3D12Translation* vertex_shader,
      D3D12Shader::D3D12Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask, bool apply_polygon_offset_in_shader,
      uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_targets_formats,
      void** pipeline_handle_out, ID3D12RootSignature** root_signature_out);

  // Returns a pipeline with deferred creation by its handle. May return nullptr
  // if failed to create the pipeline or still being created asynchronously.
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)->state.load(
        std::memory_order_acquire);
  }
  ID3D12PipelineState* AwaitD3D12PipelineByHandle(void* handle);

  ID3D12RootSignature* GetRootSignatureByHandle(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)
        ->description.root_signature;
  }

#if XE_PLATFORM_WINRT
  // A READY pipeline interchangeable with the given still-being-built one -
  // same root signature, vertex shader and fixed-function state, only the
  // pixel shader differs - or nullptr. Drawing with it instead of skipping
  // the draw is what prevents "black objects" (a multi-pass renderer keeps
  // its depth pass but loses the material pass) while the console driver
  // chews through a permutation burst. Command processor thread only: the
  // search re-runs (at most once per submission) while no substitute has
  // been found, because a suitable pipeline is usually only finished AFTER
  // the pending one was first asked for.
  // Whether this guest pixel shader is one the microcode interpreter can
  // execute - see d3d12_interpreter_render.
  static bool InterpreterCanRun(const Shader& shader);
  // Why the shaders it turned down were turned down - which restriction to
  // lift next is not a guess if the reasons are counted apart.
  static std::string GetInterpreterDeclineReport();

  void* GetReadySubstituteByHandle(void* handle);
  // The pixel shader a handle will actually execute with, so a draw taking a
  // stand-in can bind for it rather than for the one it asked for.
  D3D12Shader* GetPixelShaderByHandle(void* handle) const {
    if (!handle) {
      return nullptr;
    }
    D3D12Shader::D3D12Translation* translation =
        reinterpret_cast<const Pipeline*>(handle)->description.pixel_shader;
    return translation ? static_cast<D3D12Shader*>(&translation->shader())
                       : nullptr;
  }
#endif  // XE_PLATFORM_WINRT

  // Tells the cache that a draw is being skipped for want of this pipeline, so
  // it can be moved to the front of the creation queue. Priorities are
  // assigned once, from what a pipeline writes - which says nothing about
  // WHEN it is needed, so a pipeline the game is waiting on right now can sit
  // behind hundreds that were queued earlier and are not needed yet.
  // Command processor thread only.
  // `completes_mesh` puts it ahead of other pending draws: it is the last
  // pipeline state a mesh is waiting on, so building it finishes that mesh.
  void PrioritizePipelineForPendingDraw(void* handle,
                                        bool completes_mesh = false);

  // Adds creation threads while the queue is deep enough for the extra
  // parallelism to be worth the cores. Command processor thread only.
  bool EnsureCreationThreadsForQueueDepth();

  // The interpolator mask to translate a vertex shader (and the pixel shader
  // paired with it) for: the union of what every pixel shader seen with this
  // vertex shader so far actually reads.
  //
  // Using the intersection of the two shaders makes the vertex shader's
  // translation depend on which pixel shader it is paired with, so it is
  // translated again for each of them. Using everything the vertex shader
  // writes removes that, but then it exports interpolators nobody reads, which
  // costs real GPU time in a heavy scene. The union is the middle ground: it
  // settles after the first few pipelines (a vertex shader is used with a
  // handful of distinct interpolator layouts, not a new one every time), and
  // it never exports more than some pixel shader genuinely wanted.
  // Command processor thread only.
  uint32_t GetSharedInterpolatorMask(uint64_t vertex_shader_ucode_hash,
                                     uint32_t vertex_shader_writes,
                                     uint32_t pixel_shader_reads);

#if XE_PLATFORM_WINRT
  // How hard to look for stand-ins - see d3d12_substitute_pending_pipelines.
  enum class SubstituteMode {
    kOff,
    kOnce,
    kAlways,
  };
#endif  // XE_PLATFORM_WINRT

  // Total resident translated shader bytecode (DXBC), for telemetry. Atomic
  // scalar, safe to read from any thread.
  // Pipelines in the cache and how many of them have no state object yet
  // (still queued or being created). A count that keeps climbing means the
  // game is asking for new pipeline permutations faster than they can be
  // built - the draws using them are skipped meanwhile, which looks like
  // missing geometry and a machine busy doing nothing visible.
  void GetPipelineCounts(size_t& total_out, size_t& pending_out) const {
    total_out = pipelines_.size();
    size_t pending = 0;
    for (const auto& pipeline_pair : pipelines_) {
      if (!pipeline_pair.second->state.load(std::memory_order_relaxed)) {
        ++pending;
      }
    }
    pending_out = pending;
  }

  // How many DISTINCT pixel shaders the busiest vertex shader is paired with,
  // and that vertex shader's hash - a game pairing one VS with a great many
  // PS (a material/permutation explosion) would show a large number here.
  // Computed from the pipeline map, so it costs a walk of it; only call from
  // the telemetry path.
  void GetMaxPixelShadersPerVertexShader(uint32_t& max_ps_out,
                                         uint64_t& vertex_shader_hash_out) const {
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> ps_by_vs;
    for (const auto& pipeline_pair : pipelines_) {
      const PipelineRuntimeDescription& desc = pipeline_pair.second->description;
      if (!desc.vertex_shader) {
        continue;
      }
      uint64_t vs_hash = desc.vertex_shader->shader().ucode_data_hash();
      uint64_t ps_hash =
          desc.pixel_shader ? desc.pixel_shader->shader().ucode_data_hash() : 0;
      ps_by_vs[vs_hash].insert(ps_hash);
    }
    uint32_t max_ps = 0;
    uint64_t worst_vs = 0;
    for (const auto& vs_pair : ps_by_vs) {
      if (vs_pair.second.size() > max_ps) {
        max_ps = uint32_t(vs_pair.second.size());
        worst_vs = vs_pair.first;
      }
    }
    max_ps_out = max_ps;
    vertex_shader_hash_out = worst_vs;
  }

  // Releases translated bytecode at the memory arbiter's request. The arbiter
  // has already established that the host is short, so unlike the pressure
  // path this does not re-check the budget itself.
  // Releases translated bytecode until bytes_to_free has been freed, returning
  // how much was actually freed. Bounded on purpose - see the definition.
  uint64_t ReleaseTranslationsForArbiter(uint64_t bytes_to_free);

  uint64_t GetTranslatedShaderBytes() const {
    return translated_shader_bytes_.load(std::memory_order_relaxed);
  }

 private:
  // Update PipelineDescription::kVersion if any of the Pipeline* enums are
  // changed!

  enum class PipelineStripCutIndex : uint32_t {
    kNone,
    kFFFF,
    kFFFFFFFF,
  };

  enum class PipelineTessellationMode : uint32_t {
    kNone,
    kDiscrete,
    kContinuous,
    kAdaptive,
  };

  enum class PipelinePatchType : uint32_t {
    kNone,
    kLine,
    kTriangle,
    kQuad,
  };

  enum class PipelinePrimitiveTopologyType : uint32_t {
    kPoint,
    kLine,
    kTriangle,
  };

  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  enum class PipelineCullMode : uint32_t {
    kNone,
    kFront,
    kBack,
    // Special case, handled via disabling the pixel shader and depth / stencil.
    kDisableRasterization,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kInvSrcColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDestColor,
    kInvDestColor,
    kDestAlpha,
    kInvDestAlpha,
    kBlendFactor,
    kInvBlendFactor,
    kSrcAlphaSat,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  XEPACKEDSTRUCT(PipelineRenderTarget, {
    uint32_t used : 1;                          // 1
    xenos::ColorRenderTargetFormat format : 4;  // 5
    PipelineBlendFactor src_blend : 4;          // 9
    PipelineBlendFactor dest_blend : 4;         // 13
    xenos::BlendOp blend_op : 3;                // 16
    PipelineBlendFactor src_blend_alpha : 4;    // 20
    PipelineBlendFactor dest_blend_alpha : 4;   // 24
    xenos::BlendOp blend_op_alpha : 3;          // 27
    uint32_t write_mask : 4;                    // 31
  });

  XEPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if drawing without a pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;

    int32_t depth_bias;
    float depth_bias_slope_scaled;

    PipelineStripCutIndex strip_cut_index : 2;  // 2
    // PipelinePrimitiveTopologyType for a vertex shader.
    // xenos::TessellationMode for a domain shader.
    uint32_t primitive_topology_type_or_tessellation_mode : 2;  // 4
    // Zero for non-kVertex host_vertex_shader_type.
    PipelineGeometryShader geometry_shader : 2;       // 6
    uint32_t fill_mode_wireframe : 1;                 // 7
    PipelineCullMode cull_mode : 2;                   // 9
    uint32_t front_counter_clockwise : 1;             // 10
    uint32_t depth_clip : 1;                          // 11
    xenos::MsaaSamples host_msaa_samples : 2;         // 13
    xenos::DepthRenderTargetFormat depth_format : 1;  // 14
    xenos::CompareFunction depth_func : 3;            // 17
    uint32_t depth_write : 1;                         // 18
    uint32_t stencil_enable : 1;                      // 19
    uint32_t stencil_read_mask : 8;                   // 27
    // Native draw (scale threshold), keeps slope-scale unscaled.
    uint32_t resolution_scale_native : 1;  // 28

    uint32_t stencil_write_mask : 8;                   // 8
    xenos::StencilOp stencil_front_fail_op : 3;        // 11
    xenos::StencilOp stencil_front_depth_fail_op : 3;  // 14
    xenos::StencilOp stencil_front_pass_op : 3;        // 17
    xenos::CompareFunction stencil_front_func : 3;     // 20
    xenos::StencilOp stencil_back_fail_op : 3;         // 23
    xenos::StencilOp stencil_back_depth_fail_op : 3;   // 26
    xenos::StencilOp stencil_back_pass_op : 3;         // 29
    xenos::CompareFunction stencil_back_func : 3;      // 32

    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    inline bool operator==(const PipelineDescription& other) const;
    // 20260811: descriptions are canonicalized before hashing
    // (NormalizePipelineDescription), so a description written by an earlier
    // build hashes differently from the same state today. Without the bump the
    // stored pipelines load under their old hashes, every draw then misses and
    // waits for a pipeline to be built again, and the geometry is missing until
    // it is - which is exactly what a second launch looked like.
    static constexpr uint32_t kVersion = 0x20260811;
  });

  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  struct PipelineRuntimeDescription {
    ID3D12RootSignature* root_signature;
    D3D12Shader::D3D12Translation* vertex_shader;
    D3D12Shader::D3D12Translation* pixel_shader;
    const std::vector<uint32_t>* geometry_shader;
    PipelineDescription description;
  };

  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 2;
      uint32_t interpolator_count : 5;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
    };

    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const GeometryShaderKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const GeometryShaderKey& other_key) const {
      return !(*this == other_key);
    }
  };

  D3D12Shader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count,
                          uint64_t data_hash);

  // Can be called from multiple threads.
  bool TranslateAnalyzedShader(DxbcShaderTranslator& translator,
                               D3D12Shader::D3D12Translation& translation,
                               IDxbcConverter* dxbc_converter = nullptr,
                               IDxcUtils* dxc_utils = nullptr,
                               IDxcCompiler* dxc_compiler = nullptr);

  // Translates shaders in parallel for storage loading.
  void TranslateShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
      bool edram_rov_used);

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this! The shaders must be translated
  // and valid, unless for_placeholder is true.
  // When for_placeholder is true (async pipeline creation):
  // - Shaders don't need to be translated yet (only hash/modification used)
  // - Root signature uses VS bindings only, updated after background
  // translation
  bool GetCurrentStateDescription(
      D3D12Shader::D3D12Translation* vertex_shader,
      D3D12Shader::D3D12Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask, bool depth_bias_in_pixel_shader,
      uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_target_formats,
      PipelineRuntimeDescription& runtime_description_out,
      bool for_placeholder = false);

  // Clears the fields of a description that cannot affect rendering given the
  // state that governs them (blend factors with the write mask closed, stencil
  // operations with the test off), so descriptions that differ only in dead
  // bits hash to the same pipeline instead of each compiling their own.
  // Not static: it also strips the fields the driver will take from the
  // command list, which depends on what this device supports.
  void NormalizePipelineDescription(PipelineDescription& description);

  static bool GetGeometryShaderKey(
      PipelineGeometryShader geometry_shader_type,
      DxbcShaderTranslator::Modification vertex_shader_modification,
      DxbcShaderTranslator::Modification pixel_shader_modification,
      GeometryShaderKey& key_out);
  static void CreateDxbcGeometryShader(GeometryShaderKey key,
                                       std::vector<uint32_t>& shader_out);
  const std::vector<uint32_t>& GetGeometryShader(GeometryShaderKey key);

  ID3D12PipelineState* CreateD3D12Pipeline(
      const PipelineRuntimeDescription& runtime_description);

#if XE_PLATFORM_WINRT
  // Everything about which shader pairs the driver cannot survive lives in
  // ToxicShaderSolver now - it was two hundred lines of unrelated state in the
  // middle of pipeline creation. The pipeline cache reports what happened
  // (a contained compiler crash, an out-of-memory failure, a device removal)
  // and the solver decides what it means.
  ToxicShaderSolver solver_;
#endif  // XE_PLATFORM_WINRT


  // Sum of resident translated shader bytecode sizes (incremented on a
  // successful translation, decremented when released under memory pressure).
  // Unconditional - the translate path that updates it is compiled on all
  // platforms.
  std::atomic<uint64_t> translated_shader_bytes_{0};

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  const D3D12RenderTargetCache& render_target_cache_;
  bool bindless_resources_used_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  StringBuffer ucode_disasm_buffer_;
  // Reusable shader translator for the processor thread.
  // Background creation threads have their own translators to avoid contention.
  std::unique_ptr<DxbcShaderTranslator> shader_translator_;

  // Command processor thread DXIL conversion/disassembly interfaces, if DXIL
  // disassembly is enabled.
  IDxbcConverter* dxbc_converter_ = nullptr;
  IDxcUtils* dxc_utils_ = nullptr;
  IDxcCompiler* dxc_compiler_ = nullptr;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, D3D12Shader*, xe::hash::IdentityHasher<uint64_t>>
      shaders_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<D3D12Shader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID,
                          xe::hash::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;
  // Bindless sampler indices of different shaders, for obtaining layout UIDs.
  // For bindful, sampler count is used as the UID instead.
  std::vector<uint32_t> bindless_sampler_layouts_;
  // Keys are XXH3 hashes of used bindless sampler indices.
  std::unordered_multimap<uint64_t, LayoutUID,
                          xe::hash::IdentityHasher<uint64_t>>
      bindless_sampler_layout_map_;

  // Geometry shaders for Xenos primitive types not supported by Direct3D 12.
  std::unordered_map<GeometryShaderKey, std::vector<uint32_t>,
                     GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer via ROV when no
  // Xenos pixel shader provided.
  std::vector<uint8_t> depth_only_pixel_shader_;

  struct Pipeline {
    // nullptr if creation has failed or still pending.
    std::atomic<ID3D12PipelineState*> state{nullptr};
    PipelineRuntimeDescription description;
#if XE_PLATFORM_WINRT
    // See GetReadySubstituteByHandle. Pipelines live until ClearCache, so a
    // found substitute stays valid for this pipeline's whole life.
    std::atomic<Pipeline*> substitute{nullptr};
    // Hash over everything a substitute must match (root signature, vertex
    // and geometry shader, and the whole fixed-function description with the
    // pixel shader identity zeroed) - the key into substitute_index_.
    uint64_t substitute_key = 0;
    // Submission of the last unsuccessful substitute search, so the search
    // runs at most once per submission per pipeline.
    uint64_t substitute_search_submission = UINT64_MAX;
    // Hash of the whole description - the identity of this exact pipeline
    // STATE, as opposed to the shader pair it draws with. The execution
    // verification uses it: the same guest shaders are translated into several
    // different modifications (interpolator layout, early-Z hint, param-gen),
    // which is DIFFERENT DXBC produced by our own translator, and a pair proven
    // safe in one of them says nothing about another.
    uint64_t description_hash = 0;
    // Set once a draw has been skipped for want of this pipeline. Until then
    // it is speculative and belongs to the frozen tier - see CreationAdmitted.
    std::atomic<bool> demanded{false};
#endif  // XE_PLATFORM_WINRT
    // For background creation: stores the untranslated shaders.
    // Background thread translates both VS and PS together, then creates the
    // pipeline. Set to nullptr after translation is done.
    D3D12Shader::D3D12Translation* pending_vertex_shader{nullptr};
    D3D12Shader::D3D12Translation* pending_pixel_shader{nullptr};
    // Priority for async compilation (higher = compiled sooner).
    // Pipelines that write to visible render targets get higher priority.
    uint8_t priority{0};
    // When this pipeline was first put in the creation queue, so the wait
    // before a thread picks it up can be measured. Every submission it spends
    // waiting is a submission where its draws are skipped.
    uint64_t queued_tick{0};
  };

  // Marks the pipeline's translations as referenced by a queued or in-flight
  // creation, so the memory-pressure release can free everything else while
  // creation is running. Both must be called with creation_request_lock_ held:
  // acquire when pushing to creation_queue_, release when the creation attempt
  // ends (successful or not).
  static void AcquirePipelineTranslationsForCreation(Pipeline* pipeline);
  static void ReleasePipelineTranslationsFromCreation(Pipeline* pipeline);

#if XE_PLATFORM_WINRT
  // Hash over everything a substitute must match - see Pipeline::
  // substitute_key.
  static uint64_t ComputeSubstituteKey(
      const PipelineRuntimeDescription& runtime_description);
  // Whether two pipelines may stand in for each other: everything except the
  // pixel shader identity must be equal, and the root signature must be the
  // SAME object (a substituted bind under a different root signature is a
  // device loss).
  // How good a stand-in one pipeline is for another. Ordered best first - the
  // search takes the best it finds rather than the first.
  enum class SubstituteQuality {
    // Same guest pixel shader, translated for a different modification. Reads
    // and writes exactly the same resources (it is the same microcode), so the
    // worst case is wrong shading. This is the common case in a title that
    // draws one material under two states.
    kSameShaderOtherModification,
    // A different pixel shader that happens to sample the same textures
    // through the same bindings. Shades the object with someone else's logic -
    // visible, but it cannot read a descriptor the draw didn't bind.
    kOtherShaderSameBindings,
    // A different pixel shader reading different textures. Only usable because
    // the draw writes the descriptors for the SUBSTITUTE's shaders, not the
    // real ones - see d3d12_substitute_scope.
    kOtherShaderOtherBindings,
    kUnusable,
  };
  // What may stand in - see the cvar.
  enum class SubstituteScope {
    kStrict,
    kSimilar,
    kAny,
  };
  // One pipeline from the microcode interpreter probe shader, timed. Nothing
  // renders through it - see d3d12_interpreter_probe.
  void RunInterpreterProbe();

  // Every field of the description except the shaders, for the census table.
  static std::string DescribeRenderState(const PipelineDescription& description);

  SubstituteQuality GetSubstituteQuality(
      const PipelineRuntimeDescription& a, const PipelineRuntimeDescription& b);
  // All pipelines by substitute key, ready or not (readiness is checked when
  // picking one). Processor thread only.
  std::unordered_multimap<uint64_t, Pipeline*> substitute_index_;
  // Candidates turned down as unusable - see GetReadySubstituteByHandle.
  std::atomic<uint32_t> substitutes_rejected_{0};
  // Parsed once at initialization - this is read per skipped draw.
  SubstituteMode substitute_mode_ = SubstituteMode::kOnce;
  SubstituteScope substitute_scope_ = SubstituteScope::kStrict;
  static std::atomic<uint64_t> interpreter_declined_control_flow_;
  static std::atomic<uint64_t> interpreter_declined_textures_;
  static std::atomic<uint64_t> interpreter_declined_outputs_;
  static std::atomic<uint64_t> interpreter_declined_length_;
  // Fields the driver can take from the command list, so pipelines are not
  // specialised for them - see d3d12_dynamic_pipeline_state.
  bool dynamic_depth_bias_ = false;
  bool dynamic_strip_cut_ = false;
  // What the description would have carried, kept for the draw to set on the
  // command list. Written and read on the command processor thread only.
  int32_t pending_dynamic_depth_bias_ = 0;
  float pending_dynamic_depth_bias_slope_ = 0.0f;
  PipelineStripCutIndex pending_dynamic_strip_cut_ =
      PipelineStripCutIndex::kNone;

 public:
  bool dynamic_depth_bias() const { return dynamic_depth_bias_; }
  bool dynamic_strip_cut() const { return dynamic_strip_cut_; }
  int32_t pending_dynamic_depth_bias() const {
    return pending_dynamic_depth_bias_;
  }
  float pending_dynamic_depth_bias_slope() const {
    return pending_dynamic_depth_bias_slope_;
  }
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE pending_dynamic_strip_cut() const {
    switch (pending_dynamic_strip_cut_) {
      case PipelineStripCutIndex::kFFFF:
        return D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      case PipelineStripCutIndex::kFFFFFFFF:
        return D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      default:
        return D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    }
  }

 private:
#endif  // XE_PLATFORM_WINRT

  // Comparator for priority queue - higher priority first.
  struct PipelineCreationPriorityCompare {
    bool operator()(const Pipeline* a, const Pipeline* b) const {
      return a->priority < b->priority;  // max-heap: lower priority at bottom
    }
  };

  // Helper to translate pending shaders for a pipeline and update root
  // signature. Used by CreationThread and
  // CreateQueuedPipelinesOnProcessorThread. If use_try_claim is true
  // (background threads), uses TryClaimTranslation to prevent multiple threads
  // translating the same shader. If handle_non_placeholder is true, also
  // translates desc.pixel_shader when pending shaders are null (for pipelines
  // loaded from cache).
  void EnsurePipelineShadersTranslated(
      Pipeline* pipeline, DxbcShaderTranslator& translator,
      StringBuffer& ucode_disasm_buffer, IDxbcConverter* dxbc_converter,
      IDxcUtils* dxc_utils, IDxcCompiler* dxc_compiler, bool use_try_claim,
      bool handle_non_placeholder);

  // All previously generated pipelines identified by hash and the description.
  std::unordered_multimap<uint64_t, Pipeline*,
                          xe::hash::IdentityHasher<uint64_t>>
      pipelines_;

  // Previously used pipeline. This matches our current state settings and
  // allows us to quickly(ish) reuse the pipeline if no registers have been
  // changed.
  Pipeline* current_pipeline_ = nullptr;

  // Currently open shader storage state.
  uint32_t shader_storage_title_id_ = 0;
  std::atomic<bool> shader_storage_file_flush_needed_{false};
  std::atomic<bool> pipeline_storage_file_flush_needed_{false};

  // Storage writer for shaders and pipelines (owns file handles and storage
  // index).
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;

  // WHERE THE CPU GOES ON THE CREATION SIDE.
  //
  // This target is CPU-bound (measured 75-80% CPU against 10-20% GPU) and every
  // one of these threads competes with the guest recompiler for the same seven
  // cores. Scheduling them well is not possible without knowing what they cost,
  // so the cost is counted: total CPU consumed by the creation threads, and the
  // wall time split between OUR translator and the DRIVER's compiler, which are
  // two entirely different problems with two entirely different fixes.
  std::atomic<uint64_t> creation_cpu_100ns_{0};
  std::atomic<uint64_t> translation_ticks_{0};
  std::atomic<uint64_t> driver_compile_ticks_{0};
  std::atomic<uint64_t> driver_compile_count_{0};
  // How long a pipeline sat in the queue before a thread picked it up - the
  // number that says whether the queue is starved of threads or of time.
  std::atomic<uint64_t> queue_wait_ticks_{0};
  std::atomic<uint64_t> queue_wait_count_{0};
  std::atomic<uint32_t> queue_depth_peak_{0};

#if XE_PLATFORM_WINRT
 public:
  // How many compilers may be working at this instant. The threads exist
  // either way; this is a permit count they take before picking work up, so
  // changing it is instant and costs no thread creation. Set by the governor
  // in the command processor, which is the only place that can see the two
  // things the decision depends on - the guest frame rate and how much of the
  // console is already busy.
  void SetCreationPermits(uint32_t permits);
  // Whether speculative (never-yet-demanded) pipelines may be built right now.
  void SetFrozenTierAllowed(bool allowed);
  uint32_t creation_permits() const {
    return creation_permits_.load(std::memory_order_relaxed);
  }
  uint32_t creation_queue_depth() const {
    return creation_queue_depth_hint_.load(std::memory_order_relaxed);
  }
  // Ceiling the governor may raise permits to.
  uint32_t max_creation_permits() const {
    return uint32_t(creation_threads_.size());
  }

 private:
  // Consulted with creation_request_lock_ held, from the creation threads only.
  bool CreationAdmitted(bool candidate_demanded);
  // Whether speculative work may be built right now. The governor clears it
  // when it has had to back off, so the frozen tier only runs on headroom the
  // guest is not using.
  std::atomic<bool> frozen_tier_allowed_{true};
  // Called once per guest frame by the command processor.
  void RefillCreationBudget();
  // THE BUDGET IS A DEBT, NOT AN ALLOWANCE.
  //
  // It used to be reset to zero every guest frame, which meant a single 700 ms
  // compile - the measured cost of one pipeline on this driver - overshot the
  // whole frame's allowance and then had the overshoot forgiven. "2 ms per
  // frame" therefore behaved as "one compile started per frame", and measured
  // WORSE than no budget at all (29.8 fps against 33.5). Carrying the debt is
  // what the cvar always claimed to do: at 2 ms per frame and 30 fps it is
  // 60 ms of compiler time per second, a 6% duty cycle, and a 700 ms item
  // simply parks the compilers until it is paid off.
  int64_t creation_budget_debt_ticks_ = 0;
  uint64_t creation_budget_exhausted_frames_ = 0;
  double ticks_per_millisecond_ =
      double(xe::Clock::QueryHostTickFrequency()) / 1000.0;
  static constexpr uint32_t kInitialCreationPermits = 2;
  std::atomic<uint32_t> creation_permits_{~uint32_t(0)};
  // Governor telemetry, for the periodic report.
  std::atomic<uint32_t> governor_permits_{0};
  std::atomic<uint32_t> governor_backoffs_{0};
  std::atomic<uint32_t> governor_expansions_{0};
#endif  // XE_PLATFORM_WINRT
  // Per-thread previous GetThreadTimes sample; only creation threads index it,
  // and their count is bounded by the burst logic well below this.
  static constexpr size_t kMaxTrackedCreationThreads = 16;
  uint64_t thread_last_cpu_100ns_[kMaxTrackedCreationThreads] = {};

  // Pipeline creation threads.
  void CreationThread(size_t thread_index);
  void CreateQueuedPipelinesOnProcessorThread();
  xe_mutex creation_request_lock_;
  std::condition_variable_any creation_request_cond_;
  // Priority queue contains pointers to map entries. Pipelines are never
  // evicted as games have a finite set that should all remain cached for
  // performance. Higher priority pipelines (those writing to visible RTs)
  // are compiled first.
  std::priority_queue<Pipeline*, std::vector<Pipeline*>,
                      PipelineCreationPriorityCompare>
      creation_queue_;
  // Number of threads that are currently creating a pipeline - incremented when
  // a pipeline is dequeued (the completion event can't be triggered before this
  // is zero). Protected with creation_request_lock_.
  size_t creation_threads_busy_ = 0;
  // Manual-reset event set when the last queued pipeline is created and there
  // are no more pipelines to create. This is triggered by the thread creating
  // the last pipeline.
  std::unique_ptr<xe::threading::Event> creation_completion_event_;
  // Whether setting the event on completion is queued. Protected with
  // creation_request_lock_, notify_one creation_request_cond_ when set.
  bool creation_completion_set_event_ = false;
  // Callback to invoke when all queued pipelines are created (for non-blocking
  // initialization). Protected with creation_request_lock_.
  std::function<void()> creation_completion_callback_;
  // Creation threads with this index or above need to be shut down as soon as
  // possible. Protected with creation_request_lock_, notify_all
  // creation_request_cond_ when set.
  size_t creation_threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<xe::threading::Thread>> creation_threads_;
  // Queue depth at which more creation threads are spun up, and how many to
  // run then - see EnsureCreationThreadsForQueueDepth. The burst count is
  // computed from the core count at initialization.
  static constexpr size_t kCreationQueueBurstDepth = 64;
  size_t creation_thread_burst_count_ = 0;
  // Threads to keep once a burst is over, and how long the queue has to stay
  // shallow before the extras are shut down (so a stuttering backlog doesn't
  // make them come and go constantly).
  size_t creation_thread_base_count_ = 0;
  uint32_t creation_threads_idle_submissions_ = 0;
  static constexpr uint32_t kCreationThreadIdleSubmissions = 240;
  // Queue depth as of the last submission, so per-draw code can tell whether a
  // backlog is being compiled without taking creation_request_lock_.
  std::atomic<uint32_t> creation_queue_depth_hint_{0};
  // Above this, interpolator mask widening waits - see GetSharedInterpolatorMask.
  static constexpr uint32_t kSharedInterpolatorDeferWidenQueueDepth = 16;

  // Accumulated interpolator masks per vertex shader - see
  // GetSharedInterpolatorMask. Widening the mask retranslates the shader and
  // rebuilds its pipelines, so the number of times that may happen for one
  // shader is capped; past it the mask jumps straight to everything the
  // vertex shader writes and stops moving.
  struct SharedInterpolatorMask {
    uint32_t mask;
    uint32_t widen_count;
  };
  static constexpr uint32_t kSharedInterpolatorMaxWidenings = 2;
  // Refuse to widen when this many pipelines would have to be rebuilt for it -
  // the mask is shared to save translations, and past this the rebuilds cost
  // more than the translations ever saved.
  static constexpr size_t kSharedInterpolatorMaxPipelinesToRebuild = 8;
  uint32_t shared_interpolator_widenings_deferred_ = 0;
  uint32_t shared_interpolator_widenings_refused_ = 0;
  std::unordered_map<uint64_t, SharedInterpolatorMask>
      shared_interpolator_masks_;
  // Answer for the previous draw - consecutive draws overwhelmingly share
  // shaders, and this runs per draw.
  uint64_t last_shared_interpolator_vs_hash_ = 0;
  uint32_t last_shared_interpolator_mask_ = 0;

  // Nonzero while TranslateShadersForStorage is running on the loader
  // thread(s) - the arbiter's release must not free binaries out from under an
  // in-progress storage translation.
  std::atomic<uint32_t> storage_translations_in_progress_{0};
};
inline bool PipelineCache::PipelineDescription::operator==(
    const PipelineDescription& other) const {
  constexpr size_t cmp_size = sizeof(PipelineDescription);
#if XE_ARCH_AMD64 == 1
  if constexpr (cmp_size == 64) {
    if (vertex_shader_hash != other.vertex_shader_hash ||
        vertex_shader_modification != other.vertex_shader_modification) {
      return false;
    }
    const __m128i* thiz = (const __m128i*)this;
    const __m128i* thoze = (const __m128i*)&other;
    __m128i cmp32 =
        _mm_cmpeq_epi8(_mm_loadu_si128(thiz + 1), _mm_loadu_si128(thoze + 1));

    cmp32 = _mm_and_si128(cmp32, _mm_cmpeq_epi8(_mm_loadu_si128(thiz + 2),
                                                _mm_loadu_si128(thoze + 2)));

    cmp32 = _mm_and_si128(cmp32, _mm_cmpeq_epi8(_mm_loadu_si128(thiz + 3),
                                                _mm_loadu_si128(thoze + 3)));

    return _mm_movemask_epi8(cmp32) == 0xFFFF;

  } else
#endif
  {
    return !memcmp(this, &other, cmp_size);
  }
}
}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_PIPELINE_CACHE_H_
