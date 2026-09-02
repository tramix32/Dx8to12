#include "dlss_client.h"

#include <d3d12.h>

#include <string>

#include "aixlog.hpp"
#include "device.h"
#include "texture.h"
#include "utils/asserts.h"

namespace Dx8to12 {

namespace {

// How long a frame is willing to wait for the helper before giving up on it.
// Generous enough for a ~1.9 ms evaluate plus scheduling on a busy GPU, short
// enough that a hung helper costs a visible stutter rather than a freeze.
constexpr DWORD kFrameWaitTimeoutMs = 40;
// After this many consecutive timeouts the helper is presumed dead. Stopping
// matters more than it sounds: every timeout costs a full kFrameWaitTimeoutMs
// of frame time, so a helper that is merely gone would otherwise cap the game
// at 25 fps forever.
constexpr uint32_t kMaxConsecutiveTimeouts = 8;
// How long the helper gets to open its shared resources and say so. Creating
// a D3D12 device and opening four shared handles is fast; this is generous
// enough to survive a first-run driver shader-cache stall.
constexpr ULONGLONG kStartupTimeoutMs = 10000;

std::wstring UniqueName(const wchar_t *suffix) {
  return std::wstring(L"Local\\Dx8to12_dlss_") + std::to_wstring(GetCurrentProcessId()) +
         L"_" + suffix;
}

// Directory d3d8.dll itself was loaded from -- the game's install folder,
// which is also where the helper and its log live. Not the working
// directory, which a game is free to change.
std::wstring HelperDirectory() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&UniqueName), &self)) {
    return L"";
  }
  wchar_t path[MAX_PATH];
  const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
  if (len == 0 || len == MAX_PATH) return L"";
  std::wstring result(path, len);
  const size_t slash = result.find_last_of(L"\\/");
  return (slash == std::wstring::npos ? L"" : result.substr(0, slash + 1));
}

// Not dx8to12_rt_helper.exe: DLAA needs a binary that links Streamline's
// interposer, which the RT helper deliberately does not.
std::wstring HelperPath() {
  const std::wstring dir = HelperDirectory();
  return dir.empty() ? L"" : dir + L"dx8to12_dlaa_helper.exe";
}

}  // namespace

DlssClient::DlssClient(Device *device) : device_(device) {}

DlssClient::~DlssClient() { Stop(); }

