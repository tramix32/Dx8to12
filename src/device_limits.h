#pragma once

namespace Dx8to12 {
// Three rather than two so the CPU can stay a frame further ahead of the
// GPU. SubmitAndWait blocks on the fence for the back buffer it's about to
// reuse immediately after Present, so with only two buffers the CPU could
// never be more than one frame ahead -- effectively serializing CPU frame N+1
// against GPU frame N. Every per-slot array (command allocators, fence
// values, frame resource lists, slot generations) is sized off this constant
// and scales automatically.
static constexpr int kNumBackBuffers = 3;

static constexpr int kMaxVertexStreams = 16;
static constexpr int kMaxTexStages = 8;
static constexpr int kMaxActiveLights = 8;
static constexpr int kMaxUserClipPlanes = 8;  // Matches D3DCAPS8::MaxUserClipPlanes.

static constexpr int kMaxSamplerStates = 64;
// A real GTA: Vice City play session was observed issuing 1875+ CreateTexture
// calls with none released (every D3DPOOL_MANAGED texture keeps its SRV for
// its whole lifetime, and the game evidently keeps a fully-loaded level's
// textures all resident at once) -- comfortably exceeding the previous 1536,
// which hit pool_heap.cpp's "!free_list_.empty()" assert partway through a
// level load. Bumped with generous headroom rather than tuned to the exact
// observed count, since a bigger level/more art could need more still.
static constexpr int kMaxNumSrvs = 8192;
static constexpr int kMaxNumRtvs = 32;

// Was 40MB -- bumped since modern GPUs have far more VRAM to spare than
// when this was originally sized, and this buffer holds every dynamic
// vertex/index upload, DrawPrimitiveUP/DrawIndexedPrimitiveUP data, and
// CopyRects staging for the whole frame (see DynamicRingBuffer::Allocate's
// FAIL("OOM...") if it doesn't fit).
static constexpr int kDynamicRingBufferSize = 128 * 1024 * 1024;

static constexpr int kNumVsConstRegs = 96;
static constexpr int kNumPsConstRegs = 8;

// Helpful debug controls.

// Put non-dynamic vertex/index buffers in GPU-local memory (VRAM) with a
// CPU-visible staging copy, rather than keeping them in CPU-visible system
// memory that the GPU reads across PCIe.
//
// Off by default pending a retest on a freshly-booted machine. It doubles
// the committed resource count (each buffer gets its own 64KB-granular heap
// for both the VRAM copy and its staging copy), and the first GPU device
// removal of the session appeared shortly after it was first enabled --
// after which *every* subsequent capture failed, including builds from
// before any of this work, so the driver stayed wedged and the correlation
// could never be confirmed or ruled out. Worth revisiting: the win is
// GPU-side bandwidth, and measurements so far put this shim firmly CPU-bound
// (0% GPU fence wait), so there may be nothing to gain here anyway.
static constexpr bool kBuffersInGpuMemory = false;

// Keep buffers mapped for their whole lifetime instead of Map/Unmap around
// every Lock. Saves two driver calls per lock, but leaves graphics debuggers
// without the per-Unlock dirty range they use to track CPU writes, which
// makes them shadow and diff whole buffers instead -- suspected of breaking
// RenderDoc captures, so it's a switch until that's settled.
static constexpr bool kPersistentBufferMapping = true;

// Reuse the previous draw's PSO lookup, root signature binding and vertex
// buffer views when nothing they depend on changed. These are the only
// optimisations that skip *recording* commands, so they're the ones that
// could leave a command list depending on state it never set -- which is
// what a GPU device removal during capture replay looks like. Switchable
// while bisecting that.
static constexpr bool kCacheDrawStateBindings = true;

// Will implicitly disable Pso cache.
static constexpr bool kDisablePixelShaderCache = false;
static constexpr bool kDisablePsoCache = false;

// Does not bother keeping a CPU copy of managed resources. Frees up memory,
// helpful when trying to do a GPU capture.
static constexpr bool kDisableManagedResources = true;
}  // namespace Dx8to12
