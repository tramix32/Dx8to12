# Dx8to12 modding/configuration API

Two ways to control Dx8to12's rendering settings: a plain INI file for
end-user configuration, and an exported C API other mod DLLs can call at
runtime. Both read/write the same underlying settings.

## `dx8to12.ini`

Place a file named `dx8to12.ini` next to `d3d8.dll` (i.e. in the game's
install directory). It's read once, at DLL load (`DLL_PROCESS_ATTACH`),
before any device or rendering setup happens. If the file doesn't exist,
every setting uses its default and nothing breaks -- the INI is entirely
optional.

Format: flat `key=value` pairs, one per line. `;` and `#` start a comment
(both full-line and trailing after a value). Section headers (`[Section]`)
are accepted for readability but not required -- every key is globally
unique regardless of which section (if any) it's written under. Keys are
case-insensitive.

```ini
; dx8to12.ini -- lines starting with ; or # are comments.

[Rendering]
; -1 = don't override (use whatever the game itself requests). 1-16 = force
; this anisotropic filtering level on every texture sampler regardless of
; what the game asked for.
AnisotropicOverride=-1

; MSAA sample count. Valid: 1 (off), 2, 4, 8.
; NOT YET IMPLEMENTED as of this writing -- accepted and stored, but nothing
; in the renderer consumes it yet. Reserved so this file's format doesn't
; need to change once it is.
MSAASamples=1

; Contrast-adaptive sharpening strength, 0.0 (off) - 1.0.
; NOT YET IMPLEMENTED -- same as above.
SharpenStrength=0.0

; true/false or 1/0. NOT YET IMPLEMENTED -- reserved for a future
; higher-precision depth-stencil format switch.
HighPrecisionDepth=false

; How scene lighting is computed:
;   0 = Vertex        -- stock D3D8 fixed-function per-vertex lighting.
;   1 = PerPixel       -- same lighting model, computed per pixel instead.
;   2 = RTShadows      -- NOT YET IMPLEMENTED. Per-pixel lighting +
;                         raytraced shadow visibility per light.
;   3 = RTReflections  -- NOT YET IMPLEMENTED. Per-pixel lighting +
;                         raytraced reflections on reflective surfaces.
;   4 = RTFullGI       -- NOT YET IMPLEMENTED. Full raytraced multi-bounce
;                         global illumination.
; 2-4 need a DXR-capable GPU -- see Dx8to12_GetRaytracingSupported below.
; Requesting one without support silently falls back to PerPixel (1) and logs
; why. Since dx8to12.ini is read before device creation, a raytracing mode
; requested here always falls back at startup; set it after startup instead,
; via Dx8to12_SetSettingInt once Dx8to12_GetRaytracingSupported returns true.
LightingMode=0

[TemporalAA]
; Temporal anti-aliasing / upscaling. Needs an NVIDIA RTX GPU and the files
; dx8to12_dlaa_helper.exe + sl.*.dll + nvngx_dlss.dll next to d3d8.dll; with
; any of them missing the game runs exactly as if this were 0, and says so in
; log.txt.
;   0 = Off
;   1 = DLAA -- native-resolution temporal AA. Working.
;   2 = DLSS -- accepted, but currently behaves as DLAA: the scene is still
;               rendered at output resolution, so there is nothing to upscale
;               from yet.
; Objects that move independently of the camera (cars, pedestrians) ghost
; slightly: the motion vectors are reconstructed from depth and describe
; camera movement only.
; Turning this on also turns on TemporalJitter and MotionVectors: a temporal
; upscaler without both does not degrade gracefully, it produces a blurred,
; ghosting image that looks like a bug in the upscaler rather than a missing
; input.
TemporalAA=0

; Sub-pixel camera offset per frame (Halton). On its own this only makes the
; image shimmer -- it is an *input* to a temporal upscaler, exposed separately
; so a mod can drive its own.
TemporalJitter=false

; Camera motion vectors reconstructed from the depth buffer into an offscreen
; R16G16_FLOAT target. Also an upscaler input; also useful on its own to a mod
; doing motion blur or its own temporal effect.
; Camera-only: cars and pedestrians get the vector of the geometry behind
; them, so anything that moves independently of the camera will ghost.
MotionVectors=false

; Draws those motion vectors as false colour over the RIGHT HALF of the screen
; (the left half stays playable). Diagnostic; implies MotionVectors.
; Not written back to the INI -- see "Settings that are not persisted" below.
MotionVectorDebug=false
```

