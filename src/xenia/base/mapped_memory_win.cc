/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform_win.h"

#if XE_PLATFORM_WINRT
// CreateFile / GetFileSize are desktop-partition only; use the App-Container
// equivalents. (CreateFileMapping/MapViewOfFile already have *FromApp branches.)
#include <fileapifromapp.h>

// The App Container SDK headers gate these out, but they are present in the
// runtime (used the same way by github.com/SternXD/xbox-swap).
extern "C" {
WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(
    ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
WINBASEAPI ULONG WINAPI RemoveVectoredExceptionHandler(PVOID Handle);
}

DEFINE_bool(
    winrt_demand_page_large_files, true,
    "Xbox UWP: when a large read-only container (e.g. a 2 GB XBLA package) "
    "can't be memory-mapped because the console's mapped-view quota is "
    "exhausted, keep it on disk and page only the accessed regions into RAM "
    "on demand (a small LRU window budget) instead of reading the whole file "
    "into memory. Frees RAM for emulation and rendering at the cost of "
    "background SSD reads. Disable to always read the whole container into a "
    "RAM buffer.",
    "Memory");
DEFINE_int32(
    winrt_demand_page_budget_mb, 512,
    "Xbox UWP: resident RAM budget, in megabytes, for demand-paged large "
    "containers (winrt_demand_page_large_files). Higher keeps more of the "
    "file hot (fewer re-reads) but leaves less RAM for the game; lower frees "
    "RAM but pages from disk more often.",
    "Memory");
#endif

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | \
                            WINAPI_PARTITION_SYSTEM | WINAPI_PARTITION_GAMES)
#define XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif
#if XE_PLATFORM_WINRT
// The App Container partition still passes the check above, but must use the
// *FromApp file-mapping/view variants (see memory_win.cc for the rationale).
#undef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif

namespace xe {

#if XE_PLATFORM_WINRT
// Demand-pager for a large read-only file that can't be memory-mapped in the
// App Container (mapped-view quota, err 8). Reserves the whole range as
// address space (cheap - only 40 VA bits are needed) and, via a vectored
// exception handler, commits + reads a small window from the file the first
// time each region is touched, evicting the least-recently-paged window once
// the resident budget is exceeded. Callers see an ordinary contiguous pointer;
// only the working set costs RAM.
class DemandPagedFile {
 public:
  static DemandPagedFile* Create(HANDLE file, uint64_t file_offset,
                                  size_t total, size_t budget_bytes) {
    // 4 MB windows: big enough to amortize the fault + SSD read, small enough
    // to keep the resident set close to the budget.
    constexpr size_t kWindowSize = size_t(4) << 20;
    size_t window_count = (total + kWindowSize - 1) / kWindowSize;
    size_t budget_windows =
        std::max<size_t>(2, budget_bytes / kWindowSize);
    void* base = VirtualAllocFromApp(nullptr, total, MEM_RESERVE,
                                     PAGE_NOACCESS);
    if (!base) {
      return nullptr;
    }
    auto* pager = new DemandPagedFile();
    pager->file_ = file;
    pager->file_offset_ = file_offset;
    pager->base_ = reinterpret_cast<uint8_t*>(base);
    pager->total_ = total;
    pager->window_size_ = kWindowSize;
    pager->budget_windows_ = budget_windows;
    pager->committed_.assign(window_count, false);
    {
      std::lock_guard<std::mutex> lock(registry_mutex());
      registry().push_back(pager);
      if (!veh_handle()) {
        veh_handle() = AddVectoredExceptionHandler(1, VehThunk);
      }
    }
    return pager;
  }

  ~DemandPagedFile() {
    {
      std::lock_guard<std::mutex> lock(registry_mutex());
      auto& reg = registry();
      reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());
      // The handler stays installed as long as any pager exists; only the last
      // one removes it. (Teardown happens at game unmount, when guest threads
      // touching the container are already gone.)
      if (reg.empty() && veh_handle()) {
        RemoveVectoredExceptionHandler(veh_handle());
        veh_handle() = nullptr;
      }
    }
    if (base_) {
      VirtualFree(base_, 0, MEM_RELEASE);
    }
  }

  uint8_t* base() const { return base_; }
  size_t resident_bytes() const { return lru_.size() * window_size_; }

