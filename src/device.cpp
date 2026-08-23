#include "device.h"

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>

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

// static_assert(sizeof(void *) == 4, "Does not support 64-bit.");

// DXGI_SWAP_EFFECT_FLIP_DISCARD swap chains only accept a handful of formats,
// none of which lack an alpha channel. D3DFMT_X8R8G8B8 -- by far the most
// common DX8 backbuffer format -- maps to DXGI_FORMAT_B8G8R8X8_UNORM via
// DXGIFromD3DFormat, which CreateSwapChainForHwnd/ResizeBuffers reject
// outright (DXGI_ERROR_INVALID_CALL), aborting device creation. Swap in the
// alpha variant for the swap chain itself; games never read/write backbuffer
// alpha through the X8 format anyway.
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
    ASSERT_HR(swap_chain_->Present(1, 0));
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

  LOG(INFO) << "Creating device.\n";
#ifdef DX8TO12_ENABLE_VALIDATION
  ID3D12Debug *debug_iface = nullptr;
  ASSERT_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_iface)));
  ASSERT_HR(
      debug_iface->QueryInterface(IID_PPV_ARGS(debug_interface_.GetForInit())));
  debug_iface->Release();
  debug_interface_->EnableDebugLayer();
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
  if (HRESULT hr = D3D12CreateDevice(adapter_.get(), D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(d3d12_device_.GetForInit()));
      hr != S_OK) {
    FAIL("Failed to create device: %d", hr);
    return false;
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
  LOG(INFO) << "Create: done, returning to Direct3D8::CreateDevice()\n";
  return true;
}

HRESULT Device::Init(const D3DPRESENT_PARAMETERS &presentParams) {
  fence_values_ = {};
  next_fence_ = 1;

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
    ASSERT(depth_format == D3DFMT_D16 || depth_format == D3DFMT_D32);
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
      .Scaling = DXGI_SCALING_NONE,
      .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
  };
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

Device::~Device() { WaitForFrame(next_fence_ - 1); }

HRESULT STDMETHODCALLTYPE
Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  TRACE_ENTRY(pPresentationParameters);
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
      2, pPresentationParameters->BackBufferWidth,
      pPresentationParameters->BackBufferHeight, new_format, 0));
  LOG(INFO) << "Reset: swap_chain_->ResizeBuffers() done\n";

  DXGI_SWAP_CHAIN_DESC swap_chain_desc;
  ASSERT_HR(swap_chain_->GetDesc(&swap_chain_desc));

  if (pPresentationParameters->EnableAutoDepthStencil) {
    LOG(INFO) << "Reset: creating depth-stencil texture\n";
    D3DFORMAT depth_format = pPresentationParameters->AutoDepthStencilFormat;
    if (depth_format == D3DFMT_UNKNOWN) depth_format = D3DFMT_D32;
    ASSERT(depth_format == D3DFMT_D16 || depth_format == D3DFMT_D32);
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
  return *ppTexture != nullptr;
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
      .Scaling = DXGI_SCALING_NONE,
      .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
  };
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
  LOG(TRACE) << "Transitioning " << std::hex << texture << "From "
             << texture->current_state() << " to " << state_after << "\n";

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
  buffers_to_persist_.insert(ComWrap(buffer));
}

