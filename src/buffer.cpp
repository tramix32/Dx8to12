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
    if (static_cast<int>(speculative_write_cache_.size()) < size_to_lock) {
      speculative_write_cache_.resize(size_to_lock);
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
      // Previous value was a discard. We now know what we're appending data. So
      // allocate the entire buffer size and copy the previous value.
      PersistSpeculativeWrite(size_);
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

void DynamicBuffer::PersistSpeculativeWrite(int alloc_size) {
  // if (use_cbv_) alloc_size = AlignUp(alloc_size, 256);
  current_ring_alloc_ = device_->dynamic_ring_buffer()->Allocate(alloc_size);
  char* dest =
      device_->dynamic_ring_buffer()->GetCpuPtrFor(current_ring_alloc_);
  memcpy(dest, speculative_write_cache_.data(),
         static_cast<size_t>(speculative_write_size_));
  prev_lock_frame_ = device_->CurrentFrame();

  is_speculative_write_persisted_ = true;
}

GpuPtr DynamicBuffer::GetGpuPtr() {
  if (!is_speculative_write_persisted_ && has_speculative_write_) {
    // Persist the speculative write.
    PersistSpeculativeWrite(speculative_write_size_);
  } else if (prev_lock_frame_ < device_->CurrentFrame()) {
    LOG(kLog) << "Using backing buffer for " << std::hex << this << ".\n";
    return Buffer::GetGpuPtr();  // Boooo.
  }

  return device_->dynamic_ring_buffer()->GetGpuPtrFor(current_ring_alloc_);
}

void DynamicBuffer::PersistDynamicChanges() {
  LOG(kLog) << "Persisting changes for " << std::hex << this << "\n";
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
    device_->cmd_list()->CopyBufferRegion(
        resource(), 0, src,
        static_cast<UINT64>(current_ring_alloc_.offset + offset),
        static_cast<UINT64>(size));
  }
  device_->TransitionBuffer(this, D3D12_RESOURCE_STATE_COMMON);
  written_ranges_.ranges.clear();
  current_ring_alloc_ = {};
}
}  // namespace Dx8to12