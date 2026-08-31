#include "buffer.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "aixlog.hpp"
#include "device.h"
#include "dynamic_ring_buffer.h"
#include "util.h"

#ifdef USE_ALLOCATOR
#include "D3D12MemAlloc.h"
#endif

namespace Dx8to12 {

static AixLog::Severity kLog = AixLog::Severity::trace;

HRESULT STDMETHODCALLTYPE Buffer::QueryInterface(REFIID riid, void **ppvObj) {
  if (riid == IID_IUnknown || riid == IID_IDirect3DResource8) {
    *ppvObj = static_cast<IDirect3DVertexBuffer8 *>(this);
    AddRef();
    return S_OK;
  }
  if (!IsIndexBuffer() && riid == IID_IDirect3DVertexBuffer8) {
    *ppvObj = static_cast<IDirect3DVertexBuffer8 *>(this);
    AddRef();
    return S_OK;
  }
  if (IsIndexBuffer() && riid == IID_IDirect3DIndexBuffer8) {
    *ppvObj = static_cast<IDirect3DIndexBuffer8 *>(this);
    AddRef();
    return S_OK;
  }
  *ppvObj = nullptr;
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Buffer::GetDevice(IDirect3DDevice8 **ppDevice) {
  *ppDevice = device_;
  device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Buffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
  *pDesc = D3DVERTEXBUFFER_DESC{.Format = D3DFMT_VERTEXDATA,
                                .Type = D3DRTYPE_VERTEXBUFFER,
                                .Usage = static_cast<DWORD>(usage_),
                                .Pool = d3d8_pool_,
                                .Size = safe_cast<UINT>(d3d8_size_),
                                .FVF = fvf_};
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Buffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
  *pDesc = D3DINDEXBUFFER_DESC{.Format = DXGIToD3DFormat(index_buffer_fmt_),
                               .Type = D3DRTYPE_INDEXBUFFER,
                               .Usage = static_cast<DWORD>(usage_),
                               .Pool = d3d8_pool_,
                               .Size = safe_cast<UINT>(d3d8_size_)};
  return S_OK;
}

void Buffer::InitAsBuffer(Device* device, size_t size_in_bytes,
                          Dx8::Usage usage, D3DPOOL pool) {
  ASSERT(pool != D3DPOOL_SCRATCH);
  size_in_bytes = AlignUp(size_in_bytes, 256);
  d3d8_size_ = safe_cast<int>(size_in_bytes);
#ifdef DX8TO12_PAD_BUFFERS
  // GTA VC writes past the end of its own static index buffers -- measured
  // directly: 219 distinct index buffers tripped the shadow guard in one
  // session, every one of them starting at the very first byte past the
  // declared end. On real D3D8 (and in the working d3d8to11 port) that is
  // absorbed harmlessly; here it also means DrawIndexedPrimitive references
  // indices past the bound view, which the clamp in device.cpp then cuts off,
  // dropping exactly the trailing triangles of a material -- the shape of the
  // reported holes.
  //
  // Allocating extra room lets those writes and the draws that read them stay
  // in valid, initialised memory. The pad is invisible to the game:
  // GetDesc keeps reporting d3d8_size_, so nothing about what the app thinks
  // it owns changes.
  size_in_bytes += kBufferPadBytes;
#endif
  resource_desc_ = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                    .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                    .Width = static_cast<UINT64>(size_in_bytes),
                    .Height = 1,
                    .DepthOrArraySize = 1,
                    .MipLevels = 1,
                    .Format = DXGI_FORMAT_UNKNOWN,
                    .SampleDesc = {.Count = 1, .Quality = 0},
                    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                    .Flags = D3D12_RESOURCE_FLAG_NONE};
  d3d8_pool_ = pool;
  usage_ = usage;
  device_ = device;
  size_ = safe_cast<int>(size_in_bytes);
#ifdef DX8TO12_USE_ALLOCATOR
  D3D12MA::ALLOCATION_DESC alloc_desc{.HeapType = D3D12_HEAP_TYPE_UPLOAD};
  ASSERT_HR(device->allocator()->CreateResource(
      &alloc_desc, &resource_desc_, D3D12_RESOURCE_STATE_COMMON, nullptr,
      allocation_.GetForInit(), IID_NULL, nullptr));
#else
  D3D12_HEAP_PROPERTIES cpu_heap_props = kSystemMemHeapProps;
  if (usage.Has(Dx8::Usage::WriteOnly)) {
    // D3DUSAGE_WRITEONLY is the app explicitly promising it will never read
    // this buffer back, which is exactly the condition write-combined memory
    // wants: CPU writes stream out without the cache-snoop traffic that
    // write-back memory forces on every GPU read of the same pages. Left as
    // write-back otherwise, since WC memory is pathologically slow to read
    // and a buffer without the flag is allowed to be read back.
    cpu_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
  }

  // Dynamic buffers keep their contents in system memory: their fast paths
  // (DynamicBuffer::Lock) stream through DynamicRingBuffer and the GPU reads
  // straight out of that, so a VRAM copy would just add an upload of data
  // that's replaced again next frame. Everything else is written rarely and
  // read every frame, which is what GPU-local memory is for -- see
  // gpu_resident_.
  gpu_resident_ = kBuffersInGpuMemory && !usage.Has(Dx8::Usage::Dynamic);
  if (gpu_resident_) {
    D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ASSERT_HR(device->device()->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &resource_desc_,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(resource_.GetForInit())));
    ASSERT_HR(device->device()->CreateCommittedResource(
        &cpu_heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc_,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(staging_.GetForInit())));
  } else {
    ASSERT_HR(device->device()->CreateCommittedResource(
        &cpu_heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc_,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(resource_.GetForInit())));
  }
#endif

#ifdef DX8TO12_PAD_BUFFERS
  // The pad only helps if it is *defined*. An upload heap comes back with
  // undefined contents, so leaving it alone would trade "trailing triangles
  // clamped away" for "trailing triangles built from garbage indices", which
  // is a worse failure. Zeroes collapse into degenerate triangles for any
  // range the game does not actually fill in.
  if (!gpu_resident_) {
    D3D12_RANGE no_read{0, 0};
    BYTE *zero_ptr = nullptr;
    ASSERT_HR(
        resource_->Map(0, &no_read, reinterpret_cast<void **>(&zero_ptr)));
    memset(zero_ptr, 0, static_cast<size_t>(size_));
    if (kPersistentBufferMapping) {
      persistent_mapped_ptr_ = zero_ptr;
    } else {
      resource_->Unmap(0, nullptr);
    }
  }
#endif

