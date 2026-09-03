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
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef DX8TO12_HAVE_NGX
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#endif

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

// Runtimes NGX ships that are definitely not neural rendering. Observed on
// this machine (Streamline 2.12.0 / NGX 310.7.0): nvngx_dlss = super
// resolution, nvngx_dlssd = ray reconstruction, nvngx_dlssg = frame
// generation, nvngx_deepdvc = the video colour filter, nvngx = the loader.
// Listed so an unrecognised nvngx_*.dll can be reported as interesting.
constexpr const wchar_t* kKnownNonNrRuntimes[] = {
    L"nvngx.dll", L"nvngx_dlss.dll", L"nvngx_dlssd.dll", L"nvngx_dlssg.dll",
    L"nvngx_deepdvc.dll"};

// The neural rendering runtime. nvngx_dlssnr.dll is NGX feature 18; the
// build circulating at the time of writing reports itself as DLSSNR
// 310.8.0.0 and is around 158 MB, so a stub or a rename is easy to spot by
// size alone. The alternative spellings are kept because this file arrives
// from outside NVIDIA's own distribution -- and the search below also accepts
// an unrecognised nvngx_*.dll placed deliberately next to the helper or the
// game, so a differently-named build still works without a code change.
constexpr const wchar_t* kNeuralRenderingCandidates[] = {
    L"nvngx_dlssnr.dll", L"nvngx_dlssn.dll", L"nvngx_nr.dll"};

bool EqualsIgnoreCaseW(const wchar_t* a, const wchar_t* b) {
  return _wcsicmp(a, b) == 0;
}

std::wstring DirectoryOfThisExecutable() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return L"";
  std::wstring s(path);
  const size_t slash = s.find_last_of(L'\\');
  return slash == std::wstring::npos ? L"" : s.substr(0, slash);
}

// Where the driver keeps its own NGX runtimes. Read from the registry rather
// than assembled from a guessed Program Files path -- on this machine the
// value points into the driver store, under a directory whose name carries a
// driver hash, which no hardcoded path would ever match.
std::wstring DriverNgxDirectory() {
  wchar_t buffer[MAX_PATH] = {};
  DWORD size = sizeof(buffer);
  if (RegGetValueW(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                   L"FullPath", RRF_RT_REG_SZ, nullptr, buffer,
                   &size) != ERROR_SUCCESS) {
    return L"";
  }
  return buffer;
}

// Looks for a neural rendering runtime and reports what it found either way.
//
// This is a *hint*, not the authoritative check. NGX selects a feature by its
// enum id and derives the runtime filename itself; the correct availability
// query is NVSDK_NGX_..._GetCapabilityParameters plus that feature's
// _Available parameter, which needs the SDK headers this build may not have.
// Until those are present the presence of the file is the best evidence
// obtainable, and saying so is better than either silently assuming yes or
// refusing to look.
//
// `trusted` distinguishes a directory the user deliberately put files in
// (next to the helper, or next to the game) from the driver store: in the
// former an unrecognised nvngx_*.dll is taken as the runtime, because putting
// it there is exactly how someone would install one. In the latter only the
// candidate names are accepted -- picking up an unknown driver-shipped DLL
// and loading it into the game is not a decision to make on the user's
// behalf.
#ifdef DX8TO12_HAVE_NGX
// NGX's own diagnostics, into this helper's log. Worth wiring before it is
// needed: NGX reports one result code for a whole init or feature create and
// names no parameter, so without this its failures are a hex number and a
// guess.
static void NVSDK_CONV NgxLogCallback(const char* message,
                                      NVSDK_NGX_Logging_Level level,
                                      NVSDK_NGX_Feature source) {
  std::fprintf(stderr, "[ngx][lvl %d][feature %d] %s", static_cast<int>(level),
               static_cast<int>(source), message ? message : "(null)\n");
}
#endif

