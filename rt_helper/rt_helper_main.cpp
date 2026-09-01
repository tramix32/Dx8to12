#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <DirectXMath.h>
// The DLAA helper mode below is the only user; the rest of this file predates
// it and manages lifetimes by hand.
#include <wrl/client.h>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string_view>

#include "dlss_ipc_protocol.h"
#include "rt_ipc_protocol.h"

#ifdef DX8TO12_HAVE_STREAMLINE
#include <sl.h>
#include <sl_helpers.h>
#endif

namespace {

HRESULT CompileShadowLibrary(IDxcBlob** object_out = nullptr);
HRESULT CreateShadowPipeline(ID3D12Device* device, ID3D12StateObject** state_out,
                             ID3D12RootSignature** root_out = nullptr);
HRESULT CreateUploadBuffer(ID3D12Device* device, uint64_t size, ID3D12Resource** resource);
HRESULT WaitForQueueIdle(ID3D12Device* device, ID3D12CommandQueue* queue);
HRESULT DispatchShadowSmoke(ID3D12Device* device, ID3D12CommandQueue* queue,
                            ID3D12StateObject* pipeline);
HRESULT DispatchShadowMask(ID3D12Device* device, ID3D12CommandQueue* queue,
                           ID3D12StateObject* pipeline, ID3D12RootSignature* root,
                           ID3D12Resource* tlas, ID3D12Resource* output,
                           const Dx8to12::RtIpc::Handshake& camera);
HRESULT ReadbackShadowMask(ID3D12Device* device, ID3D12CommandQueue* queue,
                           ID3D12Resource* output,
                           Dx8to12::RtIpc::Handshake* response);

void PrintUsage() {
  std::wcerr << L"Usage: dx8to12_rt_helper --self-test | --dlss-probe"
                L" | --dlaa <shared-memory-name>\n";
}

// --------------------------------------------------------------------------
// DLAA/DLSS helper (stage D).
//
// Stage D1 is deliberately a loopback: the frame is copied input -> output and
// nothing else. That makes the first in-game test a clean question -- if the
// image is not pixel-identical, the fault is in the cross-process transport,
// not in Streamline. Streamline goes in only once that answer is yes.
// --------------------------------------------------------------------------

// Finds the adapter the game is running on. Sharing resources across two
// different physical adapters silently produces garbage rather than an error,
// so this matches on LUID rather than taking whatever is first.
HRESULT FindAdapterByLuid(LUID luid, IDXGIAdapter1** adapter_out) {
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return hr;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.AdapterLuid.LowPart == luid.LowPart &&
        desc.AdapterLuid.HighPart == luid.HighPart) {
      *adapter_out = adapter.Detach();
      return S_OK;
    }
  }
  return DXGI_ERROR_NOT_FOUND;
}

int RunDlaaHelper(const wchar_t* map_name) {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
  if (!mapping) {
    std::wcerr << L"--dlaa: could not open shared memory.\n";
    return 1;
  }
  auto* shared = static_cast<Dx8to12::DlssIpc::Handshake*>(MapViewOfFile(
      mapping, FILE_MAP_ALL_ACCESS, 0, 0,
      sizeof(Dx8to12::DlssIpc::Handshake)));
  if (!shared) {
    CloseHandle(mapping);
    return 1;
  }

  auto fail = [&](Dx8to12::DlssIpc::HelperStatus status, HRESULT hr) {
    shared->hresult = hr;
    shared->status = static_cast<uint32_t>(status);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 1;
  };

  if (shared->magic != Dx8to12::DlssIpc::kMagic ||
      shared->version != Dx8to12::DlssIpc::kVersion) {
    return fail(Dx8to12::DlssIpc::HelperStatus::kProtocolMismatch, E_FAIL);
  }
  shared->helper_process_id = GetCurrentProcessId();

  LUID luid = {};
  luid.LowPart = shared->adapter_luid_low;
  luid.HighPart = shared->adapter_luid_high;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  HRESULT hr = FindAdapterByLuid(luid, &adapter);
  if (FAILED(hr)) {
    return fail(Dx8to12::DlssIpc::HelperStatus::kAdapterNotFound, hr);
  }
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                         IID_PPV_ARGS(&device));
  if (FAILED(hr)) {
    return fail(Dx8to12::DlssIpc::HelperStatus::kDeviceCreateFailed, hr);
  }

  // Open everything x86 created. The helper never creates a resource the game
  // has to import -- that direction is what caused this project's repeated
  // device removals.
  constexpr uint32_t kSlots = Dx8to12::DlssIpc::kFrameSlots;
  Microsoft::WRL::ComPtr<ID3D12Resource> color_in[kSlots], color_out[kSlots];
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_in[kSlots], mvec_in[kSlots];
  Microsoft::WRL::ComPtr<ID3D12Fence> ready_fence, done_fence;
  auto open_shared = [&](const wchar_t* name, REFIID iid, void** out) {
    HANDLE handle = nullptr;
    HRESULT open_hr =
        device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
    if (FAILED(open_hr)) return open_hr;
    open_hr = device->OpenSharedHandle(handle, iid, out);
    CloseHandle(handle);
    return open_hr;
  };
  for (uint32_t slot = 0; slot < kSlots && SUCCEEDED(hr); ++slot) {
    hr = open_shared(shared->color_in_name[slot], __uuidof(ID3D12Resource),
                     reinterpret_cast<void**>(color_in[slot].GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = open_shared(shared->color_out_name[slot], __uuidof(ID3D12Resource),
                       reinterpret_cast<void**>(color_out[slot].GetAddressOf()));
    }
    if (SUCCEEDED(hr) && shared->depth_in_name[slot][0]) {
      hr = open_shared(shared->depth_in_name[slot], __uuidof(ID3D12Resource),
                       reinterpret_cast<void**>(depth_in[slot].GetAddressOf()));
    }
    if (SUCCEEDED(hr) && shared->mvec_in_name[slot][0]) {
      hr = open_shared(shared->mvec_in_name[slot], __uuidof(ID3D12Resource),
                       reinterpret_cast<void**>(mvec_in[slot].GetAddressOf()));
    }
  }
  if (SUCCEEDED(hr)) {
    hr = open_shared(shared->ready_fence_name, __uuidof(ID3D12Fence),
                     reinterpret_cast<void**>(ready_fence.GetAddressOf()));
  }
  if (SUCCEEDED(hr)) {
    hr = open_shared(shared->done_fence_name, __uuidof(ID3D12Fence),
                     reinterpret_cast<void**>(done_fence.GetAddressOf()));
  }
  if (FAILED(hr)) {
    return fail(Dx8to12::DlssIpc::HelperStatus::kSharedOpenFailed, hr);
  }

  const D3D12_COMMAND_QUEUE_DESC queue_desc = {
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_list;
  hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(hr)) {
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator));
  }
  if (SUCCEEDED(hr)) {
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&cmd_list));
  }
  if (FAILED(hr)) {
    return fail(Dx8to12::DlssIpc::HelperStatus::kDeviceCreateFailed, hr);
  }
  cmd_list->Close();

  // Report what actually came through the shared handles. The loopback stage
  // proves the transport only if this matches what x86 created -- a handle
  // resolving to the wrong resource, or to the right one with a different
  // format, is silent corruption rather than a failure.
  const D3D12_RESOURCE_DESC color_in_desc = color_in[0]->GetDesc();
  shared->seen_color_in_width = static_cast<uint32_t>(color_in_desc.Width);
  shared->seen_color_in_height = color_in_desc.Height;
  shared->seen_color_in_format = static_cast<uint32_t>(color_in_desc.Format);
  if (depth_in[0]) {
    const D3D12_RESOURCE_DESC depth_desc = depth_in[0]->GetDesc();
    shared->seen_depth_in_width = static_cast<uint32_t>(depth_desc.Width);
    shared->seen_depth_in_format = static_cast<uint32_t>(depth_desc.Format);
  }
  if (mvec_in[0]) {
    const D3D12_RESOURCE_DESC mvec_desc = mvec_in[0]->GetDesc();
    shared->seen_mvec_in_width = static_cast<uint32_t>(mvec_desc.Width);
    shared->seen_mvec_in_format = static_cast<uint32_t>(mvec_desc.Format);
  }

  shared->status = static_cast<uint32_t>(Dx8to12::DlssIpc::HelperStatus::kReady);

  HANDLE ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  uint64_t processed = 0;
  while (shared->shutdown_requested == 0) {
    // Block on the fence itself; never poll the shared struct.
    //
    // This loop used to check shared->frame_index and Sleep(1) when there was
    // nothing new. At Windows' default timer granularity Sleep(1) actually
    // sleeps a full ~15.6 ms tick, so every frame waited a tick for the helper
    // to *notice* it -- measured: the game ran at 64 fps (15.55 ms/frame) with
    // the GPU completely idle, against 249 fps with the feature off. The cost
    // was entirely this sleep, not the copies.
    //
    // There is also nothing to poll for: fence values are frame indices, so
    // waiting for the fence to reach the next one is the same question, asked
    // in a way the scheduler can answer immediately.
    uint64_t wanted = ready_fence->GetCompletedValue();
    if (wanted <= processed) {
      if (FAILED(ready_fence->SetEventOnCompletion(processed + 1, ready_event)))
        break;
      // Bounded rather than infinite, so shutdown_requested is still noticed
      // while the game is paused or alt-tabbed and submitting nothing.
      if (WaitForSingleObject(ready_event, 100) != WAIT_OBJECT_0) continue;
      wanted = ready_fence->GetCompletedValue();
      if (wanted <= processed) continue;
    }
    // Take the newest frame the game has signalled, not processed + 1: if the
    // helper ever falls behind, working through the backlog one frame at a
    // time would keep it behind forever. Signalling the newer value satisfies
    // any older wait too, since x86 waits for "at least" its frame index.

    // The game writes frame N into slot N % kSlots and reads the result of
    // frame N-1 out of the other one, so the two never collide.
    const uint32_t slot = static_cast<uint32_t>(wanted % kSlots);
    allocator->Reset();
    cmd_list->Reset(allocator.Get(), nullptr);
    D3D12_RESOURCE_BARRIER to_copy[2] = {};
    to_copy[0].Transition = {.pResource = color_in[slot].Get(),
                             .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                             .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE};
    to_copy[1].Transition = {.pResource = color_out[slot].Get(),
                             .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                             .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST};
    cmd_list->ResourceBarrier(2, to_copy);
    cmd_list->CopyResource(color_out[slot].Get(), color_in[slot].Get());
    // Handed back in COMMON. The game's own state tracking believes these
    // resources sit in COMMON between frames, and it is authoritative -- the
    // helper leaving them in a copy state would desynchronise it silently.
    D3D12_RESOURCE_BARRIER to_common[2] = {to_copy[0], to_copy[1]};
    for (D3D12_RESOURCE_BARRIER& barrier : to_common) {
      std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    cmd_list->ResourceBarrier(2, to_common);
    hr = cmd_list->Close();
    if (SUCCEEDED(hr)) {
      ID3D12CommandList* lists[] = {cmd_list.Get()};
      queue->ExecuteCommandLists(1, lists);
      hr = queue->Signal(done_fence.Get(), wanted);
    }
    if (FAILED(hr)) {
      shared->last_hresult = hr;
      ++shared->failed_frames;
      break;
    }
    processed = wanted;
    shared->completed_frame_index = wanted;
  }

  // Do not tear down while the GPU still owns the shared resources.
  Microsoft::WRL::ComPtr<ID3D12Fence> drain;
  if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&drain)))) {
    queue->Signal(drain.Get(), 1);
    if (drain->GetCompletedValue() < 1) {
      drain->SetEventOnCompletion(1, ready_event);
      WaitForSingleObject(ready_event, 2000);
    }
  }
  CloseHandle(ready_event);
  UnmapViewOfFile(shared);
  CloseHandle(mapping);
  return 0;
}

