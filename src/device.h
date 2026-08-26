#pragma once

#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "d3d8.h"
#include "device_limits.h"
#include "pool_heap.h"
#include "render_state.h"
#include "shader_parser.h"
#include "util.h"
#include "utils/dx_utils.h"
#include "vertex_shader.h"

interface IDXGIFactory2;
interface IDXGIAdapter;
interface IDXGISwapChain3;
interface IDXGIOutput;
interface ID3D12Device;
interface ID3D12CommandQueue;
interface ID3D12CommandList;
interface ID3D12CommandAllocator;
interface ID3D12RootSignature;

namespace D3D12MA {
class Allocator;
}

namespace Dx8to12 {
class Buffer;
class BaseSurface;
class DynamicRingBuffer;
class GpuTexture;

class Device : public IDirect3DDevice8, RefCounted {
 public:
  Device(IDirect3D8 *direct3d8);
  virtual ~Device();

  static D3DCAPS8 GetDefaultCaps(UINT adapter_index);
  bool Create(HWND window, ComPtr<IDXGIFactory2> factory,
              ComPtr<IDXGIAdapter> adapter, int adapter_index,
              const D3DPRESENT_PARAMETERS &presentParams);

  ID3D12Device *device() const { return d3d12_device_.get(); }
  ID3D12GraphicsCommandList *cmd_list() const { return cmd_list_.get(); }
  // False between the Close() and Reset() of a frame's command list, during
  // which nothing may be recorded into it.
  bool IsCommandListOpen() const {
    return !(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED);
  }
  ID3D12CommandQueue *cmd_queue() const { return cmd_queue_.get(); }
  HWND window() const { return window_; }
  UINT sync_interval() const { return sync_interval_; }
  bool tearing_supported() const { return tearing_supported_; }
  DXGI_FORMAT backbuffer_format() const;
  // Bumped once per Device::Reset() (window resize, fullscreen toggle,
  // format change): back buffer resources, format, and dimensions are all
  // recreated by Reset, so a mod holding onto its own PSO/RTV-format-derived
  // state should compare this against the value it last saw and rebuild
  // when it changes.
  uint64_t swap_chain_generation() const { return swap_chain_generation_; }
  // Duration of the most recently presented frame, in milliseconds, measured
  // between consecutive Present calls. Exposed to mods via
  // Dx8to12_GetLastFrameMs -- see MODDING.md.
  double last_frame_ms() const { return last_frame_ms_; }
  DescriptorPoolHeap &srv_heap() { return srv_heap_; }
  DescriptorPoolHeap *rtv_heap() { return &rtv_heap_; }
  DescriptorPoolHeap *dsv_heap() { return &dsv_heap_; }
  DynamicRingBuffer *dynamic_ring_buffer() {
    return dynamic_ring_buffer_.get();
  }
  // TODO: Actually put this in GPU mem.
  DynamicRingBuffer *dynamic_gpu_ring_buffer() {
    return dynamic_ring_buffer_.get();
  }

  // Mod-API render injection (dx8to12_api.cpp / MODDING.md): a registered
  // callback is invoked once per frame, right before the backbuffer
  // transitions to PRESENT, with the still-open command list -- this
  // frame's real render target (the backbuffer) is still bound from
  // BeginScene, so a callback can just record its own draw commands
  // straight into it and they land on top of the already-rendered frame.
  // Lets a companion mod (e.g. an ImGui-based overlay/trainer) render
  // natively through the real D3D12 device/command queue/command list this
  // API also exposes, rather than needing a D3D9 (or other) compatibility
  // shim underneath -- see the GrinchTrainerVC.asi investigation in
  // ROADMAP.md for why that alternative is a much larger undertaking.
  using ModRenderCallback = void(__cdecl *)(void *command_list);
  void RegisterModRenderCallback(ModRenderCallback callback);
  void UnregisterModRenderCallback(ModRenderCallback callback);

  uint64_t CurrentFrame() const;
  // True while the F9 UI dump is running -- see PollUiDumpHotkey.
  bool ui_dump_enabled() const { return ui_dump_enabled_; }
  void CopyBuffer(Buffer *dest, int64_t dest_offset, ID3D12Resource *src,
                  int64_t src_offset, int64_t num_bytes);
  void CopyBufferToTexture(GpuTexture *dest, uint32_t dest_subresource,
                           ID3D12Resource *src,
                           D3D12_PLACED_SUBRESOURCE_FOOTPRINT src_footprint,
                           uint32_t dest_x = 0, uint32_t dest_y = 0);
  void TransitionTexture(GpuTexture *texture, uint32_t subresource,
                         D3D12_RESOURCE_STATES state_after);
  // Buffers (vertex/index) rely on D3D12's implicit state promotion from
  // COMMON for read usages, but that promotion is tracked/validated per
  // command list and does NOT cover write usages like being a
  // CopyBufferRegion destination (see DynamicBuffer::PersistDynamicChanges).
  // Track each Buffer's state explicitly (mirroring TransitionTexture) so a
  // buffer written-to-after-being-drawn-from in the same command list gets a
  // correct StateBefore instead of tripping the D3D12 debug layer.
  void TransitionBuffer(Buffer *buffer, D3D12_RESOURCE_STATES state_after);
#ifdef DX8TO12_ENABLE_VALIDATION
  // See the definition (device.cpp, right after TransitionTexture/
  // TransitionBuffer) for what this measures and why.
  void LogBarrierStats(bool is_texture);
#endif

