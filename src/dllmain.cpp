// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>

#include <cstdio>
#include <iomanip>

#include "aixlog.hpp"
#include "config.h"

namespace {

#ifndef DX8TO12_DISABLE_LOGGING
#if defined(_M_IX86)
// Reads a DWORD from a possibly-invalid address without risking a second,
// nested crash inside the crash handler itself. Needs its own function (no
// C++ objects with destructors) because __try/__except can't be mixed with
// C++ object unwinding in the same function under /EHsc.
__declspec(noinline) bool SafeReadDword(const void *address, DWORD *out) {
  __try {
    *out = *reinterpret_cast<const DWORD *>(address);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
#endif

// Logs the module (and offset within it) that `address` falls in, if any --
// this is usually enough to map a crash address back to source via the PDB
// even without pulling in dbghelp for full symbolication.
void LogModuleAndOffset(const void *address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(address), &module)) {
    LOG(AixLog::Severity::fatal)
        << "  (address does not belong to any loaded module)\n";
    return;
  }
  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(module, module_path, sizeof(module_path));
  const uintptr_t offset =
      reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
  LOG(AixLog::Severity::fatal)
      << "  in " << module_path << "+0x" << std::hex << offset << std::dec
      << "\n";
}

// Vectored exception handler: logs a diagnosable record of the crash to
// log.txt before the process actually dies, even for exceptions the D3D12
// debug layer never sees (raw memory-access violations, stack overflows,
// etc). Always returns EXCEPTION_CONTINUE_SEARCH -- this does not attempt to
// recover or suppress the crash, only to leave a trail for the next time
// someone hits it.
LONG WINAPI LogCrashAndContinueSearch(EXCEPTION_POINTERS *info) {
  const EXCEPTION_RECORD *record = info->ExceptionRecord;
  switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case 0xC0000409L:  // STATUS_STACK_BUFFER_OVERRUN (/GS check failure).
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  // Thread ID is critical here: earlier investigation assumed this crash was
  // "the next thing that runs after CreateDevice returns" purely because of
  // log-line proximity, but that's only true if it's on the *same* thread.
  // If some other thread crashes on its own schedule (e.g. an asset-loading
  // or watchdog thread), the timing correlation would be coincidental.
  LOG(AixLog::Severity::fatal)
      << "=== Unhandled exception 0x" << std::hex << record->ExceptionCode
      << std::dec << " at " << record->ExceptionAddress << " on thread "
      << GetCurrentThreadId() << " ===\n";
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    const char *kind = record->ExceptionInformation[0] == 1   ? "writing"
                       : record->ExceptionInformation[0] == 8 ? "executing"
                                                              : "reading";
    LOG(AixLog::Severity::fatal)
        << "  " << kind << " address 0x" << std::hex
        << record->ExceptionInformation[1] << std::dec << "\n";
  }
  LogModuleAndOffset(record->ExceptionAddress);

  // The faulting instruction address itself is sometimes in unmapped memory
  // (e.g. a call through a corrupted/garbage function pointer) and can't be
  // mapped back to a module at all -- in that case the call stack is what
  // actually points at the culprit. Log each frame's address and the
  // module+offset it falls in; even without symbol resolution this is
  // usually enough to identify which function made the bad call.
  //
  // IMPORTANT: CaptureStackBackTrace() here would capture *this handler's
  // own* call stack (VEH dispatch -> LogCrashAndContinueSearch), not the
  // faulted thread's stack at the point of the crash -- it produced
  // identical-looking frames for completely different crashes before this
  // was fixed. The actual faulting register state is in
  // info->ContextRecord; walk the EBP chain from there instead.
#if defined(_M_IX86)
  LOG(AixLog::Severity::fatal) << "  call stack (EBP chain from Ebp=0x"
                               << std::hex << info->ContextRecord->Ebp
                               << std::dec << "):\n";
  DWORD ebp = info->ContextRecord->Ebp;
  for (int i = 0; i < 32 && ebp != 0; ++i) {
    DWORD return_address = 0, prev_ebp = 0;
    if (!SafeReadDword(reinterpret_cast<void *>(ebp + 4), &return_address))
      break;
    if (!SafeReadDword(reinterpret_cast<void *>(ebp), &prev_ebp)) break;
    LOG(AixLog::Severity::fatal)
        << "  #" << i << " " << reinterpret_cast<void *>(return_address)
        << "\n";
    LogModuleAndOffset(reinterpret_cast<void *>(return_address));
    if (prev_ebp <= ebp) break;  // Stack grows down; guard against loops.
    ebp = prev_ebp;
  }
#else
  void *frames[32] = {};
  const USHORT frame_count = CaptureStackBackTrace(0, 32, frames, nullptr);
  LOG(AixLog::Severity::fatal) << "  call stack (" << frame_count
                               << " frames, approximate -- see comment):\n";
  for (USHORT i = 0; i < frame_count; ++i) {
    LOG(AixLog::Severity::fatal) << "  #" << i << " " << frames[i] << "\n";
    LogModuleAndOffset(frames[i]);
  }
#endif

