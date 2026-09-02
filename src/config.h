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
  // don't consume this yet (see plan/ROADMAP.md); reserved so the INI/API surface
  // is stable once it is.
  int msaa_samples = 1;

  // 0.0 disables. NOT YET IMPLEMENTED -- reserved for a future
  // contrast-adaptive-sharpening post-process pass.
  float sharpen_strength = 0.0f;

  // NOT YET IMPLEMENTED -- reserved for a future higher-precision
  // depth-stencil format switch (see the D3DRS_ZBIAS gap in plan/ROADMAP.md).
  bool high_precision_depth = false;

  // Lowers the log file's sink severity threshold from Severity::info to
  // Severity::trace, i.e. turns on TRACE_ENTRY's full per-call D3D8 API
  // trace (function name + every argument, for essentially every
  // IDirect3DDevice8 method) for the whole session. Off by default because
  // the volume is enormous (dllmain.cpp measured ~15k SetTexture calls/sec
  // alone in a busy scene) -- meant to be turned on only for a short,
  // deliberately narrow repro session chasing a specific bug, then turned
  // back off. Only takes effect in a DX8TO12_ENABLE_VALIDATION (dev) build;
  // TRACE_ENTRY compiles to nothing at all in release regardless of this
  // setting.
  bool full_trace_log = false;

  // How scene lighting is computed. Values ramp from "runs on anything" to
  // "needs a DXR-capable GPU", so mods (e.g. a settings menu) should let the
  // player choose based on their hardware rather than assuming the top mode
  // is always best:
  //   0 = Vertex      -- stock D3D8 fixed-function per-vertex lighting.
  //   1 = PerPixel     -- same lighting model, computed per pixel instead.
  //   2 = RTShadows    -- NOT YET IMPLEMENTED. Per-pixel lighting +
  //                       raytraced shadow visibility per light.
  //   3 = RTReflections -- NOT YET IMPLEMENTED. Per-pixel lighting +
  //                       raytraced reflections on reflective surfaces.
  //   4 = RTFullGI     -- NOT YET IMPLEMENTED. Full raytraced multi-bounce
  //                       global illumination.
  // SetConfigValueInt clamps 2-4 down to 1 (logging why) when
  // Device::raytracing_supported() is false -- there is no native DXR device
  // or provisioned x64 helper backend to run any RT mode on.
  int lighting_mode = 0;

  // --- Temporal anti-aliasing / upscaling -------------------------------
  //
  // These are the DLAA/DLSS pipeline's runtime controls. They exist as
  // settings rather than build flags specifically so a mod (or the player,
  // via the INI) can turn them on without a special build -- see MODDING.md.
  // The DX8TO12_* CMake options only decide whether the code is compiled in
  // at all; with it compiled in, these decide whether it runs.

  //   0 = Off
  //   1 = DLAA  -- native-resolution temporal AA. Working.
  //   2 = DLSS  -- accepted, but currently behaves as DLAA: the scene is
  //               still rendered at output resolution, so there is nothing to
  //               upscale from. Rendering below output resolution is a
  //               separate piece of work (the viewport is currently one
  //               global value mixing "what the game set" with "what D3D12
  //               gets"), which is why this is not simply a mode switch.
  // Turning this on implies temporal_jitter and motion_vectors: a temporal
  // upscaler is wrong without both, so enabling it enables them.
  int temporal_aa = 0;

  // Fraction of the output resolution the scene is rendered at, 0.5 to 1.0.
  // 1.0 is DLAA (same resolution in and out); below that the upscaler is
  // reconstructing detail rather than only anti-aliasing it, which is where
  // the performance actually comes from. Only has an effect with temporal_aa
  // on -- without an upscaler this would just render the game smaller.
  //
  // The conventional DLSS ratios: 0.667 Quality, 0.58 Balanced,
  // 0.5 Performance.
  float render_scale = 1.0f;

  // --- Upscaler convention tuning ---------------------------------------
  //
  // Two values that cannot be derived from anything the game provides; they
  // only match (or fail to match) what the upscaler expects. Wrong values do
  // not produce an error -- they produce a subtly wrong image, which is why
  // they are adjustable at runtime rather than compiled in.
  //
  // Symptom to tune against: rotate the camera against a flat sky. A visible
  // seam or rotating outline is the boundary of the region the upscaler
  // decided it had no history for, and means these are wrong.

  // Multiplies the motion vector scale. The pass writes vectors in render
  // pixels and they are normalised by 1/resolution, which is what Streamline
  // documents for pixel-space vectors. 2.0 tests the [-1,1] convention.
  float mvec_scale_multiplier = 1.0f;

  // Sign of the jitter offset handed to the upscaler. The projection is
  // offset by +jitter, so the upscaler may want +jitter or -jitter depending
  // on whether it defines the value as the applied shift or the correction
  // for it. -1 tests the other one.
  float jitter_sign = 1.0f;

  // Transposes the camera matrices before handing them to the upscaler.
  // This codebase is row-vector (v * M) throughout, matching D3D8; if the
  // upscaler reads them as column-vector, every one is transposed and history
  // gets reprojected by the wrong transform. The giveaway is geometric --
  // content appearing sheared or rotated into a skewed quad -- as opposed to
  // the blurring or ghosting a wrong scale produces.
  bool transpose_upscaler_matrices = false;

  // Sub-pixel camera offset per frame (Halton). On its own this only makes
  // the image shimmer -- it is an *input* to a temporal upscaler, exposed
  // separately so a mod can drive its own.
  bool temporal_jitter = false;

  // Camera motion vectors reconstructed from depth into an offscreen
  // R16G16_FLOAT target. Also an upscaler input; also useful on its own to a
  // mod doing motion blur or temporal effects of its own.
  bool motion_vectors = false;

  // Draws those motion vectors as false colour over the right half of the
  // screen. Diagnostic; implies motion_vectors.
  bool motion_vector_debug = false;

  // Every field above, by name, for the generic string-keyed Get/Set API in
  // dx8to12_api.cpp and the INI parser in config.cpp. Keep this in sync when
  // adding a field -- see the kFields table at the top of config.cpp.
};