int RunSelfTest() {
  IDXGIFactory6* factory = nullptr;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    std::wcerr << L"CreateDXGIFactory2 failed: 0x" << std::hex << hr << L"\n";
    return 2;
  }

  IDXGIAdapter1* adapter = nullptr;
  hr = factory->EnumAdapterByGpuPreference(
      0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  factory->Release();
  if (FAILED(hr)) {
    std::wcerr << L"EnumAdapterByGpuPreference failed: 0x" << std::hex << hr
               << L"\n";
    return 3;
  }

  DXGI_ADAPTER_DESC1 desc = {};
  adapter->GetDesc1(&desc);
  ID3D12Device* device = nullptr;
  hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                         IID_PPV_ARGS(&device));
  adapter->Release();
  if (FAILED(hr)) {
    std::wcerr << L"D3D12CreateDevice failed: 0x" << std::hex << hr << L"\n";
    return 4;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
  hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                   sizeof(options5));
  ID3D12Device5* device5 = nullptr;
  const HRESULT device5_hr = device->QueryInterface(IID_PPV_ARGS(&device5));
  const HRESULT shader_hr = CompileShadowLibrary();
  ID3D12StateObject* shadow_pipeline = nullptr;
  ID3D12RootSignature* shadow_root = nullptr;
  const HRESULT pipeline_hr = CreateShadowPipeline(device, &shadow_pipeline, &shadow_root);
  if (shadow_pipeline) shadow_pipeline->Release();
  if (shadow_root) shadow_root->Release();
  if (device5 != nullptr) device5->Release();
  device->Release();

  std::wcout << L"RTHELPER-SELFTEST adapter=" << desc.Description
             << L" luid=" << std::hex << desc.AdapterLuid.HighPart << L":"
             << desc.AdapterLuid.LowPart << L" create=0x0 options5=0x" << hr
             << L" tier=" << std::dec << options5.RaytracingTier
             << L" ID3D12Device5=0x" << std::hex << device5_hr << L"\n";
  std::wcout << L"RTHELPER-SELFTEST dxr-library=0x" << std::hex << shader_hr << L"\n";
  std::wcout << L"RTHELPER-SELFTEST dxr-pipeline=0x" << std::hex << pipeline_hr << L"\n";
  std::wcout << L"RTHELPER-SELFTEST dxr-dispatch=deferred-until-valid-tlas\n";
  return SUCCEEDED(hr) && SUCCEEDED(shader_hr) && SUCCEEDED(pipeline_hr) &&
                 options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0
             ? 0
             : 5;
}

int RunSmoke(ID3D12Device* device, const Dx8to12::RtIpc::Handshake& request,
             uint32_t* marker_out) {
  HANDLE buffer_handle = nullptr, ready_handle = nullptr, done_handle = nullptr;
  ID3D12Resource* buffer = nullptr;
  ID3D12Fence* ready_fence = nullptr;
  ID3D12Fence* done_fence = nullptr;
  HRESULT hr = device->OpenSharedHandleByName(request.shared_buffer_name,
                                               GENERIC_ALL, &buffer_handle);
  if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(buffer_handle, IID_PPV_ARGS(&buffer));
  if (SUCCEEDED(hr)) hr = device->OpenSharedHandleByName(request.x86_ready_fence_name,
                                                          GENERIC_ALL, &ready_handle);
  if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(ready_handle, IID_PPV_ARGS(&ready_fence));
  if (SUCCEEDED(hr)) hr = device->OpenSharedHandleByName(request.x64_done_fence_name,
                                                          GENERIC_ALL, &done_handle);
  if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(done_handle, IID_PPV_ARGS(&done_fence));
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Resource* readback = nullptr;
  HANDLE finished = nullptr;
  if (SUCCEEDED(hr)) {
    const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  }
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     allocator, nullptr, IID_PPV_ARGS(&list));
  const D3D12_HEAP_PROPERTIES readback_heap{.Type = D3D12_HEAP_TYPE_READBACK};
  const D3D12_RESOURCE_DESC readback_desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 256, .Height = 1,
      .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(
      &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
  if (SUCCEEDED(hr)) {
    const D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = buffer,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                       .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE}};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(readback, 0, buffer, 0, 4);
    hr = list->Close();
  }
  if (SUCCEEDED(hr)) hr = queue->Wait(ready_fence, 1);
  if (SUCCEEDED(hr)) {
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    hr = queue->Signal(done_fence, 1);
  }
  if (SUCCEEDED(hr)) {
    finished = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = done_fence->SetEventOnCompletion(1, finished);
    if (SUCCEEDED(hr) && WaitForSingleObject(finished, 3000) != WAIT_OBJECT_0)
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  if (SUCCEEDED(hr)) {
    uint32_t* value = nullptr;
    D3D12_RANGE range{0, sizeof(uint32_t)};
    hr = readback->Map(0, &range, reinterpret_cast<void**>(&value));
    if (SUCCEEDED(hr)) {
      *marker_out = *value;
      readback->Unmap(0, nullptr);
    }
  }
  if (finished) CloseHandle(finished);
  if (readback) readback->Release();
  if (list) list->Release();
  if (allocator) allocator->Release();
  if (queue) queue->Release();
  if (done_fence) done_fence->Release();
  if (ready_fence) ready_fence->Release();
  if (buffer) buffer->Release();
  if (done_handle) CloseHandle(done_handle);
  if (ready_handle) CloseHandle(ready_handle);
  if (buffer_handle) CloseHandle(buffer_handle);
  return hr;
}

