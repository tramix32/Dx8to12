// Exported C API other ASI mods can call (via GetProcAddress against this
// DLL's own module handle) to read/change Dx8to12 settings at runtime, on
// top of the dx8to12.ini file. See MODDING.md for the documented contract;
// this file is just the thin extern "C" wrapper around config.h's typed
// accessors.
//
// Deliberately a flat, versioned C API (not a COM interface or C++ vtable):
// this needs to be callable from an arbitrary mod DLL that has no reason to
// share this project's headers, C++ ABI, or even compiler. A mod loads this
// DLL's exports by name via GetProcAddress, same as it would load Windows
// API functions.
#include "config.h"

namespace {
constexpr int kApiVersion = 1;
}  // namespace

extern "C" {

// Mods should call this first and refuse to use the rest of the API if the
// returned value is higher than the version they were written against --
// this file only ever adds fields/functions, but a mod written today has no
// way to know that in advance.
__declspec(dllexport) int __cdecl Dx8to12_GetApiVersion() {
  return kApiVersion;
}

__declspec(dllexport) bool __cdecl Dx8to12_GetSettingInt(const char *key,
                                                          int *out_value) {
  if (!key || !out_value) return false;
  return ::Dx8to12::GetConfigValueInt(key, out_value);
}

__declspec(dllexport) bool __cdecl Dx8to12_SetSettingInt(const char *key,
                                                          int value) {
  if (!key) return false;
  return ::Dx8to12::SetConfigValueInt(key, value);
}

__declspec(dllexport) bool __cdecl Dx8to12_GetSettingFloat(const char *key,
                                                            float *out_value) {
  if (!key || !out_value) return false;
  return ::Dx8to12::GetConfigValueFloat(key, out_value);
}

__declspec(dllexport) bool __cdecl Dx8to12_SetSettingFloat(const char *key,
                                                            float value) {
  if (!key) return false;
  return ::Dx8to12::SetConfigValueFloat(key, value);
}

__declspec(dllexport) bool __cdecl Dx8to12_GetSettingBool(const char *key,
                                                           bool *out_value) {
  if (!key || !out_value) return false;
  return ::Dx8to12::GetConfigValueBool(key, out_value);
}

__declspec(dllexport) bool __cdecl Dx8to12_SetSettingBool(const char *key,
                                                           bool value) {
  if (!key) return false;
  return ::Dx8to12::SetConfigValueBool(key, value);
}

}  // extern "C"
