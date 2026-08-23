# DX8→DX12 coverage roadmap

Started 2026-08-23 at: `IDirect3DDevice8` has 100 methods; 50 have real implementations, 50 are stubs (`NOT_IMPLEMENTED()` → abort at runtime). `IDirect3DSurface8` only implements `GetDesc`. Prioritized by "how likely is this to crash a real game" vs. implementation cost.

Current state (2026-08-23, end of day): 93/100 `IDirect3DDevice8` methods implemented, 7 stubs remain (listed in Phase 6). `IDirect3DSurface8` fully implements `LockRect`/`UnlockRect`/`QueryInterface`/`GetDevice` on every surface kind (Phase 2). `BaseTexture` (both `IDirect3DTexture8` and `IDirect3DCubeTexture8`) fully implements `QueryInterface`/`GetDevice`/`SetPriority`/`GetPriority`/`GetType`/`SetLOD`/`GetLOD` (see below); only `GetContainer` on surfaces and the private-data trio remain unimplemented/no-op across both.

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

## Real-game feedback (2026-08-23): swap chain creation crash on `D3DFMT_X8R8G8B8`

Second reported crash, deeper into device init than the MSAA one: `ASSERT_HR(Init(presentParams))` in `Device::Create` (`device.cpp`) failing. Root cause: `Device::Init`'s `HR_OR_RETURN(dxgi_factory_->CreateSwapChainForHwnd(...))` was passing `DXGIFromD3DFormat(presentParams.BackBufferFormat)` straight through as the swap chain format. `D3DFMT_X8R8G8B8` — the single most common DX8 backbuffer format, used by nearly every game including both test titles — maps to `DXGI_FORMAT_B8G8R8X8_UNORM`, which `DXGI_SWAP_EFFECT_FLIP_DISCARD` swap chains (what this code uses) reject outright with `DXGI_ERROR_INVALID_CALL`; only the alpha-having variants (`R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `R16G16B16A16_FLOAT`) are valid for flip-model. This meant essentially every game requesting the default 32-bit backbuffer format would fail at device creation. `Device::Reset` had the identical bug in its `ResizeBuffers` call.