  // wchar_t name[128];
  // _snwprintf(name, 128, L"addr:%p", this);
  // if (IsDynamic()) resource_->SetName(name);
}

void Buffer::InitAsVertexBuffer(Device* device, size_t size_in_bytes,
                                Dx8::Usage usage, D3DPOOL pool, DWORD fvf) {
  fvf_ = fvf;
  InitAsBuffer(device, size_in_bytes, usage, pool);
#ifdef DX8TO12_ENABLE_VALIDATION
  static int name_index = 0;
  std::wstringstream name;
  name << "VBuffer" << std::dec << name_index++ << ":" << std::hex << fvf;
  name_ = name.str();
  resource_->SetName(name_.c_str());
#endif
}

void Buffer::InitAsIndexBuffer(Device* device, size_t size_in_bytes,
                               Dx8::Usage usage, D3DFORMAT format,
                               D3DPOOL pool) {
  InitAsBuffer(device, size_in_bytes, usage, pool);
  index_buffer_fmt_ = DXGIFromD3DFormat(format);
}

ID3D12Resource* Buffer::resource() {
#ifdef DX8TO12_USE_ALLOCATOR
  return allocation_->GetResource();
#else
  return resource_.get();
#endif
}

// BIG TODO: Persist dynamic buffers at the end of the frame in case they are
// read the next frame.
HRESULT STDMETHODCALLTYPE Buffer::Lock(UINT OffsetToLock, UINT SizeToLock,
                                       BYTE** ppbData, DWORD Flags) {
  if (OffsetToLock >= static_cast<UINT>(size_)) return D3DERR_INVALIDCALL;
  if (SizeToLock > static_cast<UINT>(size_) - OffsetToLock)
    return D3DERR_INVALIDCALL;
#ifndef DX8TO12_BUFFER_SHADOW
  // In shadow mode D3DLOCK_DISCARD is routed here deliberately and is
  // indistinguishable from any other lock -- the shadow always holds the whole
  // buffer, so there is nothing to discard.
  ASSERT(!HasFlag(Flags, D3DLOCK_DISCARD));
#endif

  // D3D8's zero size means the remainder starting at OffsetToLock, not the
  // entire resource.  Using size_ here let a perfectly normal partial lock
  // map past the end of the upload allocation, overwriting unrelated freshly
  // streamed geometry at high frame rates.
  if (SizeToLock == 0) SizeToLock = static_cast<UINT>(size_) - OffsetToLock;
  last_lock_offset_ = OffsetToLock;
  last_lock_size_ = SizeToLock;
  last_lock_flags_ = Flags;
  ++lock_count_;

#ifdef DX8TO12_ENABLE_MINDEBUG
  // Every lock reaching this function -- static buffers always, dynamic ones
  // on the plain and first-NOOVERWRITE-of-frame paths -- hands the app a
  // pointer straight into the memory draws read, so this is where the
  // already-recorded-draw hazard has to be measured.
  ReportLockHazard(OffsetToLock, SizeToLock, Flags);
#endif

#ifdef DX8TO12_BUFFER_SHADOW
  BYTE *shadow_ptr = ShadowLock(OffsetToLock, SizeToLock);
  if (shadow_ptr == nullptr) return D3DERR_INVALIDCALL;
  *ppbData = shadow_ptr;
  return S_OK;
#else

  // Empty read range: D3D8 locks are writes unless D3DLOCK_READONLY says
  // otherwise, and declaring a read range the CPU won't actually read from
  // can force a needless cache invalidate. (The previous code declared the
  // whole locked range as read, with its own TODO noting as much.)
  D3D12_RANGE no_read{0, 0};
  if (gpu_resident_) {
    if (staging_mapped_ptr_ == nullptr) {
      ASSERT_HR(staging_->Map(
          0, &no_read, reinterpret_cast<void**>(&staging_mapped_ptr_)));
    }
    locked_offset_ = safe_cast<int>(OffsetToLock);
    locked_size_ = safe_cast<int>(SizeToLock);
    *ppbData = staging_mapped_ptr_ + OffsetToLock;
    return S_OK;
  }

  if (persistent_mapped_ptr_ == nullptr) {
    ASSERT_HR(resource()->Map(
        0, &no_read, reinterpret_cast<void**>(&persistent_mapped_ptr_)));
  }
  *ppbData = persistent_mapped_ptr_ + OffsetToLock;
  if (!kPersistentBufferMapping) {
    // Remember what to hand back to Unmap, and drop our cached pointer so
    // the next Lock maps again.
    unmap_on_unlock_ = true;
    persistent_mapped_ptr_ = nullptr;
  }

  return S_OK;
#endif  // DX8TO12_BUFFER_SHADOW
}

