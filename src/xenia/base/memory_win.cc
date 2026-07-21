/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform_win.h"

#if XE_PLATFORM_WINRT
#include <fileapifromapp.h>  // CreateFileFromAppW for the guest swap file.
#endif

#if XE_PLATFORM_WINRT
DEFINE_string(
    winrt_guest_memory_swap_file, "",
    "Xbox UWP: base path of the swap files to try backing the guest VIRTUAL "
    "memory ranges with (e.g. \"D:\\\\xenia_guest_swap.bin\" - a second file "
    "with a .1 suffix is created next to it, ~2 GB preallocated in total), or "
    "empty (recommended) to keep everything on the standard pagefile-backed "
    "section. EXPERIMENTAL AND KNOWN NOT TO WORK on current console OS "
    "builds: the OS charges writable file-backed views against the app's "
    "commit budget anyway (ERROR_NOT_ENOUGH_MEMORY on mapping / 1455 later), "
    "so nothing is gained; the emulator detects this and automatically falls "
    "back to the pagefile-backed section for everything.",
    "Memory");
#endif  // XE_PLATFORM_WINRT

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | \
                            WINAPI_PARTITION_SYSTEM | WINAPI_PARTITION_GAMES)
#define XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif
#if XE_PLATFORM_WINRT
// In the UWP App Container the partition check above still passes (the SDK sets
// WINAPI_PARTITION_SYSTEM for the App family), but the desktop VirtualAlloc and
// friends cannot allocate executable memory (e.g. the guest trampoline buffer,
// PAGE_EXECUTE_READWRITE) - that returns NULL and crashes. The *FromApp variants
// (with the codeGeneration capability) must be used instead.
#undef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif
/*
        these two dont bypass much ms garbage compared to the threading ones,
   but Protect is used by PhysicalHeap::EnableAccessCallbacks which eats a lot
   of cpu time, so every bit counts
*/
XE_NTDLL_IMPORT(NtProtectVirtualMemory, cls_NtProtectVirtualMemory,
                NtProtectVirtualMemoryPointer);
XE_NTDLL_IMPORT(NtQueryVirtualMemory, cls_NtQueryVirtualMemory,
                NtQueryVirtualMemoryPointer);
namespace xe {
namespace memory {

size_t page_size() {
#if XE_ARCH_AMD64 == 1
  return 4096;
#else
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwPageSize;
  }
  return value;
#endif
}

size_t allocation_granularity() {
#if XE_ARCH_AMD64 == 1 && XE_PLATFORM_WIN32 == 1
  return 65536;
#else
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwAllocationGranularity;
  }
  return value;
#endif
}

DWORD ToWin32ProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PAGE_NOACCESS;
    case PageAccess::kReadOnly:
      return PAGE_READONLY;
    case PageAccess::kReadWrite:
      return PAGE_READWRITE;
    case PageAccess::kExecuteReadOnly:
      return PAGE_EXECUTE_READ;
    case PageAccess::kExecuteReadWrite:
      return PAGE_EXECUTE_READWRITE;
    default:
      assert_unhandled_case(access);
      return PAGE_NOACCESS;
  }
}

PageAccess ToXeniaProtectFlags(DWORD access) {
  if (access & PAGE_GUARD) {
    // Strip the page guard flag for now...
    access &= ~PAGE_GUARD;
  }

  switch (access) {
    case PAGE_NOACCESS:
      return PageAccess::kNoAccess;
    case PAGE_READONLY:
      return PageAccess::kReadOnly;
    case PAGE_READWRITE:
      return PageAccess::kReadWrite;
    case PAGE_EXECUTE_READ:
      return PageAccess::kExecuteReadOnly;
    case PAGE_EXECUTE_READWRITE:
      return PageAccess::kExecuteReadWrite;
    default:
      return PageAccess::kNoAccess;
  }
}