// Process-wide singleton. Safe to call before LoadConfig() -- returns
// defaults until LoadConfig() has run once, so this is always usable at
// static-init time if something ever needs it that early.
Config &GetConfig();

// Names and types of every setting, in INI order. Both the INI parser and
// SaveConfig walk this rather than repeating the key list, which is what the
// comment on Config above has always promised.
enum class ConfigFieldType { Int, Float, Bool };
struct ConfigField {
  const char *name;
  ConfigFieldType type;
  // Written back to the INI by SaveConfig. False for settings that are
  // diagnostic-only and would be a nasty surprise to find still enabled in a
  // later session (see kFields in config.cpp).
  bool persist;
};
const ConfigField *ConfigFields(size_t *out_count);

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

// Writes the current settings back into dx8to12.ini, so a change a mod makes
// at runtime survives into the next session -- the INI and the mod API are two
// doors onto one state, not two separate states.
//
// Rewrites in place: existing lines keep their position, their comments and
// their spelling of the key, unknown keys are left untouched, and only missing
// keys are appended. Blowing the file away and regenerating it would silently
// eat a modder's own notes.
//
// No-op before LoadConfig has run (there is no path to write to yet).
void SaveConfig();

// SetConfigValue* calls this whenever a value actually changes. The write
// itself is deferred: a mod animating a slider would otherwise rewrite the
// file every frame. FlushConfigIfDirty does the real work, rate-limited, and
// is called once per presented frame plus on DLL unload.
void MarkConfigDirty();
void FlushConfigIfDirty(bool force);

}  // namespace Dx8to12