An unrecognized key, or a value out of the documented range, is logged as an
error to `log.txt` and otherwise ignored (that one line's setting keeps its
default; the rest of the file still applies normally).

### The INI and the mod API are one state, not two

A setting changed at runtime through `Dx8to12_SetSetting*` is **written back
to `dx8to12.ini`**, so it survives into the next session. A mod with a
settings menu therefore doesn't need to manage its own config file: it sets
the value, and the player's choice persists.

The file is rewritten **in place**. Existing lines keep their position, their
trailing comments and the spelling of the key as you wrote it; keys Dx8to12
doesn't recognize are left completely untouched (so it is safe to keep your
own mod's settings in the same file); only keys that were missing get
appended, under a `; Written by Dx8to12.` marker.

Writes are deferred and rate-limited to at most one per second, plus a final
write when the DLL unloads. A mod animating a value in a slider therefore
costs one file write per second, not one per frame. There is no need -- and no
API -- to ask for a save explicitly.

#### Settings that are not persisted

Two settings are deliberately never written back, because finding them still
enabled in a later session would look like a broken game rather than a
remembered preference:

| Setting | Why |
|---|---|
| `FullTraceLog` | Produces an enormous `log.txt` (measured ~15k `SetTexture` calls/sec). |
| `MotionVectorDebug` | Paints false colour over half the screen. |

They still work normally at runtime, and still take effect if *you* write them
into the INI by hand. They just don't get written there on your behalf.

## Exported API for other mods

Other ASI mods (or any DLL loaded into the same process) can read and change
these same settings at runtime by calling exported functions from `d3d8.dll`
directly -- there's no need to also parse the INI file yourself, and no need
to link against this project. Load the functions by name via
`GetProcAddress`, the same way you'd resolve a Windows API function from a
DLL you don't have an import library for:

```cpp
#include <windows.h>

// Match these signatures exactly -- see "Function reference" below.
using Dx8to12_GetApiVersionFn = int(__cdecl *)();
using Dx8to12_GetSettingIntFn = bool(__cdecl *)(const char *key, int *out_value);
using Dx8to12_SetSettingIntFn = bool(__cdecl *)(const char *key, int value);

void Example() {
  HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
  if (!d3d8) return;  // Not loaded (yet) -- e.g. called before device creation.

  auto get_version =
      reinterpret_cast<Dx8to12_GetApiVersionFn>(GetProcAddress(d3d8, "Dx8to12_GetApiVersion"));
  if (!get_version || get_version() > 1) {
    // Refuse to use the rest of the API against an incompatible version --
    // see "Versioning" below.
    return;
  }

  auto set_int =
      reinterpret_cast<Dx8to12_SetSettingIntFn>(GetProcAddress(d3d8, "Dx8to12_SetSettingInt"));
  if (set_int) set_int("AnisotropicOverride", 16);
}
```

`d3d8.dll` is guaranteed to already be loaded into the process by the time
any DX8 rendering happens (the game itself loads it to get `Direct3DCreate8`),
so any mod hook that runs after the game has created its `IDirect3D8`/
`IDirect3DDevice8` can safely assume `GetModuleHandleA("d3d8.dll")` succeeds.

### Versioning

`Dx8to12_GetApiVersion()` returns an integer, currently `1`. This number
only ever increases, and only when a *breaking* change is made (a function
signature changes, or a function is removed) -- adding a new setting or a
new function is not a breaking change and does not bump it. Call this first
and compare against the version you developed against; if the DLL reports a
higher version than you expect, either fall back to a documented-compatible
version's behavior or refuse to use the API. There is no plan to ever bump
this in practice unless a real compatibility problem is found; it exists as
a safety net, not because breaking changes are expected.

### Function reference

All functions use the `__cdecl` calling convention. All return `bool`
(except `Dx8to12_GetApiVersion`) indicating whether the call succeeded --
`false` means the key name wasn't recognized, or (for a `Set*` call) the
value was outside the documented valid range; the setting is left
unchanged in that case.

