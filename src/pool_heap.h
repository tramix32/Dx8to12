#pragma once

#include <d3d12.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "util.h"

namespace Dx8to12 {
class Device;

class DescriptorPoolHeap {
 public:
  // Initializing not really needed since heap_ is null.
  DescriptorPoolHeap()
      : cpu_start_({}), gpu_start_({}), increment_(0), num_descriptors_(0) {}

  DescriptorPoolHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
                     int num_descriptors);

  D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
  void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void FreeAll();

  // Descriptor slots are not reusable the moment their owner is destroyed:
  // command lists already submitted still name them, and the GPU resolves a
  // descriptor when it *executes* the draw, not when the draw was recorded.
  // Free() therefore only parks a slot, stamped with the frame in flight;
  // ReleaseCompleted() hands it back to the free list once the GPU has
  // finished every frame that could still reference it.
  void SetCurrentFrame(uint64_t frame) { current_frame_ = frame; }
  void ReleaseCompleted(uint64_t completed_frame);

  D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandleFor(
      D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle) const;
  // Stable slot number for a descriptor allocated by this heap.  Scene
  // tracking uses this as an opaque material key; it never dereferences it
  // outside the heap that produced it.
  UINT GetIndexFor(D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle) const;

  ID3D12DescriptorHeap* heap() { return heap_.get(); }

 private:
  ComPtr<ID3D12DescriptorHeap> heap_;
  std::vector<intptr_t> free_list_;
  // Slots whose owner is gone but which the GPU may still be reading.
  std::vector<std::pair<uint64_t, intptr_t>> pending_free_;
  uint64_t current_frame_ = 0;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_ = {};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_ = {};
  int increment_;
  int num_descriptors_;
};
}  // namespace Dx8to12
