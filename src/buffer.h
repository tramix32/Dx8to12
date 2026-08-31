#pragma once

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "d3d8.h"
#include "dynamic_ring_buffer.h"
#include "util.h"
#include "utils/dx_utils.h"
#include "utils/range_set.h"

namespace D3D12MA {
class Allocation;
}

namespace Dx8to12 {
class Device;

class Buffer : public IDirect3DVertexBuffer8,
               public IDirect3DIndexBuffer8,
               public RefCounted {
 public:
  Buffer() : resource_desc_({}), fvf_(0), d3d8_pool_(D3DPOOL_DEFAULT) {}

  void InitAsVertexBuffer(Device* device, size_t size_in_bytes,
                          Dx8::Usage usage, D3DPOOL pool, DWORD fvf);
  void InitAsIndexBuffer(Device* device, size_t size_in_bytes, Dx8::Usage usage,
                         D3DFORMAT d3d8_format, D3DPOOL pool);
  void InitAsBuffer(Device* device, size_t size_in_bytes, Dx8::Usage usage,
                    D3DPOOL pool);

  void AcquireDevice() {}
  void ReleaseDevice() {}

  bool IsDynamic() const { return usage_.Has(Dx8::Usage::Dynamic); }
  // Monotonically advances after CPU-visible contents have changed.  DXR
  // scene tracking uses it to rebuild a cached BLAS only when its source
  // geometry was actually modified.
  uint64_t content_generation() const { return content_generation_; }
  // Called at the end of a frame to persist any changes made to dynamic
  // buffers.
  virtual void PersistDynamicChanges();
  virtual GpuPtr GetGpuPtr();

  // Diagnostic: is [offset, offset+size) memory this frame actually filled
  // in, or is the GPU about to read whatever happened to be lying there? Only
  // dynamic buffers can answer meaningfully -- a static buffer's contents are
  // always valid, so the base returns true.
  virtual bool IsRangeWrittenThisFrame(int /*offset*/, int /*size*/) {
    return true;
  }
  // Diagnostic: CPU-readable pointer at `offset` into whatever memory the GPU
  // will read for this draw, or nullptr if there isn't one. For an ordinary
  // buffer that is its own mapped upload-heap allocation -- which is also the
  // memory a *dynamic* buffer's draws read on every frame where the game
  // appended with D3DLOCK_NOOVERWRITE without discarding first, so this base
  // implementation covers roughly half of all locks and must not be skipped.
  // Takes the size too: a diagnostic that reads past the end of whichever
  // buffer it landed on is worse than no diagnostic, and the readable extent
  // differs per path (discard cache, ring allocation, persistent resource).
  virtual const char* DebugCpuPtr(int offset, int size) {
    if (offset < 0 || size < 0 || offset + size > size_) return nullptr;
#ifdef DX8TO12_BUFFER_SHADOW
    // The shadow is authoritative in this mode: it holds every byte the app
    // has ever written, whereas the resource only holds what has been flushed
    // so far.
    if (!shadow_.empty())
      return reinterpret_cast<const char*>(shadow_.data()) + offset;
    return nullptr;
#else
    if (persistent_mapped_ptr_ == nullptr) return nullptr;
    return reinterpret_cast<const char*>(persistent_mapped_ptr_) + offset;
#endif
  }

  // Diagnostic: which memory a draw off this buffer reads right now, and how
  // it got there. A text batch reading vertex 0 is perfectly correct if the
  // game just discarded and refilled the buffer, and catastrophic if it is
  // instead seeing the previous frame's contents -- and the two are
  // indistinguishable without knowing which path the buffer is on.
  virtual std::string DebugState() {
    std::ostringstream state;
    state << "static lock=" << last_lock_offset_ << "+" << last_lock_size_
          << " flags=0x" << std::hex << last_lock_flags_ << std::dec
          << " count=" << lock_count_;
    return state.str();
  }
  // Production counterpart used by the x64 helper's CPU transport. Static
  // GPU-resident buffers expose their persistently mapped staging copy;
  // upload-heap buffers expose their own persistent mapping.
  const char* CpuDataPtr(int offset, int size) const {
    if (offset < 0 || size < 0 || offset > size_ - size) return nullptr;
#ifdef DX8TO12_BUFFER_SHADOW
    if (!shadow_.empty())
      return reinterpret_cast<const char*>(shadow_.data()) + offset;
    return nullptr;
#else
    const BYTE* data = gpu_resident_ ? staging_mapped_ptr_
                                     : persistent_mapped_ptr_;
    return data ? reinterpret_cast<const char*>(data) + offset : nullptr;
#endif
  }

  ID3D12Resource* resource();
  D3D12_RESOURCE_DESC resource_desc() const { return resource_desc_; }

  D3D12_RESOURCE_STATES current_state() const { return current_state_; }
  void set_state(D3D12_RESOURCE_STATES state) { current_state_ = state; }

  DXGI_FORMAT index_buffer_fmt() const {
    ASSERT(index_buffer_fmt_ != DXGI_FORMAT_UNKNOWN);
    return index_buffer_fmt_;
  }

  bool IsIndexBuffer() const { return index_buffer_fmt_ != DXGI_FORMAT_UNKNOWN; }

  // See Device::MarkBufferForPersist / is_marked_for_persist_.
  bool is_marked_for_persist() const { return is_marked_for_persist_; }
  void set_marked_for_persist(bool marked) { is_marked_for_persist_ = marked; }

#ifdef DX8TO12_ENABLE_VALIDATION
  const std::wstring& name() const { return name_; }
#endif

 public:
#undef PURE
#define PURE VIRT_NOT_IMPLEMENTED
  /*** IUnknown methods ***/
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** ppvObj) override;
  virtual ULONG STDMETHODCALLTYPE AddRef(THIS) override {
    return RefCounted::AddRef();
  }
  virtual ULONG STDMETHODCALLTYPE Release(THIS) override {
    return RefCounted::Release();
  }

