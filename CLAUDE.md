# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Dx8to12 implements the Direct3D 8 API (`d3d8.dll`) as a shim on top of Direct3D 12. It's a drop-in replacement DLL: a DX8 game loads `d3d8.dll`, calls `Direct3DCreate8` (the sole export, see `src/d3d8.def`), and every DX8 call gets translated into D3D12 command lists/resources under the hood.

Games verified to run: Battlefield 1942 and Age of Mythology. The implementation targets "whatever these games actually call" rather than the full DX8 spec — see "Known gaps" below. Also currently being tested against Grand Theft Auto: Vice City (not yet working — see ROADMAP.md's real-game-feedback log).

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
- `DX8TO12_ENABLE_VALIDATION` (default ON) — enables extra internal validation/assertions.

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

Known gap areas (see `ROADMAP.md` for the prioritized plan, and its running log of real-game-driven bugfixes): 7 of `IDirect3DDevice8`'s 100 methods remain unimplemented stubs (`ProcessVertices`, RT-patch methods, `CreateVolumeTexture`, `CreateAdditionalSwapChain`, `GetFrontBuffer` — see Phase 6). `SetClipPlane`/`GetClipPlane` and `SetPixelShaderConstant`/`GetPixelShaderConstant` are implemented as bookkeeping only: they store/return values correctly but don't affect rendering (no GPU-side clip planes; pixel shader constants aren't wired into the constant buffer `programmable_ps.hlsl` actually reads from — see the Phase 5 entry in ROADMAP.md for exactly what real wiring would require).

`IDirect3DSurface8::LockRect`/`UnlockRect` now work on all three surface kinds (`GpuSurface`, `CpuSurface`, `BackbufferSurface`). `GpuSurface` on a `D3DPOOL_MANAGED` texture delegates to the texture's own `LockRect`; on `D3DPOOL_DEFAULT` (lockable render targets) and on `BackbufferSurface`, `BaseSurface::LockGpuReadback`/`UnlockGpuReadback` (`surface.cpp`) do a real GPU→CPU readback: create a `D3D12_HEAP_TYPE_READBACK` buffer sized from `BaseTexture::GetFootprint`, `CopyTextureRegion` into it, then call `Device::SubmitAndWait(false)` to flush and block until the GPU copy is done before mapping — this is the same "flush mid-frame and keep going" pattern `Device::WaitForFrame` already relies on, not new fence machinery.

### Utilities

`util.h` defines `ComPtr` (COM ref-counted wrapper, distinct from `Microsoft::WRL::ComPtr`), `RefCounted`/`InternalPtr` (this project's own non-COM ref-counting for internal-only objects), and `ComWrap`. `utils/dx_utils.h/.cpp` has DX8↔DXGI format conversion and misc D3D12 helpers. `utils/asserts.h` defines `ASSERT`, `ASSERT_HR`, `HR_OR_RETURN`, `FAIL`, `NOT_IMPLEMENTED` — the project's only error-handling vocabulary (no exceptions).
