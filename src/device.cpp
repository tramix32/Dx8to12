#include "device.h"

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <sstream>
#include <utility>

#include "SimpleMath.h"
#include "aixlog.hpp"
#include "buffer.h"
#include "dynamic_ring_buffer.h"
#include "shader_parser.h"
#include "surface.h"
#include "texture.h"
#include "utils/dx_utils.h"
#include "vertex_shader.h"

#ifdef DX8TO12_USE_ALLOCATOR
#include "D3D12MemAlloc.h"
#endif

#undef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL            \
  []() {                              \
    LOG_ERROR() << "Invalid call!\n"; \
    return MAKE_D3DHRESULT(2156);     \
  }()

#define SCOPED_MARKER(annotation) ScopedGpuMarker(cmd_list_.Get(), annotation)

namespace Dx8to12 {

// The single live Device instance, exposed to dx8to12_api.cpp so the C mod
// API can reach it without every caller needing a Device* of their own.
// There is only ever one Device (one D3D12 device/swap chain per process),
// set at the end of Device::Create and cleared in ~Device.
static Device *g_current_device = nullptr;

Device *GetCurrentDeviceForModApi() { return g_current_device; }

// static_assert(sizeof(void *) == 4, "Does not support 64-bit.");

// DXGI_SWAP_EFFECT_FLIP_DISCARD swap chains only accept a handful of formats,
// none of which lack an alpha channel. D3DFMT_X8R8G8B8 -- by far the most
// common DX8 backbuffer format -- maps to DXGI_FORMAT_B8G8R8X8_UNORM via
// DXGIFromD3DFormat, which CreateSwapChainForHwnd/ResizeBuffers reject
// outright (DXGI_ERROR_INVALID_CALL), aborting device creation. Swap in the
// alpha variant for the swap chain itself; games never read/write backbuffer
// alpha through the X8 format anyway.
// D3D8's swap effect says whether the app may rely on the back buffer still
// holding the previous frame after Present. D3DSWAPEFFECT_DISCARD explicitly
// does not; FLIP and COPY (and COPY_VSYNC) do. Hardcoding FLIP_DISCARD for
// all of them means an app that legitimately renders only the part of the
// screen that changed -- which menus commonly do -- gets whatever happened to
// be left in that buffer instead of its previous frame, showing up as
// flickering remnants of older frames.
static DXGI_SWAP_EFFECT ToDxgiSwapEffect(D3DSWAPEFFECT d3d8_swap_effect) {
  switch (d3d8_swap_effect) {
    case D3DSWAPEFFECT_FLIP:
    case D3DSWAPEFFECT_COPY:
    case D3DSWAPEFFECT_COPY_VSYNC:
      // Preserves back buffer contents across Present, unlike FLIP_DISCARD.
      return DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    case D3DSWAPEFFECT_DISCARD:
    default:
      return DXGI_SWAP_EFFECT_FLIP_DISCARD;
  }
}

static DXGI_FORMAT ToFlipModelSwapChainFormat(DXGI_FORMAT format) {
  if (format == DXGI_FORMAT_B8G8R8X8_UNORM) return DXGI_FORMAT_B8G8R8A8_UNORM;
  return format;
}

// Backs IDirect3DDevice8::CreateAdditionalSwapChain. Only a thin wrapper: its
// backbuffers are plain GpuTextures/GpuSurfaces, so the app renders to them
// through the normal Device::SetRenderTarget/Draw*/etc. path -- this class
// only needs to own the second DXGI swap chain and know how to flush +
// present it, reusing Device's existing command-submission machinery rather
// than duplicating it.
class AdditionalSwapChain : public IDirect3DSwapChain8, public RefCounted {
 public:
  AdditionalSwapChain(Device *device, ComPtr<IDXGISwapChain3> swap_chain,
                      std::vector<ComPtr<GpuTexture>> back_buffers)
      : device_(device),
        swap_chain_(std::move(swap_chain)),
        back_buffers_(std::move(back_buffers)),
        current_index_(swap_chain_->GetCurrentBackBufferIndex()) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObj) override {
    if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain8) {
      *ppvObj = static_cast<IDirect3DSwapChain8 *>(this);
      AddRef();
      return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return RefCounted::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override {
    return RefCounted::Release();
  }

  HRESULT STDMETHODCALLTYPE Present(CONST RECT *pSourceRect,
                                    CONST RECT *pDestRect,
                                    HWND hDestWindowOverride,
                                    CONST RGNDATA *pDirtyRegion) override {
    // Same simplifications as Device::Present: no partial-rect presentation.
    ASSERT(pSourceRect == nullptr && pDestRect == nullptr &&
           pDirtyRegion == nullptr);
    (void)hDestWindowOverride;
    device_->TransitionTexture(back_buffers_[current_index_].get(), 0,
                               D3D12_RESOURCE_STATE_PRESENT);
    // Flush the shared command list (without presenting the *primary* swap
    // chain), then present this one specifically.
    device_->SubmitAndWait(false);
    ASSERT_HR(swap_chain_->Present(
        device_->sync_interval(),
        device_->sync_interval() == 0 && device_->tearing_supported()
            ? DXGI_PRESENT_ALLOW_TEARING
            : 0));
    current_index_ = swap_chain_->GetCurrentBackBufferIndex();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer,
                                          D3DBACKBUFFER_TYPE Type,
                                          IDirect3DSurface8 **ppBackBuffer)
      override {
    ASSERT(Type == D3DBACKBUFFER_TYPE_MONO);
    if (BackBuffer >= back_buffers_.size()) return D3DERR_INVALIDCALL;
    *ppBackBuffer = new GpuSurface(device_, back_buffers_[BackBuffer].get(), 0);
    return S_OK;
  }

 private:
  Device *device_;
  ComPtr<IDXGISwapChain3> swap_chain_;
  std::vector<ComPtr<GpuTexture>> back_buffers_;
  UINT current_index_;
};

// Implements the standard D3D8 "query size, then fetch" pattern used by
// GetVertexShaderDeclaration/GetVertexShaderFunction/GetPixelShaderFunction:
// if pData is null, just report the required size; otherwise copy up to
// whatever size the caller already put in *pSizeOfData.
static HRESULT CopyOutTokenBuffer(const std::vector<DWORD> &tokens,
                                  void *pData, DWORD *pSizeOfData) {
  const DWORD available_bytes = safe_cast<DWORD>(tokens.size() * sizeof(DWORD));
  if (pData == nullptr) {
    *pSizeOfData = available_bytes;
    return S_OK;
  }
  const DWORD bytes_to_copy = std::min(*pSizeOfData, available_bytes);
  memcpy(pData, tokens.data(), bytes_to_copy);
  *pSizeOfData = bytes_to_copy;
  return S_OK;
}

Device::DirtyFlags &operator|=(Device::DirtyFlags &a, Device::DirtyFlags b) {
  a = static_cast<Device::DirtyFlags>(static_cast<uint32_t>(a) |
                                      static_cast<uint32_t>(b));
  return a;
}

Device::DirtyFlags &operator^=(Device::DirtyFlags &a, Device::DirtyFlags b) {
  a = static_cast<Device::DirtyFlags>(static_cast<uint32_t>(a) ^
                                      static_cast<uint32_t>(b));
  return a;
}

Device::Device(IDirect3D8 *direct3d8)
    : ref_count_(1), direct3d8_(ComWrap(direct3d8)) {
  // Set some default state for the first texture stage.
  texture_stage_states_[0].color_op = D3DTOP_MODULATE;
  texture_stage_states_[0].alpha_op = D3DTOP_SELECTARG1;
  for (size_t i = 0; i < texture_stage_states_.size(); ++i) {
    texture_stage_states_[i].texcoord_index = static_cast<DWORD>(i);
  }
  for (int i = 0; i < 256; ++i) {
    const WORD identity = static_cast<WORD>(i * 257);
    gamma_ramp_.red[i] = identity;
    gamma_ramp_.green[i] = identity;
    gamma_ramp_.blue[i] = identity;
  }
}

HRESULT STDMETHODCALLTYPE Device::QueryInterface(REFIID riid, void **ppvObj) {
  if (ppvObj == nullptr)
    return E_POINTER;
  else if (riid == IID_IDirect3DDevice8 || riid == __uuidof(IUnknown)) {
    AddRef();
    *ppvObj = static_cast<IDirect3DDevice8 *>(this);
    return S_OK;
  } else {
    // Querying for an interface this object doesn't implement is normal COM
    // usage (e.g. defensive interface probing by middleware) -- it isn't an
    // error condition worth crashing over.
    *ppvObj = nullptr;
    return E_NOINTERFACE;
  }
}

static void __stdcall DebugInfoQueueMessageCallback(
    D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id, LPCSTR pDescription, void *pContext) {
  ASSERT(pDescription);
  AixLog::Severity log_severity;
  switch (severity) {
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
      log_severity = AixLog::Severity::debug;
      break;
    case D3D12_MESSAGE_SEVERITY_INFO:
      log_severity = AixLog::Severity::info;
      break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
      log_severity = AixLog::Severity::warning;
      break;
    case D3D12_MESSAGE_SEVERITY_ERROR:
      log_severity = AixLog::Severity::error;
      break;
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
      log_severity = AixLog::Severity::fatal;
      break;
  }
  OutputDebugStringA(pDescription);
  LOG(log_severity) << pDescription << "\n";
  // Only CORRUPTION (actual GPU/driver memory corruption -- vanishingly rare
  // and always worth stopping for) is fatal. ERROR-severity messages used to
  // abort too, which is right for catching *our own* bugs during
  // development, but wrong for a game the user is actually trying to play:
  // third-party overlays (RTSS/Afterburner-style FPS OSDs, screenshot tools)
  // hook Present/ExecuteCommandLists and can trip the validation layer with
  // false positives that have nothing to do with this codebase -- observed
  // in practice as "PSO deleted while still referenced by the command list"
  // exactly when such an overlay was active, reproducibly gone once it was
  // closed. Logging (still visible in log.txt for real bugs) without
  // aborting lets the game keep running through those instead of hard
  // crashing over someone else's hook.
  if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
    FAIL("D3D12 Error:\r\n%s", pDescription);
  }
}

bool Device::Create(HWND window, ComPtr<IDXGIFactory2> factory,
                    ComPtr<IDXGIAdapter> adapter, int adapter_index,
                    const D3DPRESENT_PARAMETERS &presentParams) {
  window_ = window;
  dxgi_factory_ = std::move(factory);

  {
    ComPtr<IDXGIFactory5> factory5;
    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(dxgi_factory_->QueryInterface(
            IID_PPV_ARGS(factory5.GetForInit()))) &&
        SUCCEEDED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
            sizeof(allow_tearing)))) {
      tearing_supported_ = allow_tearing;
    }
  }

  LOG(INFO) << "Creating device.\n";
#ifdef DX8TO12_ENABLE_VALIDATION
  ID3D12Debug *debug_iface = nullptr;
  ASSERT_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_iface)));
  ASSERT_HR(
      debug_iface->QueryInterface(IID_PPV_ARGS(debug_interface_.GetForInit())));
  debug_iface->Release();
  // NOTE: EnableDebugLayer() is deliberately NOT called here. It has to run
  // before any D3D12 device exists (including the cached probe devices
  // GetProbeDeviceFor creates for CheckDeviceType/CheckDeviceFormat), so it
  // lives in the Direct3D8 constructor -- see the comment there.
  // debug_interface_->SetEnableSynchronizedCommandQueueValidation(TRUE);
  // GPU-based validation is much heavier than the regular debug layer (shader
  // instrumentation on every draw/copy) -- it's the likely cause of very low
  // FPS even in menus, and its validation runs asynchronously relative to the
  // CPU submission that triggered it, which can surface as a message
  // hundreds of ms after the actual call (observed: a "Command lists must be
  // successfully closed" error logged ~350ms after the last real
  // ExecuteCommandLists, with nothing logged in between) -- misleading when
  // chasing a crash via checkpoint logging, since the real cause isn't the
  // most recently logged call. EnableDebugLayer() alone (kept, no perf cost
  // even close to GBV's) already catches the vast majority of real bugs this
  // project has actually been fixed from (resource-state validation, leaked
  // descriptors, etc.) -- turn this back on only if specifically chasing a
  // GPU-side corruption/UAV-hazard bug that plain validation can't see.
  // debug_interface_->SetEnableGPUBasedValidation(TRUE);
  // debug_interface_->SetEnableAutoName(TRUE);
#endif

  adapter_ = std::move(adapter);
  adapter_index_ = adapter_index;
  ASSERT(adapter_);
  {
    // DXGI_ERROR_DEVICE_HUNG/DEVICE_RESET (0x887A0006/0x887A0007) here mean
    // the OS already has the adapter marked as needing to be reopened --
    // typically a transient leftover from another D3D12 app (e.g. RenderDoc,
    // or this same process on a previous run) closing right before this
    // call. There's nothing about *this* call that caused it, but retrying
    // after a short wait commonly clears it without needing a manual
    // graphics-driver restart, so do that a few times before giving up.
    constexpr int kMaxAttempts = 5;
    HRESULT hr = S_OK;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      hr = D3D12CreateDevice(adapter_.get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(d3d12_device_.GetForInit()));
      if (hr == S_OK) break;
      if ((hr != DXGI_ERROR_DEVICE_HUNG && hr != DXGI_ERROR_DEVICE_RESET) ||
          attempt == kMaxAttempts) {
        break;
      }
      LOG(AixLog::Severity::error)
          << "D3D12CreateDevice failed with " << std::hex << hr << std::dec
          << " (attempt " << attempt << "/" << kMaxAttempts
          << ") -- retrying shortly.\n";
      Sleep(400);
    }
    if (hr != S_OK) {
      FAIL("Failed to create device: %d", hr);
      return false;
    }
  }
  // TODO: Pass in adapter output.
  // ASSERT_HR(adapter_->EnumOutputs(0, adapter_output_.GetForInit()));

// Create info queue.
#ifdef DX8TO12_ENABLE_VALIDATION
  if (SUCCEEDED(d3d12_device_->QueryInterface(
          IID_PPV_ARGS(info_queue_.GetForInit()))))
    info_queue_->RegisterMessageCallback(DebugInfoQueueMessageCallback,
                                         D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                         nullptr, &info_queue_cookie_);
#endif

  // D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12;
  // ASSERT_HR(d3d12_device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,
  //                                              &options12,
  //                                              sizeof(options12)));
  // ASSERT(options12.EnhancedBarriersSupported);

  ASSERT_HR(Init(presentParams));
  g_current_device = this;
  LOG(INFO) << "Create: done, returning to Direct3D8::CreateDevice()\n";
  return true;
}

namespace {
// Present() previously always used SyncInterval=1 (vsync forced on)
// regardless of what the app actually requested, leaving the GPU idle
// waiting for vblank between frames -- observed in practice as low
// GPU/CPU utilization alongside a mediocre framerate. Map the app's real
// request instead.
UINT SyncIntervalFromD3DPresentInterval(DWORD present_interval) {
  switch (present_interval) {
    case D3DPRESENT_INTERVAL_IMMEDIATE:
      return 0;
    case D3DPRESENT_INTERVAL_TWO:
      return 2;
    case D3DPRESENT_INTERVAL_THREE:
      return 3;
    case D3DPRESENT_INTERVAL_FOUR:
      return 4;
    case D3DPRESENT_INTERVAL_DEFAULT:
    case D3DPRESENT_INTERVAL_ONE:
    default:
      return 1;
  }
}
}  // namespace