| Function | Signature |
|---|---|
| `Dx8to12_GetApiVersion` | `int __cdecl ()` |
| `Dx8to12_GetSettingInt` | `bool __cdecl (const char *key, int *out_value)` |
| `Dx8to12_SetSettingInt` | `bool __cdecl (const char *key, int value)` |
| `Dx8to12_GetSettingFloat` | `bool __cdecl (const char *key, float *out_value)` |
| `Dx8to12_SetSettingFloat` | `bool __cdecl (const char *key, float value)` |
| `Dx8to12_GetSettingBool` | `bool __cdecl (const char *key, bool *out_value)` |
| `Dx8to12_SetSettingBool` | `bool __cdecl (const char *key, bool value)` |
| `Dx8to12_GetRaytracingSupported` | `bool __cdecl ()` |
| `Dx8to12_GetRtShadowOutputResource` | `void * __cdecl ()` |
| `Dx8to12_GetRtShadowDoneFence` | `void * __cdecl ()` |
| `Dx8to12_GetRtShadowDoneFenceValue` | `unsigned long long __cdecl ()` |
| `Dx8to12_GetRtShadowOutputWidth` | `unsigned int __cdecl ()` |
| `Dx8to12_GetRtShadowOutputHeight` | `unsigned int __cdecl ()` |
| `Dx8to12_GetRtShadowOutputFormat` | `unsigned int __cdecl ()` |

`key` is one of the names documented in the `dx8to12.ini` section above
(`AnisotropicOverride`, `MSAASamples`, `SharpenStrength`,
`HighPrecisionDepth`, `LightingMode`), case-insensitive, matched against the
correctly-typed function (e.g. `HighPrecisionDepth` only responds to the
`*Bool` functions).

**`Dx8to12_GetRaytracingSupported`** reports whether the adapter supports DXR
(`D3D12_RAYTRACING_TIER_1_0` or higher), checked once at device creation.
Returns `false` before device creation too. A mod's settings UI should call
this to grey out `LightingMode` values 2-4 rather than letting the player pick
one that `Dx8to12_SetSettingInt` would silently clamp back down to `PerPixel`
(1) anyway.

Changes made through this API are **in-memory only for the current process
session** -- they do not get written back to `dx8to12.ini`. A mod that wants
its change to persist across game restarts needs to write the INI file
itself; this API is for live, in-session control (e.g. a mod's own settings
menu), not for editing the user's config file on their behalf.

## Native D3D12 rendering access for mods

Dx8to12 doesn't emulate D3D8 on top of D3D9 or D3D11 -- it talks directly to
D3D12. That means a companion mod (an overlay, an ImGui-based trainer, a
debug HUD) can render natively through the *real* D3D12 device and command
queue this API exposes, instead of needing some other graphics API
underneath it. This is deliberately the escape hatch for the class of trainer
that otherwise depends on a D3D8-to-D3D9 wrapper (e.g. d3d8to9) purely to get
at a real device object to hook -- with this API there's a real device to
get directly, no wrapper needed.

```cpp
#include <windows.h>
#include <d3d12.h>

using Dx8to12_GetD3D12DeviceFn = void *(__cdecl *)();
using Dx8to12_GetD3D12CommandQueueFn = void *(__cdecl *)();
using Dx8to12_GetWindowHandleFn = void *(__cdecl *)();
using Dx8to12_GetBackbufferFormatFn = int(__cdecl *)();
using Dx8to12_GetNumBackBuffersFn = int(__cdecl *)();
using Dx8to12_RenderCallbackFn = void(__cdecl *)(void *command_list);
using Dx8to12_RegisterRenderCallbackFn = bool(__cdecl *)(Dx8to12_RenderCallbackFn);
using Dx8to12_UnregisterRenderCallbackFn = bool(__cdecl *)(Dx8to12_RenderCallbackFn);

void __cdecl MyRenderHook(void *command_list_voidptr) {
  auto *cmd_list = static_cast<ID3D12GraphicsCommandList *>(command_list_voidptr);
  // Record ImGui (or your own) draw commands into cmd_list here. The
  // backbuffer is already bound as the render target and already contains
  // the game's fully rendered frame -- anything recorded here draws on top
  // of it, in the same command list, before Present().
}

void Example(HMODULE d3d8) {
  auto register_cb = reinterpret_cast<Dx8to12_RegisterRenderCallbackFn>(
      GetProcAddress(d3d8, "Dx8to12_RegisterRenderCallback"));
  if (register_cb) register_cb(MyRenderHook);
}
```

### Function reference (rendering)

