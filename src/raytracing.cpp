#include "raytracing.h"

#include <algorithm>
#include <cstring>

#include "aixlog.hpp"
#include "buffer.h"
#include "device.h"

namespace Dx8to12 {
namespace {

UINT64 AlignRaytracingSize(UINT64 value, UINT64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

D3D12_HEAP_PROPERTIES DefaultHeapProperties() {
  return {.Type = D3D12_HEAP_TYPE_DEFAULT,
          .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
          .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
          .CreationNodeMask = 1,
          .VisibleNodeMask = 1};
}

D3D12_HEAP_PROPERTIES UploadHeapProperties() {
  return {.Type = D3D12_HEAP_TYPE_UPLOAD,
          .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
          .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
          .CreationNodeMask = 1,
          .VisibleNodeMask = 1};
}

// Direct3D 8 stores affine transforms for row-vector multiplication, while a
// DXR instance descriptor is a 3x4 row-major matrix for column vectors.  The
// transpose keeps the eventual ray-space transform consistent with the
// fixed-function world matrix.  It has no visual effect in this phase.
void CopyWorldTransform(const D3DMATRIX& world, float (&out)[3][4]) {
  out[0][0] = world._11;
  out[0][1] = world._21;
  out[0][2] = world._31;
  out[0][3] = world._41;
  out[1][0] = world._12;
  out[1][1] = world._22;
  out[1][2] = world._32;
  out[1][3] = world._42;
  out[2][0] = world._13;
  out[2][1] = world._23;
  out[2][2] = world._33;
  out[2][3] = world._43;
}

}  // namespace

RaytracingScene::RaytracingScene(Device* device) : device_(device) {}

RaytracingScene::~RaytracingScene() {
  if (tlas_srv_handle_.ptr != 0) device_->srv_heap().Free(tlas_srv_handle_);
}

size_t RaytracingScene::BlasKeyHash::operator()(const BlasKey& key) const {
  size_t hash = std::hash<Buffer*>{}(key.vertex_buffer);
  auto combine = [&hash](size_t value) {
    hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
  };
  combine(std::hash<Buffer*>{}(key.index_buffer));
  combine(key.vertex_stride);
  combine(key.start_index);
  combine(key.index_count);
  combine(key.base_vertex);
  return hash;
}

bool RaytracingScene::EnsureDevice5() {
  if (device5_) return true;
  // The public capability also includes the x64 helper backend. This class,
  // however, records native DXR commands into the x86 device's command list
  // and must therefore be gated by the native tier only.
  if (!device_->native_raytracing_supported()) return false;
  if (FAILED(device_->device()->QueryInterface(IID_PPV_ARGS(device5_.GetForInit())))) {
    LOG(AixLog::Severity::error)
        << "RTSCENE: DXR was advertised but ID3D12Device5 is unavailable; "
           "disabling scene tracking.\n";
    return false;
  }
  return true;
}

bool RaytracingScene::EnsureBuffer(ComPtr<ID3D12Resource>& buffer,
                                   UINT64& capacity, UINT64 required_size,
                                   D3D12_RESOURCE_STATES initial_state,
                                   const wchar_t* name) {
  if (buffer && capacity >= required_size) return true;
  const UINT64 size = AlignRaytracingSize(required_size,
                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
  D3D12_RESOURCE_DESC desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Alignment = 0,
      .Width = size,
      .Height = 1,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1, .Quality = 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = initial_state == D3D12_RESOURCE_STATE_GENERIC_READ
                   ? D3D12_RESOURCE_FLAG_NONE
                   : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  const D3D12_HEAP_PROPERTIES heap =
      initial_state == D3D12_RESOURCE_STATE_GENERIC_READ ? UploadHeapProperties()
                                                         : DefaultHeapProperties();
  ASSERT_HR(device_->device()->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
      IID_PPV_ARGS(buffer.GetForInit())));
  buffer->SetName(name);
  capacity = size;
  return true;
}

void RaytracingScene::RecordIndexedTriangleList(
    InternalPtr<Buffer> vertex_buffer, InternalPtr<Buffer> index_buffer,
    UINT vertex_stride, UINT start_index, UINT index_count, UINT base_vertex,
    const D3DMATRIX& world, UINT material_srv_index) {
  // DXR triangles always consume float3 positions from byte 0.  The actual
  // fixed-function declaration will be carried in a later phase; rejecting
  // smaller streams now avoids invalid GPU addresses for 2D/UI formats.
  if (!vertex_buffer || !index_buffer || vertex_stride < sizeof(float) * 3 ||
      index_count < 3) {
    return;
  }
  draws_.push_back({std::move(vertex_buffer), std::move(index_buffer),
                    vertex_stride, start_index, index_count, base_vertex,
                    world, material_srv_index});
}

bool RaytracingScene::BuildBlas(Draw& draw, BlasEntry* entry,
                                ID3D12GraphicsCommandList4* command_list) {
  Buffer* const vb = draw.vertex_buffer.Get();
  Buffer* const ib = draw.index_buffer.Get();
  const uint64_t vertex_bytes = vb->resource_desc().Width;
  if (draw.base_vertex >= vertex_bytes / draw.vertex_stride) return false;

  D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
  geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  geometry.Triangles.Transform3x4 = 0;
  geometry.Triangles.IndexFormat = ib->index_buffer_fmt();
  geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  geometry.Triangles.IndexCount = draw.index_count;
  geometry.Triangles.VertexCount =
      static_cast<UINT>(vertex_bytes / draw.vertex_stride - draw.base_vertex);
  geometry.Triangles.IndexBuffer =
      ib->GetGpuPtr() + static_cast<UINT64>(draw.start_index) *
                            (geometry.Triangles.IndexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2);
  geometry.Triangles.VertexBuffer.StartAddress =
      vb->GetGpuPtr() + static_cast<UINT64>(draw.base_vertex) * draw.vertex_stride;
  geometry.Triangles.VertexBuffer.StrideInBytes = draw.vertex_stride;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.NumDescs = 1;
  inputs.pGeometryDescs = &geometry;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
  device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  if (info.ResultDataMaxSizeInBytes == 0) return false;
  EnsureBuffer(entry->result, entry->result_capacity, info.ResultDataMaxSizeInBytes,
               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
               L"dx8to12_blas");
  entry->vertex_generation = vb->content_generation();
  entry->index_generation = ib->content_generation();
  entry->vertex_buffer = draw.vertex_buffer;
  entry->index_buffer = draw.index_buffer;
  EnsureBuffer(blas_scratch_, blas_scratch_capacity_, info.ScratchDataSizeInBytes,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"dx8to12_blas_scratch");

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
  build.Inputs = inputs;
  build.DestAccelerationStructureData = entry->result->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = blas_scratch_->GetGPUVirtualAddress();
  command_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  const D3D12_RESOURCE_BARRIER barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .UAV = {.pResource = entry->result.get()}};
  command_list->ResourceBarrier(1, &barrier);
  // Every BLAS build in this command list reuses blas_scratch_.  The result
  // barrier above makes this BLAS visible to the TLAS; this separate barrier
  // orders the scratch writes before the next BLAS build aliases that memory.
  const D3D12_RESOURCE_BARRIER scratch_barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .UAV = {.pResource = blas_scratch_.get()}};
  command_list->ResourceBarrier(1, &scratch_barrier);
  return true;
}

