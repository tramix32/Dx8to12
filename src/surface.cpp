#include "surface.h"

#include <utility>

#include "device.h"
#include "texture.h"
#include "utils/dx_utils.h"

namespace Dx8to12 {

HRESULT STDMETHODCALLTYPE BaseSurface::QueryInterface(REFIID riid,
                                                       void** ppvObj) {
  if (riid == IID_IUnknown || riid == IID_IDirect3DResource8 ||
      riid == IID_IDirect3DSurface8) {
    *ppvObj = this;
    AddRef();
    return S_OK;
  }
  *ppvObj = nullptr;
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE BaseSurface::GetDesc(D3DSURFACE_DESC* pDesc) {
  if (pDesc == nullptr) return D3DERR_INVALIDCALL;
  *pDesc = desc_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE BaseSurface::GetDevice(IDirect3DDevice8** ppDevice) {
  *ppDevice = device_;
  device_->AddRef();
  return S_OK;
}

HRESULT BaseSurface::LockGpuReadback(GpuTexture* texture, uint32_t subresource,
                                     D3DLOCKED_RECT* pLockedRect,
                                     CONST RECT* pRect, DWORD Flags) {
  ASSERT(!readback_resource_);
  const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint =
      texture->GetFootprint(subresource);
  const UINT64 total_bytes =
      footprint.Offset + static_cast<UINT64>(footprint.Footprint.RowPitch) *
                             footprint.Footprint.Height;

  D3D12_HEAP_PROPERTIES heap_props{.Type = D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC buffer_desc{
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Width = total_bytes,
      .Height = 1,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1, .Quality = 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  ASSERT_HR(device_->device()->CreateCommittedResource(
      &heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(readback_resource_.GetForInit())));

  const D3D12_RESOURCE_STATES prior_state = texture->current_state(subresource);
  device_->TransitionTexture(texture, subresource,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);

  D3D12_TEXTURE_COPY_LOCATION dst_location{
      .pResource = readback_resource_.get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = footprint};
  D3D12_TEXTURE_COPY_LOCATION src_location{
      .pResource = texture->resource(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = subresource};
  device_->cmd_list()->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location,
                                         nullptr);

  device_->TransitionTexture(texture, subresource, prior_state);
  device_->MarkResourceAsUsed(InternalPtr(texture));

  // Flush and block until the GPU has actually performed the copy above --
  // D3D8's LockRect is a synchronous call, the caller expects pBits to be
  // readable the moment this returns.
  // DIAGNOSTIC: this is a full synchronous GPU flush+wait mid-frame, with no
  // real analog on actual D3D8 hardware (a real driver's Lock on a lockable
  // render target is typically much cheaper). Investigating whether this is
  // what disrupts RenderWare's own timing assumptions for the missing-
  // ground-texture bug (see SETTEX0-NULL-CALLER in device.cpp) -- log every
  // occurrence to correlate against that diagnostic's frame numbers.
#ifdef DX8TO12_ENABLE_VALIDATION
  LOG(AixLog::Severity::error)
      << "MIDFRAME-LOCKREADBACK-FLUSH frame=" << device_->CurrentFrame()
      << "\n";
#endif
  device_->SubmitAndWait(false);

  D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
  void* mapped = nullptr;
  ASSERT_HR(readback_resource_->Map(0, &read_range, &mapped));

  char* level_ptr = static_cast<char*>(mapped) + footprint.Offset;
  if (pRect) {
    const int format_size = DXGIFormatSize(footprint.Footprint.Format);
    level_ptr +=
        pRect->top * footprint.Footprint.RowPitch + pRect->left * format_size;
  }
  pLockedRect->Pitch = static_cast<INT>(footprint.Footprint.RowPitch);
  pLockedRect->pBits = level_ptr;
  return S_OK;
}

HRESULT BaseSurface::UnlockGpuReadback() {
  if (!readback_resource_) return D3DERR_INVALIDCALL;
  D3D12_RANGE written_range{0, 0};
  readback_resource_->Unmap(0, &written_range);
  readback_resource_.Reset();
  return S_OK;
}

GpuSurface::GpuSurface(Device* device, GpuTexture* texture,
                       uint32_t subresource)
    : BaseSurface(SurfaceKind::Gpu, device),
      texture_(ComWrap(texture)),
      subresource_(subresource) {
  desc_ = texture_->GetSurfaceDesc(subresource);
}

HRESULT STDMETHODCALLTYPE GpuSurface::LockRect(D3DLOCKED_RECT* pLockedRect,
                                               CONST RECT* pRect,
                                               DWORD Flags) {
  if (texture_->d3d8_pool() == D3DPOOL_MANAGED) {
    return texture_->LockRect(subresource_, pLockedRect, pRect, Flags);
  }
  // A lockable D3DPOOL_DEFAULT render target: there is no CPU-visible copy,
  // so read the current contents back from the GPU.
  return LockGpuReadback(texture_.get(), subresource_, pLockedRect, pRect,
                         Flags);
}

HRESULT STDMETHODCALLTYPE GpuSurface::UnlockRect() {
  if (texture_->d3d8_pool() == D3DPOOL_MANAGED) {
    return texture_->UnlockRect(subresource_);
  }
  return UnlockGpuReadback();
}

CpuSurface::CpuSurface(CpuTexture* texture, int level,
                       const D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint,
                       int compact_pitch, char* data_ptr)
    : BaseSurface(SurfaceKind::Cpu, texture->device()),
      texture_(ComWrap(texture)),
      footprint_(footprint),
      compact_pitch_(compact_pitch),
      data_ptr_(data_ptr) {
  ASSERT_HR(texture_->GetLevelDesc(level, &desc_));
}

HRESULT STDMETHODCALLTYPE CpuSurface::LockRect(D3DLOCKED_RECT* pLockedRect,
                                               CONST RECT* pRect,
                                               DWORD Flags) {
  char* rect_ptr = data_ptr_;
  if (pRect) {
    // See the matching guard in CpuTexture::LockRect (texture.cpp) --
    // partial-rect locking of block-compressed (DXT/S3TC) data would need
    // pRect's texel coordinates converted to 4x4-block coordinates, which
    // isn't implemented; whole-surface locks work fine.
    DXGI_FORMAT dxgi_format = DXGIFromD3DFormat(desc_.Format);
    if (IsBlockCompressedFormat(dxgi_format)) {
      FAIL(
          "CpuSurface::LockRect: partial-rect lock of a block-compressed "
          "(DXT/S3TC) surface is not supported -- lock the whole surface "
          "instead.");
    }
    const int format_size = DXGIFormatSize(dxgi_format);
    rect_ptr += pRect->top * compact_pitch_ + pRect->left * format_size;
  }
  pLockedRect->Pitch = compact_pitch_;
  pLockedRect->pBits = rect_ptr;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CpuSurface::UnlockRect() { return S_OK; }

BackbufferSurface::BackbufferSurface(Device* device, int index,
                                     GpuTexture* texture)
    : BaseSurface(SurfaceKind::Backbuffer, device),
      index_(index),
      texture_(ComWrap(texture)) {
  const D3D12_RESOURCE_DESC& desc = texture_->resource_desc();
  desc_ = D3DSURFACE_DESC{.Format = DXGIToD3DFormat(desc.Format),
                          .Type = D3DRTYPE_SURFACE,
                          .Usage = D3DUSAGE_RENDERTARGET,
                          .Pool = D3DPOOL_DEFAULT,
                          .Size = safe_cast<UINT>(desc.Width * desc.Height *
                                                  DXGIFormatSize(desc.Format)),
                          .MultiSampleType = D3DMULTISAMPLE_NONE,
                          .Width = safe_cast<UINT>(desc.Width),
                          .Height = desc.Height};
}

HRESULT STDMETHODCALLTYPE BackbufferSurface::LockRect(
    D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
  return LockGpuReadback(texture_.get(), 0, pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE BackbufferSurface::UnlockRect() {
  return UnlockGpuReadback();
}
}  // namespace Dx8to12