HRESULT CompileShadowLibrary(IDxcBlob** object_out) {
  if (object_out) *object_out = nullptr;
static constexpr char kSource[] = R"(
struct ShadowPayload { float shadow; float reflection; float gi; uint ray_kind; };
struct ShadowAttributes { float2 barycentrics : SV_Barycentrics; };
RaytracingAccelerationStructure Scene : register(t0);
RWByteAddressBuffer ShadowMask : register(u0);
cbuffer Camera : register(b0) {
  row_major float4x4 InvView;
  row_major float4x4 InvProjection;
  uint LightingMode;
};
[shader("raygeneration")] void RayGen() {
  uint2 pixel = DispatchRaysIndex().xy;
  uint2 dimensions = DispatchRaysDimensions().xy;
  float2 uv = (float2(pixel) + 0.5) / float2(dimensions);
  float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  float4 view_target = mul(float4(ndc, 1.0, 1.0), InvProjection);
  view_target /= view_target.w;
  float3 origin = mul(float4(0.0, 0.0, 0.0, 1.0), InvView).xyz;
  float3 target = mul(float4(view_target.xyz, 1.0), InvView).xyz;
  ShadowPayload payload = {1.0, 0.0, 1.0, 0};
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = normalize(target - origin);
  ray.TMin = 0.05; ray.TMax = 10000.0;
  TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
  uint offset = (pixel.y * dimensions.x + pixel.x) * 16;
  ShadowMask.Store4(offset, asuint(float4(payload.shadow, payload.reflection,
                                          payload.gi,
                                          payload.ray_kind == 4 ? 1.0 : 0.0)));
}
[shader("miss")] void Miss(inout ShadowPayload payload) { }
[shader("closesthit")] void ClosestHit(inout ShadowPayload payload,
                                        in ShadowAttributes attributes) {
  if (payload.ray_kind == 1) { payload.shadow = 0.0; return; }
  if (payload.ray_kind == 2) { payload.reflection = 1.0; return; }
  if (payload.ray_kind == 3) { payload.gi = 0.0; return; }
  float3 hit = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
  ShadowPayload shadow = {1.0, 0.0, 1.0, 1};
  RayDesc ray;
  ray.Direction = normalize(float3(0.35, 0.25, 0.90));
  ray.Origin = hit + ray.Direction * 0.05;
  ray.TMin = 0.02; ray.TMax = 10000.0;
  TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF,
           0, 0, 0, ray, shadow);
  payload.shadow = shadow.shadow;
  float3 normal = float3(0.0, 0.0, 1.0);
  if (LightingMode >= 3) {
    ShadowPayload reflection = {1.0, 0.0, 1.0, 2};
    ray.Direction = normalize(reflect(WorldRayDirection(), normal));
    ray.Origin = hit + ray.Direction * 0.05;
    TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF,
             0, 0, 0, ray, reflection);
    payload.reflection = reflection.reflection;
  }
  if (LightingMode >= 4) {
    ShadowPayload gi = {1.0, 0.0, 1.0, 3};
    ray.Direction = normalize(normal + float3(0.31, 0.17, 0.53));
    ray.Origin = hit + ray.Direction * 0.05;
    TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF,
             0, 0, 0, ray, gi);
    payload.gi = gi.gi;
  }
  payload.ray_kind = 4;
}
)";
  IDxcUtils* utils = nullptr;
  IDxcCompiler3* compiler = nullptr;
  IDxcBlobEncoding* source = nullptr;
  IDxcResult* result = nullptr;
  HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
  if (SUCCEEDED(hr)) hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
  if (SUCCEEDED(hr)) hr = utils->CreateBlob(kSource, sizeof(kSource) - 1, DXC_CP_UTF8, &source);
  if (SUCCEEDED(hr)) {
    DxcBuffer buffer{.Ptr = source->GetBufferPointer(), .Size = source->GetBufferSize(),
                     .Encoding = DXC_CP_UTF8};
    const wchar_t* args[] = {L"-T", L"lib_6_3", L"-HV", L"2021"};
    hr = compiler->Compile(&buffer, args, _countof(args), nullptr, IID_PPV_ARGS(&result));
  }
  HRESULT compile_hr = E_FAIL;
  if (SUCCEEDED(hr)) hr = result->GetStatus(&compile_hr);
  if (SUCCEEDED(hr) && FAILED(compile_hr)) {
    IDxcBlobUtf8* errors = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors && errors->GetStringLength()) {
      std::cerr << "DXC: " << errors->GetStringPointer() << "\n";
    }
    if (errors) errors->Release();
  }
  if (SUCCEEDED(hr)) hr = compile_hr;
  if (SUCCEEDED(hr) && object_out) {
    hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object_out), nullptr);
  }
  if (result) result->Release();
  if (source) source->Release();
  if (compiler) compiler->Release();
  if (utils) utils->Release();
  return hr;
}

HRESULT CreateShadowPipeline(ID3D12Device* device, ID3D12StateObject** state_out,
                             ID3D12RootSignature** root_out) {
  *state_out = nullptr;
  if (root_out) *root_out = nullptr;
  IDxcBlob* library_blob = nullptr;
  HRESULT hr = CompileShadowLibrary(&library_blob);
  ID3D12Device5* device5 = nullptr;
  ID3D12RootSignature* root_signature = nullptr;
  if (SUCCEEDED(hr)) hr = device->QueryInterface(IID_PPV_ARGS(&device5));
  D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = 1,
       .BaseShaderRegister = 0, .RegisterSpace = 0,
       .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
      {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, .NumDescriptors = 1,
       .BaseShaderRegister = 0, .RegisterSpace = 0,
       .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};
  D3D12_ROOT_PARAMETER parameters[2] = {
      {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
       .DescriptorTable = {.NumDescriptorRanges = _countof(ranges), .pDescriptorRanges = ranges},
       .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL},
      {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
       .Constants = {.ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = 33},
       .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL}};
  D3D12_ROOT_SIGNATURE_DESC root_desc = {.NumParameters = _countof(parameters), .pParameters = parameters,
      .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob* serialized = nullptr;
  ID3DBlob* errors = nullptr;
  if (SUCCEEDED(hr)) hr = D3D12SerializeRootSignature(&root_desc,
      D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
  if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, serialized->GetBufferPointer(),
      serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature));
  if (errors) errors->Release();
  if (serialized) serialized->Release();
  if (SUCCEEDED(hr)) {
    D3D12_EXPORT_DESC exports[] = {{.Name = L"RayGen"}, {.Name = L"Miss"},
                                   {.Name = L"ClosestHit"}};
    D3D12_DXIL_LIBRARY_DESC library = {{library_blob->GetBufferPointer(),
        library_blob->GetBufferSize()}, _countof(exports), exports};
    D3D12_HIT_GROUP_DESC hit_group = {};
    hit_group.HitGroupExport = L"ShadowHitGroup";
    hit_group.ClosestHitShaderImport = L"ClosestHit";
    hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    D3D12_RAYTRACING_SHADER_CONFIG shader_config = {.MaxPayloadSizeInBytes = 16,
                                                     .MaxAttributeSizeInBytes = 8};
    D3D12_GLOBAL_ROOT_SIGNATURE global_root = {.pGlobalRootSignature = root_signature};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config = {.MaxTraceRecursionDepth = 2};
    D3D12_STATE_SUBOBJECT subs[] = {
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, .pDesc = &library},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, .pDesc = &hit_group},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, .pDesc = &shader_config},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, .pDesc = &global_root},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, .pDesc = &pipeline_config}};
    D3D12_STATE_OBJECT_DESC desc = {.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                                    .NumSubobjects = _countof(subs), .pSubobjects = subs};
    hr = device5->CreateStateObject(&desc, IID_PPV_ARGS(state_out));
  }
  if (root_out && root_signature) {
    *root_out = root_signature;
  } else if (root_signature) {
    root_signature->Release();
  }
  if (device5) device5->Release();
  if (library_blob) library_blob->Release();
  return hr;
}

HRESULT DispatchShadowSmoke(ID3D12Device* device, ID3D12CommandQueue* queue,
                            ID3D12StateObject* pipeline) {
  ID3D12StateObjectProperties* properties = nullptr;
  HRESULT hr = pipeline->QueryInterface(IID_PPV_ARGS(&properties));
  const uint64_t record_size = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
  ID3D12Resource* sbt = nullptr;
  if (SUCCEEDED(hr)) hr = CreateUploadBuffer(device, record_size * 3, &sbt);
  if (SUCCEEDED(hr)) {
    uint8_t* mapped = nullptr;
    hr = sbt->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (SUCCEEDED(hr)) {
      memcpy(mapped, properties->GetShaderIdentifier(L"RayGen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      memcpy(mapped + record_size, properties->GetShaderIdentifier(L"Miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      memcpy(mapped + record_size * 2, properties->GetShaderIdentifier(L"ShadowHitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      sbt->Unmap(0, nullptr);
    }
  }
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* base = nullptr;
  ID3D12GraphicsCommandList4* list = nullptr;
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&base));
  if (SUCCEEDED(hr)) hr = base->QueryInterface(IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr)) {
    const auto address = sbt->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dispatch = {};
    dispatch.RayGenerationShaderRecord = {.StartAddress = address, .SizeInBytes = record_size};
    dispatch.MissShaderTable = {.StartAddress = address + record_size, .SizeInBytes = record_size, .StrideInBytes = record_size};
    dispatch.HitGroupTable = {.StartAddress = address + record_size * 2, .SizeInBytes = record_size, .StrideInBytes = record_size};
    dispatch.Width = 1; dispatch.Height = 1; dispatch.Depth = 1;
    list->SetPipelineState1(pipeline);
    list->DispatchRays(&dispatch);
    hr = list->Close();
  }
  if (SUCCEEDED(hr)) { ID3D12CommandList* lists[] = {list}; queue->ExecuteCommandLists(1, lists); }
  if (list) list->Release(); if (base) base->Release(); if (allocator) allocator->Release();
  if (sbt) sbt->Release(); if (properties) properties->Release();
  return hr;
}

HRESULT DispatchShadowMask(ID3D12Device* device, ID3D12CommandQueue* queue,
                           ID3D12StateObject* pipeline, ID3D12RootSignature* root,
                           ID3D12Resource* tlas, ID3D12Resource* output,
                           const Dx8to12::RtIpc::Handshake& camera) {
  ID3D12DescriptorHeap* heap = nullptr;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {
      .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
      .NumDescriptors = 2,
      .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  HRESULT hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
  if (!tlas || !output) hr = E_INVALIDARG;
  if (SUCCEEDED(hr)) {
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = device->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {
        .ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.RaytracingAccelerationStructure.Location = tlas->GetGPUVirtualAddress();
    device->CreateShaderResourceView(nullptr, &srv, cpu);
    cpu.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER};
    uav.Buffer.NumElements = Dx8to12::RtIpc::kRtPayloadBytes;
    uav.Buffer.StructureByteStride = 0;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
  }

  ID3D12StateObjectProperties* properties = nullptr;
  ID3D12Resource* sbt = nullptr;
  if (SUCCEEDED(hr)) hr = pipeline->QueryInterface(IID_PPV_ARGS(&properties));
  const uint64_t record_size = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
  if (SUCCEEDED(hr)) hr = CreateUploadBuffer(device, record_size * 3, &sbt);
  if (SUCCEEDED(hr)) {
    uint8_t* mapped = nullptr;
    hr = sbt->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (SUCCEEDED(hr)) {
      memcpy(mapped, properties->GetShaderIdentifier(L"RayGen"),
             D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      memcpy(mapped + record_size, properties->GetShaderIdentifier(L"Miss"),
             D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      memcpy(mapped + record_size * 2,
             properties->GetShaderIdentifier(L"ShadowHitGroup"),
             D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      sbt->Unmap(0, nullptr);
    }
  }

  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* base = nullptr;
  ID3D12GraphicsCommandList4* list = nullptr;
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
      IID_PPV_ARGS(&base));
  if (SUCCEEDED(hr)) hr = base->QueryInterface(IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr)) {
    const D3D12_RESOURCE_BARRIER to_uav = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = output,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                       .StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS}};
    list->ResourceBarrier(1, &to_uav);
    ID3D12DescriptorHeap* heaps[] = {heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root);
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1, 16, camera.scene_view, 0);
    list->SetComputeRoot32BitConstants(1, 16, camera.scene_projection, 16);
    list->SetComputeRoot32BitConstant(1, camera.scene_lighting_mode, 32);
    list->SetPipelineState1(pipeline);
    const auto address = sbt->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dispatch = {};
    dispatch.RayGenerationShaderRecord = {address, record_size};
    dispatch.MissShaderTable = {address + record_size, record_size, record_size};
    dispatch.HitGroupTable = {address + record_size * 2, record_size, record_size};
    dispatch.Width = Dx8to12::RtIpc::kShadowOutputWidth;
    dispatch.Height = Dx8to12::RtIpc::kShadowOutputHeight;
    dispatch.Depth = 1;
    list->DispatchRays(&dispatch);
    const D3D12_RESOURCE_BARRIER to_common = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = output,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       .StateAfter = D3D12_RESOURCE_STATE_COMMON}};
    list->ResourceBarrier(1, &to_common);
    hr = list->Close();
  }
  if (SUCCEEDED(hr)) {
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    hr = WaitForQueueIdle(device, queue);
  }
  if (list) list->Release();
  if (base) base->Release();
  if (allocator) allocator->Release();
  if (sbt) sbt->Release();
  if (properties) properties->Release();
  if (heap) heap->Release();
  return hr;
}