bool IsWritableExecutableMemorySupported() {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return true;
#elif XE_PLATFORM_WINRT
  // With the codeGeneration capability the FromApp memory APIs allow
  // PAGE_EXECUTE_READWRITE (CreateFileMappingFromApp / MapViewOfFile3FromApp /
  // VirtualProtectFromApp), so the single RWX code-cache mapping works on
  // Xbox. This must be true: in the separate write+execute (W^X) mapping mode
  // the emitter assembles near rel32 branches relative to the write view, so
  // cross-function branches land 0x10000000 short of the target when executed
  // from the execute view (observed as an execute-DEP crash at
  // thunk_address - 0x10000000 on the first guest cross-function call).
  return true;
#else
  // To test FromApp functions on desktop, undefine
  // XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS and link to WindowsApp.lib.
  return false;
#endif
}

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  DWORD alloc_type = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      alloc_type = MEM_RESERVE;
      break;
    case AllocationType::kCommit:
      alloc_type = MEM_COMMIT;
      break;
    case AllocationType::kReserveCommit:
      alloc_type = MEM_RESERVE | MEM_COMMIT;
      break;
    default:
      assert_unhandled_case(allocation_type);
      break;
  }
  DWORD protect = ToWin32ProtectFlags(access);
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return VirtualAlloc(base_address, length, alloc_type, protect);
#else
  // VirtualAllocFromApp REJECTS PAGE_EXECUTE_* ("does not allow the creation of
  // executable pages"). The App Container JIT pattern (with the codeGeneration
  // capability) is: allocate as PAGE_READWRITE, then VirtualProtectFromApp to
  // the executable protection. Without this, executable allocations (e.g. the
  // guest trampoline buffer) return NULL and crash.
  const bool wants_execute =
      protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
      protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
  const DWORD alloc_protect = wants_execute ? PAGE_READWRITE : protect;
  void* result = VirtualAllocFromApp(base_address, length, ULONG(alloc_type),
                                     ULONG(alloc_protect));
  if (!result && alloc_type == MEM_COMMIT) {
    // The callers rarely log details; the error code is essential to tell
    // apart quota/commit exhaustion (err 1455) from range conflicts on the
    // console. Reserve(+commit) requests are not logged: several callers
    // legitimately probe fixed addresses in a loop (e.g. the PPC context
    // allocator walking 0xNN_DFFF0000 slots), which would flood the log.
    XELOGE(
        "AllocFixed: VirtualAllocFromApp(base={}, length=0x{:X}, type=0x{:X}, "
        "protect=0x{:X}) failed (err {})",
        base_address, uint64_t(length), uint32_t(alloc_type),
        uint32_t(alloc_protect), GetLastError());
  }
  if (result && wants_execute && (alloc_type & MEM_COMMIT)) {
    DWORD old_protect = 0;
    if (!VirtualProtectFromApp(result, length, ULONG(protect), &old_protect)) {
      XELOGE(
          "AllocFixed: VirtualProtectFromApp to executable failed (err {}); is "
          "the codeGeneration capability granted and the title in Game mode?",
          GetLastError());
    }
  }
  return result;
#endif
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  DWORD free_type = 0;
  switch (deallocation_type) {
    case DeallocationType::kRelease:
      free_type = MEM_RELEASE;
      length = 0;
      break;
    case DeallocationType::kDecommit:
      free_type = MEM_DECOMMIT;
      break;
    default:
      assert_unhandled_case(deallocation_type);
      break;
  }
  return VirtualFree(base_address, length, free_type) ? true : false;
}

bool IsCommitted(void* address) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(address, &mbi, sizeof(mbi))) {
    // Can't tell - assume it is, so nothing tries to "fix" it.
    return true;
  }
  return mbi.State == MEM_COMMIT;
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }
  DWORD new_protect = ToWin32ProtectFlags(access);

#if XE_USE_NTDLL_FUNCTIONS == 1

  DWORD old_protect = 0;
  SIZE_T MemoryLength = length;
  PVOID MemoryCache = base_address;

  BOOL result = NtProtectVirtualMemoryPointer.invoke<NTSTATUS>(
                    (HANDLE)0xFFFFFFFFFFFFFFFFLL, &MemoryCache, &MemoryLength,
                    new_protect, &old_protect) >= 0;

#else
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD old_protect = 0;
  BOOL result = VirtualProtect(base_address, length, new_protect, &old_protect);
#else
  ULONG old_protect = 0;
  BOOL result = VirtualProtectFromApp(base_address, length, ULONG(new_protect),
                                      &old_protect);
