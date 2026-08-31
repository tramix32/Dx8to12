#include "rt_helper_client.h"

#include <filesystem>
#include <fstream>
#include <cmath>
#include <string>

#include <d3d12.h>
#include <DirectXMath.h>

#include "aixlog.hpp"
#include "buffer.h"
#include "config.h"
#include "device.h"
#include "rt_ipc_protocol.h"

namespace Dx8to12 {
namespace {


#ifdef DX8TO12_ENABLE_MINDEBUG
void WriteRtMiniLog(const std::string& line) {
  static std::ofstream output(CURRENT_SOURCE_DIR "/rt_helper_mindebug.log",
                              std::ofstream::out | std::ofstream::trunc);
  if (output) { output << line << "\n"; output.flush(); }
}
#else
void WriteRtMiniLog(const std::string&) {}
#endif

std::wstring UniqueName(const wchar_t* kind) {
  return std::wstring(L"Local\\Dx8to12Rt-") + kind + L"-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64());
}

std::wstring HelperPath() {
  wchar_t module_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  return (std::filesystem::path(module_path).parent_path() /
          L"dx8to12_rt_helper.exe")
      .wstring();
}

}  // namespace

RtHelperClient::RtHelperClient(Device* device) : device_(device) {}
RtHelperClient::~RtHelperClient() { Stop(); }

struct RtHelperClient::SmokeResources {
  ComPtr<ID3D12Resource> buffer;
  ComPtr<ID3D12Resource> upload;
  ComPtr<ID3D12Fence> x86_ready_fence;
  ComPtr<ID3D12Fence> x64_done_fence;
  HANDLE buffer_handle = nullptr;
  HANDLE x86_ready_handle = nullptr;
  HANDLE x64_done_handle = nullptr;
  bool submitted = false;