HRESULT ReadbackShadowMask(ID3D12Device* device, ID3D12CommandQueue* queue,
                           ID3D12Resource* output,
                           Dx8to12::RtIpc::Handshake* response) {
  const uint64_t readback_size =
      uint64_t(Dx8to12::RtIpc::kRtPayloadBytes) * sizeof(uint32_t);
  const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_READBACK};
  const D3D12_RESOURCE_DESC desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Width = readback_size,
      .Height = 1,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  ID3D12Resource* readback = nullptr;
  HRESULT hr = device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&readback));
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
      IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr)) {
    const D3D12_RESOURCE_BARRIER to_copy = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = output,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                       .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE}};
    list->ResourceBarrier(1, &to_copy);
    list->CopyBufferRegion(readback, 0, output, 0, readback_size);
    const D3D12_RESOURCE_BARRIER to_common = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = output,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
                       .StateAfter = D3D12_RESOURCE_STATE_COMMON}};
    list->ResourceBarrier(1, &to_common);
    hr = list->Close();
  }
  if (SUCCEEDED(hr)) {
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    hr = WaitForQueueIdle(device, queue);
  }
  if (SUCCEEDED(hr)) {
    const uint32_t* values = nullptr;
    const D3D12_RANGE read_range = {0, static_cast<SIZE_T>(readback_size)};
    hr = readback->Map(0, &read_range,
                       reinterpret_cast<void**>(const_cast<uint32_t**>(&values)));
    if (SUCCEEDED(hr)) {
      for (uint32_t i = 0; i < Dx8to12::RtIpc::kRtPayloadBytes; ++i) {
        float visibility = 1.0f;
        memcpy(&visibility, &values[i], sizeof(visibility));
        response->shadow_payload[i] =
            static_cast<uint8_t>(std::clamp(visibility, 0.0f, 1.0f) * 255.0f);
      }
      readback->Unmap(0, nullptr);
      response->shadow_payload_size = Dx8to12::RtIpc::kRtPayloadBytes;
      response->shadow_payload_width = Dx8to12::RtIpc::kShadowOutputWidth;
      response->shadow_payload_height = Dx8to12::RtIpc::kShadowOutputHeight;
      response->shadow_payload_row_pitch =
          Dx8to12::RtIpc::kShadowOutputWidth * Dx8to12::RtIpc::kRtOutputChannels;
      response->shadow_payload_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      ++response->shadow_payload_generation;
    }
  }
  if (list) list->Release();
  if (allocator) allocator->Release();
  if (readback) readback->Release();
  return hr;
}

struct ImportedSceneResource {
  // Protocol v12 creates this upload resource entirely on the helper device
  // from the bounded CPU payload. No x86-created D3D12 resource is opened by
  // the x64 process.
  ID3D12Resource* local_resource = nullptr;
  uint64_t generation = 0;
  uint64_t byte_size = 0;
  std::vector<uint8_t> cpu_data;
};

struct BlasEntry {
  ID3D12Resource* result = nullptr;
  ID3D12Resource* scratch = nullptr;
  uint64_t vertex_generation = 0;
  uint64_t index_generation = 0;
};

struct TlasEntry {
  ID3D12Resource* result = nullptr;
  ID3D12Resource* scratch = nullptr;
  ID3D12Resource* instances = nullptr;
  uint64_t result_size = 0;
  uint64_t scratch_size = 0;
  uint64_t instance_size = 0;
  ~TlasEntry() {
    if (result) result->Release();
    if (scratch) scratch->Release();
    if (instances) instances->Release();
  }
};

uint64_t GeometryKey(const Dx8to12::RtIpc::SceneInstance& instance) {
  auto mix = [](uint64_t hash, uint64_t value) {
    return (hash ^ value) * 1099511628211ull;
  };
  uint64_t hash = 1469598103934665603ull;
  hash = mix(hash, instance.vertex_resource_id);
  hash = mix(hash, instance.index_resource_id);
  hash = mix(hash, instance.vertex_byte_offset);
  hash = mix(hash, instance.index_byte_offset);
  hash = mix(hash, instance.vertex_stride);
  hash = mix(hash, instance.start_index);
  hash = mix(hash, instance.index_count);
  hash = mix(hash, instance.index_format);
  return hash;
}

void ReleaseBlases(std::unordered_map<uint64_t, BlasEntry>& entries) {
  for (auto& [key, entry] : entries) {
    if (entry.result) entry.result->Release();
    if (entry.scratch) entry.scratch->Release();
  }
}

HRESULT CreateAsBuffer(ID3D12Device* device, uint64_t size,
                       D3D12_RESOURCE_STATES state, ID3D12Resource** resource) {
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1,
      .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                         state, nullptr, IID_PPV_ARGS(resource));
}

HRESULT CreateUploadBuffer(ID3D12Device* device, uint64_t size, ID3D12Resource** resource) {
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_UPLOAD};
  const D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
      .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(resource));
}

void ReleaseImportedSceneResources(
    std::unordered_map<uint32_t, ImportedSceneResource>& resources) {
  for (auto& [id, resource] : resources) {
    if (resource.local_resource) resource.local_resource->Release();
  }
  resources.clear();
}

HRESULT ImportCpuSceneResources(
    ID3D12Device* device, const Dx8to12::RtIpc::Handshake& request,
    std::unordered_map<uint32_t, ImportedSceneResource>& resources,
    uint32_t* imported_count) {
  *imported_count = 0;
  if (request.scene_instance_count > Dx8to12::RtIpc::kMaxSceneInstances ||
      request.scene_payload_size > Dx8to12::RtIpc::kMaxCpuScenePayloadBytes) {
    return E_INVALIDARG;
  }
  auto import_one = [&](uint32_t id, uint32_t offset, uint32_t size) -> HRESULT {
    if (id == 0 || size == 0 || offset > request.scene_payload_size ||
        size > request.scene_payload_size - offset || resources.contains(id)) {
      return E_INVALIDARG;
    }
    ID3D12Resource* upload = nullptr;
    HRESULT hr = CreateUploadBuffer(device, size, &upload);
    void* mapped = nullptr;
    if (SUCCEEDED(hr)) {
      const D3D12_RANGE no_read = {0, 0};
      hr = upload->Map(0, &no_read, &mapped);
    }
    if (SUCCEEDED(hr)) {
      memcpy(mapped, request.scene_payload + offset, size);
      const D3D12_RANGE written = {0, size};
      upload->Unmap(0, &written);
      ImportedSceneResource imported;
      imported.local_resource = upload;
      imported.generation = request.scene_sequence;
      imported.byte_size = size;
      imported.cpu_data.assign(request.scene_payload + offset,
                               request.scene_payload + offset + size);
      resources.emplace(id, std::move(imported));
      ++*imported_count;
    } else if (upload) {
      upload->Release();
    }
    return hr;
  };
  for (uint32_t i = 0; i < request.scene_instance_count; ++i) {
    const auto& instance = request.scene_instances[i];
    HRESULT hr = import_one(instance.vertex_resource_id,
                            instance.vertex_payload_offset,
                            instance.vertex_payload_size);
    if (SUCCEEDED(hr)) {
      hr = import_one(instance.index_resource_id,
                      instance.index_payload_offset,
                      instance.index_payload_size);
    }
    if (FAILED(hr)) {
      ReleaseImportedSceneResources(resources);
      return hr;
    }
  }
  return S_OK;
}

