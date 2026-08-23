#pragma once

namespace Dx8to12 {
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

static constexpr int kDynamicRingBufferSize = 40 * 1024 * 1024;

static constexpr int kNumVsConstRegs = 96;
static constexpr int kNumPsConstRegs = 8;

// Helpful debug controls.

// Will implicitly disable Pso cache.
static constexpr bool kDisablePixelShaderCache = false;
static constexpr bool kDisablePsoCache = false;

// Does not bother keeping a CPU copy of managed resources. Frees up memory,
// helpful when trying to do a GPU capture.
static constexpr bool kDisableManagedResources = true;
}  // namespace Dx8to12
