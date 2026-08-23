#include "asserts.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <memory>

namespace Dx8to12 {
void MessageBoxFmt(unsigned int flags, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  constexpr size_t kMsgSize = 64 * 1024;
  std::unique_ptr<char[]> msg(new char[kMsgSize]);
  vsnprintf(msg.get(), kMsgSize, fmt, args);
  va_end(args);

  LOG(AixLog::Severity::error) << msg << "\n";

  int clicked = MessageBoxA(nullptr, msg.get(), nullptr, MB_TASKMODAL | flags);
  switch (clicked) {
    case IDRETRY:
      // Break into an attached debugger, then keep going -- only useful
      // interactively, under a debugger.
      __debugbreak();
      break;
    default:
      // Includes IDOK/IDABORT, and -- importantly -- IDIGNORE and anything
      // else (e.g. the dialog never being seen at all: fullscreen-exclusive
      // games can render it off-screen or behind the game surface, and a
      // stray keypress/click meant for the game can dismiss it via whatever
      // the default button happens to be). ASSERT is used throughout this
      // codebase for invariants that are genuinely unsafe to continue past
      // (e.g. ASSERT_HR after a D3D12 resource-creation call) -- silently
      // continuing past a violated one previously meant the *real* failure
      // could be swallowed here while the process kept running, only to
      // crash later somewhere unrelated with no diagnostic tying it back to
      // this. Always exit, matching FAIL's abort() -- there is no
      // "ignore and continue safely" case for this macro in practice.
      exit(1);
  }
}
}  // namespace Dx8to12