HRESULT STDMETHODCALLTYPE Buffer::Unlock() {
#ifdef DX8TO12_BUFFER_SHADOW
  // Nothing to upload from here. The dirty range goes out in GetGpuPtr(),
  // i.e. immediately before the draw that needs it, where it lands in the
  // command list *after* every previously recorded draw -- which is the whole
  // point of this mode. Uploading at Unlock time would work too, but only if
  // the command list happens to be open, and it is not always.
  ++content_generation_;
  return S_OK;
#else
  // Deliberately keeps the mapping alive -- see persistent_mapped_ptr_. The
  // old Unmap(0, nullptr) here also declared the *entire* resource as
  // written on every unlock, which is strictly more invalidation than any
  // single lock actually dirtied.
  if (unmap_on_unlock_) {
    unmap_on_unlock_ = false;
    resource()->Unmap(0, nullptr);
  }
  if (gpu_resident_ && locked_size_ > 0 && device_->IsCommandListOpen()) {
    // Push just the range this lock handed out up to the GPU-local copy.
    // Recorded into the current command list, so it lands ahead of any draw
    // that reads this buffer later in the frame.
    device_->TransitionBuffer(this, D3D12_RESOURCE_STATE_COPY_DEST);
    device_->cmd_list()->CopyBufferRegion(
        resource(), static_cast<UINT64>(locked_offset_), staging_.get(),
        static_cast<UINT64>(locked_offset_),
        static_cast<UINT64>(locked_size_));
    device_->TransitionBuffer(this, D3D12_RESOURCE_STATE_COMMON);
    locked_size_ = 0;
  }
  // A successful Lock/Unlock pair is the only public write path for a
  // non-dynamic buffer.  Increment even for an empty lock: it is harmlessly
  // conservative and avoids treating a driver-visible write as stale.
  ++content_generation_;
  return S_OK;
#endif
}

void Buffer::PersistDynamicChanges() {
  FAIL("Unexpected dynamic change persist in static buffer.");
}

#ifdef DX8TO12_ENABLE_MINDEBUG
namespace {
uint32_t g_locks_after_draw = 0;
uint32_t g_locks_after_draw_static = 0;
uint32_t g_locks_after_draw_index = 0;
uint32_t g_locks_total = 0;
uint64_t g_next_lock_summary_frame = 0;
// Total bytes held by CPU shadows. This process is 32-bit, so a mode that
// keeps a full CPU copy of every vertex/index buffer alongside the D3D12
// resource is a real address-space risk, not a theoretical one -- and an
// allocation failure here is an uncatchable crash, since this codebase has no
// exception handling. Tracked so a crash can be told apart from a rendering
// bug immediately.
size_t g_shadow_bytes = 0;
size_t g_shadow_buffers = 0;
}  // namespace

void Buffer::NoteShadowAllocated(size_t bytes) {
  g_shadow_bytes += bytes;
  ++g_shadow_buffers;
}

void Buffer::ReportLockHazard(UINT offset, UINT size, DWORD flags) {
  const uint64_t frame = device_->CurrentFrame();
  ++g_locks_total;
  // A draw recorded earlier in this frame already took this buffer's address.
  // The command list has not been submitted, so that draw will read whatever
  // this lock leaves behind -- not what the buffer held when it was recorded.
  const bool after_draw = draw_frame_ == frame;
  if (after_draw) {
    ++g_locks_after_draw;
    if (!IsDynamic()) ++g_locks_after_draw_static;
    if (IsIndexBuffer()) ++g_locks_after_draw_index;
  }
  if (after_draw && !logged_hazard_) {
    logged_hazard_ = true;
    std::ostringstream line;
    line << "LOCKAFTERDRAW-FIRST buf=" << this
         << (IsIndexBuffer() ? " kind=IB" : " kind=VB")
         << (IsDynamic() ? " dynamic=1" : " dynamic=0") << " size=" << size_
         << " lock=" << offset << "+" << size << " flags=0x" << std::hex
         << flags << std::dec << " frame=" << frame
         << " generation=" << content_generation_ << "\n";
    WriteMindebugDiagnosticLine(line.str());
  }
  if (frame >= g_next_lock_summary_frame) {
    g_next_lock_summary_frame = frame + 600;
    std::ostringstream line;
    line << "LOCKAFTERDRAW-TOTALS frame=" << frame
         << " locks=" << g_locks_total << " afterDraw=" << g_locks_after_draw
         << " afterDrawStatic=" << g_locks_after_draw_static
         << " afterDrawIB=" << g_locks_after_draw_index
         << " shadowBuffers=" << g_shadow_buffers
         << " shadowMB=" << (g_shadow_bytes / (1024 * 1024)) << "\n";
    WriteMindebugDiagnosticLine(line.str());
  }
}
#endif  // DX8TO12_ENABLE_MINDEBUG