Fixed with a small `ToFlipModelSwapChainFormat` helper in `device.cpp` that substitutes `DXGI_FORMAT_B8G8R8A8_UNORM` whenever `DXGIFromD3DFormat` would otherwise produce the rejected X8 variant, applied at both call sites (`Init`'s `CreateSwapChainForHwnd` and `Reset`'s `ResizeTarget`/`ResizeBuffers`). Harmless for compatibility: `DXGIToD3DFormat` already maps `B8G8R8A8_UNORM` back to `D3DFMT_A8R8G8B8` for anything that queries the backbuffer format afterward (`GetAdapterDisplayMode`, `GetBackBuffer`'s surface desc, etc.) — the app simply never reads/writes the backbuffer's alpha channel through the X8 format, so gaining one it doesn't ask for is a no-op in practice. This is the standard trick used by essentially every DX8/DX9-on-DX12 or DX8/DX9-on-Vulkan wrapper for exactly this reason.

## Real-game feedback (2026-08-23): `CheckDeviceMultiSampleType`

Reported crash from an actual test run: `IDirect3D8::CheckDeviceMultiSampleType` was a bare `NOT_IMPLEMENTED()` stub, and a real game's adapter-capability probing hit it before device creation even started — this jumps ahead of the phased plan below because it's an empirically confirmed blocker, not a guess.

Fixed in `direct3d8.cpp`: only `D3DMULTISAMPLE_NONE` reports as available (`S_OK`); any actual multisample level returns `D3DERR_NOTAVAILABLE`. This isn't a stopgap — `Device::CreatePSO` always builds PSOs with `SampleDesc.Count = 1`, so there is no real MSAA implementation to honestly report "yes" for. Games probing MSAA levels top-down will fall back to no-AA and continue, instead of believing they got antialiasing they silently didn't. True MSAA support (rendering to a multisampled RT + resolve) would be new work, not covered by this fix — add it here as a later phase if a game turns out to depend on it rather than falling back gracefully.

## Phase 2 — Surface LockRect/UnlockRect (done, 2026-08-23)

Was the highest functional-impact gap: `BaseSurface` only implemented `GetDesc`, so screenshots, render-target readback, and `CreateImageSurface` all aborted.

- `GpuSurface::LockRect/UnlockRect` — on a `D3DPOOL_MANAGED` texture, delegates straight to the owning `GpuTexture`'s per-subresource `LockRect`/`UnlockRect` (already implemented in `texture.cpp`). On `D3DPOOL_DEFAULT` (lockable render targets), goes through the new GPU readback path below.
- `CpuSurface::LockRect/UnlockRect` — reads straight from the footprint/pointer captured at `GetSurfaceLevel` time (`compact_pitch_`/`data_ptr_`), no delegation needed.
- `BackbufferSurface::LockRect` and `GpuSurface::LockRect` (default-pool case) share `BaseSurface::LockGpuReadback`/`UnlockGpuReadback` (`surface.cpp`): create a `D3D12_HEAP_TYPE_READBACK` buffer sized via the new `BaseTexture::GetFootprint` accessor, `CopyTextureRegion` the subresource into it, then call `Device::SubmitAndWait(false)` to flush and block until done (reusing the exact flush-mid-frame path `Device::WaitForFrame` already relies on — no new fence code), then `Map` it. Read-path only; a locked render target is not expected to be written back through `Unlock`.
- `QueryInterface`, `GetDevice` on surfaces — implemented, mirror the equivalent `Device`/`Texture` patterns. `SetPrivateData`/`GetPrivateData`/`FreePrivateData` return `D3DERR_NOTAVAILABLE` (same choice already made for the palette APIs — no private-data storage exists, and returning a clean error is preferable to a fake success). `GetContainer` is still unimplemented (rare, pushed to Phase 6).

## Phase 3 — Standalone surface creation (done, 2026-08-23)

Was blocking shadow maps, extra render targets, offscreen buffers.

All three (`CreateRenderTarget`, `CreateDepthStencilSurface`, `CreateImageSurface`) build directly on `BaseTexture::Create` (single mip level, `D3DUSAGE_RENDERTARGET`/`D3DUSAGE_DEPTHSTENCIL`/`D3DPOOL_SYSTEMMEM` respectively) + `GetSurfaceLevel(0, ppSurface)` — no duplicated resource-desc/footprint logic. Requested `MultiSample != D3DMULTISAMPLE_NONE` logs and silently falls back to single-sample rather than failing, consistent with `CheckDeviceMultiSampleType` (see above) telling callers up front that only no-AA is available.

One easy-to-miss lifetime detail: `BaseTexture::Create` returns an object with an initial refcount of 1 that the app is normally expected to own directly (e.g. `CreateTexture`'s `*ppTexture = BaseTexture::Create(...)`). Here the app never sees the texture, only the surface wrapping it, and `GetSurfaceLevel` internally takes its *own* ref on the texture (`ComWrap` inside the `GpuSurface`/`CpuSurface` constructor) rather than adopting the caller's — so the initial ref has to be explicitly dropped (`ComOwn(texture)` locally, released when it goes out of scope) once the surface has taken its own, or the texture (and its GPU resource / descriptor slots) leaks for the lifetime of the process every single call.

## Real-game feedback (2026-08-23): pixel shader crash on the `_comp` (complement) source modifier

Crash: `ParseSourceParamToken` (`shader_parser.cpp`) FAILed on source modifier value 6 (`D3DSPSM_COMP`, the `1-x` "complement" modifier) for a `ps.1.x` instruction. `ParseShader` is shared by both vertex- and pixel-shader parsing, but the modifier switch only ever handled `D3DSPSM_NONE`/`D3DSPSM_NEG` — every other ps_1_x source modifier (`_bias`, `_bx2`, `_x2`, `_comp`, etc. — common in texture-blend instructions) hit the same `FAIL`.

Fixed by restructuring `ParseSourceParamToken` to build the register+swizzle expression into a local `std::ostringstream` first, then wrap it per-modifier once the modifier is known (previously it streamed straight into the shared `os`, which only allows appending a suffix — enough for `_neg` as `x*-1`, not enough for a modifier like `_comp` that needs to wrap the *whole* expression as `1-x`). Now handles: `NONE`, `NEG` (`-x`), `BIAS` (`x-0.5`), `BIASNEG` (`0.5-x`), `SIGN` (`2x-1`), `SIGNNEG` (`1-2x`), `COMP` (`1-x`), `X2` (`2x`), `X2NEG` (`-2x`). Left unimplemented (still `FAIL`s with a clear message): `DZ`/`DW` (divide-through-by-z/w, used by `texm3x2tex`-style instructions) — these need per-component access to the *same* source register rather than a simple expression wrap, and are rare enough in ps_1_x content to not be worth guessing at blind.

## Phase 4 — State blocks (done, 2026-08-23)

`BeginStateBlock`/`EndStateBlock`/`ApplyStateBlock`/`CaptureStateBlock`/`DeleteStateBlock`/`CreateStateBlock` are implemented via `Device::StateBlock` (`device.h`) — a full snapshot of `RenderState`, `TextureStageState[]`, transforms, material, lights, bound textures, bound vertex/pixel shader handle, and vertex shader constants, stored in `state_blocks_` keyed by a token.

**Documented simplification**: real D3D8 partitions state by `D3DSTATEBLOCKTYPE` (`D3DSBT_ALL`/`D3DSBT_PIXELSTATE`/`D3DSBT_VERTEXSTATE`) and, for a `Begin`/`End`-recorded block, captures only the states actually `Set()` during the recording window — applying the block later only touches those specific states, leaving everything else alone. This implementation always captures/restores the *entire* state listed above regardless of type or what was actually touched. That's harmless for the common case (games tend to save/restore a broad swath of state around a special effect anyway) but can restore more than a game expects if it specifically relies on the precise partitioning or on untouched states surviving an `ApplyStateBlock`. If a game's behavior depends on that, implementing real recording means hooking every `Set*` call to also write into a currently-recording `StateBlock` plus a per-field "was touched" flag — mechanical but touches ~15 call sites, noted here rather than done speculatively.

`ApplyState` stomps `dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES` unconditionally after restoring, forcing a full re-bind/re-upload before the next draw call rather than tracking exactly what changed — correct but not free; state blocks are not a hot path in typical usage, so this wasn't worth the bookkeeping to avoid.

## Real-game feedback (2026-08-23): `GetTextureStageState` stub, missing `D3DTSS_BORDERCOLOR`

Two more issues found while implementing Phase 4, both pre-existing (not introduced by earlier fixes) and both surfaced by continued real-game testing:

- `Device::GetTextureStageState` was declared `override` in `device.h` (so Phase 1 missed it, reading the header as "already implemented") but its body in `device.cpp` was a bare `NOT_IMPLEMENTED()`. Fixed to mirror `SetTextureStageState` via `TextureStageState::GetAtIndex`. Lesson: the `PURE`-macro coverage check only catches methods that never got a body at all — a method can still be declared `override` and silently stub itself in the `.cpp`. Worth a final sweep for other `NOT_IMPLEMENTED()` calls hiding behind an `override` declaration (see Phase 6).
- Crash: `TextureStageState::GetAtIndex` (`render_state.cpp`) had no case for `D3DTSS_BORDERCOLOR` (value 15) — any game calling `SetTextureStageState`/`GetTextureStageState` with border color (used for `D3DTADDRESS_BORDER` texture wrapping) hit the `FAIL("Unexpected texture stage state")` abort. Added a `border_color` field to `TextureStageState` and wired it into `SamplerDesc`'s `D3D12_SAMPLER_DESC::BorderColor` (`dx_utils.cpp`, via the existing `Dx8::Color` D3DCOLOR→float helper) — previously that field was a `{}`-zeroed `// TODO`, so border-addressed samplers were already silently rendering with black borders instead of the app-requested color even before this crash was reachable.

## Phase 5 — Hardware cursor, and a coverage sweep (done, 2026-08-23)

`SetCursorProperties`/`SetCursorPosition`/`ShowCursor` now wrap `SetCursorPos`/`ShowCursor` (WinAPI). `ShowCursor` tracks its own `cursor_visible_` bool rather than calling Win32 `ShowCursor` unconditionally, since D3D8's version returns the *previous* state and Win32's is a non-idempotent display counter — calling it on every same-value invocation would drift the counter and eventually strand the cursor hidden/shown.

While closing out the easy remaining gaps, also implemented in the same pass: `GetAvailableTextureMem` (fixed 256MB budget — we don't track VRAM usage, and games mostly treat this as a rough heuristic anyway), `GetCreationParameters` (derived from `adapter_index_`/`window_` plus the constants `CreateDevice` already asserts: always `D3DDEVTYPE_HAL` + `D3DCREATE_HARDWARE_VERTEXPROCESSING`), `GetDisplayMode` (delegates to the already-implemented `Direct3D8::GetAdapterDisplayMode`), `ValidateDevice` (always reports 1 pass — we never need multi-pass fallback), `GetCurrentTexturePalette` (now symmetric with `SetCurrentTexturePalette`'s `D3DERR_NOTAVAILABLE`), `SetClipStatus`/`GetClipStatus` (plain struct storage), `SetClipPlane`/`GetClipPlane`, and `SetPixelShaderConstant`/`GetPixelShaderConstant`.

**Bookkeeping-only, documented limitation**: `SetClipPlane`/`GetClipPlane` and `SetPixelShaderConstant`/`GetPixelShaderConstant` store values (so nothing aborts and `Get*` round-trips correctly) but do **not** affect rendering — there is no GPU-side user-clip-plane implementation, and `bound_ps_cregs_` is not wired into the pixel shader's constant buffer. On the pixel-shader-constant front specifically: `programmable_ps.hlsl` currently declares `cbuffer ConstantData : register(b10) { float4 c[NUM_VS_CONST_REGS]; }` — the *same* register slot and size as the vertex shader's constant buffer (`programmable_vs.hlsl`), meaning `c[N]` reads in a ps.1.x shader today actually read the **vertex** shader's constants, not the app's pixel shader constants. There's already an allocated-but-unused `ps_creg_cbuffer_` member (sized for `kNumPsConstRegs`) that looks like an abandoned start at fixing this properly. Wiring it up for real means: uploading `bound_ps_cregs_` into `ps_creg_cbuffer_` (mirroring how `ps_cbuffer_`/`vs_cbuffer_` already get uploaded), adding a new root-signature parameter bound at a distinct register (e.g. `b11`, pixel-visibility only) in `InitRootSignatures`, binding it via `SetGraphicsRootConstantBufferView` at draw time, and changing `programmable_ps.hlsl`'s `ConstantData` to read from that new slot instead of sharing `b10`. Deliberately **not** done here — it touches the root signature/PSO binding layout, which is high-blast-radius and impossible to verify without a real game exercising a ps.1.x shader that actually calls `SetPixelShaderConstant`, so it's left as a scoped, well-understood follow-up rather than guessed at blind.

## Proactive audit (2026-08-23): fixed-function pixel pipeline gaps

Given how well "check every `FAIL()` call against the real D3D8 enum" worked for finding the border-color and shader-modifier crashes, did the same sweep over `ff_pixel_shader.cpp` — the fixed-function texture-stage pixel pipeline, which is the *more* commonly exercised path than programmable ps.1.x shaders for games of this era, so gaps here are higher-probability hits than in the already-covered programmable path.

Found and fixed, all in `ff_pixel_shader.cpp`:
- `D3DTOP_*` (texture stage color/alpha op) coverage was missing `D3DTOP_ADDSIGNED2X`, `D3DTOP_SUBTRACT`, `D3DTOP_ADDSMOOTH`, `D3DTOP_BLENDDIFFUSEALPHA`, `D3DTOP_BLENDTEXTUREALPHAPM` — added all five (straightforward 2-argument HLSL expressions, same shape as the already-handled ops). Left unimplemented: `D3DTOP_LERP`, `D3DTOP_MULTIPLYADD`, `D3DTOP_PREMODULATE`, `D3DTOP_MODULATE*_ADD*`, `D3DTOP_BUMPENVMAP*` — these need a third argument (`D3DTA_*` via `D3DTSS_COLORARG0`/`ALPHAARG0`) that `ApplyOperation`/`GenerateArgValue` don't currently thread through at all; adding one of these means widening that plumbing, not just adding a switch case, so left for when a game actually needs one specifically.
- **Pre-existing bug, not something I introduced**: `D3DTOP_BLENDTEXTUREALPHA`/`D3DTOP_BLENDCURRENTALPHA` used the formula `arg1 + arg2*(1-alpha)` (missing `arg1*alpha`). Per the D3D8 spec all of `BLENDDIFFUSEALPHA`/`BLENDFACTORALPHA`/`BLENDTEXTUREALPHA`/`BLENDCURRENTALPHA` share the same `arg1*alpha + arg2*(1-alpha)` formula, differing only in where `alpha` comes from — the formula that was actually implemented (`arg1 + arg2*(1-alpha)`) belongs to `D3DTOP_BLENDTEXTUREALPHAPM`, the *premultiplied* variant (where `arg1` is deliberately not re-multiplied because the texture already has alpha baked in). Fixed all four non-PM ops to the correct shared formula, and added `BLENDTEXTUREALPHAPM` separately with the formula that was there before.
- `D3DCMP_*` (alpha test comparison function) was missing `D3DCMP_EQUAL`, `D3DCMP_NOTEQUAL`, `D3DCMP_GREATEREQUAL` — `GREATEREQUAL` in particular is a common alpha-cutoff comparison (e.g. foliage/vegetation alpha testing). Added all three.

Not a gap: the auto-texgen mode switch (`D3DTSS_TCI_*`) looked incomplete at first glance (only 3 of 4 modes handled) but the 4th, `D3DTSS_TCI_PASSTHRU`, is value `0` and structurally can't reach that switch (it's only entered when `texcoord_index & ~0xFFFF` is nonzero) — full coverage, no fix needed.

Also not implemented, lower priority: `D3DTA_TEMP` (`GenerateArgValue`'s arg-selector switch) — requires the same triadic-op / `D3DTSS_RESULTARG` plumbing as the ops above, so a standalone fix would be incomplete on its own.

## Phase 6 — Long tail (in progress, reactive)

Second pass (2026-08-23) knocked out the remaining cheap/self-contained ones:

- `MultiplyTransform` — composes the given matrix with the state's current one via `DirectX::SimpleMath::Matrix` (row-vector convention: input matrix applied first, then the existing state), then calls the existing `SetTransform`.
- `GetRasterStatus` — reports a canned "not in vblank, scanline 0" status; this backend has no access to real scanline/vblank timing to report honestly, but a plausible fixed answer beats aborting.
- `SetGammaRamp`/`GetGammaRamp` — plain storage (`gamma_ramp_`, initialized to an identity ramp in the constructor). Deliberately does **not** touch the real display gamma (`SetDeviceGammaRamp`) — that would affect the user's actual monitor output as a side effect of a rendering-compat shim, out of proportion to the value for a feature few games rely on for correctness.
- `DrawIndexedPrimitiveUP` — mirrors `DrawPrimitiveUP` (vertex upload via the dynamic ring buffer) combined with `DrawIndexedPrimitive`'s index-count-from-primitive-type logic, uploading both vertex and index data into the ring buffer per call. Known limitation: unlike `DrawPrimitiveUP`, does not rewrite `D3DPT_TRIANGLEFAN` (rewriting a fan's *index list* is more involved than rewriting a flat vertex list) — `PrepareDrawCall` already rejects fans outright, so this is a pre-existing constraint, not a new gap.

Third pass (2026-08-23) implemented the `Get*ShaderDeclaration`/`Get*ShaderFunction` family:

- `ParseShader` (`shader_parser.cpp`, shared by vertex and pixel shader parsing) now returns a pointer just past the `D3DSIO_END` token instead of `void`, so its callers know exactly how many DWORDs of the input token stream they consumed — reusing the parser's own instruction-length knowledge rather than re-implementing token-stream scanning (which would risk getting some opcode's operand count wrong and mis-copying).
- `VertexShader`/`PixelShader` (`vertex_shader.h`) gained `declaration_tokens`/`function_tokens` vectors, populated in `ParseProgrammableVertexShader`/`ParsePixelShader` from that returned end pointer, and in `Device::CreateVertexShader` for the declaration (scanned separately for its own `D3DVSD_END()` terminator, since the declaration and function are two independent token streams).
- `GetVertexShaderDeclaration`/`GetVertexShaderFunction`/`GetPixelShaderFunction` implement the standard D3D8 "pass `pData = nullptr` to query the required size, then call again with a big-enough buffer" pattern via a shared `CopyOutTokenBuffer` helper.
- Fixed-function shaders (created via the raw-FVF `SetVertexShader(fvf)` path, not `CreateVertexShader`) correctly return `D3DERR_INVALIDCALL` — they have no real declaration/function token stream to return, matching real D3D8 behavior for handles that aren't `CreateVertexShader`-created.