bool ProbeNeuralRenderingRuntime(std::wstring* found_path,
                                 std::string* found_name) {
  struct SearchDir {
    std::wstring path;
    bool trusted;
  };
  std::vector<SearchDir> dirs;
  const std::wstring exe_dir = DirectoryOfThisExecutable();
  if (!exe_dir.empty()) dirs.push_back({exe_dir, true});
  wchar_t cwd[MAX_PATH] = {};
  if (GetCurrentDirectoryW(MAX_PATH, cwd) != 0 &&
      (exe_dir.empty() || !EqualsIgnoreCaseW(cwd, exe_dir.c_str()))) {
    dirs.push_back({cwd, true});
  }
  const std::wstring driver_dir = DriverNgxDirectory();
  if (!driver_dir.empty()) dirs.push_back({driver_dir, false});

  for (const SearchDir& dir : dirs) {
    const std::wstring pattern = dir.path + L"\\nvngx_*.dll";
    WIN32_FIND_DATAW find = {};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) continue;
    do {
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      bool known_other = false;
      for (const wchar_t* known : kKnownNonNrRuntimes) {
        if (EqualsIgnoreCaseW(find.cFileName, known)) known_other = true;
      }
      if (known_other) continue;

      bool is_candidate = false;
      for (const wchar_t* candidate : kNeuralRenderingCandidates) {
        if (EqualsIgnoreCaseW(find.cFileName, candidate)) is_candidate = true;
      }
      if (!is_candidate && !dir.trusted) {
        // Interesting, but not ours to load. Name it so it stops being a
        // mystery if neural rendering is expected and does not appear.
        std::fprintf(stderr,
                     "DLAA helper: driver ships an unrecognised NGX runtime "
                     "%ls; not loaded (only a deliberately installed one "
                     "is).\n",
                     find.cFileName);
        continue;
      }
      const std::wstring path = dir.path + L"\\" + find.cFileName;
      char narrow[64] = {};
      WideCharToMultiByte(CP_UTF8, 0, find.cFileName, -1, narrow,
                          static_cast<int>(sizeof(narrow) - 1), nullptr,
                          nullptr);
      std::fprintf(stderr,
                   "DLAA helper: neural rendering runtime candidate %ls (%s) "
                   "in %ls\n",
                   find.cFileName,
                   is_candidate ? "known name" : "installed here",
                   dir.path.c_str());
      FindClose(handle);
      if (found_path) *found_path = path;
      if (found_name) *found_name = narrow;
      return true;
    } while (FindNextFileW(handle, &find));
    FindClose(handle);
  }
  std::fprintf(stderr,
               "DLAA helper: no neural rendering runtime found. Drop one next "
               "to the game or this helper to enable it.\n");
  return false;
}

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
// The parameter contract for feature 18, read off a shipping implementation
// (RenoDX's DLSS 5 ReShade add-on) rather than invented. Names are NGX
// parameter strings, set on the block from NVSDK_NGX_D3D12_AllocateParameters
// before CreateFeature/EvaluateFeature:
//
//   create:   DLSSNR.Width, DLSSNR.Height, DLSSNR.InputWidth,
//             DLSSNR.InputHeight, DLSSNR.OutputWidth, DLSSNR.OutputHeight,
//             DLSSNR.Upscaling, DLSSNR.ScalingRatio,
//             DLSSNR.Hint.Render.Preset
//   evaluate: DLSSNR.Color, DLSSNR.Depth, DLSSNR.MVec, DLSSNR.Output,
//             DLSSNR.MVecScaleX / Y, DLSSNR.DepthInverted, DLSSNR.Reset,
//             DLSSNR.Enabled, plus a *SubrectBaseX/Y and *SubrectWidth/Height
//             quad for each of Color, Depth, MVec and Output
//   look:     DLSSNR.Style, DLSSNR.Intensity, DLSSNR.GlobalToneStrength,
//             DLSSNR.LocalToneStrength, DLSSNR.LocalStructureStrength,
//             DLSSNR.SkinStructureStrength, DLSSNR.UseAutoMask,
//             DLSSNR.UICorrection
//
// Two behaviours worth knowing before wiring it: the feature expects an
// *opaque* colour input, with alpha restored afterwards by the caller; and
// the runtime may refuse to upscale and fall back to native, so the 1:1 path
// has to work on its own rather than being a special case of the scaled one.
//
// What is known about the remaining work, so it is not re-derived:
//
//   * The feature is NGX feature id 18, driven through the NGX D3D12 entry
//     points -- not through Streamline, whose 2.12 feature list stops at
//     DLSS, DLSS_G, DLSS_RR and DirectSR.
//   * It evaluates *on D3D12*. Every other integration of it has to mirror
//     colour, depth and motion vectors into shared D3D12 textures and copy
//     the result back; this helper is already a D3D12 process being handed
//     exactly those three as shared textures, which is why the transport work
//     is done rather than pending.
//   * Depth must be a 32-bit format. Ours is R32_FLOAT, so that holds.
//   * The runtime refuses calls from a caller it does not recognise: it wants
//     a real NVSDK_NGX_D3D12_Init_ProjectID, not a placeholder. This is the
//     part that cannot be written without the SDK headers, and the reason
//     the calls below are absent rather than approximated.
//   * RTX 40 is a supported target, not only RTX 50.
struct NeuralRendering {
  bool active = false;
  bool available = false;
  std::string runtime_name;

