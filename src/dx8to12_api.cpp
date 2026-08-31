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

// Whether an RT backend is available: native D3D12 DXR, or the provisioned
// x64 helper used when NVIDIA's x86 D3D12 runtime reports Tier 0. LightingMode values that need raytracing
// (see config.h) are clamped by SetConfigValueInt when this is false, but a
// mod's UI should check this itself too -- to grey those choices out instead
// of letting the user pick a mode that silently gets clamped underneath them.
// Returns false (not just "unsupported") before device creation as well.
__declspec(dllexport) bool __cdecl Dx8to12_GetRaytracingSupported() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->raytracing_supported() : false;
}

// H4 result-channel accessors. The resource is a borrowed x86-local RGBA8
// Texture2D, never AddRef'd, and stays null until the first result upload.
// Protocol v13's fence accessor/value intentionally return null/zero because
// upload and mod callbacks are ordered on the same command list.
__declspec(dllexport) void* __cdecl Dx8to12_GetRtShadowOutputResource() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->rt_shadow_output_resource() : nullptr;
}

__declspec(dllexport) void* __cdecl Dx8to12_GetRtShadowDoneFence() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->rt_shadow_done_fence() : nullptr;
}

__declspec(dllexport) unsigned long long __cdecl
Dx8to12_GetRtShadowDoneFenceValue() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->rt_shadow_done_fence_value() : 0;
}

__declspec(dllexport) unsigned int __cdecl Dx8to12_GetRtShadowOutputWidth() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->rt_shadow_output_width() : 0;
}

__declspec(dllexport) unsigned int __cdecl Dx8to12_GetRtShadowOutputHeight() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->rt_shadow_output_height() : 0;
}

// DXGI_FORMAT value; currently DXGI_FORMAT_R8G8B8A8_UNORM (28), or UNKNOWN when
// the helper channel does not exist.
__declspec(dllexport) unsigned int __cdecl Dx8to12_GetRtShadowOutputFormat() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->rt_shadow_output_format() : 0;
}

// Level 1 mod-API scene metadata -- see MODDING.md's "Scene metadata for
// mods" section for the full contract (in particular: the depth buffer
// accessors return null/0 until Dx8to12_RequestDepthBufferAccess(true) has
// been called, and take one extra frame to become valid after that call).

__declspec(dllexport) bool __cdecl Dx8to12_RequestDepthBufferAccess(
    bool enable) {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->RequestDepthBufferAccess(enable) : false;
}

__declspec(dllexport) void *__cdecl Dx8to12_GetDepthBufferSrv() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->depth_buffer_srv_resource() : nullptr;
}

// D3D12_GPU_DESCRIPTOR_HANDLE.ptr for an SRV already created (in Dx8to12's
// own srv_heap(), which the render callback's command list already has
// bound -- see MODDING.md's "Descriptor heaps are shared state" note)
// against the resource Dx8to12_GetDepthBufferSrv returns. Usable directly
// with SetGraphicsRootDescriptorTable with no extra work on the mod's side.
__declspec(dllexport) unsigned long long __cdecl
Dx8to12_GetDepthBufferSrvGpuHandle() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->depth_buffer_srv_gpu_handle() : 0;
}

// DXGI_FORMAT to use if building your own SRV desc against the resource
// Dx8to12_GetDepthBufferSrv returns (e.g. if your own descriptor heap is
// bound instead of Dx8to12_GetDepthBufferSrvGpuHandle's).
__declspec(dllexport) unsigned int __cdecl Dx8to12_GetDepthBufferFormat() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->depth_buffer_srv_format() : 0;
}

// Combined view*projection matrix, row-major (D3D8 convention -- the same
// layout D3DMATRIX/SetTransform uses), as a flat 16-float array. Returns
// false if there's no device yet (out_matrix is left untouched).
__declspec(dllexport) bool __cdecl Dx8to12_GetViewProjMatrix(
    float out_matrix[16]) {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->GetViewProjMatrix(out_matrix) : false;
}

__declspec(dllexport) int __cdecl Dx8to12_GetActiveLightCount() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->GetActiveLightCount() : 0;
}

// index ranges over [0, Dx8to12_GetActiveLightCount()) -- see MODDING.md for
// the Dx8to12_LightInfo struct layout.
__declspec(dllexport) bool __cdecl Dx8to12_GetActiveLight(
    int index, Dx8to12_LightInfo *out) {
  if (!out) return false;
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->GetActiveLight(index, out) : false;
}

// Level 3 mod-API: inject a custom HLSL fragment into the generated
// fixed-function pixel shader -- see MODDING.md's "Pixel shader injection"
// section for the full contract (in particular: only one callback may be
// registered at a time, and a fragment that fails to compile is silently
// dropped for that shader permutation, logged, and does not affect the rest
// of the game's rendering).

__declspec(dllexport) bool __cdecl Dx8to12_RegisterPixelShaderInjection(
    Dx8to12_PixelShaderInjectionFn callback) {
  if (!callback) return false;
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->RegisterPixelShaderInjection(callback) : false;
}

__declspec(dllexport) bool __cdecl Dx8to12_UnregisterPixelShaderInjection(
    Dx8to12_PixelShaderInjectionFn callback) {
  if (!callback) return false;
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  return device ? device->UnregisterPixelShaderInjection(callback) : false;
}

// For a mod that wants to change what its already-registered callback
// produces at runtime without a full Unregister/Register cycle -- forces
// every fixed-function pixel shader to regenerate on next use.
__declspec(dllexport) bool __cdecl Dx8to12_InvalidatePixelShaderCache() {
  ::Dx8to12::Device *device = ::Dx8to12::GetCurrentDeviceForModApi();
  if (!device) return false;
  device->InvalidatePixelShaderCache();
  return true;
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
