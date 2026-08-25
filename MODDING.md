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
```

An unrecognized key, or a value out of the documented range, is logged as an
error to `log.txt` and otherwise ignored (that one line's setting keeps its
default; the rest of the file still applies normally).

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

`key` is one of the names documented in the `dx8to12.ini` section above
(`AnisotropicOverride`, `MSAASamples`, `SharpenStrength`,
`HighPrecisionDepth`), case-insensitive, matched against the correctly-typed
function (e.g. `HighPrecisionDepth` only responds to the `*Bool` functions).

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

**Measuring framerate**: `Dx8to12_GetLastFrameMs()` returns the interval
between the last two `Present` calls -- the frames the game actually put on
screen -- so `1000.0 / Dx8to12_GetLastFrameMs()` is the current FPS. Prefer
this over a third-party FPS overlay: overlays that work by hooking
`Present`/`ExecuteCommandLists` (RivaTuner/MSI Afterburner, and similar) do
not reliably interoperate with this shim's hand-rolled D3D12 usage, and have
been observed both misreporting the framerate and rendering corrupted
overlay elements on top of the game (see ROADMAP.md). Reading the number
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

### Adding a new setting (for anyone extending this project)

Add the field to `Dx8to12::Config` (`src/config.h`), then a case for it
in each of the six `Get/SetConfigValue{Int,Float,Bool}` functions and the
INI key-parsing `if`/`else if` chain in `src/config.cpp` -- there's a single
source of truth per setting (the field), touched from a small, consistent
set of places; no separate registration/reflection system. Document the new
key in this file's INI example and the function-reference table above.