  // Looks for the runtime whether or not it was asked for, so a settings
  // panel can offer the option only when it would do something. The
  // difference between "no runtime installed" and "installed but switched
  // off" is the user's to see and only one of the two is theirs to fix,
  // which is why this is separate from Initialise.
  void Probe() {
    std::wstring path;
    available = ProbeNeuralRenderingRuntime(&path, &runtime_name);
    runtime_path_ = path;
  }

  bool Initialise(ID3D12Device* device, uint32_t output_width,
                  uint32_t output_height) {
    if (!available) return false;
#ifdef DX8TO12_HAVE_NGX
    device_ = device;
    width_ = output_width;
    height_ = output_height;

    // A real project identity, because the runtime declines callers it cannot
    // identify. ENGINE_TYPE_CUSTOM is what the SDK documents for anything
    // that is not Unreal/Unity/Omniverse, which this is.
    // A real directory NGX may write its own logs and model cache into. The
    // helper's own directory is the one place guaranteed to exist and to be
    // the same across runs.
    wchar_t data_path[MAX_PATH] = L".";
    if (!runtime_path_.empty()) {
      const size_t slash = runtime_path_.find_last_of(L'\\');
      if (slash != std::wstring::npos) {
        wcsncpy_s(data_path, runtime_path_.substr(0, slash).c_str(),
                  _TRUNCATE);
      }
    }

    // Where to look for the feature DLL, and NGX's own log routed into this
    // helper's log.
    //
    // Passing nullptr here is what produced FAIL_UnableToInitializeFeature
    // ("the library for that feature could not be found") on the first
    // attempt. The SDK does search the application folder by default, and the
    // runtime is sitting in it, so the default was evidently not enough --
    // hence both the explicit path list and the logging callback, because
    // NGX's own reason is worth more than another guess from here.
    const wchar_t* search_paths[] = {data_path, L"."};
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = search_paths;
    common.PathListInfo.Length =
        static_cast<unsigned int>(_countof(search_paths));
    common.LoggingInfo.LoggingCallback = &NgxLogCallback;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = false;

    const NVSDK_NGX_Result init = NVSDK_NGX_D3D12_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, data_path,
        device, &common, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(init)) {
      std::fprintf(stderr,
                   "DLAA helper: NGX init failed 0x%08X; neural rendering "
                   "off.\n",
                   static_cast<unsigned>(init));
      return false;
    }
    ngx_initialised_ = true;

    const NVSDK_NGX_Result alloc =
        NVSDK_NGX_D3D12_AllocateParameters(&params_);
    if (NVSDK_NGX_FAILED(alloc) || params_ == nullptr) {
      std::fprintf(stderr, "DLAA helper: NGX AllocateParameters failed 0x%08X\n",
                   static_cast<unsigned>(alloc));
      // Do not leave a half-initialised NGX behind: super resolution goes
      // through the same NGX core in this process, and it matters more than
      // this feature does.
      NVSDK_NGX_D3D12_Shutdown1(device_);
      ngx_initialised_ = false;
      params_ = nullptr;
      return false;
    }

