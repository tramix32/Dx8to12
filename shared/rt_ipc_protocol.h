#pragma once

#include <cstdint>

// Deliberately C-compatible and pointer-free: this object is mapped by both
// the x86 shim and the x64 DXR helper.  Bump kVersion for every incompatible
// layout change.
namespace Dx8to12::RtIpc {
inline constexpr uint32_t kMagic = 0x54523844;  // "D8RT"
inline constexpr uint32_t kVersion = 14;
// Conservative fixed resolution for the first compositable RT channel. It
// avoids reallocating shared resources on a window resize; the consumer may
// bilinearly upscale it to its render target.
inline constexpr uint32_t kShadowOutputWidth = 320;
inline constexpr uint32_t kShadowOutputHeight = 180;
inline constexpr uint32_t kShadowPixelCount =
    kShadowOutputWidth * kShadowOutputHeight;
inline constexpr uint32_t kRtOutputChannels = 4;
inline constexpr uint32_t kRtPayloadBytes =
    kShadowPixelCount * kRtOutputChannels;
inline constexpr uint32_t kMaxSceneInstances = 128;
inline constexpr uint32_t kMaxSceneResources = 64;
// Bounded CPU transport. Geometry is copied into the file mapping by x86 and
// uploaded into helper-owned resources by x64. This deliberately avoids any
// D3D12 resource sharing between the two devices.
inline constexpr uint32_t kMaxCpuScenePayloadBytes = 8u * 1024u * 1024u;

enum class HelperStatus : uint32_t {
  kStarting = 0,
  kReady = 1,
  kAdapterNotFound = 2,
  kDeviceCreateFailed = 3,
  kOptions5Failed = 4,
  kRaytracingUnavailable = 5,
  kProtocolMismatch = 6,
  kSmokeFailed = 7,
};

enum class Command : uint32_t { kNone = 0, kSmokeBuffer = 1 };

// H3: fixed-width instance data. Resource IDs identify the two helper-owned
// upload buffers created from this batch's bounded CPU payload.
struct SceneInstance {
  uint32_t vertex_resource_id = 0;
  uint32_t index_resource_id = 0;
  uint32_t vertex_stride = 0;
  uint32_t start_index = 0;
  uint32_t index_count = 0;
  uint32_t base_vertex = 0;
  // From the D3D8 DrawIndexedPrimitive call itself (MinVertexIndex/
  // NumVertices) -- the actual vertex range this draw touches, used to
  // bound the BLAS's VertexCount correctly instead of guessing from the
  // mirror buffer's total size (see BuildBlases; guessing from buffer size
  // alone was wrong for the shared dynamic-ring mirror, where it produced a
  // VertexCount of millions -- most of the buffer belongs to unrelated
  // draws -- and fed garbage data into BLAS/TLAS builds).
  uint32_t min_vertex_index = 0;
  uint32_t num_vertices = 0;
  uint32_t material_srv_index = 0;
  uint32_t index_format = 0;  // DXGI_FORMAT_R16_UINT or DXGI_FORMAT_R32_UINT.
  // Offset of a float3 NORMAL in the vertex. UINT_MAX means the submitted
  // declaration has no usable normal (reflections/GI must skip it).
  uint32_t normal_byte_offset = UINT32_MAX;
  uint64_t vertex_byte_offset = 0;
  uint64_t index_byte_offset = 0;
  uint32_t vertex_payload_offset = 0;
  uint32_t vertex_payload_size = 0;
  uint32_t index_payload_offset = 0;
  uint32_t index_payload_size = 0;
  float world[16] = {};
};

// Legacy v11 GPU-mirror metadata. Kept in the fixed layout temporarily while
// the old implementation is removed; protocol v12 does not publish entries.
struct SceneResource {
  uint32_t id = 0;
  uint32_t kind = 0;  // 1 = vertex buffer, 2 = index buffer.
  uint64_t generation = 0;
  uint64_t byte_size = 0;
  wchar_t shared_name[128] = {};
};

struct Handshake {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  uint32_t shim_process_id = 0;
  uint32_t helper_process_id = 0;
  uint32_t adapter_luid_low = 0;
  int32_t adapter_luid_high = 0;
  uint32_t status = static_cast<uint32_t>(HelperStatus::kStarting);
  int32_t hresult = 0;
  uint32_t raytracing_tier = 0;
  uint32_t command = static_cast<uint32_t>(Command::kNone);
  uint32_t command_sequence = 0;
  uint32_t completed_sequence = 0;
  int32_t command_hresult = 0;
  uint32_t smoke_value = 0;
  wchar_t shared_buffer_name[128] = {};
  wchar_t x86_ready_fence_name[128] = {};
  wchar_t x64_done_fence_name[128] = {};
  wchar_t scene_ready_fence_name[128] = {};
  uint64_t scene_ready_fence_value = 0;
  uint32_t scene_instance_count = 0;
  uint32_t scene_resource_count = 0;
  uint32_t scene_payload_size = 0;
  uint32_t scene_lighting_mode = 0;
  uint32_t scene_sequence = 0;
  uint32_t scene_completed_sequence = 0;
  int32_t scene_hresult = 0;
  uint32_t scene_opened_resource_count = 0;
  uint32_t scene_blas_rebuilt = 0;
  uint32_t scene_blas_cached = 0;
  uint32_t scene_blas_skipped_base_vertex = 0;
  uint32_t scene_blas_skipped_format_or_range = 0;
  uint32_t scene_tlas_instances = 0;
  int32_t shadow_smoke_hresult = 0;
  // IEEE-754 bits read back from the first real TraceRay dispatch.  A
  // separate fixed-width field keeps the x86/x64 shared layout unambiguous.
  uint32_t scene_shadow_visibility_bits = 0;
  uint32_t shadow_payload_size = 0;
  uint32_t shadow_payload_width = 0;
  uint32_t shadow_payload_height = 0;
  uint32_t shadow_payload_row_pitch = 0;
  uint32_t shadow_payload_format = 0;
  uint32_t shadow_payload_generation = 0;
  // H4: x86 owns the shared result and completion fence.  The helper imports
  // both, writes the result, restores COMMON, then signals the fence.  This
  // keeps the later x86 compositing pass free of CPU waits.
  wchar_t shadow_output_name[128] = {};
  wchar_t shadow_done_fence_name[128] = {};
  uint64_t shadow_done_fence_value = 0;
  // Inverse D3D8 row-major view/projection matrices captured at submission.
  // Scalar floats keep the x86/x64 boundary independent of D3DMATRIX ABI.
  float scene_view[16] = {};
  float scene_projection[16] = {};
  SceneResource scene_resources[kMaxSceneResources] = {};
  SceneInstance scene_instances[kMaxSceneInstances] = {};
  uint8_t scene_payload[kMaxCpuScenePayloadBytes] = {};
  // RGBA8: R=shadow visibility, G=reflection hit, B=diffuse visibility,
  // A=primary-ray geometry validity.
  uint8_t shadow_payload[kRtPayloadBytes] = {};
};

static_assert(sizeof(SceneInstance) == 144);
static_assert(sizeof(SceneResource) == 280);
}  // namespace Dx8to12::RtIpc