  // Marks a dynamic buffer that needs to be persisted at the end of the frame.
  void MarkBufferForPersist(Buffer *buffer);

  template <typename T>
  void MarkResourceAsUsed(InternalPtr<T> resource) {
    // See RefCounted::last_marked_generation_ for the full reasoning. This
    // is purely a same-slot-since-last-clear dedup -- independent of
    // frame/fence numbering -- so it can't be fooled by mid-frame flushes or
    // anything else that advances next_fence_ without clearing this slot.
    RefCounted *ptr = resource.Get();
    if (ptr->last_marked_generation_[current_back_buffer_] ==
        slot_generation_[current_back_buffer_]) {
      return;
    }
    ptr->last_marked_generation_[current_back_buffer_] =
        slot_generation_[current_back_buffer_];
    frame_resources_to_free_.at(current_back_buffer_)
        .push_back(InternalPtr<RefCounted>(ptr));
  }

#ifdef DX8TO12_USE_ALLOCATOR
  D3D12MA::Allocator *allocator() { return allocator_.get(); }
  ComPtr<D3D12MA::Allocator> allocator_;
#endif

  // IDirect3DDevice8 implementation.
 public:
#undef PURE
#define PURE VIRT_NOT_IMPLEMENTED

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override;
  ULONG STDMETHODCALLTYPE AddRef(THIS) override { return RefCounted::AddRef(); }
  ULONG STDMETHODCALLTYPE Release(THIS) override {
    return RefCounted::Release();
  }