#endif
#endif
  if (!result) {
    return false;
  }
  if (out_old_access) {
    *out_old_access = ToXeniaProtectFlags(DWORD(old_protect));
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;

  MEMORY_BASIC_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
#if XE_USE_NTDLL_FUNCTIONS == 1
  ULONG_PTR ResultLength;

  NTSTATUS query_result = NtQueryVirtualMemoryPointer.invoke<NTSTATUS>(
      (HANDLE)0xFFFFFFFFFFFFFFFFLL, (PVOID)base_address,
      0 /* MemoryBasicInformation*/, &info, length, &ResultLength);
  SIZE_T result = query_result >= 0 ? ResultLength : 0;
#else
  SIZE_T result = VirtualQuery(base_address, &info, sizeof(info));
#endif
  if (!result) {
    return false;
  }

  length = info.RegionSize;
  access_out = ToXeniaProtectFlags(info.Protect);
  return true;
}

#if XE_PLATFORM_WINRT
// xbox-swap-inspired guest memory swapping. Backing the whole guest mapping
// with ONE file-backed section fails on the console: the OS appears to charge
// roughly the section size against the app's commit budget for EVERY writable
// view (observed: the first 1 GB view of the 4.5 GB section maps, the second
// fails with ERROR_NOT_ENOUGH_MEMORY at any base address, with ~5 GB free).
// So instead only the two non-aliased guest virtual ranges are backed - each
// by its own section over its own swap file sized exactly like the single
// view it gets - while the aliased XEX/physical ranges stay on the
// pagefile-backed SEC_RESERVE section (aliased views must share one section
// anyway). This moves up to 2 GB of guest virtual heap commit (where game
// allocations live) off the 5 GB budget.
static bool last_mapping_used_guest_swap_file = false;
static bool guest_memory_swap_file_disabled = false;
// The two unique (non-aliased) ranges of the guest mapping; the file offset
// and view length must match Memory's map_info entries exactly - MapFileView
// substitutes the swap section only on an exact match for the guest mapping
// handle.
static const struct {
  uint64_t file_offset;
  size_t length;
} kGuestSwapRanges[2] = {{0x00000000ull, 0x40000000},
                         {0x40000000ull, 0x3F000000}};
static HANDLE guest_swap_sections[2] = {nullptr, nullptr};
// Whether the CURRENT view of each range actually comes from its swap section
// (per-view fallback may leave a range on the pagefile-backed section) - the
// heaps skip host commit only for genuinely file-backed ranges.
static bool guest_swap_range_mapped[2] = {false, false};
// The pagefile-backed guest mapping whose views the swap sections substitute
// (also keeps the swap away from other CreateFileMappingHandle users - the
// JIT code cache in particular).
static FileMappingHandle guest_swap_main_mapping = kFileMappingHandleInvalid;

bool LastFileMappingUsedGuestSwapFile() {
  return last_mapping_used_guest_swap_file;
}

bool IsGuestMemoryRangeSwapBacked(uint64_t file_offset) {
  for (size_t i = 0; i < xe::countof(kGuestSwapRanges); ++i) {
    if (kGuestSwapRanges[i].file_offset == file_offset) {
      return guest_swap_range_mapped[i];
    }
  }
  return false;
}

void DisableGuestMemorySwapFile() {
  guest_memory_swap_file_disabled = true;
  for (auto& section : guest_swap_sections) {
    if (section) {
      CloseHandle(section);
      section = nullptr;
    }
  }
  for (auto& mapped : guest_swap_range_mapped) {
    mapped = false;
  }
  guest_swap_main_mapping = kFileMappingHandleInvalid;
}

// Creates a section over a freshly truncated (the guest must see zeroed
// memory), fully preallocated swap file covering one guest range. NOT sparse,
// and the clusters are explicitly allocated: only fully guaranteed backing
// storage lets writable views avoid the commit charge.
static HANDLE CreateGuestSwapRangeSection(const std::filesystem::path& path,
                                          size_t range_length, ULONG protect) {
  HANDLE file =
      CreateFileFromAppW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    XELOGW("Failed to create guest swap file {} (error {})",
           xe::path_to_utf8(path), GetLastError());
    return nullptr;
  }
  FILE_ALLOCATION_INFO allocation_info;
  allocation_info.AllocationSize.QuadPart = LONGLONG(range_length);
  SetFileInformationByHandle(file, FileAllocationInfo, &allocation_info,
                             sizeof(allocation_info));
  LARGE_INTEGER file_size;
  file_size.QuadPart = LONGLONG(range_length);
  if (!SetFilePointerEx(file, file_size, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(file)) {
    XELOGW("Failed to size guest swap file {} to {} MB (error {})",
           xe::path_to_utf8(path), range_length >> 20, GetLastError());
    CloseHandle(file);
    return nullptr;
  }
  // Unnamed - the name isn't needed for anything. The section holds its own
  // reference to the file object, so the file handle is closed regardless of
  // the outcome.
  HANDLE mapping = CreateFileMappingFromApp(file, nullptr, protect,
                                            ULONG64(range_length), nullptr);
  CloseHandle(file);
  if (!mapping) {
    XELOGW("Failed to create a section over guest swap file {} (error {})",
           xe::path_to_utf8(path), GetLastError());
  }
  return mapping;
}
#endif  // XE_PLATFORM_WINRT

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
  DWORD protect =
      ToWin32ProtectFlags(access) | (commit ? SEC_COMMIT : SEC_RESERVE);
  auto full_path = "Local" / path;
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, protect,
                            static_cast<DWORD>(length >> 32),
                            static_cast<DWORD>(length), full_path.c_str());