Remaining stubs, 7: `CreateAdditionalSwapChain`, `CreateVolumeTexture`, `DeletePatch`, `DrawRectPatch`/`DrawTriPatch` (RT-patch tessellation — genuinely complex, needs actual tessellation, not attempted blind), `GetFrontBuffer`, `ProcessVertices` (needs either CPU-side vertex shader emulation or a GPU stream-out path — substantial new work). `NOT_IMPLEMENTED()` already logs file/line/function to `log.txt`, so implement these as specific games hit them rather than speculatively.

Texture-side (done, 2026-08-23): `QueryInterface` (matches `IID_IDirect3DTexture8`/`IID_IDirect3DCubeTexture8` by `kind_`, plus the common `IUnknown`/`IDirect3DResource8`/`IDirect3DBaseTexture8` IIDs), `GetDevice`, `SetPriority`/`GetPriority` and `SetLOD`/`GetLOD` (bookkeeping only — no resource-eviction or mip-clamping implementation, we always upload/sample the full chain), `GetType` (derived from `kind_`). `Set/GetPrivateData`/`FreePrivateData` return `D3DERR_NOTAVAILABLE`, matching the same choice already made for surfaces and the palette APIs.

Surface-side, still open: `GetContainer` (would let a mip-level surface answer "what texture do I belong to" via `QueryInterface` on the owning texture — `GpuSurface`/`CpuSurface` already hold that pointer, so this is cheap for those two; left alone only because it's genuinely rare to call and `BackbufferSurface`'s notion of a "container" is ambiguous without a real `IDirect3DSwapChain8` object, which this codebase doesn't expose).