bool DlssClient::Start(uint32_t render_width, uint32_t render_height,
                       uint32_t output_width, uint32_t output_height,
                       DlssIpc::Mode mode) {
  Stop();
  render_width_ = render_width;
  render_height_ = render_height;
  output_width_ = output_width;
  output_height_ = output_height;
  if (render_width == 0 || render_height == 0 || output_width == 0 ||
      output_height == 0) {
    return false;
  }

  const std::wstring map_name = UniqueName(L"map");
  const std::wstring ready_name = UniqueName(L"ready");
  const std::wstring done_name = UniqueName(L"done");

  mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                sizeof(DlssIpc::Handshake), map_name.c_str());
  if (!mapping_) {
    LOG(AixLog::Severity::error)
        << "DLSS: CreateFileMapping failed, error " << GetLastError() << "\n";
    Stop();
    return false;
  }
  shared_ = static_cast<DlssIpc::Handshake *>(
      MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DlssIpc::Handshake)));
  if (!shared_) {
    LOG(AixLog::Severity::error) << "DLSS: MapViewOfFile failed.\n";
    Stop();
    return false;
  }
  *shared_ = DlssIpc::Handshake{};

  // Both textures are the backbuffer's format and size. Same reasoning as the
  // scene target: matching formats keeps the copy a plain CopyResource and
  // keeps the PSO cache from forking.
  const DXGI_FORMAT format = device_->backbuffer_format();
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
      // Overwritten per texture by create_shared below -- inputs are the
      // scene's size, the output is the presented size.
      .Width = render_width,
      .Height = render_height,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = format,
      .SampleDesc = {.Count = 1, .Quality = 0},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      // UAV because Streamline writes the output through one; RENDER_TARGET
      // because GpuTexture::InitFromResource unconditionally builds an RTV
      // (it is the wrapper used for back buffers), and an RTV on a resource
      // without this flag is invalid. Both flags on both textures, so a mode
      // change never means recreating them.
      .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};

  auto create_shared = [&](ComPtr<GpuTexture> *out_texture, HANDLE *out_handle,
                           const std::wstring &name, bool at_output_resolution,
                           DXGI_FORMAT override_format = DXGI_FORMAT_UNKNOWN) {
    D3D12_RESOURCE_DESC this_desc = desc;
    // Only the upscaled result is output-sized; everything the upscaler reads
    // describes the smaller scene.
    this_desc.Width = at_output_resolution ? output_width : render_width;
    this_desc.Height = at_output_resolution ? output_height : render_height;
    if (override_format != DXGI_FORMAT_UNKNOWN) {
      this_desc.Format = override_format;
    }
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device_->device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_SHARED, &this_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(resource.GetForInit())))) {
      return false;
    }
    if (FAILED(device_->device()->CreateSharedHandle(
            resource.get(), nullptr, GENERIC_ALL, name.c_str(), out_handle))) {
      return false;
    }
    *out_texture = ComOwn(GpuTexture::InitFromResource(device_, resource));
    return true;
  };
  for (uint32_t slot = 0; slot < DlssIpc::kFrameSlots; ++slot) {
    const std::wstring suffix = L"_" + std::to_wstring(slot);
    const std::wstring color_in_name = UniqueName((L"colorin" + suffix).c_str());
    const std::wstring color_out_name = UniqueName((L"colorout" + suffix).c_str());
    const std::wstring depth_in_name = UniqueName((L"depthin" + suffix).c_str());
    const std::wstring mvec_in_name = UniqueName((L"mvecin" + suffix).c_str());
    if (!create_shared(&color_in_[slot], &color_in_handle_[slot],
                       color_in_name, /*at_output_resolution=*/false) ||
        !create_shared(&color_out_[slot], &color_out_handle_[slot],
                       color_out_name, /*at_output_resolution=*/true) ||
        !create_shared(&depth_in_[slot], &depth_in_handle_[slot], depth_in_name,
                       /*at_output_resolution=*/false,
                       DXGI_FORMAT_R32_FLOAT) ||
        !create_shared(&mvec_in_[slot], &mvec_in_handle_[slot], mvec_in_name,
                       /*at_output_resolution=*/false,
                       DXGI_FORMAT_R16G16_FLOAT)) {
      LOG(AixLog::Severity::error) << "DLSS: shared texture creation failed.\n";
      Stop();
      return false;
    }
    const std::string tag = "_" + std::to_string(slot);
    color_in_[slot]->SetName("dlss_color_in" + tag);
    color_out_[slot]->SetName("dlss_color_out" + tag);
    depth_in_[slot]->SetName("dlss_depth_in" + tag);
    mvec_in_[slot]->SetName("dlss_mvec_in" + tag);
    wcsncpy_s(shared_->color_in_name[slot], color_in_name.c_str(), _TRUNCATE);
    wcsncpy_s(shared_->color_out_name[slot], color_out_name.c_str(), _TRUNCATE);
    wcsncpy_s(shared_->depth_in_name[slot], depth_in_name.c_str(), _TRUNCATE);
    wcsncpy_s(shared_->mvec_in_name[slot], mvec_in_name.c_str(), _TRUNCATE);
  }

  if (FAILED(device_->device()->CreateFence(
          0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(ready_fence_.GetForInit()))) ||
      FAILED(device_->device()->CreateFence(
          0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(done_fence_.GetForInit()))) ||
      FAILED(device_->device()->CreateSharedHandle(ready_fence_.get(), nullptr,
                                                   GENERIC_ALL, ready_name.c_str(),
                                                   &ready_fence_handle_)) ||
      FAILED(device_->device()->CreateSharedHandle(done_fence_.get(), nullptr,
                                                   GENERIC_ALL, done_name.c_str(),
                                                   &done_fence_handle_))) {
    LOG(AixLog::Severity::error) << "DLSS: shared fence creation failed.\n";
    Stop();
    return false;
  }
  done_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!done_event_) {
    Stop();
    return false;
  }

  LUID luid = device_->device()->GetAdapterLuid();
  shared_->shim_process_id = GetCurrentProcessId();
  shared_->adapter_luid_low = luid.LowPart;
  shared_->adapter_luid_high = luid.HighPart;
  shared_->render_width = render_width;
  shared_->render_height = render_height;
  shared_->output_width = output_width;
  shared_->output_height = output_height;
  shared_->mode = static_cast<uint32_t>(mode);
  shared_->dlss_preset = static_cast<uint32_t>(GetConfig().dlss_preset);
  wcsncpy_s(shared_->ready_fence_name, ready_name.c_str(), _TRUNCATE);
  wcsncpy_s(shared_->done_fence_name, done_name.c_str(), _TRUNCATE);

  const std::wstring helper = HelperPath();
  if (helper.empty()) {
    LOG(AixLog::Severity::error) << "DLSS: could not locate the helper exe.\n";
    Stop();
    return false;
  }
  std::wstring command_line = L"\"" + helper + L"\" --dlaa \"" + map_name + L"\"";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);

  // Capture the helper's output. It runs with no console, and Streamline
  // reports most of its real failures only through its log callback -- which
  // means that without this, a failed slEvaluateFeature is completely silent
  // on this side and shows up only as a black screen.
  const std::wstring helper_log = HelperDirectory() + L"dx8to12_dlaa_helper.log";
  SECURITY_ATTRIBUTES inheritable{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                  .lpSecurityDescriptor = nullptr,
                                  .bInheritHandle = TRUE};
  HANDLE log_handle = CreateFileW(
      helper_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  const bool have_log = log_handle != INVALID_HANDLE_VALUE;
  if (have_log) {
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdOutput = log_handle;
    startup.hStdError = log_handle;
    startup.hStdInput = nullptr;
  }
  if (!CreateProcessW(helper.c_str(), command_line.data(), nullptr, nullptr,
                      /*bInheritHandles=*/have_log ? TRUE : FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &helper_process_)) {
    if (have_log) CloseHandle(log_handle);
    LOG(AixLog::Severity::error)
        << "DLSS: could not launch " << std::string(helper.begin(), helper.end())
        << ", error " << GetLastError() << "\n";
    Stop();
    return false;
  }
  // The child holds its own duplicate; this one is done with.
  if (have_log) CloseHandle(log_handle);

  healthy_ = true;
  pending_history_reset_ = true;
  frame_index_ = 0;
  consecutive_timeouts_ = 0;
  start_tick_ = GetTickCount64();
  LOG(AixLog::Severity::info)
      << "DLSS: helper launched, rendering " << render_width << "x"
      << render_height << " -> " << output_width << "x" << output_height
      << ", mode " << static_cast<uint32_t>(mode) << ".\n";
  return true;
}

