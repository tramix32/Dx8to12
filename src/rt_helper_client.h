#pragma once

#include <windows.h>
#include "d3d8.h"

#include <memory>
#include "util.h"

namespace Dx8to12 {
class Device;
class Buffer;
class DynamicBuffer;

// Owns the x64 helper process and the H1 handshake objects.  It does not
// export geometry or alter rendering yet; failed startup is intentionally a
// non-fatal capability result.
class RtHelperClient {
 public:
  explicit RtHelperClient(Device* device);
  ~RtHelperClient();

  RtHelperClient(const RtHelperClient&) = delete;
  RtHelperClient& operator=(const RtHelperClient&) = delete;

  bool Start();
  // H2: record the x86 half of a one-shot shared-buffer/fence test.  Called
  // while the normal command list is open; completion is polled later.
  void BeginSmokeTest();
  void OnX86Submission();
  void PollSmokeTest();
  void ObserveIndexedDraw(D3DPRIMITIVETYPE primitive_type, const Buffer* vertex_buffer,
                          const Buffer* index_buffer);
  void RecordStaticTriangle(Buffer* vertex_buffer, Buffer* index_buffer,
                            UINT vertex_stride, UINT start_index, UINT index_count,
                            UINT base_vertex, UINT min_vertex_index, UINT num_vertices,
                            const D3DMATRIX& world,
                            UINT material_srv_index, UINT index_format,
                            UINT normal_byte_offset);
  void RecordDynamicTriangle(DynamicBuffer* vertex_buffer, DynamicBuffer* index_buffer,
                             UINT vertex_stride, UINT start_index, UINT index_count,
                             UINT base_vertex, UINT min_vertex_index, UINT num_vertices,
                             const D3DMATRIX& world,
                             UINT material_srv_index, UINT index_format,
                             UINT normal_byte_offset);
  bool ready() const { return ready_; }
  uint32_t raytracing_tier() const { return raytracing_tier_; }
  // Borrowed native pointers. They remain owned by the client and are valid
  // only until device reset/destruction. A mod must queue-wait on the fence
  // value before sampling the result.
  void* shadow_output_resource() const;
  void* shadow_done_fence() const;
  uint64_t shadow_done_fence_value() const { return shadow_done_fence_value_; }
  uint32_t shadow_output_width() const;
  uint32_t shadow_output_height() const;
  uint32_t shadow_output_format() const;

 private:
  struct SmokeResources;
  struct ShadowOutputResources;
  bool AppendCpuScenePayload(const void* data, uint32_t size,
                             uint32_t* payload_offset);
  bool RecordShadowUpload();
  void Stop();

  Device* device_;
  HANDLE mapping_ = nullptr;
  void* mapping_view_ = nullptr;
  HANDLE ready_event_ = nullptr;
  HANDLE shutdown_event_ = nullptr;
  HANDLE work_event_ = nullptr;
  HANDLE done_event_ = nullptr;
  // Owns the helper process tree. KILL_ON_JOB_CLOSE makes an abnormal x86
  // process exit clean up the x64 helper too, rather than leaving DXC/DXIL
  // loaded and the next deployment locked.
  HANDLE helper_job_ = nullptr;
  PROCESS_INFORMATION process_ = {};
  bool ready_ = false;
  bool start_attempted_ = false;
  uint32_t raytracing_tier_ = 0;
  std::unique_ptr<SmokeResources> smoke_;
  std::unique_ptr<ShadowOutputResources> shadow_output_;
  bool smoke_test_attempted_ = false;
  bool scene_batch_in_flight_ = false;
  uint32_t next_geometry_id_ = 1;
  uint32_t h3_draws_ = 0;
  uint32_t h3_acks_ = 0;
  uint64_t observed_indexed_draws_ = 0;
  uint64_t observed_triangle_lists_ = 0;
  uint64_t observed_static_triangles_ = 0;
  uint64_t shadow_done_fence_value_ = 0;
  uint64_t capture_not_before_tick_ = 0;
};
}  // namespace Dx8to12
