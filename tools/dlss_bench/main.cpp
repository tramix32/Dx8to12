// What does a DLSS evaluate cost at 2560x1440 on this GPU?
//
// That is the one number missing from the DLSS feasibility budget in
// plan/DLSS_X64_HELPER_HANDOFF.md. The cross-process transport was already
// measured (tools/share_test), so this deliberately leaves IPC out and runs
// DLSS against locally created textures.
//
// The inputs are not a real scene -- depth and motion vectors are static,
// so the image this produces is meaningless. That is fine and intentional:
// DLSS's cost is driven by resolution and mode, not by what the pixels
// contain, and building a real G-buffer is exactly the expensive renderer
// work this measurement is meant to happen *before*.
//
// Known limitation: there is no swap chain here, so Streamline's
// presentCommon() never runs and it complains that its per-frame bookkeeping
// and garbage collection are not being driven. That is inherent to a headless
// benchmark and does not affect the per-evaluate timings, but it does mean
// this program is not a model for how to integrate DLSS -- a real one
// presents every frame.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_helpers.h>

namespace {

constexpr UINT kOutputWidth = 2560;
constexpr UINT kOutputHeight = 1440;
constexpr int kWarmup = 30;
constexpr int kMeasured = 300;

template <typename T>
void Release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

bool Ok(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr)) return true;
  std::printf("FAIL %s hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
  return false;
}

bool Ok(sl::Result r, const char* what) {
  if (r == sl::Result::eOk) return true;
  std::printf("FAIL %s -> %s\n", what, sl::getResultAsStr(r));
  return false;
}

ID3D12Resource* CreateTex(ID3D12Device* device, UINT w, UINT h,
                          DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                          D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Width = w,
      .Height = h,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = format,
      .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = flags};
  ID3D12Resource* out = nullptr;
  if (!Ok(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                          state, nullptr, IID_PPV_ARGS(&out)),
          "CreateCommittedResource"))
    return nullptr;
  return out;
}

// Streamline logs nothing to stdout and file logging is off unless
// pathToLogsAndData is set, so route its diagnostics here -- without them a
// failure like eErrorFeatureMissing gives no reason at all.
void LogCallback(sl::LogType type, const char* msg) {
  if (type == sl::LogType::eError || type == sl::LogType::eWarn) {
    std::printf("[sl] %s", msg);
  }
}

const char* ModeName(sl::DLSSMode mode) {
  switch (mode) {
    case sl::DLSSMode::eDLAA: return "DLAA";
    case sl::DLSSMode::eMaxQuality: return "Quality";
    case sl::DLSSMode::eBalanced: return "Balanced";
    case sl::DLSSMode::eMaxPerformance: return "Performance";
    case sl::DLSSMode::eUltraPerformance: return "UltraPerformance";
    default: return "?";
  }
}

}  // namespace