#ifdef DX8TO12_BUFFER_SHADOW
BYTE *Buffer::ShadowLock(UINT offset_to_lock, UINT size_to_lock) {
  const int offset = safe_cast<int>(offset_to_lock);
  const int size = safe_cast<int>(size_to_lock);
  if (offset < 0 || size < 0 || offset > size_ - size) return nullptr;
  // Sized to size_, which is the 256-aligned allocation size, matching the
  // resource width the vertex/index buffer views span. Zero-initialised so a
  // draw reading a region the app never wrote sees degenerate triangles rather
  // than whatever was in the heap.
  if (shadow_.empty()) {
    // Over-allocate by a guard page and fill the tail with a sentinel. The
    // app used to write into a D3D12 upload-heap resource -- its own VirtualAlloc
    // region, with page slack after the buffer -- so a write running past the
    // locked range was absorbed silently. A std::vector sized exactly size_
    // has CRT heap metadata immediately after it instead, which turns that
    // same harmless-looking overrun into heap corruption that only surfaces
    // much later, in an unrelated free(). Keeping the slack preserves the old
    // forgiveness; CheckShadowGuard reports whether it is actually being used,
    // because an app overrun is itself a finding.
    shadow_.assign(static_cast<size_t>(size_) + kShadowGuardBytes,
                   kShadowGuardFill);
    std::fill_n(shadow_.begin(), static_cast<size_t>(size_), uint8_t{0});
#ifdef DX8TO12_ENABLE_MINDEBUG
    NoteShadowAllocated(static_cast<size_t>(size_) + kShadowGuardBytes);
#endif
  }
  if (shadow_dirty_lo_ >= shadow_dirty_hi_) {
    shadow_dirty_lo_ = offset;
    shadow_dirty_hi_ = offset + size;
  } else {
    shadow_dirty_lo_ = std::min(shadow_dirty_lo_, offset);
    shadow_dirty_hi_ = std::max(shadow_dirty_hi_, offset + size);
  }
  return shadow_.data() + offset;
}

void Buffer::CheckShadowGuard() {
  if (logged_guard_overrun_ || shadow_.empty()) return;
  // Only the head of the guard region is checked: an overrun starts right
  // after the buffer, so this catches it without a 4KB compare per flush.
  constexpr int kChecked = 64;
  const uint8_t *guard = shadow_.data() + size_;
  for (int i = 0; i < kChecked; ++i) {
    if (guard[i] == kShadowGuardFill) continue;
    logged_guard_overrun_ = true;
    std::ostringstream line;
    line << "SHADOWGUARD-OVERRUN buf=" << this
         << (IsIndexBuffer() ? " kind=IB" : " kind=VB")
         << (IsDynamic() ? " dynamic=1" : " dynamic=0") << " size=" << size_
         << " firstBadByte=+" << i << " lastLock=" << last_lock_offset_ << "+"
         << last_lock_size_ << " flags=0x" << std::hex << last_lock_flags_
         << std::dec << " frame=" << device_->CurrentFrame() << "\n";
    WriteMindebugDiagnosticLine(line.str());
    return;
  }
}

void Buffer::FlushShadowToResource() {
#ifdef DX8TO12_ENABLE_MINDEBUG
  CheckShadowGuard();
#endif
  if (shadow_dirty_lo_ >= shadow_dirty_hi_) return;
  // No open command list means there is nowhere to record the copy. Keep the
  // dirty range pending rather than dropping it: the next draw that needs
  // these bytes calls back in here with the list open. (The pre-existing
  // gpu_resident_ path in Unlock() drops the copy in this situation, which
  // silently loses the write.)
  if (!device_->IsCommandListOpen()) return;

  const int offset = shadow_dirty_lo_;
  const int size = shadow_dirty_hi_ - shadow_dirty_lo_;
  DynamicRingBuffer *ring = device_->dynamic_ring_buffer();
  DynamicRingBuffer::Allocation alloc{};
  if (!ring->TryAllocate(static_cast<size_t>(size),
                         D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                         &alloc)) {
    // The ring only reclaims at frame granularity, and this mode routes every
    // buffer's writes through it -- including static geometry during a heavy
    // streaming frame, which never touched the ring before. Rather than taking
    // the process down on a budget problem, fall back to writing the mapped
    // resource directly. That is exactly the unordered write this mode exists
    // to eliminate, so it is reported, not silently tolerated: a run with
    // SHADOWFLUSH-FALLBACK lines is not a clean A/B.
    D3D12_RANGE no_read{0, 0};
    if (persistent_mapped_ptr_ == nullptr) {
      ASSERT_HR(resource()->Map(
          0, &no_read, reinterpret_cast<void **>(&persistent_mapped_ptr_)));
    }
    memcpy(persistent_mapped_ptr_ + offset, shadow_.data() + offset,
           static_cast<size_t>(size));
    WriteMindebugDiagnosticLine(
        "SHADOWFLUSH-FALLBACK ringExhausted bytes=" + std::to_string(size) +
        " frame=" + std::to_string(device_->CurrentFrame()) + "\n");
    shadow_dirty_lo_ = 0;
    shadow_dirty_hi_ = 0;
    return;
  }
  memcpy(ring->GetCpuPtrFor(alloc), shadow_.data() + offset,
         static_cast<size_t>(size));

  // Both states have to be put back exactly as they were. PrepareDrawCall
  // transitions the ring and then this buffer into their draw states *before*
  // asking for this address (device.cpp), so leaving either in COPY_* here
  // would hand the draw resources in the wrong state.
  const D3D12_RESOURCE_STATES prev_buffer_state = current_state();
  const D3D12_RESOURCE_STATES prev_ring_state = ring->current_state();
  device_->TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  device_->TransitionBuffer(this, D3D12_RESOURCE_STATE_COPY_DEST);
  device_->cmd_list()->CopyBufferRegion(
      resource(), static_cast<UINT64>(offset), ring->GetBackingResource(),
      static_cast<UINT64>(alloc.offset), static_cast<UINT64>(size));
  device_->TransitionBuffer(this, prev_buffer_state);
  device_->TransitionDynamicRingBuffer(prev_ring_state);

  shadow_dirty_lo_ = 0;
  shadow_dirty_hi_ = 0;
}
#endif  // DX8TO12_BUFFER_SHADOW