    std::fprintf(stderr,
                 "DLAA helper: NGX up for neural rendering via %s, %ux%u. The "
                 "feature itself is created on the first frame, where there "
                 "is a command list to create it on.\n",
                 runtime_name.c_str(), width_, height_);
    return true;
#else
    (void)device;
    (void)output_width;
    (void)output_height;
    std::fprintf(stderr,
                 "DLAA helper: found %s, but this build has no NGX SDK "
                 "(third_party/ngx) to drive it with. Running super "
                 "resolution only.\n",
                 runtime_name.c_str());
    return false;
#endif
  }

  // Runs after slEvaluateFeature, over its output. `color` is both the input
  // and, after the copy at the end, where the result lands -- the feature
  // wants a distinct output resource, so one is kept here and copied back
  // rather than exposed to the rest of the pipeline.
  void Evaluate(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* color,
                ID3D12Resource* depth, ID3D12Resource* mvec,
                const Ipc::Handshake* shared) {
#ifdef DX8TO12_HAVE_NGX
    if (!params_ || create_failed_ || !cmd_list || !color) return;
    if (!EnsureOutput(color)) return;
    if (!EnsureFeature(cmd_list)) return;

    // Everything arrives and leaves in COMMON: that is what this helper tags
    // to Streamline, so it is the state the rest of the frame assumes.
    const D3D12_RESOURCE_BARRIER to_nr[] = {
        Barrier(color, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(output_.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    cmd_list->ResourceBarrier(_countof(to_nr), to_nr);

    NVSDK_NGX_Parameter_SetD3d12Resource(params_, "DLSSNR.Color", color);
    NVSDK_NGX_Parameter_SetD3d12Resource(params_, "DLSSNR.Output",
                                         output_.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(params_, "DLSSNR.Depth", depth);
    NVSDK_NGX_Parameter_SetD3d12Resource(params_, "DLSSNR.MVec", mvec);

    SetSubrect("DLSSNR.Color", width_, height_);
    SetSubrect("DLSSNR.Depth", width_, height_);
    SetSubrect("DLSSNR.MVec", width_, height_);
    SetSubrect("DLSSNR.Output", width_, height_);

    // Motion vectors are already in pixels here (the shim's own pass writes
    // them that way), so the scale is 1 rather than the render dimensions.
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.MVecScaleX", 1.0f);
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.MVecScaleY", 1.0f);
    NVSDK_NGX_Parameter_SetI(params_, "DLSSNR.DepthInverted", 0);
    NVSDK_NGX_Parameter_SetI(params_, "DLSSNR.Enabled", 1);
    NVSDK_NGX_Parameter_SetI(params_, "DLSSNR.Reset",
                             shared && shared->reset_history ? 1 : 0);
    ApplyLook(shared);

    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    __try {
      result = NVSDK_NGX_D3D12_EvaluateFeature(cmd_list, handle_, params_,
                                               nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A signed runtime that dislikes its caller can take the process down
      // rather than return a code. Losing neural rendering is recoverable;
      // losing the helper mid-frame is what leaves the game without an
      // upscaler at all.
      std::fprintf(stderr,
                   "DLAA helper: feature 18 evaluate raised an exception; "
                   "neural rendering disabled for this session.\n");
      create_failed_ = true;
      active = false;
      result = NVSDK_NGX_Result_Fail;
    }

    const D3D12_RESOURCE_BARRIER from_nr[] = {
        Barrier(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE)};
    cmd_list->ResourceBarrier(_countof(from_nr), from_nr);

    if (NVSDK_NGX_SUCCEED(result)) {
      cmd_list->CopyResource(color, output_.Get());
      if (++evaluations_ == 1) {
        std::fprintf(stderr, "DLAA helper: feature 18 evaluation succeeded.\n");
      }
    } else if (!create_failed_) {
      std::fprintf(stderr, "DLAA helper: feature 18 evaluate failed 0x%08X\n",
                   static_cast<unsigned>(result));
      create_failed_ = true;
      active = false;
    }

    const D3D12_RESOURCE_BARRIER back[] = {
        Barrier(color, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON),
        Barrier(output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON)};
    cmd_list->ResourceBarrier(_countof(back), back);
#else
    (void)cmd_list;
    (void)color;
    (void)depth;
    (void)mvec;
    (void)shared;
#endif
  }

  void Shutdown() {
    active = false;
#ifdef DX8TO12_HAVE_NGX
    if (handle_) {
      NVSDK_NGX_D3D12_ReleaseFeature(handle_);
      handle_ = nullptr;
    }
    if (params_) {
      NVSDK_NGX_D3D12_DestroyParameters(params_);
      params_ = nullptr;
    }
    if (ngx_initialised_) {
      NVSDK_NGX_D3D12_Shutdown1(device_);
      ngx_initialised_ = false;
    }
    output_.Reset();
#endif
  }

 private:
  std::wstring runtime_path_;
#ifdef DX8TO12_HAVE_NGX
  // Identity this project presents to NGX. Its own, deliberately: the runtime
  // wants to know who is calling, and answering with somebody else's is not
  // something this project does.
  //
  // This must be a real UUID. A first attempt spelled the last group
  // "dx8to12neural", which reads nicely and is not hexadecimal, and NGX
  // rejected the whole init with FAIL_InvalidParameter (0xBAD00005) -- an
  // error that says nothing about which parameter, so it is worth being
  // precise here rather than clever.
  static constexpr const char* kProjectId =
      "a1d4f0e2-6c3b-4f5a-9d21-7e6b5c4a3f20";
  static constexpr const char* kEngineVersion = "1.0";

  static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* resource,
                                        D3D12_RESOURCE_STATES before,
                                        D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
  }

  void SetSubrect(const char* prefix, uint32_t w, uint32_t h) {
    char key[64];
    std::snprintf(key, sizeof(key), "%s%s", prefix, "SubrectBaseX");
    NVSDK_NGX_Parameter_SetUI(params_, key, 0);
    std::snprintf(key, sizeof(key), "%s%s", prefix, "SubrectBaseY");
    NVSDK_NGX_Parameter_SetUI(params_, key, 0);
    std::snprintf(key, sizeof(key), "%s%s", prefix, "SubrectWidth");
    NVSDK_NGX_Parameter_SetUI(params_, key, w);
    std::snprintf(key, sizeof(key), "%s%s", prefix, "SubrectHeight");
    NVSDK_NGX_Parameter_SetUI(params_, key, h);
  }

  void ApplyLook(const Ipc::Handshake* shared) {
    if (!shared) return;
    NVSDK_NGX_Parameter_SetI(params_, "DLSSNR.Style",
                             static_cast<int>(shared->nr_style));
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.Intensity", shared->nr_intensity);
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.GlobalToneStrength",
                             shared->nr_global_tone);
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.LocalToneStrength",
                             shared->nr_local_tone);
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.LocalStructureStrength",
                             shared->nr_local_structure);
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.SkinStructureStrength",
                             shared->nr_skin_structure);
    NVSDK_NGX_Parameter_SetI(params_, "DLSSNR.UseAutoMask",
                             shared->nr_auto_mask ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(params_, "DLSSNR.UICorrection",
                             shared->nr_ui_correction ? 1 : 0);
  }

  bool EnsureOutput(ID3D12Resource* color) {
    if (output_) return true;
    D3D12_RESOURCE_DESC desc = color->GetDesc();
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&output_));
    if (FAILED(hr)) {
      std::fprintf(stderr,
                   "DLAA helper: neural rendering output alloc failed 0x%08X\n",
                   static_cast<unsigned>(hr));
      create_failed_ = true;
      return false;
    }
    width_ = static_cast<uint32_t>(desc.Width);
    height_ = desc.Height;
    return true;
  }

  bool EnsureFeature(ID3D12GraphicsCommandList* cmd_list) {
    if (handle_) return true;

    NVSDK_NGX_Parameter_SetUI(params_, "DLSSNR.Width", width_);
    NVSDK_NGX_Parameter_SetUI(params_, "DLSSNR.Height", height_);
    NVSDK_NGX_Parameter_SetUI(params_, "DLSSNR.InputWidth", width_);
    NVSDK_NGX_Parameter_SetUI(params_, "DLSSNR.InputHeight", height_);
    NVSDK_NGX_Parameter_SetUI(params_, "DLSSNR.OutputWidth", width_);
    NVSDK_NGX_Parameter_SetUI(params_, "DLSSNR.OutputHeight", height_);
    // Super resolution has already run by this point, so NR runs 1:1 over its
    // output. Its own upscaling is a separate thing and the runtime is free
    // to refuse it, which is why nothing here depends on it.
    NVSDK_NGX_Parameter_SetI(params_, "DLSSNR.Upscaling", 0);
    NVSDK_NGX_Parameter_SetF(params_, "DLSSNR.ScalingRatio", 1.0f);
    NVSDK_NGX_Parameter_SetUI(params_, "DLSSNR.Hint.Render.Preset",
                              nr_preset_);
    NVSDK_NGX_Parameter_SetUI(params_, "CreationNodeMask", 1);
    NVSDK_NGX_Parameter_SetUI(params_, "VisibilityNodeMask", 1);

    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    __try {
      result = NVSDK_NGX_D3D12_CreateFeature(
          cmd_list, static_cast<NVSDK_NGX_Feature>(kFeatureNeuralRendering),
          params_, &handle_);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      std::fprintf(stderr,
                   "DLAA helper: feature 18 create raised an exception; "
                   "nothing was submitted.\n");
      handle_ = nullptr;
      create_failed_ = true;
      active = false;
      return false;
    }
    if (NVSDK_NGX_FAILED(result) || handle_ == nullptr) {
      std::fprintf(stderr, "DLAA helper: feature 18 create failed 0x%08X\n",
                   static_cast<unsigned>(result));
      handle_ = nullptr;
      create_failed_ = true;
      active = false;
      return false;
    }
    std::fprintf(stderr, "DLAA helper: feature 18 created, %ux%u.\n", width_,
                 height_);
    return true;
  }

  // NVSDK_NGX_Feature_Reserved18 in the public SDK; DLSSNR/reserved-18 in the
  // runtime. Named here so the number is not a bare literal at the call site.
  static constexpr int kFeatureNeuralRendering = 18;

  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* handle_ = nullptr;
  ComPtr<ID3D12Resource> output_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t nr_preset_ = 0;
  uint64_t evaluations_ = 0;
  bool ngx_initialised_ = false;
  bool create_failed_ = false;
