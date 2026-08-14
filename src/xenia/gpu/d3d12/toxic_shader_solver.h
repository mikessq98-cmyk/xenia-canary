/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_TOXIC_SHADER_SOLVER_H_
#define XENIA_GPU_D3D12_TOXIC_SHADER_SOLVER_H_

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <utility>

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

namespace xe {
namespace gpu {
namespace d3d12 {

// Finds the shader pairs the console's GPU driver cannot survive, and skips
// them permanently - without the user ever editing a list by hand.
//
// The Xbox UWP driver fails on shaders in three different ways, and each needs
// a different kind of evidence:
//
//  1. The shader compiler (newbe_xs.dll) takes an access violation while
//     compiling. Contained by a structured-exception guard around the creation
//     call, so the process survives; the pair is quarantined on the spot.
//  2. The compiler HANGS and never returns. Nothing can be caught, so each
//     creation is bracketed by an on-disk journal: whatever is still in the
//     journal at the next launch was in flight when the process died. A
//     recovery run serializes creation, so exactly one pair is left behind.
//  3. The pipeline builds fine and the GPU hangs EXECUTING it. The creation
//     side cannot see this at all, and the list of recently bound pipelines
//     cannot identify it either (the GPU runs behind, so its newest entry is
//     just the newest). Handled by drawing each never-yet-proven pair alone and
//     waiting on it, which costs one synchronization per distinct pair per
//     game and leaves the culprit alone in the execution journal.
//
// Everything is per title AND per resolution scale: shaders are translated
// differently when scaling is active, so a pair that kills the compiler at 2x2
// is usually fine at 1x1. All the files are plain text and can be pruned by
// hand if a suspect turns out innocent.
//
// This lived inside PipelineCache until it was two hundred lines of unrelated
// state in the middle of pipeline creation. The pipeline cache now only says
// what happened; every decision about what that means is here.
class ToxicShaderSolver {
 public:
  ToxicShaderSolver() = default;
  ToxicShaderSolver(const ToxicShaderSolver&) = delete;
  ToxicShaderSolver& operator=(const ToxicShaderSolver&) = delete;
  ~ToxicShaderSolver();

  // `shader_storage_root` is the directory the pipeline cache keeps its shader
  // storage in; the solver's files live next to it.
  void Initialize(const std::filesystem::path& shader_storage_root,
                  uint32_t title_id);
  void Shutdown(bool clean_exit);

  bool enabled() const { return enabled_; }
  const std::filesystem::path& toxic_path() const { return toxic_path_; }

  // Confirmed-toxic lookup: the list loaded at startup, plus anything this run
  // has quarantined since. A quarantine takes effect IMMEDIATELY - writing it
  // to the file and waiting for the next launch to honour it means the run that
  // found the bad pair keeps drawing with it.
  bool IsShaderToxic(uint64_t vertex_shader_hash,
                     uint64_t pixel_shader_hash) const;

  // What the caller should do about a pipeline that is about to be created.
  enum class PreflightVerdict {
    // Nothing known against it - hand it to the driver.
    kProceed,
    // The process is short enough of memory that the driver's compiler is
    // expected to fail on its own allocations. Submitting it anyway buys an
    // access violation inside the compiler, which is contained but not free -
    // and if it lands while the compiler holds a lock, it takes the run with
    // it. The caller should leave the pipeline unbuilt and try again later.
    kDeferOutOfMemory,
  };
  // Asked immediately before the creation call. This is the part that does not
  // need a death to learn from: the failure it prevents is the one this session
  // ends with every time the host runs out of memory at a resolution scale, and
  // the condition is measurable in advance rather than only in hindsight.
  PreflightVerdict Preflight() const;

  // RAII bracket around one pipeline creation: takes the safe-mode serialize
  // lock if the run is in safe mode, writes the pair to the crash journal
  // before the driver is called, and removes it on any normal return. If the
  // driver hard-crashes the process the destructor never runs, so the pair is
  // left behind as the suspect. Journaling only happens while recovering from a
  // crash, so a normal run does no per-creation file I/O at all.
  class CreationProbe {
   public:
    CreationProbe(ToxicShaderSolver& solver, uint64_t vertex_shader_hash,
                  uint64_t pixel_shader_hash);
    CreationProbe(const CreationProbe&) = delete;
    CreationProbe& operator=(const CreationProbe&) = delete;
    ~CreationProbe();

   private:
    ToxicShaderSolver& solver_;
    uint64_t vertex_shader_hash_;
    uint64_t pixel_shader_hash_;
    std::unique_lock<std::mutex> serialize_lock_;
    bool active_ = false;
  };