HRESULT Device::Init(const D3DPRESENT_PARAMETERS &presentParams) {
  fence_values_ = {};
  next_fence_ = 1;
  sync_interval_ = SyncIntervalFromD3DPresentInterval(
      presentParams.FullScreen_PresentationInterval);

  srv_heap_ = DescriptorPoolHeap(
      d3d12_device_.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxNumSrvs);
  rtv_heap_ = DescriptorPoolHeap(d3d12_device_.get(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxNumRtvs);
  dsv_heap_ = DescriptorPoolHeap(d3d12_device_.get(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kMaxNumRtvs);
  sampler_heap_ =
      DescriptorPoolHeap(d3d12_device_.get(),
                         D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kMaxSamplerStates);

  dynamic_ring_buffer_ = std::make_unique<DynamicRingBuffer>(
      d3d12_device_.get(), kDynamicRingBufferSize);

  dynamic_ring_buffer_->SetCurrentFrame(CurrentFrame());

#ifdef DX8TO12_USE_ALLOCATOR
  {
    D3D12MA::ALLOCATOR_DESC desc{.pDevice = d3d12_device_.get(),
                                 .PreferredBlockSize = 2 * 1024 * 1024,
                                 .pAdapter = adapter_.get()};
    ASSERT_HR(D3D12MA::CreateAllocator(&desc, allocator_.GetForInit()));
  }
#endif

  if (presentParams.EnableAutoDepthStencil) {
    LOG(INFO) << "Auto depth stencil.\n";
    D3DFORMAT depth_format = presentParams.AutoDepthStencilFormat;
    if (depth_format == D3DFMT_UNKNOWN) depth_format = D3DFMT_D32;
    ASSERT(depth_format == D3DFMT_D16 || depth_format == D3DFMT_D32 ||
           depth_format == D3DFMT_D24S8 || depth_format == D3DFMT_D24X8 ||
           depth_format == D3DFMT_D24X4S4);
    depth_stencil_tex_ = ComOwn(static_cast<GpuTexture *>(BaseTexture::Create(
        this, TextureKind::Texture2d, presentParams.BackBufferWidth,
        presentParams.BackBufferHeight, 1, 1, D3DUSAGE_DEPTHSTENCIL,
        depth_format, D3DPOOL_DEFAULT)));
    // Per the D3D8 spec, D3DRS_ZENABLE's default value is D3DZB_TRUE when
    // EnableAutoDepthStencil is set (D3DZB_FALSE otherwise, which is
    // RenderState's default member value already). A game that doesn't
    // explicitly SetRenderState(D3DRS_ZENABLE, ...) -- reasonable, since it
    // asked for an auto depth-stencil buffer specifically to get this
    // default -- would otherwise silently render with depth testing off:
    // no crash, just badly wrong draw order/z-fighting.
    render_state_.zbuffer_type = D3DZB_TRUE;
  }

  viewport_.Width = static_cast<float>(presentParams.BackBufferWidth);
  viewport_.Height = static_cast<float>(presentParams.BackBufferHeight);

  caps_ = GetDefaultCaps(static_cast<UINT>(adapter_index_));

  // Create command queue.
  D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
      .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
      .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
      .NodeMask = 0};
  ASSERT_HR(d3d12_device_->CreateCommandQueue(
      &cmd_queue_desc, IID_PPV_ARGS(cmd_queue_.GetForInit())));
  for (auto &allocator : cmd_allocators_) {
    ASSERT_HR(d3d12_device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetForInit())));
  }
  ASSERT_HR(d3d12_device_->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocators_[0].get(), nullptr,
      IID_PPV_ARGS(cmd_list_.GetForInit())));
  dirty_flags_ ^= DIRTY_FLAG_CMD_LIST_CLOSED;
  ASSERT_HR(d3d12_device_->CreateFence(
      0, D3D12_FENCE_FLAG_NONE,
      IID_PPV_ARGS(cmd_list_done_fence_.GetForInit())));
  cmd_list_done_event_handle_ =
      CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  ASSERT(cmd_list_done_event_handle_ != INVALID_HANDLE_VALUE);

  // Create the swap chain.
  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{
      .Width = presentParams.BackBufferWidth,
      .Height = presentParams.BackBufferHeight,
      .Format = ToFlipModelSwapChainFormat(
          DXGIFromD3DFormat(presentParams.BackBufferFormat)),
      .SampleDesc = {.Count = 1, .Quality = 0},
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = kNumBackBuffers,
      // Stretch, not DXGI_SCALING_NONE: the back buffer is sized to whatever
      // resolution the game asked for, which routinely differs from the size
      // of the window it's presenting into. NONE means "don't scale" -- DXGI
      // puts the buffer in the window's top-left corner at 1:1 and leaves the
      // rest blank, which is what made a game running at, say, 1920x1080 on a
      // 2560x1440 window render into just part of the screen. Real D3D8
      // changed the display mode for fullscreen instead, so the picture
      // always filled the screen; STRETCH is the flip-model equivalent.
      .Scaling = DXGI_SCALING_STRETCH,
      .SwapEffect = ToDxgiSwapEffect(presentParams.SwapEffect),
      .Flags = tearing_supported_
                   ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
                   : 0u,
  };
  LOG(INFO) << "Swap chain: app requested D3DSWAPEFFECT " << std::dec
            << presentParams.SwapEffect << " -> DXGI swap effect "
            << swap_chain_desc.SwapEffect << ".\n";
  // Don't crash if creating the swap chain fails. This might happen during
  // device reset.
  ComPtr<IDXGISwapChain1> swap_chain1;
  HR_OR_RETURN(dxgi_factory_->CreateSwapChainForHwnd(
      cmd_queue_.get(), window_, &swap_chain_desc, nullptr, nullptr,
      swap_chain1.GetForInit()));
  ASSERT_HR(swap_chain1->QueryInterface(swap_chain_.GetForInit()));

  // Without this, DXGI keeps monitoring `window_` itself -- intercepting
  // Alt+Enter and reacting to window state changes -- which real D3D8 never
  // did. A game written against real D3D8 doesn't expect DXGI to be
  // synchronously interacting with its window at all, and unexpected
  // reentrancy into the game's own WndProc during our swap chain setup is a
  // plausible source of otherwise-unexplained corruption/crashes shortly
  // afterward. Opt out of all of DXGI's automatic window handling.
  ASSERT_HR(dxgi_factory_->MakeWindowAssociation(
      window_, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER |
                   DXGI_MWA_NO_PRINT_SCREEN));

  current_back_buffer_ = swap_chain_->GetCurrentBackBufferIndex();

  // Create the back buffer.
  ASSERT(presentParams.BackBufferCount <= 1);
  ASSERT(back_buffers_.empty());
  for (uint32_t i = 0; i < swap_chain_desc.BufferCount; ++i) {
    ComPtr<ID3D12Resource> back_buffer_resource;
    ASSERT_HR(swap_chain_->GetBuffer(
        i, IID_PPV_ARGS(back_buffer_resource.GetForInit())));
    GpuTexture *back_buffer =
        GpuTexture::InitFromResource(this, back_buffer_resource);
    back_buffers_.push_back(ComOwn(back_buffer));
  }

  D3DPRESENT_PARAMETERS params = presentParams;
  ASSERT_HR(Reset(&params));

  InitRootSignatures();

  // Real D3D8 devices come out of CreateDevice with a default vertex format
  // already active (D3DFVF_XYZ -- untransformed position only), not with no
  // format set at all. Confirmed by comparing against d3d8to9 (a known-good
  // D3D8-on-D3D9 wrapper), which explicitly calls SetFVF(D3DFVF_XYZ) right
  // after constructing its device wrapper, before returning it to the app.
  // Without this, bound_vertex_shader_ defaults to 0 with no corresponding
  // entry in vertex_shaders_ (SetVertexShader was never actually called),
  // which is a real, observable difference from every other implementation
  // a game might have been tested against -- some games rely on a default
  // format being active before they ever call SetVertexShader/SetFVF
  // themselves.
  ASSERT_HR(SetVertexShader(D3DFVF_XYZ));

  LOG(INFO) << "Init: done, returning to Create()\n";
  return S_OK;
}

Device::~Device() {
  WaitForFrame(next_fence_ - 1);
  if (g_current_device == this) g_current_device = nullptr;
}

DXGI_FORMAT Device::backbuffer_format() const {
  return back_buffers_.at(current_back_buffer_).get()->resource_desc().Format;
}

void Device::RegisterModRenderCallback(ModRenderCallback callback) {
  if (std::find(mod_render_callbacks_.begin(), mod_render_callbacks_.end(),
                callback) != mod_render_callbacks_.end()) {
    return;
  }
  mod_render_callbacks_.push_back(callback);
}

void Device::UnregisterModRenderCallback(ModRenderCallback callback) {
  std::erase(mod_render_callbacks_, callback);
}

HRESULT STDMETHODCALLTYPE
Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  TRACE_ENTRY(pPresentationParameters);
  sync_interval_ = SyncIntervalFromD3DPresentInterval(
      pPresentationParameters->FullScreen_PresentationInterval);
  if (!(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED)) {
    LOG(INFO) << "Resetting device: Submitting commands..\n";
    SubmitAndWait(false);
    LOG(INFO) << "Reset: WaitForFrame(next_fence_ - 1)\n";
    WaitForFrame(next_fence_ - 1);
    LOG(INFO) << "Reset: cmd_list_->Close()\n";
    ASSERT_HR(cmd_list_->Close());
    dirty_flags_ |= DIRTY_FLAG_CMD_LIST_CLOSED;
  } else {
    LOG(INFO) << "Resetting device. Commands already submitted.\n";
  }
  LOG(INFO) << "Reset: releasing old back buffers/depth-stencil\n";
  // These caches (see the comment on their declaration in device.h) hold
  // their own ref on the depth-stencil/render-target texture via the
  // GpuSurface they wrap, on top of the refs checked below. Left in place
  // across a Reset, that extra ref keeps the old texture's total_ref_count()
  // at 2 forever, which silently defeats the asserts right after this (the
  // user can Ignore through the message box) and, worse, means the old
  // GpuTexture's destructor -- and the DSV/RTV descriptor it frees -- never
  // runs. Repeated Reset() calls then permanently burn one dsv_heap_/
  // rtv_heap_ slot each, eventually exhausting the 32-slot pool.
  cached_render_target_surface_.Reset();
  cached_render_target_surface_key_ = nullptr;
  cached_depth_stencil_surface_.Reset();
  cached_depth_stencil_surface_key_ = nullptr;
  bound_render_target_.Reset();
  bound_depth_target_.Reset();
  ASSERT(depth_stencil_tex_->total_ref_count() == 1);
  for (auto &rtv : back_buffers_) {
    ASSERT(rtv->total_ref_count() == 1);
  }
  back_buffers_.clear();
  depth_stencil_tex_.Reset();
  DXGI_FORMAT new_format = ToFlipModelSwapChainFormat(
      DXGIFromD3DFormat(pPresentationParameters->BackBufferFormat));

  // Deliberately not calling IDXGISwapChain::ResizeTarget here: for a
  // windowed swap chain (this codebase never calls SetFullscreenState, so
  // that's always the case), ResizeTarget actually moves/resizes the target
  // *window* itself via SetWindowPos -- real D3D8 never touched the app's
  // window like that. That SetWindowPos synchronously dispatches
  // WM_WINDOWPOSCHANGING/CHANGED/SIZE to the window's own WndProc, and a
  // real crash log (GTA: Vice City) showed exactly this: our code calling
  // into the game's WndProc during this Reset, which then null-derefs
  // because the game evidently isn't ready to handle a resize message this
  // early in its own init sequence. ResizeBuffers alone is enough to make
  // the swap chain match the window's existing size -- we're not the one
  // deciding the window should move or resize.
  LOG(INFO) << "Reset: swap_chain_->ResizeBuffers()\n";
  ASSERT_HR(swap_chain_->ResizeBuffers(
      kNumBackBuffers, pPresentationParameters->BackBufferWidth,
      pPresentationParameters->BackBufferHeight, new_format,
      tearing_supported_
          ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
          : 0u));
  LOG(INFO) << "Reset: swap_chain_->ResizeBuffers() done\n";

  DXGI_SWAP_CHAIN_DESC swap_chain_desc;
  ASSERT_HR(swap_chain_->GetDesc(&swap_chain_desc));

  if (pPresentationParameters->EnableAutoDepthStencil) {
    LOG(INFO) << "Reset: creating depth-stencil texture\n";
    D3DFORMAT depth_format = pPresentationParameters->AutoDepthStencilFormat;
    if (depth_format == D3DFMT_UNKNOWN) depth_format = D3DFMT_D32;
    ASSERT(depth_format == D3DFMT_D16 || depth_format == D3DFMT_D32 ||
           depth_format == D3DFMT_D24S8 || depth_format == D3DFMT_D24X8 ||
           depth_format == D3DFMT_D24X4S4);
    depth_stencil_tex_ = ComOwn(static_cast<GpuTexture *>(BaseTexture::Create(
        this, TextureKind::Texture2d, pPresentationParameters->BackBufferWidth,
        pPresentationParameters->BackBufferHeight, 1, 1, D3DUSAGE_DEPTHSTENCIL,
        depth_format, D3DPOOL_DEFAULT)));
    depth_stencil_tex_->SetName("depth_stencil_tex");
    bound_depth_target_ = InternalPtr(depth_stencil_tex_.Get());
    // See the matching comment in Init(): D3DRS_ZENABLE defaults to
    // D3DZB_TRUE when EnableAutoDepthStencil is set.
    render_state_.zbuffer_type = D3DZB_TRUE;
  }

  LOG(INFO) << "Reset: re-acquiring " << swap_chain_desc.BufferCount
            << " back buffer(s)\n";
  ASSERT(back_buffers_.empty());
  for (uint32_t i = 0; i < swap_chain_desc.BufferCount; ++i) {
    ComPtr<ID3D12Resource> back_buffer_resource;
    ASSERT_HR(swap_chain_->GetBuffer(
        i, IID_PPV_ARGS(back_buffer_resource.GetForInit())));
    GpuTexture *back_buffer =
        GpuTexture::InitFromResource(this, back_buffer_resource);
    back_buffer->SetName(std::string("back_buffer_") + std::to_string(i));
    back_buffers_.push_back(ComOwn(back_buffer));
  }
  LOG(INFO) << "Reset: back buffers re-acquired\n";

  current_back_buffer_ = swap_chain_->GetCurrentBackBufferIndex();

  LOG(INFO) << "Reset: final allocator/cmd list reset\n";
  ASSERT_HR(cmd_allocators_[current_back_buffer_]->Reset());
  ASSERT_HR(
      cmd_list_->Reset(cmd_allocators_[current_back_buffer_].get(), nullptr));
  dirty_flags_ ^= DIRTY_FLAG_CMD_LIST_CLOSED;
  last_prim_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  // A fresh command list has no root signature, root arguments, or pipeline
  // state bound.
  root_sig_bound_ = false;
  last_set_pso_ = nullptr;
  last_vbuffer_view_count_ = 0;
  // Everything the renderer thinks is bound was dropped along with the old
  // command list -- including the descriptor heaps, which only BeginScene
  // ever binds. Without this, the first draw after a device reset could skip
  // BeginScene (DIRTY_FLAG_OM having been left clear) and then call
  // SetGraphicsRootDescriptorTable against heaps that were never bound,
  // which the D3D12 debug layer flags as "the descriptor heap containing
  // handle ... is different from currently set descriptor heap". The
  // matching command list reset in SubmitAndWait already did this; this one
  // was missing it.
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
  ++swap_chain_generation_;

  LOG(INFO) << "Reset: done\n";
  return S_OK;
}

D3DCAPS8 Device::GetDefaultCaps(UINT adapter_index) {
  D3DCAPS8 caps{
      .DeviceType = D3DDEVTYPE_HAL,
      .AdapterOrdinal = adapter_index,
      .Caps = 0,  // D3DCAPS_READ_SCANLINE or D3DCAPS_OVERLAY.
      .Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_CANMANAGERESOURCE |
               D3DCAPS2_DYNAMICTEXTURES,
      .Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD,
      .PresentationIntervals =
          D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE |
          D3DPRESENT_INTERVAL_TWO | D3DPRESENT_INTERVAL_THREE |
          D3DPRESENT_INTERVAL_FOUR,

      .CursorCaps = D3DCURSORCAPS_COLOR,

      .DevCaps =
          D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
          D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY |
          D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
          D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
          D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX |
          D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_CANBLTSYSTONONLOCAL |
          D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE,

      .PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE |
                           D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                           D3DPMISCCAPS_COLORWRITEENABLE |
                           D3DPMISCCAPS_CLIPPLANESCALEDPOINTS |
                           D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_BLENDOP,

      .RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX |
                    D3DPRASTERCAPS_ANTIALIASEDGES |
                    D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS |
                    D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY |
                    D3DPRASTERCAPS_COLORPERSPECTIVE,

      .ZCmpCaps = 0xFF,
      .SrcBlendCaps = 0x1FFF,
      .DestBlendCaps = 0x1FFF,
      .AlphaCmpCaps = 0xFF,
      .ShadeCaps = 0xFFFFFFFF,
      // Deliberately not advertising D3DPTEXTURECAPS_VOLUMEMAP/
      // MIPVOLUMEMAP: CreateVolumeTexture is an unimplemented stub, and a
      // compliant game that checks capabilities before using a feature
      // (rather than just trying it) would otherwise get a clean "yes,
      // supported" answer here and then abort on the actual create call.
      .TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA |
                     D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPMAP |
                     D3DPTEXTURECAPS_MIPCUBEMAP,
      .TextureFilterCaps =
          D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
          D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT |
          D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
          D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC,
      // .CubeTextureFilterCaps =.VolumeTextureFilterCaps =.TextureFilterCaps,
      .TextureAddressCaps = 0xFF,
      .VolumeTextureAddressCaps = 0xFF,

      .LineCaps = 0,

      .MaxTextureWidth = 8182,
      .MaxTextureHeight = 8192,
      .MaxVolumeExtent = 2048,

      .MaxTextureRepeat = 128,
      .MaxTextureAspectRatio = 8192,
      .MaxAnisotropy = 16,
      .MaxVertexW = 1410065408,

      .GuardBandLeft = -FLT_MAX,
      .GuardBandTop = -FLT_MAX,
      .GuardBandRight = FLT_MAX,
      .GuardBandBottom = FLT_MAX,
      .ExtentsAdjust = 0,
      .StencilCaps = 0x1FF,

      .FVFCaps = D3DFVFCAPS_DONOTSTRIPELEMENTS |
                 D3DFVFCAPS_TEXCOORDCOUNTMASK,  // Do we need PSIZE?
      .TextureOpCaps = 0xFFFFFFFF,
      .MaxTextureBlendStages = 8,
      .MaxSimultaneousTextures = 8,

      .VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 |
                              D3DVTXPCAPS_DIRECTIONALLIGHTS |
                              D3DVTXPCAPS_POSITIONALLIGHTS,
      .MaxActiveLights = kMaxActiveLights,
      .MaxUserClipPlanes = 8,
      // 0, not 4: CreateFixedFunctionVertexShader (vertex_shader.cpp)
      // explicitly rejects D3DFVF_XYZB1..5 (FVF-based vertex blending) --
      // advertising real blend-matrix support here would invite a
      // compliant game doing fixed-function skinning to hit that assert
      // instead of falling back to shader-based skinning.
      .MaxVertexBlendMatrices = 0,
      .MaxVertexBlendMatrixIndex = 0,  // ??

      .MaxPointSize = 1.f,

      .MaxPrimitiveCount = 0xFFFFFF,
      .MaxVertexIndex = 0xFFFFFF,  // Completely arbitrary.
      .MaxStreams = 16,
      .MaxStreamStride = 0xFF,

      .VertexShaderVersion = D3DVS_VERSION(1, 1),
      .MaxVertexShaderConst = kNumVsConstRegs,
      .PixelShaderVersion = D3DPS_VERSION(1, 3),
      .MaxPixelShaderValue = 65504.f};

  caps.CubeTextureFilterCaps = caps.VolumeTextureFilterCaps =
      caps.TextureFilterCaps;
  return caps;
}