// The next scene batch discards the previous batch's BLAS entries and
// rebuilds the TLAS in the same resources.  DispatchRays is asynchronous, so
// merely signalling the cross-process completion fence is not enough to make
// those releases safe: the GPU could still be reading the old TLAS/BLAS.
// Keep this conservative fence wait until the scene owns a multi-frame AS
// lifetime.  Correct resource lifetime is more important than throughput for
// this validation path.
HRESULT WaitForQueueIdle(ID3D12Device* device, ID3D12CommandQueue* queue) {
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (SUCCEEDED(hr)) hr = queue->Signal(fence, 1);
  if (SUCCEEDED(hr)) {
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = event ? fence->SetEventOnCompletion(1, event)
               : HRESULT_FROM_WIN32(GetLastError());
  }
  if (SUCCEEDED(hr) && WaitForSingleObject(event, 3000) != WAIT_OBJECT_0)
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  if (event) CloseHandle(event);
  if (fence) fence->Release();
  return hr;
}

void CopyWorldTransform(const float (&world)[16], float (&out)[3][4]) {
  out[0][0]=world[0]; out[0][1]=world[4]; out[0][2]=world[8]; out[0][3]=world[12];
  out[1][0]=world[1]; out[1][1]=world[5]; out[1][2]=world[9]; out[1][3]=world[13];
  out[2][0]=world[2]; out[2][1]=world[6]; out[2][2]=world[10]; out[2][3]=world[14];
}

// Builds a conservative BLAS only for geometry whose layout is known to be
// position=float3 at byte zero. Input buffers are helper-owned UPLOAD
// resources and stay in GENERIC_READ for their entire lifetime.
HRESULT BuildBlases(
    ID3D12Device* device, ID3D12CommandQueue* queue,
    const Dx8to12::RtIpc::Handshake& request,
    std::unordered_map<uint32_t, ImportedSceneResource>& resources,
    std::unordered_map<uint64_t, BlasEntry>& blases, uint32_t* rebuilt,
    uint32_t* cached, uint32_t* skipped_base_vertex, uint32_t* skipped_format_or_range) {
  *rebuilt = 0;
  *cached = 0;
  *skipped_base_vertex = 0;
  *skipped_format_or_range = 0;
  ID3D12Device5* device5 = nullptr;
  HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device5));
  if (FAILED(hr)) return hr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* base_list = nullptr;
  ID3D12GraphicsCommandList4* list = nullptr;
  hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&base_list));
  if (SUCCEEDED(hr)) hr = base_list->QueryInterface(IID_PPV_ARGS(&list));
  // Keep the helper bounded while the scene selector is still deliberately
  // broad.  Building 20+ dynamic BLAS serially can trip a driver watchdog.
  constexpr uint32_t kMaxBlasBuildsPerBatch = 16;
  for (uint32_t i = 0; SUCCEEDED(hr) && i < request.scene_instance_count &&
                       *rebuilt < kMaxBlasBuildsPerBatch; ++i) {
    const auto& instance = request.scene_instances[i];
    const auto vb_it = resources.find(instance.vertex_resource_id);
    const auto ib_it = resources.find(instance.index_resource_id);
    if (vb_it == resources.end() || ib_it == resources.end() ||
        instance.vertex_stride < 12 ||
        (instance.index_format != DXGI_FORMAT_R16_UINT &&
         instance.index_format != DXGI_FORMAT_R32_UINT)) {
      ++*skipped_format_or_range;
      continue;
    }
    const uint64_t index_size = instance.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4;
    if (instance.index_byte_offset + uint64_t(instance.start_index) * index_size +
            uint64_t(instance.index_count) * index_size > ib_it->second.byte_size) {
      ++*skipped_format_or_range;
      continue;
    }
    const uint8_t* index_data =
        ib_it->second.cpu_data.data() + instance.index_byte_offset +
        uint64_t(instance.start_index) * index_size;
    uint32_t max_index = 0;
    for (uint32_t index = 0; index < instance.index_count; ++index) {
      uint32_t value = 0;
      if (index_size == 2) {
        uint16_t value16 = 0;
        memcpy(&value16, index_data + uint64_t(index) * 2, sizeof(value16));
        value = value16;
      } else {
        memcpy(&value, index_data + uint64_t(index) * 4, sizeof(value));
      }
      max_index = std::max(max_index, value);
    }
    const uint64_t key = GeometryKey(instance);
    auto& entry = blases[key];
    if (entry.result && entry.vertex_generation == vb_it->second.generation &&
        entry.index_generation == ib_it->second.generation) {
      ++*cached;
      continue;
    }
    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    const uint64_t base_vertex_offset =
        uint64_t(instance.base_vertex) * uint64_t(instance.vertex_stride);
    // Derive the exact required range from the copied index bytes. D3D8's
    // MinVertexIndex/NumVertices are app-provided hints and cannot be trusted
    // as a memory-safety boundary for a DXR build.
    const uint64_t vertex_count = uint64_t(max_index) + 1;
    if (instance.vertex_byte_offset >= vb_it->second.byte_size ||
        base_vertex_offset > vb_it->second.byte_size - instance.vertex_byte_offset ||
        instance.index_byte_offset >= ib_it->second.byte_size ||
        vertex_count == 0 ||
        vertex_count * uint64_t(instance.vertex_stride) >
            vb_it->second.byte_size - instance.vertex_byte_offset - base_vertex_offset) {
      ++*skipped_format_or_range;
      continue;
    }
    bool finite_positions = true;
    for (uint32_t index = 0; index < instance.index_count; ++index) {
      uint32_t value = 0;
      if (index_size == 2) {
        uint16_t value16 = 0;
        memcpy(&value16, index_data + uint64_t(index) * 2, sizeof(value16));
        value = value16;
      } else {
        memcpy(&value, index_data + uint64_t(index) * 4, sizeof(value));
      }
      const uint64_t position_offset = instance.vertex_byte_offset +
          base_vertex_offset + uint64_t(value) * instance.vertex_stride;
      float position[3] = {};
      memcpy(position, vb_it->second.cpu_data.data() + position_offset,
             sizeof(position));
      if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
          !std::isfinite(position[2])) {
        finite_positions = false;
        break;
      }
    }
    if (!finite_positions) {
      ++*skipped_format_or_range;
      continue;
    }
    geometry.Triangles.VertexBuffer.StartAddress =
        vb_it->second.local_resource->GetGPUVirtualAddress() + instance.vertex_byte_offset +
        base_vertex_offset;
    geometry.Triangles.VertexBuffer.StrideInBytes = instance.vertex_stride;
    geometry.Triangles.VertexCount = static_cast<UINT>(vertex_count);
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.IndexBuffer = ib_it->second.local_resource->GetGPUVirtualAddress() +
                                    instance.index_byte_offset +
                                    uint64_t(instance.start_index) * index_size;
    geometry.Triangles.IndexCount = instance.index_count;
    geometry.Triangles.IndexFormat = static_cast<DXGI_FORMAT>(instance.index_format);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs_desc = {};
    inputs_desc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs_desc.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs_desc.NumDescs = 1;
    inputs_desc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs_desc.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs_desc, &info);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) continue;
    if (entry.result) entry.result->Release();
    if (entry.scratch) entry.scratch->Release();
    entry = {};
    hr = CreateAsBuffer(device, info.ResultDataMaxSizeInBytes,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                        &entry.result);
    if (SUCCEEDED(hr)) hr = CreateAsBuffer(device, info.ScratchDataSizeInBytes,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           &entry.scratch);
    if (FAILED(hr)) break;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs = inputs_desc;
    build.DestAccelerationStructureData = entry.result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = entry.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    const D3D12_RESOURCE_BARRIER uav = {.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                         .UAV = {.pResource = entry.result}};
    list->ResourceBarrier(1, &uav);
    entry.vertex_generation = vb_it->second.generation;
    entry.index_generation = ib_it->second.generation;
    ++*rebuilt;
  }
  if (SUCCEEDED(hr)) hr = list->Close();
  if (SUCCEEDED(hr) && *rebuilt != 0) {
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* completion = nullptr;
    HANDLE event = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion));
    if (SUCCEEDED(hr)) hr = queue->Signal(completion, 1);
    if (SUCCEEDED(hr)) {
      event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) hr = completion->SetEventOnCompletion(1, event);
    if (SUCCEEDED(hr) && WaitForSingleObject(event, 3000) != WAIT_OBJECT_0)
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (event) CloseHandle(event);
    if (completion) completion->Release();
  }
  if (list) list->Release();
  if (base_list) base_list->Release();
  if (allocator) allocator->Release();
  device5->Release();
  return hr;
}