  /*** IDirect3DDevice8 methods ***/
  virtual HRESULT STDMETHODCALLTYPE TestCooperativeLevel(THIS) override;
  virtual UINT STDMETHODCALLTYPE GetAvailableTextureMem(THIS) override;
  virtual HRESULT STDMETHODCALLTYPE
  ResourceManagerDiscardBytes(DWORD Bytes) override {
    TRACE_ENTRY(Bytes);
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8 **ppD3D8) override {
    *ppD3D8 = direct3d8_.get();
    direct3d8_->AddRef();
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8 *pCaps) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetDisplayMode(D3DDISPLAYMODE *pMode) override {
    return direct3d8_->GetAdapterDisplayMode(
        static_cast<UINT>(adapter_index_), pMode);
  }
  virtual HRESULT STDMETHODCALLTYPE
  GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override;
  virtual HRESULT STDMETHODCALLTYPE SetCursorProperties(
      UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) override;
  virtual void STDMETHODCALLTYPE SetCursorPosition(int X, int Y,
                                                   DWORD Flags) override;
  virtual BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override;
  virtual HRESULT STDMETHODCALLTYPE
  CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters,
                            IDirect3DSwapChain8 **pSwapChain) override;
  virtual HRESULT STDMETHODCALLTYPE
  Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
  virtual HRESULT STDMETHODCALLTYPE
  Present(CONST RECT *pSourceRect, CONST RECT *pDestRect,
          HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type,
                IDirect3DSurface8 **ppBackBuffer) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override {
    // No access to real scanline/vblank timing through this backend; report
    // a plausible "not in vblank, top of frame" status rather than aborting.
    *pRasterStatus = D3DRASTER_STATUS{.InVBlank = FALSE, .ScanLine = 0};
    return S_OK;
  }
  virtual void STDMETHODCALLTYPE SetGammaRamp(
      DWORD Flags, CONST D3DGAMMARAMP *pRamp) override {
    // Bookkeeping only -- does not touch the actual display gamma.
    (void)Flags;
    gamma_ramp_ = *pRamp;
  }
  virtual void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP *pRamp) override {
    *pRamp = gamma_ramp_;
  }
  virtual HRESULT STDMETHODCALLTYPE CreateTexture(
      UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
      D3DPOOL Pool, IDirect3DTexture8 **ppTexture) override;
  virtual HRESULT STDMETHODCALLTYPE
  CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels,
                      DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                      IDirect3DVolumeTexture8 **ppVolumeTexture) PURE;
  virtual HRESULT STDMETHODCALLTYPE CreateCubeTexture(
      UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DCubeTexture8 **ppCubeTexture) override;
  COM_DECLSPEC_NOTHROW HRESULT STDMETHODCALLTYPE
  CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                     IDirect3DVertexBuffer8 **ppVertexBuffer) override;
  virtual HRESULT STDMETHODCALLTYPE
  CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                    IDirect3DIndexBuffer8 **ppIndexBuffer) override;
  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                     D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable,
                     IDirect3DSurface8 **ppSurface) override;
  virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(
      UINT Width, UINT Height, D3DFORMAT Format,
      D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface) override;
  virtual HRESULT STDMETHODCALLTYPE
  CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format,
                     IDirect3DSurface8 **ppSurface) override;
  virtual HRESULT STDMETHODCALLTYPE
  CopyRects(IDirect3DSurface8 *pSourceSurface, CONST RECT *pSourceRectsArray,
            UINT cRects, IDirect3DSurface8 *pDestinationSurface,
            CONST POINT *pDestPointsArray) override;
  virtual HRESULT STDMETHODCALLTYPE
  UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture,
                IDirect3DBaseTexture8 *pDestinationTexture) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetFrontBuffer(IDirect3DSurface8 *pDestSurface) PURE;
  HRESULT STDMETHODCALLTYPE
  SetRenderTarget(IDirect3DSurface8 *pRenderTarget,
                  IDirect3DSurface8 *pNewZStencil) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetRenderTarget(IDirect3DSurface8 **ppRenderTarget) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface) override;
  virtual HRESULT STDMETHODCALLTYPE BeginScene(THIS) override;
  virtual HRESULT STDMETHODCALLTYPE EndScene(THIS) override;
  virtual HRESULT STDMETHODCALLTYPE Clear(DWORD Count, CONST D3DRECT *pRects,
                                          DWORD Flags, D3DCOLOR Color, float Z,
                                          DWORD Stencil) override;
  virtual HRESULT STDMETHODCALLTYPE
  SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override;
  virtual HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State,
                                                 D3DMATRIX *pMatrix) override;
  virtual HRESULT STDMETHODCALLTYPE
  MultiplyTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX *) override;
  virtual HRESULT STDMETHODCALLTYPE
  SetViewport(CONST D3DVIEWPORT8 *pViewport) override;
  virtual HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8 *pViewport) override;
  virtual HRESULT STDMETHODCALLTYPE
  SetMaterial(CONST D3DMATERIAL8 *pMaterial) override;
  virtual HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8 *pMaterial) override;
  virtual HRESULT STDMETHODCALLTYPE SetLight(DWORD Index,
                                             CONST D3DLIGHT8 *) override;
  virtual HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT8 *) override;
  virtual HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index,
                                                BOOL Enable) override;
  virtual HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index,
                                                   BOOL *pEnable) override;
  virtual HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index,
                                                 CONST float *pPlane) override;
  virtual HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index,
                                                 float *pPlane) override;
  virtual HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State,
                                                   DWORD Value) override;
  virtual HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,
                                                   DWORD *pValue) override;
  virtual HRESULT STDMETHODCALLTYPE BeginStateBlock(THIS) override;
  virtual HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD *pToken) override;
  virtual HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD Token) override;
  virtual HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD Token) override;
  virtual HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD Token) override;
  virtual HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                                     DWORD *pToken) override;
  virtual HRESULT STDMETHODCALLTYPE
  SetClipStatus(CONST D3DCLIPSTATUS8 *pClipStatus) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetClipStatus(D3DCLIPSTATUS8 *pClipStatus) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetTexture(DWORD Stage, IDirect3DBaseTexture8 **ppTexture) override;
  virtual HRESULT STDMETHODCALLTYPE
  SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture) override;
  virtual HRESULT STDMETHODCALLTYPE GetTextureStageState(
      DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
  virtual HRESULT STDMETHODCALLTYPE SetTextureStageState(
      DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
  virtual HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetInfo(DWORD DevInfoID, void *pDevInfoStruct,
          DWORD DevInfoStructSize) VIRT_NOT_IMPLEMENTED;
  virtual HRESULT STDMETHODCALLTYPE
  SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY *pEntries) override {
    return D3DERR_NOTAVAILABLE;
  }
  virtual HRESULT STDMETHODCALLTYPE
  GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override {
    return D3DERR_NOTAVAILABLE;
  }
  virtual HRESULT STDMETHODCALLTYPE
  SetCurrentTexturePalette(UINT PaletteNumber) override {
    return D3DERR_NOTAVAILABLE;
  }
  virtual HRESULT STDMETHODCALLTYPE
  GetCurrentTexturePalette(UINT *PaletteNumber) override {
    return D3DERR_NOTAVAILABLE;
  }
  virtual HRESULT STDMETHODCALLTYPE
  DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                UINT PrimitiveCount) override;
  virtual HRESULT STDMETHODCALLTYPE
  DrawIndexedPrimitive(D3DPRIMITIVETYPE, UINT minIndex, UINT NumVertices,
                       UINT startIndex, UINT primCount) override;
  virtual HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
      D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
      CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
      D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
      UINT NumVertexIndices, UINT PrimitiveCount, CONST void *pIndexData,
      D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData,
      UINT VertexStreamZeroStride) override;
  virtual HRESULT STDMETHODCALLTYPE
  ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                  IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) PURE;
  virtual HRESULT STDMETHODCALLTYPE
  CreateVertexShader(CONST DWORD *pDeclaration, CONST DWORD *pFunction,
                     DWORD *pHandle, DWORD Usage) override;
  virtual HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD Handle) override;
  virtual HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD *pHandle) override;
  HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD Handle) override;
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(
      DWORD Register, CONST void *pConstantData, DWORD ConstantCount) override;
  virtual HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(
      DWORD Register, void *pConstantData, DWORD ConstantCount) override;
  virtual HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(
      DWORD Handle, void *pData, DWORD *pSizeOfData) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
  virtual HRESULT STDMETHODCALLTYPE
  SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData,
                  UINT Stride) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData,
                  UINT *pStride) override;
  virtual HRESULT STDMETHODCALLTYPE
  SetIndices(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetIndices(IDirect3DIndexBuffer8 **ppIndexData,
             UINT *pBaseVertexIndex) override;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD *pFunction,
                                              DWORD *pHandle) override;
  HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD Handle) override;
  virtual HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD *pHandle) override;
  HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD Handle) override;
  virtual HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(
      DWORD Register, CONST void *pConstantData, DWORD ConstantCount) override;
  virtual HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(
      DWORD Register, void *pConstantData, DWORD ConstantCount) override;
  virtual HRESULT STDMETHODCALLTYPE
  GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
  virtual HRESULT STDMETHODCALLTYPE
  DrawRectPatch(UINT Handle, CONST float *pNumSegs,
                CONST D3DRECTPATCH_INFO *pRectPatchInfo) PURE;
  virtual HRESULT STDMETHODCALLTYPE
  DrawTriPatch(UINT Handle, CONST float *pNumSegs,
               CONST D3DTRIPATCH_INFO *pTriPatchInfo) PURE;
  virtual HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) PURE;

  // Is called only on device reset. Does not advance current frame. Does not
  // adjust persist any dynamic buffers.
  void SubmitAndWait(bool should_present);
  void WaitForFrame(uint64_t frame_number);