void Device::InitRootSignatures() {
  LOG(INFO) << "InitRootSignatures: start\n";
  std::vector<D3D12_ROOT_PARAMETER> root_params{
      {
          // Cbuffer 0: Transforms cbuffer.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 0},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
      },
      {
          // Cbuffer 1: Material cbuffer.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 1},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
      },
      {
          // CBuffer 2: Lights cbuffer.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 2},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
      },
      {
          // CBuffer 3: Programmable vs constants.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 10},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
      },
  };
  textures_start_bindslot_ = root_params.size();
  // Add all kMaxTexStages textures.
  std::array<D3D12_DESCRIPTOR_RANGE, kMaxTexStages> srv_ranges;
  std::array<D3D12_DESCRIPTOR_RANGE, kMaxTexStages> sampler_ranges;
  for (unsigned int i = 0; i < kMaxTexStages; ++i) {
    srv_ranges[i] = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                     .NumDescriptors = 1,
                     .BaseShaderRegister = i,
                     .OffsetInDescriptorsFromTableStart = 0};
    sampler_ranges[i] = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
                         .NumDescriptors = 1,
                         .BaseShaderRegister = i};
    root_params.push_back(D3D12_ROOT_PARAMETER{
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
        .DescriptorTable = {.NumDescriptorRanges = 1,
                            .pDescriptorRanges = &srv_ranges[i]},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    });
  }
  // And all samplers.
  for (unsigned int i = 0; i < kMaxTexStages; ++i) {
    root_params.push_back(D3D12_ROOT_PARAMETER{
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
        .DescriptorTable = {.NumDescriptorRanges = 1,
                            .pDescriptorRanges = &sampler_ranges[i]},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    });
  }

  D3D12_ROOT_SIGNATURE_DESC sig_desc{
      .NumParameters = static_cast<UINT>(root_params.size()),
      .pParameters = root_params.data(),
      .NumStaticSamplers = 0,
      .pStaticSamplers = nullptr,
      .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};

  LOG(INFO) << "InitRootSignatures: D3D12SerializeRootSignature\n";
  ComPtr<ID3DBlob> sig_blob, error_blob;
  HRESULT hr = D3D12SerializeRootSignature(
      &sig_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, sig_blob.GetForInit(),
      error_blob.GetForInit());
  if (hr != S_OK) {
    FAIL("Could not create root signature:\r\n%s",
         (const char *)error_blob->GetBufferPointer());
  }

  LOG(INFO) << "InitRootSignatures: CreateRootSignature\n";
  ASSERT_HR(d3d12_device_->CreateRootSignature(
      0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
      IID_PPV_ARGS(main_root_sig_.GetForInit())));

  // Create the cbuffers.
  LOG(INFO) << "InitRootSignatures: creating cbuffers\n";
  vs_cbuffer_ = ComOwn(new DynamicBuffer());
  vs_cbuffer_->InitAsBuffer(this, sizeof(VertexCBuffer), Dx8::Usage::Dynamic,
                            D3DPOOL_SYSTEMMEM);
  lights_cbuffer_ = ComOwn(new DynamicBuffer());
  lights_cbuffer_->InitAsBuffer(this, sizeof(LightsCBuffer),
                                Dx8::Usage::Dynamic, D3DPOOL_SYSTEMMEM);
  ps_cbuffer_ = ComOwn(new DynamicBuffer());
  ps_cbuffer_->InitAsBuffer(this, sizeof(PixelCBuffer), Dx8::Usage::Dynamic,
                            D3DPOOL_SYSTEMMEM);

  vs_creg_cbuffer_ = ComOwn(new DynamicBuffer());
  vs_creg_cbuffer_->InitAsBuffer(this, sizeof(float[4]) * kNumVsConstRegs,
                                 Dx8::Usage::Dynamic, D3DPOOL_SYSTEMMEM);
  bound_vs_cregs_.resize(kNumVsConstRegs);

  ps_creg_cbuffer_ = ComOwn(new DynamicBuffer());
  ps_creg_cbuffer_->InitAsBuffer(this, sizeof(float[4]) * kNumPsConstRegs,
                                 Dx8::Usage::Dynamic, D3DPOOL_SYSTEMMEM);
  LOG(INFO) << "InitRootSignatures: done\n";
}

