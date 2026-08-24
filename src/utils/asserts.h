#pragma once

namespace Dx8to12 {
#ifdef __clang__
__attribute__((__format__(__printf__, 2, 0)))
#endif
void MessageBoxFmt(unsigned int flags, const char* fmt, ...);

#define FAIL(fmt, ...)                                                    \
  do {                                                                    \
    ::Dx8to12::MessageBoxFmt(MB_ABORTRETRYIGNORE,                         \
                             "Fatal error on %s:%d in function %s: " fmt, \
                             __FILE__, __LINE__, __func__, __VA_ARGS__);  \
    abort();                                                              \
  } while (0)

#define NOT_IMPLEMENTED() \
  FAIL("%s:%d: Feature not implemented (%s).", __FILE__, __LINE__, __func__)

#define ASSERT(expr)                                                    \
  do {                                                                  \
    if (!(expr)) {                                                      \
      ::Dx8to12::MessageBoxFmt(MB_ABORTRETRYIGNORE,                     \
                               "Assertion %s:%d failed:\n%s", __FILE__, \
                               __LINE__, #expr);                        \
    }                                                                   \
  } while (0)

#define ASSERT_TODO(expr, msg) ASSERT(expr)

#define ASSERT_HR(expr)     \
  do {                      \
    HRESULT _hr = expr;     \
    (void)_hr;              \
    ASSERT(SUCCEEDED(_hr)); \
  } while (0)

// Checks the result of expr. If it is not S_OK, its value is returned.
#define HR_OR_RETURN(expr)         \
  do {                             \
    HRESULT hr = expr;             \
    if (!SUCCEEDED(hr)) return hr; \
  } while (0)

#define VIRT_NOT_IMPLEMENTED \
  override { NOT_IMPLEMENTED(); }

inline void TraceFunctionHelper(std::ostream& out) {}
template <typename T>
inline void TraceFunctionHelper(std::ostream& out, T arg) {
  out << arg;
}

template <typename Arg, typename... Args>
inline void TraceFunctionHelper(std::ostream& out, Arg arg, Args... args) {
  // out << std::forward<Arg>(arg) << ", ";
  out << arg << ", ";
  TraceFunctionHelper(out, args...);
}

#define TRACE_ENTRY_LEVEL TRACE

// AixLog's severity filtering happens per-sink at dispatch time, not at the
// call site -- so even though the file sink's threshold (Severity::info,
// see dllmain.cpp) already discards TRACE_ENTRY_LEVEL output, each call
// still pays for building the formatted line (three separate LOG(...)
// dispatches per invocation) before it gets thrown away. Every single
// IDirect3DDevice8 method call goes through TRACE_ENTRY, so on a game
// issuing thousands of draws/state-changes per frame this is a real,
// silently-paid cost for output nobody ever sees at the current threshold.
// Compiles away entirely in a non-validation (release) build; still works
// normally in a dev build if someone lowers the sink threshold to actually
// see it.
#ifdef DX8TO12_ENABLE_VALIDATION
#define TRACE_ENTRY(...)                                      \
  do {                                                        \
    LOG(TRACE_ENTRY_LEVEL) << __func__ << "(";                \
    TraceFunctionHelper(LOG(TRACE_ENTRY_LEVEL), __VA_ARGS__); \
    LOG(TRACE_ENTRY_LEVEL) << ");\n";                         \
  } while (0)
#else
#define TRACE_ENTRY(...) \
  do {                   \
  } while (0)
#endif

}  // namespace Dx8to12