#else
#if XE_PLATFORM_WINRT
  // The guest mapping (recognized by its size - the code cache and tests are
  // far smaller) additionally gets the per-range swap sections; the returned
  // mapping is always the pagefile-backed one, with MapFileView substituting
  // the swap sections for the matching views (falling back per-view on
  // failure).
  if (length > kGuestSwapRanges[1].file_offset + kGuestSwapRanges[1].length &&
      !commit) {
    last_mapping_used_guest_swap_file = false;
    if (!cvars::winrt_guest_memory_swap_file.empty() &&
        !guest_memory_swap_file_disabled) {
      bool swap_sections_created = true;
      for (size_t i = 0; i < xe::countof(kGuestSwapRanges); ++i) {
        if (guest_swap_sections[i]) {
          CloseHandle(guest_swap_sections[i]);
          guest_swap_sections[i] = nullptr;
        }
        std::filesystem::path range_path(
            i ? cvars::winrt_guest_memory_swap_file + "." + std::to_string(i)
              : cvars::winrt_guest_memory_swap_file);
        guest_swap_sections[i] = CreateGuestSwapRangeSection(
            range_path, kGuestSwapRanges[i].length,
            ULONG(ToWin32ProtectFlags(access)));
        if (!guest_swap_sections[i]) {
          swap_sections_created = false;
          break;
        }
      }
      if (swap_sections_created) {
        size_t swap_total = 0;
        for (const auto& range : kGuestSwapRanges) {
          swap_total += range.length;
        }
        XELOGI(
            "Guest virtual memory ({} MB) will be backed by swap files at {} "
            "- excluded from the app commit budget",
            swap_total >> 20, cvars::winrt_guest_memory_swap_file);
        last_mapping_used_guest_swap_file = true;
      } else {
        DisableGuestMemorySwapFile();
      }
    }
    FileMappingHandle main_mapping = CreateFileMappingFromApp(
        INVALID_HANDLE_VALUE, nullptr, ULONG(protect), ULONG64(length),
        full_path.c_str());
    guest_swap_main_mapping = last_mapping_used_guest_swap_file
                                  ? main_mapping
                                  : kFileMappingHandleInvalid;
    return main_mapping;
  }
#endif  // XE_PLATFORM_WINRT
  return CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, ULONG(protect),
                                  ULONG64(length), full_path.c_str());
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
#if XE_PLATFORM_WINRT
  if (handle == guest_swap_main_mapping &&
      guest_swap_main_mapping != kFileMappingHandleInvalid) {
    for (auto& section : guest_swap_sections) {
      if (section) {
        CloseHandle(section);
        section = nullptr;
      }
    }
    for (auto& mapped : guest_swap_range_mapped) {
      mapped = false;
    }
    guest_swap_main_mapping = kFileMappingHandleInvalid;
  }
#endif  // XE_PLATFORM_WINRT
  CloseHandle(handle);
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD target_address_low = static_cast<DWORD>(file_offset);
  DWORD target_address_high = static_cast<DWORD>(file_offset >> 32);
  DWORD file_access = 0;
  switch (access) {
    case PageAccess::kReadOnly:
      file_access = FILE_MAP_READ;
      break;
    case PageAccess::kReadWrite:
      file_access = FILE_MAP_ALL_ACCESS;
      break;
    case PageAccess::kExecuteReadOnly:
      file_access = FILE_MAP_READ | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kExecuteReadWrite:
      file_access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kNoAccess:
    default:
      assert_unhandled_case(access);
      return nullptr;
  }
  return MapViewOfFileEx(handle, file_access, target_address_high,
                         target_address_low, length, base_address);
