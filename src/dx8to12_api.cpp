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
#include "device.h"
#include "device_limits.h"

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

// Native D3D12 rendering access for mods (e.g. an ImGui-based trainer/
// overlay) that want to draw directly through the real device instead of
// needing a D3D9 (or other) compatibility shim -- see MODDING.md. All of
// these return null/false before device creation (i.e. before the game has
// called CreateDevice) or after device destruction.

__declspec(dllexport) void *__cdecl Dx8to12_GetD3D12Device() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->device() : nullptr;
}

__declspec(dllexport) void *__cdecl Dx8to12_GetD3D12CommandQueue() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->cmd_queue() : nullptr;
}

__declspec(dllexport) void *__cdecl Dx8to12_GetWindowHandle() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->window() : nullptr;
}

// Returns a DXGI_FORMAT value (as used by the swap chain's render target
// views), or 0 (DXGI_FORMAT_UNKNOWN) if there is no device yet.
__declspec(dllexport) int __cdecl Dx8to12_GetBackbufferFormat() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? static_cast<int>(device->backbuffer_format()) : 0;
}

__declspec(dllexport) int __cdecl Dx8to12_GetNumBackBuffers() {
  return ::Dx8to12::kNumBackBuffers;
}

// Duration of the most recently presented frame, in milliseconds, measured
// between consecutive Present calls -- i.e. exactly the frames the game
// actually put on screen. Returns 0 before the second presented frame (there
// is no interval to measure yet) or if there's no device.
//
// Provided because third-party overlays that hook Present/ExecuteCommandLists
// don't reliably interoperate with this shim's hand-rolled D3D12 usage (see
// ROADMAP.md), so a mod wanting a trustworthy framerate should read it from
// here rather than from such an overlay. Divide 1000 by this for FPS.
__declspec(dllexport) double __cdecl Dx8to12_GetLastFrameMs() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->last_frame_ms() : 0.0;
}

// Bumped once per Device::Reset() (window resize, fullscreen toggle, format
// change) -- back buffer resources, format, and dimensions are all recreated
// by Reset. Compare this against the value you last saw and rebuild any
// PSO/RTV-format-derived state (and re-fetch Dx8to12_GetBackbufferFormat)
// when it changes. Returns 0 if there's no device yet.
__declspec(dllexport) unsigned long long __cdecl
Dx8to12_GetSwapChainGeneration() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->swap_chain_generation() : 0;
}

// callback is invoked once per frame, right before the backbuffer
// transitions to PRESENT, with the still-open ID3D12GraphicsCommandList*
// (cast from the void* parameter) -- see Device::RegisterModRenderCallback
// in device.h for the full contract. Returns false if there's no device yet;
// register again after your hook observes device creation in that case.
__declspec(dllexport) bool __cdecl Dx8to12_RegisterRenderCallback(
    void(__cdecl *callback)(void *command_list)) {
  if (!callback) return false;
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  if (!device) return false;
  device->RegisterModRenderCallback(callback);
  return true;
}

__declspec(dllexport) bool __cdecl Dx8to12_UnregisterRenderCallback(
    void(__cdecl *callback)(void *command_list)) {
  if (!callback) return false;
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  if (!device) return false;
  device->UnregisterModRenderCallback(callback);
  return true;
}

}  // extern "C"