void RaytracingScene::UpdateTlasSrv() {
  if (tlas_srv_handle_.ptr == 0) tlas_srv_handle_ = device_->srv_heap().Allocate();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
  desc.RaytracingAccelerationStructure.Location = tlas_result_->GetGPUVirtualAddress();
  device_->device()->CreateShaderResourceView(nullptr, &desc, tlas_srv_handle_);
}

void RaytracingScene::BuildForFrame() {
  if (draws_.empty()) return;
  if (!EnsureDevice5()) {
    draws_.clear();
    return;
  }

  ComPtr<ID3D12GraphicsCommandList4> command_list;
  if (FAILED(device_->cmd_list()->QueryInterface(
          IID_PPV_ARGS(command_list.GetForInit())))) {
    LOG(AixLog::Severity::error) << "RTSCENE: command list does not expose v4.\n";
    draws_.clear();
    return;
  }

  uint32_t rebuilt_blas = 0;
  struct InstanceSource {
    const Draw* draw;
    BlasEntry* blas;
  };
  std::vector<InstanceSource> instances;
  instances.reserve(draws_.size());
  for (Draw& draw : draws_) {
    BlasKey key{draw.vertex_buffer.Get(), draw.index_buffer.Get(), draw.vertex_stride,
                draw.start_index, draw.index_count, draw.base_vertex};
    BlasEntry& entry = blas_cache_[key];
    if (!entry.result || entry.vertex_generation != draw.vertex_buffer->content_generation() ||
        entry.index_generation != draw.index_buffer->content_generation()) {
      if (!BuildBlas(draw, &entry, command_list.get())) continue;
      ++rebuilt_blas;
    }
    instances.push_back({&draw, &entry});
  }
  if (instances.empty()) return;

  const UINT64 instance_bytes =
      static_cast<UINT64>(instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
  EnsureBuffer(instance_upload_, instance_upload_capacity_, instance_bytes,
               D3D12_RESOURCE_STATE_GENERIC_READ, L"dx8to12_tlas_instances");
  D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
  ASSERT_HR(instance_upload_->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
  for (size_t i = 0; i < instances.size(); ++i) {
    D3D12_RAYTRACING_INSTANCE_DESC& instance = mapped[i];
    std::memset(&instance, 0, sizeof(instance));
    CopyWorldTransform(instances[i].draw->world, instance.Transform);
    instance.InstanceID = static_cast<UINT>(i);
    instance.InstanceContributionToHitGroupIndex = 0;
    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    instance.AccelerationStructure =
        instances[i].blas->result->GetGPUVirtualAddress();
  }
  instance_upload_->Unmap(0, nullptr);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.NumDescs = static_cast<UINT>(instances.size());
  inputs.InstanceDescs = instance_upload_->GetGPUVirtualAddress();
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
  device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  EnsureBuffer(tlas_result_, tlas_result_capacity_, info.ResultDataMaxSizeInBytes,
               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
               L"dx8to12_tlas");
  EnsureBuffer(tlas_scratch_, tlas_scratch_capacity_, info.ScratchDataSizeInBytes,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"dx8to12_tlas_scratch");
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
  build.Inputs = inputs;
  build.DestAccelerationStructureData = tlas_result_->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = tlas_scratch_->GetGPUVirtualAddress();
  command_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  const D3D12_RESOURCE_BARRIER barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .UAV = {.pResource = tlas_result_.get()}};
  command_list->ResourceBarrier(1, &barrier);
  UpdateTlasSrv();

  static uint32_t log_frames = 0;
  if ((log_frames++ % 120) == 0) {
    LOG(INFO) << "RTSCENE: frame=" << device_->CurrentFrame()
              << " instances=" << instances.size()
              << " blas_rebuilt=" << rebuilt_blas
              << " blas_cached=" << blas_cache_.size() << "\n";
  }
  draws_.clear();
}

}  // namespace Dx8to12