  /*** IDirect3DResource8 methods ***/
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** ppDevice) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void* pData,
                                           DWORD SizeOfData,
                                           DWORD Flags) override {
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void* pData,
                                           DWORD* pSizeOfData) override {
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override {
    return D3DERR_NOTAVAILABLE;
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override {
    // Bookkeeping only -- we don't implement resource eviction/priority.
    DWORD previous = priority_;
    priority_ = PriorityNew;
    return previous;
  }
  DWORD STDMETHODCALLTYPE GetPriority(THIS) override { return priority_; }
  void STDMETHODCALLTYPE PreLoad(THIS) override {}  // Do nothing.
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType(THIS) override {
    return IsIndexBuffer() ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER;
  }

  virtual HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock,
                                         BYTE** ppbData, DWORD Flags) override;
  virtual HRESULT STDMETHODCALLTYPE Unlock(THIS) override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* pDesc) override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* pDesc) override;

 protected:
  Device* device_;
#ifdef USE_ALLOCATOR
  ComPtr<D3D12MA::Allocation> allocation_;
#else
  ComPtr<ID3D12Resource> resource_;
#endif
  D3D12_RESOURCE_DESC resource_desc_;
  // Persistent CPU mapping, established lazily on the first Lock and kept
  // for the buffer's lifetime. Every buffer here lives in CPU-visible memory
  // (see kSystemMemHeapProps), so there's nothing to gain from unmapping
  // between locks -- and Map/Unmap are driver calls that a game locking its
  // buffers every frame would otherwise pay on every single lock.
  // DynamicRingBuffer already uses this same map-once approach.
  BYTE* persistent_mapped_ptr_ = nullptr;
  // Set between Lock and Unlock when kPersistentBufferMapping is off.
  bool unmap_on_unlock_ = false;
  // Non-dynamic buffers live in GPU-local memory (D3D12_HEAP_TYPE_DEFAULT)
  // so the GPU reads their geometry out of VRAM instead of pulling it across
  // PCIe from system memory on every frame that draws them. Since a DEFAULT
  // heap resource can't be CPU-mapped, each one carries a persistently
  // mapped CPU-visible staging copy that Lock() hands out and Unlock()
  // uploads the written range from.
  //
  // A dedicated per-buffer staging resource rather than the shared
  // DynamicRingBuffer: uploads here are driven by the app's Lock/Unlock
  // pattern, not by frame cadence, so a level load doing hundreds of
  // buffer fills in a single frame would blow through the ring buffer's
  // frame-scoped budget (which only reclaims space once the GPU finishes a
  // frame) and hit its hard OOM failure. It costs one system-memory copy per
  // buffer, which is what D3DPOOL_MANAGED semantically implies anyway.
  bool gpu_resident_ = false;
  ComPtr<ID3D12Resource> staging_;
  BYTE* staging_mapped_ptr_ = nullptr;
  // Range handed out by the most recent Lock(), uploaded by Unlock().
  int locked_offset_ = 0;
  int locked_size_ = 0;
  // Set while this buffer is sitting in Device::buffers_to_persist_, so
  // repeat locks in the same frame don't queue it more than once. See
  // Device::MarkBufferForPersist.
  bool is_marked_for_persist_ = false;
  DWORD fvf_ = 0;
  D3DPOOL d3d8_pool_ = D3DPOOL_DEFAULT;
  Dx8::Usage usage_;
  DXGI_FORMAT index_buffer_fmt_ = DXGI_FORMAT_UNKNOWN;
  // Allocated size, including any DX8TO12_PAD_BUFFERS slack. Everything
  // internal -- lock bounds, the vertex/index buffer views, the index clamp in
  // device.cpp -- uses this.
  int size_ = 0;
  // Size reported back to the game through GetDesc: the 256-aligned request
  // WITHOUT the pad, so padding cannot change what the app believes it owns.
  int d3d8_size_ = 0;
  // How much slack DX8TO12_PAD_BUFFERS adds. Sized from the measurement that
  // motivated it (see InitAsBuffer): the overrunning buffers were 256-3072
  // bytes, so a 4KB pad covers a full extra copy of the largest of them.
  static constexpr int kBufferPadBytes = 4096;
  // Compact last-lock record used only by release-mindebug snapshots. Keeping
  // it in the resource itself avoids hot-path file I/O while preserving the
  // exact D3D8 range that populated a streamed static mesh.
  UINT last_lock_offset_ = 0;
  UINT last_lock_size_ = 0;
  DWORD last_lock_flags_ = 0;
  uint32_t lock_count_ = 0;
  uint64_t content_generation_ = 0;
  DWORD priority_ = 0;
  D3D12_RESOURCE_STATES current_state_ = D3D12_RESOURCE_STATE_COMMON;