GpuPtr Buffer::GetGpuPtr() {
#ifdef DX8TO12_ENABLE_MINDEBUG
  // A draw is now committing to this buffer's contents. See ReportLockHazard.
  draw_frame_ = device_->CurrentFrame();
#endif
#ifdef DX8TO12_BUFFER_SHADOW
  FlushShadowToResource();
#endif
  return resource()->GetGPUVirtualAddress();
}

// BIG TODO: Persist dynamic buffers at the end of the frame in case they are
// read the next frame.
HRESULT STDMETHODCALLTYPE DynamicBuffer::Lock(UINT OffsetToLock,
                                              UINT SizeToLock, BYTE** ppbData,
                                              DWORD Flags) noexcept {
  if (OffsetToLock >= static_cast<UINT>(size_) ||
      SizeToLock > static_cast<UINT>(size_) - OffsetToLock)
    return D3DERR_INVALIDCALL;

#ifdef DX8TO12_BUFFER_SHADOW
  // One uniform path for every buffer and every lock flag, exactly as the
  // working d3d8to11 port does. Everything below -- the speculative write
  // cache, the ring allocation, written_ranges_ -- is bypassed, and with it
  // the ring-vs-persistent divergence: a plain lock currently writes the
  // persistent resource while GetGpuPtr keeps handing draws the ring address,
  // so those bytes never reach a draw and are then overwritten from the ring
  // by PersistDynamicChanges at end of frame.
  return Buffer::Lock(OffsetToLock, SizeToLock, ppbData, Flags);
#else

  // Keep the dynamic path byte-for-byte consistent with Buffer::Lock.  In
  // particular, a zero-sized partial lock must end at the logical end of the
  // buffer; its previous whole-buffer interpretation overran the ring-backed
  // allocation after OffsetToLock.
  if (SizeToLock == 0) SizeToLock = static_cast<UINT>(size_) - OffsetToLock;

  const int offset = safe_cast<int>(OffsetToLock);
  int size_to_lock = safe_cast<int>(SizeToLock);

  // LOG(kLog) << "(Dynamic) Locking " << std::hex << this << " ("
  //           << Dx8::LockFlagToString(Flags) << ") offset " << OffsetToLock
  //           << " size " << size_to_lock << ". Buffer size " << std::dec <<
  //           size_
  //           << "\n";

  const bool is_discard = HasFlag(Flags, D3DLOCK_DISCARD);
  const bool is_nooverwrite = HasFlag(Flags, D3DLOCK_NOOVERWRITE);

  // Lock traffic for the F9 dump. The draw-side dump showed the game reaching
  // BaseVertexIndex in the thousands within a 256KB dynamic buffer, so which
  // path each lock takes -- and where its data actually lands -- is what
  // decides whether those vertices exist by the time the GPU reads them.
  if (device_->ui_dump_enabled()) {
    LOG(AixLog::Severity::error)
        << "LOCKDUMP buf=" << this << " size=" << size_
        << " offset=" << OffsetToLock << " lockSize=" << size_to_lock
        << " discard=" << is_discard << " nooverwrite=" << is_nooverwrite
        << " prevLockFrame=" << prev_lock_frame_
        << " curFrame=" << device_->CurrentFrame()
        << " hasSpec=" << has_speculative_write_
        << " specPersisted=" << is_speculative_write_persisted_
        << " ringOff=" << current_ring_alloc_.offset
        << " ringSize=" << current_ring_alloc_.size << "\n";
  }
  // const bool is_entire_buffer = size_to_lock == size_;

  if (is_nooverwrite && prev_lock_frame_ < device_->CurrentFrame()) {
    return Buffer::Lock(OffsetToLock, SizeToLock, ppbData, Flags);
  }
  if (!is_discard && !is_nooverwrite) {
    // Neither flag set: real D3D9 docs say this "behaves the same as if
    // usage were not dynamic" -- a plain, synchronized lock against the
    // buffer's own persistent resource, skipping the speculative-write/
    // ring-buffer fast paths below entirely. A real, if less common, usage
    // pattern -- hit in practice by GTA: Vice City.
#ifdef DX8TO12_ENABLE_MINDEBUG
    ReportPlainLockHazard(offset, size_to_lock);
#endif
    is_plain_lock_ = true;
    return Buffer::Lock(OffsetToLock, SizeToLock, ppbData, Flags);
  }

  // We're modifying the contents of the buffer. We have to persist the last
  // modification.
  device_->MarkBufferForPersist(this);

  prev_lock_frame_ = device_->CurrentFrame();
  if (is_discard) {
    ASSERT(offset == 0);
    // Any previous speculative write was either not used, or already
    // persisted by a call to GetGpuPtr -- either way it's dead now.
    current_ring_alloc_ = {};
    // Speculatively cache this write. Grow-only: never shrink and never
    // re-zero, since the caller is about to overwrite the whole range.
    if (kSkipDiscardZeroFill) {
      if (static_cast<int>(speculative_write_cache_.size()) < size_to_lock) {
        speculative_write_cache_.resize(size_to_lock);
      }
    } else {
      // Zero the whole locked range, as the original clear()+resize() did:
      // anything the app leaves unwritten then reads as zeroes rather than as
      // last frame's contents.
      speculative_write_cache_.assign(size_to_lock, 0);
    }
    has_speculative_write_ = true;
    speculative_write_size_ = size_to_lock;
    is_speculative_write_persisted_ = false;
    // But save a spot in the CSV heap for it.
    *ppbData = reinterpret_cast<BYTE*>(speculative_write_cache_.data());

    written_ranges_.ranges.clear();
    written_ranges_.insert({offset, size_to_lock});
  } else {
    ASSERT(is_nooverwrite);
    ASSERT(prev_lock_frame_ == device_->CurrentFrame());
    // This is a no overwrite.
    if (has_speculative_write_) {
      // Previous value was a discard. We now know we're appending data, so it
      // has to live in the ring where the append can reach it. If a draw
      // already forced it out via GetGpuPtr the allocation is there and is
      // full-size, so reuse it rather than allocating a second copy: appending
      // into a region a recorded draw already references is exactly what
      // D3DLOCK_NOOVERWRITE promises is safe.
      if (!is_speculative_write_persisted_) PersistSpeculativeWrite();
      has_speculative_write_ = false;
    }
    char* dest =
        device_->dynamic_ring_buffer()->GetCpuPtrFor(current_ring_alloc_) +
        offset;
    *ppbData = reinterpret_cast<BYTE*>(dest);
    written_ranges_.insert({offset, size_to_lock});
  }

  return S_OK;
#endif  // DX8TO12_BUFFER_SHADOW
}