void DlssClient::CloseSharedObjects() {
  auto close = [](HANDLE *handle) {
    if (*handle) {
      CloseHandle(*handle);
      *handle = nullptr;
    }
  };
  for (uint32_t slot = 0; slot < DlssIpc::kFrameSlots; ++slot) {
    close(&color_in_handle_[slot]);
    close(&color_out_handle_[slot]);
    close(&depth_in_handle_[slot]);
    close(&mvec_in_handle_[slot]);
  }
  close(&ready_fence_handle_);
  close(&done_fence_handle_);
  close(&done_event_);
}

void DlssClient::Stop() {
  if (shared_) {
    // Ask before killing: a helper mid-Evaluate holds GPU work referencing
    // resources this process is about to free.
    shared_->shutdown_requested = 1;
  }
  if (helper_process_.hProcess) {
    if (WaitForSingleObject(helper_process_.hProcess, 2000) != WAIT_OBJECT_0) {
      LOG(AixLog::Severity::error)
          << "DLSS: helper did not exit on request; terminating.\n";
      TerminateProcess(helper_process_.hProcess, 1);
      WaitForSingleObject(helper_process_.hProcess, 1000);
    }
    CloseHandle(helper_process_.hProcess);
    if (helper_process_.hThread) CloseHandle(helper_process_.hThread);
    helper_process_ = {};
  }
  CloseSharedObjects();
  for (uint32_t slot = 0; slot < DlssIpc::kFrameSlots; ++slot) {
    color_in_[slot].Reset();
    color_out_[slot].Reset();
    depth_in_[slot].Reset();
    mvec_in_[slot].Reset();
  }
  ready_fence_.Reset();
  done_fence_.Reset();
  if (shared_) {
    UnmapViewOfFile(shared_);
    shared_ = nullptr;
  }
  if (mapping_) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
  ready_ = false;
  healthy_ = false;
}

