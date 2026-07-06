/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/exception_handler.h"

#include <string>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_WINRT
// The SDK hides the Vectored Exception Handler API behind the desktop partition,
// but the functions are exported by kernel32 and work in the App Container at
// runtime (Dev Mode sideload). Declare the exception-handler functions normally
// but avoid declaring the continue-handler import to prevent link-time
// dependencies; the continue-handler functions will be looked up dynamically
// when needed.
extern "C" {
WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(
  ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
WINBASEAPI ULONG WINAPI RemoveVectoredExceptionHandler(PVOID Handle);
}
#include <windows.h>
namespace {
using AddVectoredContinueHandlerFn =
  PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
using RemoveVectoredContinueHandlerFn = ULONG(WINAPI*)(PVOID);
inline AddVectoredContinueHandlerFn GetAddVectoredContinueHandler() {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  return k ? (AddVectoredContinueHandlerFn)
         GetProcAddress(k, "AddVectoredContinueHandler")
       : nullptr;
}
inline RemoveVectoredContinueHandlerFn GetRemoveVectoredContinueHandler() {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  return k ? (RemoveVectoredContinueHandlerFn)
         GetProcAddress(k, "RemoveVectoredContinueHandler")
       : nullptr;
}
}  // namespace
#endif  // XE_PLATFORM_WINRT

namespace xe {

// Handle of the added VectoredExceptionHandler.
void* veh_handle_ = nullptr;
// Handle of the added VectoredContinueHandler.
void* vch_handle_ = nullptr;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

static void CaptureThreadContext(HostThreadContext& thread_context,
                                 PCONTEXT ctx) {
#if XE_ARCH_AMD64
  thread_context.rip = ctx->Rip;
  thread_context.eflags = ctx->EFlags;
  std::memcpy(thread_context.int_registers, &ctx->Rax,
              sizeof(thread_context.int_registers));
  std::memcpy(thread_context.xmm_registers, &ctx->Xmm0,
              sizeof(thread_context.xmm_registers));
#elif XE_ARCH_ARM64
  thread_context.pc = ctx->Pc;
  thread_context.pstate = ctx->Cpsr;
  thread_context.sp = ctx->Sp;
  std::memcpy(thread_context.x, &ctx->X0, sizeof(thread_context.x));
  std::memcpy(thread_context.v, &ctx->V[0], sizeof(thread_context.v));
#endif
}

static void RestoreThreadContext(PCONTEXT ctx,
                                 const HostThreadContext& thread_context,
                                 const Exception& ex) {
#if XE_ARCH_AMD64
  ctx->Rip = thread_context.rip;
  ctx->EFlags = thread_context.eflags;
  uint32_t modified_register_index;
  uint16_t modified_int_registers_remaining = ex.modified_int_registers();
  while (xe::bit_scan_forward(modified_int_registers_remaining,
                              &modified_register_index)) {
    modified_int_registers_remaining &=
        ~(UINT16_C(1) << modified_register_index);
    (&ctx->Rax)[modified_register_index] =
        thread_context.int_registers[modified_register_index];
  }
  uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
  while (xe::bit_scan_forward(modified_xmm_registers_remaining,
                              &modified_register_index)) {
    modified_xmm_registers_remaining &=
        ~(UINT16_C(1) << modified_register_index);
    std::memcpy(&ctx->Xmm0 + modified_register_index,
                &thread_context.xmm_registers[modified_register_index],
                sizeof(vec128_t));
  }
#elif XE_ARCH_ARM64
  ctx->Pc = thread_context.pc;
  ctx->Cpsr = thread_context.pstate;
  ctx->Sp = thread_context.sp;
  std::memcpy(&ctx->X0, thread_context.x, sizeof(thread_context.x));
  std::memcpy(&ctx->V[0], thread_context.v, sizeof(thread_context.v));
#endif
}

#if XE_PLATFORM_WINRT
// Resolves an address to "module.dll+0xOFFSET" (or JIT/unknown) for crash
// logs; App Container allows GetModuleHandleExW/GetModuleFileNameW.
std::string DescribeCrashAddress(uint64_t address) {
  if (address >= 0xA0000000ull && address < 0xB0000000ull) {
    return fmt::format("JIT_code_cache+0x{:X}", address - 0xA0000000ull);
  }
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(uintptr_t(address)),
                         &module) &&
      module) {
    wchar_t path[MAX_PATH] = {0};
    if (GetModuleFileNameW(module, path, MAX_PATH)) {
      std::wstring wpath(path);
      size_t slash = wpath.find_last_of(L"\\/");
      std::wstring name =
          slash == std::wstring::npos ? wpath : wpath.substr(slash + 1);
      return fmt::format("{}+0x{:X}",
                         xe::to_utf8(std::u16string(name.begin(), name.end())),
                         address - uint64_t(uintptr_t(module)));
    }
  }
  return "unknown";
}

