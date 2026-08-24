#pragma once

#include <windows.h>

#include <string>

// User- and mod-facing configuration for Dx8to12, backed by an INI file
// (dx8to12.ini, next to d3d8.dll) plus a small exported C API
// (dx8to12_api.cpp) that other ASI mods can call at runtime to read/change
// settings in the same process. See MODDING.md for the full, documented
// contract -- this header is the *internal* representation only; mods must
// go through the exported Dx8to12_* functions, not this struct directly
// (there's no ABI stability promise on Config's layout).
namespace Dx8to12 {

struct Config {
  // -1 disables the override entirely (use whatever the app itself requests
  // via SetSamplerState/D3DTSS_MAXANISOTROPY). 1-16 forces that anisotropy
  // level on every sampler regardless of what the app asked for.
  int anisotropic_override = -1;

  // MSAA sample count for the backbuffer/render targets. 1 = disabled.
  // Valid: 1, 2, 4, 8. NOT YET IMPLEMENTED -- CreatePSO/swap chain creation
  // don't consume this yet (see ROADMAP.md); reserved so the INI/API surface
  // is stable once it is.
  int msaa_samples = 1;

  // 0.0 disables. NOT YET IMPLEMENTED -- reserved for a future
  // contrast-adaptive-sharpening post-process pass.
  float sharpen_strength = 0.0f;

  // NOT YET IMPLEMENTED -- reserved for a future higher-precision
  // depth-stencil format switch (see the D3DRS_ZBIAS gap in ROADMAP.md).
  bool high_precision_depth = false;

  // Every field above, by name, for the generic string-keyed Get/Set API in
  // dx8to12_api.cpp and the INI parser in config.cpp. Keep this in sync when
  // adding a field -- see the kFields table at the top of config.cpp.
};

// Process-wide singleton. Safe to call before LoadConfig() -- returns
// defaults until LoadConfig() has run once, so this is always usable at
// static-init time if something ever needs it that early.
Config &GetConfig();

// Reads dx8to12.ini from next to `dll_module`'s own file (i.e. next to
// wherever d3d8.dll was actually loaded from -- the game's install
// directory), applying any keys it finds on top of the defaults above.
// Missing file or missing/unrecognized keys are not errors -- an absent INI
// just means "use every default". Called once from DllMain
// (DLL_PROCESS_ATTACH), before any device/rendering setup.
void LoadConfig(HMODULE dll_module);

// String-keyed accessors backing the exported Dx8to12_GetSetting*/
// SetSetting* API (dx8to12_api.cpp) -- shared so both the INI parser and the
// runtime mod API stay consistent about what keys exist and how they're
// typed. Returns false for an unrecognized key or a type mismatch (e.g.
// asking for "MSAASamples" as a float).
bool GetConfigValueInt(const std::string &key, int *out_value);
bool SetConfigValueInt(const std::string &key, int value);
bool GetConfigValueFloat(const std::string &key, float *out_value);
bool SetConfigValueFloat(const std::string &key, float value);
bool GetConfigValueBool(const std::string &key, bool *out_value);
bool SetConfigValueBool(const std::string &key, bool value);

}  // namespace Dx8to12