#endif
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
  // Probe regardless of the request: the shim reports availability to mods,
  // and a panel needs to know before anyone asks for it.
  neural.Probe();
  shared->neural_rendering_available = neural.available ? 1u : 0u;
  // Whether to bring neural rendering up at all. Deferred to the first frame
  // super resolution has actually completed, rather than done here: this
  // process shares one NGX core between Streamline and NGX-direct calls, and
  // touching it before Streamline has finished its own bring-up puts this
  // feature in a position to break the one that already works. Nothing about
  // neural rendering is worth that.
  bool neural_requested = shared->neural_rendering != 0;
  bool neural_tried = false;
  strncpy_s(shared->neural_rendering_runtime,
            sizeof(shared->neural_rendering_runtime),
            neural.runtime_name.c_str(), _TRUNCATE);

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
        // eValidUntilPresent, not eOnlyValidNow. The difference is not a
        // hint: eOnlyValidNow tells Streamline the resource may change or be
        // destroyed the moment it has been handed over, so Streamline
        // *clones* it. On an input that only wastes a copy per frame. On the
        // **output** it is silently fatal -- super resolution writes into
        // Streamline's clone, nothing copies that back, and the texture the
        // game presents is never touched. slEvaluateFeature still reports
        // success, so the failure reaches the player as a black screen with
        // the HUD drawn over it and not one error anywhere.
        //
        // These four are dedicated shared textures owned by this helper and
        // its client, written once per frame and read by nothing else in
        // between, which is exactly what eValidUntilPresent describes.
        // Streamline's own guidance is to tag everything that way first.
        const sl::ResourceTag tags[] = {
            {&r_in, sl::kBufferTypeScalingInputColor,
             sl::ResourceLifecycle::eValidUntilPresent, &render_extent},
            {&r_out, sl::kBufferTypeScalingOutputColor,
             sl::ResourceLifecycle::eValidUntilPresent, &output_extent},
            {&r_depth, sl::kBufferTypeDepth,
             sl::ResourceLifecycle::eValidUntilPresent, &render_extent},
            {&r_mvec, sl::kBufferTypeMotionVectors,
             sl::ResourceLifecycle::eValidUntilPresent, &render_extent}};

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
        // Loopback skips super resolution entirely; the !recorded path below
        // then copies input to output. Reusing that rather than adding a
        // second copy: it already handles the size check and the barriers,
        // and it is the path a failed evaluate has always taken.
        if (!shared->force_loopback &&
            Ok(slSetTagForFrame(*token, viewport, tags, _countof(tags),
                                cmd_list.Get()),
               "slSetTagForFrame") &&
            Ok(slSetConstants(consts, *token, viewport), "slSetConstants") &&
            Ok(slEvaluateFeature(sl::kFeatureDLSS, *token, inputs,
                                 _countof(inputs), cmd_list.Get()),
               "slEvaluateFeature")) {
          recorded = true;
          if (shared->produced_frames == 0) {
            std::fprintf(stderr,
                         "DLAA helper: first super resolution evaluate "
                         "succeeded.\n");
          }
          // After super resolution, over its output: NR is a post-pass, and
          // the same depth and motion vectors it was given still describe the
          // frame.
          if (neural_requested && !neural.active && !neural_tried) {
            // First frame past a working super resolution: NGX is up and
            // settled, so it is now safe to ask it for feature 18 as well.
            neural_tried = true;
            neural.active = neural.Initialise(device.Get(),
                                              shared->output_width,
                                              shared->output_height);
            shared->neural_rendering_active = neural.active ? 1u : 0u;
          }
          if (neural.active) {
            neural.Evaluate(cmd_list.Get(), res.color_out.Get(),
                            res.depth_in.Get(), res.mvec_in.Get(), shared);
            shared->neural_rendering_active = neural.active ? 1u : 0u;
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
    ++shared->produced_frames;
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