  ~SmokeResources() {
    if (buffer_handle) CloseHandle(buffer_handle);
    if (x86_ready_handle) CloseHandle(x86_ready_handle);
    if (x64_done_handle) CloseHandle(x64_done_handle);
  }
};

// Owned exclusively by the x86 device. The helper never opens this resource;
// it returns R8 bytes through the mapped IPC block and x86 records the upload
// before invoking mod callbacks on the same command list.
struct RtHelperClient::ShadowOutputResources {
  ComPtr<ID3D12Resource> texture;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  uint32_t uploaded_generation = 0;
};

void* RtHelperClient::shadow_output_resource() const {
  return shadow_output_ && shadow_output_->uploaded_generation != 0
             ? shadow_output_->texture.get()
             : nullptr;
}

void* RtHelperClient::shadow_done_fence() const {
  return nullptr;
}

uint32_t RtHelperClient::shadow_output_width() const {
  return shadow_output_ && shadow_output_->uploaded_generation != 0
             ? RtIpc::kShadowOutputWidth
             : 0;
}

uint32_t RtHelperClient::shadow_output_height() const {
  return shadow_output_ && shadow_output_->uploaded_generation != 0
             ? RtIpc::kShadowOutputHeight
             : 0;
}

uint32_t RtHelperClient::shadow_output_format() const {
  return shadow_output_ && shadow_output_->uploaded_generation != 0
             ? DXGI_FORMAT_R8G8B8A8_UNORM
             : DXGI_FORMAT_UNKNOWN;
}

bool RtHelperClient::Start() {
  if (start_attempted_) return ready_;
  start_attempted_ = true;
  const std::wstring helper_path = HelperPath();
  if (!std::filesystem::exists(helper_path)) {
    LOG(INFO) << "RTHELPER: not installed next to d3d8.dll; x64 DXR disabled.\n";
    WriteRtMiniLog("RTHELPER start=missing-helper");
    return false;
  }

  const std::wstring map_name = UniqueName(L"map");
  const std::wstring ready_name = UniqueName(L"ready");
  const std::wstring shutdown_name = UniqueName(L"shutdown");
  const std::wstring work_name = UniqueName(L"work");
  const std::wstring done_name = UniqueName(L"done");
  mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, sizeof(RtIpc::Handshake), map_name.c_str());
  if (!mapping_) return false;
  mapping_view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                sizeof(RtIpc::Handshake));
  ready_event_ = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
  shutdown_event_ = CreateEventW(nullptr, TRUE, FALSE, shutdown_name.c_str());
  work_event_ = CreateEventW(nullptr, FALSE, FALSE, work_name.c_str());
  done_event_ = CreateEventW(nullptr, TRUE, FALSE, done_name.c_str());
  if (!mapping_view_ || !ready_event_ || !shutdown_event_ || !work_event_ ||
      !done_event_) {
    Stop();
    return false;
  }

  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  // Handshake now contains an 8 MiB payload. Aggregate assignment may create
  // a temporary larger than the x86 thread stack, so initialize the mapping
  // in place and restore the two non-zero protocol defaults explicitly.
  memset(handshake, 0, sizeof(*handshake));
  handshake->magic = RtIpc::kMagic;
  handshake->version = RtIpc::kVersion;
  DXGI_ADAPTER_DESC desc = {};
  if (FAILED(device_->adapter()->GetDesc(&desc))) {
    Stop();
    return false;
  }
  handshake->shim_process_id = GetCurrentProcessId();
  handshake->adapter_luid_low = desc.AdapterLuid.LowPart;
  handshake->adapter_luid_high = desc.AdapterLuid.HighPart;
  auto shadow_output = std::make_unique<ShadowOutputResources>();
  const D3D12_HEAP_PROPERTIES shadow_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC shadow_desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Width = RtIpc::kShadowOutputWidth,
      .Height = RtIpc::kShadowOutputHeight,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
      .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN};
  if (FAILED(device_->device()->CreateCommittedResource(
          &shadow_heap, D3D12_HEAP_FLAG_NONE, &shadow_desc,
          shadow_output->state, nullptr,
          IID_PPV_ARGS(shadow_output->texture.GetForInit())))) {
    Stop();
    return false;
  }
  shadow_output_ = std::move(shadow_output);

  std::wstring command_line = L"\"" + helper_path + L"\" --handshake \"" +
                              map_name + L"\" \"" + ready_name + L"\" \"" +
                              shutdown_name + L"\" \"" + work_name + L"\" \"" +
                              done_name + L"\"";
  STARTUPINFOW startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESHOWWINDOW,
                          .wShowWindow = SW_HIDE};
  if (!CreateProcessW(helper_path.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process_)) {
    LOG(AixLog::Severity::error) << "RTHELPER: CreateProcess failed: "
                                 << GetLastError() << "\n";
    WriteRtMiniLog("RTHELPER start=CreateProcess-failed error=" +
                   std::to_string(GetLastError()));
    Stop();
    return false;
  }
  helper_job_ = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!helper_job_ || !SetInformationJobObject(
                         helper_job_, JobObjectExtendedLimitInformation,
                         &limits, sizeof(limits)) ||
      !AssignProcessToJobObject(helper_job_, process_.hProcess)) {
    LOG(AixLog::Severity::error)
        << "RTHELPER: unable to assign helper kill-on-close job: "
        << GetLastError() << "\n";
    Stop();
    return false;
  }

  if (WaitForSingleObject(ready_event_, 3000) != WAIT_OBJECT_0) {
    LOG(AixLog::Severity::error) << "RTHELPER: handshake timed out.\n";
    WriteRtMiniLog("RTHELPER start=handshake-timeout");
    Stop();
    return false;
  }
  const auto status = static_cast<RtIpc::HelperStatus>(handshake->status);
  raytracing_tier_ = handshake->raytracing_tier;
  ready_ = status == RtIpc::HelperStatus::kReady;
  LOG(INFO) << "RTHELPER: status=" << static_cast<uint32_t>(status)
            << " hr=0x" << std::hex << handshake->hresult << std::dec
            << " tier=" << raytracing_tier_ << "\n";
  WriteRtMiniLog("RTHELPER shadow-smoke-hr=" +
                 std::to_string(handshake->shadow_smoke_hresult) +
                 " status=" + std::to_string(static_cast<uint32_t>(status)) +
                 " tier=" + std::to_string(raytracing_tier_) +
                 " hr=" + std::to_string(handshake->hresult));
  if (!ready_) Stop();
  return ready_;
}