#ifdef DX8TO12_ENABLE_MINDEBUG
  // Frame of the most recent GetGpuPtr(), i.e. the last frame in which a draw
  // actually took this buffer's address. GetGpuPtr is a Buffer's only per-draw
  // touch point, so this is the cheapest way to answer "has a draw already
  // committed to this buffer's contents this frame" -- which is precisely what
  // turns an ordinary Lock into the aliasing hazard: that draw is recorded but
  // not yet submitted, and it reads the very bytes the Lock is about to
  // rewrite. See ReportLockHazard.
  uint64_t draw_frame_ = 0;
  // Only the first hazard per buffer is logged; the rest are counted.
  bool logged_hazard_ = false;
  // Reports a Lock that lands on memory a draw recorded earlier in this same,
  // not-yet-submitted frame is going to read.
  void ReportLockHazard(UINT offset, UINT size, DWORD flags);
  // Accounts a newly allocated CPU shadow against the process-wide total.
  static void NoteShadowAllocated(size_t bytes);
#endif

#ifdef DX8TO12_BUFFER_SHADOW
  // Full CPU shadow of the buffer, modelled directly on the working d3d8to11
  // port (d3d8to11_vertex_buffer.cpp's m_shadow). Every Lock -- whatever its
  // flags -- hands out a pointer into here, so the app can never write memory
  // the GPU is already reading; the dirty range is pushed to the real resource
  // through the command list in GetGpuPtr(), where it is ordered against the
  // draws around it.
  //
  // This is what the shim structurally lacks otherwise: Buffer::Lock() returns
  // the mapped upload heap itself, i.e. exactly the bytes an already-recorded
  // draw will read once the command list finally executes.
  // Allocated as size_ + kShadowGuardBytes; the tail is filled with
  // kShadowGuardFill so an app write past the buffer is caught rather than
  // silently smashing CRT heap metadata. See ShadowLock/CheckShadowGuard.
  static constexpr int kShadowGuardBytes = 4096;
  static constexpr uint8_t kShadowGuardFill = 0xCD;
  std::vector<uint8_t> shadow_;
  // Half-open [lo, hi) byte range touched since the last upload. Empty when
  // lo >= hi. A single pair rather than a RangeSet on purpose: RangeSet::insert
  // asserts on partially overlapping inserts, and re-locking an already-locked
  // sub-range is entirely legal in D3D8.
  int shadow_dirty_lo_ = 0;
  int shadow_dirty_hi_ = 0;

  // Grows the shadow on first use and widens the dirty range. Returns the
  // pointer the app should write through, or nullptr if the range is invalid.
  BYTE* ShadowLock(UINT offset_to_lock, UINT size_to_lock);
  // Records a CopyBufferRegion of the pending dirty range into the command
  // list, if there is one and the list is open.
  void FlushShadowToResource();
  // Verifies the guard tail is still intact, i.e. that the app has not written
  // past the end of the buffer. Reported once per buffer.
  void CheckShadowGuard();
  bool logged_guard_overrun_ = false;