bool DlssClient::PollReady() {
  if (!shared_ || !healthy_) return false;
  if (ready_) return true;

  const auto status = static_cast<DlssIpc::HelperStatus>(shared_->status);
  if (status == DlssIpc::HelperStatus::kReady) {
    ready_ = true;
    // Checked, not assumed: a shared handle that opens but resolves to a
    // resource of the wrong size or format corrupts silently rather than
    // failing, so compare what the helper sees against what was created.
    // Slot 0 stands for all of them: they are created in one loop from one
    // description, so a per-slot check would only ever catch a bug in that
    // loop, not in the sharing this is here to verify.
    const bool matches =
        shared_->seen_color_in_width == render_width_ &&
        shared_->seen_color_in_height == render_height_ &&
        shared_->seen_depth_in_width == render_width_ &&
        shared_->seen_mvec_in_width == render_width_ &&
        shared_->seen_depth_in_format ==
            static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT) &&
        shared_->seen_mvec_in_format ==
            static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT);
    LOG(AixLog::Severity::info)
        << "DLSS: helper reported ready. Sees color " << shared_->seen_color_in_width
        << "x" << shared_->seen_color_in_height << " fmt "
        << shared_->seen_color_in_format << ", depth "
        << shared_->seen_depth_in_width << " fmt " << shared_->seen_depth_in_format
        << ", mvec " << shared_->seen_mvec_in_width << " fmt "
        << shared_->seen_mvec_in_format << " -- "
        << (matches ? "matches what we created" : "MISMATCH") << ".\n";
    if (!matches) {
      LOG(AixLog::Severity::error)
          << "DLSS: shared resources did not resolve to what was created; "
             "disabling rather than feeding the upscaler the wrong data.\n";
      healthy_ = false;
      return false;
    }
    return true;
  }
  if (status != DlssIpc::HelperStatus::kStarting) {
    LOG(AixLog::Severity::error)
        << "DLSS: helper failed to start, status " << shared_->status
        << " hr=0x" << std::hex << shared_->hresult << std::dec
        << " -- carrying on without it.\n";
    healthy_ = false;
    return false;
  }

  // Still "starting". Two ways that can be a lie, and both used to be
  // invisible: the process died before it could write a status at all (a
  // missing DLL exits at load time, which is how tools/share_test once hung),
  // or it is alive and simply never getting there.
  if (helper_process_.hProcess &&
      WaitForSingleObject(helper_process_.hProcess, 0) == WAIT_OBJECT_0) {
    DWORD exit_code = 0;
    GetExitCodeProcess(helper_process_.hProcess, &exit_code);
    LOG(AixLog::Severity::error)
        << "DLSS: helper exited during startup with code 0x" << std::hex
        << exit_code << std::dec << " -- carrying on without it.\n";
    healthy_ = false;
    return false;
  }
  if (GetTickCount64() - start_tick_ > kStartupTimeoutMs) {
    LOG(AixLog::Severity::error)
        << "DLSS: helper still had not reported ready after "
        << kStartupTimeoutMs << "ms -- carrying on without it.\n";
    healthy_ = false;
    return false;
  }
  return false;
}