HRESULT BuildTlas(ID3D12Device* device, ID3D12CommandQueue* queue,
                  const Dx8to12::RtIpc::Handshake& request,
                  const std::unordered_map<uint64_t, BlasEntry>& blases,
                  TlasEntry* tlas, uint32_t* instance_count) {
  std::vector<const BlasEntry*> entries;
  std::vector<const Dx8to12::RtIpc::SceneInstance*> source;
  constexpr uint32_t kMaxTlasInstancesPerBatch = 16;
  for (uint32_t i = 0; i < request.scene_instance_count &&
                       entries.size() < kMaxTlasInstancesPerBatch; ++i) {
    const auto it = blases.find(GeometryKey(request.scene_instances[i]));
    if (it != blases.end() && it->second.result) {
      entries.push_back(&it->second);
      source.push_back(&request.scene_instances[i]);
    }
  }
  *instance_count = static_cast<uint32_t>(entries.size());
  if (entries.empty()) return S_OK;

  const uint64_t bytes = uint64_t(kMaxTlasInstancesPerBatch) *
                         sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
  HRESULT hr = S_OK;
  if (!tlas->instances) {
    tlas->instance_size = bytes;
    hr = CreateUploadBuffer(device, bytes, &tlas->instances);
    if (FAILED(hr)) return hr;
  }
  D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
  hr = tlas->instances->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  if (FAILED(hr)) return hr;
  for (size_t i = 0; i < entries.size(); ++i) {
    mapped[i] = {};
    CopyWorldTransform(source[i]->world, mapped[i].Transform);
    mapped[i].InstanceID = static_cast<UINT>(i);
    mapped[i].InstanceMask = 0xFF;
    mapped[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    mapped[i].AccelerationStructure =
        entries[i]->result->GetGPUVirtualAddress();
  }
  tlas->instances->Unmap(0,nullptr);

  ID3D12Device5* device5 = nullptr;
  hr = device->QueryInterface(IID_PPV_ARGS(&device5));
  if (FAILED(hr)) return hr;
  if (!tlas->result || !tlas->scratch) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS sizing = {};
    sizing.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    sizing.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    sizing.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    sizing.NumDescs = kMaxTlasInstancesPerBatch;
    sizing.InstanceDescs = tlas->instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&sizing, &info);
    if (!tlas->result) {
      tlas->result_size = info.ResultDataMaxSizeInBytes;
      hr = CreateAsBuffer(
          device, tlas->result_size,
          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
          &tlas->result);
    }
    if (SUCCEEDED(hr) && !tlas->scratch) {
      tlas->scratch_size = info.ScratchDataSizeInBytes;
      hr = CreateAsBuffer(device, tlas->scratch_size,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          &tlas->scratch);
    }
  }
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  inputs.Flags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.NumDescs = *instance_count;
  inputs.InstanceDescs = tlas->instances->GetGPUVirtualAddress();
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* base = nullptr;
  ID3D12GraphicsCommandList4* list = nullptr;
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
      IID_PPV_ARGS(&base));
  if (SUCCEEDED(hr)) hr = base->QueryInterface(IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr)) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = tlas->result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData =
        tlas->scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    const D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
        .UAV = {.pResource = tlas->result}};
    list->ResourceBarrier(1, &barrier);
    hr = list->Close();
  }
  if (SUCCEEDED(hr)) {
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    hr = WaitForQueueIdle(device, queue);
  }
  if (list) list->Release();
  if (base) base->Release();
  if (allocator) allocator->Release();
  device5->Release();
  return hr;
}