HRESULT STDMETHODCALLTYPE Device::GetDeviceCaps(D3DCAPS8 *pCaps) {
  *pCaps = caps_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::TestCooperativeLevel() { return S_OK; }

UINT STDMETHODCALLTYPE Device::GetAvailableTextureMem() {
  // Real drivers report actual free VRAM; we don't track GPU memory usage,
  // so report a generously large fixed budget. Games generally treat this as
  // a rough quality/streaming heuristic, not an exact figure.
  return 256 * 1024 * 1024;
}

HRESULT STDMETHODCALLTYPE Device::GetCreationParameters(
    D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  // CreateDevice (direct3d8.cpp) asserts DeviceType == D3DDEVTYPE_HAL and
  // BehaviorFlags == D3DCREATE_HARDWARE_VERTEXPROCESSING on every call, so
  // those are safe to report as constants here.
  *pParameters = D3DDEVICE_CREATION_PARAMETERS{
      .AdapterOrdinal = static_cast<UINT>(adapter_index_),
      .DeviceType = D3DDEVTYPE_HAL,
      .hFocusWindow = window_,
      .BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING};
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetCursorProperties(
    UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) {
  // Games render their own software cursor almost universally; the
  // hardware-cursor bitmap itself isn't wired up, but accepting the call
  // instead of aborting is enough for the common case.
  (void)XHotSpot;
  (void)YHotSpot;
  (void)pCursorBitmap;
  return S_OK;
}

void STDMETHODCALLTYPE Device::SetCursorPosition(int X, int Y, DWORD Flags) {
  (void)Flags;
  SetCursorPos(X, Y);
}

BOOL STDMETHODCALLTYPE Device::ShowCursor(BOOL bShow) {
  // IDirect3DDevice8::ShowCursor returns the *previous* visibility state,
  // unlike Win32's ShowCursor (a display counter, not idempotent) -- track
  // our own boolean and only touch the Win32 counter on an actual change so
  // repeated same-value calls don't drift it.
  BOOL previous = cursor_visible_;
  if (static_cast<bool>(bShow) != cursor_visible_) {
    ::ShowCursor(bShow);
    cursor_visible_ = bShow;
  }
  return previous;
}

HRESULT STDMETHODCALLTYPE
Device::GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type,
                      IDirect3DSurface8 **ppBackBuffer) {
  TRACE_ENTRY(Type, ppBackBuffer);
  ASSERT(Type == D3DBACKBUFFER_TYPE_MONO);
  ASSERT(BackBuffer == 0);
  ASSERT(ppBackBuffer);
  *ppBackBuffer =
      new BackbufferSurface(this, BackBuffer, back_buffers_[0].get());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface) {
  TRACE_ENTRY(ppZStencilSurface);
  void *key = depth_stencil_tex_.Get();
  if (!cached_depth_stencil_surface_ ||
      cached_depth_stencil_surface_key_ != key) {
    cached_depth_stencil_surface_ =
        ComOwn<BaseSurface>(new GpuSurface(this, depth_stencil_tex_.Get(), 0));
    cached_depth_stencil_surface_key_ = key;
  }
  cached_depth_stencil_surface_->AddRef();
  *ppZStencilSurface = cached_depth_stencil_surface_.get();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateTexture(UINT Width, UINT Height,
                                                UINT Levels, DWORD Usage,
                                                D3DFORMAT Format, D3DPOOL Pool,
                                                IDirect3DTexture8 **ppTexture) {
  TRACE_ENTRY(Width, Height, Levels, Usage, Format, Pool, ppTexture);
  *ppTexture = BaseTexture::Create(this, TextureKind::Texture2d, Width, Height,
                                   1, Levels, Usage, Format, Pool);
  // BaseTexture::Create's only silent-failure path is an invalid usage/pool
  // combo (D3DUSAGE_DYNAMIC without D3DPOOL_DEFAULT). Was previously
  // `return *ppTexture != nullptr;` -- inverted: that's 0 (S_OK) exactly
  // when creation *failed* (null) and a nonzero/failure-looking value when
  // it *succeeded*, so a caller checking SUCCEEDED()/FAILED() on the
  // returned HRESULT could never actually detect either outcome correctly
  // (0 and 1 both satisfy SUCCEEDED()) -- a game could easily end up
  // treating a failed creation as successful and later binding a null
  // texture wherever it expected one.
  return *ppTexture != nullptr ? S_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture8 **ppCubeTexture) {
  ASSERT(!(Usage & D3DUSAGE_DYNAMIC));
  *ppCubeTexture =
      BaseTexture::Create(this, TextureKind::Cube, EdgeLength, EdgeLength, 6,
                          Levels, Usage, Format, Pool);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable,
    IDirect3DSurface8 **ppSurface) {
  TRACE_ENTRY(Width, Height, Format, MultiSample, Lockable, ppSurface);
  if (MultiSample != D3DMULTISAMPLE_NONE) {
    // Matches CheckDeviceMultiSampleType: the pipeline never builds a
    // multisampled PSO, so fall back to a regular single-sample target
    // instead of failing outright.
    LOG_ERROR() << "Multisampled render targets are not supported; creating "
                  "a single-sample target instead.\n";
  }
  // Render targets are always D3DPOOL_DEFAULT; Lockable only affects whether
  // the resulting surface supports Lock/Unlock. GpuSurface::LockRect already
  // handles D3DPOOL_DEFAULT via a GPU readback, so both cases work the same.
  BaseTexture *texture = BaseTexture::Create(
      this, TextureKind::Texture2d, Width, Height, 1, 1, D3DUSAGE_RENDERTARGET,
      Format, D3DPOOL_DEFAULT);
  if (!texture) return D3DERR_INVALIDCALL;
  // The app never sees the texture object directly, only the surface -- drop
  // our initial ref once the surface (created below) has taken its own, so
  // the texture's lifetime is tied purely to the surface's.
  ComPtr<BaseTexture> owned_texture = ComOwn(texture);
  return owned_texture->GetSurfaceLevel(0, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface) {
  TRACE_ENTRY(Width, Height, Format, MultiSample, ppSurface);
  if (MultiSample != D3DMULTISAMPLE_NONE) {
    LOG_ERROR() << "Multisampled depth-stencil surfaces are not supported; "
                  "creating a single-sample surface instead.\n";
  }
  BaseTexture *texture = BaseTexture::Create(
      this, TextureKind::Texture2d, Width, Height, 1, 1, D3DUSAGE_DEPTHSTENCIL,
      Format, D3DPOOL_DEFAULT);
  if (!texture) return D3DERR_INVALIDCALL;
  ComPtr<BaseTexture> owned_texture = ComOwn(texture);
  return owned_texture->GetSurfaceLevel(0, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateImageSurface(
    UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface) {
  TRACE_ENTRY(Width, Height, Format, ppSurface);
  // Image surfaces are always plain system memory (D3DPOOL_SYSTEMMEM); they
  // exist to be filled by the app and pushed to a real resource via
  // CopyRects/UpdateTexture, not to be usable as a render target or texture.
  BaseTexture *texture =
      BaseTexture::Create(this, TextureKind::Texture2d, Width, Height, 1, 1, 0,
                          Format, D3DPOOL_SYSTEMMEM);
  if (!texture) return D3DERR_INVALIDCALL;
  ComPtr<BaseTexture> owned_texture = ComOwn(texture);
  return owned_texture->GetSurfaceLevel(0, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DSwapChain8 **pSwapChain) {
  TRACE_ENTRY(pPresentationParameters, pSwapChain);
  // D3D8 lets an additional swap chain target a different window than the
  // device's primary one (D3DPRESENT_PARAMETERS::hDeviceWindow); fall back to
  // the primary window if the caller didn't specify one, matching how the
  // primary swap chain itself is created.
  HWND target_window = pPresentationParameters->hDeviceWindow
                           ? pPresentationParameters->hDeviceWindow
                           : window_;

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{
      .Width = pPresentationParameters->BackBufferWidth,
      .Height = pPresentationParameters->BackBufferHeight,
      .Format = ToFlipModelSwapChainFormat(
          DXGIFromD3DFormat(pPresentationParameters->BackBufferFormat)),
      .SampleDesc = {.Count = 1, .Quality = 0},
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = kNumBackBuffers,
      // Stretch, not DXGI_SCALING_NONE: the back buffer is sized to whatever
      // resolution the game asked for, which routinely differs from the size
      // of the window it's presenting into. NONE means "don't scale" -- DXGI
      // puts the buffer in the window's top-left corner at 1:1 and leaves the
      // rest blank, which is what made a game running at, say, 1920x1080 on a
      // 2560x1440 window render into just part of the screen. Real D3D8
      // changed the display mode for fullscreen instead, so the picture
      // always filled the screen; STRETCH is the flip-model equivalent.
      .Scaling = DXGI_SCALING_STRETCH,
      .SwapEffect = ToDxgiSwapEffect(pPresentationParameters->SwapEffect),
      .Flags = tearing_supported_
                   ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
                   : 0u,
  };
  // Logged unconditionally (not TRACE_ENTRY, which release builds compile
  // out): an extra swap chain is a rare, notable event -- each one shows up
  // as its own framerate reading in overlay tools and holds its own set of
  // back buffers -- so it's worth being able to tell from a release log
  // whether one actually got created.
  LOG(INFO) << "CreateAdditionalSwapChain: creating an additional swap chain "
               "for window "
            << target_window << ".\n";
  ComPtr<IDXGISwapChain1> swap_chain1;
  HR_OR_RETURN(dxgi_factory_->CreateSwapChainForHwnd(
      cmd_queue_.get(), target_window, &swap_chain_desc, nullptr, nullptr,
      swap_chain1.GetForInit()));
  ComPtr<IDXGISwapChain3> swap_chain3;
  ASSERT_HR(swap_chain1->QueryInterface(swap_chain3.GetForInit()));
  // See the matching comment in Init() -- opt out of DXGI's automatic
  // window monitoring for this window too.
  ASSERT_HR(dxgi_factory_->MakeWindowAssociation(
      target_window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER |
                         DXGI_MWA_NO_PRINT_SCREEN));

  std::vector<ComPtr<GpuTexture>> back_buffers;
  for (uint32_t i = 0; i < swap_chain_desc.BufferCount; ++i) {
    ComPtr<ID3D12Resource> resource;
    ASSERT_HR(
        swap_chain3->GetBuffer(i, IID_PPV_ARGS(resource.GetForInit())));
    back_buffers.push_back(ComOwn(GpuTexture::InitFromResource(this, resource)));
  }

  *pSwapChain = new AdditionalSwapChain(this, std::move(swap_chain3),
                                        std::move(back_buffers));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                           IDirect3DVertexBuffer8 **ppVertexBuffer) {
  ASSERT(!(Usage & D3DUSAGE_SOFTWAREPROCESSING));
  // Buffer *buffer = new Buffer();
  Buffer *buffer =
      HasFlag(Usage, D3DUSAGE_DYNAMIC) ? new DynamicBuffer() : new Buffer();
  buffer->InitAsVertexBuffer(this, static_cast<size_t>(Length), Usage, Pool,
                             FVF);
  *ppVertexBuffer = buffer;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format,
                          D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer) {
  ASSERT(!(Usage & D3DUSAGE_SOFTWAREPROCESSING));
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32) {
    LOG_ERROR() << "Invalid Format for CreateIndexBuffer: " << Format << "\n";
    return D3DERR_INVALIDCALL;
  }
  Buffer *buffer =
      HasFlag(Usage, D3DUSAGE_DYNAMIC) ? new DynamicBuffer() : new Buffer();
  buffer->InitAsIndexBuffer(this, static_cast<size_t>(Length), Usage, Format,
                            Pool);
  *ppIndexBuffer = buffer;
  return S_OK;
}

void Device::TransitionTexture(GpuTexture *texture, uint32_t subresource,
                               D3D12_RESOURCE_STATES state_after) {
  if (texture->current_state() == state_after) return;
#ifdef DX8TO12_ENABLE_VALIDATION
  // AixLog's severity filtering happens per-sink at dispatch time, not at
  // this call site -- an unguarded LOG() here would pay full temporary-
  // object-construction and stream-formatting cost (std::hex, three
  // operator<< calls) on every single state-changing transition even when
  // the TRACE severity is filtered out and nothing ends up written, exactly
  // the cost TRACE_ENTRY already had to be gated against elsewhere in this
  // file. TransitionTexture is hot enough (called for close to every
  // resource state change, so multiple times per draw in typical scenes)
  // that this was worth gating explicitly rather than assuming the sink
  // threshold alone would make it free.
  LOG(TRACE) << "Transitioning " << std::hex << texture << "From "
             << texture->current_state() << " to " << state_after << "\n";
#endif

  D3D12_RESOURCE_BARRIER barrier{
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {.pResource = texture->resource(),
                     .Subresource = subresource,
                     .StateBefore = texture->current_state(),
                     .StateAfter = state_after}};
  cmd_list_->ResourceBarrier(1, &barrier);
  texture->set_state(state_after);
  MarkResourceAsUsed(InternalPtr(texture));
}

void Device::TransitionBuffer(Buffer *buffer,
                              D3D12_RESOURCE_STATES state_after) {
  if (buffer->current_state() == state_after) return;
  D3D12_RESOURCE_BARRIER barrier = CreateBufferTransition(
      buffer->resource(), buffer->current_state(), state_after);
  cmd_list_->ResourceBarrier(1, &barrier);
  buffer->set_state(state_after);
}

void Device::CopyBuffer(Buffer *dest, int64_t dest_offset,
                        ID3D12Resource *src, int64_t src_offset,
                        int64_t num_bytes) {
  TransitionBuffer(dest, D3D12_RESOURCE_STATE_COPY_DEST);
  cmd_list_->CopyBufferRegion(dest->resource(),
                              static_cast<UINT64>(dest_offset), src,
                              static_cast<UINT64>(src_offset),
                              static_cast<UINT64>(num_bytes));
  TransitionBuffer(dest, D3D12_RESOURCE_STATE_COMMON);
}

void Device::CopyBufferToTexture(
    GpuTexture *dest, uint32_t dest_subresource, ID3D12Resource *src,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT src_footprint, uint32_t dest_x,
    uint32_t dest_y) {
  D3D12_TEXTURE_COPY_LOCATION dest_location{
      .pResource = dest->resource(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = dest_subresource};
  D3D12_TEXTURE_COPY_LOCATION src_location{
      .pResource = src,
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = src_footprint};

  TransitionTexture(dest, dest_subresource, D3D12_RESOURCE_STATE_COPY_DEST);

  cmd_list_->CopyTextureRegion(&dest_location, dest_x, dest_y, 0,
                               &src_location, nullptr);
  // TODO: Transition away from copy destination back to whatever state the
  // texture was in, instead of transitioning back to common.
  TransitionTexture(dest, dest_subresource,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
  MarkResourceAsUsed(InternalPtr(dest));
  // TODO: Mark src as used as well.
}

void Device::MarkBufferForPersist(Buffer *buffer) {
  if (buffer->is_marked_for_persist()) return;
  buffer->set_marked_for_persist(true);
  buffers_to_persist_.push_back(ComWrap(buffer));
}

HRESULT STDMETHODCALLTYPE Device::CopyRects(
    IDirect3DSurface8 *pSourceSurface, CONST RECT *pSourceRectsArray,
    UINT cRects, IDirect3DSurface8 *pDestinationSurface,
    CONST POINT *pDestPointsArray) {
  TRACE_ENTRY(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface,
              pDestPointsArray);

  ASSERT(static_cast<BaseSurface *>(pDestinationSurface)->kind() ==
         SurfaceKind::Gpu);
  GpuSurface *dest_surface = static_cast<GpuSurface *>(pDestinationSurface);

  SurfaceKind source_kind = static_cast<BaseSurface *>(pSourceSurface)->kind();
  if (source_kind == SurfaceKind::Gpu ||
      source_kind == SurfaceKind::Backbuffer) {
    // GPU-to-GPU: no CPU staging needed, just a direct region copy per rect.
    GpuTexture *src_texture;
    uint32_t src_subresource;
    if (source_kind == SurfaceKind::Gpu) {
      GpuSurface *src_gpu_surface = static_cast<GpuSurface *>(pSourceSurface);
      src_texture = src_gpu_surface->texture();
      src_subresource = src_gpu_surface->subresource();
    } else {
      src_texture =
          static_cast<BackbufferSurface *>(pSourceSurface)->texture();
      src_subresource = 0;
    }
    const D3D12_RESOURCE_DESC &src_desc = src_texture->resource_desc();
    RECT whole_surface_rect{.left = 0,
                            .top = 0,
                            .right = static_cast<LONG>(src_desc.Width),
                            .bottom = static_cast<LONG>(src_desc.Height)};
    const bool copy_whole_surface = pSourceRectsArray == nullptr;
    const UINT num_rects = copy_whole_surface ? 1 : cRects;

    const D3D12_RESOURCE_DESC &dst_desc =
        dest_surface->texture()->resource_desc();
    // CopyRects' rect/point math throughout this function operates in raw
    // texel coordinates; block-compressed (DXT/S3TC) resources need those
    // converted to (and D3D12-required to be aligned to) 4x4-block
    // coordinates instead, which isn't implemented -- CopyRects onto/from
    // static compressed texture data essentially never happens in practice
    // (it's almost always used for render-target/backbuffer blits and
    // dynamic surface updates), so this is a loud, specific failure rather
    // than guessed-at block math that could silently corrupt the copy.
    if (IsBlockCompressedFormat(src_desc.Format) ||
        IsBlockCompressedFormat(dst_desc.Format)) {
      FAIL(
          "CopyRects: block-compressed (DXT/S3TC) source/destination "
          "surfaces are not supported.");
    }
    // D3D12 requires an exact (or explicitly-equivalent, per the small set
    // the validation layer recognizes -- BC[1|4], BC[2|3|5|6|7],
    // R9G9B9E5_SHAREDEXP) format match for a direct texture-to-texture
    // CopyTextureRegion. It rejects e.g. B8G8R8A8 <-> B8G8R8X8 even though
    // they're byte-for-byte identical layouts (alpha vs. an unused padding
    // channel) -- exactly the backbuffer-vs-render-target mismatch this hit
    // in practice. Route through an intermediate buffer in that case: a
    // texture-to-buffer copy doesn't care about the source's pixel format
    // (the destination is just raw bytes), and the following buffer-to-
    // texture copy only needs to agree with the *destination* texture's own
    // format, which it does by construction.
    const bool needs_staging = src_desc.Format != dst_desc.Format;

    const D3D12_RESOURCE_STATES src_prior_state = src_texture->current_state();
    TransitionTexture(src_texture, src_subresource,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionTexture(dest_surface->texture(), dest_surface->subresource(),
                      D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst_location{
        .pResource = dest_surface->texture()->resource(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = dest_surface->subresource()};
    D3D12_TEXTURE_COPY_LOCATION src_location{
        .pResource = src_texture->resource(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = src_subresource};
    const int src_format_size = DXGIFormatSize(src_desc.Format);
    for (UINT i = 0; i < num_rects; ++i) {
      const RECT &rect =
          copy_whole_surface ? whole_surface_rect : pSourceRectsArray[i];
      const POINT dest_point = pDestPointsArray
                                    ? pDestPointsArray[i]
                                    : POINT{.x = rect.left, .y = rect.top};
      if (!needs_staging) {
        D3D12_BOX src_box{.left = static_cast<UINT>(rect.left),
                          .top = static_cast<UINT>(rect.top),
                          .front = 0,
                          .right = static_cast<UINT>(rect.right),
                          .bottom = static_cast<UINT>(rect.bottom),
                          .back = 1};
        cmd_list_->CopyTextureRegion(&dst_location,
                                     safe_cast<uint32_t>(dest_point.x),
                                     safe_cast<uint32_t>(dest_point.y), 0,
                                     &src_location, &src_box);
        continue;
      }
      const uint32_t rect_width =
          static_cast<uint32_t>(rect.right - rect.left);
      const uint32_t rect_height =
          static_cast<uint32_t>(rect.bottom - rect.top);
      const uint32_t row_bytes =
          rect_width * static_cast<uint32_t>(src_format_size);
      const uint32_t staged_row_pitch = safe_cast<uint32_t>(
          AlignUp(static_cast<int>(row_bytes),
                  D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      DynamicRingBuffer::Allocation staging_alloc =
          dynamic_ring_buffer()->Allocate(
              static_cast<size_t>(staged_row_pitch) * rect_height,
              D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      D3D12_TEXTURE_COPY_LOCATION staging_location{
          .pResource = dynamic_ring_buffer_->GetBackingResource(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
          .PlacedFootprint = {
              .Offset = safe_cast<uint64_t>(staging_alloc.offset),
              .Footprint = {.Format = src_desc.Format,
                            .Width = rect_width,
                            .Height = rect_height,
                            .Depth = 1,
                            .RowPitch = staged_row_pitch}}};
      D3D12_BOX src_box{.left = static_cast<UINT>(rect.left),
                        .top = static_cast<UINT>(rect.top),
                        .front = 0,
                        .right = static_cast<UINT>(rect.right),
                        .bottom = static_cast<UINT>(rect.bottom),
                        .back = 1};
      cmd_list_->CopyTextureRegion(&staging_location, 0, 0, 0, &src_location,
                                   &src_box);
      D3D12_TEXTURE_COPY_LOCATION staging_src_location = staging_location;
      staging_src_location.PlacedFootprint.Footprint.Format = dst_desc.Format;
      cmd_list_->CopyTextureRegion(&dst_location,
                                   safe_cast<uint32_t>(dest_point.x),
                                   safe_cast<uint32_t>(dest_point.y), 0,
                                   &staging_src_location, nullptr);
    }
    TransitionTexture(src_texture, src_subresource, src_prior_state);
    TransitionTexture(dest_surface->texture(), dest_surface->subresource(),
                      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    MarkResourceAsUsed(InternalPtr(src_texture));
    MarkResourceAsUsed(InternalPtr(dest_surface));
    return S_OK;
  }

  ASSERT(source_kind == SurfaceKind::Cpu);
  CpuSurface *source_surface = static_cast<CpuSurface *>(pSourceSurface);

  const D3D12_SUBRESOURCE_FOOTPRINT &source_footprint =
      source_surface->footprint().Footprint;
  const uint32_t compact_pitch =
      safe_cast<uint32_t>(source_surface->compact_pitch());
  // See the matching guard/comment on the GPU-source path above.
  if (IsBlockCompressedFormat(source_footprint.Format)) {
    FAIL(
        "CopyRects: block-compressed (DXT/S3TC) source surface is not "
        "supported.");
  }
  const int format_size = DXGIFormatSize(source_footprint.Format);

  // No source rects means "copy the whole surface", per the D3D8 docs; a
  // single synthetic rect covering it lets the general per-rect path below
  // handle both cases identically.
  RECT whole_surface_rect{.left = 0,
                          .top = 0,
                          .right = static_cast<LONG>(source_footprint.Width),
                          .bottom = static_cast<LONG>(source_footprint.Height)};
  const bool copy_whole_surface = pSourceRectsArray == nullptr;
  const UINT num_rects = copy_whole_surface ? 1 : cRects;

  for (UINT i = 0; i < num_rects; ++i) {
    const RECT &rect =
        copy_whole_surface ? whole_surface_rect : pSourceRectsArray[i];
    const uint32_t rect_width = static_cast<uint32_t>(rect.right - rect.left);
    const uint32_t rect_height = static_cast<uint32_t>(rect.bottom - rect.top);
    const POINT dest_point = pDestPointsArray
                                  ? pDestPointsArray[i]
                                  : POINT{.x = rect.left, .y = rect.top};

    // Allocate space in our ring buffer and move just this rect's source
    // data, row by row (the source rect's rows aren't contiguous in the
    // backing CPU surface unless the rect is the full width).
    const uint32_t row_bytes = rect_width * static_cast<uint32_t>(format_size);
    const uint32_t dest_row_pitch =
        safe_cast<uint32_t>(AlignUp(static_cast<int>(row_bytes),
                                    D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const size_t num_bytes =
        static_cast<size_t>(dest_row_pitch) * rect_height;
    DynamicRingBuffer::Allocation ring_alloc =
        dynamic_ring_buffer()->Allocate(
            num_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    char *source_ring_ptr = dynamic_ring_buffer()->GetCpuPtrFor(ring_alloc);
    const char *rect_src_ptr = source_surface->GetPtr() +
                               rect.top * compact_pitch +
                               rect.left * format_size;
    for (uint32_t row = 0; row < rect_height; ++row) {
      memcpy(source_ring_ptr + row * dest_row_pitch,
             rect_src_ptr + row * compact_pitch, row_bytes);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT src_placed_footprint{
        .Offset = safe_cast<uint64_t>(ring_alloc.offset),
        .Footprint = {.Format = source_footprint.Format,
                      .Width = rect_width,
                      .Height = rect_height,
                      .Depth = 1,
                      .RowPitch = dest_row_pitch}};

    CopyBufferToTexture(dest_surface->texture(), dest_surface->subresource(),
                        dynamic_ring_buffer_->GetBackingResource(),
                        src_placed_footprint,
                        safe_cast<uint32_t>(dest_point.x),
                        safe_cast<uint32_t>(dest_point.y));
  }

  MarkResourceAsUsed(InternalPtr(dest_surface));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture,
                      IDirect3DBaseTexture8 *pDestinationTexture) {
  TRACE_ENTRY(pSourceTexture, pDestinationTexture);
  BaseTexture *source = dynamic_cast<BaseTexture *>(pSourceTexture);
  ASSERT(source->GetSurfaceDesc(0).Pool == D3DPOOL_SYSTEMMEM);
  BaseTexture *dest = dynamic_cast<BaseTexture *>(pDestinationTexture);
  ASSERT(dest->GetSurfaceDesc(0).Pool != D3DPOOL_SYSTEMMEM);
  // Transition dest.
  TransitionTexture(static_cast<GpuTexture *>(dest),
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST);
  static_cast<CpuTexture *>(source)->CopyToGpuTexture(
      static_cast<GpuTexture *>(dest));
  // Transition dest.
  TransitionTexture(static_cast<GpuTexture *>(dest),
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
  MarkResourceAsUsed(InternalPtr(source));
  MarkResourceAsUsed(InternalPtr(dest));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetViewport(const D3DVIEWPORT8 *pViewport) {
  viewport_.TopLeftX = static_cast<float>(pViewport->X);
  viewport_.TopLeftY = static_cast<float>(pViewport->Y);
  viewport_.Width = static_cast<float>(pViewport->Width);
  viewport_.Height = static_cast<float>(pViewport->Height);
  viewport_.MinDepth = pViewport->MinZ;
  viewport_.MaxDepth = pViewport->MaxZ;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetViewport(D3DVIEWPORT8 *pViewport) {
  pViewport->X = static_cast<DWORD>(viewport_.TopLeftX);
  pViewport->Y = static_cast<DWORD>(viewport_.TopLeftY);
  pViewport->Width = static_cast<DWORD>(viewport_.Width);
  pViewport->Height = static_cast<DWORD>(viewport_.Height);
  pViewport->MinZ = viewport_.MinDepth;
  pViewport->MaxZ = viewport_.MaxDepth;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTransform(D3DTRANSFORMSTATETYPE State,
                                               CONST D3DMATRIX *pMatrix) {
  if (State > 511 || State < D3DTS_VIEW ||
      (State > D3DTS_PROJECTION && State < D3DTS_TEXTURE0)) {
    LOG_ERROR() << "Invalid SetTransform index: " << State << "\n";
    return D3DERR_INVALIDCALL;
  }
  if (State == D3DTS_VIEW) {
    // Lights are uploaded to the GPU in view-space, so we must update them if
    // the view matrix changes.
    dirty_flags_ |= DIRTY_FLAG_LIGHTS;
  }
  transforms_[State] = *pMatrix;
  dirty_flags_ |= DIRTY_FLAG_TRANSFORMS;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetTransform(D3DTRANSFORMSTATETYPE State,
                                               D3DMATRIX *pMatrix) {
  if (State > 511 || State < D3DTS_VIEW ||
      (State > D3DTS_PROJECTION && State < D3DTS_TEXTURE0)) {
    LOG_ERROR() << "Invalid SetTransform index: " << State << "\n";
    return D3DERR_INVALIDCALL;
  }
  if (transforms_.contains(State)) {
    *pMatrix = transforms_[State];
  } else {
    static DirectX::SimpleMath::Matrix identity;
    memcpy(pMatrix, &identity, sizeof(identity));
  }
  return S_OK;
}

D3DMATRIX Device::GetTransform(D3DTRANSFORMSTATETYPE state) {
  D3DMATRIX matrix;
  ASSERT_HR(GetTransform(state, &matrix));
  return matrix;
}

HRESULT STDMETHODCALLTYPE
Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State,
                          CONST D3DMATRIX *pMatrix) {
  D3DMATRIX current = GetTransform(State);
  DirectX::SimpleMath::Matrix lhs, rhs;
  memcpy(&lhs, pMatrix, sizeof(lhs));
  memcpy(&rhs, &current, sizeof(rhs));
  // Row-vector convention (matches D3D8): applying pMatrix first, then the
  // state's existing matrix.
  DirectX::SimpleMath::Matrix result = lhs * rhs;
  D3DMATRIX result_d3d;
  memcpy(&result_d3d, &result, sizeof(result_d3d));
  return SetTransform(State, &result_d3d);
}

HRESULT STDMETHODCALLTYPE Device::SetMaterial(const D3DMATERIAL8 *pMaterial) {
  material_ = *pMaterial;
  dirty_flags_ |= DIRTY_FLAG_PS_CBUFFER;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetMaterial(D3DMATERIAL8 *pMaterial) {
  *pMaterial = material_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetLight(DWORD Index,
                                           CONST D3DLIGHT8 *light) {
  lights_[Index] = *light;
  if (enabled_lights_.contains(Index)) {
    dirty_flags_ |= DIRTY_FLAG_LIGHTS;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetLight(DWORD Index, D3DLIGHT8 *light) {
  if (!lights_.contains(Index)) return D3DERR_INVALIDCALL;
  *light = lights_[Index];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetLightEnable(DWORD Index, BOOL *pEnable) {
  *pEnable = enabled_lights_.contains(Index);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::LightEnable(DWORD Index, BOOL Enable) {
  if (!lights_.contains(Index)) {
    // Create the default light if it does not already exist.
    lights_[Index] = D3DLIGHT8{.Type = D3DLIGHT_DIRECTIONAL,
                               .Diffuse = {1, 1, 1, 0},
                               .Direction = {0.f, 0.f, 1.f}};
  }
  if (Enable) {
    if (enabled_lights_.size() >= caps_.MaxActiveLights) {
      LOG_ERROR() << "Trying to enable more than " << caps_.MaxActiveLights
                  << " lights.\n";
      return D3DERR_INVALIDCALL;
    } else {
      enabled_lights_.insert(Index);
    }
  } else {
    enabled_lights_.erase(Index);
  }
  dirty_flags_ |= DIRTY_FLAG_LIGHTS;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetClipPlane(DWORD Index,
                                               CONST float *pPlane) {
  // Bookkeeping only -- no GPU-side user clip plane implementation, so this
  // does not actually affect rendering. See the clip_planes_ comment.
  if (Index >= clip_planes_.size()) return D3DERR_INVALIDCALL;
  memcpy(clip_planes_[Index].data(), pPlane, sizeof(float) * 4);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetClipPlane(DWORD Index, float *pPlane) {
  if (Index >= clip_planes_.size()) return D3DERR_INVALIDCALL;
  memcpy(pPlane, clip_planes_[Index].data(), sizeof(float) * 4);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::SetClipStatus(CONST D3DCLIPSTATUS8 *pClipStatus) {
  clip_status_ = *pClipStatus;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetClipStatus(D3DCLIPSTATUS8 *pClipStatus) {
  *pClipStatus = clip_status_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::ValidateDevice(DWORD *pNumPasses) {
  // We never need more than a single pass to render the current state.
  *pNumPasses = 1;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetRenderState(D3DRENDERSTATETYPE State,
                                                 DWORD Value) {
  // Redundant-set early-out. RenderWare (and D3D8-era engines generally)
  // re-set the same render state value many times per frame rather than
  // tracking what's already current, so this is a very common case. Skipping
  // it avoids dirtying cbuffers that would then be re-uploaded for no reason.
  // Bitwise comparison is correct here even for the float-typed states (this
  // accessor hands them back reinterpreted as DWORD): identical bits mean an
  // identical value, and the only false negatives (e.g. -0.0f vs +0.0f) fall
  // through to the old behavior rather than skipping a real change.
  DWORD &state_slot = render_state_.GetEnumAtIndex(State);
  if (state_slot == Value) return S_OK;
  state_slot = Value;
  // Every render state is part of the PSO key (PSOState embeds the whole
  // RenderState), so any real change invalidates the cached lookup.
  dirty_flags_ |= DIRTY_FLAG_PSO;
  switch (State) {
    case D3DRS_TEXTUREFACTOR:
    case D3DRS_ALPHAREF:
      dirty_flags_ |= DIRTY_FLAG_PS_CBUFFER;
      break;
    case D3DRS_LIGHTING:
    case D3DRS_COLORVERTEX:
    case D3DRS_DIFFUSEMATERIALSOURCE:
    case D3DRS_AMBIENTMATERIALSOURCE:
    case D3DRS_SPECULARMATERIALSOURCE:
    case D3DRS_AMBIENT:
    case D3DRS_SPECULARENABLE:
    case D3DRS_NORMALIZENORMALS:
      dirty_flags_ |= DIRTY_FLAG_LIGHTS;
      break;
    default:
      break;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetRenderState(D3DRENDERSTATETYPE State,
                                                 DWORD *pValue) {
  *pValue = render_state_.GetEnumAtIndex(State);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  if (Stage >= texture_stage_states_.size()) return D3DERR_INVALIDCALL;
  *pValue = texture_stage_states_[Stage].GetAtIndex(static_cast<size_t>(Type));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  if (Stage >= texture_stage_states_.size()) return D3DERR_INVALIDCALL;
  // Redundant-set early-out -- same reasoning as SetRenderState above. This
  // one matters extra because a sampler-affecting state redundantly re-set
  // would dirty DIRTY_FLAG_PS_SAMPLERS, which costs a full 8-stage sampler
  // rebind on the next draw.
  DWORD &stage_slot =
      texture_stage_states_[Stage].GetAtIndex(static_cast<size_t>(Type));
  if (stage_slot == Value) return S_OK;
  stage_slot = Value;
  // Texture stage state drives fixed-function pixel shader generation, which
  // is part of the PSO key.
  dirty_flags_ |= DIRTY_FLAG_PSO;
  if ((Type >= D3DTSS_ADDRESSU && Type <= D3DTSS_MAXANISOTROPY) ||
      Type == D3DTSS_ADDRESSW) {
    dirty_flags_ |= DIRTY_FLAG_PS_SAMPLERS;
  }
  return S_OK;
}

namespace {
// Mirrors the exhaustive lists of state types handled by
// RenderState::GetEnumAtIndex / TextureStageState::GetAtIndex (excluding
// D3DRS_WRAP0..7, which are handled separately below since they're a
// contiguous range).
constexpr D3DRENDERSTATETYPE kAllRenderStateTypes[] = {
    D3DRS_ZENABLE,
    D3DRS_ZWRITEENABLE,
    D3DRS_SHADEMODE,
    D3DRS_FILLMODE,
    D3DRS_ALPHATESTENABLE,
    D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,
    D3DRS_CULLMODE,
    D3DRS_ZFUNC,
    D3DRS_ALPHAREF,
    D3DRS_ALPHAFUNC,
    D3DRS_DITHERENABLE,
    D3DRS_ALPHABLENDENABLE,
    D3DRS_FOGENABLE,
    D3DRS_SPECULARENABLE,
    D3DRS_FOGCOLOR,
    D3DRS_FOGTABLEMODE,
    D3DRS_FOGSTART,
    D3DRS_FOGEND,
    D3DRS_FOGDENSITY,
    D3DRS_EDGEANTIALIAS,
    D3DRS_ZBIAS,
    D3DRS_RANGEFOGENABLE,
    D3DRS_STENCILENABLE,
    D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL,
    D3DRS_STENCILPASS,
    D3DRS_STENCILFUNC,
    D3DRS_STENCILREF,
    D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK,
    D3DRS_TEXTUREFACTOR,
    D3DRS_LIGHTING,
    D3DRS_AMBIENT,
    D3DRS_FOGVERTEXMODE,
    D3DRS_COLORVERTEX,
    D3DRS_LOCALVIEWER,
    D3DRS_NORMALIZENORMALS,
    D3DRS_DIFFUSEMATERIALSOURCE,
    D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE,
    D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_POINTSIZE,
    D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSCALE_A,
    D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C,
    D3DRS_MULTISAMPLEANTIALIAS,
    D3DRS_POINTSIZE_MAX,
    D3DRS_COLORWRITEENABLE,
    D3DRS_BLENDOP,
    D3DRS_CLIPPING,
    D3DRS_CLIPPLANEENABLE,
    D3DRS_LASTPIXEL,
    D3DRS_LINEPATTERN,
    D3DRS_ZVISIBLE,
    D3DRS_SOFTWAREVERTEXPROCESSING,
    D3DRS_MULTISAMPLEMASK,
    D3DRS_PATCHEDGESTYLE,
    D3DRS_PATCHSEGMENTS,
    D3DRS_DEBUGMONITORTOKEN,
    D3DRS_VERTEXBLEND,
    D3DRS_INDEXEDVERTEXBLENDENABLE,
    D3DRS_TWEENFACTOR,
    D3DRS_POSITIONORDER,
    D3DRS_NORMALORDER,
    D3DRS_WRAP0,
    D3DRS_WRAP1,
    D3DRS_WRAP2,
    D3DRS_WRAP3,
    D3DRS_WRAP4,
    D3DRS_WRAP5,
    D3DRS_WRAP6,
    D3DRS_WRAP7,
};

constexpr D3DTEXTURESTAGESTATETYPE kAllTextureStageStateTypes[] = {
    D3DTSS_COLOROP,          D3DTSS_COLORARG1,      D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP,          D3DTSS_ALPHAARG1,      D3DTSS_ALPHAARG2,
    D3DTSS_TEXCOORDINDEX,    D3DTSS_ADDRESSU,       D3DTSS_ADDRESSV,
    D3DTSS_BORDERCOLOR,      D3DTSS_MAGFILTER,      D3DTSS_MINFILTER,
    D3DTSS_MIPFILTER,        D3DTSS_MIPMAPLODBIAS,  D3DTSS_MAXANISOTROPY,
    D3DTSS_TEXTURETRANSFORMFLAGS,                   D3DTSS_ADDRESSW,
    D3DTSS_COLORARG0,        D3DTSS_ALPHAARG0,      D3DTSS_RESULTARG,
    D3DTSS_BUMPENVMAT00,     D3DTSS_BUMPENVMAT01,   D3DTSS_BUMPENVMAT10,
    D3DTSS_BUMPENVMAT11,     D3DTSS_BUMPENVLSCALE,  D3DTSS_BUMPENVLOFFSET,
};
}  // namespace

Device::StateSnapshot Device::CaptureCurrentState() const {
  return StateSnapshot{
      .render_state = render_state_,
      .texture_stage_states = texture_stage_states_,
      .transforms = transforms_,
      .material = material_,
      .lights = lights_,
      .enabled_lights = enabled_lights_,
      .bound_textures = bound_textures_,
      .bound_vertex_shader = bound_vertex_shader_,
      .bound_pixel_shader = bound_pixel_shader_,
      .bound_vs_cregs = bound_vs_cregs_,
  };
}

Device::StateBlock Device::CaptureFullStateBlock() const {
  StateBlock block;
  RenderState rs = render_state_;
  for (D3DRENDERSTATETYPE type : kAllRenderStateTypes) {
    block.render_state[type] = rs.GetEnumAtIndex(type);
  }
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    TextureStageState tss = texture_stage_states_[stage];
    for (D3DTEXTURESTAGESTATETYPE type : kAllTextureStageStateTypes) {
      block.texture_stage_states[stage][type] =
          tss.GetAtIndex(static_cast<size_t>(type));
    }
  }
  block.transforms = transforms_;
  block.material = material_;
  block.lights = lights_;
  block.enabled_lights = enabled_lights_;
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    block.bound_textures[stage] = bound_textures_[stage];
  }
  block.bound_vertex_shader = bound_vertex_shader_;
  block.bound_pixel_shader = bound_pixel_shader_;
  for (UINT i = 0; i < bound_vs_cregs_.size(); ++i) {
    block.bound_vs_cregs[i] = bound_vs_cregs_[i];
  }
  return block;
}

Device::StateBlock Device::CaptureStateBlockDelta(
    const StateSnapshot &before) const {
  StateBlock block;

  RenderState before_rs = before.render_state;
  RenderState after_rs = render_state_;
  for (D3DRENDERSTATETYPE type : kAllRenderStateTypes) {
    DWORD before_value = before_rs.GetEnumAtIndex(type);
    DWORD after_value = after_rs.GetEnumAtIndex(type);
    if (before_value != after_value) block.render_state[type] = after_value;
  }

  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    TextureStageState before_tss = before.texture_stage_states[stage];
    TextureStageState after_tss = texture_stage_states_[stage];
    for (D3DTEXTURESTAGESTATETYPE type : kAllTextureStageStateTypes) {
      DWORD before_value = before_tss.GetAtIndex(static_cast<size_t>(type));
      DWORD after_value = after_tss.GetAtIndex(static_cast<size_t>(type));
      if (before_value != after_value)
        block.texture_stage_states[stage][type] = after_value;
    }
  }

  for (const auto &[type, matrix] : transforms_) {
    auto before_it = before.transforms.find(type);
    if (before_it == before.transforms.end() ||
        memcmp(&before_it->second, &matrix, sizeof(matrix)) != 0) {
      block.transforms[type] = matrix;
    }
  }

  if (memcmp(&before.material, &material_, sizeof(material_)) != 0) {
    block.material = material_;
  }

  for (const auto &[index, light] : lights_) {
    auto before_it = before.lights.find(index);
    if (before_it == before.lights.end() ||
        memcmp(&before_it->second, &light, sizeof(light)) != 0) {
      block.lights[index] = light;
    }
  }

  if (before.enabled_lights != enabled_lights_) {
    block.enabled_lights = enabled_lights_;
  }

  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    if (!(before.bound_textures[stage] == bound_textures_[stage])) {
      block.bound_textures[stage] = bound_textures_[stage];
    }
  }

  if (before.bound_vertex_shader != bound_vertex_shader_) {
    block.bound_vertex_shader = bound_vertex_shader_;
  }
  if (before.bound_pixel_shader != bound_pixel_shader_) {
    block.bound_pixel_shader = bound_pixel_shader_;
  }

  for (UINT i = 0; i < bound_vs_cregs_.size(); ++i) {
    const auto &after_value = bound_vs_cregs_[i];
    if (i >= before.bound_vs_cregs.size() ||
        before.bound_vs_cregs[i] != after_value) {
      block.bound_vs_cregs[i] = after_value;
    }
  }

  return block;
}

void Device::ApplyState(const StateBlock &block) {
  for (const auto &[type, value] : block.render_state) {
    render_state_.GetEnumAtIndex(type) = value;
  }
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    for (const auto &[type, value] : block.texture_stage_states[stage]) {
      texture_stage_states_[stage].GetAtIndex(static_cast<size_t>(type)) =
          value;
    }
  }
  for (const auto &[type, matrix] : block.transforms) {
    transforms_[type] = matrix;
  }
  if (block.material) material_ = *block.material;
  for (const auto &[index, light] : block.lights) {
    lights_[index] = light;
  }
  if (block.enabled_lights) enabled_lights_ = *block.enabled_lights;
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    if (block.bound_textures[stage]) {
      bound_textures_[stage] = *block.bound_textures[stage];
    }
  }
  if (block.bound_vertex_shader) bound_vertex_shader_ = *block.bound_vertex_shader;
  if (block.bound_pixel_shader) bound_pixel_shader_ = *block.bound_pixel_shader;
  for (const auto &[index, value] : block.bound_vs_cregs) {
    bound_vs_cregs_.at(index) = value;
  }
  // Force everything above to actually get re-bound/re-uploaded before the
  // next draw call, since we just changed it out from under the renderer.
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
}

HRESULT STDMETHODCALLTYPE Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                                   DWORD *pToken) {
  // Simplification: always captures every state regardless of Type
  // (D3DSBT_ALL/D3DSBT_PIXELSTATE/D3DSBT_VERTEXSTATE don't get the precise
  // real-D3D8 partitioning), but -- unlike Begin/EndStateBlock below --
  // capturing everything really is correct semantics for this API: it snapshots
  // the live state at this exact point in time to restore later.
  *pToken = next_state_block_token_++;
  state_blocks_[*pToken] = CaptureFullStateBlock();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::BeginStateBlock() {
  if (recording_state_block_) return D3DERR_INVALIDCALL;
  recording_state_block_ = true;
  state_block_recording_start_ = CaptureCurrentState();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::EndStateBlock(DWORD *pToken) {
  if (!recording_state_block_) return D3DERR_INVALIDCALL;
  recording_state_block_ = false;
  // Only the states actually Set() (i.e. changed) during the Begin/End
  // window are captured, matching real D3D8 semantics -- ApplyStateBlock()
  // must not clobber unrelated state a game changed in between recording and
  // applying the block.
  *pToken = next_state_block_token_++;
  state_blocks_[*pToken] = CaptureStateBlockDelta(state_block_recording_start_);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::ApplyStateBlock(DWORD Token) {
  auto it = state_blocks_.find(Token);
  if (it == state_blocks_.end()) return D3DERR_INVALIDCALL;
  ApplyState(it->second);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CaptureStateBlock(DWORD Token) {
  auto it = state_blocks_.find(Token);
  if (it == state_blocks_.end()) return D3DERR_INVALIDCALL;
  // Per the D3D8 docs, this refreshes the values of the states already
  // recorded in this block from the current live state -- it does not add
  // or remove which states are tracked.
  StateBlock &block = it->second;
  RenderState live_rs = render_state_;
  for (auto &[type, value] : block.render_state) {
    value = live_rs.GetEnumAtIndex(type);
  }
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    TextureStageState live_tss = texture_stage_states_[stage];
    for (auto &[type, value] : block.texture_stage_states[stage]) {
      value = live_tss.GetAtIndex(static_cast<size_t>(type));
    }
  }
  for (auto &[type, matrix] : block.transforms) {
    matrix = transforms_.contains(type) ? transforms_.at(type) : D3DMATRIX{};
  }
  if (block.material) block.material = material_;
  for (auto &[index, light] : block.lights) {
    if (lights_.contains(index)) light = lights_.at(index);
  }
  if (block.enabled_lights) block.enabled_lights = enabled_lights_;
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    if (block.bound_textures[stage]) {
      block.bound_textures[stage] = bound_textures_[stage];
    }
  }
  if (block.bound_vertex_shader) block.bound_vertex_shader = bound_vertex_shader_;
  if (block.bound_pixel_shader) block.bound_pixel_shader = bound_pixel_shader_;
  for (auto &[index, value] : block.bound_vs_cregs) {
    value = bound_vs_cregs_.at(index);
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DeleteStateBlock(DWORD Token) {
  if (state_blocks_.erase(Token) == 0) return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTexture(DWORD Stage,
                                             IDirect3DBaseTexture8 *pTexture) {
  TRACE_ENTRY(Stage, pTexture);
  if (Stage >= bound_textures_.size()) return D3DERR_INVALIDCALL;
  // Profiled hot path (WPA CPU sampling, see the session that added this
  // comment): SetTexture alone accounted for more sampled CPU time than
  // DrawPrimitive and SetStreamSource combined, almost entirely from doing
  // *two* separate dynamic_casts of the same pointer. BaseTexture multiply-
  // inherits IDirect3DTexture8/IDirect3DCubeTexture8 (both of which derive
  // from IDirect3DBaseTexture8), so casting from the IDirect3DBaseTexture8*
  // the app hands us requires a real dynamic_cast (a static_cast across that
  // diamond isn't well-defined) -- but that's only true for *this* first
  // step. BaseTexture -> GpuTexture is a plain single-inheritance downcast
  // (same pattern already used in CopyRects below), so reuse the one
  // dynamic_cast's result via static_cast instead of paying for RTTI twice.
  BaseTexture *base_texture = dynamic_cast<BaseTexture *>(pTexture);
  if (base_texture) {
    ASSERT(base_texture->GetSurfaceDesc(0).Pool != D3DPOOL_SYSTEMMEM);
  }
  GpuTexture *texture = static_cast<GpuTexture *>(base_texture);
  // Redundant-set early-out. Re-binding the texture that's already bound to
  // this stage would otherwise dirty DIRTY_FLAG_PS_TEXTURES and cost a full
  // 8-stage descriptor-table rebind on the next draw for no change at all.
  // Note InternalPtr::Get() asserts non-null, and both sides are legitimately
  // null routinely here (a game unbinding an already-unbound stage), so read
  // the current binding through the bool conversion rather than Get().
  GpuTexture *const current_texture =
      bound_textures_[Stage] ? bound_textures_[Stage].Get() : nullptr;
  if (current_texture == texture) return S_OK;
  bound_textures_[Stage] = InternalPtr(texture);
  dirty_flags_ |= DIRTY_FLAG_PS_TEXTURES;
  // Whether a stage has a texture at all feeds PixelShaderState (and so the
  // generated shader), which is part of the PSO key.
  dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetTexture(DWORD Stage,
                                             IDirect3DBaseTexture8 **ppTexture) {
  if (Stage >= bound_textures_.size()) return D3DERR_INVALIDCALL;
  GpuTexture *texture = bound_textures_[Stage].Get();
  *ppTexture = static_cast<IDirect3DTexture8 *>(texture);
  if (texture) texture->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetRenderTarget(
    IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil) {
  if (pRenderTarget) {
    SCOPED_MARKER("SetRenderTarget");
    if (bound_render_target_) {
      // Transition out of render target into common.
      TransitionTexture(bound_render_target_.Get(), 0,
                        D3D12_RESOURCE_STATE_COMMON);
    }

    BaseSurface *base_surface = static_cast<BaseSurface *>(pRenderTarget);
    GpuTexture *texture = nullptr;
    D3D12_RESOURCE_DESC resource_desc = {};
    switch (base_surface->kind()) {
      case SurfaceKind::Gpu:
        texture = static_cast<GpuSurface *>(base_surface)->texture();
        resource_desc = texture->resource_desc();
        // Real D3D8 games commonly render to an off-screen target with a
        // different format than the backbuffer (radar map, menu blur/
        // reflection effects, etc.) -- CreatePSO reads the *actual* bound
        // render target's format (see its RTVFormats comment) rather than
        // assuming it always matches the backbuffer, so this no longer needs
        // to be true.
        TransitionTexture(texture, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        break;
      case SurfaceKind::Backbuffer:
        ASSERT(static_cast<BackbufferSurface *>(base_surface)->index() == 0);
        texture = nullptr;
        resource_desc = back_buffers_.at(0)->resource_desc();
        break;
      case SurfaceKind::Cpu:
        LOG_ERROR() << "Cannot set SYSTEMMEM surface as render target.\n";
        return D3DERR_INVALIDCALL;
    }
    bound_render_target_ = InternalPtr(texture);

    // Reset viewport to the size of this one.
    D3DVIEWPORT8 viewport{.Width = safe_cast<DWORD>(resource_desc.Width),
                          .Height = resource_desc.Height,
                          .MaxZ = 1.f};
    ASSERT_HR(SetViewport(&viewport));
  }
  if (pNewZStencil) {
    SCOPED_MARKER("SetDepthTarget");
    BaseSurface *base_surface = dynamic_cast<BaseSurface *>(pNewZStencil);
    GpuTexture *texture = nullptr;
    switch (base_surface->kind()) {
      case SurfaceKind::Gpu:
        texture = static_cast<GpuSurface *>(base_surface)->texture();
        // A custom depth-stencil surface (from CreateDepthStencilSurface) is
        // as legitimate a target here as the implicit one -- e.g. paired
        // with an off-screen color render target for a menu/mirror/reflection
        // effect. Games routinely bind these together, and the previous
        // ASSERT below only ever allowed depth_stencil_tex_ itself, hard-
        // failing on any such effect. GpuTexture already starts in
        // DEPTH_WRITE for D3DUSAGE_DEPTHSTENCIL textures, but transition
        // explicitly (a no-op if already correct) in case it was reused for
        // something else in between.
        TransitionTexture(texture, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        break;
      case SurfaceKind::Backbuffer:
        ASSERT(static_cast<BackbufferSurface *>(base_surface)->index() == 0);
        texture = depth_stencil_tex_.Get();
        break;
      case SurfaceKind::Cpu:
        LOG_ERROR() << "Cannot set SYSTEMMEM surface as render target.\n";
        return D3DERR_INVALIDCALL;
    }
    bound_depth_target_ = InternalPtr(texture);
  } else {
    bound_depth_target_.Reset();
  }
  dirty_flags_ |= DIRTY_FLAG_OM;
  // Render target / depth target formats are part of the PSO key.
  dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::GetRenderTarget(IDirect3DSurface8 **ppRenderTarget) {
  void *key = bound_render_target_ ? static_cast<void *>(bound_render_target_.Get())
                                   : static_cast<void *>(back_buffers_[0].get());
  if (!cached_render_target_surface_ || cached_render_target_surface_key_ != key) {
    if (bound_render_target_) {
      cached_render_target_surface_ = ComOwn<BaseSurface>(
          new GpuSurface(this, bound_render_target_.Get(), 0));
    } else {
      cached_render_target_surface_ = ComOwn<BaseSurface>(
          new BackbufferSurface(this, 0, back_buffers_[0].get()));
    }
    cached_render_target_surface_key_ = key;
  }
  cached_render_target_surface_->AddRef();
  *ppRenderTarget = cached_render_target_surface_.get();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexShader(const DWORD *pDeclaration,
                                                     const DWORD *pFunction,
                                                     DWORD *pHandle,
                                                     DWORD Usage) {
  auto decl = ParseShaderDeclaration(pDeclaration);

  VertexShader shader;
  if (pFunction == nullptr) {
    shader = CreateFixedFunctionVertexShader(viewport_, 0, decl);
  } else {
    shader = ParseProgrammableVertexShader(decl, pFunction);
  }

  // Keep a copy of the original declaration token stream for
  // GetVertexShaderDeclaration. Cap the scan for the D3DVSD_END() terminator
  // so a malformed/corrupt declaration produces a clear, logged failure
  // instead of walking off into unmapped memory looking for a token that
  // isn't there.
  static constexpr ptrdiff_t kMaxDeclarationTokens = 512;
  const DWORD *decl_end = pDeclaration;
  while (*decl_end != D3DVSD_END()) {
    if (decl_end - pDeclaration >= kMaxDeclarationTokens) {
      FAIL(
          "CreateVertexShader: declaration token stream did not terminate "
          "with D3DVSD_END() within %td tokens; pDeclaration=%p is likely "
          "invalid.",
          kMaxDeclarationTokens, pDeclaration);
    }
    ++decl_end;
  }
  ++decl_end;  // Include the END token itself.
  shader.declaration_tokens.assign(pDeclaration, decl_end);

  ASSERT(next_shader_handle_ < UINT32_MAX);
  DWORD handle = next_shader_handle_++;
  ASSERT(handle >= kFirstShaderHandle);
  vertex_shaders_[handle] = InternalPtr(new VertexShader(std::move(shader)));
  *pHandle = handle;

  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreatePixelShader(const DWORD *pFunction,
                                                    DWORD *pHandle) {
  if (!pFunction) return D3DERR_INVALIDCALL;
  PixelShader shader = ParsePixelShader(pFunction);
  ASSERT(next_shader_handle_ < UINT32_MAX);
  *pHandle = next_shader_handle_++;
  pixel_shaders_[*pHandle] = InternalPtr(new PixelShader(std::move(shader)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DeleteVertexShader(DWORD Handle) {
  ASSERT(Handle >= kFirstShaderHandle);
  auto found = vertex_shaders_.erase(Handle);
  ASSERT(found != 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DeletePixelShader(DWORD Handle) {
  auto found = pixel_shaders_.erase(Handle);
  ASSERT(found != 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShader(DWORD handle) {
  if (handle < kFirstShaderHandle) {
    // This is a fixed-function shader. Create it if it does not already
    // exist.
    if (!vertex_shaders_.contains(handle)) {
      vertex_shaders_[handle] =
          InternalPtr(new VertexShader(CreateFixedFunctionVertexShader(
              viewport_, handle,
              VertexShaderDeclaration::CreateFromFVFDesc(handle))));
    }
  } else {
    ASSERT(vertex_shaders_.contains(handle));
  }
  bound_vertex_shader_ = handle;
  dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShader(DWORD *pHandle) {
  *pHandle = bound_vertex_shader_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderDeclaration(
    DWORD Handle, void *pData, DWORD *pSizeOfData) {
  if (Handle < kFirstShaderHandle || !vertex_shaders_.contains(Handle))
    return D3DERR_INVALIDCALL;
  return CopyOutTokenBuffer(vertex_shaders_.at(Handle)->declaration_tokens,
                            pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderFunction(
    DWORD Handle, void *pData, DWORD *pSizeOfData) {
  if (Handle < kFirstShaderHandle || !vertex_shaders_.contains(Handle))
    return D3DERR_INVALIDCALL;
  return CopyOutTokenBuffer(vertex_shaders_.at(Handle)->function_tokens,
                            pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShader(DWORD Handle) {
  if (Handle != 0 && !pixel_shaders_.contains(Handle))
    return D3DERR_INVALIDCALL;
  bound_pixel_shader_ = Handle;
  dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShader(DWORD *pHandle) {
  *pHandle = bound_pixel_shader_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShaderFunction(DWORD Handle,
                                                         void *pData,
                                                         DWORD *pSizeOfData) {
  if (!pixel_shaders_.contains(Handle)) return D3DERR_INVALIDCALL;
  return CopyOutTokenBuffer(pixel_shaders_.at(Handle)->function_tokens, pData,
                            pSizeOfData);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstant(
    DWORD Register, CONST void *pConstantData, DWORD ConstantCount) {
  if ((Register + ConstantCount) > kNumVsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;

  memcpy(&bound_vs_cregs_.at(Register), pConstantData,
         ConstantCount * sizeof(float[4]));
  dirty_flags_ |= DIRTY_FLAG_VS_CBUFFER;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstant(
    DWORD Register, void *pConstantData, DWORD ConstantCount) {
  if ((Register + ConstantCount) > kNumVsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;

  memcpy(pConstantData, &bound_vs_cregs_.at(Register),
         ConstantCount * sizeof(float[4]));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstant(
    DWORD Register, CONST void *pConstantData, DWORD ConstantCount) {
  // Bookkeeping only -- bound_ps_cregs_ is not currently wired into the
  // pixel shader's constant buffer (programmable_ps.hlsl reads the *vertex*
  // shader's constant array via the shared b10 cbuffer), so ps.1.x shaders
  // referencing these registers will not see the app's values yet. Storing
  // them anyway means at least GetPixelShaderConstant round-trips correctly
  // and nothing aborts.
  if ((Register + ConstantCount) > kNumPsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;
  memcpy(&bound_ps_cregs_.at(Register), pConstantData,
         ConstantCount * sizeof(float[4]));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstant(
    DWORD Register, void *pConstantData, DWORD ConstantCount) {
  if ((Register + ConstantCount) > kNumPsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &bound_ps_cregs_.at(Register),
         ConstantCount * sizeof(float[4]));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride) {
  TRACE_ENTRY(StreamNumber, pStreamData, Stride);
  if (StreamNumber >= kMaxVertexStreams) return D3DERR_INVALIDCALL;
  if (Stride > caps_.MaxStreamStride) return D3DERR_INVALIDCALL;
  Buffer *buffer = static_cast<Buffer *>(pStreamData);
  bound_vertex_streams_[StreamNumber] = InternalPtr(buffer);
  bound_vertex_stream_strides_[StreamNumber] = Stride;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) {
  if (StreamNumber >= kMaxVertexStreams) return D3DERR_INVALIDCALL;
  Buffer *buffer = bound_vertex_streams_[StreamNumber].Get();
  *ppStreamData = buffer;
  if (buffer) buffer->AddRef();
  *pStride = bound_vertex_stream_strides_[StreamNumber];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetIndices(IDirect3DIndexBuffer8 *pIndexData,
                                             UINT BaseVertexIndex) {
  bound_index_buffer_ = InternalPtr(static_cast<Buffer *>(pIndexData));
  bound_base_vertex_ = BaseVertexIndex;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetIndices(
    IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) {
  Buffer *buffer = bound_index_buffer_.Get();
  *ppIndexData = buffer;
  if (buffer) buffer->AddRef();
  *pBaseVertexIndex = bound_base_vertex_;
  return S_OK;
}

ComPtr<ID3D12PipelineState> Device::CreatePSO(D3DPRIMITIVETYPE d3d8_prim_type) {
  std::array<bool, kMaxTexStages> stage_has_texture = {};
  for (int i = 0; i < 8; ++i) {
    stage_has_texture[i] = bound_textures_[i];
    if (!stage_has_texture[i]) break;
  }
  ASSERT(bound_vertex_shader_ != 0);
  VertexShader *vertex_shader = vertex_shaders_.at(bound_vertex_shader_).Get();
  // If no pixel shader is bound, generate a fixed-function shader.
  ComPtr<ID3DBlob> pixel_shader;
  if (bound_pixel_shader_ == 0) {
    // Try to find the fixed-function pixel shader in our cache.
    PixelShaderState key(render_state_, stage_has_texture.data(),
                         texture_stage_states_.data());
    auto iter = ps_cache_.find(key);
    if (iter != ps_cache_.end()) {
      pixel_shader = iter->second;
    } else {
      pixel_shader = CreatePixelShaderFromState(key);
      if (!kDisablePixelShaderCache)
        ps_cache_.emplace_hint(iter, key, pixel_shader);
    }
  } else {
    auto iter = pixel_shaders_.find(bound_pixel_shader_);
    ASSERT(iter != pixel_shaders_.end());
    pixel_shader = iter->second->blob;
  }

  // Matches the render target BeginScene actually binds via
  // OMSetRenderTargets -- the bound target if one's set, otherwise the
  // current back buffer. Games commonly render to an off-screen target with
  // a different format than the backbuffer (radar map, menu blur/reflection
  // effects, etc.), so the PSO's declared output format has to track
  // whichever target is actually bound rather than assuming it's always the
  // backbuffer's format.
  const DXGI_FORMAT current_rtv_format =
      (bound_render_target_ ? bound_render_target_.Get()
                            : back_buffers_.at(current_back_buffer_).Get())
          ->resource_desc()
          .Format;

  // Now that we know our pixel shader, try to look into the PSO cache.
  PSOState pso_key{
      .rs = render_state_,
      .vs = vertex_shader->blob.get(),
      .ps = pixel_shader.get(),
      .prim_type = d3d8_prim_type,
      .dsv_format = bound_depth_target_
                        ? bound_depth_target_->resource_desc().Format
                        : DXGI_FORMAT_UNKNOWN,
      .rtv_format = current_rtv_format};

  // Zero out/normalize every RenderState field that doesn't actually affect
  // the D3D12_GRAPHICS_PIPELINE_STATE_DESC built below, isn't fed into any
  // cbuffer at draw time, and doesn't influence which vertex/pixel shader
  // gets selected (those already get their own distinct `vs`/`ps` blob
  // pointers above, which the PSO key captures) -- otherwise every distinct
  // value any of these fields ever take (e.g. D3DRS_ALPHAREF, commonly
  // varied per-material for alpha-cutout objects like foliage/fences)
  // produces a spurious *new* cache entry for what is, byte-for-byte, an
  // identical PSO. Confirmed dead for PSO/shader-selection purposes by
  // grepping every other use site in this file. This was previously mostly
  // commented out (i.e. not actually applied) -- verified via a real,
  // long GTA: Vice City session that accumulated 75,000+ live D3D12
  // objects by the end (`pso_cache_`/`ps_cache_` never evict), degrading
  // performance over time and eventually crashing outright.
  pso_key.rs.texture_factor = 0;
  pso_key.rs.ambient = 0;
  pso_key.rs.diffuse_material_source = pso_key.rs.specular_material_source =
      pso_key.rs.ambient_material_source = pso_key.rs.emissive_material_source =
          D3DMCS_MATERIAL;
  pso_key.rs.alpha_ref = 0;
  pso_key.rs.dither_enable = 0;
  pso_key.rs.fog_enable = 0;
  pso_key.rs.fog_color = 0;
  pso_key.rs.fog_table_mode = D3DFOG_NONE;
  pso_key.rs.fog_start = 0;
  pso_key.rs.fog_end = 0;
  pso_key.rs.fog_density = 0;
  pso_key.rs.range_fog_enable = 0;
  pso_key.rs.fog_vertex_mode = D3DFOG_NONE;
  pso_key.rs.color_vertex = 0;
  pso_key.rs.local_viewer = FALSE;
  pso_key.rs.normalized_normals = FALSE;

  auto pso_cache_iter = pso_cache_.find(pso_key);
  if (pso_cache_iter != pso_cache_.end()) {
    return pso_cache_iter->second;
  }

  // LOG(INFO) << "Num PSOs: " << std::dec << pso_cache_.size() << "\n";

  ASSERT(render_state_.zbuffer_type <= 1);

  D3D12_PRIMITIVE_TOPOLOGY_TYPE d3d12_prim_type;
  switch (d3d8_prim_type) {
    case D3DPT_POINTLIST:
      d3d12_prim_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;
    case D3DPT_LINELIST:
    case D3DPT_LINESTRIP:
      // The PSO's topology *type* only distinguishes point/line/triangle/
      // patch, not list vs strip -- that's set per-draw via
      // IASetPrimitiveTopology, so list and strip share a PSO type.
      d3d12_prim_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    case D3DPT_TRIANGLELIST:
    case D3DPT_TRIANGLESTRIP:
      d3d12_prim_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
    default:
      FAIL("Unimplemented primitive type %d", d3d8_prim_type);
  }
  ASSERT(render_state_.src_blend <= D3DBLEND_SRCALPHASAT);
  ASSERT(render_state_.dest_blend <= D3DBLEND_SRCALPHASAT);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{
      .pRootSignature = main_root_sig_.get(),
      .VS = {.pShaderBytecode = vertex_shader->blob->GetBufferPointer(),
             .BytecodeLength = vertex_shader->blob->GetBufferSize()},
      .PS = {.pShaderBytecode = pixel_shader->GetBufferPointer(),
             .BytecodeLength = pixel_shader->GetBufferSize()},
      .BlendState =
          {.RenderTarget = {{
               .BlendEnable = render_state_.alpha_blend_enable != 0,
               .SrcBlend = static_cast<D3D12_BLEND>(render_state_.src_blend),
               .DestBlend = static_cast<D3D12_BLEND>(render_state_.dest_blend),
               .BlendOp = static_cast<D3D12_BLEND_OP>(render_state_.blend_op),
               .SrcBlendAlpha = D3D12_BLEND_ONE,
               .DestBlendAlpha = D3D12_BLEND_ZERO,
               .BlendOpAlpha = D3D12_BLEND_OP_ADD,
               .LogicOp = D3D12_LOGIC_OP_NOOP,
               .RenderTargetWriteMask =
                   safe_cast<uint8_t>(render_state_.color_write_enable),
           }}},
      .SampleMask = UINT_MAX,
      .RasterizerState =
          {
              .FillMode = static_cast<D3D12_FILL_MODE>(render_state_.fill_mode),
              .CullMode = render_state_.cull_mode != D3DCULL_NONE
                              ? D3D12_CULL_MODE_BACK
                              : D3D12_CULL_MODE_NONE,
              .FrontCounterClockwise = render_state_.cull_mode == D3DCULL_CW,
              // D3DRS_ZBIAS is a legacy 0-16 integer scale (MSDN: "a larger
              // value indicates a greater bias" toward the camera -- higher
              // values win the depth test against coplanar/near-coplanar
              // geometry, the classic use being a decal drawn flush against
              // a surface without z-fighting it). There's no single correct
              // D3D8->D3D12 unit conversion (MSDN never specified one --
              // real D3D8 drivers already varied per-vendor here), so this
              // is a reasonable small negative scale (toward the camera)
              // rather than the previously-hardcoded 0, which silently
              // dropped the bias entirely and left this class of decal
              // exposed to z-fighting/occlusion mismatches against the
              // surface it's meant to sit on top of.
              .DepthBias = -static_cast<INT>(render_state_.z_bias) * 16,
              .DepthBiasClamp = 0.f,
              .MultisampleEnable = render_state_.multisample_antialias != 0,
              .AntialiasedLineEnable = render_state_.edge_antialias != 0,
          },
      .DepthStencilState =
          {
              .DepthEnable = render_state_.zbuffer_type && bound_depth_target_,
              .DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(
                  render_state_.zwrite_enable != 0),
              .DepthFunc =
                  static_cast<D3D12_COMPARISON_FUNC>(render_state_.z_func),
          },
      .InputLayout = {.pInputElementDescs =
                          vertex_shader->decl.input_elements.data(),
                      .NumElements = static_cast<UINT>(
                          vertex_shader->decl.input_elements.size())},
      .PrimitiveTopologyType = d3d12_prim_type,
      .NumRenderTargets = 1,
      .RTVFormats = {current_rtv_format},
      .DSVFormat = bound_depth_target_
                       ? bound_depth_target_->resource_desc().Format
                       : DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1, .Quality = 0}};
  ComPtr<ID3D12PipelineState> pso;
  ASSERT_HR(d3d12_device_->CreateGraphicsPipelineState(
      &desc, IID_PPV_ARGS(pso.GetForInit())));
  if (!kDisablePsoCache)
    pso_cache_.emplace_hint(pso_cache_iter, std::move(pso_key), pso);
  return pso;
}

HRESULT STDMETHODCALLTYPE Device::BeginScene() {
  TRACE_ENTRY();
  // Set viewports.
  cmd_list_->RSSetViewports(1, &viewport_);
  D3D12_RECT scissors = {.left = 0,
                         .top = 0,
                         .right = static_cast<LONG>(viewport_.Width),
                         .bottom = static_cast<LONG>(viewport_.Height)};
  cmd_list_->RSSetScissorRects(1, &scissors);

  ID3D12DescriptorHeap *heaps[] = {srv_heap_.heap(), sampler_heap_.heap()};
  cmd_list_->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);

  GpuTexture *render_target =
      bound_render_target_ ? bound_render_target_.Get()
                           : back_buffers_.at(current_back_buffer_).Get();

  // Transition the back buffer from present (or common) to render target.
  TransitionTexture(render_target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Set the default render targets.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_target->rtv_handle();
  D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
  if (bound_depth_target_) {
    dsv_handle = bound_depth_target_->dsv_handle();
    MarkResourceAsUsed(InternalPtr(bound_depth_target_.Get()));
  }
  cmd_list_->OMSetRenderTargets(1, &rtv_handle, 1,
                                bound_depth_target_ ? &dsv_handle : nullptr);
  MarkResourceAsUsed(InternalPtr(render_target));
  dirty_flags_ ^= DIRTY_FLAG_OM;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE Device::EndScene() { return S_OK; }

HRESULT STDMETHODCALLTYPE Device::Clear(DWORD Count, CONST D3DRECT *pRects,
                                        DWORD Flags, D3DCOLOR Color, float Z,
                                        DWORD Stencil) {
  // Small-buffer optimized: D3D8 Clear rect counts are tiny in practice (a
  // full-screen clear passes none at all), so the common case shouldn't heap
  // allocate. Falls back to the vector only for an unexpectedly large Count.
  std::array<D3D12_RECT, 8> rect_inline;
  std::vector<D3D12_RECT> rect_storage;
  D3D12_RECT *rects = nullptr;
  if (pRects) {
    D3D12_RECT *rect_dest = rect_inline.data();
    if (Count > rect_inline.size()) {
      rect_storage.resize(Count);
      rect_dest = rect_storage.data();
    }
    for (DWORD i = 0; i < Count; ++i) {
      rect_dest[i] = {.left = pRects[i].x1,
                         .top = pRects[i].y1,
                         .right = pRects[i].x2,
                         .bottom = pRects[i].y2};
    }
    rects = rect_dest;
  }

  if (Flags & D3DCLEAR_TARGET) {
    // Clear can be called before BeginScene - so make sure to transition the
    // render taret.
    GpuTexture *render_target = bound_render_target_
                                    ? bound_render_target_.Get()
                                    : back_buffers_[current_back_buffer_].Get();
    TransitionTexture(render_target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float color[4] = {((Color >> 16) & 0xFF) / 255.f,
                      ((Color >> 8) & 0xFF) / 255.f, (Color & 0xFF) / 255.f,
                      ((Color >> 24) & 0xFF) / 255.f};
    cmd_list_->ClearRenderTargetView(render_target->rtv_handle(), color,
                                     static_cast<UINT>(rects ? Count : 0),
                                     rects);
  }
  if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
    if (!bound_depth_target_) {
      LOG_ERROR()
          << "Do not have any depth stencil texture allocated to clear.\n";
      return D3DERR_INVALIDCALL;
    }
    D3D12_CLEAR_FLAGS clear_flags = {};
    if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
    cmd_list_->ClearDepthStencilView(
        bound_depth_target_->dsv_handle(), clear_flags, Z,
        static_cast<UINT8>(Stencil), static_cast<UINT>(rects ? Count : 0),
        rects);
  }
  return S_OK;
}

HRESULT Device::PrepareDrawCall(D3DPRIMITIVETYPE PrimitiveType,
                                int start_vertex, int num_vertices) {
  if (PrimitiveType > D3DPT_TRIANGLEFAN) {
    LOG_ERROR() << "Invalid primitive type " << PrimitiveType << "\n";
    return D3DERR_INVALIDCALL;
  }
  ASSERT(PrimitiveType !=
         D3DPT_TRIANGLEFAN);  // We don't actually support fans.

  // Configure the output-merger stage if anything reset it (like flushes).
  if (dirty_flags_ & DIRTY_FLAG_OM) {
    BeginScene();
  }

  // Most draws in a row reuse the same primitive type (e.g. a long run of
  // D3DPT_TRIANGLELIST calls) -- skip the redundant IASetPrimitiveTopology
  // call rather than reissuing it on every single draw.
  {
    D3D12_PRIMITIVE_TOPOLOGY topology =
        static_cast<D3D12_PRIMITIVE_TOPOLOGY>(PrimitiveType);
    if (topology != last_prim_topology_) {
      cmd_list_->IASetPrimitiveTopology(topology);
      last_prim_topology_ = topology;
    }
  }

  ASSERT(bound_vertex_shader_ != 0);
  VertexShader *vertex_shader = vertex_shaders_.at(bound_vertex_shader_).Get();
  if (bound_vertex_shader_ >= kFirstShaderHandle) {
    MarkResourceAsUsed(InternalPtr(vertex_shader));
  }
  if (bound_pixel_shader_) {
    MarkResourceAsUsed(
        InternalPtr(pixel_shaders_.at(bound_pixel_shader_).Get()));
  }

  std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexStreams> vbuffer_views = {};
  size_t max_index = 0;
  for (size_t i = 0; i < bound_vertex_streams_.size(); ++i) {
    if (vertex_shader->decl.buffer_strides[i] > 0) {
      auto &d3d_buffer = bound_vertex_streams_[i];
      if (d3d_buffer) {
        Buffer *buffer = static_cast<Buffer *>(d3d_buffer.Get());
        TransitionBuffer(buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        int stride = vertex_shader->decl.buffer_strides[i];
        vbuffer_views[i] = {.BufferLocation = buffer->GetGpuPtr(),
                            .SizeInBytes = static_cast<UINT>(
                                stride * (start_vertex + num_vertices)),
                            .StrideInBytes = static_cast<UINT>(stride)};
        if (i > max_index) max_index = i;
        MarkResourceAsUsed(bound_vertex_streams_[i]);
      } else {
        // FAIL("Shader requires bound buffer at slot %d, but none are bound.",
        // i);
      }
    }
  }

  // Skip the rebind when nothing about the stream bindings changed. The
  // views do vary per draw whenever the vertex range does (SizeInBytes is
  // derived from start_vertex/num_vertices), so this only helps runs of
  // draws sharing the same range -- but those runs are common, and the
  // comparison is far cheaper than the driver call it avoids.
  const size_t vbuffer_view_count = max_index + 1;
  if (!kCacheDrawStateBindings || vbuffer_view_count != last_vbuffer_view_count_ ||
      memcmp(last_vbuffer_views_.data(), vbuffer_views.data(),
             vbuffer_view_count * sizeof(vbuffer_views[0])) != 0) {
    cmd_list_->IASetVertexBuffers(0, static_cast<UINT>(vbuffer_view_count),
                                  vbuffer_views.data());
    memcpy(last_vbuffer_views_.data(), vbuffer_views.data(),
           vbuffer_view_count * sizeof(vbuffer_views[0]));
    last_vbuffer_view_count_ = vbuffer_view_count;
  }

  // Only rebuild the PSO cache key when something it depends on actually
  // changed. CreatePSO's key (PSOState + PixelShaderState) copies and hashes
  // the entire RenderState and all 8 TextureStageStates -- over a kilobyte
  // -- and then compares the same again on a cache hit, so running it for
  // every draw call dominated state-change cost in draw-heavy frames even
  // though consecutive draws almost always share identical state. The
  // primitive type is part of the key but arrives as a per-draw argument
  // rather than device state, so it's compared separately.
  if (!kCacheDrawStateBindings || (dirty_flags_ & DIRTY_FLAG_PSO) ||
      PrimitiveType != last_pso_prim_type_) {
    last_pso_ = CreatePSO(PrimitiveType);
    last_pso_prim_type_ = PrimitiveType;
    if (dirty_flags_ & DIRTY_FLAG_PSO) dirty_flags_ ^= DIRTY_FLAG_PSO;
  }
  if (!kCacheDrawStateBindings || last_pso_.get() != last_set_pso_) {
    cmd_list_->SetPipelineState(last_pso_.get());
    last_set_pso_ = last_pso_.get();
  }
  // MarkResourceAsUsed(pso);
  using ::DirectX::SimpleMath::Matrix;
  // Only the transform and lighting cbuffer updates below consume this, and
  // both are gated on their own dirty flag -- computing it unconditionally
  // meant an unordered_map lookup plus a 64-byte copy and conversion on
  // every single draw call, with the result thrown away for the large
  // majority of them (a typical frame changes transforms/lights far less
  // often than it draws).
  Matrix view;
  if (dirty_flags_ & (DIRTY_FLAG_TRANSFORMS | DIRTY_FLAG_LIGHTS)) {
    view = MatrixFromD3D(GetTransform(D3DTS_VIEW));
  }

  // Set the vertex cbuffer.
  if (dirty_flags_ & DIRTY_FLAG_TRANSFORMS) {
    VertexCBuffer *cbuffer;
    ASSERT_HR(vs_cbuffer_->Lock(0, sizeof(VertexCBuffer), (BYTE **)&cbuffer,
                                D3DLOCK_DISCARD));
    Matrix proj = MatrixFromD3D(GetTransform(D3DTS_PROJECTION));
    Matrix world = MatrixFromD3D(GetTransform(D3DTS_WORLD));
    cbuffer->world_view_proj = world * view * proj;
    cbuffer->world_view = world * view;
    cbuffer->camera_position = DirectX::SimpleMath::Vector3(0, 0, 0);
    cbuffer->inv_viewport_size = DirectX::SimpleMath::Vector2(
        2.f / viewport_.Width, 2.f / viewport_.Height);
    ASSERT_HR(vs_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_TRANSFORMS;
  }
  if (dirty_flags_ & DIRTY_FLAG_VS_CBUFFER) {
    // TODO: Only copy changed constants.
    BYTE *cbuffer;
    ASSERT_HR(vs_creg_cbuffer_->Lock(
        0, bound_vs_cregs_.size() * sizeof(bound_vs_cregs_[0]), &cbuffer,
        D3DLOCK_DISCARD));
    memcpy(cbuffer, bound_vs_cregs_.data(),
           bound_vs_cregs_.size() * sizeof(bound_vs_cregs_[0]));
    ASSERT_HR(vs_creg_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_VS_CBUFFER;
  }
  if (dirty_flags_ & DIRTY_FLAG_LIGHTS) {
    LightsCBuffer *cbuffer;
    ASSERT_HR(lights_cbuffer_->Lock(0, sizeof(LightsCBuffer),
                                    reinterpret_cast<BYTE **>(&cbuffer),
                                    D3DLOCK_DISCARD));
    int i = 0;
    ASSERT(enabled_lights_.size() <= kMaxActiveLights);
    for (auto light_index : enabled_lights_) {
      // ASSERT(render_state_.lighting);
      cbuffer->lights[i] = ShaderLightMarshall(view, lights_[light_index]);
      ++i;
    }
    cbuffer->num_lights = i;
    cbuffer->diffuse_material_source =
        render_state_.color_vertex ? render_state_.diffuse_material_source
                                   : D3DMCS_MATERIAL;
    cbuffer->ambient_material_source =
        render_state_.color_vertex ? render_state_.ambient_material_source
                                   : D3DMCS_MATERIAL;
    cbuffer->specular_material_source =
        render_state_.color_vertex ? render_state_.specular_material_source
                                   : D3DMCS_MATERIAL;
    cbuffer->specular_enable = render_state_.specular_enable;
    cbuffer->lighting_enabled = render_state_.lighting;
    cbuffer->global_ambient = Dx8::Color(render_state_.ambient).ToValue();
    ASSERT_HR(lights_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_LIGHTS;
  }
  if (dirty_flags_ & DIRTY_FLAG_PS_CBUFFER) {
    // And pixel cbuffer.
    PixelCBuffer *cbuffer;
    ASSERT_HR(ps_cbuffer_->Lock(0, sizeof(PixelCBuffer), (BYTE **)&cbuffer,
                                D3DLOCK_DISCARD));
    cbuffer->material_diffuse = material_.Diffuse;
    cbuffer->material_ambient = material_.Ambient;
    cbuffer->material_specular = material_.Specular;
    cbuffer->material_power = material_.Power;

    cbuffer->alpha_ref = (render_state_.alpha_ref & 0xFF) / 255.f;
    cbuffer->texture_factor =
        Dx8::Color(render_state_.texture_factor).ToValue();
    ASSERT_HR(ps_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_PS_CBUFFER;
  }
  // The root signature only actually needs (re)binding once per command
  // list, not once per draw. Re-binding it is not free, and per the D3D12
  // spec it also *invalidates every root argument* -- so the old
  // unconditional call here was, strictly speaking, invalidating the CBVs
  // and descriptor tables set just below it on the previous draw and relying
  // on them being re-set again. Bind it once per command list instead, and
  // invalidate our own root-argument caches whenever we do.
  if (!kCacheDrawStateBindings || !root_sig_bound_) {
    cmd_list_->SetGraphicsRootSignature(main_root_sig_.get());
    root_sig_bound_ = true;
    last_root_cbvs_.fill(0);
    // Descriptor tables are root arguments too, so they need re-issuing for
    // the same reason.
    dirty_flags_ |= DIRTY_FLAG_PS_TEXTURES;
    dirty_flags_ |= DIRTY_FLAG_PS_SAMPLERS;
  }

  // Set all the necessary roots. These addresses only change when the
  // underlying cbuffer is re-locked with D3DLOCK_DISCARD (which hands back a
  // fresh ring-buffer allocation) -- which is exactly what the dirty-flag
  // blocks above do, and only when something actually changed. On every
  // other draw all four are identical to what's already bound.
  auto set_root_cbv = [&](UINT slot, GpuPtr gpu_ptr) {
    const D3D12_GPU_VIRTUAL_ADDRESS address = gpu_ptr;
    if (kCacheDrawStateBindings && last_root_cbvs_[slot] == address) return;
    last_root_cbvs_[slot] = address;
    cmd_list_->SetGraphicsRootConstantBufferView(slot, address);
  };
  set_root_cbv(0, vs_cbuffer_->GetGpuPtr());
  set_root_cbv(1, ps_cbuffer_->GetGpuPtr());
  set_root_cbv(2, lights_cbuffer_->GetGpuPtr());
  set_root_cbv(3, vs_creg_cbuffer_->GetGpuPtr());

  // Keep every currently-bound texture's keep-alive ref fresh for this
  // frame's back-buffer slot, regardless of whether the GPU-visible
  // descriptor table binding itself needs to be re-issued below. D3D8
  // texture bindings are sticky -- a game can (and routinely does) draw many
  // times, across many frames, off a single SetTexture call, relying on the
  // binding staying in effect without repeating it. MarkResourceAsUsed used
  // to only run inside the "rebind the descriptor table" branch, which only
  // fires on the draw right after SetTexture actually changes something; on
  // every later draw reusing the same sticky binding, the bound texture was
  // never re-marked as used for that frame's slot. Once this session's
  // MarkResourceAsUsed dedup (see slot_generation_) started skipping the
  // *previously unconditional* AddRef instead of just being redundant with
  // it, a sticky-bound texture could have its keep-alive ref lapse (and its
  // SRV descriptor slot get freed and reused by a different texture) while
  // the GPU was still actively rendering from that same descriptor slot --
  // observed as another texture's contents flashing in (a font atlas filling
  // a menu background, the sky flickering). MarkResourceAsUsed is itself
  // already a cheap same-generation dedup check, so doing this unconditionally
  // every draw is fine.
  for (int i = 0; i < kMaxTexStages; ++i) {
    if (bound_textures_[i]) {
      MarkResourceAsUsed(bound_textures_[i]);
    }
  }

  if (dirty_flags_ & DIRTY_FLAG_PS_TEXTURES) {
    // And all the textures.
    for (int i = 0; i < kMaxTexStages; ++i) {
      if (bound_textures_[i]) {
        const auto gpu_handle =
            srv_heap_.GetGPUHandleFor(bound_textures_[i]->srv_handle());
        cmd_list_->SetGraphicsRootDescriptorTable(textures_start_bindslot_ + i,
                                                  gpu_handle);
      }
    }
    dirty_flags_ ^= DIRTY_FLAG_PS_TEXTURES;
  }

  if (dirty_flags_ & DIRTY_FLAG_PS_SAMPLERS) {
    // Set all the samplers.
    for (int i = 0; i < kMaxTexStages; ++i) {
      SamplerDesc desc(texture_stage_states_[i]);
      auto iter = sampler_cache_.find(desc);
      if (iter == sampler_cache_.end()) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = sampler_heap_.Allocate();
        d3d12_device_->CreateSampler(&desc, cpu_handle);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle =
            sampler_heap_.GetGPUHandleFor(cpu_handle);
        iter = sampler_cache_.insert(iter, std::pair(desc, gpu_handle));
      }
      ASSERT(iter->second.ptr != 0);
      cmd_list_->SetGraphicsRootDescriptorTable(
          textures_start_bindslot_ + kMaxTexStages + i, iter->second);
    }
    dirty_flags_ ^= DIRTY_FLAG_PS_SAMPLERS;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                                UINT StartVertex,
                                                UINT PrimitiveCount) {
  // D3D12 has no fan topology. Emulate it with a generated index list (0,
  // i+1, i+2 for each triangle) drawn as a triangle list against the
  // already-bound vertex buffer, the same trick DrawPrimitiveUP already uses
  // by rewriting the vertex data directly -- here we can't rewrite the
  // (GPU-side, already bound) vertex buffer, so we index into it instead.
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    const UINT vertex_count = PrimitiveCount + 2;
    const UINT index_count = 3 * PrimitiveCount;
    std::vector<uint16_t> indices;
    indices.reserve(index_count);
    for (UINT i = 0; i < PrimitiveCount; ++i) {
      indices.push_back(static_cast<uint16_t>(StartVertex));
      indices.push_back(static_cast<uint16_t>(StartVertex + i + 1));
      indices.push_back(static_cast<uint16_t>(StartVertex + i + 2));
    }
    const size_t index_bytes = index_count * sizeof(uint16_t);
    DynamicRingBuffer::Allocation index_alloc =
        dynamic_ring_buffer()->Allocate(index_bytes);
    memcpy(dynamic_ring_buffer()->GetCpuPtrFor(index_alloc), indices.data(),
           index_bytes);
    D3D12_INDEX_BUFFER_VIEW ib_view{
        .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(index_alloc),
        .SizeInBytes = safe_cast<UINT>(index_bytes),
        .Format = DXGI_FORMAT_R16_UINT};

    HR_OR_RETURN(
        PrepareDrawCall(D3DPT_TRIANGLELIST, StartVertex, vertex_count));
    cmd_list_->IASetIndexBuffer(&ib_view);
    cmd_list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    return S_OK;
  }

  int vertex_count;
  switch (PrimitiveType) {
    case D3DPT_POINTLIST:
      vertex_count = PrimitiveCount;
      break;
    case D3DPT_LINELIST:
      vertex_count = 2 * PrimitiveCount;
      break;
    case D3DPT_LINESTRIP:
      vertex_count = 1 + PrimitiveCount;
      break;
    case D3DPT_TRIANGLELIST:
      vertex_count = 3 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      vertex_count = 2 + PrimitiveCount;
      break;
    default:
      FAIL("TODO: Count number of vertices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }
  HR_OR_RETURN(PrepareDrawCall(PrimitiveType, StartVertex, vertex_count));
  cmd_list_->DrawInstanced(vertex_count, 1, StartVertex, 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  if (!bound_vertex_shader_) {
    LOG_ERROR() << "Cannot use DrawPrimitiveUP without a vertex shader.\n";
    return D3DERR_INVALIDCALL;
  }

  // Rewrite triangle fans as triangle lists.
  std::vector<uint8_t> rewritten_fan;
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    rewritten_fan.reserve(3 * PrimitiveCount * VertexStreamZeroStride);

    auto insert_vertex = [&](uint32_t index) {
      const uint8_t *pStart =
          static_cast<const uint8_t *>(pVertexStreamZeroData) +
          index * VertexStreamZeroStride;
      std::copy(pStart, pStart + VertexStreamZeroStride,
                std::back_inserter(rewritten_fan));
    };

    for (uint32_t i = 0; i < PrimitiveCount; ++i) {
      insert_vertex(0);
      insert_vertex(i + 1);
      insert_vertex(i + 2);
    }
    pVertexStreamZeroData = rewritten_fan.data();
    PrimitiveType = D3DPT_TRIANGLELIST;
  }

  int vertex_count;
  switch (PrimitiveType) {
    case D3DPT_LINELIST:
      vertex_count = 2 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLELIST:
      vertex_count = 3 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      vertex_count = 2 + PrimitiveCount;
      break;
    default:
      FAIL("TODO: Count number of vertices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }

  // Allocate some ring buffer memory.
  size_t num_bytes = vertex_count * VertexStreamZeroStride;
  DynamicRingBuffer::Allocation alloc =
      dynamic_ring_buffer()->Allocate(num_bytes);
  memcpy(dynamic_ring_buffer()->GetCpuPtrFor(alloc), pVertexStreamZeroData,
         num_bytes);
  D3D12_VERTEX_BUFFER_VIEW vbuffer_view{
      .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(alloc),
      .SizeInBytes = safe_cast<UINT>(num_bytes),
      .StrideInBytes = VertexStreamZeroStride};

  ASSERT_HR(SetStreamSource(0, nullptr, 0));
  HR_OR_RETURN(PrepareDrawCall(PrimitiveType, 0, vertex_count));
  // Overwrite whatever vertex buffer the prepare set. This bypasses
  // PrepareDrawCall's vertex-buffer-view cache, so that cache has to be
  // invalidated -- otherwise a later regular (non-UP) draw whose views
  // happen to match the cached entry would skip its rebind and wrongly keep
  // rendering from this call's scratch ring-buffer data.
  cmd_list_->IASetVertexBuffers(0, 1, &vbuffer_view);
  last_vbuffer_view_count_ = 0;
  cmd_list_->DrawInstanced(vertex_count, 1, 0, 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices,
    UINT startIndex, UINT primCount) {
  if (!bound_index_buffer_) return D3DERR_INVALIDCALL;

  int index_count;
  switch (PrimitiveType) {
    case D3DPT_POINTLIST:
      index_count = primCount;
      break;
    case D3DPT_LINELIST:
      index_count = 2 * primCount;
      break;
    case D3DPT_LINESTRIP:
      index_count = 1 + primCount;
      break;
    case D3DPT_TRIANGLELIST:
      index_count = 3 * primCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      index_count = 2 + primCount;
      break;
    default:
      FAIL("TODO: Count number of vertices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }

  HR_OR_RETURN(PrepareDrawCall(PrimitiveType, minIndex + bound_base_vertex_,
                               NumVertices));

  TransitionBuffer(bound_index_buffer_.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER);
  D3D12_INDEX_BUFFER_VIEW ib_view{
      .BufferLocation = bound_index_buffer_->GetGpuPtr(),
      .SizeInBytes = static_cast<UINT>(
          DXGIFormatSize(bound_index_buffer_->index_buffer_fmt()) *
          (startIndex + index_count)),
      .Format = bound_index_buffer_->index_buffer_fmt()};
  MarkResourceAsUsed(bound_index_buffer_);
  cmd_list_->IASetIndexBuffer(&ib_view);

  cmd_list_->DrawIndexedInstanced(index_count, 1, startIndex,
                                  bound_base_vertex_, 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
    UINT NumVertexIndices, UINT PrimitiveCount, CONST void *pIndexData,
    D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData,
    UINT VertexStreamZeroStride) {
  TRACE_ENTRY(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount,
             pIndexData, IndexDataFormat, pVertexStreamZeroData,
             VertexStreamZeroStride);
  if (!bound_vertex_shader_) {
    LOG_ERROR()
        << "Cannot use DrawIndexedPrimitiveUP without a vertex shader.\n";
    return D3DERR_INVALIDCALL;
  }
  if (IndexDataFormat != D3DFMT_INDEX16 && IndexDataFormat != D3DFMT_INDEX32) {
    LOG_ERROR() << "Invalid IndexDataFormat for DrawIndexedPrimitiveUP: "
                << IndexDataFormat << "\n";
    return D3DERR_INVALIDCALL;
  }
  if (pIndexData == nullptr || pVertexStreamZeroData == nullptr ||
      VertexStreamZeroStride == 0) {
    LOG_ERROR() << "Invalid DrawIndexedPrimitiveUP arguments: pIndexData="
                << pIndexData << " pVertexStreamZeroData="
                << pVertexStreamZeroData
                << " VertexStreamZeroStride=" << VertexStreamZeroStride
                << "\n";
    return D3DERR_INVALIDCALL;
  }
  // Not supported: PrepareDrawCall rejects fans outright, and rewriting a fan
  // index list (as opposed to DrawPrimitiveUP's flat vertex list) isn't
  // implemented.
  ASSERT(PrimitiveType != D3DPT_TRIANGLEFAN);

  int index_count;
  switch (PrimitiveType) {
    case D3DPT_LINELIST:
      index_count = 2 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLELIST:
      index_count = 3 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      index_count = 2 + PrimitiveCount;
      break;
    default:
      FAIL("TODO: Count number of indices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }

  // Upload the vertex data. MinVertexIndex/NumVertexIndices describe the
  // range of vertices this call actually touches, but pVertexStreamZeroData
  // is indexed from element 0 (indices in pIndexData are absolute, not
  // relative to MinVertexIndex), so we have to bring along everything up to
  // the top of that range.
  const size_t num_vertices_to_upload = MinVertexIndex + NumVertexIndices;
  const size_t vertex_bytes = num_vertices_to_upload * VertexStreamZeroStride;
  DynamicRingBuffer::Allocation vertex_alloc =
      dynamic_ring_buffer()->Allocate(vertex_bytes);
  memcpy(dynamic_ring_buffer()->GetCpuPtrFor(vertex_alloc),
         pVertexStreamZeroData, vertex_bytes);
  D3D12_VERTEX_BUFFER_VIEW vbuffer_view{
      .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(vertex_alloc),
      .SizeInBytes = safe_cast<UINT>(vertex_bytes),
      .StrideInBytes = VertexStreamZeroStride};

  // Upload the index data.
  const DXGI_FORMAT index_format = DXGIFromD3DFormat(IndexDataFormat);
  const size_t index_bytes =
      static_cast<size_t>(index_count) * DXGIFormatSize(index_format);
  DynamicRingBuffer::Allocation index_alloc =
      dynamic_ring_buffer()->Allocate(index_bytes);
  memcpy(dynamic_ring_buffer()->GetCpuPtrFor(index_alloc), pIndexData,
         index_bytes);
  D3D12_INDEX_BUFFER_VIEW ib_view{
      .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(index_alloc),
      .SizeInBytes = safe_cast<UINT>(index_bytes),
      .Format = index_format};

  ASSERT_HR(SetStreamSource(0, nullptr, 0));
  HR_OR_RETURN(PrepareDrawCall(PrimitiveType, static_cast<int>(MinVertexIndex),
                               static_cast<int>(NumVertexIndices)));
  // Overwrite whatever vertex/index buffer the prepare set. See the matching
  // note in DrawPrimitiveUP for why the view cache must be invalidated here.
  cmd_list_->IASetVertexBuffers(0, 1, &vbuffer_view);
  last_vbuffer_view_count_ = 0;
  cmd_list_->IASetIndexBuffer(&ib_view);
  cmd_list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::Present(CONST RECT *pSourceRect,
                                          CONST RECT *pDestRect,
                                          HWND hDestWindowOverride,
                                          CONST RGNDATA *pDirtyRegion) {
  TRACE_ENTRY(hDestWindowOverride);
  ASSERT(hDestWindowOverride == nullptr || hDestWindowOverride == window_);
  SubmitAndWait(true);
  return S_OK;
}

// Only used during reset. Does not clean up fence state.
void Device::SubmitAndWait(bool should_present) {
  // TEMP DIAGNOSTIC: see perf_wait_ticks_accum_ comment in device.h.
  LARGE_INTEGER perf_now;
  QueryPerformanceCounter(&perf_now);
  if (should_present) {
    if (perf_last_frame_ticks_ != 0) {
      perf_frame_ticks_accum_ += perf_now.QuadPart - perf_last_frame_ticks_;
      perf_wait_ticks_accum_ += perf_wait_ticks_this_frame_;
      ++perf_frame_sample_count_;
      if (perf_frame_sample_count_ >= 120) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double avg_frame_ms = 1000.0 * perf_frame_ticks_accum_ /
                              perf_frame_sample_count_ / freq.QuadPart;
        double avg_wait_ms = 1000.0 * perf_wait_ticks_accum_ /
                             perf_frame_sample_count_ / freq.QuadPart;
        LOG(AixLog::Severity::error)
            << "Perf: avg frame=" << avg_frame_ms
            << "ms (fps=" << (1000.0 / avg_frame_ms)
            << ") avg GPU-fence-wait=" << avg_wait_ms << "ms ("
            << (100.0 * avg_wait_ms / avg_frame_ms) << "% of frame)\n";
        perf_frame_ticks_accum_ = 0;
        perf_wait_ticks_accum_ = 0;
        perf_frame_sample_count_ = 0;
      }
    }
    if (perf_last_frame_ticks_ != 0) {
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      last_frame_ms_ = 1000.0 *
                       static_cast<double>(perf_now.QuadPart -
                                           perf_last_frame_ticks_) /
                       static_cast<double>(freq.QuadPart);
    }
    perf_last_frame_ticks_ = perf_now.QuadPart;
    perf_wait_ticks_this_frame_ = 0;
  }

  ASSERT(!(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED));

  // Let mod-API render callbacks draw on top of this frame's backbuffer
  // while it's still bound and the command list is still open -- see
  // RegisterModRenderCallback. BeginScene only actually runs lazily, on a
  // game's first draw/Clear call of the frame (DIRTY_FLAG_OM) -- a frame
  // presented with zero game draws (e.g. right after Reset(), or a blank
  // loading-screen frame) would otherwise reach here with no render target
  // ever bound and the backbuffer still sitting in D3D12_RESOURCE_STATE_
  // PRESENT, not RENDER_TARGET. A mod callback recording into that raw,
  // uninitialized command list is a well-known way to crash the GPU driver
  // (observed: an access violation inside nvwgf2um.dll while recording a
  // mod's DrawIndexedInstanced). Force BeginScene here so the contract
  // MODDING.md documents -- backbuffer bound as the active render target --
  // actually holds on every presented frame, not just ones the game itself
  // drew into.
  if (should_present) {
    if (dirty_flags_ & DIRTY_FLAG_OM) {
      BeginScene();
    }
    for (ModRenderCallback callback : mod_render_callbacks_) {
      callback(cmd_list_.Get());
    }
  }

  // Transition back buffer to present.
  if (should_present) {
    TransitionTexture(back_buffers_[current_back_buffer_].get(), 0,
                      D3D12_RESOURCE_STATE_PRESENT);
  }

  // Persist any dynamic buffers.
  for (auto buffer : buffers_to_persist_) {
    buffer->PersistDynamicChanges();
    buffer->set_marked_for_persist(false);
  }
  buffers_to_persist_.clear();

  // Close the command list, then execute it.
  ASSERT_HR(cmd_list_->Close());
  dirty_flags_ |= DIRTY_FLAG_CMD_LIST_CLOSED;
  ID3D12CommandList *cmd_list = cmd_list_.Get();
  cmd_queue_->ExecuteCommandLists(1, &cmd_list);
  // Present!
  if (should_present) {
    ASSERT_HR(swap_chain_->Present(
        sync_interval_, sync_interval_ == 0 && tearing_supported_
                             ? DXGI_PRESENT_ALLOW_TEARING
                             : 0));
  }

  // Grab a new fence value, set it at the end of the command queue execution.
  fence_values_.at(current_back_buffer_) = next_fence_++;
  ASSERT_HR(cmd_queue_->Signal(cmd_list_done_fence_.get(),
                               fence_values_[current_back_buffer_]));

  // Update our back buffer index.
  current_back_buffer_ = swap_chain_->GetCurrentBackBufferIndex();

  // Wait for it.
  WaitForFrame(fence_values_[current_back_buffer_]);

  // Reset the command list for the next frame.
  ASSERT_HR(cmd_allocators_[current_back_buffer_]->Reset());
  ASSERT_HR(
      cmd_list_->Reset(cmd_allocators_[current_back_buffer_].get(), nullptr));
  dirty_flags_ ^= DIRTY_FLAG_CMD_LIST_CLOSED;
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
  // Resetting the command list drops all IA state (topology included), so
  // the next draw must re-set it regardless of what PrepareDrawCall's own
  // redundant-set check (last_prim_topology_) thinks is currently bound.
  last_prim_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  // Likewise for the root signature, root arguments, and pipeline state.
  root_sig_bound_ = false;
  last_set_pso_ = nullptr;
  last_vbuffer_view_count_ = 0;
}

void Device::WaitForFrame(uint64_t frame_number) {
  ASSERT(frame_number <= next_fence_);

  if (cmd_list_done_fence_->GetCompletedValue() < frame_number) {
    // Is this a frame that we're currently building?
    if (frame_number + 1 == next_fence_ &&
        !(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED)) {
      // SubmitAndWait will call us again to wait for the frame, but at that
      // point fence_values_[current_back_buffer_] will have incremented.
      SubmitAndWait(false);
    } else {
      ASSERT_HR(cmd_list_done_fence_->SetEventOnCompletion(
          frame_number, cmd_list_done_event_handle_));
      // TEMP DIAGNOSTIC: see perf_wait_ticks_accum_ comment in device.h.
      LARGE_INTEGER wait_start, wait_end;
      QueryPerformanceCounter(&wait_start);
      DWORD wait_result =
          WaitForSingleObjectEx(cmd_list_done_event_handle_, 60 * 1000, FALSE);
      QueryPerformanceCounter(&wait_end);
      perf_wait_ticks_this_frame_ += wait_end.QuadPart - wait_start.QuadPart;
      if (wait_result != WAIT_OBJECT_0) {
        // The fence never signaled -- most likely the GPU driver hung and
        // got TDR-reset (device removed), or genuinely never finished this
        // workload. Previously this return value was ignored entirely: on
        // timeout, execution fell straight through to FreeFrameResources()
        // below as if the wait had succeeded, freeing/reusing resources the
        // GPU might still (think it) owns -- silent corruption instead of a
        // diagnosable failure, observed in practice as an unresponsive
        // ~60s "freeze" followed by the process dying with no error shown.
        HRESULT removed_reason = d3d12_device_->GetDeviceRemovedReason();
        FAIL(
            "WaitForFrame: fence %llu never signaled after 60s (wait_result="
            "0x%X). GetDeviceRemovedReason=0x%X -- the GPU driver likely "
            "hung or was TDR-reset.",
            static_cast<unsigned long long>(frame_number), wait_result,
            removed_reason);
      }
    }
  }

  // Free any frame resources.
  FreeFrameResources(frame_number);
}

void Device::FreeFrameResources(uint64_t frame_number) {
  for (size_t i = 0; i < frame_resources_to_free_.size(); ++i) {
    if (fence_values_[i] <= frame_number) {
      frame_resources_to_free_[i].clear();
      // Invalidate every resource's "already marked for this slot" stamp --
      // see RefCounted::last_marked_generation_ / MarkResourceAsUsed.
      ++slot_generation_[i];
    }
  }

  dynamic_ring_buffer_->HasCompletedFrame(frame_number);
  dynamic_ring_buffer_->SetCurrentFrame(CurrentFrame());
}

uint64_t Device::CurrentFrame() const { return next_fence_; }

}  // namespace Dx8to12