 private:
  static std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
  }
  static std::vector<DemandPagedFile*>& registry() {
    static std::vector<DemandPagedFile*> r;
    return r;
  }
  static PVOID& veh_handle() {
    static PVOID h = nullptr;
    return h;
  }

  static LONG CALLBACK VehThunk(PEXCEPTION_POINTERS exception_pointers) {
    if (exception_pointers->ExceptionRecord->ExceptionCode !=
        EXCEPTION_ACCESS_VIOLATION) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    void* addr = reinterpret_cast<void*>(
        exception_pointers->ExceptionRecord->ExceptionInformation[1]);
    DemandPagedFile* owner = nullptr;
    {
      std::lock_guard<std::mutex> lock(registry_mutex());
      for (DemandPagedFile* pager : registry()) {
        if (addr >= pager->base_ && addr < pager->base_ + pager->total_) {
          owner = pager;
          break;
        }
      }
    }
    if (!owner) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    return owner->HandleFault(reinterpret_cast<uint8_t*>(addr))
               ? EXCEPTION_CONTINUE_EXECUTION
               : EXCEPTION_CONTINUE_SEARCH;
  }

  bool HandleFault(uint8_t* addr) {
    size_t window_index = size_t(addr - base_) / window_size_;
    std::lock_guard<std::mutex> lock(mutex_);
    if (committed_[window_index]) {
      // Another thread paged it in between the fault and acquiring the lock.
      return true;
    }
    uint8_t* window_base = base_ + window_index * window_size_;
    size_t window_bytes =
        std::min(window_size_, total_ - window_index * window_size_);
    if (!VirtualAllocFromApp(window_base, window_bytes, MEM_COMMIT,
                             PAGE_READWRITE)) {
      return false;
    }
    OVERLAPPED overlapped = {};
    uint64_t read_at = file_offset_ + uint64_t(window_index) * window_size_;
    overlapped.Offset = DWORD(read_at & 0xFFFFFFFF);
    overlapped.OffsetHigh = DWORD(read_at >> 32);
    size_t read_total = 0;
    bool read_ok = true;
    while (read_total < window_bytes) {
      DWORD to_read = DWORD(std::min<size_t>(window_bytes - read_total,
                                             DWORD(0x7FFFFFFF)));
      DWORD bytes_read = 0;
      // Synchronous handle + explicit OVERLAPPED offset: reads at read_at
      // without touching a shared file position, blocks until done.
      if (!ReadFile(file_, window_base + read_total, to_read, &bytes_read,
                    &overlapped) ||
          bytes_read == 0) {
        // A short read past EOF is fine (the last window may be partial and
        // the reserved tail is zero-filled); a hard failure is not.
        read_ok = (GetLastError() == ERROR_HANDLE_EOF);
        break;
      }
      read_total += bytes_read;
      uint64_t next = read_at + read_total;
      overlapped.Offset = DWORD(next & 0xFFFFFFFF);
      overlapped.OffsetHigh = DWORD(next >> 32);
    }
    if (!read_ok) {
      VirtualFree(window_base, window_bytes, MEM_DECOMMIT);
      return false;
    }
    committed_[window_index] = true;
    lru_.push_back(window_index);
    // Evict the oldest window(s) once over budget. Never the one just paged in
    // (it's at the back, and the budget is >= 2, so front != back).
    while (lru_.size() > budget_windows_) {
      size_t evict_index = lru_.front();
      lru_.pop_front();
      VirtualFree(base_ + evict_index * window_size_,
                  std::min(window_size_, total_ - evict_index * window_size_),
                  MEM_DECOMMIT);
      committed_[evict_index] = false;
    }
    return true;
  }

  HANDLE file_ = INVALID_HANDLE_VALUE;
  uint64_t file_offset_ = 0;
  uint8_t* base_ = nullptr;
  size_t total_ = 0;
  size_t window_size_ = 0;
  size_t budget_windows_ = 0;
  std::mutex mutex_;
  std::vector<bool> committed_;
  std::deque<size_t> lru_;
};
#endif  // XE_PLATFORM_WINRT

class Win32MappedMemory : public MappedMemory {
 public:
  // CreateFile returns INVALID_HANDLE_VALUE in case of failure.
  // chrispy: made inline const to get around clang error
  static inline constexpr HANDLE kFileHandleInvalid = INVALID_HANDLE_VALUE;
  // CreateFileMapping returns nullptr in case of failure.
  static constexpr HANDLE kMappingHandleInvalid = nullptr;