#ifdef DX8TO12_ENABLE_MINDEBUG
namespace {
uint32_t g_plain_locks = 0;
uint32_t g_hazard_ring_then_plain = 0;
uint32_t g_hazard_plain_after_draw = 0;
uint64_t g_next_hazard_summary_frame = 0;
}  // namespace

void DynamicBuffer::ReportPlainLockHazard(int offset, int size) {
  const uint64_t frame = device_->CurrentFrame();
  // A plain lock while this frame's draws are reading the ring: the write
  // lands in the persistent resource, GetGpuPtr keeps returning the ring
  // address, and PersistDynamicChanges copies the ring back over it at the end
  // of the frame. These bytes can never reach a draw.
  const bool ring_then_plain =
      prev_lock_frame_ == frame && current_ring_alloc_.size > 0;
  // A plain lock after a draw this frame already took this buffer's address:
  // that draw is recorded but not yet submitted, and it reads exactly the
  // memory this lock is about to overwrite.
  const bool plain_after_draw = draw_frame_ == frame;

  // Emitted once per session on the very first plain lock, so a run with no
  // hazard lines can be told apart from a run where this never armed.
  if (g_plain_locks++ == 0) {
    WriteMindebugDiagnosticLine("LOCKHAZARD-ARMED frame=" +
                                std::to_string(frame) + "\n");
  }
  if (ring_then_plain) ++g_hazard_ring_then_plain;
  if (plain_after_draw) ++g_hazard_plain_after_draw;

  if ((ring_then_plain || plain_after_draw) && !logged_hazard_) {
    logged_hazard_ = true;
    std::ostringstream line;
    line << "LOCKHAZARD-FIRST buf=" << this << " size=" << size_ << " lock="
         << offset << "+" << size << " ringThenPlain=" << ring_then_plain
         << " plainAfterDraw=" << plain_after_draw
         << " prevLockFrame=" << prev_lock_frame_
         << " drawFrame=" << draw_frame_ << " frame=" << frame
         << " ringOff=" << current_ring_alloc_.offset
         << " ringSize=" << current_ring_alloc_.size << "\n";
    WriteMindebugDiagnosticLine(line.str());
  }
  if (frame >= g_next_hazard_summary_frame) {
    g_next_hazard_summary_frame = frame + 600;
    std::ostringstream line;
    line << "LOCKHAZARD-TOTALS frame=" << frame
         << " plainLocks=" << g_plain_locks
         << " ringThenPlain=" << g_hazard_ring_then_plain
         << " plainAfterDraw=" << g_hazard_plain_after_draw << "\n";
    WriteMindebugDiagnosticLine(line.str());
  }
}
#endif  // DX8TO12_ENABLE_MINDEBUG

HRESULT STDMETHODCALLTYPE DynamicBuffer::Unlock() noexcept {
#ifdef DX8TO12_BUFFER_SHADOW
  return Buffer::Unlock();
#else
  if (is_plain_lock_) {
    is_plain_lock_ = false;
    return Buffer::Unlock();
  }
  if (prev_lock_frame_ < device_->CurrentFrame()) return Buffer::Unlock();
  return S_OK;
#endif  // DX8TO12_BUFFER_SHADOW
}

bool DynamicBuffer::IsRangeWrittenThisFrame(int offset, int size) {
#ifdef DX8TO12_BUFFER_SHADOW
  // The shadow always holds every byte the app ever wrote, and the whole
  // buffer is zero-initialised before that, so there is no uninitialised
  // memory for this check to find.
  (void)offset;
  (void)size;
  return true;
#else
  // Not streaming through the ring this frame: the draw reads the buffer's own
  // persistent resource, which still holds earlier frames' contents. Whether
  // *that* is safe is a separate question (the CPU writes it with no
  // synchronisation against a GPU that may still be reading it), but it is not
  // uninitialised memory, so it is not what this check is looking for.
  if (prev_lock_frame_ < device_->CurrentFrame()) return true;
  if (current_ring_alloc_.size == 0) return true;
  // ranges are kept sorted and coalesced by RangeSet, so the only candidate is
  // the last range starting at or before `offset`. Binary search keeps this
  // cheap enough to run on every draw rather than only under the F9 dump --
  // which matters, because the dump's own cost is what has been hiding the
  // bug.
  const auto& ranges = written_ranges_.ranges;
  auto iter = std::upper_bound(
      ranges.begin(), ranges.end(), offset,
      [](int value, const RangeSet::Range& r) { return value < r.offset; });
  if (iter == ranges.begin()) return false;
  --iter;
  return offset >= iter->offset &&
         offset + size <= iter->offset + iter->size;
#endif  // DX8TO12_BUFFER_SHADOW
}

