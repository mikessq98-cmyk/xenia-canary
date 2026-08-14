/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/graphics_system.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/processor.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

DEFINE_uint32(custom_internal_display_resolution_x, 0,
              "Custom width. See internal_display_resolution. Range 1-1920.",
              "Video");
DEFINE_uint32(custom_internal_display_resolution_y, 0,
              "Custom height. See internal_display_resolution. Range 1-1080.\n",
              "Video");

DEFINE_bool(
    store_shaders, true,
    "Store shaders persistently and load them when loading games to avoid "
    "runtime spikes and freezes when playing the game not for the first time.",
    "GPU");

DEFINE_bool(
    store_shaders_blocking_load, false,
    "When loading persistently stored shaders on a subsequent run, finish "
    "compiling all of them BEFORE the game starts loading, instead of "
    "compiling them in the background in parallel with the game. Makes the "
    "initial load longer (the emulator sits on a black screen while the "
    "pipelines build), but removes the in-game stutter and the GPU spike that "
    "async compilation causes at the start. Recommended on fixed hardware like "
    "Xbox where the stored cache exactly matches what the game needs.",
    "GPU");

DECLARE_int32(memory_statistics_interval_seconds);

namespace xe {
namespace gpu {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if XE_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // XE_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : frame_limiter_worker_running_(false) {
  register_file_ = reinterpret_cast<RegisterFile*>(memory::AllocFixed(
      nullptr, sizeof(RegisterFile), memory::AllocationType::kReserveCommit,
      memory::PageAccess::kReadWrite));
}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::Setup(cpu::Processor* processor,
                               kernel::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context,
                               bool with_presentation) {
  memory_ = processor->memory();
  processor_ = processor;
  kernel_state_ = kernel_state;
  app_context_ = app_context;

  scaled_aspect_x_ = 16;
  scaled_aspect_y_ = 9;

  if (with_presentation && provider_) {
    // Safe if either the UI thread call or the presenter creation fails.
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() {
        presenter_ = provider_->CreatePresenter(
            [this](bool is_responsible, bool statically_from_ui_thread) {
              OnHostGpuLossFromAnyThread(is_responsible);
            });
      });
    } else {
      // May be needed for offscreen use, such as capturing the guest output
      // image.
      presenter_ = provider_->CreatePresenter(
          [this](bool is_responsible, bool statically_from_ui_thread) {
            OnHostGpuLossFromAnyThread(is_responsible);
          });
    }
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    XELOGE("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Let the processor know we want register access callbacks.
  memory_->AddVirtualMappedRange(
      0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<cpu::MMIOReadCallback>(ReadRegisterThunk),
      reinterpret_cast<cpu::MMIOWriteCallback>(WriteRegisterThunk));

  // Frame limiter thread.
  frame_limiter_worker_running_ = true;
  frame_limiter_worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {
            uint64_t normalized_framerate_limit =
                std::max<uint64_t>(0, cvars::framerate_limit);

            // If VSYNC is enabled, but frames are not limited,
            // lock framerate at default value of 60
            if (normalized_framerate_limit == 0 && cvars::vsync) {
              normalized_framerate_limit = 60;
            }

            const double vsync_duration_d =
                cvars::vsync
                    ? std::max<double>(5.0,
                                       1000.0 / static_cast<double>(
                                                    normalized_framerate_limit))
                    : 1.0;
            uint64_t last_frame_time = Clock::QueryGuestTickCount();
    // Sleep for 90% of the vblank duration on Windows, spin for 10%
    // Linux uses full sleep duration due to scheduler quantum issues
#if XE_PLATFORM_WIN32
            constexpr double duration_scalar = 0.90;
#endif
#if XE_PLATFORM_LINUX
            constexpr double duration_scalar = 1.0;
#endif

            uint64_t last_memory_stats_time = Clock::QueryHostTickCount();
            while (frame_limiter_worker_running_) {
              // If there is no title running then there is no need for guest
              // frame limiter thread.
              if (!kernel_state_->is_title_open()) {
                xe::threading::Sleep(std::chrono::milliseconds(100));
                continue;
              }

              // Periodic memory-usage telemetry (see
              // memory_statistics_interval_seconds). This thread ticks ~every
              // vblank, giving fine-enough granularity for an N-second log.
              if (cvars::memory_statistics_interval_seconds > 0 && memory_) {
                uint64_t now = Clock::QueryHostTickCount();
                if ((now - last_memory_stats_time) >=
                    uint64_t(cvars::memory_statistics_interval_seconds) *
                        Clock::QueryHostTickFrequency()) {
                  last_memory_stats_time = now;
                  memory_->LogMemoryStatistics();
                  command_processor_->LogHostMemoryStatistics();
                  if (processor_ && processor_->backend() &&
                      processor_->backend()->code_cache()) {
                    const cpu::backend::CodeCache* code_cache =
                        processor_->backend()->code_cache();
                    XELOGI(
                        "[MEM] JIT: code cache {} MB committed (of {} MB "
                        "reserved), indirection table {} MB, {} functions",
                        code_cache->committed_bytes() >> 20,
                        code_cache->total_size() >> 20,
                        code_cache->indirection_committed_bytes() >> 20,
                        code_cache->function_count());
                  }
                  if (processor_) {
                    LogGuestCpuStatistics();
                  }
                }
              }

              register_file()->values[XE_GPU_REG_D1MODE_V_COUNTER] +=
                  GetResolution().second;

#if XE_PLATFORM_WIN32
              if (cvars::vsync) {
                const uint64_t current_time = Clock::QueryGuestTickCount();
                const uint64_t tick_freq = Clock::guest_tick_frequency();
                const uint64_t time_delta = current_time - last_frame_time;
                const double elapsed_d =
                    static_cast<double>(time_delta) /
                    (static_cast<double>(tick_freq) / 1000.0);
                if (elapsed_d >= vsync_duration_d) {
                  last_frame_time = current_time;

                  MarkVblank();
                  const uint64_t estimated_nanoseconds = static_cast<uint64_t>(
                      (vsync_duration_d * 1000000.0) *
                      duration_scalar);  // 1000 microseconds = 1 ms

                  threading::NanoSleep(estimated_nanoseconds);
                }
#if XE_PLATFORM_WINRT
                else {
                  // Xbox: without this the loop busy-spins for the last ~10%
                  // of every frame (the sleep above only covers 90% of the
                  // vblank period), incrementing V_COUNTER thousands of times
                  // per frame and burning a core the console doesn't have to
                  // spare. A short sleep keeps a couple of wakeups per frame,
                  // which is plenty of V_COUNTER granularity for guest code
                  // polling it.
                  threading::NanoSleep(500000);  // 0.5 ms
                }
#endif  // XE_PLATFORM_WINRT
              }

              if (!cvars::vsync) {
                MarkVblank();
                if (normalized_framerate_limit > 0) {
                  // framerate_limit is over 0, vsync disabled
                  //  - No VSYNC + limited frames defined by user
                  uint64_t framerate_limited_sleep_time =
                      1000000000 / normalized_framerate_limit;
                  xe::threading::NanoSleep(framerate_limited_sleep_time);
                } else {
                  // framerate_limit is 0, vsync disabled
                  //  - No VSYNC + unlimited frames
                  xe::threading::Sleep(std::chrono::milliseconds(1));
                }
              }
#endif
#if XE_PLATFORM_LINUX
              // Linux: Use simplified timing logic to avoid oversleeping
              MarkVblank();

              if (cvars::vsync || normalized_framerate_limit > 0) {
                uint64_t sleep_duration_ns =
                    static_cast<uint64_t>(vsync_duration_d * 1000000.0);
                if (!cvars::vsync && normalized_framerate_limit > 0) {
                  sleep_duration_ns = 1000000000 / normalized_framerate_limit;
                }
                threading::NanoSleep(sleep_duration_ns);
              } else {
                xe::threading::Sleep(std::chrono::milliseconds(1));
              }
#endif
            }
            return 0;
          },
          kernel_state->GetIdleProcess()));
  // As we run vblank interrupts the debugger must be able to suspend us.
  frame_limiter_worker_thread_->set_can_debugger_suspend(true);
  frame_limiter_worker_thread_->set_name("GPU Frame limiter");
  frame_limiter_worker_thread_->Create();
  frame_limiter_worker_thread_->thread()->set_priority(
      threading::ThreadPriority::kLowest);
  if (cvars::trace_gpu_stream) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (frame_limiter_worker_thread_) {
    frame_limiter_worker_running_ = false;
    frame_limiter_worker_thread_->Wait(0, 0, 0, nullptr);
    frame_limiter_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread(
    [[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }

  // Let the backend preserve crash-diagnosis state (e.g. the toxic-shader
  // solver's journal on Xbox UWP) before the process is torn down.
  if (command_processor_) {
    command_processor_->OnHostGpuLossFromAnyThread();
  }

  config::SaveConfig();

  xe::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context,
                                           GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                        uint32_t addr, uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x1951:  // interrupt status
      return 1;   // vblank
    case 0x1961:  // AVIVO_D1MODE_VIEWPORT_SIZE
                  // Screen res - 1280x720
                  // maximum [width(0x0FFF), height(0x0FFF)]
      return 0x050002D0;
    default:
      if (!register_file()->IsValidRegister(r)) {
        XELOGE("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file()->values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      XELOGW("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  this->register_file()->values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr,
                                                uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback,
                                          uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  XELOGGPU("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  kernel_state()->EmulateCPInterruptDPC(interrupt_callback_,
                                        interrupt_callback_data_, source, cpu);
}

void GraphicsSystem::MarkVblank() {
  SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  command_processor_->increment_counter();

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread(
      [&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  if (!cvars::store_shaders) {
    if (completion_callback) {
      completion_callback();
    }
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(
          cache_root, title_id, true, std::move(completion_callback));
    } else {
      xe::threading::Fence fence;
      command_processor_->CallInThread(
          [this, cache_root, title_id, &fence,
           completion_callback = std::move(completion_callback)]() mutable {
            command_processor_->InitializeShaderStorage(
                cache_root, title_id, true, std::move(completion_callback));
            fence.Signal();
          });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread(
        [this, cache_root, title_id,
         completion_callback = std::move(completion_callback)]() mutable {
          command_processor_->InitializeShaderStorage(
              cache_root, title_id, false, std::move(completion_callback));
        });
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(cvars::trace_gpu_prefix);
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(cvars::trace_gpu_prefix);
}

void GraphicsSystem::LogGuestCpuStatistics() {
  // Which guest threads are actually burning the console. The process-wide
  // line already says how many cores are busy and how few of them are the
  // pipeline compilers; this says which of the thirty-odd guest threads the
  // rest of it is.
  double ticks_to_ms = 1000.0 / double(Clock::QueryHostTickFrequency());
  double translation_ms =
      double(processor_->translation_ticks()) * ticks_to_ms;
  XELOGI("[MEM] guest cpu: {} functions translated, {:.0f} ms on the guest "
         "threads that first called them",
         processor_->translation_count(), translation_ms);

  if (!kernel_state_) {
    return;
  }
  struct ThreadCost {
    std::string name;
    double cores;
  };
  std::vector<ThreadCost> costs;
  double total_cores = 0.0;
  uint64_t now = Clock::QueryHostTickCount();
  double elapsed_seconds =
      last_guest_cpu_sample_ticks_
          ? double(now - last_guest_cpu_sample_ticks_) /
                double(Clock::QueryHostTickFrequency())
          : 0.0;
  last_guest_cpu_sample_ticks_ = now;
  for (const auto& thread : kernel_state_->object_table()
                                ->GetObjectsByType<kernel::XThread>()) {
    if (!thread || !thread->is_guest_thread() || !thread->thread()) {
      continue;
    }
    HANDLE handle = HANDLE(thread->thread()->native_handle());
    FILETIME creation_time, exit_time, kernel_time, user_time;
    if (!handle || !GetThreadTimes(handle, &creation_time, &exit_time,
                                   &kernel_time, &user_time)) {
      continue;
    }
    uint64_t cpu_100ns =
        (uint64_t(kernel_time.dwHighDateTime) << 32 |
         kernel_time.dwLowDateTime) +
        (uint64_t(user_time.dwHighDateTime) << 32 | user_time.dwLowDateTime);
    uint32_t handle_value = thread->handle();
    uint64_t& previous = guest_thread_cpu_100ns_[handle_value];
    uint64_t delta = cpu_100ns >= previous ? cpu_100ns - previous : 0;
    previous = cpu_100ns;
    if (elapsed_seconds <= 0.0) {
      continue;
    }
    double cores = double(delta) * 1.0e-7 / elapsed_seconds;
    total_cores += cores;
    if (cores >= 0.02) {
      costs.push_back({thread->thread_name(), cores});
    }
  }
  if (costs.empty()) {
    return;
  }
  std::sort(costs.begin(), costs.end(),
            [](const ThreadCost& a, const ThreadCost& b) {
              return a.cores > b.cores;
            });
  std::string busiest;
  for (size_t i = 0; i < costs.size() && i < 6; ++i) {
    if (!busiest.empty()) {
      busiest += ", ";
    }
    busiest += fmt::format("{} {:.2f}", costs[i].name, costs[i].cores);
  }
  XELOGI("[MEM] guest threads: {:.2f} cores across {} of them | busiest: {}",
         total_cores, costs.size(), busiest);
}

void GraphicsSystem::EndTracing() { command_processor_->EndTracing(); }

void GraphicsSystem::Pause() {
  paused_ = true;

  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;

  command_processor_->Resume();
}

bool GraphicsSystem::Save(ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);

  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();

  return command_processor_->Restore(stream);
}

std::pair<uint32_t, uint32_t> GraphicsSystem::GetResolution() const {
  if (!kernel_state_) {
    return {1280, 720};
  }

  if (cvars::custom_internal_display_resolution_x != 0 &&
      cvars::custom_internal_display_resolution_y != 0) {
    return {cvars::custom_internal_display_resolution_x,
            cvars::custom_internal_display_resolution_y};
  }

  const auto resolution =
      kernel::Resolution(kernel_state()->xconfig()->ReadSetting<uint32_t>(
          kernel::XCONFIG_USER_CATEGORY,
          kernel::XCONFIG_USER_AV_COMPOSITE_SCREENSZ));

  return {resolution.width_, resolution.height_};
}

}  // namespace gpu
}  // namespace xe
