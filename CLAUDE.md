# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Planning documents are local-only

`plan/` holds the roadmap and the cross-agent handoff notes. It is gitignored:
those are working documents, not something a reader of this repo needs. Code
comments and this file point at `plan/ROADMAP.md` and similar, so those
references resolve in a working copy but not in a fresh clone. Public-facing
documentation lives in `README.md` and `MODDING.md`; keep it that way.

## What this is

Dx8to12 implements the Direct3D 8 API (`d3d8.dll`) as a shim on top of Direct3D 12. It's a drop-in replacement DLL: a DX8 game loads `d3d8.dll`, calls `Direct3DCreate8` (the sole export, see `src/d3d8.def`), and every DX8 call gets translated into D3D12 command lists/resources under the hood.

Games verified to run: Battlefield 1942, Age of Mythology, and Grand Theft Auto: Vice City. The implementation targets "whatever these games actually call" rather than the full DX8 spec — see "Known gaps" below. Vice City is the game most of the recent work has been driven by (see plan/ROADMAP.md's real-game-feedback log) and the one DLAA was brought up against.

## Build

Requires Windows + MSVC (Visual Studio toolchain) and the Windows SDK (`DXGI.lib`, `D3D12.lib`, `D3DCompiler.lib`, `dxguid.lib`). CMake ≥ 3.11.

**Must be built as x86 (Win32), not x64.** DX8-era games (Battlefield 1942, Age of Mythology) are 32-bit processes, and a 64-bit `d3d8.dll` cannot be loaded into a 32-bit process — Windows rejects it at `LoadLibrary` time regardless of anything in the code. This is a pure architecture-of-the-binary constraint, not a D3D12 capability limit: `d3d12.dll`/`D3D12.lib` fully support x86 client processes via SysWOW64 on x64 Windows, so the graphics layer itself is not the bottleneck. Always pass `-arch=x86` to `VsDevCmd.bat` (or otherwise select the x86 toolset) before configuring — an x64 build will compile and link cleanly with zero code changes, which makes the mistake easy to make and easy to miss until you try to actually load the DLL into a game.

From an x86 Developer Command Prompt (or after running `VsDevCmd.bat -arch=x86`):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is required with CMake ≥4.0 because the vendored `CMakeRC.cmake` (third-party resource-embedding helper) declares an old `cmake_minimum_required` that newer CMake rejects outright without it.

**Use `RelWithDebInfo`, not `Release`, as the default build type.** `Release` produces no PDB (no `/Zi`), which means the crash-diagnostic vectored exception handler in `dllmain.cpp` can log addresses and a call stack but nothing can map them back to source — exactly the situation that came up debugging a real crash: the log had a faulting address and a full stack trace, but no way to turn `d3d8.dll+0x2af8e` into a function/line without a matching PDB. `RelWithDebInfo` is `/O2 /Ob1` (barely less aggressive inlining than Release's `/O2 /Ob2`) plus `/Zi` — essentially the same runtime performance with a usable PDB (`build/d3d8.pdb`) alongside the DLL. Only reach for plain `Release` if you specifically need to compare against a symbol-free build.

Output is `build/d3d8.dll` (target name `d3d8`), with `build/d3d8.pdb` next to it when built with debug info. There is no test suite — validation is "run a real DX8 game against the built DLL." A `POST_BUILD` step also copies just `d3d8.dll` + `d3d8.pdb` into `build/dist/` (and `build-x86/dist/`) after every build — that's the folder to actually copy into a game's install directory, so you're not hunting for the right two files among all the other build tree clutter (`.obj`, `.ninja` files, CMake cache, etc).

**Symbolicating a crash from `log.txt`'s vectored-exception-handler output**: the log gives `module+offset` for the faulting address and every stack frame (e.g. `d3d8.dll+0x2af8e`). This can only be resolved against the *exact* DLL binary that produced it — optimizer decisions shift code addresses between builds, so a PDB from a different (even seemingly-identical-source) build won't line up. Keep the specific `d3d8.dll`+`d3d8.pdb` pair that was actually running when a crash log was captured if you need to investigate it later; don't rebuild first and expect the old log's offsets to still resolve correctly.

Notable CMake options (top of `CMakeLists.txt`):
- `DX8TO12_USE_ALLOCATOR` (default OFF) — pulls in D3D12MemoryAllocator via `FetchContent` instead of manual suballocation.
- `DX8TO12_ENABLE_VALIDATION` (default ON) — enables the D3D12 debug layer (`EnableDebugLayer()`) and `TRACE_ENTRY` (a per-call trace log built into every single `IDirect3DDevice8` method, gated to compile to nothing when this is off). Both are genuinely expensive on a game issuing thousands of draws/state-changes per frame — measured, disabling this roughly doubled FPS in a light test scene. See the dev vs. release build split below.

### Build profiles

Same source, one or two CMake flags apart. All of them are `RelWithDebInfo`
and all produce a `d3d8.pdb`, so a crash stays symbolicatable in every profile.
Every build tree is gitignored (`build-*/`).

| Profile | Directory | Extra CMake flags | Writes `log.txt`? | Use for |
|---|---|---|---|---|
| **Dev** | `build-x86` | *(none — validation is ON by default)* | Yes, everything | Active development on this codebase |
| **Release** | `build-x86-release` | `-DDX8TO12_ENABLE_VALIDATION=OFF` | No | The general-purpose build; any D3D8 game |
| **Release-VC** | `build-x86-release-vc` | `... =OFF -DDX8TO12_DRAW_STATE_CACHE=ON` | No | Playing GTA: Vice City at maximum speed |
| **Mindebug** | `build-x86-release-mindebug*` | `... =OFF -DDX8TO12_ENABLE_MINDEBUG=ON` | Yes, compact only | Diagnosing a bug at full frame rate |

```
# Dev: full D3D12 debug-layer validation + TRACE_ENTRY.
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-x86 --target d3d8

# Release: same optimization level and PDB (still RelWithDebInfo, not plain Release --
# see the PDB rationale above), but DX8TO12_ENABLE_VALIDATION off.
cmake -S . -B build-x86-release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDX8TO12_ENABLE_VALIDATION=OFF
cmake --build build-x86-release --target d3d8

# Release-VC: release plus the draw-state binding cache.
cmake -S . -B build-x86-release-vc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDX8TO12_ENABLE_VALIDATION=OFF -DDX8TO12_DRAW_STATE_CACHE=ON
cmake --build build-x86-release-vc --target d3d8
```

Use `build-x86/dist/` while actively working on this codebase (a real bug is much easier to diagnose with the debug layer's resource-state/descriptor validation and a full log.txt). Use a release profile for actually playing/benchmarking. Only the debug-layer + trace-log overhead differs between them, not crash-diagnosability.

**"Release-VC" is about tuning, not about hardcoding a game.** Nothing in it keys off GTA: Vice City -- it is the ordinary release plus `DX8TO12_DRAW_STATE_CACHE`, an optimisation that was switched off during the missing-geometry investigation and has not yet been proven across a long session on other games. The name records where it has actually been exercised, nothing more.

**Do not run a `mindebug` build against any game other than GTA: Vice City.** Those builds read *and write* the host process's memory at addresses hardcoded for VC 1.0 (entity lists, streaming tables, and with `DX8TO12_KEEP_TARGET_LOD` a model's alpha byte). Under a different executable those addresses are meaningless. All of it lives inside `#ifdef DX8TO12_ENABLE_MINDEBUG` in `device.cpp` and is compiled out of every other profile -- `kGtaPreferredImageBase` is defined inside that block, so a stray use outside it would fail to link rather than ship silently.

### Measuring anything

Compare one binary against itself, in one place, toggled at runtime. Almost
every graphics and tuning switch is a `dx8to12.ini` setting rather than a
build flag for exactly this reason: the frame-time difference between two
spots in Vice City is larger than most of the differences worth measuring, so
a comparison across two builds -- which means two sessions and two places --
returns noise. Several measurements in this project were inconclusive until
they were redone this way.

Use `build-x86-perf` (or `build-x86-perf-cache`), stand still, and let the
`Perf:` lines accumulate for ~10s per configuration. F5/F6/F7 switch modes in
place; each writes a marker line to `log.txt`, so samples can be attributed
afterwards.

`tools/record_cpu_trace.ps1` records a WPR CPU trace when the question is
"what is the CPU actually doing" rather than "how long is a frame". It needs
Administrator. Process the result with `xperf -i trace.etl -tle -tti -symbols
-a stack -butterfly 500 -process gta-vc.exe` -- `-tle -tti` because a trace
with lost events is otherwise refused, and note that its own stack-walking
shows up as ~20% of the samples.

### CMake options

| Option | Default | Effect |
|---|---|---|
| `DX8TO12_ENABLE_VALIDATION` | ON | D3D12 debug layer + `TRACE_ENTRY` + full AixLog. Roughly halves FPS; turn off for anything but development. |
| `DX8TO12_ENABLE_MINDEBUG` | OFF | Compact GTA VC-specific diagnostics through `WriteMindebugDiagnosticLine` instead of AixLog. Requires `ENABLE_VALIDATION=OFF`. |
| `DX8TO12_PAD_BUFFERS` | **ON** | Over-allocates every vertex/index buffer and hides the pad from `GetDesc`. Fixes the "disappearing road/building" bug -- GTA VC writes past the end of its own static index buffers and the draw count was then clamped, dropping the trailing triangles. Turn OFF only to reproduce that bug. See `plan/MISSING_TEXTURES_DIAGNOSTIC_HANDOFF.md`. |
| `DX8TO12_DRAW_STATE_CACHE` | OFF | Compiles in the draw-state binding cache; the `DrawStateCache` setting then decides whether it runs. Measured at ~31% faster on a CPU-bound scene, but it is the only optimisation that skips *recording* commands -- read `CacheDrawStateBindings` in `device_limits.h` first. |
| `DX8TO12_PERF_LOG` | OFF | Release speed with one frame-time line per ~120 frames. Exists because the only build that reported frame times was the dev build, whose debug layer roughly halves the frame rate -- so it could measure the debug layer and nothing else. |
| `DX8TO12_TEMPORAL_JITTER` | OFF | Sub-pixel Halton offset on the projection. A temporal upscaler needs it; on its own it only makes the image shimmer. |
| `DX8TO12_SCENE_TARGET` | OFF | Renders the scene into an offscreen colour target and copies it to the backbuffer before mod callbacks. Correct means **invisible**: any difference from a build without it is a bug. |
| `DX8TO12_MOTION_VECTORS` | OFF | Camera-only motion vectors reconstructed from depth into an offscreen `R16G16_FLOAT` target. Requires `SCENE_TARGET`. Nothing samples it yet. |
| `DX8TO12_MOTION_VECTORS_DEBUG` | OFF | Draws those vectors as false colour over the **right half** of the screen (left half stays playable). Implies `MOTION_VECTORS`. |
| `DX8TO12_BUFFER_SHADOW` | OFF | Diagnostic: gives every buffer a full CPU shadow with a guard page, so writes past a buffer's end are caught instead of corrupting the heap. This is what identified the bug `PAD_BUFFERS` fixes. |
| `DX8TO12_KEEP_TARGET_LOD` | OFF | Pins one VC road model in its fading state so its LOD is never culled. Writes to game memory every frame; pollutes any baseline measurement. |
| `DX8TO12_FORCE_GPU_IDLE` | OFF | Waits for the just-submitted frame. Note it waits *after* submission, so it does not remove aliasing within one unsubmitted command list. |
| `DX8TO12_PASSTHROUGH_OOB_INDICES` | OFF | Submits out-of-bounds indexed draws untruncated. Undefined behaviour in D3D12; `PAD_BUFFERS` is the supported fix for the same symptom. |
| `DX8TO12_VALIDATE_DEVICE_ALWAYS_FAIL` | OFF | Makes `ValidateDevice` fail like the d3d8to11 reference port (compatibility A/B). |
| `DX8TO12_ENABLE_D3D12_DEBUG_LAYER` | OFF | Debug layer without the dev trace logging. |
| `DX8TO12_USE_ALLOCATOR` | OFF | D3D12MemoryAllocator instead of manual suballocation. Note the option currently defines `USE_ALLOCATOR`, while parts of the code test `DX8TO12_USE_ALLOCATOR` -- do not copy this option's wiring as a template. |

On Clang, warnings are treated as errors (`-Werror`); MSVC uses `/W4` with a couple of disabled warnings. Precompiled header covers `aixlog.hpp` and `<d3d12.h>`.

Logging goes to `log.txt` next to the source tree (`CURRENT_SOURCE_DIR` baked in at compile time), via AixLog (`third_party/aixlog.hpp`).

`dllmain.cpp` also installs a vectored exception handler (`AddVectoredExceptionHandler`) that logs a `=== Unhandled exception ===` block to `log.txt` — code, faulting address, and the module+offset it falls in — on native crashes (access violations, stack overflow, etc.) before letting the crash proceed normally. This catches crashes `ASSERT`/`FAIL`/`NOT_IMPLEMENTED()` can't, since those only fire on paths that are explicitly guarded. When a game crashes with no `FAIL`/`ASSERT` message in the log, check for this block first — it narrows down the faulting module/offset even when nothing on the D3D12 debug-layer side reported anything.

## Architecture

### Layer mapping

Each DX8 COM interface has a corresponding implementation class, all under `namespace Dx8to12`:

| DX8 interface | Implementation | File |
|---|---|---|
| `IDirect3D8` | `Direct3D8` | `direct3d8.h/.cpp` |
| `IDirect3DDevice8` | `Device` | `device.h/.cpp` |
| `IDirect3DSurface8` | `BaseSurface` + `GpuSurface` / `CpuSurface` / `BackbufferSurface` | `surface.h/.cpp` |
| `IDirect3DTexture8` / `IDirect3DCubeTexture8` | `BaseTexture` + `GpuTexture` / `CpuTexture` / `DynamicTexture` | `texture.h/.cpp` |
| `IDirect3DVertexBuffer8` / `IDirect3DIndexBuffer8` | `Buffer` | `buffer.h/.cpp` |

`Device` (`device.cpp`, ~1600 lines) is the center of gravity: it owns the D3D12 device, command queue/lists, root signatures, and all fixed-function state, and does the actual translation from DX8 draw calls into D3D12 PSOs and command list submission (`CreatePSO`, `PrepareDrawCall`, `SubmitAndWait`).

### Resource variants (Gpu / Cpu / Dynamic)

Textures and surfaces come in three flavors reflecting DX8 memory pools and usage flags, each with its own D3D12 backing strategy:
- **Gpu** (`GpuTexture`/`GpuSurface`) — default-pool GPU-resident resource.
- **Cpu** (`CpuTexture`/`CpuSurface`) — CPU-visible copy kept alongside (used for `D3DPOOL_MANAGED`/lockable resources); `CpuTexture::CopyToGpuTexture` / `CopySubresourceToGpuTexture` push updates to the GPU-side twin.
- **Dynamic** (`DynamicTexture`) — discard-write-only GPU resource for `D3DUSAGE_DYNAMIC`, simpler than the general dynamic buffer path since it only supports discard.

`Buffer` (vertex/index buffers) has an analogous dynamic path via `DynamicRingBuffer` (`dynamic_ring_buffer.h/.cpp`) for `D3DUSAGE_DYNAMIC` buffers that get remapped every frame.

`PoolHeap` (`pool_heap.h/.cpp`) manages descriptor heap suballocation for SRV/RTV/DSV/sampler descriptors (limits in `device_limits.h`: `kMaxNumSrvs`, `kMaxNumRtvs`, `kMaxSamplerStates`, etc.).

### Shader translation

- Fixed-function pipeline: DX8 render state (lighting, fog, texture stage state) drives dynamically-selected/generated HLSL in `src/shaders/` (`ff_vertex_shader.hlsl`, `lighting.hlsl`, `ps_common.hlsl`), assembled by `ff_pixel_shader.cpp` based on active `TextureStageState`. Shaders are compiled at build time into a CMakeRC resource archive (`Dx8to12_shaders` target, `src/shaders/CMakeLists.txt`) and loaded from memory at runtime — there is no runtime file I/O for shaders.
- Programmable shaders: DX8 vertex shader bytecode (`vs_1_1`) is parsed and transpiled by `shader_parser.cpp`/`shader_parser.h` (see `vertex_shader.cpp` for the vertex declaration/shader object) into HLSL matching `programmable_vs.hlsl`/`programmable_ps.hlsl` conventions, then compiled via `D3DCompiler`. Only a subset of `ps_1_3` is supported — extend `shader_parser.cpp` and the `common.hlsl`/`ps_common.hlsl` templates together when adding opcodes.

### Command submission model

`Device::SubmitAndWait` drives frame submission; `WaitForFrame` / `FreeFrameResources` manage a fence-based `kNumBackBuffers`-deep (2) frame pipeline. `MarkBufferForPersist` tracks buffers that must outlive the current command list until the GPU has consumed them.

### Root signatures

`Device::InitRootSignatures` builds the D3D12 root signature(s) once at device init/reset time: constant buffers for VS/PS constants + per-stage descriptor tables for textures and samplers (`kMaxTexStages` = 8). PSOs are created/cached per draw-call shape in `CreatePSO`.

### Implementation-coverage pattern (important when adding methods)

Every unimplemented DX8 method is marked with the `PURE`/`VIRT_NOT_IMPLEMENTED` macro pair (`src/utils/asserts.h`) instead of being silently missing:

```cpp
#define VIRT_NOT_IMPLEMENTED override { NOT_IMPLEMENTED(); }
```

In `device.h`, `#define PURE VIRT_NOT_IMPLEMENTED` is active for the whole `IDirect3DDevice8` method block, then `#undef`'d and redefined to `= 0` afterward for the class's own abstract-ish private helpers. `NOT_IMPLEMENTED()` shows a message box and `abort()`s at runtime — it's a loud, file/line/function-tagged crash, not a silent no-op. When implementing a previously-stubbed method, change its trailing `PURE` to `override` and give it a body in the corresponding `.cpp`. This pattern is also used in `surface.h`, `texture.h`, and `buffer.h` for their respective interfaces.

Known gap areas (see `plan/ROADMAP.md` for the prioritized plan, and its running log of real-game-driven bugfixes): 7 of `IDirect3DDevice8`'s 100 methods remain unimplemented stubs (`ProcessVertices`, RT-patch methods, `CreateVolumeTexture`, `CreateAdditionalSwapChain`, `GetFrontBuffer` — see Phase 6). `SetClipPlane`/`GetClipPlane` and `SetPixelShaderConstant`/`GetPixelShaderConstant` are implemented as bookkeeping only: they store/return values correctly but don't affect rendering (no GPU-side clip planes; pixel shader constants aren't wired into the constant buffer `programmable_ps.hlsl` actually reads from — see the Phase 5 entry in plan/ROADMAP.md for exactly what real wiring would require).

`IDirect3DSurface8::LockRect`/`UnlockRect` now work on all three surface kinds (`GpuSurface`, `CpuSurface`, `BackbufferSurface`). `GpuSurface` on a `D3DPOOL_MANAGED` texture delegates to the texture's own `LockRect`; on `D3DPOOL_DEFAULT` (lockable render targets) and on `BackbufferSurface`, `BaseSurface::LockGpuReadback`/`UnlockGpuReadback` (`surface.cpp`) do a real GPU→CPU readback: create a `D3D12_HEAP_TYPE_READBACK` buffer sized from `BaseTexture::GetFootprint`, `CopyTextureRegion` into it, then call `Device::SubmitAndWait(false)` to flush and block until the GPU copy is done before mapping — this is the same "flush mid-frame and keep going" pattern `Device::WaitForFrame` already relies on, not new fence machinery.

### Utilities

`util.h` defines `ComPtr` (COM ref-counted wrapper, distinct from `Microsoft::WRL::ComPtr`), `RefCounted`/`InternalPtr` (this project's own non-COM ref-counting for internal-only objects), and `ComWrap`. `utils/dx_utils.h/.cpp` has DX8↔DXGI format conversion and misc D3D12 helpers. `utils/asserts.h` defines `ASSERT`, `ASSERT_HR`, `HR_OR_RETURN`, `FAIL`, `NOT_IMPLEMENTED` — the project's only error-handling vocabulary (no exceptions).
