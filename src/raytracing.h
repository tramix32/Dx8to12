#pragma once

#include <d3d12.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "d3d8.h"
#include "util.h"

namespace Dx8to12 {
class Buffer;
class Device;

// Phase 2 DXR scene collector.  It deliberately owns no ray-generation
// shaders and never changes the graphics pipeline: its only output is a TLAS
// that later lighting modes can consume.
class RaytracingScene {
 public:
  explicit RaytracingScene(Device* device);
  ~RaytracingScene();

  RaytracingScene(const RaytracingScene&) = delete;
  RaytracingScene& operator=(const RaytracingScene&) = delete;

  void RecordIndexedTriangleList(InternalPtr<Buffer> vertex_buffer,
                                 InternalPtr<Buffer> index_buffer,
                                 UINT vertex_stride, UINT start_index,
                                 UINT index_count, UINT base_vertex,
                                 const D3DMATRIX& world,
                                 UINT material_srv_index);
  void DiscardFrame() { draws_.clear(); }
  // Called immediately before mod render callbacks / Present.  Recording the
  // build here guarantees all source draws for this frame have been observed,
  // while leaving a valid TLAS for later callback-driven RT consumers.
  void BuildForFrame();

 private:
  struct Draw {
    InternalPtr<Buffer> vertex_buffer;
    InternalPtr<Buffer> index_buffer;
    UINT vertex_stride = 0;
    UINT start_index = 0;
    UINT index_count = 0;
    UINT base_vertex = 0;
    D3DMATRIX world = {};
    UINT material_srv_index = UINT_MAX;
  };

  struct BlasKey {
    Buffer* vertex_buffer = nullptr;
    Buffer* index_buffer = nullptr;
    UINT vertex_stride = 0;
    UINT start_index = 0;
    UINT index_count = 0;
    UINT base_vertex = 0;

    bool operator==(const BlasKey&) const = default;
  };
  struct BlasKeyHash {
    size_t operator()(const BlasKey& key) const;
  };
  struct BlasEntry {
    // Keeps cache keys valid for the lifetime of their BLAS.  Static level
    // buffers are expected to outlive a level; eviction policy comes with
    // the later streaming phase.
    InternalPtr<Buffer> vertex_buffer;
    InternalPtr<Buffer> index_buffer;
    ComPtr<ID3D12Resource> result;
    UINT64 result_capacity = 0;
    uint64_t vertex_generation = 0;
    uint64_t index_generation = 0;
  };

  bool EnsureDevice5();
  bool EnsureBuffer(ComPtr<ID3D12Resource>& buffer, UINT64& capacity,
                    UINT64 required_size,
                    D3D12_RESOURCE_STATES initial_state,
                    const wchar_t* name);
  bool BuildBlas(Draw& draw, BlasEntry* entry,
                 ID3D12GraphicsCommandList4* command_list);
  void UpdateTlasSrv();

  Device* device_ = nullptr;
  ComPtr<ID3D12Device5> device5_;
  std::vector<Draw> draws_;
  std::unordered_map<BlasKey, BlasEntry, BlasKeyHash> blas_cache_;

  ComPtr<ID3D12Resource> blas_scratch_;
  UINT64 blas_scratch_capacity_ = 0;
  ComPtr<ID3D12Resource> tlas_result_;
  UINT64 tlas_result_capacity_ = 0;
  ComPtr<ID3D12Resource> tlas_scratch_;
  UINT64 tlas_scratch_capacity_ = 0;
  ComPtr<ID3D12Resource> instance_upload_;
  UINT64 instance_upload_capacity_ = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE tlas_srv_handle_ = {};
};

}  // namespace Dx8to12