  ~Win32MappedMemory() override {
    if (data_) {
      UnmapAllViews();
    }
    if (mapping_handle != kMappingHandleInvalid) {
      CloseHandle(mapping_handle);
    }
    if (file_handle != kFileHandleInvalid) {
      CloseHandle(file_handle);
    }
  }

  void Close(uint64_t truncate_size) override {
    if (data_) {
      UnmapAllViews();
      data_ = nullptr;
    }
    if (mapping_handle != kMappingHandleInvalid) {
      CloseHandle(mapping_handle);
      mapping_handle = kMappingHandleInvalid;
    }
    if (file_handle != kFileHandleInvalid) {
      if (truncate_size) {
        LONG distance_high = truncate_size >> 32;
        SetFilePointer(file_handle, truncate_size & 0xFFFFFFFF, &distance_high,
                       FILE_BEGIN);
        SetEndOfFile(file_handle);
      }

      CloseHandle(file_handle);
      file_handle = kFileHandleInvalid;
    }
  }

  void Flush() override {
#if XE_PLATFORM_WINRT
    if (owns_buffer_ || demand_pager_) {
      // A read-only snapshot buffer / demand-paged view, not a mapping -
      // nothing to flush.
      return;
    }
#endif
    FlushViewOfFile(data(), size());
  }
  bool Remap(size_t offset, size_t length) override {
#if XE_PLATFORM_WINRT
    if (!chunk_views_.empty() || owns_buffer_ || demand_pager_) {
      // Chunked read-only mappings, read-into-buffer snapshots and
      // demand-paged views are not remappable.
      return false;
    }
#endif
    size_t aligned_offset = offset & ~(memory::allocation_granularity() - 1);
    size_t aligned_length = length + (offset - aligned_offset);

    UnmapViewOfFile(data_);
#ifdef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
    data_ = MapViewOfFile(mapping_handle, view_access_, aligned_offset >> 32,
                          aligned_offset & 0xFFFFFFFF, aligned_length);
#else
    data_ = MapViewOfFileFromApp(mapping_handle, ULONG(view_access_),
                                 ULONG64(aligned_offset), aligned_length);
#endif
    if (!data_) {
      return false;
    }

    if (length) {
      size_ = aligned_length;
    } else {
#if XE_PLATFORM_WINRT
      LARGE_INTEGER file_size_li = {};
      GetFileSizeEx(file_handle, &file_size_li);
      size_t map_length = static_cast<size_t>(file_size_li.QuadPart);
#else
      DWORD length_high;
      size_t map_length = GetFileSize(file_handle, &length_high);
      map_length |= static_cast<uint64_t>(length_high) << 32;
#endif
      size_ = map_length - aligned_offset;
    }

    return true;
  }

  HANDLE file_handle = kFileHandleInvalid;
  HANDLE mapping_handle = kMappingHandleInvalid;
  DWORD view_access_ = 0;
#if XE_PLATFORM_WINRT
  // Non-empty when the file was mapped as multiple consecutive views over one
  // reserved placeholder region (fallback for huge read-only containers that
  // MapViewOfFileFromApp refuses to map in one piece - err 8 on the console).
  std::vector<void*> chunk_views_;
  // True when data_ is a plain committed buffer the file was READ into (the
  // final fallback when even chunked views exhaust the console's mapped-view
  // quota) - freed with VirtualFree, not unmapped.
  bool owns_buffer_ = false;
  // Non-null when data_ is a demand-paged view of the file (the tier before
  // the full-buffer read): the file stays on disk and only accessed windows
  // are resident. Owns the reserved region and the exception handler.
  std::unique_ptr<DemandPagedFile> demand_pager_;
#endif

 private:
  void UnmapAllViews() {
#if XE_PLATFORM_WINRT
    if (demand_pager_) {
      // Destroying the pager removes the exception handler and frees the
      // reserved region (which is data_).
      demand_pager_.reset();
      return;
    }
    if (owns_buffer_) {
      VirtualFree(data_, 0, MEM_RELEASE);
      owns_buffer_ = false;
      return;
    }
    if (!chunk_views_.empty()) {
      for (void* view : chunk_views_) {
        UnmapViewOfFile(view);
      }
      chunk_views_.clear();
      return;
    }
#endif
    UnmapViewOfFile(data_);
  }
};

