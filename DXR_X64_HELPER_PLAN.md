# x64 DXR helper: architecture and delivery plan

## Why a separate process

The game and `d3d8.dll` must remain x86.  NVIDIA reports native DXR as Tier 0
to an x86 D3D12 process even on an RTX 4080, while an x64 D3D12 device on the
same adapter can expose DXR.  Therefore DXR cannot be added directly to the
shim.  `rt_helper/dx8to12_rt_helper.exe` is a separately configured x64
process; it never loads the game or the D3D8 interfaces.

## Non-negotiable boundary

```
GTA VC (x86) -> d3d8.dll (x86, D3D12 raster) -> shared D3D12 resources/fences
                                                        |
                                                        v
                                  dx8to12_rt_helper.exe (x64, same adapter, DXR)
```

The helper selects the *same* DXGI adapter by LUID, not merely the first
high-performance adapter.  It opens named shared resources and shared fences
on its own x64 D3D12 device.  GPU virtual addresses are process-local: only
resource IDs/descriptions travel through IPC, never addresses.

## Synchronisation and IPC

- **Control plane:** a named, versioned shared-memory mapping with fixed-width
  records and named events.  It carries adapter LUID, resource registration,
  frame IDs, transforms, material IDs and error/status messages.  No pointers,
  `size_t`, COM interfaces or Win32 handle values cross the 32/64-bit boundary.
- **Data plane:** named D3D12 shared resources.  The x86 side creates default
  heap geometry mirrors with `D3D12_HEAP_FLAG_SHARED`, then creates a named NT
  shared handle.  The x64 side calls `OpenSharedHandleByName`.
- **Ordering:** two named shared D3D12 fences.  x86 signals `geometry_ready`
  after copying its source VB/IB into shared default buffers; the helper queue
  waits before BLAS/TLAS work.  The helper signals `rt_ready` after writing a
  shared output texture; x86 waits only when consuming that texture.  CPU
  events only wake/control processes; they do not substitute for GPU fences.
- **Lifetime:** unregistering a resource is a two-phase protocol.  x86 first
  waits for helper acknowledgement and its fence value, then closes the named
  handle/releases the mirror.  A helper crash degrades cleanly to PerPixel;
  x86 never waits indefinitely during Present.

## Delivery sequence

### H0 — x64 capability/adapter proof (implemented)

`rt_helper` is an independent x64 CMake project.  Its `--self-test` creates
an x64 device on the high-performance adapter and reports adapter name/LUID,
OPTIONS5 HRESULT, RaytracingTier and Device5 availability.  This proves the
driver exposes DXR outside the x86 process before any IPC is introduced.

### H1 — process lifecycle and handshake

Add the fixed-width protocol header shared by both targets.  The x86 shim
launches the helper hidden with a unique session nonce, sends its adapter LUID
and waits briefly for an `x64-ready` handshake.  A LUID mismatch, x64 Tier 0,
timeout or helper exit leaves `LightingMode` at PerPixel and emits one compact
diagnostic; gameplay continues.

### H2 — shared-resource smoke test

Create/open one named shared default-heap buffer and both shared fences.  The
x86 side copies a known pattern, signals; x64 waits and validates/copies it
back through a second shared resource.  Verify fence ordering and cleanup
through repeated Reset/helper-restart tests.  Do not involve scene draws yet.

### H3 — static geometry export and helper BLAS/TLAS

For non-dynamic VB/IB draws, allocate a shareable GPU-default mirror and copy
only on `content_generation` changes.  Register immutable resource IDs with
the helper and transmit per-frame instances (world transform, index span,
material/SRV ID).  The helper caches BLAS by resource ID+generation and builds
TLAS per frame.  This is still status-only: no ray output reaches the game.

### H4 — shared RT output/composite proof

Helper writes a simple ray output to a named shared texture and signals
`rt_ready`; x86 opens it as an SRV and composites a debug overlay only after
the matching fence.  This validates cross-process image sharing, state
transitions and frame latency before lighting is changed.

### H5 — RT shadows, then reflections/GI

Only after H4: export camera/light/material data, generate shadow visibility
in x64, and multiply it into the x86 PerPixel path.  Reflections and GI need
their own history/denoising/output protocol and are separate milestones.

## Explicit initial constraints

- One adapter only; no cross-adapter heaps or fences.
- Static triangle lists first; dynamic geometry, UI and instanced special
  cases are excluded until their mirror/update contracts exist.
- No GPU virtual addresses, descriptor handles or raw `HANDLE` values in IPC.
- All resources are bounded and resource registration is rate-limited to
  avoid exhausting VRAM during Vice City streaming.
- H0--H3 have no visible rendering change.  H4 is the first opt-in visual
  diagnostic; H5 changes lighting.