void RtHelperClient::BeginSmokeTest() {
  if (!ready_ || smoke_ || smoke_test_attempted_) return;
  smoke_test_attempted_ = true;
  auto resources = std::make_unique<SmokeResources>();
  const std::wstring buffer_name = UniqueName(L"smoke-buffer");
  const std::wstring ready_fence_name = UniqueName(L"smoke-x86-ready");
  const std::wstring done_fence_name = UniqueName(L"smoke-x64-done");
  const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC buffer_desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Width = 256,
      .Height = 1,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  if (FAILED(device_->device()->CreateCommittedResource(
          &default_heap, D3D12_HEAP_FLAG_SHARED, &buffer_desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr,
          IID_PPV_ARGS(resources->buffer.GetForInit()))) ||
      FAILED(device_->device()->CreateSharedHandle(
          resources->buffer.get(), nullptr, GENERIC_ALL, buffer_name.c_str(),
          &resources->buffer_handle))) {
    LOG(AixLog::Severity::error) << "RTHELPER-H2: shared buffer creation failed.\n";
    return;
  }
  const D3D12_HEAP_PROPERTIES upload_heap{.Type = D3D12_HEAP_TYPE_UPLOAD};
  if (FAILED(device_->device()->CreateCommittedResource(
          &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(resources->upload.GetForInit())))) {
    return;
  }
  uint32_t* marker = nullptr;
  ASSERT_HR(resources->upload->Map(0, nullptr, reinterpret_cast<void**>(&marker)));
  *marker = 0xD8A2C0DEu;
  resources->upload->Unmap(0, nullptr);
  if (FAILED(device_->device()->CreateFence(
          0, D3D12_FENCE_FLAG_SHARED,
          IID_PPV_ARGS(resources->x86_ready_fence.GetForInit()))) ||
      FAILED(device_->device()->CreateFence(
          0, D3D12_FENCE_FLAG_SHARED,
          IID_PPV_ARGS(resources->x64_done_fence.GetForInit()))) ||
      FAILED(device_->device()->CreateSharedHandle(
          resources->x86_ready_fence.get(), nullptr, GENERIC_ALL,
          ready_fence_name.c_str(), &resources->x86_ready_handle)) ||
      FAILED(device_->device()->CreateSharedHandle(
          resources->x64_done_fence.get(), nullptr, GENERIC_ALL,
          done_fence_name.c_str(), &resources->x64_done_handle))) {
    LOG(AixLog::Severity::error) << "RTHELPER-H2: shared fence creation failed.\n";
    return;
  }
  const D3D12_RESOURCE_BARRIER to_copy = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Transition = {.pResource = resources->buffer.get(),
                     .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                     .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST}};
  const D3D12_RESOURCE_BARRIER to_common = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Transition = {.pResource = resources->buffer.get(),
                     .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                     .StateAfter = D3D12_RESOURCE_STATE_COMMON}};
  device_->cmd_list()->ResourceBarrier(1, &to_copy);
  device_->cmd_list()->CopyBufferRegion(resources->buffer.get(), 0,
                                        resources->upload.get(), 0, 4);
  device_->cmd_list()->ResourceBarrier(1, &to_common);
  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  wcsncpy_s(handshake->shared_buffer_name, buffer_name.c_str(), _TRUNCATE);
  wcsncpy_s(handshake->x86_ready_fence_name, ready_fence_name.c_str(), _TRUNCATE);
  wcsncpy_s(handshake->x64_done_fence_name, done_fence_name.c_str(), _TRUNCATE);
  handshake->command_hresult = 0;
  handshake->smoke_value = 0;
  handshake->command = static_cast<uint32_t>(RtIpc::Command::kSmokeBuffer);
  ++handshake->command_sequence;
  ResetEvent(done_event_);
  smoke_ = std::move(resources);
  LOG(INFO) << "RTHELPER-H2: x86 copy recorded.\n";
}