int RunSceneSelfTest() {
  ID3D12Debug* debug = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
    debug->EnableDebugLayer();
  }
  IDXGIFactory6* factory = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12StateObject* pipeline = nullptr;
  ID3D12RootSignature* root = nullptr;
  ID3D12Resource* output = nullptr;
  std::unordered_map<uint32_t, ImportedSceneResource> resources;
  std::unordered_map<uint64_t, BlasEntry> blases;
  TlasEntry tlas;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) hr = factory->EnumAdapterByGpuPreference(
      0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  if (SUCCEEDED(hr)) hr = D3D12CreateDevice(
      adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  const D3D12_COMMAND_QUEUE_DESC queue_desc = {
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(
      &queue_desc, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(hr)) hr = CreateShadowPipeline(device, &pipeline, &root);

  auto request = std::make_unique<Dx8to12::RtIpc::Handshake>();
  request->scene_sequence = 1;
  request->scene_lighting_mode = 4;
  request->scene_instance_count = 1;
  auto& instance = request->scene_instances[0];
  instance.vertex_resource_id = 1;
  instance.index_resource_id = 2;
  instance.vertex_stride = sizeof(float) * 3;
  instance.index_count = 6;
  instance.num_vertices = 6;
  instance.index_format = DXGI_FORMAT_R16_UINT;
  instance.vertex_payload_offset = 0;
  instance.vertex_payload_size = sizeof(float) * 18;
  instance.index_payload_offset = instance.vertex_payload_size;
  instance.index_payload_size = sizeof(uint16_t) * 6;
  instance.world[0] = instance.world[5] = instance.world[10] =
      instance.world[15] = 1.0f;
  const float vertices[] = {
      -10.0f, -10.0f, 5.0f, 0.0f, 10.0f, 5.0f,
      10.0f, -10.0f, 5.0f,
      -1.6f, -1.7f, 6.0f, 0.4f, 2.3f, 6.0f, 2.4f, -1.7f, 6.0f};
  const uint16_t indices[] = {0, 1, 2, 3, 4, 5};
  memcpy(request->scene_payload, vertices, sizeof(vertices));
  memcpy(request->scene_payload + instance.index_payload_offset, indices,
         sizeof(indices));
  request->scene_payload_size =
      instance.index_payload_offset + instance.index_payload_size;
  for (uint32_t i : {0u, 5u, 10u, 15u}) {
    request->scene_view[i] = 1.0f;
    request->scene_projection[i] = 1.0f;
  }

  uint32_t imported = 0;
  uint32_t rebuilt = 0;
  uint32_t cached = 0;
  uint32_t skipped_base = 0;
  uint32_t skipped_range = 0;
  uint32_t tlas_instances = 0;
  if (SUCCEEDED(hr)) hr = ImportCpuSceneResources(
      device, *request, resources, &imported);
  if (SUCCEEDED(hr)) hr = BuildBlases(
      device, queue, *request, resources, blases, &rebuilt, &cached,
      &skipped_base, &skipped_range);
  if (SUCCEEDED(hr)) hr = BuildTlas(
      device, queue, *request, blases, &tlas, &tlas_instances);
  if (SUCCEEDED(hr)) {
    hr = CreateAsBuffer(
        device,
        uint64_t(Dx8to12::RtIpc::kRtPayloadBytes) * sizeof(uint32_t),
        D3D12_RESOURCE_STATE_COMMON, &output);
  }
  if (SUCCEEDED(hr)) hr = DispatchShadowMask(
      device, queue, pipeline, root, tlas.result, output, *request);
  if (SUCCEEDED(hr)) hr = ReadbackShadowMask(
      device, queue, output, request.get());

  uint32_t dark = 0;
  uint32_t lit = 0;
  if (SUCCEEDED(hr)) {
    for (uint8_t value : request->shadow_payload) {
      value < 128 ? ++dark : ++lit;
    }
  }
  std::wcout << L"RTHELPER-SCENE-SELFTEST hr=0x" << std::hex << hr
             << L" imported=" << std::dec << imported
             << L" blas=" << rebuilt << L" tlas=" << tlas_instances
             << L" dark=" << dark << L" lit=" << lit << L"\n";

  const HRESULT removed_reason =
      device ? device->GetDeviceRemovedReason() : E_POINTER;
  std::wcout << L"RTHELPER-SCENE-SELFTEST device-removed=0x" << std::hex
             << removed_reason << L"\n";

  if (output) output->Release();
  ReleaseBlases(blases);
  ReleaseImportedSceneResources(resources);
  if (pipeline) pipeline->Release();
  if (root) root->Release();
  if (queue) queue->Release();
  if (device) device->Release();
  if (adapter) adapter->Release();
  if (factory) factory->Release();
  if (debug) debug->Release();
  return SUCCEEDED(hr) && imported == 2 && rebuilt == 1 &&
                 tlas_instances == 1 && request->shadow_payload_generation == 1 &&
                 SUCCEEDED(removed_reason)
             ? 0
             : 6;
}

int RunHandshake(const wchar_t* map_name, const wchar_t* ready_event_name,
                 const wchar_t* shutdown_event_name, const wchar_t* work_event_name,
                 const wchar_t* done_event_name) {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
  HANDLE ready_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_event_name);
  HANDLE shutdown_event = OpenEventW(SYNCHRONIZE, FALSE, shutdown_event_name);
  HANDLE work_event = OpenEventW(SYNCHRONIZE, FALSE, work_event_name);
  HANDLE done_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, done_event_name);
  if (!mapping || !ready_event || !shutdown_event || !work_event || !done_event) return 10;
  auto* handshake = static_cast<Dx8to12::RtIpc::Handshake*>(
      MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                    sizeof(Dx8to12::RtIpc::Handshake)));
  if (!handshake || handshake->magic != Dx8to12::RtIpc::kMagic ||
      handshake->version != Dx8to12::RtIpc::kVersion) {
    if (handshake) {
      handshake->status = static_cast<uint32_t>(
          Dx8to12::RtIpc::HelperStatus::kProtocolMismatch);
    }
    SetEvent(ready_event);
    return 11;
  }

  handshake->helper_process_id = GetCurrentProcessId();
  IDXGIFactory6* factory = nullptr;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  IDXGIAdapter1* selected = nullptr;
  if (SUCCEEDED(hr)) {
    for (UINT i = 0;; ++i) {
      IDXGIAdapter1* candidate = nullptr;
      if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc = {};
      candidate->GetDesc1(&desc);
      if (desc.AdapterLuid.LowPart == handshake->adapter_luid_low &&
          desc.AdapterLuid.HighPart == handshake->adapter_luid_high) {
        selected = candidate;
        break;
      }
      candidate->Release();
    }
  }
  if (factory) factory->Release();
  if (!selected) {
    handshake->status = static_cast<uint32_t>(
        Dx8to12::RtIpc::HelperStatus::kAdapterNotFound);
    handshake->hresult = hr;
    SetEvent(ready_event);
    return 12;
  }
  ID3D12Device* device = nullptr;
  hr = D3D12CreateDevice(selected, D3D_FEATURE_LEVEL_11_0,
                         IID_PPV_ARGS(&device));
  selected->Release();
  if (FAILED(hr)) {
    handshake->status = static_cast<uint32_t>(
        Dx8to12::RtIpc::HelperStatus::kDeviceCreateFailed);
    handshake->hresult = hr;
    SetEvent(ready_event);
    return 13;
  }
  ID3D12StateObject* shadow_pipeline = nullptr;
  ID3D12RootSignature* shadow_root = nullptr;
  ID3D12Resource* shadow_output = nullptr;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
  hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                   sizeof(options5));
  handshake->hresult = hr;
  handshake->raytracing_tier = options5.RaytracingTier;
  handshake->status = static_cast<uint32_t>(
      SUCCEEDED(hr) && options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0
          ? Dx8to12::RtIpc::HelperStatus::kReady
          : SUCCEEDED(hr) ? Dx8to12::RtIpc::HelperStatus::kRaytracingUnavailable
                          : Dx8to12::RtIpc::HelperStatus::kOptions5Failed);
  if (handshake->status == static_cast<uint32_t>(Dx8to12::RtIpc::HelperStatus::kReady)) {
    const HRESULT pipeline_hr = CreateShadowPipeline(device, &shadow_pipeline, &shadow_root);
    if (FAILED(pipeline_hr)) {
      handshake->hresult = pipeline_hr;
      handshake->status = static_cast<uint32_t>(Dx8to12::RtIpc::HelperStatus::kOptions5Failed);
    }
  }
  if (handshake->status == static_cast<uint32_t>(Dx8to12::RtIpc::HelperStatus::kReady)) {
    const uint64_t output_bytes =
        uint64_t(Dx8to12::RtIpc::kRtPayloadBytes) * sizeof(uint32_t);
    hr = CreateAsBuffer(device, output_bytes, D3D12_RESOURCE_STATE_COMMON,
                        &shadow_output);
    if (FAILED(hr)) {
      handshake->hresult = hr;
      handshake->status = static_cast<uint32_t>(Dx8to12::RtIpc::HelperStatus::kDeviceCreateFailed);
    }
  }
  SetEvent(ready_event);
  if (handshake->status !=
      static_cast<uint32_t>(Dx8to12::RtIpc::HelperStatus::kReady)) {
    return 14;
  }
  // The scene queue and every resource consumed by DXR belong exclusively to
  // this x64 device. Geometry arrives as bytes in the shared file mapping.
  ID3D12CommandQueue* scene_queue = nullptr;
  const D3D12_COMMAND_QUEUE_DESC scene_queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  hr = device->CreateCommandQueue(&scene_queue_desc, IID_PPV_ARGS(&scene_queue));
  handshake->shadow_smoke_hresult = hr;
  std::unordered_map<uint32_t, ImportedSceneResource> scene_resources;
  std::unordered_map<uint64_t, BlasEntry> blases;
  TlasEntry tlas;
  uint32_t completed = 0;
  HANDLE events[] = {shutdown_event, work_event};
  for (;;) {
    const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) break;
    if (wait != WAIT_OBJECT_0 + 1) break;
    if (handshake->scene_sequence != handshake->scene_completed_sequence) {
      HRESULT scene_hr = scene_queue ? S_OK : hr;
      handshake->scene_shadow_visibility_bits = 0;
      handshake->shadow_done_fence_value = 0;
      uint32_t opened = 0;
      // BuildBlases waits for its work to finish, so resources from the prior
      // batch can be released before importing the next immutable snapshot.
      ReleaseBlases(blases);
      blases.clear();
      ReleaseImportedSceneResources(scene_resources);
      if (SUCCEEDED(scene_hr)) {
        scene_hr = ImportCpuSceneResources(device, *handshake, scene_resources,
                                           &opened);
      }
      uint32_t rebuilt = 0;
      uint32_t cached = 0;
      uint32_t skipped_base_vertex = 0;
      uint32_t skipped_format_or_range = 0;
      if (SUCCEEDED(scene_hr)) {
        scene_hr = BuildBlases(device, scene_queue, *handshake, scene_resources,
                               blases, &rebuilt, &cached, &skipped_base_vertex,
                               &skipped_format_or_range);
      }
      // Attempt #1: VertexCount bug (fixed). Attempt #2: InstanceMask=0 +
      // blases-cleared-every-batch bugs found and fixed; ran 121+ clean
      // dispatched batches of dynamic geometry, then removed the device
      // the instant the first static-geometry batch got dispatched -- the
      // first time this project mixed BLAS entries from two mirror sources
      // in one dispatched TLAS. Attempt #3 (those two fixes applied): the
      // first static mesh now mixed with dynamic geometry cleanly for
      // ~30s -- real progress -- but device removal (3rd this session)
      // still happened within ~7ms of a SECOND distinct static mesh
      // joining the already-dispatching cycle. Code audit afterward found
      // BuildTlas's instances/result/scratch buffers were sized for
      // entries.size() *that batch* rather than a fixed worst case --
      // as more distinct BLAS entries became simultaneously valid
      // (especially after the caching fix let static geometry persist),
      // entries.size() could grow past an earlier batch's allocation and
      // trigger a Release()+recreate cycle on these buffers mid-session,
      // right around the kind of moment all 3 removals coincided with.
      // Fixed above: sized for the fixed kMaxTlasInstancesPerBatch cap
      // once, eliminating that resize cycle after the first batch.
      // Attempt #4 (this fix, tested 2026-08-29): ALSO removed the device
      // -- same DXGI_ERROR_DRIVER_INTERNAL_ERROR, same device.cpp Present
      // assertion. So the TLAS-buffer-resize hypothesis was not the (sole)
      // cause either. 4 failed dispatch attempts + 1 full system crash
      // total this investigation -- stopped here per explicit user
      // decision; needs live debugging of dx8to12_rt_helper.exe (cut short
      // once already by the system crash) in a fresh, lower-stakes
      // session, not another guess-and-check code-audit cycle. See Dx8to12
      // project memory dx8to12-mod-api-and-h3 for the full incident
      // history before attempting a 5th retry.
      // Attempt #5 (2026-08-29): re-enabled under a live debugger this
      // time (cdbX64.exe attached to this process), not another blind
      // code-audit guess -- see project memory dx8to12-mod-api-and-h3.
      // Protocol v13 has no cross-device D3D12 resources in this path:
      // geometry and the R8 result cross the process boundary as bounded CPU
      // bytes, while BLAS/TLAS/Dispatch all stay on the helper's x64 device.
      constexpr bool kEnableTlasAndDispatch = true;
      uint32_t tlas_instances = 0;
      if (kEnableTlasAndDispatch && SUCCEEDED(scene_hr)) {
        scene_hr = BuildTlas(device, scene_queue, *handshake, blases, &tlas,
                             &tlas_instances);
      }
      if (kEnableTlasAndDispatch && SUCCEEDED(scene_hr) && tlas_instances != 0) {
        scene_hr = DispatchShadowMask(device, scene_queue, shadow_pipeline,
                                      shadow_root, tlas.result, shadow_output,
                                      *handshake);
        if (SUCCEEDED(scene_hr)) {
          scene_hr = ReadbackShadowMask(device, scene_queue, shadow_output,
                                        handshake);
        }
        if (SUCCEEDED(scene_hr)) {
          handshake->scene_shadow_visibility_bits = 0x4B53414Du;
        }
      }
      handshake->scene_hresult = scene_hr;
      handshake->scene_opened_resource_count = opened;
      handshake->scene_blas_rebuilt = rebuilt;
      handshake->scene_blas_cached = cached;
      handshake->scene_blas_skipped_base_vertex = skipped_base_vertex;
      handshake->scene_blas_skipped_format_or_range = skipped_format_or_range;
      handshake->scene_tlas_instances = tlas_instances;
      handshake->scene_completed_sequence = handshake->scene_sequence;
      SetEvent(done_event);
      continue;
    }
    if (handshake->command_sequence == completed ||
        handshake->command != static_cast<uint32_t>(Dx8to12::RtIpc::Command::kSmokeBuffer))
      continue;
    uint32_t marker = 0;
    const HRESULT command_hr = RunSmoke(device, *handshake, &marker);
    handshake->command_hresult = command_hr;
    handshake->smoke_value = marker;
    completed = handshake->command_sequence;
    handshake->completed_sequence = completed;
    SetEvent(done_event);
  }
  ReleaseBlases(blases);
  ReleaseImportedSceneResources(scene_resources);
  if (shadow_output) shadow_output->Release();
  if (scene_queue) scene_queue->Release();
  if (shadow_pipeline) shadow_pipeline->Release();
  if (shadow_root) shadow_root->Release();
  device->Release();
  UnmapViewOfFile(handshake);
  CloseHandle(done_event);
  CloseHandle(work_event);
  CloseHandle(shutdown_event);
  CloseHandle(ready_event);
  CloseHandle(mapping);
  return 0;
}

}  // namespace

// D0 from DLSS_X64_HELPER_HANDOFF.md: report which NVIDIA features this
// machine actually offers, and never let their absence stop the game.
//
// Deliberately a standalone mode rather than something wired into
// RunHandshake. Streamline is normally linked through sl.interposer.lib,
// which routes the process's D3D12 entry points through Streamline -- that
// would change the behaviour of the existing, working RT path for every
// session, including ones that never asked for DLSS. Loading the interposer
// dynamically, only in this mode, keeps that path byte-for-byte as it is.
#ifdef DX8TO12_HAVE_STREAMLINE
namespace {

// sl::getResultAsStr returns narrow text; std::wcout has no overload for it.
std::wstring result_to_wstring(sl::Result result) {
  const char* text = sl::getResultAsStr(result);
  return std::wstring(text, text + strlen(text));
}

void PrintSlResult(const wchar_t* what, sl::Result result) {
  std::wcout << what << L": " << result_to_wstring(result) << L"\n";
}

}  // namespace

