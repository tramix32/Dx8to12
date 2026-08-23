// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>

#include <cstdio>
#include <iomanip>

#include "aixlog.hpp"

namespace {

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
  LOG(AixLog::Severity::fatal)
      << "=== Unhandled exception 0x" << std::hex << record->ExceptionCode
      << std::dec << " at " << record->ExceptionAddress << " ===\n";
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
  void *frames[32] = {};
  const USHORT frame_count = CaptureStackBackTrace(0, 32, frames, nullptr);
  LOG(AixLog::Severity::fatal) << "  call stack (" << frame_count
                               << " frames):\n";
  for (USHORT i = 0; i < frame_count; ++i) {
    LOG(AixLog::Severity::fatal) << "  #" << i << " " << frames[i] << "\n";
    LogModuleAndOffset(frames[i]);
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved);
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      AixLog::Log::init<AixLog::SinkFile>(AixLog::Severity::info,
                                          CURRENT_SOURCE_DIR "/log.txt");
      AddVectoredExceptionHandler(1 /* call first */,
                                  LogCrashAndContinueSearch);
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
      break;
  }

  return TRUE;
}