std::string DynamicBuffer::DebugState() {
  std::ostringstream s;
  const bool on_ring = prev_lock_frame_ == device_->CurrentFrame() &&
                       current_ring_alloc_.size > 0;
  s << (on_ring ? "ring" : "persistent") << " ringOff="
    << current_ring_alloc_.offset << " ringSize=" << current_ring_alloc_.size
    << " prevLockFrame=" << prev_lock_frame_
    << " curFrame=" << device_->CurrentFrame()
    << " hasSpec=" << has_speculative_write_
    << " specSize=" << speculative_write_size_
    << " ranges=" << written_ranges_.ranges.size();
  // The range covering the vertices the draw is about to read, which is what
  // says whether this frame put them there or they are last frame's leftovers.
  for (size_t i = 0; i < written_ranges_.ranges.size() && i < 3; ++i) {
    s << " r" << i << "=[" << written_ranges_.ranges[i].offset << "+"
      << written_ranges_.ranges[i].size << "]";
  }
  return s.str();
}

const char* DynamicBuffer::DebugCpuPtr(int offset, int size) {
#ifdef DX8TO12_BUFFER_SHADOW
  // The shadow is what the draw will read (the resource is a flushed copy of
  // it), so the base implementation is already correct here.
  return Buffer::DebugCpuPtr(offset, size);
#else
  // A D3DLOCK_DISCARD write that no draw has forced out yet still lives in the
  // CPU-side cache; the ring copy only happens on the next GetGpuPtr. Since
  // this runs *before* PrepareDrawCall, that is the normal state for the first
  // draw after a discard -- and reading anywhere else there means reading the
  // previous frame's leftovers instead of the vertices the GPU will use, which
  // is exactly how this check manufactured a fake full-screen quad.
  if (has_speculative_write_ && !is_speculative_write_persisted_) {
    if (offset < 0 || size < 0 || offset + size > speculative_write_size_) {
      return nullptr;
    }
    return speculative_write_cache_.data() + offset;
  }
  // Streaming through the ring this frame: that is the memory the draw reads.
  if (prev_lock_frame_ == device_->CurrentFrame() &&
      current_ring_alloc_.size > 0 && offset >= 0 && size >= 0 &&
      offset + size <= current_ring_alloc_.size) {
    return device_->dynamic_ring_buffer()->GetCpuPtrFor(current_ring_alloc_) +
           offset;
  }
  // Otherwise the draw reads the buffer's own persistent resource, and so
  // should we. Returning nullptr here left the vertex scan blind on every
  // draw that took the D3DLOCK_NOOVERWRITE-without-discard path, which is
  // most of them -- and a scan that silently examines nothing looks exactly
  // like a scan that found nothing wrong.
  return Buffer::DebugCpuPtr(offset, size);
#endif  // DX8TO12_BUFFER_SHADOW
}

void DynamicBuffer::PersistSpeculativeWrite() {
  // Always reserve the *whole* buffer, never just the bytes the D3DLOCK_DISCARD
  // happened to lock. Draws bind a vertex buffer view spanning the full buffer
  // size (they have to -- D3D8's BaseVertexIndex is an index into the entire
  // buffer, and GTA: Vice City's 2D text batching reaches BaseVertexIndex in
  // the thousands), so a shorter allocation leaves that view pointing past its
  // own region and straight into whatever else the ring is holding. GTA
  // discards only 0x2a00 bytes of a 0x40000 buffer and then D3DLOCK_NOOVERWRITE
  // appends out to offset 0x3f070, so nearly the entire view sat over other
  // allocations' memory -- correct textures, garbage positions, and different
  // garbage every frame depending on where the ring pointer had got to. That is
  // the whole-font-atlas-stretched-over-the-menu glitch, and it is why slowing
  // the CPU down (a graphics debugger, or just this file's own logging) shuffled
  // the ring enough to hide it.
  current_ring_alloc_ = device_->dynamic_ring_buffer()->Allocate(size_);
  char* dest =
      device_->dynamic_ring_buffer()->GetCpuPtrFor(current_ring_alloc_);
  memcpy(dest, speculative_write_cache_.data(),
         static_cast<size_t>(speculative_write_size_));
  // Everything the discard did not cover has to be *defined*, not merely
  // reserved. D3DLOCK_DISCARD says the untouched remainder is undefined, so
  // zeroes are a legal thing to put there -- and unlike stale ring bytes they
  // collapse into degenerate triangles instead of screen-filling noise if the
  // app ever draws from a region it did not rewrite this frame.
  if (size_ > speculative_write_size_) {
    memset(dest + speculative_write_size_, 0,
           static_cast<size_t>(size_ - speculative_write_size_));
  }
  prev_lock_frame_ = device_->CurrentFrame();

  is_speculative_write_persisted_ = true;
}

