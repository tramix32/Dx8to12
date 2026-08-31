#pragma once

#include <d3d12.h>

#include <climits>
#include <cstdint>
#include <deque>

#include "util.h"
#include "utils/dx_utils.h"

interface ID3D12Device;
interface ID3D12Resource;

namespace Dx8to12 {
class Device;

// Simple ring buffer that needs to be reset each frame.
class DynamicRingBuffer {
 public:
  struct Allocation {
    uint64_t frame;
    int offset;
    int size;
  };

  // Initializes current_frame to 1.
  DynamicRingBuffer(ID3D12Device* device, size_t size);
  ~DynamicRingBuffer();

  void SetCurrentFrame(uint64_t frame);
  void HasCompletedFrame(uint64_t frame);

  Allocation Allocate(
      size_t num_bytes,
      uint32_t alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  // Same, but reports exhaustion instead of FAIL()ing. For callers that have a
  // correct (if less desirable) fallback and would rather degrade than take
  // the whole process down -- the ring only reclaims space at frame
  // granularity, so a single frame doing unusually heavy streaming can run it
  // out even though nothing is wrong.
  bool TryAllocate(size_t num_bytes, uint32_t alignment, Allocation* out);
  char* GetCpuPtrFor(Allocation offset);
  GpuPtr GetGpuPtrFor(Allocation offset);

  ID3D12Resource* GetBackingResource() { return buffer_.get(); }
  D3D12_RESOURCE_STATES current_state() const { return current_state_; }
  void set_state(D3D12_RESOURCE_STATES state) { current_state_ = state; }

 private:
  ComPtr<ID3D12Resource> buffer_;
  char* cpu_ptr_;
  GpuPtr gpu_ptr_;

  const size_t max_size_;
  size_t head_;
  size_t tail_;

  std::deque<std::pair<uint64_t, size_t>> frame_heads_;
  uint64_t current_frame_ = 0;
  D3D12_RESOURCE_STATES current_state_ = D3D12_RESOURCE_STATE_COMMON;

  const uint32_t min_align_ = 256;

  // DIAGNOSTIC: head_/tail_ arithmetic is supposed to make two live
  // allocations' byte ranges overlap impossible, but this exact class of bug
  // has bitten this file before (see the HasCompletedFrame comment) and,
  // per that comment, is a timing-sensitive race that hides under a
  // debugger or heavy logging -- exactly the conditions every earlier
  // RenderDoc-driven audit of this bug ran under. Rather than trust the
  // arithmetic, verify it directly: track every allocation not yet known to
  // be GPU-complete and flag any new one whose byte range overlaps a still
  // "live" one, which the arithmetic should make structurally impossible.
#ifdef DX8TO12_ENABLE_VALIDATION
  std::deque<Allocation> live_allocs_;
  void CheckForOverlap(const Allocation& alloc);
#endif
};
}  // namespace Dx8to12