std::unique_ptr<MappedMemory> MappedMemory::Open(
    const std::filesystem::path& path, Mode mode, size_t offset,
    size_t length) {
  DWORD file_access = 0;
  DWORD file_share = 0;
  DWORD create_mode = 0;
  DWORD mapping_protect = 0;
  DWORD view_access = 0;
  switch (mode) {
    case Mode::kRead:
      file_access |= GENERIC_READ;
      file_share |= FILE_SHARE_READ;
      create_mode |= OPEN_EXISTING;
      mapping_protect |= PAGE_READONLY;
      view_access |= FILE_MAP_READ;
      break;
    case Mode::kReadWrite:
      file_access |= GENERIC_READ | GENERIC_WRITE;
      file_share |= 0;
      create_mode |= OPEN_EXISTING;
      mapping_protect |= PAGE_READWRITE;
      view_access |= FILE_MAP_READ | FILE_MAP_WRITE;
      break;
  }

  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);

  const size_t aligned_offset =
      offset & ~static_cast<size_t>(system_info.dwAllocationGranularity - 1);
  const size_t aligned_length = length + (offset - aligned_offset);

  auto mm = std::make_unique<Win32MappedMemory>();
  mm->view_access_ = view_access;

#if XE_PLATFORM_WINRT
  CREATEFILE2_EXTENDED_PARAMETERS mm_cf_params = {};
  mm_cf_params.dwSize = sizeof(mm_cf_params);
  mm_cf_params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  mm->file_handle = CreateFile2FromAppW(path.c_str(), file_access, file_share,
                                        create_mode, &mm_cf_params);
#else
  mm->file_handle = CreateFile(path.c_str(), file_access, file_share, nullptr,
                               create_mode, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
  if (mm->file_handle == Win32MappedMemory::kFileHandleInvalid) {
#if XE_PLATFORM_WINRT
    XELOGE("MappedMemory::Open: CreateFile2FromAppW('{}') failed (err {})",
           xe::path_to_utf8(path), GetLastError());
#endif
    return nullptr;
  }

#ifdef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  mm->mapping_handle = CreateFileMapping(
      mm->file_handle, nullptr, mapping_protect, DWORD(aligned_length >> 32),
      DWORD(aligned_length), nullptr);
#else
  mm->mapping_handle =
      CreateFileMappingFromApp(mm->file_handle, nullptr, ULONG(mapping_protect),
                               ULONG64(aligned_length), nullptr);
#endif
  if (mm->mapping_handle == Win32MappedMemory::kMappingHandleInvalid) {
#if XE_PLATFORM_WINRT
    XELOGE("MappedMemory::Open: CreateFileMappingFromApp('{}') failed (err {})",
           xe::path_to_utf8(path), GetLastError());
#endif
    return nullptr;
  }

#ifdef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  mm->data_ = reinterpret_cast<uint8_t*>(MapViewOfFile(
      mm->mapping_handle, view_access, DWORD(aligned_offset >> 32),
      DWORD(aligned_offset), aligned_length));
#else
  mm->data_ = reinterpret_cast<uint8_t*>(
      MapViewOfFileFromApp(mm->mapping_handle, ULONG(view_access),
                           ULONG64(aligned_offset), aligned_length));