GpuPtr DynamicBuffer::GetGpuPtr() {
#ifdef DX8TO12_ENABLE_MINDEBUG
  // GetGpuPtr is a Buffer's only per-draw touch point, so this is where "a
  // draw has now committed to this buffer's contents" becomes true. See
  // ReportPlainLockHazard.
  draw_frame_ = device_->CurrentFrame();
#endif
#ifdef DX8TO12_BUFFER_SHADOW
  // Flushes the pending dirty range into the command list and returns the
  // buffer's own address; draws never read the ring in this mode.
  return Buffer::GetGpuPtr();
#else
  if (!is_speculative_write_persisted_ && has_speculative_write_) {
    // Persist the speculative write.
    PersistSpeculativeWrite();
  } else if (prev_lock_frame_ < device_->CurrentFrame()) {
    // std::dec after the pointer: AixLog's Log singleton hijacks std::clog's
    // streambuf (aixlog.hpp), so every LOG(...) call -- even one filtered
    // out entirely by severity, since that filtering happens at sink-dispatch
    // time, after the << chain has already run (see CLAUDE.md's TRACE_ENTRY
    // note) -- writes through the *same* shared stream object. std::hex left
    // unreset here silently flips every *other* LOG() call in the process,
    // on any thread, into hex formatting until something else happens to
    // reset it -- this trace-level line doesn't even need to be seen for the
    // damage to happen. Confirmed as the actual cause of "frame=cd"-style
    // garbage in an unrelated decimal-only diagnostic elsewhere in this file.
    LOG(kLog) << "Using backing buffer for " << std::hex << this << std::dec
             << ".\n";
    return Buffer::GetGpuPtr();  // Boooo.
  }

  return device_->dynamic_ring_buffer()->GetGpuPtrFor(current_ring_alloc_);
#endif  // DX8TO12_BUFFER_SHADOW
}

bool DynamicBuffer::GetCurrentRingAllocation(
    DynamicRingBuffer::Allocation* allocation) {
#ifdef DX8TO12_BUFFER_SHADOW
  // Draws read the buffer's own resource in this mode, so there is no ring
  // allocation for the x64 RT helper to mirror. Reporting none makes it skip
  // this buffer rather than snapshot memory nothing is drawing from.
  (void)allocation;
  return false;
#else
  GetGpuPtr();
  if (current_ring_alloc_.frame != device_->CurrentFrame() ||
      current_ring_alloc_.size <= 0) {
    return false;
  }
  *allocation = current_ring_alloc_;
  return true;
#endif  // DX8TO12_BUFFER_SHADOW
}

void DynamicBuffer::PersistDynamicChanges() {
#ifdef DX8TO12_BUFFER_SHADOW
  // Nothing to persist: every write already reached the buffer's own resource
  // through the command list, in order, at draw time. Nothing ever calls this
  // anyway in this mode -- Lock no longer calls MarkBufferForPersist -- but a
  // stale queue entry from before a mode switch must not fall into the
  // ring-copy path below and overwrite the resource with an empty allocation.
  return;
#else
  // See the std::dec comment on the "Using backing buffer" LOG above --
  // same shared-stream hazard, same fix.
  LOG(kLog) << "Persisting changes for " << std::hex << this << std::dec
           << "\n";
  // Make sure any speculative writes are committed.
  GetGpuPtr();
  ASSERT(current_ring_alloc_.frame == device_->CurrentFrame());
  ASSERT(current_ring_alloc_.size > 0);
  // Multiple non-contiguous written ranges are a real, legitimate case (e.g.
  // repeated D3DLOCK_NOOVERWRITE appends to disjoint sub-regions within one
  // frame) -- RangeSet::insert only coalesces adjacent/overlapping ranges,
  // so ranges.size() > 1 is expected here, not a bug. The loop below already
  // handles any number of ranges correctly.
  // Hoist the state transitions out of the loop: Device::CopyBuffer
  // transitions to COPY_DEST and back to COMMON around each individual copy,
  // so calling it per range emitted 2N barriers for N ranges when one pair
  // covers the whole batch.
  ID3D12Resource* const src =
      device_->dynamic_ring_buffer()->GetBackingResource();
  // The shared ring was used as a vertex/index/constant buffer by the draws
  // recorded above. CopyBufferRegion requires it to be COPY_SOURCE here.
  // Omitting this barrier makes the persisted VB/IB contents undefined and
  // can make a whole textured mesh disappear on a later frame.
  device_->TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  device_->TransitionBuffer(this, D3D12_RESOURCE_STATE_COPY_DEST);
  for (auto [offset, size] : written_ranges_.ranges) {
    // Destination offset is the range's own offset, not 0. written_ranges_
    // holds offsets within this buffer, and the ring allocation mirrors that
    // layout, so a range written at offset N belongs at offset N here too.
    // Copying every range to 0 piled them all on top of each other at the
    // start of the buffer: with a single range starting at 0 (the common
    // case) that happened to be right, but a game appending several
    // D3DLOCK_NOOVERWRITE batches into one buffer -- which is exactly how
    // GTA: Vice City batches 2D text, reaching BaseVertexIndex in the
    // thousands -- got every batch after the first written to the wrong
    // place, and read garbage back on any later frame that drew from this
    // buffer without re-locking it.
    device_->cmd_list()->CopyBufferRegion(
        resource(), static_cast<UINT64>(offset), src,
        static_cast<UINT64>(current_ring_alloc_.offset + offset),
        static_cast<UINT64>(size));
  }
  device_->TransitionBuffer(this, D3D12_RESOURCE_STATE_COMMON);
  written_ranges_.ranges.clear();
  current_ring_alloc_ = {};
#endif  // DX8TO12_BUFFER_SHADOW
}
}  // namespace Dx8to12
