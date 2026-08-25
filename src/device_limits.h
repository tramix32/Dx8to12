#pragma once

namespace Dx8to12 {
// Back to two. Three would let the CPU run a frame further ahead of the GPU,
// but measurements put this shim firmly CPU-bound (0% of frame spent waiting
// on the GPU fence), so there was nothing to win -- and with
// DXGI_SWAP_EFFECT_FLIP_DISCARD a back buffer's contents are undefined after
// Present, so any screen the game only partially redraws (menus lean on this)
// shows whatever was in that buffer N frames ago. Going from 2 to 3 made that
// stale content older and the resulting flicker visible.
static constexpr int kNumBackBuffers = 2;

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

// Skip zero-filling a dynamic buffer's speculative write cache on
// D3DLOCK_DISCARD. Saves a memset per discard lock, but leaves whatever the
// previous frame wrote in any part of the locked range the app doesn't fill
// in this time -- where it used to read as zeroes. Off while investigating
// intermittent garbage geometry (a glitch that vanishes under a graphics
// debugger, which points at reads of undefined memory).
static constexpr bool kSkipDiscardZeroFill = false;

// Resize the app's window to exactly cover its monitor when it asks for a
// fullscreen device (D3DPRESENT_PARAMETERS::Windowed == FALSE). Real D3D8
// switched the display mode for such a request, so the window ended up
// covering the screen precisely; this shim never does, which leaves the app's
// own window sizing built on assumptions that no longer hold -- GTA: Vice
// City ends up with a 2576x1479 client area on a 2560x1440 monitor (its
// AdjustWindowRect added room for a title bar and borders the window doesn't
// have), so the picture runs off the right and bottom edges.
//
// A switch because touching the app's window makes Windows dispatch
// WM_SIZE/WM_WINDOWPOSCHANGED into its WndProc, and this codebase has already
// hit one crash that way (see the ResizeTarget comment in Device::Reset).
static constexpr bool kResizeWindowForFullscreen = true;

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