| Function | Signature | Notes |
|---|---|---|
| `Dx8to12_GetD3D12Device` | `void* __cdecl ()` | Returns `ID3D12Device*`, or null before device creation. |
| `Dx8to12_GetD3D12CommandQueue` | `void* __cdecl ()` | Returns `ID3D12CommandQueue*`, or null before device creation. |
| `Dx8to12_GetWindowHandle` | `void* __cdecl ()` | Returns the game's `HWND`, or null before device creation. |
| `Dx8to12_GetBackbufferFormat` | `int __cdecl ()` | Returns a `DXGI_FORMAT` value (0/`DXGI_FORMAT_UNKNOWN` before device creation). |
| `Dx8to12_GetNumBackBuffers` | `int __cdecl ()` | Number of buffers in the swap chain (currently always 2). |
| `Dx8to12_RegisterRenderCallback` | `bool __cdecl (void(__cdecl*)(void*))` | Registers a per-frame render callback; see below. Returns `false` if there's no device yet. |
| `Dx8to12_UnregisterRenderCallback` | `bool __cdecl (void(__cdecl*)(void*))` | Removes a previously registered callback. |
| `Dx8to12_GetSwapChainGeneration` | `unsigned long long __cdecl ()` | Counter bumped once per device `Reset()`; see below. Returns 0 if there's no device yet. |
| `Dx8to12_GetLastFrameMs` | `double __cdecl ()` | Milliseconds between the last two presented frames; see below. Returns 0 before the second frame or if there's no device. |
| `Dx8to12_GetRtShadowOutputResource` | `void* __cdecl ()` | Borrowed x86-local `ID3D12Resource*` (`Texture2D`); null until the first completed result upload. |
| `Dx8to12_GetRtShadowDoneFence` | `void* __cdecl ()` | Legacy compatibility export. Protocol v13 returns null because upload and callbacks share one command list. |
| `Dx8to12_GetRtShadowDoneFenceValue` | `unsigned long long __cdecl ()` | Legacy compatibility export. Protocol v13 returns 0; do not queue-wait when the texture itself is valid. |
| `Dx8to12_GetRtShadowOutputWidth` | `unsigned int __cdecl ()` | Current mask width (320). Returns 0 before the first upload. |
| `Dx8to12_GetRtShadowOutputHeight` | `unsigned int __cdecl ()` | Current mask height (180). Returns 0 before the first upload. |
| `Dx8to12_GetRtShadowOutputFormat` | `unsigned int __cdecl ()` | Protocol v14: `DXGI_FORMAT_R8G8B8A8_UNORM` (R shadow, G reflection hit, B diffuse visibility/GI, A geometry validity). Returns 0 when unavailable. |

**Measuring framerate**: `Dx8to12_GetLastFrameMs()` returns the interval
between the last two `Present` calls -- the frames the game actually put on
screen -- so `1000.0 / Dx8to12_GetLastFrameMs()` is the current FPS. Prefer
this over a third-party FPS overlay: overlays that work by hooking
`Present`/`ExecuteCommandLists` (RivaTuner/MSI Afterburner, and similar) do
not reliably interoperate with this shim's hand-rolled D3D12 usage, and have
been observed both misreporting the framerate and rendering corrupted
overlay elements on top of the game. Reading the number
here and drawing it from your own render callback avoids that entirely.

**Render callback contract**: once registered, your callback is invoked once
per presented frame, right before the backbuffer transitions to
`D3D12_RESOURCE_STATE_PRESENT`, with that frame's still-open
`ID3D12GraphicsCommandList*` (passed as `void*` to keep the API C-ABI-safe --
cast it back to `ID3D12GraphicsCommandList*`). At that point the backbuffer
is **guaranteed** to already be bound as the active render target (in
`D3D12_RESOURCE_STATE_RENDER_TARGET`, via `OMSetRenderTargets`) and holding
the game's fully rendered frame so far -- Dx8to12 forces this even on a
frame the game itself never drew anything into (e.g. a blank loading-screen
frame, or the first frame right after a device `Reset()`), so a callback can
always just record its own draw commands (e.g. `ImGui_ImplDX12_RenderDrawData`)
straight into the same command list and they composite on top, before
`Present()` is called. Don't call `Close()`/`ExecuteCommandLists()`/
`Present()` yourself from inside the callback -- Dx8to12 owns the command
list's lifecycle; just record commands into it.