// Last-chance logger so crashes aren't silent on the Xbox (no debugger, no
// WER UI in an App Container game): record what died into the log file before
// the process is torn down. Registered with SetUnhandledExceptionFilter, so it
// only runs for exceptions nobody handled (no first-chance C++/guest noise).
LONG CALLBACK UnhandledExceptionLogger(PEXCEPTION_POINTERS ex_info) {
  const EXCEPTION_RECORD* record = ex_info->ExceptionRecord;
  uint64_t crash_address =
      uint64_t(reinterpret_cast<uintptr_t>(record->ExceptionAddress));
  XELOGE("=== UNHANDLED EXCEPTION code=0x{:08X} address=0x{:016X} ({}) ===",
         uint32_t(record->ExceptionCode), crash_address,
         DescribeCrashAddress(crash_address));
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    const char* op = record->ExceptionInformation[0] == 0   ? "read"
                     : record->ExceptionInformation[0] == 1 ? "write"
                     : record->ExceptionInformation[0] == 8 ? "execute (DEP)"
                                                            : "unknown";
    XELOGE("Access violation: {} of 0x{:016X}", op,
           uint64_t(record->ExceptionInformation[1]));
  }
#if XE_ARCH_AMD64
  const CONTEXT* ctx = ex_info->ContextRecord;
  XELOGE("RIP=0x{:016X} RSP=0x{:016X} RAX=0x{:016X} RCX=0x{:016X}", ctx->Rip,
         ctx->Rsp, ctx->Rax, ctx->Rcx);
  XELOGE("RDX=0x{:016X} R8=0x{:016X} R9=0x{:016X} RBX=0x{:016X}", ctx->Rdx,
         ctx->R8, ctx->R9, ctx->Rbx);
  // Poor man's stack trace: scan the live stack for return addresses that
  // resolve into modules or the JIT cache (no dbghelp in the App Container).
  {
    MEMORY_BASIC_INFORMATION mbi = {};
    const uint64_t* stack = reinterpret_cast<const uint64_t*>(ctx->Rsp);
    int hits = 0;
    for (size_t i = 0; i < 256 && hits < 12; ++i) {
      const uint64_t* slot = stack + i;
      if (!VirtualQuery(slot, &mbi, sizeof(mbi)) ||
          mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
          (mbi.Protect &
           (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE)) == 0) {
        break;
      }
      uint64_t value = *slot;
      if (value < 0x10000 || value > 0x7FFFFFFFFFFFull) {
        continue;
      }
      std::string desc = DescribeCrashAddress(value);
      if (desc != "unknown") {
        XELOGE("  stack[+0x{:03X}] = 0x{:016X} ({})", i * 8, value, desc);
        ++hits;
      }
    }
  }
#endif
  xe::FlushLog();
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // XE_PLATFORM_WINRT

LONG CALLBACK ExceptionHandlerCallback(PEXCEPTION_POINTERS ex_info) {
  // Visual Studio SetThreadName.
  if (ex_info->ExceptionRecord->ExceptionCode == 0x406D1388) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HostThreadContext thread_context;
  CaptureThreadContext(thread_context, ex_info->ContextRecord);

  // https://msdn.microsoft.com/en-us/library/ms679331(v=vs.85).aspx
  // https://msdn.microsoft.com/en-us/library/aa363082(v=vs.85).aspx
  Exception ex;
  switch (ex_info->ExceptionRecord->ExceptionCode) {
    case STATUS_ILLEGAL_INSTRUCTION:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case STATUS_ACCESS_VIOLATION: {
      Exception::AccessViolationOperation access_violation_operation;
      switch (ex_info->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
          access_violation_operation =
              Exception::AccessViolationOperation::kRead;
          break;
        case 1:
          access_violation_operation =
              Exception::AccessViolationOperation::kWrite;
          break;
        default:
          access_violation_operation =
              Exception::AccessViolationOperation::kUnknown;
          break;
      }
      ex.InitializeAccessViolation(
          &thread_context, ex_info->ExceptionRecord->ExceptionInformation[1],
          access_violation_operation);
    } break;
    default:
      // Unknown/unhandled type.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  for (size_t i = 0; i < xe::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
      RestoreThreadContext(ex_info->ContextRecord, thread_context, ex);
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!veh_handle_) {
    veh_handle_ = AddVectoredExceptionHandler(1, ExceptionHandlerCallback);
#if XE_PLATFORM_WINRT
    SetUnhandledExceptionFilter(UnhandledExceptionLogger);
#endif

    if (IsDebuggerPresent()) {
      // TODO(benvanik): do we need a continue handler if a debugger is
      // attached?
      // vch_handle_ = AddVectoredContinueHandler(1, ExceptionHandlerCallback);
    }
  }

  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < xe::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (veh_handle_) {
      RemoveVectoredExceptionHandler(veh_handle_);
      veh_handle_ = nullptr;
    }
    if (vch_handle_) {
#if XE_PLATFORM_WINRT
      auto remove_fn = GetRemoveVectoredContinueHandler();
      if (remove_fn) {
        remove_fn(vch_handle_);
      }
      vch_handle_ = nullptr;
#else
      RemoveVectoredContinueHandler(vch_handle_);
      vch_handle_ = nullptr;
#endif
    }
  }
}

}  // namespace xe