int RunDlssProbe() {
  std::wcout << L"Streamline headers: " << SL_VERSION_MAJOR << L"."
             << SL_VERSION_MINOR << L"." << SL_VERSION_PATCH << L"\n";

  // Loaded by name from the helper's own directory: the SDK's runtime DLLs are
  // copied next to the executable at build time (see rt_helper/CMakeLists.txt).
  HMODULE interposer = LoadLibraryW(L"sl.interposer.dll");
  if (!interposer) {
    std::wcout << L"sl.interposer.dll not loadable (error "
               << GetLastError()
               << L") -- DLSS unavailable, everything else still works.\n";
    return 0;
  }

  auto sl_init = reinterpret_cast<PFun_slInit*>(
      GetProcAddress(interposer, "slInit"));
  auto sl_shutdown = reinterpret_cast<PFun_slShutdown*>(
      GetProcAddress(interposer, "slShutdown"));
  auto sl_is_supported = reinterpret_cast<PFun_slIsFeatureSupported*>(
      GetProcAddress(interposer, "slIsFeatureSupported"));
  auto sl_get_version = reinterpret_cast<PFun_slGetFeatureVersion*>(
      GetProcAddress(interposer, "slGetFeatureVersion"));
  if (!sl_init || !sl_shutdown || !sl_is_supported) {
    std::wcout << L"sl.interposer.dll is missing expected exports -- DLSS "
                  L"unavailable.\n";
    FreeLibrary(interposer);
    return 0;
  }

  const sl::Feature features[] = {sl::kFeatureDLSS};
  sl::Preferences prefs{};
  prefs.featuresToLoad = features;
  prefs.numFeaturesToLoad = _countof(features);
  prefs.renderAPI = sl::RenderAPI::eD3D12;
  prefs.engine = sl::EngineType::eCustom;
  prefs.engineVersion = "0.1";
  // Identifies this integration to Streamline. Not an NVIDIA-issued
  // applicationId -- the SDK accepts engine+projectId instead, which is the
  // correct path for a project that has not been through NVIDIA onboarding.
  prefs.projectId = "7c9f1e2a-4b3d-4f8a-9c15-2e6d8b0a41f3";
  prefs.logLevel = sl::LogLevel::eOff;
  prefs.pathToLogsAndData = nullptr;

  const sl::Result init_result = sl_init(prefs, sl::kSDKVersion);
  PrintSlResult(L"slInit", init_result);
  if (init_result != sl::Result::eOk) {
    FreeLibrary(interposer);
    // Still not an error for the caller: the game must run without DLSS.
    return 0;
  }

  if (sl_get_version) {
    sl::FeatureVersion version{};
    const sl::Result version_result = sl_get_version(sl::kFeatureDLSS, version);
    if (version_result == sl::Result::eOk) {
      std::wcout << L"DLSS plugin: sl=" << version.versionSL.toWStr()
                 << L" ngx=" << version.versionNGX.toWStr() << L"\n";
    } else {
      PrintSlResult(L"slGetFeatureVersion", version_result);
    }
  }

  // Support is per adapter, so ask about each hardware adapter rather than
  // assuming the first one is the one the game will end up on.
  IDXGIFactory6* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
          !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        sl::AdapterInfo info{};
        info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        info.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
        const sl::Result supported =
            sl_is_supported(sl::kFeatureDLSS, info);
        std::wcout << L"adapter[" << i << L"] " << desc.Description
                   << L" DLSS: " << result_to_wstring(supported) << L"\n";
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
  }

  PrintSlResult(L"slShutdown", sl_shutdown());
  FreeLibrary(interposer);
  return 0;
}
#else
int RunDlssProbe() {
  std::wcout << L"Built without the Streamline SDK. Drop it into "
                L"third_party/streamline and reconfigure to enable DLSS "
                L"probing; the game runs either way.\n";
  return 0;
}
#endif  // DX8TO12_HAVE_STREAMLINE

// x64 half of the cross-process shared-texture test (tools/share_test).
// Opens the x86-created Texture2D and fences, then for each iteration waits
// for "input ready", writes the whole texture, and signals "output ready" --
// the DLSS D1 round trip with the upscaler replaced by a clear.
int RunShareTest(const wchar_t* tex_name, const wchar_t* ready_name,
                 const wchar_t* done_name, uint32_t luid_low, int32_t luid_high,
                 int iterations) {
  IDXGIFactory6* factory = nullptr;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;

  // Same GPU as the x86 side, matched by LUID rather than by enumeration
  // order -- the two processes do not necessarily enumerate identically.
  IDXGIAdapter1* adapter = nullptr;
  ID3D12Device* device = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.AdapterLuid.LowPart == luid_low &&
        desc.AdapterLuid.HighPart == luid_high &&
        SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&device)))) {
      break;
    }
    adapter->Release();
    adapter = nullptr;
  }
  if (!device) {
    std::wcerr << L"share-test: no adapter with the requested LUID\n";
    if (factory) factory->Release();
    return 1;
  }

  ID3D12CommandQueue* queue = nullptr;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type =
                                                D3D12_COMMAND_LIST_TYPE_DIRECT};
  HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));

  ID3D12Resource* shared_tex = nullptr;
  ID3D12Fence* ready_fence = nullptr;
  ID3D12Fence* done_fence = nullptr;
  HANDLE handle = nullptr;
  if (SUCCEEDED(hr) &&
      SUCCEEDED(hr = device->OpenSharedHandleByName(tex_name, GENERIC_ALL,
                                                    &handle))) {
    hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&shared_tex));
    CloseHandle(handle);
    handle = nullptr;
  }
  if (SUCCEEDED(hr) &&
      SUCCEEDED(hr = device->OpenSharedHandleByName(ready_name, GENERIC_ALL,
                                                    &handle))) {
    hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&ready_fence));
    CloseHandle(handle);
    handle = nullptr;
  }
  if (SUCCEEDED(hr) &&
      SUCCEEDED(hr = device->OpenSharedHandleByName(done_name, GENERIC_ALL,
                                                    &handle))) {
    hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&done_fence));
    CloseHandle(handle);
    handle = nullptr;
  }
  if (FAILED(hr)) {
    std::wcerr << L"share-test: open shared objects failed hr=0x" << std::hex
               << hr << std::dec << L"\n";
    return 1;
  }
  std::wcout << L"share-test(x64): opened shared texture and fences\n";

  // An RTV is the cheapest way to write the whole surface every iteration,
  // which is what matters here -- the point is to touch the shared allocation
  // from the x64 device, not to produce a particular image.
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{
      .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1};
  hr = device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  if (SUCCEEDED(hr)) {
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(shared_tex, nullptr, rtv);
  }

  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  if (SUCCEEDED(hr))
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr))
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                   nullptr, IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr)) hr = list->Close();
  if (FAILED(hr)) {
    std::wcerr << L"share-test: command objects failed hr=0x" << std::hex << hr
               << std::dec << L"\n";
    return 1;
  }

  for (int i = 1; i <= iterations; ++i) {
    const uint64_t value = static_cast<uint64_t>(i);
    if (FAILED(hr = queue->Wait(ready_fence, value))) break;
    if (FAILED(hr = allocator->Reset())) break;
    if (FAILED(hr = list->Reset(allocator, nullptr))) break;
    const D3D12_RESOURCE_BARRIER to_rt = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = shared_tex,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                       .StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET}};
    list->ResourceBarrier(1, &to_rt);
    const float colour[4] = {static_cast<float>(i % 16) / 16.0f, 0.25f, 0.5f,
                             1.0f};
    list->ClearRenderTargetView(rtv, colour, 0, nullptr);
    const D3D12_RESOURCE_BARRIER to_common = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = shared_tex,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
                       .StateAfter = D3D12_RESOURCE_STATE_COMMON}};
    list->ResourceBarrier(1, &to_common);
    if (FAILED(hr = list->Close())) break;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(hr = queue->Signal(done_fence, value))) break;

    const HRESULT removed = device->GetDeviceRemovedReason();
    if (removed != S_OK) {
      std::wcerr << L"share-test(x64): DEVICE REMOVED at " << i << L" reason=0x"
                 << std::hex << removed << std::dec << L"\n";
      return 2;
    }
  }

  if (FAILED(hr)) {
    std::wcerr << L"share-test(x64): failed hr=0x" << std::hex << hr << std::dec
               << L"\n";
  } else {
    std::wcout << L"share-test(x64): completed " << iterations
               << L" iterations\n";
  }
  if (list) list->Release();
  if (allocator) allocator->Release();
  if (rtv_heap) rtv_heap->Release();
  if (done_fence) done_fence->Release();
  if (ready_fence) ready_fence->Release();
  if (shared_tex) shared_tex->Release();
  if (queue) queue->Release();
  device->Release();
  if (adapter) adapter->Release();
  factory->Release();
  return FAILED(hr) ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
  if (argc == 8 && std::wstring_view(argv[1]) == L"--share-test") {
    return RunShareTest(argv[2], argv[3], argv[4],
                        static_cast<uint32_t>(_wtoi64(argv[5])),
                        static_cast<int32_t>(_wtoi64(argv[6])), _wtoi(argv[7]));
  }
  if (argc == 2 && std::wstring_view(argv[1]) == L"--dlss-probe") {
    return RunDlssProbe();
  }
  if (argc == 3 && std::wstring_view(argv[1]) == L"--dlaa") {
    return RunDlaaHelper(argv[2]);
  }
  if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") {
    return RunSelfTest();
  }
  if (argc == 2 && std::wstring_view(argv[1]) == L"--scene-self-test") {
    return RunSceneSelfTest();
  }
  if (argc == 7 && std::wstring_view(argv[1]) == L"--handshake") {
    return RunHandshake(argv[2], argv[3], argv[4], argv[5], argv[6]);
  }
  PrintUsage();
  return 1;
}