#endif
  if (!mm->data_) {
#if XE_PLATFORM_WINRT
    DWORD map_error = GetLastError();
    // Fallback for huge read-only containers (multi-GB XContent packages):
    // the console refuses a single giant view, but consecutive smaller views
    // over one reserved placeholder region produce the same contiguous
    // pointer. IMPORTANT: MEM_REPLACE_PLACEHOLDER requires the placeholder to
    // EXACTLY match the view being mapped - a placeholder does NOT get split
    // automatically (mapping a chunk into the front of a bigger placeholder
    // fails with ERROR_INVALID_ADDRESS, 487). Before each chunk except the
    // final one, split off an exactly chunk-sized placeholder with
    // VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) as per the
    // placeholder API contract.
    if (mode == Mode::kRead) {
      LARGE_INTEGER file_size_li = {};
      GetFileSizeEx(mm->file_handle, &file_size_li);
      size_t total = length ? aligned_length
                            : size_t(file_size_li.QuadPart) - aligned_offset;
      HANDLE process = GetCurrentProcess();
      void* base = VirtualAlloc2FromApp(
          process, nullptr, total, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
          PAGE_NOACCESS, nullptr, 0);
      if (base) {
        constexpr size_t kChunkSize = 256ull << 20;  // 256 MB per view.
        size_t off = 0;
        bool split_done_at_off = false;
        bool ok = true;
        while (off < total) {
          size_t chunk = std::min(kChunkSize, total - off);
          uint8_t* chunk_base = reinterpret_cast<uint8_t*>(base) + off;
          split_done_at_off = false;
          if (off + chunk < total) {
            // Not the final piece - split [off, off+chunk) off the remaining
            // placeholder so the view replaces an exactly-sized placeholder.
            if (!VirtualFree(chunk_base, chunk,
                             MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
              XELOGE(
                  "MappedMemory::Open: placeholder split failed at offset "
                  "0x{:X} (err {})",
                  uint64_t(off), GetLastError());
              ok = false;
              break;
            }
            split_done_at_off = true;
          }
          void* view = MapViewOfFile3FromApp(
              mm->mapping_handle, process, chunk_base,
              ULONG64(aligned_offset + off), chunk, MEM_REPLACE_PLACEHOLDER,
              ULONG(PAGE_READONLY), nullptr, 0);
          if (!view) {
            XELOGE(
                "MappedMemory::Open: chunked MapViewOfFile3FromApp failed at "
                "offset 0x{:X} (err {})",
                uint64_t(off), GetLastError());
            ok = false;
            break;
          }
          mm->chunk_views_.push_back(view);
          off += chunk;
        }
        if (ok) {
          XELOGI(
              "MappedMemory::Open: '{}' mapped as {} chunked views ({} MB "
              "total) after single-view failure (err {})",
              xe::path_to_utf8(path), mm->chunk_views_.size(), total >> 20,
              map_error);
          mm->data_ = reinterpret_cast<uint8_t*>(base);
        } else {
          // Unmap the successfully mapped views (releases their regions), then
          // free the remaining placeholder pieces: after splitting, the
          // reservation is no longer a single allocation, so free the piece at
          // the failure offset and, if the failed step had already split, the
          // piece after it.
          for (void* view : mm->chunk_views_) {
            UnmapViewOfFile(view);
          }
          mm->chunk_views_.clear();
          uint8_t* fail_base = reinterpret_cast<uint8_t*>(base) + off;
          VirtualFree(fail_base, 0, MEM_RELEASE);
          if (split_done_at_off && off + std::min(kChunkSize, total - off) < total) {
            VirtualFree(fail_base + std::min(kChunkSize, total - off), 0,
                        MEM_RELEASE);
          }
        }
      }
      if (!mm->data_) {
        // The console also caps the TOTAL bytes of file views a process may
        // have mapped (the chunked path can die with err 8 partway through a
        // multi-GB container once the quota is exhausted - the guest memory
        // mapping already uses a large share of it). Reading the whole file
        // into a committed buffer works but costs its full size in RAM (2+ GB
        // for a big XBLA package), starving emulation and rendering. Prefer
        // demand paging: keep the file on disk, reserve the address space, and
        // page only the accessed windows into RAM under an LRU budget.
        if (cvars::winrt_demand_page_large_files) {
          size_t budget_bytes =
              size_t(std::max(int32_t(64),
                              cvars::winrt_demand_page_budget_mb)) << 20;
          DemandPagedFile* pager = DemandPagedFile::Create(
              mm->file_handle, aligned_offset, total, budget_bytes);
          if (pager) {
            XELOGI(
                "MappedMemory::Open: '{}' demand-paged ({} MB file, {} MB "
                "resident budget) after view mapping failures - kept on disk, "
                "paged on access",
                xe::path_to_utf8(path), total >> 20, budget_bytes >> 20);
            mm->demand_pager_.reset(pager);
            mm->data_ = pager->base();
          }
        }
      }
      if (!mm->data_) {
        // Last resort: plain committed memory is limited only by the App/Game
        // commit budget (5 GB in Game mode with expandedResources), so
        // allocate a buffer and READ the container into it instead of mapping.
        void* buffer = VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
        if (buffer) {
          LARGE_INTEGER read_pos;
          read_pos.QuadPart = LONGLONG(aligned_offset);
          bool read_ok =
              SetFilePointerEx(mm->file_handle, read_pos, nullptr, FILE_BEGIN);
          size_t read_off = 0;
          while (read_ok && read_off < total) {
            DWORD to_read =
                DWORD(std::min<size_t>(64ull << 20, total - read_off));
            DWORD bytes_read = 0;
            if (!ReadFile(mm->file_handle,
                          reinterpret_cast<uint8_t*>(buffer) + read_off,
                          to_read, &bytes_read, nullptr) ||
                bytes_read == 0) {
              read_ok = false;
              break;
            }
            read_off += bytes_read;
          }
          if (read_ok && read_off >= total) {
            // Read-only from the caller's perspective - drop the write access.
            DWORD old_protect;
            VirtualProtect(buffer, total, PAGE_READONLY, &old_protect);
            XELOGI(
                "MappedMemory::Open: '{}' read into a {} MB buffer after view "
                "mapping failures (the console's mapped-view quota)",
                xe::path_to_utf8(path), total >> 20);
            mm->owns_buffer_ = true;
            mm->data_ = reinterpret_cast<uint8_t*>(buffer);
          } else {
            XELOGE(
                "MappedMemory::Open: buffered read of '{}' failed at offset "
                "0x{:X} (err {})",
                xe::path_to_utf8(path), uint64_t(read_off), GetLastError());
            VirtualFree(buffer, 0, MEM_RELEASE);
          }
        } else {
          XELOGE(
              "MappedMemory::Open: couldn't allocate a {} MB buffer for '{}' "
              "(err {})",
              total >> 20, xe::path_to_utf8(path), GetLastError());
        }
      }
    }
    if (!mm->data_) {
      XELOGE("MappedMemory::Open: MapViewOfFileFromApp('{}') failed (err {})",
             xe::path_to_utf8(path), map_error);
      return nullptr;
    }
#else
    return nullptr;
#endif
  }

  if (length) {
    mm->size_ = aligned_length;
  } else {
#if XE_PLATFORM_WINRT
    LARGE_INTEGER file_size_li = {};
    GetFileSizeEx(mm->file_handle, &file_size_li);
    size_t map_length = static_cast<size_t>(file_size_li.QuadPart);
#else
    DWORD length_high;
    size_t map_length = GetFileSize(mm->file_handle, &length_high);
    map_length |= static_cast<uint64_t>(length_high) << 32;
#endif
    mm->size_ = map_length - aligned_offset;
  }

  return std::move(mm);
}