**Descriptor heaps are shared state -- bind your own before using it**: the
command list handed to your callback already has Dx8to12's own SRV/sampler
descriptor heaps bound (via `SetDescriptorHeaps`) from its own rendering
earlier in the frame. A D3D12 command list can only have one CBV/SRV/UAV heap
and one sampler heap bound at a time, so if your callback uses its own
descriptor heap for its own resources (e.g. `ImGui_ImplDX12_Init`'s SRV heap
for the font atlas), you **must** call `SetDescriptorHeaps` with your own
heap(s) yourself before issuing any draw or `SetGraphicsRootDescriptorTable`
call that references a handle from it -- don't assume Dx8to12's heaps happen
to work, and don't assume the ImGui DX12 backend does this for you: it
doesn't, by design (see its own examples -- binding the heap is always left to
the host application, since the backend has no way to know what else the host
might have bound). Forgetting this produces a real bug, not just a debug-layer
nag: the D3D12 debug layer reports it as `SET_DESCRIPTOR_TABLE_INVALID`
("the descriptor heap containing handle ... is different from currently set
descriptor heap"), but on a release build without the debug layer it's
silent, undefined GPU behavior -- the draw may render garbage, sample an
unrelated texture, or (depending on driver/hardware) work by coincidence most
of the time and fail intermittently, which is exactly what makes it easy to
ship unnoticed. Confirmed in practice: an ImGui-based trainer using this exact
API called `ImGui_ImplDX12_RenderDrawData` without first rebinding its own
heap, producing this error every frame; the fix was one `SetDescriptorHeaps`
call right before the `RenderDrawData` call.

**Multiple registered callbacks share the same command list, in registration
order**: Dx8to12 rebinds its own heaps before invoking *each* callback, so
every mod always starts from Dx8to12's own known heap state regardless of
what an earlier-registered mod's callback did -- you don't need to guard
against another mod's leftover heap binding, only against your own callback
leaving Dx8to12's heap unbound *within* your own draws if you switch to your
own heap mid-callback (rebind before your own draws, per the paragraph
above, same as always). If you don't switch heaps at all, you can rely on
Dx8to12's heap being bound at the start of your callback unconditionally.

**Surviving a device `Reset()`**: a window resize, fullscreen toggle, or
format change destroys and recreates the back buffer resources (and can
change their format/dimensions). If your PSO or other render-target-format-
derived state was built against the old backbuffer, it needs rebuilding.
Call `Dx8to12_GetSwapChainGeneration()` once per frame (or before you touch
any cached format/dimension state) and compare it against the value you saw
last time -- it's a plain counter bumped once per `Reset()`. When it changes,
re-fetch `Dx8to12_GetBackbufferFormat()`/window size and rebuild whatever
depended on them.

Register once your mod DLL has initialized (typically from your `DllMain`'s
`DLL_PROCESS_ATTACH`, after resolving the function pointer via
`GetProcAddress`, or from wherever else you first detect the game is ready).
If `Dx8to12_RegisterRenderCallback` returns `false` because no device exists
yet, retry after detecting `IDirect3DDevice8` creation (e.g. by hooking
`Direct3DCreate8`/`CreateDevice`, or simply polling
`Dx8to12_GetD3D12Device` for a non-null result).

## Scene metadata for mods

`Dx8to12_RegisterRenderCallback` only ever hands a mod the finished, flat 2D
backbuffer -- enough for an overlay, but not for anything that needs to know
how the frame was actually built (world-space reconstruction, per-pixel
effects driven by the scene's own lights, screen-space ambient occlusion).
This section adds three read-only accessors for that: the current frame's
depth buffer, the combined view*projection matrix, and the list of
currently-active D3D8 lights.

### Function reference (scene metadata)

| Function | Signature | Notes |
|---|---|---|
| `Dx8to12_RequestDepthBufferAccess` | `bool __cdecl (bool enable)` | Opt in (or back out) to depth buffer access; see below. Returns `false` if there's no device yet. |
| `Dx8to12_GetDepthBufferSrv` | `void* __cdecl ()` | Borrowed `ID3D12Resource*` for the current depth buffer. Null unless requested and ready. |
| `Dx8to12_GetDepthBufferSrvGpuHandle` | `unsigned long long __cdecl ()` | `D3D12_GPU_DESCRIPTOR_HANDLE.ptr` for an SRV Dx8to12 already created against that resource, in its own `srv_heap()`. 0 unless requested and ready. |
| `Dx8to12_GetDepthBufferFormat` | `unsigned int __cdecl ()` | `DXGI_FORMAT` to use if you build your own SRV desc against `Dx8to12_GetDepthBufferSrv`'s resource instead. 0 unless requested and ready. |
| `Dx8to12_GetViewProjMatrix` | `bool __cdecl (float out_matrix[16])` | Combined view*projection matrix, row-major (D3D8/`D3DMATRIX` convention). Returns `false` if there's no device yet. |
| `Dx8to12_GetActiveLightCount` | `int __cdecl ()` | Number of currently-enabled D3D8 lights (0-8). |
| `Dx8to12_GetActiveLight` | `bool __cdecl (int index, Dx8to12_LightInfo*)` | Fills in the light at `index` (`[0, GetActiveLightCount())`); see the struct below. Returns `false` for an out-of-range index or no device. |

```c
// Flat mirror of D3DLIGHT8's fields -- not D3DLIGHT8 itself, since that
// type isn't guaranteed ABI-stable across compilers/SDKs for an external
// mod DLL to link against directly.
typedef struct {
  int type;              // D3DLIGHT8 numeric type: 1=POINT, 2=SPOT, 3=DIRECTIONAL
  float diffuse[4], specular[4], ambient[4];  // r,g,b,a
  float position[3], direction[3];            // world space, x,y,z
  float range, falloff;
  float attenuation0, attenuation1, attenuation2;
  float theta, phi;
} Dx8to12_LightInfo;
```

**Depth buffer access is opt-in, and takes one frame to arrive**: creating
the depth buffer's SRV is free (a one-time descriptor allocation at texture
creation, not a per-frame cost), but actually letting a mod *sample* it
during its render callback isn't -- the depth buffer otherwise sits in
`D3D12_RESOURCE_STATE_DEPTH_WRITE` for the whole frame, and making it
readable requires a resource-state transition (and a symmetric one back,
afterward) around every render callback invocation that frame. To keep that
cost at zero for mods that don't need it, nothing is transitioned until you
call `Dx8to12_RequestDepthBufferAccess(true)` -- and because `Present()`
decides whether to do that transition once, at the top of the frame, before
your callback runs, calling it for the first time *from inside* your own
callback only takes effect starting the *next* frame (the same "may need a
frame to arrive" caveat `Dx8to12_RegisterRenderCallback` already has for
device creation). Call it once, e.g. right after your render callback
registers successfully, rather than gating it behind per-frame logic.

While depth access is enabled, Dx8to12 detaches the writable DSV before
transitioning that resource to SRV state. Render callbacks can sample the
depth texture and render to the color target, but must not assume the game's
writable DSV remains bound or attempt depth writes during that callback.

**Two ways to read the depth buffer**: `Dx8to12_GetDepthBufferSrvGpuHandle`
is the direct path -- Dx8to12's own SRV/sampler heaps are already bound on
the command list your render callback gets (see "Descriptor heaps are
shared state" above), so you can call `SetGraphicsRootDescriptorTable`
against that handle immediately, no heap of your own required, as long as
you sample the depth buffer *before* switching the command list to your own
heap (e.g. for ImGui) in the same callback. If your mod would rather keep
everything in its own heap, use `Dx8to12_GetDepthBufferSrv` (the raw
resource) plus `Dx8to12_GetDepthBufferFormat` (the format to put in your own
`D3D12_SHADER_RESOURCE_VIEW_DESC`) instead -- same pattern as
`Dx8to12_GetRtShadowOutputResource` below. Either way, the pointer/handle is
owned by Dx8to12 and becomes invalid at device destruction/reset -- re-fetch
it (and don't call `Release`) the same as any other borrowed resource here.

```cpp
// World-space reconstruction from depth, inside your render callback.
float view_proj[16];
get_view_proj(view_proj);  // Dx8to12_GetViewProjMatrix
// Invert view_proj yourself (D3DXMatrixInverse/DirectXMath), then in your
// pixel shader: sample depth at this pixel's UV via the GPU handle above,
// reconstruct clip-space {u*2-1, (1-v)*2-1, depth, 1}, multiply by the
// inverse, divide by w -- that's the pixel's world-space position.
```

## Pixel shader injection

Beyond reading scene metadata yourself (above), a mod can also inject a
custom HLSL fragment directly into the fixed-function pixel shader Dx8to12
generates -- e.g. adding a glow term to `diffuse_color` using
`Dx8to12_GetActiveLight`, without needing a full post-process pass. The
callback runs once per (re)compile of a given fixed-function pixel shader
*permutation*, not once per frame or per draw call.

```c
typedef struct {
  int has_normal;              // exact for this compiled permutation
  int has_view_pos;            // exact for this compiled permutation
  int texture_stage_count;
} Dx8to12_PixelShaderInjectionContext;

// Return the number of bytes written to out_hlsl_snippet (0 = inject
// nothing this compile). A return value greater than
// out_hlsl_snippet_capacity is treated as "wrote nothing".
typedef size_t (__cdecl *Dx8to12_PixelShaderInjectionFn)(
    const Dx8to12_PixelShaderInjectionContext* context,
    char* out_hlsl_snippet, size_t out_hlsl_snippet_capacity);

bool __cdecl Dx8to12_RegisterPixelShaderInjection(Dx8to12_PixelShaderInjectionFn callback);
bool __cdecl Dx8to12_UnregisterPixelShaderInjection(Dx8to12_PixelShaderInjectionFn callback);
```

| Function | Signature | Notes |
|---|---|---|
| `Dx8to12_RegisterPixelShaderInjection` | `bool __cdecl (Dx8to12_PixelShaderInjectionFn)` | Only one callback at a time -- returns `false` for null or if a *different* one is already registered (re-registering the same pointer is a no-op success). Invalidates the pixel shader cache on the render thread. |
| `Dx8to12_UnregisterPixelShaderInjection` | `bool __cdecl (Dx8to12_PixelShaderInjectionFn)` | Returns `false` for null or if `callback` isn't the one currently registered. Waits for an invocation already in progress, then invalidates the pixel shader cache on the render thread. |
| `Dx8to12_InvalidatePixelShaderCache` | `bool __cdecl ()` | Forces every fixed-function pixel shader to regenerate on next use -- call this if your registered callback's *output* changes at runtime (a toggle, a setting) without a full Unregister/Register cycle. |

**Where your snippet runs**: right after `diffuse_color`/`specular_color`
are set from the vertex output (and after per-pixel lighting, if
`LightingMode` has it enabled, has already updated them) but *before* any
texture stage or `result_color` exists -- your snippet may read/write
`diffuse_color` and `specular_color` and read `IN` (`FFVertexOutput`,
declared in `ff_vertex_shader.hlsl`), plus everything `lighting.hlsl`/
`ps_common.hlsl` already provide (both `#include`d ahead of your snippet).
It runs *before* the texture stage chain, so it cannot see or modify
`result_color`/`temp_color` -- those don't exist yet at this point.

**`has_normal`/`has_view_pos` are part of the shader-cache key**: every
compiled permutation now matches the vertex declaration that supplied these
flags. `IN.oViewNormal`/`IN.oViewPos` always exist in `FFVertexOutput`, but a
zero flag means their contents are not meaningful for that draw shape.

The injection callback is serialized with register/unregister so an ASI may
unload safely after unregister returns. Keep the callback pure and fast: do
not call the injection registration/invalidation exports recursively from
inside it, and do not perform GPU work there. It runs during shader creation.

**A snippet that fails to compile does not break the game**: if the shader
with your injected fragment fails to compile, Dx8to12 logs the HLSL compiler
error and recompiles that permutation *without* your injection instead --
your effect silently doesn't apply for that permutation, but the rest of the
game keeps rendering normally. Check `log.txt` for
`Dx8to12_PixelShaderInjectionFn produced HLSL that failed to compile` while
developing your snippet. There is no protection against HLSL that *compiles*
but is pathologically expensive or hangs the GPU (an accidental infinite
loop, etc.) -- that's a real TDR/driver-reset risk exactly like any other
shader bug, so test incrementally.

### Adding a new setting (for anyone extending this project)

Three places, in this order:

1. The field on `Dx8to12::Config` (`src/config.h`), with a comment saying what
   it means and what its valid range is.
2. An entry in the `kFields` table at the top of `src/config.cpp` -- name,
   type, and whether it should be written back to the INI. The INI parser, the
   INI writer and the startup log all walk this table, so those three come for
   free; there is no `if`/`else if` chain to extend any more.
3. A case in the matching `GetConfigValue*` / `SetConfigValue*` pair, which is
   where range validation, any cross-setting implication, and the
   `MarkConfigDirty()` call on an actual change live.

Set `persist = false` in `kFields` for anything a player would be alarmed to
find still on next session (see "Settings that are not persisted" above).

Then document the key in this file's INI example. The exported
`Dx8to12_*Setting*` functions are generic over the key name, so a new setting
needs no new export and no ABI version bump.

**H4 helper result channel.** `Dx8to12_GetRtShadowOutputResource` returns a
borrowed `ID3D12Resource*` containing the helper's current RT result;
`Dx8to12_GetRtShadowDoneFence` and
`Dx8to12_GetRtShadowDoneFenceValue` are retained for ABI compatibility with
the old shared-GPU prototype. Protocol v13 returns a null fence and zero value:
the result upload is recorded on the game's current command list before mod
callbacks, so no queue wait is needed. The resource stays null until the
helper's startup handshake and first completed scene batch. Pointers are owned by
Dx8to12 and become invalid at device destruction/reset; a mod must not call
`Release` on them or retain them across a reset.

**Current safety status:** protocol v14 enables bounded BLAS/TLAS building and
a 320x180 shadow `DispatchRays`. Geometry is copied into an 8 MiB shared-memory payload;
the x64 helper creates its own UPLOAD resources from those bytes. It no longer
opens x86-created vertex/index resources, waits on the x86 render queue or
inserts geometry-copy commands into the game's command list. Submissions are
limited to one batch per 500 ms. This isolation replaces the mixed-device GPU
resource path that caused repeated NVIDIA device removals and one system hang.

The helper reads its own uint shadow buffer back into the IPC mapping. Dx8to12
then uploads that byte mask into an **x86-local `Texture2D` in
`DXGI_FORMAT_R8G8B8A8_UNORM`**, transitions it to `PIXEL_SHADER_RESOURCE`, and only
then invokes mod callbacks. No D3D12 resource or fence crosses the x86/x64
device boundary.

Before recording any command that samples the result, queue a GPU wait rather
than blocking the CPU:

```cpp
auto* output = static_cast<ID3D12Resource*>(get_rt_output());
auto* fence = static_cast<ID3D12Fence*>(get_rt_fence());
const uint64_t value = get_rt_fence_value();
if (output && fence && value != 0) { // Legacy shared-result protocol only.
  command_queue->Wait(fence, value);
}
if (output) {
  // Protocol v13: sample directly in this render callback. The upload and
  // PIXEL_SHADER_RESOURCE transition are earlier on the same command list.
}
```

### Integrating the RT shadow mask in a companion mod

A companion ASI such as VCVisual12 should load these exports with
`GetProcAddress` after Dx8to12 has created its device:

```cpp
using GetRtShadowOutputFn = void *(__cdecl *)();
using GetRtShadowFenceFn = void *(__cdecl *)();
using GetRtShadowFenceValueFn = unsigned long long(__cdecl *)();
using GetRtShadowOutputDimensionFn = unsigned int(__cdecl *)();
using GetRtShadowOutputFormatFn = unsigned int(__cdecl *)();
using RegisterRenderCallbackFn = bool(__cdecl *)(void(__cdecl *)(void *));
```

Register a render callback and, from that callback, obtain the borrowed
`ID3D12Resource*`, `ID3D12Fence*`, and fence value. For protocol v14 the
resource is valid while fence/value are null/zero: do not skip composition in
that case and do not add a queue wait. If a future/legacy implementation
returns a non-null fence and nonzero value, use the GPU queue wait shown above.
Do **not** call `SetEventOnCompletion` plus a CPU wait on every frame.

The callback then records its fullscreen composite on the command list passed
by `Dx8to12_RegisterRenderCallback`. The backbuffer is already bound and has
the game's fully rendered image, so the composite belongs before returning
from that callback. The mod must create its SRV/PSO resources itself and
recreate any backbuffer-format-dependent state after
`Dx8to12_GetSwapChainGeneration` changes.

Query the width, height, and format exports instead of assuming resource
dimensions. The current object is an x86-local RGBA8 texture. Do not CPU-wait and
do not retain the borrowed pointer over a device reset.