  // What a contained compiler crash turned out to mean.
  enum class CrashVerdict {
    // The pair is quarantined and will be skipped from now on.
    kQuarantined,
    // The process is out of memory and the compiler crashed on its own failed
    // allocations - on a shader that is very likely perfectly valid. The draws
    // are skipped for this run, but nothing is written to the skip list.
    kOutOfMemoryVictim,
  };
  CrashVerdict ReportCompilerCrash(uint64_t vertex_shader_hash,
                                   uint64_t pixel_shader_hash);
  // A creation call returned E_OUTOFMEMORY. Everything quarantined earlier in
  // this run was probably an out-of-memory victim too, so the skip list is
  // rewound to what was known at startup.
  void ReportOutOfMemory();
  // The device was removed while this pair was being created. In serialized
  // safe mode exactly one pipeline was in flight, so the pair is the confirmed
  // culprit.
  void ReportDeviceRemovedDuringCreation(uint64_t vertex_shader_hash,
                                         uint64_t pixel_shader_hash);

  // Device loss reported from anywhere (present, submission, or a creation call
  // that hung and never returned). Keeps the crash journal on shutdown even
  // though the process then exits gracefully.
  void OnDeviceLost();

  // Records that the device died while EXECUTING draws rather than while
  // creating a pipeline. The next launch then runs draws one at a time,
  // journalling each, so the hang leaves exactly one suspect behind.
  void MarkExecutionHang();
  // While true, every draw is submitted alone, waited on, and written to the
  // execution journal first. Costs a slideshow for one launch and finds the
  // hanging draw with no guesswork; cleared automatically once it has.
  bool execution_safe_mode() const { return execution_safe_mode_; }
  // Called just before a draw is submitted in execution safe mode.
  void ExecutionJournalDraw(uint64_t vertex_shader_hash,
                            uint64_t pixel_shader_hash);
  // Whether this pair has never been seen to survive execution. The FIRST draw
  // using it is submitted alone and waited on, so if it hangs the GPU the
  // journal holds it alone and the next launch quarantines it. Pairs that
  // survive are remembered on disk and never checked again - so the cost is one
  // synchronization per distinct pair per game, not per draw.
  bool NeedsExecutionVerification(uint64_t vertex_shader_hash,
                                  uint64_t pixel_shader_hash,
                                  uint64_t state_key);
  // The pair drew without hanging the GPU. Remembered across launches.
  void MarkExecutionVerified(uint64_t vertex_shader_hash,
                             uint64_t pixel_shader_hash, uint64_t state_key);
  // Quarantines a pipeline identified as an execution-side hang suspect (the
  // most recently bound pipeline when the device was removed with
  // DXGI_ERROR_DEVICE_HUNG). The suspect may be innocent - the hang can lag the
  // guilty draw - and the .toxic file can be pruned.
  void QuarantineExecutionSuspect(uint64_t vertex_shader_hash,
                                  uint64_t pixel_shader_hash);

  // THE draw did not come back. This is the one execution-side verdict that is
  // not a guess: the pair was submitted on its own and waited on, so nothing
  // else was in flight and the attribution is exact. Quarantined here and now
  // rather than left in the journal for the next launch to interpret - the run
  // that found it is the run that knows, and a file that has to survive a
  // device loss and a shutdown to carry the answer is one more thing that can
  // go wrong.
  void QuarantineIsolatedExecutionHang(uint64_t vertex_shader_hash,
                                       uint64_t pixel_shader_hash);
  // Whether the culprit is already named, so the device-loss path does not also
  // ask the next launch to go looking for it.
  bool execution_hang_identified() const { return execution_hang_identified_; }

  // Pairs proven to survive execution: loaded at startup, and added this run.
  // Reported by the periodic memory log - a successful verification is silent
  // by design, and without these two numbers there is no way to tell "nothing
  // needed checking" from "the list is not being kept".
  size_t verified_at_startup() const { return verified_at_startup_; }
  size_t verified_this_run() const { return verified_this_run_; }

  // Appends a pair to the persistent skip list, promoting the vertex shader as
  // a whole once it has hung with enough different pixel shaders.
  void AppendToxic(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash);

 private:
  void JournalBegin(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash);
  void JournalEnd(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash);
  // Rewrites the whole journal from inflight_. Caller holds journal_mutex_.
  void RewriteJournalLocked();
  // On the first observed E_OUTOFMEMORY: rewrites the skip list with only the
  // entries known at startup.
  void RetractThisRunToxic();
  // Free host commit, or UINT64_MAX when it cannot be queried.
  static uint64_t QueryFreeHostBytes();

  bool enabled_ = false;
  // Whether to write the per-creation crash journal this session. To avoid the
  // journal's file I/O (under a lock, on every pipeline creation) becoming
  // thread contention during normal prewarming, journaling is OFF unless the
  // previous run did NOT exit cleanly (a leftover ".running" marker) - i.e. we
  // only pay the cost while actually hunting a driver crash. Known-toxic
  // skipping (the cheap in-memory lookup) is always active.
  bool journaling_ = false;
  // All pipeline creation serialized to one-in-flight to pinpoint the culprit.
  bool safe_mode_ = false;
  // Creations performed in safe mode, and the number after which safe mode
  // gives up on reproducing the previous run's crash instead of starving the
  // game of pipelines for the whole session.
  std::atomic<uint32_t> safe_mode_creations_{0};
  static constexpr uint32_t kSafeModeMaxCreations = 192;

