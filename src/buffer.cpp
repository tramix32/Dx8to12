#include "buffer.h"

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
                                .Size = safe_cast<UINT>(size_),
                                .FVF = fvf_};
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Buffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
  *pDesc = D3DINDEXBUFFER_DESC{.Format = DXGIToD3DFormat(index_buffer_fmt_),
                               .Type = D3DRTYPE_INDEXBUFFER,
                               .Usage = static_cast<DWORD>(usage_),
                               .Pool = d3d8_pool_,
                               .Size = safe_cast<UINT>(size_)};
  return S_OK;
}

void Buffer::InitAsBuffer(Device* device, size_t size_in_bytes,
                          Dx8::Usage usage, D3DPOOL pool) {
  ASSERT(pool != D3DPOOL_SCRATCH);
  size_in_bytes = AlignUp(size_in_bytes, 256);
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
  ASSERT(OffsetToLock <= INT32_MAX);
  ASSERT(SizeToLock <= INT32_MAX);
  ASSERT(!HasFlag(Flags, D3DLOCK_DISCARD));
  ASSERT((int)SizeToLock <= size_);

  if (SizeToLock == 0) SizeToLock = size_;

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
}

HRESULT STDMETHODCALLTYPE Buffer::Unlock() {
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
  return S_OK;
}

void Buffer::PersistDynamicChanges() {
  FAIL("Unexpected dynamic change persist in static buffer.");
}

GpuPtr Buffer::GetGpuPtr() { return resource()->GetGPUVirtualAddress(); }

// BIG TODO: Persist dynamic buffers at the end of the frame in case they are
// read the next frame.
HRESULT STDMETHODCALLTYPE DynamicBuffer::Lock(UINT OffsetToLock,
                                              UINT SizeToLock, BYTE** ppbData,
                                              DWORD Flags) noexcept {
  ASSERT(OffsetToLock <= INT32_MAX);
  ASSERT(SizeToLock <= INT32_MAX);

  if (SizeToLock == 0) SizeToLock = size_;

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
}

HRESULT STDMETHODCALLTYPE DynamicBuffer::Unlock() noexcept {
  if (is_plain_lock_) {
    is_plain_lock_ = false;
    return Buffer::Unlock();
  }
  if (prev_lock_frame_ < device_->CurrentFrame()) return Buffer::Unlock();
  return S_OK;
}

bool DynamicBuffer::IsRangeWrittenThisFrame(int offset, int size) {
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
}

void DynamicBuffer::PersistDynamicChanges() {
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
}
}  // namespace Dx8to12