#else
  // VirtualAlloc2FromApp and MapViewOfFile3FromApp were added in 10.0.17134.0.
  // https://docs.microsoft.com/en-us/uwp/win32-and-com/win32-apis
  HANDLE process = GetCurrentProcess();
  void* placeholder = VirtualAlloc2FromApp(
      process, base_address, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
      PAGE_NOACCESS, nullptr, 0);
  if (!placeholder) {
    return nullptr;
  }
#if XE_PLATFORM_WINRT
  // For the guest mapping, views of the non-aliased ranges come from the
  // per-range swap-file sections when those are active. Falls back to the
  // pagefile-backed section per-view on failure (reusing the placeholder).
  if (handle == guest_swap_main_mapping &&
      guest_swap_main_mapping != kFileMappingHandleInvalid) {
    for (size_t i = 0; i < xe::countof(kGuestSwapRanges); ++i) {
      if (!guest_swap_sections[i] ||
          file_offset != kGuestSwapRanges[i].file_offset ||
          length != kGuestSwapRanges[i].length) {
        continue;
      }
      void* swap_mapping = MapViewOfFile3FromApp(
          guest_swap_sections[i], process, placeholder, 0, length,
          MEM_REPLACE_PLACEHOLDER, ULONG(ToWin32ProtectFlags(access)), nullptr,
          0);
      if (swap_mapping) {
        guest_swap_range_mapped[i] = true;
        static bool swap_view_logged[xe::countof(kGuestSwapRanges)] = {};
        if (!swap_view_logged[i]) {
          swap_view_logged[i] = true;
          XELOGI(
              "Guest memory range at file offset 0x{:X} ({} MB) mapped from "
              "its swap file",
              uint64_t(file_offset), uint64_t(length) >> 20);
        }
        return swap_mapping;
      }
      // A failed swap view means the OS charges commit for writable
      // file-backed views after all (observed on the console: even a 1 GB
      // view of a 1 GB section is refused with ERROR_NOT_ENOUGH_MEMORY, and a
      // previously mapped swap view correlates with commit exhaustion - err
      // 1455 on physical heap commits later). A partially swap-backed guest
      // both fails the technique AND steals commit, so disable the swap
      // entirely and fail this whole mapping pass - the caller retries at the
      // next base address with everything on the pagefile-backed section.
      XELOGW(
          "MapFileView: swap section view (offset=0x{:X}, length=0x{:X}) "
          "failed (err {}) - the OS charges commit for file-backed views; "
          "disabling the guest memory swap files and remapping from the "
          "pagefile-backed section only",
          uint64_t(file_offset), uint64_t(length), GetLastError());
      DisableGuestMemorySwapFile();
      VirtualFree(placeholder, 0, MEM_RELEASE);
      return nullptr;
    }
  }
#endif  // XE_PLATFORM_WINRT
  void* mapping = MapViewOfFile3FromApp(
      handle, process, placeholder, ULONG64(file_offset), length,
      MEM_REPLACE_PLACEHOLDER, ULONG(ToWin32ProtectFlags(access)), nullptr, 0);
  if (!mapping) {
    // Capped: the guest mapping probes dozens of base addresses in a loop and
    // the failure reason is the same at each of them, but a few occurrences
    // are needed to also see the swap-file -> pagefile fallback pass; the
    // error code is essential for telling why a section can't be view-mapped
    // at all (e.g. commit charge for views of a not-fully-allocated file).
    static int map_view_failures_logged = 0;
    if (map_view_failures_logged < 4) {
      ++map_view_failures_logged;
      XELOGW(
          "MapFileView: MapViewOfFile3FromApp(base={}, length=0x{:X}, "
          "offset=0x{:X}, protect=0x{:X}) failed (err {})",
          base_address, uint64_t(length), uint64_t(file_offset),
          uint32_t(ToWin32ProtectFlags(access)), GetLastError());
    }
    // MEM_RELEASE requires a zero size - with `length` the call fails with
    // ERROR_INVALID_PARAMETER and the placeholder leaks, permanently blocking
    // this base address for any later mapping attempt (this is how a failed
    // swap-file mapping pass used to also break the pagefile fallback).
    VirtualFree(placeholder, 0, MEM_RELEASE);
    return nullptr;
  }
  return mapping;
#endif
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  return UnmapViewOfFile(base_address) ? true : false;
}

}  // namespace memory
}  // namespace xe