void RtHelperClient::OnX86Submission() {
  if (smoke_ && !smoke_->submitted) {
    ASSERT_HR(device_->cmd_queue()->Signal(smoke_->x86_ready_fence.get(), 1));
    smoke_->submitted = true;
    SetEvent(work_event_);
    LOG(INFO) << "RTHELPER-H2: x86 fence signaled.\n";
  }
  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  if (!scene_batch_in_flight_ && handshake->scene_instance_count != 0) {
    D3DMATRIX view = {};
    D3DMATRIX projection = {};
    if (FAILED(device_->GetTransform(D3DTS_VIEW, &view)) ||
        FAILED(device_->GetTransform(D3DTS_PROJECTION, &projection))) {
      return;
    }
    DirectX::XMFLOAT4X4 view_matrix = {};
    DirectX::XMFLOAT4X4 projection_matrix = {};
    memcpy(&view_matrix, &view, sizeof(view_matrix));
    memcpy(&projection_matrix, &projection, sizeof(projection_matrix));
    DirectX::XMVECTOR view_determinant = {};
    DirectX::XMVECTOR projection_determinant = {};
    const DirectX::XMMATRIX inverse_view = DirectX::XMMatrixInverse(
        &view_determinant, DirectX::XMLoadFloat4x4(&view_matrix));
    const DirectX::XMMATRIX inverse_projection = DirectX::XMMatrixInverse(
        &projection_determinant, DirectX::XMLoadFloat4x4(&projection_matrix));
    if (fabsf(DirectX::XMVectorGetX(view_determinant)) < 1e-12f ||
        fabsf(DirectX::XMVectorGetX(projection_determinant)) < 1e-12f) {
      return;
    }
    DirectX::XMFLOAT4X4 inverse_view_matrix = {};
    DirectX::XMFLOAT4X4 inverse_projection_matrix = {};
    DirectX::XMStoreFloat4x4(&inverse_view_matrix, inverse_view);
    DirectX::XMStoreFloat4x4(&inverse_projection_matrix, inverse_projection);
    memcpy(handshake->scene_view, &inverse_view_matrix,
           sizeof(handshake->scene_view));
    memcpy(handshake->scene_projection, &inverse_projection_matrix,
           sizeof(handshake->scene_projection));
    handshake->scene_ready_fence_value = 0;
    handshake->scene_lighting_mode =
        static_cast<uint32_t>(GetConfig().lighting_mode);
    ++handshake->scene_sequence;
    scene_batch_in_flight_ = true;
    ResetEvent(done_event_);
    SetEvent(work_event_);
  }
}

void RtHelperClient::PollSmokeTest() {
  if (WaitForSingleObject(done_event_, 0) != WAIT_OBJECT_0) return;
  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  if (smoke_ && smoke_->submitted &&
      handshake->completed_sequence == handshake->command_sequence &&
      smoke_->x64_done_fence->GetCompletedValue() >= 1) {
    LOG(INFO) << "RTHELPER-H2: result hr=0x" << std::hex
              << handshake->command_hresult << std::dec << " marker=0x"
              << std::hex << handshake->smoke_value << std::dec << "\n";
  }
  if (smoke_) smoke_.reset();
  if (scene_batch_in_flight_ &&
      handshake->scene_completed_sequence == handshake->scene_sequence) {
    RecordShadowUpload();
    const uint32_t instance_count = handshake->scene_instance_count;
    const uint32_t ack_number = h3_acks_++;
    if (ack_number < 5 || (ack_number % 120) == 0) {
      LOG(INFO) << "RTHELPER-H3: helper imported "
                << handshake->scene_instance_count << " instances, "
                << handshake->scene_opened_resource_count << " resources, fence="
                << handshake->scene_ready_fence_value << " hr=0x" << std::hex
                << handshake->scene_hresult << std::dec << " blas(rebuilt="
                << handshake->scene_blas_rebuilt << ", cached="
                << handshake->scene_blas_cached << ", skipBase="
                << handshake->scene_blas_skipped_base_vertex << ", skipFmtRange="
                << handshake->scene_blas_skipped_format_or_range << ")\n";
    }
    handshake->scene_instance_count = 0;
    handshake->scene_resource_count = 0;
    handshake->scene_payload_size = 0;
    scene_batch_in_flight_ = false;
    capture_not_before_tick_ = GetTickCount64() + 500;
    // The result texture is x86-local and its upload is ordered earlier on
    // this same command list than mod callbacks. No cross-queue wait exists.
    shadow_done_fence_value_ = 0;
    WriteRtMiniLog("RTHELPER-H3 instances=" + std::to_string(instance_count) +
                   " resources=" + std::to_string(handshake->scene_opened_resource_count) +
                   " rebuilt=" + std::to_string(handshake->scene_blas_rebuilt) +
                   " cached=" + std::to_string(handshake->scene_blas_cached) +
                   " tlasInstances=" + std::to_string(handshake->scene_tlas_instances) +
                   " hr=" + std::to_string(handshake->scene_hresult) +
                   " shadowFence=" + std::to_string(handshake->shadow_done_fence_value) +
                   " shadowGeneration=" +
                       std::to_string(handshake->shadow_payload_generation) +
                   " shadowBytes=" +
                       std::to_string(handshake->shadow_payload_size) +
                   " shadowBits=0x" + [&] { std::ostringstream out; out << std::hex
                       << handshake->scene_shadow_visibility_bits; return out.str(); }());
  }
}

