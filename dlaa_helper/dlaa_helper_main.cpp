// x64 DLAA/DLSS helper for the x86 D3D8 shim.
//
// Why this is a separate process: Streamline/NGX ships x64-only and the game
// is a 32-bit process. That is not a limitation of this codebase -- it is the
// entire reason this executable exists.
//
// Why it is a separate *executable* from dx8to12_rt_helper: Streamline needs
// slInit to run before the D3D12 device is created so its interposer can wrap
// it, which means linking sl.interposer.lib, which routes every D3D12 entry
// point in the process through Streamline. The RT helper must not be routed
// through it. Two binaries rather than two modes of one.
//
// The loopback mode is kept alongside the real path on purpose. It copies
// input to output and nothing else, so when something looks wrong on screen
// it answers "is this the transport or the upscaler?" without a rebuild --
// which is exactly how the transport was brought up in the first place.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <iostream>
#include <string_view>

#include "dlss_ipc_protocol.h"

#ifdef DX8TO12_HAVE_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_helpers.h>
#endif

namespace {

using Microsoft::WRL::ComPtr;
namespace Ipc = Dx8to12::DlssIpc;

void PrintUsage() {
  std::wcerr << L"Usage: dx8to12_dlaa_helper --dlaa <shared-memory-name>\n";
}

// Sharing resources across two different physical adapters produces garbage
// rather than an error, so match the game's adapter by LUID instead of taking
// whichever comes first.
HRESULT FindAdapterByLuid(LUID luid, IDXGIAdapter1** adapter_out) {
  ComPtr<IDXGIFactory4> factory;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return hr;
  ComPtr<IDXGIAdapter1> adapter;
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

#ifdef DX8TO12_HAVE_STREAMLINE
void LogCallback(sl::LogType type, const char* message) {
  // Streamline reports most of its real failures only through this callback.
  // Four separate bring-up problems in tools/dlss_bench were invisible until
  // it was installed, so it stays installed.
  if (type == sl::LogType::eError || type == sl::LogType::eWarn) {
    std::fprintf(stderr, "[sl] %s", message ? message : "(null)");
  }
}

bool Ok(sl::Result result, const char* what) {
  if (result == sl::Result::eOk) return true;
  std::fprintf(stderr, "%s failed: %d\n", what, static_cast<int>(result));
  return false;
}

sl::float4x4 ToSlMatrix(const float m[16]) {
  sl::float4x4 out{};
  for (uint32_t row = 0; row < 4; ++row) {
    out.row[row] = {m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2],
                    m[row * 4 + 3]};
  }
  return out;
}
#endif

// Streamline's sl.common plugin does its per-frame bookkeeping and garbage
// collection inside the Present it hooks. A helper process has no reason of
// its own to present anything, and without a Present slEvaluateFeature
// reports "presentCommon() was not observed" and misbehaves.
//
// So the helper gets a swap chain purely to have something to present: a 1x1
// hidden window, flip-discard, presented once per processed frame. Nothing
// ever looks at it. This is the cost of running Streamline somewhere it does
// not expect to be -- which is itself forced, since it is x64-only and the
// game is not.
struct PresentPump {
  HWND window = nullptr;
  ComPtr<IDXGISwapChain1> swap_chain;

  bool Create(ID3D12CommandQueue* queue) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Dx8to12DlaaPresentPump";
    RegisterClassExW(&wc);
    window = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    const DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = 1,
        .Height = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = {.Count = 1},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = 2,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD};
    return SUCCEEDED(factory->CreateSwapChainForHwnd(
        queue, window, &desc, nullptr, nullptr, &swap_chain));
  }

  void Pump() {
    if (swap_chain) swap_chain->Present(0, 0);
  }

  void Destroy() {
    swap_chain.Reset();
    if (window) {
      DestroyWindow(window);
      window = nullptr;
    }
  }
};