class Win32ChunkedMappedMemoryWriter : public ChunkedMappedMemoryWriter {
 public:
  Win32ChunkedMappedMemoryWriter(const std::filesystem::path& path,
                                 size_t chunk_size, bool low_address_space)
      : ChunkedMappedMemoryWriter(path, chunk_size, low_address_space) {}

  ~Win32ChunkedMappedMemoryWriter() override {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
  }

  uint8_t* Allocate(size_t length) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!chunks_.empty()) {
      uint8_t* result = chunks_.back()->Allocate(length);
      if (result != nullptr) {
        return result;
      }
    }
    auto chunk = std::make_unique<Chunk>(chunk_size_);
    auto chunk_path =
        path_.replace_extension(fmt::format(".{}", chunks_.size()));
    if (!chunk->Open(chunk_path, low_address_space_)) {
      return nullptr;
    }
    uint8_t* result = chunk->Allocate(length);
    chunks_.push_back(std::move(chunk));
    return result;
  }

  void Flush() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& chunk : chunks_) {
      chunk->Flush();
    }
  }

  void FlushNew() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& chunk : chunks_) {
      chunk->FlushNew();
    }
  }

 private:
  class Chunk {
   public:
    explicit Chunk(size_t capacity)
        : file_handle_(Win32MappedMemory::kFileHandleInvalid),
          mapping_handle_(Win32MappedMemory::kMappingHandleInvalid),
          data_(nullptr),
          offset_(0),
          capacity_(capacity),
          last_flush_offset_(0) {}

    ~Chunk() {
      if (data_) {
        UnmapViewOfFile(data_);
      }
      if (mapping_handle_ != Win32MappedMemory::kMappingHandleInvalid) {
        CloseHandle(mapping_handle_);
      }
      if (file_handle_ != Win32MappedMemory::kFileHandleInvalid) {
        CloseHandle(file_handle_);
      }
    }

    bool Open(const std::filesystem::path& path, bool low_address_space) {
      DWORD file_access = GENERIC_READ | GENERIC_WRITE;
      DWORD file_share = FILE_SHARE_READ;
      DWORD create_mode = CREATE_ALWAYS;
      DWORD mapping_protect = PAGE_READWRITE;
      DWORD view_access = FILE_MAP_READ | FILE_MAP_WRITE;

#if XE_PLATFORM_WINRT
      CREATEFILE2_EXTENDED_PARAMETERS cf_params = {};
      cf_params.dwSize = sizeof(cf_params);
      cf_params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      file_handle_ = CreateFile2FromAppW(path.c_str(), file_access, file_share,
                                         create_mode, &cf_params);
#else
      file_handle_ = CreateFile(path.c_str(), file_access, file_share, nullptr,
                                create_mode, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
      if (file_handle_ == Win32MappedMemory::kFileHandleInvalid) {
        return false;
      }

#ifdef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
      mapping_handle_ =
          CreateFileMapping(file_handle_, nullptr, mapping_protect,
                            DWORD(capacity_ >> 32), DWORD(capacity_), nullptr);
#else
      mapping_handle_ = CreateFileMappingFromApp(file_handle_, nullptr,
                                                 ULONG(mapping_protect),
                                                 ULONG64(capacity_), nullptr);
#endif
      if (mapping_handle_ == Win32MappedMemory::kMappingHandleInvalid) {
        return false;
      }

      // If specified, ensure the allocation occurs in the lower 32-bit address
      // space.
      if (low_address_space) {
        bool successful = false;
        data_ = reinterpret_cast<uint8_t*>(0x10000000);
#ifndef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
        HANDLE process = GetCurrentProcess();
#endif
        for (int i = 0; i < 1000; ++i) {
#ifdef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
          if (MapViewOfFileEx(mapping_handle_, view_access, 0, 0, capacity_,
                              data_)) {
            successful = true;
          }
#else
          // VirtualAlloc2FromApp and MapViewOfFile3FromApp were added in
          // 10.0.17134.0.
          // https://docs.microsoft.com/en-us/uwp/win32-and-com/win32-apis
          if (VirtualAlloc2FromApp(process, data_, capacity_,
                                   MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                   PAGE_NOACCESS, nullptr, 0)) {
            if (MapViewOfFile3FromApp(mapping_handle_, process, data_, 0,
                                      capacity_, MEM_REPLACE_PLACEHOLDER,
                                      ULONG(mapping_protect), nullptr, 0)) {
              successful = true;
            } else {
              VirtualFree(data_, capacity_, MEM_RELEASE);
            }
          }
#endif
          if (successful) {
            break;
          }
          data_ += capacity_;
          if (!successful) {
            XELOGE("Unable to find space for mapping");
            data_ = nullptr;
            return false;
          }
        }
      } else {
#ifdef XE_BASE_MAPPED_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
        data_ = reinterpret_cast<uint8_t*>(
            MapViewOfFile(mapping_handle_, view_access, 0, 0, capacity_));
#else
        data_ = reinterpret_cast<uint8_t*>(MapViewOfFileFromApp(
            mapping_handle_, ULONG(view_access), 0, capacity_));
#endif
      }
      if (!data_) {
        return false;
      }

      return true;
    }

    uint8_t* Allocate(size_t length) {
      if (capacity_ - offset_ < length) {
        return nullptr;
      }
      uint8_t* result = data_ + offset_;
      offset_ += length;
      return result;
    }

    void Flush() { FlushViewOfFile(data_, offset_); }

    void FlushNew() {
      FlushViewOfFile(data_ + last_flush_offset_, offset_ - last_flush_offset_);
      last_flush_offset_ = offset_;
    }

   private:
    HANDLE file_handle_;
    HANDLE mapping_handle_;
    uint8_t* data_;
    size_t offset_;
    size_t capacity_;
    size_t last_flush_offset_;
  };

  std::mutex mutex_;
  std::vector<std::unique_ptr<Chunk>> chunks_;
};

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size,
    bool low_address_space) {
  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);
  size_t aligned_chunk_size =
      xe::round_up(chunk_size, system_info.dwAllocationGranularity);
  return std::make_unique<Win32ChunkedMappedMemoryWriter>(
      path, aligned_chunk_size, low_address_space);
}

}  // namespace xe