int wmain() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // slInit must happen before the device is created so the interposer can
  // wrap it -- this is why the tool links sl.interposer.lib instead of
  // loading it on demand the way rt_helper's probe does.
  const sl::Feature features[] = {sl::kFeatureDLSS};
  sl::Preferences prefs{};
  prefs.featuresToLoad = features;
  prefs.numFeaturesToLoad = _countof(features);
  prefs.renderAPI = sl::RenderAPI::eD3D12;
  prefs.engine = sl::EngineType::eCustom;
  prefs.engineVersion = "0.1";
  prefs.projectId = "7c9f1e2a-4b3d-4f8a-9c15-2e6d8b0a41f3";
  prefs.logLevel = sl::LogLevel::eVerbose;
  prefs.logMessageCallback = LogCallback;
  // slSetTagForFrame is refused outright unless this is opted into; the
  // per-frame tagging path is what a real integration wants, since it ties
  // each resource set to the frame token DLSS is evaluated with.
  prefs.flags = prefs.flags | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  if (!Ok(slInit(prefs), "slInit")) return 1;

  IDXGIFactory6* factory = nullptr;
  if (!Ok(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2"))
    return 1;
  // Ask which adapter supports DLSS *before* creating any device. This
  // machine enumerates the 4080 twice and only one entry reports support, and
  // asking after device creation was observed to answer
  // eErrorFeatureNotSupported regardless -- so the order matters.
  IDXGIAdapter1* adapter = nullptr;
  DXGI_ADAPTER_DESC1 chosen{};
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      sl::AdapterInfo info{};
      info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
      info.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
      const sl::Result supported = slIsFeatureSupported(sl::kFeatureDLSS, info);
      std::wprintf(L"adapter[%u] %s DLSS: %hs\n", i, desc.Description,
                   sl::getResultAsStr(supported));
      if (supported == sl::Result::eOk) {
        chosen = desc;
        break;
      }
    }
    Release(adapter);
  }
  if (!adapter) {
    std::printf("FAIL no adapter reports DLSS support\n");
    return 1;
  }

  ID3D12Device* device = nullptr;
  if (!Ok(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                            IID_PPV_ARGS(&device)),
          "D3D12CreateDevice"))
    return 1;
  if (!Ok(slSetD3DDevice(device), "slSetD3DDevice")) return 1;

  ID3D12CommandQueue* queue = nullptr;
  const D3D12_COMMAND_QUEUE_DESC qdesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  if (!Ok(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)),
          "CreateCommandQueue"))
    return 1;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  if (!Ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator") ||
      !Ok(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    allocator, nullptr, IID_PPV_ARGS(&list)),
          "CreateCommandList"))
    return 1;
  list->Close();

  ID3D12Fence* fence = nullptr;
  if (!Ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
          "CreateFence"))
    return 1;
  HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  uint64_t fence_value = 0;

  const sl::DLSSMode modes[] = {sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality,
                                sl::DLSSMode::eBalanced,
                                sl::DLSSMode::eMaxPerformance};
  std::printf("output %ux%u, %d warmup + %d measured frames per mode\n\n",
              kOutputWidth, kOutputHeight, kWarmup, kMeasured);

  for (const sl::DLSSMode mode : modes) {
    sl::DLSSOptions options{};
    options.mode = mode;
    options.outputWidth = kOutputWidth;
    options.outputHeight = kOutputHeight;
    options.colorBuffersHDR = sl::Boolean::eFalse;

    sl::DLSSOptimalSettings settings{};
    if (!Ok(slDLSSGetOptimalSettings(options, settings),
            "slDLSSGetOptimalSettings"))
      continue;
    const UINT render_w = settings.optimalRenderWidth;
    const UINT render_h = settings.optimalRenderHeight;

    // DLSS manages the state of tagged resources itself, so these are created
    // in the states it expects to find them in and then left alone.
    ID3D12Resource* color_in =
        CreateTex(device, render_w, render_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* mvec =
        CreateTex(device, render_w, render_h, DXGI_FORMAT_R16G16_FLOAT,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* depth =
        CreateTex(device, render_w, render_h, DXGI_FORMAT_R32_FLOAT,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* color_out =
        CreateTex(device, kOutputWidth, kOutputHeight,
                  DXGI_FORMAT_R16G16B16A16_FLOAT,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!color_in || !mvec || !depth || !color_out) return 1;

    const sl::ViewportHandle viewport(0);
    if (!Ok(slDLSSSetOptions(viewport, options), "slDLSSSetOptions")) continue;

    double total_ms = 0.0;
    double worst_ms = 0.0;
    bool failed = false;

    for (int frame = 0; frame < kWarmup + kMeasured && !failed; ++frame) {
      sl::FrameToken* token = nullptr;
      const uint32_t frame_index = static_cast<uint32_t>(frame);
      if (!Ok(slGetNewFrameToken(token, &frame_index), "slGetNewFrameToken")) {
        failed = true;
        break;
      }

      if (!Ok(allocator->Reset(), "allocator->Reset") ||
          !Ok(list->Reset(allocator, nullptr), "list->Reset")) {
        failed = true;
        break;
      }

      // The state has to be passed explicitly with frame-based tagging --
      // Streamline takes over transitioning these, but it has to be told what
      // it is starting from. All four were created in UNORDERED_ACCESS and
      // nothing else touches them here.
      const uint32_t uav_state =
          static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      sl::Resource r_in{sl::ResourceType::eTex2d, color_in, uav_state};
      sl::Resource r_out{sl::ResourceType::eTex2d, color_out, uav_state};
      sl::Resource r_depth{sl::ResourceType::eTex2d, depth, uav_state};
      sl::Resource r_mvec{sl::ResourceType::eTex2d, mvec, uav_state};
      sl::Extent render_extent{0, 0, render_w, render_h};
      sl::Extent output_extent{0, 0, kOutputWidth, kOutputHeight};
      const sl::ResourceTag tags[] = {
          {&r_in, sl::kBufferTypeScalingInputColor,
           sl::ResourceLifecycle::eOnlyValidNow, &render_extent},
          {&r_out, sl::kBufferTypeScalingOutputColor,
           sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
          {&r_depth, sl::kBufferTypeDepth,
           sl::ResourceLifecycle::eOnlyValidNow, &render_extent},
          {&r_mvec, sl::kBufferTypeMotionVectors,
           sl::ResourceLifecycle::eOnlyValidNow, &render_extent}};
      if (!Ok(slSetTagForFrame(*token, viewport, tags, _countof(tags), list),
              "slSetTagForFrame")) {
        failed = true;
        break;
      }

      // A static camera with no jitter. Wrong for image quality, irrelevant
      // for cost -- see the file header.
      sl::Constants consts{};
      sl::float4x4 identity{};
      for (uint32_t row = 0; row < 4; ++row) {
        identity.row[row] = {row == 0 ? 1.0f : 0.0f, row == 1 ? 1.0f : 0.0f,
                             row == 2 ? 1.0f : 0.0f, row == 3 ? 1.0f : 0.0f};
      }
      consts.cameraViewToClip = identity;
      consts.clipToCameraView = identity;
      consts.clipToPrevClip = identity;
      consts.prevClipToClip = identity;
      consts.jitterOffset = {0.0f, 0.0f};
      consts.mvecScale = {1.0f, 1.0f};
      consts.cameraPinholeOffset = {0.0f, 0.0f};
      consts.cameraPos = {0.0f, 0.0f, 0.0f};
      consts.cameraUp = {0.0f, 1.0f, 0.0f};
      consts.cameraRight = {1.0f, 0.0f, 0.0f};
      consts.cameraFwd = {0.0f, 0.0f, 1.0f};
      consts.cameraNear = 0.1f;
      consts.cameraFar = 1000.0f;
      consts.cameraFOV = 1.0f;
      consts.cameraAspectRatio =
          static_cast<float>(kOutputWidth) / static_cast<float>(kOutputHeight);
      consts.depthInverted = sl::Boolean::eFalse;
      consts.cameraMotionIncluded = sl::Boolean::eTrue;
      consts.motionVectors3D = sl::Boolean::eFalse;
      consts.reset = frame == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
      if (!Ok(slSetConstants(consts, *token, viewport), "slSetConstants")) {
        failed = true;
        break;
      }

      const sl::BaseStructure* inputs[] = {&viewport};
      const sl::Result eval =
          slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, _countof(inputs),
                            list);
      if (!Ok(eval, "slEvaluateFeature")) {
        failed = true;
        break;
      }

      if (!Ok(list->Close(), "list->Close")) {
        failed = true;
        break;
      }

      // Timed around a full submit-and-wait: this is wall-clock cost of the
      // evaluate on an otherwise idle GPU, which is the number that has to fit
      // in a frame budget.
      const auto t0 = std::chrono::steady_clock::now();
      ID3D12CommandList* lists[] = {list};
      queue->ExecuteCommandLists(1, lists);
      ++fence_value;
      if (!Ok(queue->Signal(fence, fence_value), "Signal")) {
        failed = true;
        break;
      }
      if (fence->GetCompletedValue() < fence_value) {
        fence->SetEventOnCompletion(fence_value, fence_event);
        WaitForSingleObject(fence_event, 5000);
      }
      const double ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - t0)
              .count();

      if (frame >= kWarmup) {
        total_ms += ms;
        worst_ms = std::max(worst_ms, ms);
      }
      if (device->GetDeviceRemovedReason() != S_OK) {
        std::printf("DEVICE REMOVED during %s\n", ModeName(mode));
        failed = true;
      }
    }

    if (!failed) {
      const double avg = total_ms / kMeasured;
      std::printf("%-16s render %4ux%-4u -> %ux%u   avg %.2f ms   worst %.2f ms"
                  "   (%.0f fps if DLSS were the only cost)\n",
                  ModeName(mode), render_w, render_h, kOutputWidth,
                  kOutputHeight, avg, worst_ms, 1000.0 / avg);
    }

    Release(color_out);
    Release(depth);
    Release(mvec);
    Release(color_in);
  }

  CloseHandle(fence_event);
  Release(fence);
  Release(list);
  Release(allocator);
  Release(queue);
  Release(device);
  Release(adapter);
  Release(factory);
  slShutdown();
  return 0;
}