void DlssClient::SetCameraConstants(const DlssCameraConstants &constants) {
  if (!shared_) return;
  memcpy(shared_->camera_view_to_clip, constants.view_to_clip,
         sizeof(shared_->camera_view_to_clip));
  memcpy(shared_->clip_to_camera_view, constants.clip_to_view,
         sizeof(shared_->clip_to_camera_view));
  memcpy(shared_->clip_to_prev_clip, constants.clip_to_prev_clip,
         sizeof(shared_->clip_to_prev_clip));
  memcpy(shared_->prev_clip_to_clip, constants.prev_clip_to_clip,
         sizeof(shared_->prev_clip_to_clip));
  memcpy(shared_->camera_pos, constants.pos, sizeof(shared_->camera_pos));
  memcpy(shared_->camera_right, constants.right, sizeof(shared_->camera_right));
  memcpy(shared_->camera_up, constants.up, sizeof(shared_->camera_up));
  memcpy(shared_->camera_fwd, constants.fwd, sizeof(shared_->camera_fwd));
  shared_->camera_near = constants.near_plane;
  shared_->camera_far = constants.far_plane;
  shared_->camera_fov = constants.fov;
  shared_->camera_aspect = constants.aspect;
  shared_->mvec_scale[0] = constants.mvec_scale[0];
  shared_->mvec_scale[1] = constants.mvec_scale[1];
}

bool DlssClient::SubmitFrame(float jitter_x, float jitter_y,
                             bool reset_history) {
  if (!shared_ || !healthy_ || !ready_) return false;

  ++frame_index_;
  shared_->jitter_x = jitter_x;
  shared_->jitter_y = jitter_y;
  shared_->reset_history =
      (reset_history || pending_history_reset_) ? 1u : 0u;
  pending_history_reset_ = false;
  // Published last: the helper treats a new frame_index as "everything else
  // in this struct is now valid for that frame".
  shared_->frame_index = frame_index_;

  // Signalled on the game's own queue, so it lands only after the copies into
  // color_in have actually executed.
  if (FAILED(device_->cmd_queue()->Signal(ready_fence_.get(), frame_index_))) {
    healthy_ = false;
    return false;
  }

  // Deliberately no wait here: the whole point of the frame slots is that the
  // helper works on this frame while the game moves on to the next one.
  return true;
}

GpuTexture *DlssClient::AcquirePreviousResult() {
  if (!shared_ || !healthy_ || !ready_) return nullptr;
  // Frame 1 has no predecessor; nothing to present until frame 2.
  if (frame_index_ < 2) return nullptr;
  const uint64_t wanted = frame_index_ - 1;

  // CPU wait, never a queue wait -- see the class comment. It should not
  // actually wait: the helper has had a whole frame to finish this one, so a
  // miss here means the helper is broken or gone, not merely behind.
  if (done_fence_->GetCompletedValue() < wanted) {
    if (FAILED(done_fence_->SetEventOnCompletion(wanted, done_event_))) {
      healthy_ = false;
      return nullptr;
    }
    if (WaitForSingleObject(done_event_, kFrameWaitTimeoutMs) != WAIT_OBJECT_0) {
      ++consecutive_timeouts_;
      if (consecutive_timeouts_ >= kMaxConsecutiveTimeouts) {
        LOG(AixLog::Severity::error)
            << "DLSS: helper missed " << consecutive_timeouts_
            << " frames in a row; disabling it rather than paying "
            << kFrameWaitTimeoutMs << "ms a frame forever.\n";
        healthy_ = false;
      }
      // A frame the upscaler never saw is a hole in its history.
      pending_history_reset_ = true;
      return nullptr;
    }
  }
  consecutive_timeouts_ = 0;

  if (shared_->failed_frames != 0) {
    LOG(AixLog::Severity::error)
        << "DLSS: helper reported " << shared_->failed_frames
        << " failed frames, last hr=0x" << std::hex << shared_->last_hresult
        << std::dec << "; disabling.\n";
    healthy_ = false;
    return nullptr;
  }
  return color_out_[static_cast<size_t>(wanted % DlssIpc::kFrameSlots)].Get();
}

}  // namespace Dx8to12