// DLSS 5 Neural Rendering.
//
// Separate from everything above because it is not a Streamline feature:
// Streamline 2.12's kFeature list stops at DLSS, DLSS_G, DLSS_RR and
// DirectSR, so NR is reached through NGX directly. That is why the helper can
// end up linking both SDKs.
//
// It is reachable at all because NR is a post-pass over colour, depth and
// motion vectors rather than a renderer wanting a full G-buffer -- and those
// three are already crossing to this process every frame for super
// resolution. The motion vectors here are reconstructed from the real depth
// buffer rather than estimated from the final image, which is strictly more
// than a post-process injector can offer it.
//
// Deliberately left unimplemented rather than guessed at: the NR feature id
// and its NGX parameter names are not something to invent, and this session
// has already paid for assumptions about another library's conventions. With
// the SDK present these are declarations to read, not guesses to make.
struct NeuralRendering {
  bool active = false;

  bool Initialise(ID3D12Device* device, uint32_t output_width,
                  uint32_t output_height) {
#ifdef DX8TO12_HAVE_NGX
    (void)device;
    (void)output_width;
    (void)output_height;
    std::fprintf(stderr,
                 "DLAA helper: the NGX SDK is present but the neural "
                 "rendering calls are not written yet.\n");
    return false;
#else
    (void)device;
    (void)output_width;
    (void)output_height;
    std::fprintf(stderr,
                 "DLAA helper: neural rendering was requested, but this build "
                 "has no NGX SDK (third_party/ngx). Running super resolution "
                 "only.\n");
    return false;
#endif
  }

  // Runs after slEvaluateFeature, over its output.
  void Evaluate(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* color,
                ID3D12Resource* depth, ID3D12Resource* mvec) {
    (void)cmd_list;
    (void)color;
    (void)depth;
    (void)mvec;
  }

  void Shutdown() { active = false; }
};

struct SlotResources {
  ComPtr<ID3D12Resource> color_in;
  ComPtr<ID3D12Resource> color_out;
  ComPtr<ID3D12Resource> depth_in;
  ComPtr<ID3D12Resource> mvec_in;
};

int RunDlaaHelper(const wchar_t* map_name) {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name);
  if (!mapping) {
    std::wcerr << L"--dlaa: could not open shared memory.\n";
    return 1;
  }
  auto* shared = static_cast<Ipc::Handshake*>(
      MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ipc::Handshake)));
  if (!shared) {
    CloseHandle(mapping);
    return 1;
  }

  auto fail = [&](Ipc::HelperStatus status, HRESULT hr) {
    shared->hresult = hr;
    shared->status = static_cast<uint32_t>(status);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 1;
  };

  if (shared->magic != Ipc::kMagic || shared->version != Ipc::kVersion) {
    return fail(Ipc::HelperStatus::kProtocolMismatch, E_FAIL);
  }
  shared->helper_process_id = GetCurrentProcessId();
  const auto mode = static_cast<Ipc::Mode>(shared->mode);

#ifdef DX8TO12_HAVE_STREAMLINE
  bool streamline_ready = false;
  if (mode != Ipc::Mode::kLoopback) {
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences prefs{};
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = _countof(features);
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "0.1";
    // Must be a real GUID. A malformed one gets as far as NGX and then fails
    // with 0xBAD00005, which says nothing about the actual cause.
    prefs.projectId = "7c9f1e2a-4b3d-4f8a-9c15-2e6d8b0a41f3";
    prefs.logLevel = sl::LogLevel::eDefault;
    prefs.logMessageCallback = LogCallback;
    // slSetTagForFrame is refused outright without this.
    prefs.flags = prefs.flags | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    // Before device creation, so the interposer can wrap it.
    streamline_ready = Ok(slInit(prefs), "slInit");
    if (!streamline_ready) {
      return fail(Ipc::HelperStatus::kStreamlineInitFailed, E_FAIL);
    }
  }
#else
  if (mode != Ipc::Mode::kLoopback) {
    std::fprintf(stderr,
                 "Built without the Streamline SDK; loopback only.\n");
  }
#endif

  LUID luid = {};
  luid.LowPart = shared->adapter_luid_low;
  luid.HighPart = shared->adapter_luid_high;
  ComPtr<IDXGIAdapter1> adapter;
  HRESULT hr = FindAdapterByLuid(luid, &adapter);
  if (FAILED(hr)) return fail(Ipc::HelperStatus::kAdapterNotFound, hr);

  ComPtr<ID3D12Device> device;
  hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                         IID_PPV_ARGS(&device));
  if (FAILED(hr)) return fail(Ipc::HelperStatus::kDeviceCreateFailed, hr);