HRESULT STDMETHODCALLTYPE Device::CopyRects(
    IDirect3DSurface8 *pSourceSurface, CONST RECT *pSourceRectsArray,
    UINT cRects, IDirect3DSurface8 *pDestinationSurface,
    CONST POINT *pDestPointsArray) {
  TRACE_ENTRY(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface,
              pDestPointsArray);

  ASSERT(static_cast<BaseSurface *>(pSourceSurface)->kind() ==
         SurfaceKind::Cpu);
  CpuSurface *source_surface = static_cast<CpuSurface *>(pSourceSurface);
  ASSERT(static_cast<BaseSurface *>(pDestinationSurface)->kind() ==
         SurfaceKind::Gpu);
  GpuSurface *dest_surface = static_cast<GpuSurface *>(pDestinationSurface);

  const D3D12_SUBRESOURCE_FOOTPRINT &source_footprint =
      source_surface->footprint().Footprint;
  const uint32_t compact_pitch =
      safe_cast<uint32_t>(source_surface->compact_pitch());
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
  render_state_.GetEnumAtIndex(State) = Value;
  switch (State) {
    case D3DRS_TEXTUREFACTOR:
    case D3DRS_ALPHAREF:
      dirty_flags_ |= DIRTY_FLAG_PS_CBUFFER;
      break;
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
  if ((Type >= D3DTSS_ADDRESSU && Type <= D3DTSS_MAXANISOTROPY) ||
      Type == D3DTSS_ADDRESSW) {
    dirty_flags_ |= DIRTY_FLAG_PS_SAMPLERS;
  }
  texture_stage_states_[Stage].GetAtIndex(static_cast<size_t>(Type)) = Value;
  return S_OK;
}

Device::StateBlock Device::CaptureCurrentState() const {
  return StateBlock{
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

void Device::ApplyState(const StateBlock &block) {
  render_state_ = block.render_state;
  texture_stage_states_ = block.texture_stage_states;
  transforms_ = block.transforms;
  material_ = block.material;
  lights_ = block.lights;
  enabled_lights_ = block.enabled_lights;
  bound_textures_ = block.bound_textures;
  bound_vertex_shader_ = block.bound_vertex_shader;
  bound_pixel_shader_ = block.bound_pixel_shader;
  bound_vs_cregs_ = block.bound_vs_cregs;
  // Force everything above to actually get re-bound/re-uploaded before the
  // next draw call, since we just changed it out from under the renderer.
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
}

HRESULT STDMETHODCALLTYPE Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                                   DWORD *pToken) {
  // Simplification: always captures the full state snapshot regardless of
  // Type (D3DSBT_ALL/D3DSBT_PIXELSTATE/D3DSBT_VERTEXSTATE) -- see the
  // StateBlock comment in device.h.
  *pToken = next_state_block_token_++;
  state_blocks_[*pToken] = CaptureCurrentState();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::BeginStateBlock() {
  if (recording_state_block_) return D3DERR_INVALIDCALL;
  recording_state_block_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::EndStateBlock(DWORD *pToken) {
  if (!recording_state_block_) return D3DERR_INVALIDCALL;
  recording_state_block_ = false;
  // Simplification: captures the full live state as of now, rather than only
  // the states actually Set() during the Begin/End window -- see the
  // StateBlock comment in device.h.
  *pToken = next_state_block_token_++;
  state_blocks_[*pToken] = CaptureCurrentState();
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
  it->second = CaptureCurrentState();
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
  if (pTexture)
    ASSERT(dynamic_cast<BaseTexture *>(pTexture)->GetSurfaceDesc(0).Pool !=
           D3DPOOL_SYSTEMMEM);
  GpuTexture *texture = dynamic_cast<GpuTexture *>(pTexture);
  bound_textures_[Stage] = InternalPtr(texture);
  dirty_flags_ |= DIRTY_FLAG_PS_TEXTURES;
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
        ASSERT(resource_desc.Format ==
               back_buffers_.at(0)->resource_desc().Format);
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
        break;
      case SurfaceKind::Backbuffer:
        ASSERT(static_cast<BackbufferSurface *>(base_surface)->index() == 0);
        texture = depth_stencil_tex_.Get();
        break;
      case SurfaceKind::Cpu:
        LOG_ERROR() << "Cannot set SYSTEMMEM surface as render target.\n";
        return D3DERR_INVALIDCALL;
    }
    // TODO: Support actually binding a separate depth stencil tex.
    ASSERT(texture == depth_stencil_tex_.Get());
    bound_depth_target_ = InternalPtr(depth_stencil_tex_.Get());
    // TODO: Update viewport.
    ASSERT(viewport_.Width == bound_depth_target_->resource_desc().Width);
    ASSERT(viewport_.Height == bound_depth_target_->resource_desc().Height);
  } else {
    bound_depth_target_.Reset();
  }
  dirty_flags_ |= DIRTY_FLAG_OM;
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

  // Now that we know our pixel shader, try to look into the PSO cache.
  PSOState pso_key{
      .rs = render_state_,
      .input_elements = vertex_shader->decl.input_elements,
      .vs = vertex_shader->blob.get(),
      .ps = pixel_shader.get(),
      .prim_type = d3d8_prim_type,
      .dsv_format = bound_depth_target_
                        ? bound_depth_target_->resource_desc().Format
                        : DXGI_FORMAT_UNKNOWN};

  // Some things don't get used here. (TODO: Move to PSOState constructor).
  pso_key.rs.texture_factor = 0;
  pso_key.rs.ambient = 0;
  pso_key.rs.diffuse_material_source = pso_key.rs.specular_material_source =
      pso_key.rs.ambient_material_source = pso_key.rs.emissive_material_source =
          D3DMCS_MATERIAL;
  // pso_key.rs.alpha_ref = 0;
  // pso_key.rs.dither_enable = 0;
  // pso_key.rs.fog_enable = 0;
  // pso_key.rs.fog_color = 0;
  // pso_key.rs.fog_table_mode = D3DFOG_NONE;
  // pso_key.rs.fog_start = 0;
  // pso_key.rs.fog_end = 0;
  // pso_key.rs.fog_density = 0;
  // pso_key.rs.range_fog_enable = 0;
  // pso_key.rs.fog_vertex_mode = D3DFOG_NONE;
  // pso_key.rs.color_vertex = 0;
  // pso_key.rs.local_viewer = FALSE;
  // pso_key.rs.normalized_normals = FALSE;

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
              .DepthBias = 0,         // TODO.
              .DepthBiasClamp = 0.f,  // TODO.
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
      .RTVFormats = {back_buffers_[0]->resource_desc().Format},
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
  D3D12_RECT rect, *rects = nullptr;
  if (pRects) {
    ASSERT(Count == 1);
    rect.left = pRects->x1;
    rect.top = pRects->y1;
    rect.right = pRects->x2;
    rect.bottom = pRects->y2;
    rects = &rect;
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
    cmd_list_->ClearRenderTargetView(render_target->rtv_handle(), color, 0,
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
    cmd_list_->ClearDepthStencilView(bound_depth_target_->dsv_handle(),
                                     clear_flags, Z,
                                     static_cast<UINT8>(Stencil), 0, rects);
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

  cmd_list_->IASetPrimitiveTopology(
      static_cast<D3D12_PRIMITIVE_TOPOLOGY>(PrimitiveType));

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

  cmd_list_->IASetVertexBuffers(0, max_index + 1, vbuffer_views.data());

  ComPtr<ID3D12PipelineState> pso = CreatePSO(PrimitiveType);
  cmd_list_->SetPipelineState(pso.get());
  // MarkResourceAsUsed(pso);
  using ::DirectX::SimpleMath::Matrix;
  const Matrix view = MatrixFromD3D(GetTransform(D3DTS_VIEW));

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
      ASSERT(cbuffer->lights[i].type != D3DLIGHT_SPOT);
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
  cmd_list_->SetGraphicsRootSignature(main_root_sig_.get());

  // Set all the necessary roots.
  cmd_list_->SetGraphicsRootConstantBufferView(0, vs_cbuffer_->GetGpuPtr());
  cmd_list_->SetGraphicsRootConstantBufferView(1, ps_cbuffer_->GetGpuPtr());
  cmd_list_->SetGraphicsRootConstantBufferView(2, lights_cbuffer_->GetGpuPtr());
  cmd_list_->SetGraphicsRootConstantBufferView(3,
                                               vs_creg_cbuffer_->GetGpuPtr());

  if (dirty_flags_ & DIRTY_FLAG_PS_TEXTURES) {
    // And all the textures.
    for (int i = 0; i < kMaxTexStages; ++i) {
      if (bound_textures_[i]) {
        const auto gpu_handle =
            srv_heap_.GetGPUHandleFor(bound_textures_[i]->srv_handle());
        cmd_list_->SetGraphicsRootDescriptorTable(textures_start_bindslot_ + i,
                                                  gpu_handle);
        MarkResourceAsUsed(bound_textures_[i]);
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
  // Overwrite whatever vertex buffer the prepare set.
  cmd_list_->IASetVertexBuffers(0, 1, &vbuffer_view);
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
  // Overwrite whatever vertex/index buffer the prepare set.
  cmd_list_->IASetVertexBuffers(0, 1, &vbuffer_view);
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
  ASSERT(!(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED));
  // Fine-grained checkpoints while chasing a crash that lands somewhere in
  // this function with no other diagnosable cause -- see ROADMAP.md. Remove
  // once that's resolved; this is deliberately noisy per-call, not meant to
  // stay long-term.
  LOG(INFO) << "SubmitAndWait(should_present=" << should_present << ")\n";

  // Transition back buffer to present.
  if (should_present) {
    TransitionTexture(back_buffers_[current_back_buffer_].get(), 0,
                      D3D12_RESOURCE_STATE_PRESENT);
  }

  // Persist any dynamic buffers.
  for (auto buffer : buffers_to_persist_) {
    buffer->PersistDynamicChanges();
  }
  buffers_to_persist_.clear();

  // Close the command list, then execute it.
  LOG(INFO) << "SubmitAndWait: cmd_list_->Close()\n";
  ASSERT_HR(cmd_list_->Close());
  dirty_flags_ |= DIRTY_FLAG_CMD_LIST_CLOSED;
  ID3D12CommandList *cmd_list = cmd_list_.Get();
  LOG(INFO) << "SubmitAndWait: cmd_queue_->ExecuteCommandLists()\n";
  cmd_queue_->ExecuteCommandLists(1, &cmd_list);
  // Present!
  if (should_present) {
    LOG(INFO) << "SubmitAndWait: swap_chain_->Present()\n";
    ASSERT_HR(swap_chain_->Present(1, 0));
  }

  // Grab a new fence value, set it at the end of the command queue execution.
  fence_values_.at(current_back_buffer_) = next_fence_++;
  LOG(INFO) << "SubmitAndWait: cmd_queue_->Signal()\n";
  ASSERT_HR(cmd_queue_->Signal(cmd_list_done_fence_.get(),
                               fence_values_[current_back_buffer_]));

  // Update our back buffer index.
  LOG(INFO) << "SubmitAndWait: swap_chain_->GetCurrentBackBufferIndex()\n";
  current_back_buffer_ = swap_chain_->GetCurrentBackBufferIndex();

  // Wait for it.
  LOG(INFO) << "SubmitAndWait: WaitForFrame()\n";
  WaitForFrame(fence_values_[current_back_buffer_]);

  // Reset the command list for the next frame.
  LOG(INFO) << "SubmitAndWait: resetting allocator/cmd list\n";
  ASSERT_HR(cmd_allocators_[current_back_buffer_]->Reset());
  ASSERT_HR(
      cmd_list_->Reset(cmd_allocators_[current_back_buffer_].get(), nullptr));
  dirty_flags_ ^= DIRTY_FLAG_CMD_LIST_CLOSED;
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
  LOG(INFO) << "SubmitAndWait: done\n";
}

void Device::WaitForFrame(uint64_t frame_number) {
  ASSERT(frame_number <= next_fence_);
  LOG(INFO) << "WaitForFrame(" << frame_number << ")\n";

  if (cmd_list_done_fence_->GetCompletedValue() < frame_number) {
    // Is this a frame that we're currently building?
    if (frame_number + 1 == next_fence_ &&
        !(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED)) {
      // SubmitAndWait will call us again to wait for the frame, but at that
      // point fence_values_[current_back_buffer_] will have incremented.
      LOG(INFO) << "WaitForFrame: recursing into SubmitAndWait(false)\n";
      SubmitAndWait(false);
    } else {
      LOG(INFO) << "WaitForFrame: waiting on fence event ("
                << cmd_list_done_fence_->GetCompletedValue() << " < "
                << frame_number << ")\n";
      LOG(TRACE) << "Waiting for fence " << frame_number << ".\n";
      ASSERT_HR(cmd_list_done_fence_->SetEventOnCompletion(
          frame_number, cmd_list_done_event_handle_));
      WaitForSingleObjectEx(cmd_list_done_event_handle_, 60 * 1000, FALSE);
      LOG(INFO) << "WaitForFrame: fence event signaled/timed out\n";
    }
  }

  // Free any frame resources.
  FreeFrameResources(frame_number);
  LOG(INFO) << "WaitForFrame: done\n";
}

void Device::FreeFrameResources(uint64_t frame_number) {
  for (size_t i = 0; i < frame_resources_to_free_.size(); ++i) {
    if (fence_values_[i] <= frame_number) {
      frame_resources_to_free_[i].clear();
    }
  }

  dynamic_ring_buffer_->HasCompletedFrame(frame_number);
  dynamic_ring_buffer_->SetCurrentFrame(CurrentFrame());
}

uint64_t Device::CurrentFrame() const { return next_fence_; }

}  // namespace Dx8to12