  std::filesystem::path toxic_path_;
  std::filesystem::path journal_path_;
  // Presence at startup means the last run crashed (deleted on clean exit).
  std::filesystem::path running_path_;

  // As the pixel shader of a toxic entry: matches every pixel shader, i.e. the
  // vertex shader itself is quarantined. A guest shader can't hash to this, and
  // the manual d3d12_skip_shaders syntax has the same idea as "[hash,sol]".
  static constexpr uint64_t kToxicAnyPixelShader = UINT64_MAX;
  // Distinct pixel shaders one vertex shader must hang with before it is blamed
  // as a whole. Two is enough to tell "this pair is broken" from "this vertex
  // shader is broken", and waiting for more costs a crash each.
  static constexpr size_t kToxicPairsPerVertexShader = 2;
  // Loaded at startup and not written to afterwards, so reads need no lock.
  std::set<std::pair<uint64_t, uint64_t>> toxic_shaders_;

  // Quarantined DURING this run. Kept separate from the startup set, and as a
  // flat array rather than a container, because it is read on the draw path
  // from the command processor thread while a creation thread may be appending
  // to it: a fixed array plus a release/acquire count is a correct lock-free
  // read, and a std::set would need a lock on every draw to be one. It only
  // ever holds a handful of entries - a session that finds dozens of pairs the
  // driver cannot survive has a bigger problem than this list.
  static constexpr size_t kMaxRuntimeQuarantine = 64;
  std::pair<uint64_t, uint64_t> runtime_quarantine_[kMaxRuntimeQuarantine] = {};
  std::atomic<uint32_t> runtime_quarantine_count_{0};
  void QuarantineForThisRun(uint64_t vertex_shader_hash,
                            uint64_t pixel_shader_hash);

  std::mutex journal_mutex_;
  std::set<std::pair<uint64_t, uint64_t>> inflight_;
  std::FILE* journal_file_ = nullptr;
  // Set once the D3D12 device is lost during creation. Tells the in-flight
  // probes to leave their entries in the journal (so they survive as suspects)
  // even on a graceful removal where the process doesn't hard-crash.
  std::atomic<bool> device_lost_{false};
  // Set once any pipeline creation fails with E_OUTOFMEMORY: from that point
  // driver shader compiler crashes are treated as out-of-memory victims and are
  // NOT quarantined as toxic pairs.
  std::atomic<bool> oom_seen_{false};
  // Held while a pipeline is created in safe mode (only one at a time).
  std::mutex serialize_mutex_;
  // Commit headroom below which the driver's compiler is expected to fail on
  // its own allocations. Measured: a GTA IV session at 2x2 interleaved
  // E_OUTOFMEMORY failures with compiler access violations on pairs that
  // compile fine at 1x1, and a Dark Souls II session at 3x3 ended with exactly
  // that crash at 727 MB free.
  static constexpr uint64_t kCompilerOutOfMemoryBytes = UINT64_C(768) << 20;
  // Preflight is asked once per pipeline creation, and a prewarm creates every
  // pipeline a title has. The host reading is shared for this long instead.
  static constexpr uint64_t kPreflightSampleValidMs = 50;
  mutable std::atomic<uint64_t> preflight_free_bytes_{0};
  mutable std::atomic<uint64_t> preflight_sample_time_ms_{0};

  // Execution-side solver. A pipeline that CREATES fine can still hang the GPU
  // when it runs (Black Ops does, reproducibly, on one object).
  bool execution_safe_mode_ = false;
  std::filesystem::path execution_journal_path_;
  // Written when the device is lost outside pipeline creation; its presence at
  // the next launch is what turns execution safe mode on.
  std::filesystem::path execution_hang_path_;
  std::FILE* execution_journal_file_ = nullptr;
  uint32_t execution_draws_ = 0;
  // Pairs already observed to execute without hanging the GPU, kept across
  // launches so a game is only ever checked once per pair. Command processor
  // thread only.
  // Keys, not pairs - see MakeVerificationKey.
  static uint64_t MakeVerificationKey(uint64_t vertex_shader_hash,
                                      uint64_t pixel_shader_hash,
                                      uint64_t state_key);
  std::set<uint64_t> verified_;
  std::filesystem::path verified_path_;
  std::FILE* verified_file_ = nullptr;
  size_t verified_at_startup_ = 0;
  size_t verified_this_run_ = 0;
  // Set once an isolated draw has named itself - see
  // QuarantineIsolatedExecutionHang.
  bool execution_hang_identified_ = false;
  // Give up reproducing after this many draws rather than leaving the game a
  // slideshow forever when the hang doesn't come back.
  static constexpr uint32_t kExecutionSafeModeMaxDraws = 300000;
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XE_PLATFORM_WINRT

#endif  // XENIA_GPU_D3D12_TOXIC_SHADER_SOLVER_H_
