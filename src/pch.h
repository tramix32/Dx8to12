#pragma once

#include "aixlog.hpp"

#include <windows.h>

#include <d3d12.h>

// A filtered AixLog call still formats every argument before the sink drops
// the line. Release profiles instead remove the complete stream expression,
// including argument evaluation. release-mindebug uses a separate explicit
// writer for the few counters selected for a particular investigation.
#ifdef DX8TO12_DISABLE_LOGGING
#undef LOG
#define LOG(...)                                                          \
  for (bool dx8to12_skip_log_ = false; dx8to12_skip_log_;                 \
       dx8to12_skip_log_ = false)                                         \
  std::clog
#endif