#endif

#ifdef DX8TO12_ENABLE_VALIDATION
  std::wstring name_;
#endif
};

class DynamicBuffer : public Buffer {
 public:
  DynamicBuffer() : Buffer() {}

  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock,
                                 BYTE** ppbData, DWORD Flags) noexcept override;
  HRESULT STDMETHODCALLTYPE Unlock() noexcept override;

  GpuPtr GetGpuPtr() override;

  // Resolves the allocation that the next draw actually reads.  Calling this
  // may persist a speculative DISCARD write, exactly like GetGpuPtr().  It is
  // intentionally exposed for the x64 RT helper's per-frame snapshot path;
  // Buffer::resource() is not the source of a dynamic draw.
  bool GetCurrentRingAllocation(DynamicRingBuffer::Allocation* allocation);

  void PersistDynamicChanges() override;

  bool IsRangeWrittenThisFrame(int offset, int size) override;
  const char* DebugCpuPtr(int offset, int size) override;
  std::string DebugState() override;

 private:
  void PersistSpeculativeWrite();
  void UpdateCbvForRingBuffer(int offset, int size);
#ifdef DX8TO12_ENABLE_MINDEBUG
  // Detects the two lock/draw orderings that let a plain Lock change what an
  // already-recorded draw reads, or land in memory no draw will ever read.
  // See draw_frame_ and the LOCKHAZARD lines in buffer.cpp.
  void ReportPlainLockHazard(int offset, int size);
#endif

  // We store the last dynamic write that the user has done. The vector is
  // grow-only and its size() is NOT the logical size of the pending write --
  // it used to be cleared and resized on every D3DLOCK_DISCARD, which
  // value-initializes the whole buffer, i.e. a full memset of vertex data the
  // caller is about to overwrite anyway (that being exactly what DISCARD
  // means). has_speculative_write_/speculative_write_size_ carry the state
  // that emptiness/size() used to imply, so the storage can just be reused.
  std::vector<char> speculative_write_cache_;
  bool has_speculative_write_ = false;
  int speculative_write_size_ = 0;
  bool is_speculative_write_persisted_ = false;

  DynamicRingBuffer::Allocation current_ring_alloc_ = {};
  D3D12_CPU_DESCRIPTOR_HANDLE prev_csv_handle_ = {};
  uint64_t prev_lock_frame_ = 0;
  // Set for the lifetime of a Lock()/Unlock() pair that fell back to the
  // plain Buffer::Lock() path (neither D3DLOCK_DISCARD nor
  // D3DLOCK_NOOVERWRITE) -- Unlock() must check this explicitly rather than
  // re-deriving it from prev_lock_frame_, since a plain lock can happen in
  // the same frame as an earlier DISCARD/NOOVERWRITE one, where the frame
  // comparison alone can't tell the two apart.
  bool is_plain_lock_ = false;

  RangeSet written_ranges_;
  // static constexpr bool use_cbv_ = false;

};

#undef PURE
#define PURE = 0

}  // namespace Dx8to12
