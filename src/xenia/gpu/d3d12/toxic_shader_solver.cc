/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/toxic_shader_solver.h"

#if XE_PLATFORM_WINRT

#include <fstream>
#include <io.h>  // _chsize_s, _fileno - for the crash-journal truncation.
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

DECLARE_bool(d3d12_verify_new_draws);
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);

namespace xe {
namespace gpu {
namespace d3d12 {

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

ToxicShaderSolver::~ToxicShaderSolver() {
  // Never a clean exit from here: if the destructor is the first thing to close
  // the solver down, the run did not shut down through the normal path.
  if (enabled_) {
    Shutdown(/*clean_exit=*/false);
  }
}

uint64_t ToxicShaderSolver::QueryFreeHostBytes() {
  MEMORYSTATUSEX status = {sizeof(status)};
  if (!GlobalMemoryStatusEx(&status)) {
    return UINT64_MAX;
  }
  // Commit, not physical: the compiler fails its allocations when the commit
  // charge is exhausted while free physical RAM still looks plentiful.
  return status.ullAvailPageFile;
}

void ToxicShaderSolver::Initialize(
    const std::filesystem::path& shader_storage_root, uint32_t title_id) {
  // Re-entrant safe: close any journal handle left open by a prior game.
  {
    std::lock_guard<std::mutex> lock(journal_mutex_);
    if (journal_file_) {
      std::fclose(journal_file_);
      journal_file_ = nullptr;
    }
    inflight_.clear();
  }
  enabled_ = false;
  journaling_ = false;
  safe_mode_ = false;
  device_lost_.store(false, std::memory_order_release);
  oom_seen_.store(false, std::memory_order_release);
  toxic_shaders_.clear();

  std::error_code ec;
  // The solver state is per resolution scale: shaders are TRANSLATED
  // differently when scaling is active, so a pair whose translation crashes
  // the driver's compiler at 2x2 is usually perfectly fine at 1x1 (and vice
  // versa) - quarantines must not leak between scales.
  std::string scale_suffix;
  if (cvars::draw_resolution_scale_x > 1 || cvars::draw_resolution_scale_y > 1) {
    scale_suffix = fmt::format(".{}x{}", cvars::draw_resolution_scale_x,
                               cvars::draw_resolution_scale_y);
  }

  const std::filesystem::path& root = shader_storage_root;
  toxic_path_ =
      root / fmt::format("{:08X}.d3d12{}.toxic", title_id, scale_suffix);
  journal_path_ =
      root / fmt::format("{:08X}.d3d12{}.inflight", title_id, scale_suffix);
  running_path_ =
      root / fmt::format("{:08X}.d3d12{}.running", title_id, scale_suffix);
  execution_journal_path_ = root / fmt::format("{:08X}.d3d12{}.exec-inflight",
                                               title_id, scale_suffix);
  execution_hang_path_ =
      root / fmt::format("{:08X}.d3d12{}.exec-hang", title_id, scale_suffix);
  verified_path_ =
      root / fmt::format("{:08X}.d3d12{}.verified", title_id, scale_suffix);

  // Load the persistent per-game skip list (confirmed-toxic pairs).
  {
    std::ifstream toxic_file(toxic_path_);
    std::string line;
    while (std::getline(toxic_file, line)) {
      uint64_t vs = 0, ps = 0;
      if (ParseSolverLine(line, vs, ps)) {
        toxic_shaders_.emplace(vs, ps);
      }
    }
  }

  // A leftover ".running" marker means the previous run did NOT exit cleanly -
  // i.e. it crashed. We only pay for the per-creation crash journal while
  // recovering from such a crash; a normal run does no journaling at all (no
  // per-pipeline file I/O under a lock, so no contention on the creation
  // threads), only the cheap in-memory toxic lookup.
  const bool crashed_last_run = std::filesystem::exists(running_path_, ec);
  journaling_ = crashed_last_run;

  // Read any journal left behind by that crash (only meaningful if we crashed).
  std::vector<std::pair<uint64_t, uint64_t>> suspects;
  if (crashed_last_run) {
    std::ifstream journal_file(journal_path_);
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
        suspects.size(), xe::path_to_utf8(toxic_path_));
    for (const std::pair<uint64_t, uint64_t>& s : suspects) {
      if (toxic_shaders_.emplace(s.first, s.second).second) {
        AppendToxic(s.first, s.second);
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
  safe_mode_ = journaling_;

  // The same treatment for pipelines that create fine and hang the GPU when
  // they RUN. Whatever the last run left in the execution journal was in flight
  // when the device died, and in a serialized run that is exactly one draw.
  {
    std::vector<std::pair<uint64_t, uint64_t>> execution_suspects;
    std::ifstream execution_journal(execution_journal_path_);
    std::string line;
    while (std::getline(execution_journal, line)) {
      uint64_t vs = 0, ps = 0;
      if (ParseSolverLine(line, vs, ps)) {
        execution_suspects.emplace_back(vs, ps);
      }
    }
    if (!execution_suspects.empty()) {
      XELOGW(
          "Toxic-shader solver: {} draw(s) were in flight when the GPU hung "
          "last run - quarantining:",
          execution_suspects.size());
      for (const std::pair<uint64_t, uint64_t>& s : execution_suspects) {
        if (toxic_shaders_.emplace(s.first, s.second).second) {
          AppendToxic(s.first, s.second);
        }
        XELOGW("  quarantined VS {:016X}, PS {:016X} (execution hang)", s.first,
               s.second);
      }
      // Solved - the next run is a normal one.
      std::filesystem::remove(execution_journal_path_, ec);
      std::filesystem::remove(execution_hang_path_, ec);
    } else if (std::filesystem::exists(execution_hang_path_, ec)) {
      // The GPU hung last run but nothing was journalled. Serialize EVERY draw
      // this run, not just unverified pairs - the culprit is evidently
      // something already on the verified list, or something that only hangs
      // in a particular state.
      execution_safe_mode_ = true;
      XELOGW(
          "Toxic-shader solver: the GPU hung last run while drawing and the "
          "journal was empty - serializing every draw this run to catch it. "
          "Expect it to be very slow until it does.");
    }
  }

  // Load the pairs already proven to execute without hanging, and keep the
  // file open to append to it. Verification is what makes a hang survivable
  // on the FIRST run: an unproven pair is drawn on its own and waited on, so
  // if it takes the GPU down it is alone in the journal and the next launch
  // knows exactly what to quarantine. Proven pairs cost nothing ever again.
  if (cvars::d3d12_verify_new_draws || execution_safe_mode_) {
    if (cvars::d3d12_verify_new_draws) {
      std::ifstream verified_file(verified_path_);
      std::string line;
      while (std::getline(verified_file, line)) {
        uint64_t vs = 0, ps = 0;
        if (ParseSolverLine(line, vs, ps)) {
          verified_.emplace(vs, ps);
        }
      }
      verified_at_startup_ = verified_.size();
      verified_file_ = xe::filesystem::OpenFile(verified_path_, "ab");
      if (!verified_file_) {
        // Silent before: the run would verify every pair, throw the answers
        // away and re-verify the whole game on the next launch, with nothing
        // in the log to say why.
        XELOGW(
            "Toxic-shader solver: couldn't open {} for appending - pairs "
            "proven safe this run will NOT be remembered and every one of them "
            "will be checked again next launch",
            xe::path_to_utf8(verified_path_));
      }
    }
    execution_journal_file_ =
        xe::filesystem::OpenFile(execution_journal_path_, "wb+");
    if (!execution_journal_file_) {
      XELOGW(
          "Toxic-shader solver: couldn't open the execution journal {}; draws "
          "will not be checked this run",
          xe::path_to_utf8(execution_journal_path_));
      execution_safe_mode_ = false;
    } else if (cvars::d3d12_verify_new_draws) {
      XELOGI(
          "Toxic-shader solver: {} shader pair(s) already proven safe to "
          "execute; new ones will be checked one at a time",
          verified_.size());
    }
  }

  // Mark this run as in progress; deleted on clean shutdown, so its presence at
  // the next launch is what signals a crash.
  { std::ofstream running_marker(running_path_, std::ios::trunc); }

  // Only open the crash journal (which costs per-creation file I/O under a
  // lock) while actually recovering from a crash - a normal run journals
  // nothing.
  if (journaling_) {
    std::lock_guard<std::mutex> lock(journal_mutex_);
    journal_file_ = xe::filesystem::OpenFile(journal_path_, "wb+");
    if (!journal_file_) {
      XELOGW(
          "Toxic-shader solver: couldn't open crash journal {}; detection "
          "disabled this run (known-toxic skipping still active)",
          xe::path_to_utf8(journal_path_));
      journaling_ = false;
    }
  }

  enabled_ = true;
  XELOGI(
      "Toxic-shader solver active: {} known-toxic pair(s) will be skipped{}{}",
      toxic_shaders_.size(),
      journaling_ ? ", crash journaling ON (recovering)" : "",
      safe_mode_ ? ", serialized safe mode ON" : "");
}

void ToxicShaderSolver::Shutdown(bool clean_exit) {
  std::lock_guard<std::mutex> lock(journal_mutex_);
  if (journal_file_) {
    std::fclose(journal_file_);
    journal_file_ = nullptr;
  }
  // A GRACEFUL device removal (process survives, the user quits via the
  // "device lost" message box) must still count as a crash for the solver:
  // the culprit's journal entry - possibly from a creation call that HUNG the
  // driver's shader compiler and never returned - is the only evidence, and a
  // "clean" shutdown would destroy it. Observed with dxbc_switch=true: a
  // specific in-game shader hangs newbe_xs.dll, the device is eventually
  // removed, and without this the suspect was erased on exit.
  if (clean_exit && device_lost_.load(std::memory_order_acquire)) {
    XELOGW(
        "Toxic-shader solver: shutdown after device loss - keeping the crash "
        "journal so the culprit can be confirmed on the next launch");
    clean_exit = false;
  }
  if (execution_journal_file_) {
    std::fclose(execution_journal_file_);
    execution_journal_file_ = nullptr;
  }
  if (verified_file_) {
    std::fclose(verified_file_);
    verified_file_ = nullptr;
  }
  verified_.clear();
  if (clean_exit) {
    // No crash occurred - drop the in-progress marker and the journal so this
    // normal shutdown isn't treated as a crash next launch.
    std::error_code ec;
    if (!running_path_.empty()) {
      std::filesystem::remove(running_path_, ec);
    }
    if (!journal_path_.empty()) {
      std::filesystem::remove(journal_path_, ec);
    }
    // Same for the execution side: the game was quit normally, so whatever
    // draw was journalled last is simply the last one drawn, not a suspect.
    if (!execution_journal_path_.empty()) {
      std::filesystem::remove(execution_journal_path_, ec);
    }
    if (!execution_hang_path_.empty()) {
      std::filesystem::remove(execution_hang_path_, ec);
    }
  }
  inflight_.clear();
  enabled_ = false;
  journaling_ = false;
  safe_mode_ = false;
  execution_safe_mode_ = false;
  execution_draws_ = 0;
}

void ToxicShaderSolver::QuarantineForThisRun(uint64_t vertex_shader_hash,
                                             uint64_t pixel_shader_hash) {
  // Caller holds nothing; appends are serialized by journal_mutex_, and the
  // release store publishes the entry to the lock-free readers.
  std::lock_guard<std::mutex> lock(journal_mutex_);
  uint32_t count = runtime_quarantine_count_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < count; ++i) {
    if (runtime_quarantine_[i].first == vertex_shader_hash &&
        runtime_quarantine_[i].second == pixel_shader_hash) {
      return;
    }
  }
  if (count >= kMaxRuntimeQuarantine) {
    return;
  }
  runtime_quarantine_[count] = {vertex_shader_hash, pixel_shader_hash};
  runtime_quarantine_count_.store(count + 1, std::memory_order_release);
}

bool ToxicShaderSolver::IsShaderToxic(uint64_t vertex_shader_hash,
                                      uint64_t pixel_shader_hash) const {
  // Quarantined during this run - checked first because it is the cheap case
  // and almost always empty.
  uint32_t runtime_count =
      runtime_quarantine_count_.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < runtime_count; ++i) {
    const std::pair<uint64_t, uint64_t>& entry = runtime_quarantine_[i];
    if (entry.first != vertex_shader_hash) {
      continue;
    }
    if (entry.second == pixel_shader_hash ||
        entry.second == kToxicAnyPixelShader) {
      return true;
    }
  }
  if (toxic_shaders_.empty()) {
    return false;
  }
  // A whole-vertex-shader entry (any pixel shader). Written once the same
  // vertex shader has hung with kToxicPairsPerVertexShader different pixel
  // shaders - at that point it is the vertex shader that is broken, and
  // quarantining pairs one at a time would need a restart per pixel shader it
  // is ever paired with. Black Ops has one that appears with at least four.
  if (toxic_shaders_.find(std::make_pair(vertex_shader_hash,
                                         kToxicAnyPixelShader)) !=
      toxic_shaders_.end()) {
    return true;
  }
  return toxic_shaders_.find(std::make_pair(vertex_shader_hash,
                                            pixel_shader_hash)) !=
         toxic_shaders_.end();
}

ToxicShaderSolver::PreflightVerdict ToxicShaderSolver::Preflight() const {
  if (!enabled_) {
    return PreflightVerdict::kProceed;
  }
  // The one thing that can be known BEFORE handing a pipeline to the driver:
  // whether the compiler has room to compile it. Below this the compiler fails
  // its own allocations and takes an access violation, and while the guard
  // contains that, the process has still been through a driver crash - the
  // Dark Souls II session at 3x3 that ends in an unhandled access violation
  // inside XBSC_XS goes through several of these first. Deferring costs a draw
  // that would have been skipped anyway (the pipeline would not have been
  // built), and the memory core is meanwhile freeing memory, so the retry
  // usually succeeds.
  uint64_t free_bytes = QueryFreeHostBytes();
  if (free_bytes != UINT64_MAX && free_bytes < kCompilerOutOfMemoryBytes) {
    return PreflightVerdict::kDeferOutOfMemory;
  }
  return PreflightVerdict::kProceed;
}

ToxicShaderSolver::CreationProbe::CreationProbe(ToxicShaderSolver& solver,
                                                uint64_t vertex_shader_hash,
                                                uint64_t pixel_shader_hash)
    : solver_(solver),
      vertex_shader_hash_(vertex_shader_hash),
      pixel_shader_hash_(pixel_shader_hash) {
  // Only bracket the creation with the crash journal while recovering from a
  // crash; a normal run does zero per-creation file I/O.
  if (!solver_.enabled_ || !solver_.journaling_) {
    return;
  }
  if (solver_.safe_mode_) {
    // Safe mode serializes every creation to pin down which pipeline kills the
    // driver - which also means the game gets its pipelines one at a time and
    // renders almost nothing until they arrive. That's a price worth paying for
    // the first pipelines of a run, not for the whole session: if this many
    // have been created without a crash, whatever killed the previous run isn't
    // reproducing (a run killed by running out of memory leaves the same
    // "crashed" marker as a toxic shader), so stop isolating and let creation
    // go wide again.
    if (solver_.safe_mode_creations_.fetch_add(1, std::memory_order_relaxed) >=
        kSafeModeMaxCreations) {
      solver_.safe_mode_ = false;
      XELOGW(
          "Toxic-shader solver: {} pipelines created in safe mode without a "
          "crash - the previous run's death isn't reproducing, resuming "
          "parallel creation (journaling stays on)",
          kSafeModeMaxCreations);
    } else {
      serialize_lock_ = std::unique_lock<std::mutex>(solver_.serialize_mutex_);
    }
  }
  solver_.JournalBegin(vertex_shader_hash_, pixel_shader_hash_);
  active_ = true;
}

ToxicShaderSolver::CreationProbe::~CreationProbe() {
  // Leave the entry in the journal if the device was lost - it stays a suspect
  // for the next launch. Otherwise a normal return clears it.
  if (active_ && !solver_.device_lost_.load(std::memory_order_acquire)) {
    solver_.JournalEnd(vertex_shader_hash_, pixel_shader_hash_);
  }
}

ToxicShaderSolver::CrashVerdict ToxicShaderSolver::ReportCompilerCrash(
    uint64_t vertex_shader_hash, uint64_t pixel_shader_hash) {
  // With the process out of memory, the driver's shader compiler crashes on
  // its own failed allocations - on perfectly valid shaders (confirmed in
  // GTA IV at 2x2: E_OUTOFMEMORY failures interleaved with these AVs, and the
  // same pairs compile fine at 1x1). The crash may even PRECEDE the first
  // E_OUTOFMEMORY pipeline failure (the compiler allocates a lot), so also
  // check the actual memory headroom right now - if the title is nearly out of
  // its budget, this is an OOM victim, not a toxic shader.
  if (!oom_seen_.load(std::memory_order_acquire)) {
    uint64_t free_bytes = QueryFreeHostBytes();
    if (free_bytes != UINT64_MAX && free_bytes < kCompilerOutOfMemoryBytes) {
      if (!oom_seen_.exchange(true, std::memory_order_acq_rel)) {
        XELOGW(
            "Toxic-shader solver: the driver's shader compiler crashed with "
            "only {} MB of commit left - treating this and further crashes as "
            "out-of-memory victims, not toxic shaders (lower the memory usage "
            "- e.g. the resolution scale - instead)",
            free_bytes >> 20);
        std::error_code ec;
        std::filesystem::remove(running_path_, ec);
        RetractThisRunToxic();
      }
    }
  }
  if (oom_seen_.load(std::memory_order_acquire)) {
    return CrashVerdict::kOutOfMemoryVictim;
  }
  if (enabled_) {
    AppendToxic(vertex_shader_hash, pixel_shader_hash);
  }
  return CrashVerdict::kQuarantined;
}

void ToxicShaderSolver::ReportOutOfMemory() {
  // The driver's compiler may CRASH (access violation on its own failed
  // allocations) on perfectly valid shaders once the process is out of memory.
  // Those are OOM victims, not toxic pipelines: stop quarantining, and drop the
  // crash marker so a subsequent out-of-memory process death doesn't promote
  // the in-flight pairs to the per-game skip list on the next launch.
  if (!enabled_ || oom_seen_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  XELOGW(
      "Toxic-shader solver: pipeline creation failed with E_OUTOFMEMORY - the "
      "process is out of memory; driver shader compiler crashes from now on "
      "will NOT be treated as toxic shaders (lower the memory usage - e.g. the "
      "resolution scale or the post-processing output resolution - instead)");
  std::error_code ec;
  std::filesystem::remove(running_path_, ec);
  // Pairs quarantined earlier in this run were likely OOM victims too.
  RetractThisRunToxic();
}

void ToxicShaderSolver::ReportDeviceRemovedDuringCreation(
    uint64_t vertex_shader_hash, uint64_t pixel_shader_hash) {
  if (!enabled_) {
    return;
  }
  // Whatever is in the journal stays there: the entries remain suspects for the
  // next launch even though the process survived a *graceful* removal - a hard
  // crash would have left them anyway.
  device_lost_.store(true, std::memory_order_release);
  if (!safe_mode_) {
    return;
  }
  // In serialized safe mode exactly one pipeline was in flight, so this pair is
  // the confirmed culprit.
  AppendToxic(vertex_shader_hash, pixel_shader_hash);
  XELOGW(
      "Toxic-shader solver: confirmed VS {:016X}, PS {:016X} as toxic (caused "
      "device removal while isolated in safe mode); it will be skipped from "
      "the next launch",
      vertex_shader_hash, pixel_shader_hash);
}

void ToxicShaderSolver::OnDeviceLost() {
  if (!device_lost_.exchange(true, std::memory_order_acq_rel)) {
    XELOGW(
        "Toxic-shader solver: device loss reported - the crash journal will be "
        "kept on shutdown");
  }
}

void ToxicShaderSolver::QuarantineIsolatedExecutionHang(
    uint64_t vertex_shader_hash, uint64_t pixel_shader_hash) {
  if (!enabled_) {
    return;
  }
  execution_hang_identified_ = true;
  AppendToxic(vertex_shader_hash, pixel_shader_hash);
  // The device is going down with it, so keep whatever else this run learned.
  device_lost_.store(true, std::memory_order_release);
  XELOGE(
      "Toxic-shader solver: VS {:016X}, PS {:016X} HUNG THE GPU. It was the "
      "only draw in flight - it was submitted on its own and waited on - so "
      "this is the culprit, not a guess. Quarantined in {}: the next launch "
      "skips it and needs no hunting. Remove the line if it turns out to be "
      "the wrong call.",
      vertex_shader_hash, pixel_shader_hash, xe::path_to_utf8(toxic_path_));
}

void ToxicShaderSolver::MarkExecutionHang() {
  if (!enabled_) {
    return;
  }
  if (execution_hang_identified_) {
    // An isolated draw already named itself. Asking the next launch to
    // serialize every draw would cost a whole slideshow session to rediscover
    // something that is written down.
    return;
  }
  // A marker, not a suspect list: this run wasn't serializing, so nothing here
  // knows which draw did it. Its presence makes the NEXT run serialize, and
  // that run's journal names the culprit.
  std::error_code ec;
  if (std::filesystem::exists(execution_hang_path_, ec)) {
    return;
  }
  { std::ofstream marker(execution_hang_path_, std::ios::trunc); }
  // The user usually quits through the "device lost" message box, which is a
  // graceful shutdown - and a graceful shutdown deletes the solver's evidence.
  // Mark the device as lost so it is kept instead.
  device_lost_.store(true, std::memory_order_release);
  XELOGW(
      "Toxic-shader solver: recorded a GPU hang during drawing - the next "
      "launch will serialize draws to find which one did it");
}

bool ToxicShaderSolver::NeedsExecutionVerification(uint64_t vertex_shader_hash,
                                                   uint64_t pixel_shader_hash) {
  if (!enabled_ || !cvars::d3d12_verify_new_draws || !execution_journal_file_) {
    return false;
  }
  return verified_.find({vertex_shader_hash, pixel_shader_hash}) ==
         verified_.end();
}

void ToxicShaderSolver::MarkExecutionVerified(uint64_t vertex_shader_hash,
                                              uint64_t pixel_shader_hash) {
  if (!verified_.emplace(vertex_shader_hash, pixel_shader_hash).second) {
    return;
  }
  ++verified_this_run_;
  if (!verified_file_) {
    return;
  }
  // Appended as it is learned, not written at shutdown: a run that ends in a
  // hang must still keep everything it proved safe beforehand, otherwise the
  // next launch re-checks the whole game.
  std::string line =
      fmt::format("{:016X} {:016X}\n", vertex_shader_hash, pixel_shader_hash);
  std::fwrite(line.data(), 1, line.size(), verified_file_);
  std::fflush(verified_file_);
}

void ToxicShaderSolver::ExecutionJournalDraw(uint64_t vertex_shader_hash,
                                             uint64_t pixel_shader_hash) {
  if (!execution_journal_file_) {
    return;
  }
  if (execution_safe_mode_ &&
      ++execution_draws_ > kExecutionSafeModeMaxDraws) {
    // The hang isn't coming back. Stop punishing the session for it - and clear
    // the marker, so the next launch is normal too.
    XELOGW(
        "Toxic-shader solver: {} draws serialized without a hang - it isn't "
        "reproducing, resuming normal drawing",
        execution_draws_ - 1);
    std::fclose(execution_journal_file_);
    execution_journal_file_ = nullptr;
    execution_safe_mode_ = false;
    std::error_code ec;
    std::filesystem::remove(execution_journal_path_, ec);
    std::filesystem::remove(execution_hang_path_, ec);
    return;
  }
  // One line, rewritten each draw and pushed to the OS before the draw is
  // submitted - so whatever is in the file when the device dies is the draw
  // that was running. The OS flushes its own cache even though our process
  // died, which is what makes this survive.
  std::string line =
      fmt::format("{:016X} {:016X}\n", vertex_shader_hash, pixel_shader_hash);
  std::rewind(execution_journal_file_);
  std::fwrite(line.data(), 1, line.size(), execution_journal_file_);
  std::fflush(execution_journal_file_);
  _chsize_s(_fileno(execution_journal_file_),
            static_cast<__int64>(line.size()));
}

void ToxicShaderSolver::QuarantineExecutionSuspect(
    uint64_t vertex_shader_hash, uint64_t pixel_shader_hash) {
  if (!enabled_) {
    return;
  }
  AppendToxic(vertex_shader_hash, pixel_shader_hash);
  XELOGW(
      "Toxic-shader solver: quarantined EXECUTION hang suspect VS {:016X}, PS "
      "{:016X} (the most recently bound pipeline when the device hung) - it "
      "will be skipped from the next launch; if the hang persists, the next "
      "suspect will be quarantined on the next death. Remove the pair from {} "
      "if it turns out innocent.",
      vertex_shader_hash, pixel_shader_hash, xe::path_to_utf8(toxic_path_));
}

void ToxicShaderSolver::RewriteJournalLocked() {
  if (!journal_file_) {
    return;
  }
  std::string buffer;
  for (const std::pair<uint64_t, uint64_t>& s : inflight_) {
    buffer += fmt::format("{:016X} {:016X}\n", s.first, s.second);
  }
  std::rewind(journal_file_);
  if (!buffer.empty()) {
    std::fwrite(buffer.data(), 1, buffer.size(), journal_file_);
  }
  // Push the CRT buffer to the OS so the data survives a process crash (the OS
  // still writes its cache to disk even though our process died); then shrink
  // the file to exactly what we wrote so stale trailing bytes aren't parsed.
  std::fflush(journal_file_);
  _chsize_s(_fileno(journal_file_), static_cast<__int64>(buffer.size()));
}

void ToxicShaderSolver::JournalBegin(uint64_t vertex_shader_hash,
                                     uint64_t pixel_shader_hash) {
  std::lock_guard<std::mutex> lock(journal_mutex_);
  inflight_.emplace(vertex_shader_hash, pixel_shader_hash);
  RewriteJournalLocked();
}

void ToxicShaderSolver::JournalEnd(uint64_t vertex_shader_hash,
                                   uint64_t pixel_shader_hash) {
  std::lock_guard<std::mutex> lock(journal_mutex_);
  inflight_.erase(std::make_pair(vertex_shader_hash, pixel_shader_hash));
  RewriteJournalLocked();
}

void ToxicShaderSolver::AppendToxic(uint64_t vertex_shader_hash,
                                    uint64_t pixel_shader_hash) {
  // Effective from this instant, not from the next launch. Every quarantine
  // goes through here - the compiler crash, the device removal in safe mode,
  // the execution hang - so all of them now stop the pair being used for the
  // rest of the session as well as being written down for the next one.
  QuarantineForThisRun(vertex_shader_hash, pixel_shader_hash);
  {
    // May be called from multiple creation threads (crash-catch path).
    std::lock_guard<std::mutex> lock(journal_mutex_);
    std::ofstream toxic_file(toxic_path_, std::ios::app);
    if (!toxic_file) {
      return;
    }
    toxic_file << fmt::format("{:016X} {:016X}\n", vertex_shader_hash,
                              pixel_shader_hash);
  }
  if (pixel_shader_hash == kToxicAnyPixelShader) {
    return;
  }
  // If this vertex shader has now hung with several different pixel shaders,
  // it is the vertex shader that is at fault, not any of the pairs. Left as
  // pairs, the game would need a crash-and-restart cycle for every pixel
  // shader it is ever drawn with - Black Ops has one such vertex shader
  // appearing with at least four. Promote it to a whole-shader entry.
  std::lock_guard<std::mutex> lock(journal_mutex_);
  size_t pairs_with_vertex_shader = 0;
  for (const std::pair<uint64_t, uint64_t>& toxic : toxic_shaders_) {
    if (toxic.first == vertex_shader_hash &&
        toxic.second != kToxicAnyPixelShader) {
      ++pairs_with_vertex_shader;
    }
  }
  if (pairs_with_vertex_shader < kToxicPairsPerVertexShader) {
    return;
  }
  if (!toxic_shaders_.emplace(vertex_shader_hash, kToxicAnyPixelShader)
           .second) {
    return;
  }
  // The whole-shader entry has to take effect this run too. Appended inline
  // rather than through QuarantineForThisRun because journal_mutex_ is held
  // here and it is not recursive.
  uint32_t runtime_count =
      runtime_quarantine_count_.load(std::memory_order_relaxed);
  if (runtime_count < kMaxRuntimeQuarantine) {
    runtime_quarantine_[runtime_count] = {vertex_shader_hash,
                                          kToxicAnyPixelShader};
    runtime_quarantine_count_.store(runtime_count + 1,
                                    std::memory_order_release);
  }
  std::ofstream toxic_file(toxic_path_, std::ios::app);
  if (toxic_file) {
    toxic_file << fmt::format("{:016X} {:016X}\n", vertex_shader_hash,
                              kToxicAnyPixelShader);
  }
  XELOGW(
      "Toxic-shader solver: VS {:016X} has now hung the GPU with {} different "
      "pixel shaders - quarantining every draw that uses it, rather than one "
      "pair per crash",
      vertex_shader_hash, pairs_with_vertex_shader);
}

void ToxicShaderSolver::RetractThisRunToxic() {
  // Out-of-memory was detected: compiler crashes quarantined EARLIER in this
  // run (before the first observed E_OUTOFMEMORY) were most likely also
  // out-of-memory victims. Rewrite the skip list with only the entries known at
  // startup, dropping everything appended during this session.
  std::lock_guard<std::mutex> lock(journal_mutex_);
  std::ofstream toxic_file(toxic_path_, std::ios::trunc);
  if (!toxic_file) {
    return;
  }
  for (const auto& pair : toxic_shaders_) {
    toxic_file << fmt::format("{:016X} {:016X}\n", pair.first, pair.second);
  }
  XELOGW(
      "Toxic-shader solver: dropped the pairs quarantined during this "
      "out-of-memory session from {} (kept the {} known at startup)",
      xe::path_to_utf8(toxic_path_), toxic_shaders_.size());
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