bool RtHelperClient::RecordShadowUpload() {
  if (!shadow_output_) return false;
  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  if (handshake->shadow_payload_generation == 0 ||
      handshake->shadow_payload_generation ==
          shadow_output_->uploaded_generation) {
    return false;
  }
  constexpr uint32_t source_row_pitch =
      RtIpc::kShadowOutputWidth * RtIpc::kRtOutputChannels;
  if (handshake->shadow_payload_size != RtIpc::kRtPayloadBytes ||
      handshake->shadow_payload_width != RtIpc::kShadowOutputWidth ||
      handshake->shadow_payload_height != RtIpc::kShadowOutputHeight ||
      handshake->shadow_payload_row_pitch != source_row_pitch ||
      handshake->shadow_payload_format != DXGI_FORMAT_R8G8B8A8_UNORM) {
    return false;
  }

  constexpr uint32_t row_pitch =
      (source_row_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
      ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  constexpr uint32_t upload_size = row_pitch * RtIpc::kShadowOutputHeight;
  const DynamicRingBuffer::Allocation upload =
      device_->dynamic_ring_buffer()->Allocate(
          upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  char* destination = device_->dynamic_ring_buffer()->GetCpuPtrFor(upload);
  memset(destination, 0, upload_size);
  for (uint32_t y = 0; y < RtIpc::kShadowOutputHeight; ++y) {
    memcpy(destination + y * row_pitch,
           handshake->shadow_payload + y * source_row_pitch,
           source_row_pitch);
  }

  const D3D12_RESOURCE_STATES ring_state =
      device_->dynamic_ring_buffer()->current_state();
  device_->TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_RESOURCE_BARRIER to_copy = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Transition = {
          .pResource = shadow_output_->texture.get(),
          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          .StateBefore = shadow_output_->state,
          .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST}};
  device_->cmd_list()->ResourceBarrier(1, &to_copy);
  shadow_output_->state = D3D12_RESOURCE_STATE_COPY_DEST;

  const D3D12_TEXTURE_COPY_LOCATION source = {
      .pResource = device_->dynamic_ring_buffer()->GetBackingResource(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = {
          .Offset = static_cast<uint64_t>(upload.offset),
          .Footprint = {.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                        .Width = RtIpc::kShadowOutputWidth,
                        .Height = RtIpc::kShadowOutputHeight,
                        .Depth = 1,
                        .RowPitch = row_pitch}}};
  const D3D12_TEXTURE_COPY_LOCATION target = {
      .pResource = shadow_output_->texture.get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = 0};
  device_->cmd_list()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
  const D3D12_RESOURCE_BARRIER to_shader = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Transition = {
          .pResource = shadow_output_->texture.get(),
          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
          .StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}};
  device_->cmd_list()->ResourceBarrier(1, &to_shader);
  shadow_output_->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  device_->TransitionDynamicRingBuffer(ring_state);
  shadow_output_->uploaded_generation = handshake->shadow_payload_generation;
  return true;
}

void RtHelperClient::ObserveIndexedDraw(D3DPRIMITIVETYPE primitive_type,
                                        const Buffer* vertex_buffer,
                                        const Buffer* index_buffer) {
  ++observed_indexed_draws_;
  if (primitive_type != D3DPT_TRIANGLELIST) return;
  ++observed_triangle_lists_;
  if (vertex_buffer && index_buffer && !vertex_buffer->IsDynamic() &&
      !index_buffer->IsDynamic()) {
    ++observed_static_triangles_;
  }
}

bool RtHelperClient::AppendCpuScenePayload(const void* data, uint32_t size,
                                           uint32_t* payload_offset) {
  if (!data || !payload_offset || size == 0) return false;
  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  const uint32_t aligned = (handshake->scene_payload_size + 15u) & ~15u;
  if (aligned > RtIpc::kMaxCpuScenePayloadBytes ||
      size > RtIpc::kMaxCpuScenePayloadBytes - aligned) {
    return false;
  }
  memcpy(handshake->scene_payload + aligned, data, size);
  handshake->scene_payload_size = aligned + size;
  *payload_offset = aligned;
  return true;
}

void RtHelperClient::RecordDynamicTriangle(
    DynamicBuffer* vertex_buffer, DynamicBuffer* index_buffer, UINT vertex_stride,
    UINT start_index, UINT index_count, UINT base_vertex, UINT min_vertex_index,
    UINT num_vertices, const D3DMATRIX& world,
    UINT material_srv_index, UINT index_format, UINT normal_byte_offset) {
  // The allocations are resolved after the normal draw was recorded.  This
  // is deliberate: it snapshots the exact dynamic-ring ranges D3D12 used.
  if (!ready_ || scene_batch_in_flight_ ||
      GetTickCount64() < capture_not_before_tick_ || vertex_stride < 12 ||
      index_count < 3)
    return;
  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  // Check before recording barriers/copies. The previous order filled the
  // 20-instance IPC array and then kept copying every remaining draw in the
  // frame even though none of those copies could be published.
  if (handshake->scene_instance_count >= RtIpc::kMaxSceneInstances) return;
  DynamicRingBuffer::Allocation vb_alloc = {}, ib_alloc = {};
  if (!vertex_buffer->GetCurrentRingAllocation(&vb_alloc) ||
      !index_buffer->GetCurrentRingAllocation(&ib_alloc)) return;
  const char* vb_data = device_->dynamic_ring_buffer()->GetCpuPtrFor(vb_alloc);
  const char* ib_data = device_->dynamic_ring_buffer()->GetCpuPtrFor(ib_alloc);
  uint32_t vb_payload = 0;
  uint32_t ib_payload = 0;
  const uint32_t payload_before = handshake->scene_payload_size;
  if (!AppendCpuScenePayload(vb_data, static_cast<uint32_t>(vb_alloc.size),
                            &vb_payload) ||
      !AppendCpuScenePayload(ib_data, static_cast<uint32_t>(ib_alloc.size),
                            &ib_payload)) {
    handshake->scene_payload_size = payload_before;
    return;
  }
  auto& instance = handshake->scene_instances[handshake->scene_instance_count++];
  instance = {};
  instance.vertex_resource_id = next_geometry_id_++;
  instance.index_resource_id = next_geometry_id_++;
  instance.vertex_stride = vertex_stride;
  instance.start_index = start_index;
  instance.index_count = index_count;
  instance.base_vertex = base_vertex;
  instance.min_vertex_index = min_vertex_index;
  instance.num_vertices = num_vertices;
  instance.material_srv_index = material_srv_index;
  instance.index_format = index_format;
  instance.normal_byte_offset = normal_byte_offset;
  instance.vertex_payload_offset = vb_payload;
  instance.vertex_payload_size = static_cast<uint32_t>(vb_alloc.size);
  instance.index_payload_offset = ib_payload;
  instance.index_payload_size = static_cast<uint32_t>(ib_alloc.size);
  memcpy(instance.world, &world, sizeof(instance.world));
}

void RtHelperClient::RecordStaticTriangle(Buffer* vertex_buffer,
                                          Buffer* index_buffer,
                                          UINT vertex_stride,
                                          UINT start_index, UINT index_count,
                                          UINT base_vertex,
                                          UINT min_vertex_index,
                                          UINT num_vertices,
                                          const D3DMATRIX& world,
                                          UINT material_srv_index,
                                          UINT index_format,
                                          UINT normal_byte_offset) {
  if (!ready_ || scene_batch_in_flight_ ||
      GetTickCount64() < capture_not_before_tick_ || vertex_stride < 12 ||
      index_count < 3) return;
  auto* handshake = static_cast<RtIpc::Handshake*>(mapping_view_);
  if (handshake->scene_instance_count >= RtIpc::kMaxSceneInstances) return;
  const uint64_t vb_size64 = vertex_buffer->resource_desc().Width;
  const uint64_t ib_size64 = index_buffer->resource_desc().Width;
  if (vb_size64 == 0 || ib_size64 == 0 || vb_size64 > INT_MAX ||
      ib_size64 > INT_MAX) return;
  const uint32_t vb_size = static_cast<uint32_t>(vb_size64);
  const uint32_t ib_size = static_cast<uint32_t>(ib_size64);
  const char* vb_data = vertex_buffer->CpuDataPtr(0, static_cast<int>(vb_size));
  const char* ib_data = index_buffer->CpuDataPtr(0, static_cast<int>(ib_size));
  uint32_t vb_payload = 0;
  uint32_t ib_payload = 0;
  const uint32_t payload_before = handshake->scene_payload_size;
  if (!AppendCpuScenePayload(vb_data, vb_size, &vb_payload) ||
      !AppendCpuScenePayload(ib_data, ib_size, &ib_payload)) {
    handshake->scene_payload_size = payload_before;
    return;
  }
  auto& instance = handshake->scene_instances[handshake->scene_instance_count++];
  instance = {};
  instance.vertex_resource_id = next_geometry_id_++;
  instance.index_resource_id = next_geometry_id_++;
  instance.vertex_stride = vertex_stride;
  instance.start_index = start_index;
  instance.index_count = index_count;
  instance.base_vertex = base_vertex;
  instance.min_vertex_index = min_vertex_index;
  instance.num_vertices = num_vertices;
  instance.material_srv_index = material_srv_index;
  instance.index_format = index_format;
  instance.normal_byte_offset = normal_byte_offset;
  instance.vertex_payload_offset = vb_payload;
  instance.vertex_payload_size = vb_size;
  instance.index_payload_offset = ib_payload;
  instance.index_payload_size = ib_size;
  memcpy(instance.world, &world, sizeof(instance.world));
  if ((h3_draws_++ % 240) == 0) {
    LOG(INFO) << "RTHELPER-H3: CPU snapshot indices=" << index_count
              << " payload=" << handshake->scene_payload_size << " bytes\n";
  }
}

void RtHelperClient::Stop() {
  if (ready_) {
    LOG(INFO) << "RTHELPER-H3: observed indexed=" << observed_indexed_draws_
              << " triangleLists=" << observed_triangle_lists_
              << " staticTriangles=" << observed_static_triangles_ << "\n";
  }
  smoke_.reset();
  shadow_output_.reset();
  if (shutdown_event_) SetEvent(shutdown_event_);
  if (process_.hProcess) {
    if (WaitForSingleObject(process_.hProcess, 1000) == WAIT_TIMEOUT) {
      TerminateProcess(process_.hProcess, ERROR_PROCESS_ABORTED);
      WaitForSingleObject(process_.hProcess, 1000);
    }
    CloseHandle(process_.hThread);
    CloseHandle(process_.hProcess);
  }
  if (helper_job_) {
    CloseHandle(helper_job_);
    helper_job_ = nullptr;
  }
  process_ = {};
  if (mapping_view_) UnmapViewOfFile(mapping_view_);
  if (shutdown_event_) CloseHandle(shutdown_event_);
  if (ready_event_) CloseHandle(ready_event_);
  if (work_event_) CloseHandle(work_event_);
  if (done_event_) CloseHandle(done_event_);
  if (mapping_) CloseHandle(mapping_);
  mapping_view_ = nullptr;
  shutdown_event_ = nullptr;
  ready_event_ = nullptr;
  work_event_ = nullptr;
  done_event_ = nullptr;
  mapping_ = nullptr;
  ready_ = false;
  start_attempted_ = false;
}

}  // namespace Dx8to12
