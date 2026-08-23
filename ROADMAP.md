# DX8→DX12 coverage roadmap

Snapshot from 2026-08-23: `IDirect3DDevice8` has 100 methods; 50 have real implementations, 50 are stubs (`NOT_IMPLEMENTED()` → abort at runtime). `IDirect3DSurface8` only implements `GetDesc`. Prioritized by "how likely is this to crash a real game" vs. implementation cost.

**Build architecture (verified 2026-08-23): must be x86.** Target games are 32-bit processes; a 64-bit `d3d8.dll` can't be loaded into them regardless of code correctness. This is not a D3D12 limitation — `D3D12.lib`/`d3d12.dll` fully support x86 client processes on x64 Windows via SysWOW64. Confirmed the codebase builds clean as x86 with zero source changes (`VsDevCmd.bat -arch=x86` + same CMake invocation) — output is a genuine `PE32 executable ... Intel i386` DLL. See CLAUDE.md's Build section; always build x86 for anything meant to actually run against a game.

## Phase 1 — Mirror getters (done, 2026-08-23)

State was already tracked on `Device` (`render_state_`, `lights_`, `enabled_lights_`, bound textures/targets, current shader handles, etc.); these `Get*` methods just read it back:

- `GetRenderState` (mirrors `SetRenderState`, uses `RenderState::GetEnumAtIndex`)
- `GetLight`, `GetLightEnable` (mirrors `SetLight`/`LightEnable`, reads `lights_`/`enabled_lights_`)
- `GetMaterial`
- `GetTexture`
- `GetViewport` (mirrors `SetViewport`)
- `GetRenderTarget` (returns the explicitly-bound render target, or the current backbuffer if none was set — mirrors the implicit-backbuffer convention already used by `GetDepthStencilSurface`/`GetBackBuffer`)
- `GetIndices` (mirrors `SetIndices`, reads `bound_index_buffer_`/`bound_base_vertex_`)
- `GetStreamSource` (mirrors `SetStreamSource`; had to add `bound_vertex_stream_strides_` since `SetStreamSource` wasn't storing the stride at all before this)
- `GetVertexShader`, `GetPixelShader`
- `GetVertexShaderConstant` (mirrors `SetVertexShaderConstant`, reads `bound_vs_cregs_`)

Skipped, not implementable as a cheap mirror — their `Set*` counterpart is itself a stub, so there's no state to read back:
- `GetClipPlane` (`SetClipPlane` is unimplemented)
- `GetClipStatus` (`SetClipStatus` is unimplemented)
- `GetPixelShaderConstant` (`SetPixelShaderConstant` is unimplemented)
- `GetCurrentTexturePalette` (palette APIs deliberately return `D3DERR_NOTAVAILABLE`, no palette state exists)

These four need their `Set*` implemented first (small features in their own right), so they've been pushed to Phase 6.

## Real-game feedback (2026-08-23): `CheckDeviceMultiSampleType`

Reported crash from an actual test run: `IDirect3D8::CheckDeviceMultiSampleType` was a bare `NOT_IMPLEMENTED()` stub, and a real game's adapter-capability probing hit it before device creation even started — this jumps ahead of the phased plan below because it's an empirically confirmed blocker, not a guess.

Fixed in `direct3d8.cpp`: only `D3DMULTISAMPLE_NONE` reports as available (`S_OK`); any actual multisample level returns `D3DERR_NOTAVAILABLE`. This isn't a stopgap — `Device::CreatePSO` always builds PSOs with `SampleDesc.Count = 1`, so there is no real MSAA implementation to honestly report "yes" for. Games probing MSAA levels top-down will fall back to no-AA and continue, instead of believing they got antialiasing they silently didn't. True MSAA support (rendering to a multisampled RT + resolve) would be new work, not covered by this fix — add it here as a later phase if a game turns out to depend on it rather than falling back gracefully.

## Phase 2 — Surface LockRect/UnlockRect (done, 2026-08-23)

Was the highest functional-impact gap: `BaseSurface` only implemented `GetDesc`, so screenshots, render-target readback, and `CreateImageSurface` all aborted.

- `GpuSurface::LockRect/UnlockRect` — on a `D3DPOOL_MANAGED` texture, delegates straight to the owning `GpuTexture`'s per-subresource `LockRect`/`UnlockRect` (already implemented in `texture.cpp`). On `D3DPOOL_DEFAULT` (lockable render targets), goes through the new GPU readback path below.
- `CpuSurface::LockRect/UnlockRect` — reads straight from the footprint/pointer captured at `GetSurfaceLevel` time (`compact_pitch_`/`data_ptr_`), no delegation needed.
- `BackbufferSurface::LockRect` and `GpuSurface::LockRect` (default-pool case) share `BaseSurface::LockGpuReadback`/`UnlockGpuReadback` (`surface.cpp`): create a `D3D12_HEAP_TYPE_READBACK` buffer sized via the new `BaseTexture::GetFootprint` accessor, `CopyTextureRegion` the subresource into it, then call `Device::SubmitAndWait(false)` to flush and block until done (reusing the exact flush-mid-frame path `Device::WaitForFrame` already relies on — no new fence code), then `Map` it. Read-path only; a locked render target is not expected to be written back through `Unlock`.
- `QueryInterface`, `GetDevice` on surfaces — implemented, mirror the equivalent `Device`/`Texture` patterns. `SetPrivateData`/`GetPrivateData`/`FreePrivateData` return `D3DERR_NOTAVAILABLE` (same choice already made for the palette APIs — no private-data storage exists, and returning a clean error is preferable to a fake success). `GetContainer` is still unimplemented (rare, pushed to Phase 6).

## Phase 3 — Standalone surface creation

`CreateRenderTarget`, `CreateDepthStencilSurface`, `CreateImageSurface` are stubs. Blocks shadow maps, extra render targets, offscreen buffers. Depends on Phase 2 (locking) to be useful.

Build on `BaseTexture::Create` + `GpuTexture::GetSurfaceLevel` — these are a single-mip-level, texture-less-wrapper variant of what `CreateTexture` already does; don't duplicate resource-desc/footprint logic.

## Phase 4 — State blocks

`BeginStateBlock`/`EndStateBlock`/`ApplyStateBlock`/`CaptureStateBlock`/`DeleteStateBlock` are unimplemented. `CreateStateBlock` currently returns `S_OK` without doing anything — this is worse than a stub because it's a *silent* lie. Fix immediately regardless of when the rest of Phase 4 lands: make it call `NOT_IMPLEMENTED()` like its siblings until real support exists.

Full implementation: snapshot/restore of `RenderState` (already has `operator==`, so it's POD-copyable), `TextureStageState[]`, transform matrices, active vertex/pixel shader, per-stage bound textures. The nontrivial part is *recording* mode: `BeginStateBlock`→`EndStateBlock` must capture only the state actually touched in that window, not the whole state (per D3D8 spec `D3DSBT_ALL` vs `D3DSBT_PIXELSTATE`/`D3DSBT_VERTEXSTATE` vs recorded-diff).

## Phase 5 — Hardware cursor

`SetCursorProperties`, `SetCursorPosition`, `ShowCursor` — thin wrappers over `SetCursor`/`SetCursorPos`/`ShowCursor` (WinAPI). Low game-compatibility impact (most games draw their own cursor) but cheap.

## Phase 6 — Long tail (implement reactively)

`ProcessVertices`, `DrawIndexedPrimitiveUP`, `DrawRectPatch`/`DrawTriPatch`/`DeletePatch` (RT-patches, rare outside old RTS titles), `Get/SetGammaRamp`, `CreateAdditionalSwapChain`, `GetFrontBuffer`, `GetCreationParameters`, `GetAvailableTextureMem`, `CreateVolumeTexture`. `NOT_IMPLEMENTED()` already logs file/line/function to `log.txt`, so implement these as specific games hit them rather than speculatively.

Texture-side long tail (lower priority, rarely called by games): `QueryInterface`, `GetDevice`, `Set/GetPrivateData`, `FreePrivateData`, `Set/GetPriority`, `GetType`, `Set/GetLOD` on `BaseTexture`.