#undef PURE
#define PURE = 0

 private:
  static constexpr DWORD kFirstShaderHandle =
      0x10000;  // Assume worst-case 0xFFFF FVF flag usage.

  static constexpr bool debug_lockstep_ = true;

  // Called both during normal construction and after Reset.
  HRESULT Init(const D3DPRESENT_PARAMETERS &presentParams);
  void InitRootSignatures();

  ComPtr<ID3D12PipelineState> CreatePSO(D3DPRIMITIVETYPE d3d8_prim_type);
  HRESULT PrepareDrawCall(D3DPRIMITIVETYPE PrimitiveType, int start_vertex,
                          int num_vertices);

  // Empties buffers_to_persist_, releases any frame resources, advances current
  // frame.
  void FreeFrameResources(uint64_t frame_number);

  D3DMATRIX GetTransform(D3DTRANSFORMSTATETYPE state);

  // Full snapshot of the fixed-function/shader-binding state, used as the
  // "before" and "after" points that a Begin/EndStateBlock recording is
  // diffed against -- see StateBlock below.
  struct StateSnapshot {
    RenderState render_state;
    std::array<TextureStageState, kMaxTexStages> texture_stage_states;
    std::unordered_map<D3DTRANSFORMSTATETYPE, D3DMATRIX> transforms;
    D3DMATERIAL8 material;
    std::unordered_map<DWORD, D3DLIGHT8> lights;
    std::unordered_set<DWORD> enabled_lights;
    std::array<InternalPtr<GpuTexture>, kMaxTexStages> bound_textures;
    DWORD bound_vertex_shader = 0;
    DWORD bound_pixel_shader = 0;
    std::vector<DirectX::SimpleMath::Vector4> bound_vs_cregs;
  };
  StateSnapshot CaptureCurrentState() const;

  // A sparse set of state changes, as recorded by a D3D8 state block. Real
  // D3D8 state blocks only capture/restore the specific states that were
  // actually Set() during recording (Begin/EndStateBlock) -- applying one
  // must not clobber unrelated state that a game changed in between
  // recording and applying. Building a full StateSnapshot instead (the
  // previous implementation) restored *everything* back to how it was at
  // recording time, which could stomp on legitimate state changes made after
  // the block was recorded -- e.g. an ApplyStateBlock() call silently
  // resetting D3DRS_ALPHABLENDENABLE to a stale value.
  struct StateBlock {
    std::unordered_map<D3DRENDERSTATETYPE, DWORD> render_state;
    std::array<std::unordered_map<D3DTEXTURESTAGESTATETYPE, DWORD>,
               kMaxTexStages>
        texture_stage_states;
    std::unordered_map<D3DTRANSFORMSTATETYPE, D3DMATRIX> transforms;
    std::optional<D3DMATERIAL8> material;
    std::unordered_map<DWORD, D3DLIGHT8> lights;
    // Only present if any light's enabled/disabled status changed.
    std::optional<std::unordered_set<DWORD>> enabled_lights;
    std::array<std::optional<InternalPtr<GpuTexture>>, kMaxTexStages>
        bound_textures;
    std::optional<DWORD> bound_vertex_shader;
    std::optional<DWORD> bound_pixel_shader;
    std::unordered_map<UINT, DirectX::SimpleMath::Vector4> bound_vs_cregs;
  };
  // Every state, unconditionally marked as "touched" -- used for
  // CreateStateBlock(), which (unlike Begin/EndStateBlock) always snapshots
  // the full live state regardless of D3DSTATEBLOCKTYPE partitioning.
  StateBlock CaptureFullStateBlock() const;
  // Only the states that differ between `before` and the current live state
  // -- used for Begin/EndStateBlock() recordings.
  StateBlock CaptureStateBlockDelta(const StateSnapshot &before) const;
  void ApplyState(const StateBlock &block);

  std::unordered_map<DWORD, StateBlock> state_blocks_;
  DWORD next_state_block_token_ = 1;
  bool recording_state_block_ = false;
  StateSnapshot state_block_recording_start_;

  // See RegisterModRenderCallback.
  std::vector<ModRenderCallback> mod_render_callbacks_;

  uint64_t swap_chain_generation_ = 1;

  // Root-argument state that survives across draws within one command list.
  // Both are invalidated wherever cmd_list_ is Reset (a fresh command list
  // has no root signature and no root arguments bound) -- see the
  // root_sig_bound_ block in PrepareDrawCall for why binding the root
  // signature once per command list rather than once per draw matters.
  // F9 toggles a detailed dump of 2D UI draws to log.txt -- see
  // PollUiDumpHotkey. Lets a glitch that never survives a graphics-debugger
  // capture be recorded at the moment it's actually on screen.
  void PollUiDumpHotkey();
  bool ui_dump_enabled_ = false;
  bool ui_dump_key_was_down_ = false;
  int ui_dump_frames_left_ = 0;

  bool root_sig_bound_ = false;
  std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4> last_root_cbvs_ = {};

  // Result of the last CreatePSO lookup, reused while DIRTY_FLAG_PSO stays
  // clear. The primitive type is part of the PSO key but is a per-draw
  // argument rather than device state, so it's tracked separately here
  // instead of via the dirty flag.
  // Last vertex buffer view set on the command list, so an unchanged binding
  // can skip the IASetVertexBuffers call. Invalidated (count reset to 0)
  // wherever cmd_list_ is Reset, since that drops all IA state.
  std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexStreams> last_vbuffer_views_ =
      {};
  size_t last_vbuffer_view_count_ = 0;

  ComPtr<ID3D12PipelineState> last_pso_;
  D3DPRIMITIVETYPE last_pso_prim_type_ = D3DPT_FORCE_DWORD;
  // Last PSO actually bound on the command list (null after a command list
  // reset, which drops all pipeline state).
  ID3D12PipelineState *last_set_pso_ = nullptr;
  // D3DRS_STENCILREF, like the viewport/scissor, is a dynamic per-draw value
  // in D3D12 (OMSetStencilRef) rather than something baked into the PSO --
  // see pso_key.rs.stencil_ref being zeroed in CreatePSO's key. -1 is not a
  // legal 8-bit stencil ref, so it forces the first draw after any command
  // list reset to actually issue the call instead of skipping it as
  // redundant against a stale value from a previous, already-closed list.
  int last_stencil_ref_ = -1;
  // Per-stage "does this root descriptor table need rebinding" bits,
  // mirroring DIRTY_FLAG_PS_TEXTURES/PS_SAMPLERS but at per-stage
  // granularity -- those flags only say *some* stage's binding changed
  // (SetTexture/SetTextureStageState don't know which slot a game will
  // touch next), so without this every draw after any single SetTexture call
  // rebound all kMaxTexStages (8) root descriptor tables -- one
  // SetGraphicsRootDescriptorTable call each -- even for the 7 stages whose
  // binding didn't change.
  //
  // Deliberately NOT implemented as "does bound_textures_[i] differ from
  // whatever we bound last time" (comparing cached GpuTexture* or descriptor
  // handles): both are heap-allocated identities that get freed and reused
  // by an unrelated later object (GpuTexture via RefCounted's `delete this`,
  // descriptor slots via DescriptorPoolHeap's free list -- the exact hazard
  // the pending_free_ generation-based reclaim in pool_heap.cpp exists to
  // manage on the *pool's* side, but does nothing to protect a cache held
  // here). A texture destroyed and replaced by a new, unrelated one that
  // happens to land at the same address/slot would compare equal to a stale
  // cached value and wrongly skip the rebind, leaving the GPU sampling
  // whatever the root table already pointed at. Tracking dirtiness as a bit
  // set at the moment of mutation (SetTexture/SetTextureStageState) instead
  // of inferred later by comparing identities sidesteps that class of bug
  // entirely -- there is no stale value to alias against.
  uint32_t dirty_texture_stage_mask_ = 0xFF;
  uint32_t dirty_sampler_stage_mask_ = 0xFF;
  // DIAGNOSTIC: the contiguous has-texture bitmask (matching CreatePSO's own
  // stage_has_texture computation) at the moment last_pso_ was actually
  // built. Compared every draw against the *current* live mask to catch a
  // cached PSO/pixel-shader being reused for a draw whose texture-stage
  // shape has since changed without DIRTY_FLAG_PSO having been raised for
  // it -- the failure mode that would explain a draw silently getting an
  // untextured pixel shader when a texture is actually bound.
  uint32_t last_pso_texture_mask_ = 0;

  int ref_count_;

  ComPtr<IDirect3D8> direct3d8_;  // Have to hold on for GetDirect3D.
  HWND window_ = nullptr;
  ComPtr<IDXGIFactory2> dxgi_factory_;
  ComPtr<ID3D12Device> d3d12_device_;
  ComPtr<IDXGISwapChain3> swap_chain_;
  ComPtr<IDXGIAdapter> adapter_;
  ComPtr<IDXGIOutput> adapter_output_;
  int adapter_index_;

  ComPtr<ID3D12CommandQueue> cmd_queue_;
  std::array<ComPtr<ID3D12CommandAllocator>, kNumBackBuffers> cmd_allocators_;
  ComPtr<ID3D12GraphicsCommandList>
      cmd_list_;  // Main list used for everything.

  ComPtr<ID3D12Fence> cmd_list_done_fence_;
  HANDLE cmd_list_done_event_handle_ = nullptr;

  int current_back_buffer_ = 0;
  std::array<uint64_t, kNumBackBuffers> fence_values_ = {};
  uint64_t next_fence_ = 1;

  // DIAGNOSTIC: counts every Draw*Primitive* entry (indexed/non-indexed,
  // UP/non-UP), incremented before any early-return, so it reflects calls
  // the game actually made regardless of whether the shim went on to submit
  // anything. Reset and logged in SubmitAndWait so it can be compared
  // against a RenderDoc capture's own draw count for the same frame -- if
  // they match, a draw call missing from a capture was never issued by the
  // game in the first place (not dropped by the shim); if the shim's count
  // is higher, something in a Draw* early-return path is eating a real call.
  uint64_t draw_calls_this_frame_ = 0;

  // TEMP DIAGNOSTIC: measuring how much of each frame's CPU time is spent
  // blocked in WaitForSingleObjectEx (waiting on the GPU fence) vs. actually
  // building the command list -- chasing a report of low GPU+CPU utilization
  // (both under 50%) alongside a large FPS regression vs. the real d3d8.dll.
  double last_frame_ms_ = 0.0;
  int64_t perf_last_frame_ticks_ = 0;
  int64_t perf_wait_ticks_this_frame_ = 0;
  int64_t perf_wait_ticks_accum_ = 0;
  int64_t perf_frame_ticks_accum_ = 0;
  int perf_frame_sample_count_ = 0;

  ComPtr<ID3D12Debug5> debug_interface_;
  ComPtr<ID3D12InfoQueue1> info_queue_;
  DWORD info_queue_cookie_;

  D3DCAPS8 caps_;

  std::vector<ComPtr<GpuTexture>> back_buffers_;

  ComPtr<GpuTexture> depth_stencil_tex_;

  // GetRenderTarget/GetDepthStencilSurface must return the SAME surface
  // object (with an incremented refcount) across repeated calls as long as
  // the underlying texture hasn't changed -- real D3D8 preserves COM
  // identity here, and games (e.g. RenderWare-based titles) rely on it,
  // sometimes Release()ing a previously-fetched pointer and expecting a
  // fresh GetRenderTarget() call to still be valid/consistent with other
  // outstanding references. Returning a brand-new independently-refcounted
  // wrapper on every call breaks that assumption and can free the surface
  // out from under a reference the game still expects to be live.
  //
  // Keyed by bound_render_target_generation_ (bumped every SetRenderTarget
  // call that actually reassigns bound_render_target_), not by the backing
  // texture's raw pointer. A custom render target set via SetRenderTarget
  // is *not* kept alive the way a texture actively bound for drawing is --
  // MarkResourceAsUsed only runs at draw time (BeginScene/PrepareDrawCall),
  // so a target that's set, queried once via GetRenderTarget, then swapped
  // away before ever being drawn to can have its C++ wrapper destroyed
  // (RefCounted::Release -> delete this) the moment the app also drops its
  // own reference. A later, unrelated GpuTexture allocated at that same
  // freed heap address would then compare equal to the stale cached
  // pointer, handing back a wrapper around an already-destroyed resource --
  // the same identity-reuse (ABA) hazard already found and fixed tonight
  // for the texture-descriptor rebind cache (see dirty_texture_stage_mask_).
  // A generation counter bumped at the point of mutation, rather than
  // compared by identity after the fact, has no stale value to alias
  // against. GetDepthStencilSurface/GetBackBuffer don't need the same
  // treatment: they key off depth_stencil_tex_/back_buffers_[0], both of
  // which are held alive by this Device for the whole session (only
  // replaced, never destroyed-and-maybe-reused, at a Reset() boundary that
  // already invalidates their caches unconditionally).
  ComPtr<BaseSurface> cached_render_target_surface_;
  uint64_t cached_render_target_surface_key_ = 0;
  uint64_t bound_render_target_generation_ = 1;
  ComPtr<BaseSurface> cached_depth_stencil_surface_;
  void *cached_depth_stencil_surface_key_ = nullptr;
  // Same COM-identity requirement as the two caches above -- GetBackBuffer
  // previously allocated a brand new BackbufferSurface wrapper on every
  // call, so two calls at the same device state hand the app two distinct
  // interface pointers to what's supposed to be the *same* object; anything
  // that compares them for identity, or holds one past the other's Release
  // expecting shared state, breaks. Keyed on back_buffers_[0].get() the same
  // way GetRenderTarget's fallback case is, so a Reset() (which re-acquires
  // the back buffers, changing the pointer) correctly invalidates this.
  ComPtr<BaseSurface> cached_backbuffer_surface_;
  void *cached_backbuffer_surface_key_ = nullptr;

  // Shader resources/handles.
  // TODO: Don't count shader references, instead make PSO own shaders and count
  // PSO references instead.
  std::unordered_map<DWORD, InternalPtr<VertexShader>> vertex_shaders_;
  std::unordered_map<DWORD, InternalPtr<PixelShader>> pixel_shaders_;
  DWORD next_shader_handle_ = kFirstShaderHandle;

  // Render state.

  // Explicitly-set render targets.
  InternalPtr<GpuTexture> bound_render_target_;
  InternalPtr<GpuTexture> bound_depth_target_;

  // Viewport.
  D3D12_VIEWPORT viewport_ = {.MaxDepth = 1.f};
  // Present()'s SyncInterval, derived from the app's requested
  // D3DPRESENT_PARAMETERS::FullScreen_PresentationInterval at Init()/
  // Reset() time. Defaults to 1 (vsync on), matching real D3D8's default.
  UINT sync_interval_ = 1;
  // Whether the adapter/swap chain support tearing (DXGI_FEATURE_PRESENT_
  // ALLOW_TEARING), checked once at Init() time. Required to actually
  // Present() with SyncInterval=0 (D3DPRESENT_INTERVAL_IMMEDIATE) on a
  // DXGI_SWAP_EFFECT_FLIP_DISCARD swap chain -- without it and the matching
  // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING/DXGI_PRESENT_ALLOW_TEARING flags,
  // Present(0, ...) can fail outright rather than just presenting with
  // vsync anyway.
  bool tearing_supported_ = false;
  // Material.
  D3DMATERIAL8 material_ = {};
  // Light definitions.
  std::unordered_map<DWORD, D3DLIGHT8> lights_;
  // Which lights are enabled.
  std::unordered_set<DWORD> enabled_lights_;  // TODO: Use bitfield.
  // Transforms.
  // TODO: Don't store in unordered map.. Don't be lazy..
  std::unordered_map<D3DTRANSFORMSTATETYPE, D3DMATRIX> transforms_;
  // Bound textures.
  std::array<InternalPtr<GpuTexture>, kMaxTexStages> bound_textures_;
  // Bound vertex streams.
  std::array<InternalPtr<Buffer>, kMaxVertexStreams>
      bound_vertex_streams_;  // Have to store as vertex buffer because of
                              // ambiguous cast.
  std::array<UINT, kMaxVertexStreams> bound_vertex_stream_strides_ = {};
  InternalPtr<Buffer> bound_index_buffer_;
  uint32_t bound_base_vertex_ = 0;

  // Bound vertex shader.
  DWORD bound_vertex_shader_ = 0;
  DWORD bound_pixel_shader_ = 0;

  // Bound vertex/pixel shader constants.
  std::vector<DirectX::SimpleMath::Vector4> bound_vs_cregs_;
  // Bookkeeping only: unlike bound_vs_cregs_, this is not currently wired
  // into the pixel shader's constant buffer (see ps_creg_cbuffer_ below) --
  // ps.1.x shaders reading these registers will not see the app's values.
  std::array<DirectX::SimpleMath::Vector4, kNumPsConstRegs> bound_ps_cregs_ =
      {};

  // Bookkeeping only, no GPU-side clip-plane implementation.
  std::array<std::array<float, 4>, kMaxUserClipPlanes> clip_planes_ = {};
  D3DCLIPSTATUS8 clip_status_ = {};

  bool cursor_visible_ = false;
  D3DGAMMARAMP gamma_ramp_ = {};  // Populated with an identity ramp in Device().

  RenderState render_state_;
  std::array<TextureStageState, kMaxTexStages> texture_stage_states_;

  // Internal rendering resources.

  std::unordered_map<PSOState, ComPtr<ID3D12PipelineState>> pso_cache_;
  // Carries a NextShaderId() (vertex_shader.h) alongside the blob so
  // PSOState.ps can key off a stable id here too, the same as it does for
  // programmable pixel shaders (PixelShader::unique_id) -- see NextShaderId's
  // comment for why a raw ID3DBlob* isn't safe to use as that key. This
  // specific cache never evicts, so its own blobs' addresses never actually
  // get reused -- but pso_key.ps has to be one consistent kind of value
  // regardless of which of the two pixel-shader paths (this cache, or a
  // programmable PixelShader) produced it.
  struct CachedPixelShader {
    ComPtr<ID3DBlob> blob;
    uint64_t id = NextShaderId();
  };
  std::unordered_map<PixelShaderState, CachedPixelShader> ps_cache_;
  std::unordered_map<SamplerDesc, D3D12_GPU_DESCRIPTOR_HANDLE> sampler_cache_;

  enum DirtyFlags : uint32_t {
    DIRTY_FLAG_CMD_LIST_CLOSED = 0x00000001,
    DIRTY_FLAG_OM = DIRTY_FLAG_CMD_LIST_CLOSED << 1,
    DIRTY_FLAG_TRANSFORMS = DIRTY_FLAG_OM << 1,
    DIRTY_FLAG_VS_CBUFFER = DIRTY_FLAG_TRANSFORMS << 1,
    DIRTY_FLAG_PS_CBUFFER = DIRTY_FLAG_VS_CBUFFER << 1,
    DIRTY_FLAG_PS_TEXTURES = DIRTY_FLAG_PS_CBUFFER << 1,
    DIRTY_FLAG_PS_SAMPLERS = DIRTY_FLAG_PS_TEXTURES << 1,
    DIRTY_FLAG_LIGHTS = DIRTY_FLAG_PS_SAMPLERS << 1,
    // Set by every state change that feeds into CreatePSO's cache key
    // (render state, texture stage state, bound textures/shaders, render
    // target formats). Building that key is expensive -- it copies and
    // hashes the whole RenderState plus all 8 TextureStageStates, well over
    // a kilobyte, and on a cache hit compares the same again -- so doing it
    // per draw call was pure overhead for the large majority of draws, which
    // follow another draw with identical state.
    DIRTY_FLAG_PSO = DIRTY_FLAG_LIGHTS << 1,

    DIRTY_FLAG_ALL = DIRTY_FLAG_PSO | (DIRTY_FLAG_PSO - 1),
    DIRTY_FLAG_ALL_RESOURCES = DIRTY_FLAG_ALL & ~DIRTY_FLAG_CMD_LIST_CLOSED,
  };

  DirtyFlags dirty_flags_ = DIRTY_FLAG_ALL;

  // Redundant-set check for PrepareDrawCall's IASetPrimitiveTopology --
  // reset to UNDEFINED whenever the command list itself gets Reset() (which
  // drops all IA state), so the next draw always re-sets it there.
  D3D12_PRIMITIVE_TOPOLOGY last_prim_topology_ =
      D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

  std::unique_ptr<DynamicRingBuffer> dynamic_ring_buffer_;

  ComPtr<Buffer> vs_cbuffer_;
  ComPtr<Buffer> lights_cbuffer_;
  ComPtr<Buffer> ps_cbuffer_;

  // Constant buffers used to store constants for the programmable vertex and
  // pixel shaders.
  ComPtr<Buffer> vs_creg_cbuffer_;
  ComPtr<Buffer> ps_creg_cbuffer_;

  ComPtr<ID3D12RootSignature> main_root_sig_;
  unsigned int textures_start_bindslot_ = UINT32_MAX;

  DescriptorPoolHeap rtv_heap_;
  DescriptorPoolHeap srv_heap_;
  DescriptorPoolHeap sampler_heap_;
  DescriptorPoolHeap dsv_heap_;

  std::array<std::vector<InternalPtr<RefCounted>>, kNumBackBuffers>
      frame_resources_to_free_;
  // See RefCounted::last_marked_generation_. Starts at 1 (not the array's
  // default 0) so a freshly-constructed resource's zero-initialized stamp
  // can never spuriously match before it's ever been marked.
  std::array<uint64_t, kNumBackBuffers> slot_generation_ = [] {
    std::array<uint64_t, kNumBackBuffers> a;
    a.fill(1);
    return a;
  }();
  // Plain vector rather than a set: dedup is handled by a flag on the buffer
  // itself (Buffer::is_marked_for_persist_), which avoids hashing a pointer
  // and -- more importantly -- re-AddRef'ing the same buffer on every one of
  // the many dynamic locks a streaming buffer takes within a single frame.
  std::vector<ComPtr<Buffer>> buffers_to_persist_;

  // TODO: Make macro for this. Or just make dirty_flags_ an int.
  friend DirtyFlags &operator|=(DirtyFlags &, DirtyFlags);
  friend DirtyFlags &operator^=(DirtyFlags &, DirtyFlags);
};

static constexpr D3D12_HEAP_PROPERTIES kSystemMemHeapProps = {
    .Type = D3D12_HEAP_TYPE_CUSTOM,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_L0};

ComPtr<ID3DBlob> CreatePixelShaderFromState(const PixelShaderState &s);

// The single live Device instance, for dx8to12_api.cpp (see device.cpp).
// Returns null before device creation / after device destruction.
Device *GetCurrentDeviceForModApi();

}  // namespace Dx8to12