#ifdef DX8TO12_HAVE_STREAMLINE
  if (streamline_ready && !Ok(slSetD3DDevice(device.Get()), "slSetD3DDevice")) {
    return fail(Ipc::HelperStatus::kStreamlineInitFailed, E_FAIL);
  }
#endif

  // Everything is created by x86 and opened here. That direction is the one
  // that has proven stable in this project; the reverse caused repeated
  // device removals.
  auto open_shared = [&](const wchar_t* name, REFIID iid, void** out) {
    HANDLE handle = nullptr;
    HRESULT open_hr = device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
    if (FAILED(open_hr)) return open_hr;
    open_hr = device->OpenSharedHandle(handle, iid, out);
    CloseHandle(handle);
    return open_hr;
  };

  SlotResources slots[Ipc::kFrameSlots];
  ComPtr<ID3D12Fence> ready_fence, done_fence;
  for (uint32_t slot = 0; slot < Ipc::kFrameSlots && SUCCEEDED(hr); ++slot) {
    hr = open_shared(shared->color_in_name[slot], __uuidof(ID3D12Resource),
                     reinterpret_cast<void**>(slots[slot].color_in.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = open_shared(shared->color_out_name[slot], __uuidof(ID3D12Resource),
                       reinterpret_cast<void**>(slots[slot].color_out.GetAddressOf()));
    }
    if (SUCCEEDED(hr) && shared->depth_in_name[slot][0]) {
      hr = open_shared(shared->depth_in_name[slot], __uuidof(ID3D12Resource),
                       reinterpret_cast<void**>(slots[slot].depth_in.GetAddressOf()));
    }
    if (SUCCEEDED(hr) && shared->mvec_in_name[slot][0]) {
      hr = open_shared(shared->mvec_in_name[slot], __uuidof(ID3D12Resource),
                       reinterpret_cast<void**>(slots[slot].mvec_in.GetAddressOf()));
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
  if (FAILED(hr)) return fail(Ipc::HelperStatus::kSharedOpenFailed, hr);

  const D3D12_COMMAND_QUEUE_DESC queue_desc = {
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> cmd_list;
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
  if (FAILED(hr)) return fail(Ipc::HelperStatus::kDeviceCreateFailed, hr);
  cmd_list->Close();

  PresentPump present_pump;
#ifdef DX8TO12_HAVE_STREAMLINE
  if (streamline_ready && !present_pump.Create(queue.Get())) {
    std::fprintf(stderr,
                 "DLAA helper: could not create the present pump; Streamline's "
                 "per-frame bookkeeping will not run.\n");
  }

  sl::ViewportHandle viewport{0};
  if (streamline_ready) {
    sl::DLSSOptions options{};
    // Derived from the resolution the game actually chose rather than
    // hardcoded: DLSS sizes its internal buffers from the mode, so a mode
    // that disagrees with the render extent being tagged is asking it to
    // reconstruct from something other than what it is given.
    const float scale =
        shared->output_width
            ? static_cast<float>(shared->render_width) /
                  static_cast<float>(shared->output_width)
            : 1.f;
    if (mode == Ipc::Mode::kDlaa || scale > 0.95f) {
      options.mode = sl::DLSSMode::eDLAA;
    } else if (scale > 0.62f) {
      options.mode = sl::DLSSMode::eMaxQuality;
    } else if (scale > 0.54f) {
      options.mode = sl::DLSSMode::eBalanced;
    } else {
      options.mode = sl::DLSSMode::eMaxPerformance;
    }
    options.outputWidth = shared->output_width;
    options.outputHeight = shared->output_height;
    options.colorBuffersHDR = sl::Boolean::eFalse;
    // Zero means "let the SDK choose", which is both the right default and
    // what lets a newer DLSS model be adopted by dropping in newer DLLs. A
    // non-zero value is passed straight through, so a preset that does not
    // exist yet is still selectable from the INI without changing this code.
    if (shared->dlss_preset != 0) {
      const auto preset = static_cast<sl::DLSSPreset>(shared->dlss_preset);
      options.dlaaPreset = preset;
      options.qualityPreset = preset;
      options.balancedPreset = preset;
      options.performancePreset = preset;
      options.ultraPerformancePreset = preset;
      std::fprintf(stderr, "DLAA helper: requesting DLSS preset %u\n",
                   shared->dlss_preset);
    }
    std::fprintf(stderr,
                 "DLAA helper: render %ux%u -> output %ux%u (scale %.3f), "
                 "DLSSMode %d\n",
                 shared->render_width, shared->render_height,
                 shared->output_width, shared->output_height, scale,
                 static_cast<int>(options.mode));
    if (!Ok(slDLSSSetOptions(viewport, options), "slDLSSSetOptions")) {
      return fail(Ipc::HelperStatus::kFeatureUnavailable, E_FAIL);
    }
  }
#endif

  // Report what actually came through the shared handles, so the game can
  // check the sharing resolved to the resources it created rather than
  // assuming it did -- a handle that opens onto the wrong resource corrupts
  // silently instead of failing.
  const D3D12_RESOURCE_DESC color_desc = slots[0].color_in->GetDesc();
  shared->seen_color_in_width = static_cast<uint32_t>(color_desc.Width);
  shared->seen_color_in_height = color_desc.Height;
  shared->seen_color_in_format = static_cast<uint32_t>(color_desc.Format);
  if (slots[0].depth_in) {
    const D3D12_RESOURCE_DESC d = slots[0].depth_in->GetDesc();
    shared->seen_depth_in_width = static_cast<uint32_t>(d.Width);
    shared->seen_depth_in_format = static_cast<uint32_t>(d.Format);
  }
  if (slots[0].mvec_in) {
    const D3D12_RESOURCE_DESC d = slots[0].mvec_in->GetDesc();
    shared->seen_mvec_in_width = static_cast<uint32_t>(d.Width);
    shared->seen_mvec_in_format = static_cast<uint32_t>(d.Format);
  }
  NeuralRendering neural;
  if (shared->neural_rendering) {
    neural.active = neural.Initialise(device.Get(), shared->output_width,
                                      shared->output_height);
  }
  // What is actually running, not what was asked for -- the game's status API
  // reports this, and the two differ whenever the runtime is missing.
  shared->neural_rendering_active = neural.active ? 1u : 0u;

  shared->status = static_cast<uint32_t>(Ipc::HelperStatus::kReady);

  HANDLE ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // A handle to the game, waited on alongside the frame fence. shutdown_
  // requested only arrives if the game exits cleanly; when it crashes -- or is
  // killed -- nothing writes it, and this process would sit in its wait loop
  // forever. Orphans were observed accumulating exactly that way, one per
  // crashed session, each holding its own view of shared memory.
  HANDLE shim_process =
      shared->shim_process_id
          ? OpenProcess(SYNCHRONIZE, FALSE, shared->shim_process_id)
          : nullptr;
  if (!shim_process) {
    std::fprintf(stderr,
                 "DLAA helper: could not open the game process; will exit only "
                 "on a clean shutdown request.\n");
  }

  uint64_t processed = 0;
  while (shared->shutdown_requested == 0) {
    if (shim_process && WaitForSingleObject(shim_process, 0) == WAIT_OBJECT_0) {
      std::fprintf(stderr, "DLAA helper: the game exited; shutting down.\n");
      break;
    }
    // Block on the fence; never poll. A Sleep(1) here sleeps a full ~15.6 ms
    // timer tick at Windows' default granularity, which measured as the game
    // dropping from 249 to 64 fps with the GPU completely idle.
    uint64_t wanted = ready_fence->GetCompletedValue();
    if (wanted <= processed) {
      if (FAILED(ready_fence->SetEventOnCompletion(processed + 1, ready_event)))
        break;
      // Waits on the game's process handle as well as the frame, so a crash
      // wakes this immediately instead of after the timeout. Still bounded, so
      // a clean shutdown request is noticed while the game sits paused.
      HANDLE waits[2] = {ready_event, shim_process};
      const DWORD count = shim_process ? 2u : 1u;
      const DWORD woke = WaitForMultipleObjects(count, waits, FALSE, 100);
      if (woke == WAIT_OBJECT_0 + 1) {
        std::fprintf(stderr, "DLAA helper: the game exited; shutting down.\n");
        break;
      }
      if (woke != WAIT_OBJECT_0) continue;
      wanted = ready_fence->GetCompletedValue();
      if (wanted <= processed) continue;
    }
    // The newest signalled frame, not processed + 1: working through a
    // backlog one frame at a time would keep the helper behind forever.
    const uint32_t slot = static_cast<uint32_t>(wanted % Ipc::kFrameSlots);
    SlotResources& res = slots[slot];

    allocator->Reset();
    cmd_list->Reset(allocator.Get(), nullptr);

    bool recorded = false;
#ifdef DX8TO12_HAVE_STREAMLINE
    if (streamline_ready && res.depth_in && res.mvec_in) {
      sl::FrameToken* token = nullptr;
      const uint32_t frame_index = static_cast<uint32_t>(wanted);
      if (Ok(slGetNewFrameToken(token, &frame_index), "slGetNewFrameToken")) {
        // Frame-based tagging requires the state to be passed explicitly:
        // Streamline takes over transitioning these, but has to be told what
        // it is starting from. The game hands them over in COMMON.
        const uint32_t common =
            static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON);
        sl::Resource r_in{sl::ResourceType::eTex2d, res.color_in.Get(), common};
        sl::Resource r_out{sl::ResourceType::eTex2d, res.color_out.Get(), common};
        sl::Resource r_depth{sl::ResourceType::eTex2d, res.depth_in.Get(), common};
        sl::Resource r_mvec{sl::ResourceType::eTex2d, res.mvec_in.Get(), common};
        sl::Extent render_extent{0, 0, shared->render_width,
                                 shared->render_height};
        sl::Extent output_extent{0, 0, shared->output_width,
                                 shared->output_height};
        const sl::ResourceTag tags[] = {
            {&r_in, sl::kBufferTypeScalingInputColor,
             sl::ResourceLifecycle::eOnlyValidNow, &render_extent},
            {&r_out, sl::kBufferTypeScalingOutputColor,
             sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
            {&r_depth, sl::kBufferTypeDepth,
             sl::ResourceLifecycle::eOnlyValidNow, &render_extent},
            {&r_mvec, sl::kBufferTypeMotionVectors,
             sl::ResourceLifecycle::eOnlyValidNow, &render_extent}};

        sl::Constants consts{};
        consts.cameraViewToClip = ToSlMatrix(shared->camera_view_to_clip);
        consts.clipToCameraView = ToSlMatrix(shared->clip_to_camera_view);
        consts.clipToPrevClip = ToSlMatrix(shared->clip_to_prev_clip);
        consts.prevClipToClip = ToSlMatrix(shared->prev_clip_to_clip);
        consts.jitterOffset = {shared->jitter_x, shared->jitter_y};
        consts.mvecScale = {shared->mvec_scale[0], shared->mvec_scale[1]};
        consts.cameraPinholeOffset = {0.f, 0.f};
        consts.cameraPos = {shared->camera_pos[0], shared->camera_pos[1],
                            shared->camera_pos[2]};
        consts.cameraUp = {shared->camera_up[0], shared->camera_up[1],
                           shared->camera_up[2]};
        consts.cameraRight = {shared->camera_right[0], shared->camera_right[1],
                              shared->camera_right[2]};
        consts.cameraFwd = {shared->camera_fwd[0], shared->camera_fwd[1],
                            shared->camera_fwd[2]};
        consts.cameraNear = shared->camera_near;
        consts.cameraFar = shared->camera_far;
        consts.cameraFOV = shared->camera_fov;
        consts.cameraAspectRatio = shared->camera_aspect;
        consts.depthInverted = sl::Boolean::eFalse;
        // The motion vectors describe camera movement only -- see
        // motion_vectors.hlsl. Saying otherwise makes DLSS expect per-object
        // motion it will not find.
        consts.cameraMotionIncluded = sl::Boolean::eTrue;
        consts.motionVectors3D = sl::Boolean::eFalse;
        consts.reset = shared->reset_history ? sl::Boolean::eTrue
                                             : sl::Boolean::eFalse;

        const sl::BaseStructure* inputs[] = {&viewport};
        if (Ok(slSetTagForFrame(*token, viewport, tags, _countof(tags),
                                cmd_list.Get()),
               "slSetTagForFrame") &&
            Ok(slSetConstants(consts, *token, viewport), "slSetConstants") &&
            Ok(slEvaluateFeature(sl::kFeatureDLSS, *token, inputs,
                                 _countof(inputs), cmd_list.Get()),
               "slEvaluateFeature")) {
          recorded = true;
          // After super resolution, over its output: NR is a post-pass, and
          // the same depth and motion vectors it was given still describe the
          // frame.
          if (neural.active) {
            neural.Evaluate(cmd_list.Get(), res.color_out.Get(),
                            res.depth_in.Get(), res.mvec_in.Get());
          }
        } else {
          shared->last_hresult = E_FAIL;
          ++shared->failed_frames;
        }
      }
    }
#endif

    if (!recorded && shared->render_width != shared->output_width) {
      // Nothing sensible to fall back to: CopyResource needs identical
      // dimensions, and issuing it anyway is what turned a failed evaluate
      // into a black screen with no error anywhere. Report it instead, so the
      // game stops waiting on a helper that cannot deliver.
      std::fprintf(stderr,
                   "DLAA helper: frame %llu could not be evaluated and the "
                   "sizes differ, so there is no copy fallback.\n",
                   static_cast<unsigned long long>(wanted));
      shared->last_hresult = E_FAIL;
      ++shared->failed_frames;
      queue->Signal(done_fence.Get(), wanted);
      processed = wanted;
      shared->completed_frame_index = wanted;
      cmd_list->Close();
      continue;
    }
    if (!recorded) {
      // Loopback, and the fallback if an evaluate could not be recorded at
      // 1:1: the game still gets a correct (if un-upscaled) frame rather than
      // a stale or empty one.
      D3D12_RESOURCE_BARRIER to_copy[2] = {};
      to_copy[0].Transition = {
          .pResource = res.color_in.Get(),
          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          .StateBefore = D3D12_RESOURCE_STATE_COMMON,
          .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE};
      to_copy[1].Transition = {
          .pResource = res.color_out.Get(),
          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          .StateBefore = D3D12_RESOURCE_STATE_COMMON,
          .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST};
      cmd_list->ResourceBarrier(2, to_copy);
      cmd_list->CopyResource(res.color_out.Get(), res.color_in.Get());
      D3D12_RESOURCE_BARRIER to_common[2] = {to_copy[0], to_copy[1]};
      for (D3D12_RESOURCE_BARRIER& barrier : to_common) {
        const auto before = barrier.Transition.StateBefore;
        barrier.Transition.StateBefore = barrier.Transition.StateAfter;
        barrier.Transition.StateAfter = before;
      }
      cmd_list->ResourceBarrier(2, to_common);
    }

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
    // Streamline hooks Present to run its per-frame bookkeeping; without it
    // slEvaluateFeature reports "presentCommon() was not observed". Nothing
    // looks at what this presents.
    present_pump.Pump();
    processed = wanted;
    shared->completed_frame_index = wanted;
  }

  // Never tear down while the GPU still owns resources another process is
  // about to free.
  ComPtr<ID3D12Fence> drain;
  if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&drain)))) {
    queue->Signal(drain.Get(), 1);
    if (drain->GetCompletedValue() < 1) {
      drain->SetEventOnCompletion(1, ready_event);
      WaitForSingleObject(ready_event, 2000);
    }
  }
  neural.Shutdown();
  if (shim_process) CloseHandle(shim_process);
  present_pump.Destroy();
#ifdef DX8TO12_HAVE_STREAMLINE
  if (streamline_ready) slShutdown();
#endif
  CloseHandle(ready_event);
  UnmapViewOfFile(shared);
  CloseHandle(mapping);
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--dlaa") {
    return RunDlaaHelper(argv[2]);
  }
  PrintUsage();
  return 1;
}