  return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // DX8TO12_DISABLE_LOGGING

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved);
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
#ifndef DX8TO12_DISABLE_LOGGING
      auto log_sink = AixLog::Log::init<AixLog::SinkFile>(
          AixLog::Severity::info, CURRENT_SOURCE_DIR "/log.txt");
      AddVectoredExceptionHandler(1 /* call first */,
                                  LogCrashAndContinueSearch);
      LOG(AixLog::Severity::info)
          << "DLL_PROCESS_ATTACH on thread " << GetCurrentThreadId() << "\n";
      // Build stamp. A log is only evidence about the code that produced it,
      // and "was the DLL under test actually the one the game loaded?" has
      // already cost a full debugging round-trip once. Compare this against
      // the build you just made before drawing any conclusion from a log.
      {
        // Report the DLL file's own last-write time, not __DATE__/__TIME__.
        // Those expand when *this translation unit* is compiled, so an
        // incremental build that only recompiles other files leaves the stamp
        // frozen at an older time -- which already once made a correctly
        // deployed DLL look like a stale one. The file timestamp cannot drift
        // from the binary that is actually loaded.
        wchar_t path[MAX_PATH] = {};
        WIN32_FILE_ATTRIBUTE_DATA attr = {};
        SYSTEMTIME st = {};
        if (GetModuleFileNameW(hModule, path, MAX_PATH) &&
            GetFileAttributesExW(path, GetFileExInfoStandard, &attr) &&
            FileTimeToSystemTime(&attr.ftLastWriteTime, &st)) {
          char stamp[64];
          snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                   st.wSecond);
          LOG(AixLog::Severity::info)
              << "Dx8to12 binary written " << stamp << " (compiled " __DATE__
                 " " __TIME__
#ifdef DX8TO12_ENABLE_VALIDATION
                 ", dev: validation+trace)"
#else
                 ", release: no validation)"
#endif
                 "\n";
        }
      }
#endif
      Dx8to12::LoadConfig(hModule);
#ifndef DX8TO12_DISABLE_LOGGING
      // FullTraceLog (dx8to12.ini): lower the sink's own severity threshold
      // from info to trace *after* LoadConfig has run, since LoadConfig
      // itself needs to log through the already-initialized sink above at
      // the normal (info) threshold. Sink::filter is public specifically to
      // allow this -- see AixLog::Filter::add_filter, which just overwrites
      // the "*" tag's threshold, so this is a real, in-place raise of the
      // verbosity floor, not a second sink or a reinit.
      if (Dx8to12::GetConfig().full_trace_log) {
        log_sink->filter.add_filter(AixLog::Severity::trace);
        LOG(AixLog::Severity::info)
            << "FullTraceLog enabled -- every IDirect3DDevice8 call will be "
               "logged via TRACE_ENTRY for the rest of this session. Expect "
               "a very large log.txt; turn this back off in dx8to12.ini "
               "once done.\n";
      }
#endif
      break;
    }
    case DLL_THREAD_ATTACH:
      // Cheap breadcrumb for the thread-ID cross-referencing above -- lets
      // us see whether a crash on some other thread is one the game spun up
      // itself vs. one of ours.
      LOG(AixLog::Severity::info)
          << "DLL_THREAD_ATTACH on thread " << GetCurrentThreadId() << "\n";
      break;
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
      break;
  }

  return TRUE;
}
