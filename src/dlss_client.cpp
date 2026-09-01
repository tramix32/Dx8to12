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

std::wstring UniqueName(const wchar_t *suffix) {
  return std::wstring(L"Local\\Dx8to12_dlss_") + std::to_wstring(GetCurrentProcessId()) +
         L"_" + suffix;
}

// The helper sits next to d3d8.dll, not next to the game's exe -- it ships
// with this DLL.
std::wstring HelperPath() {
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
  result = (slash == std::wstring::npos ? L"" : result.substr(0, slash + 1));
  return result + L"dx8to12_rt_helper.exe";
}

}  // namespace

DlssClient::DlssClient(Device *device) : device_(device) {}

DlssClient::~DlssClient() { Stop(); }

bool DlssClient::Start(uint32_t width, uint32_t height, DlssIpc::Mode mode) {
  Stop();
  width_ = width;
  height_ = height;
  if (width == 0 || height == 0) return false;

  const std::wstring map_name = UniqueName(L"map");
  const std::wstring color_in_name = UniqueName(L"colorin");
  const std::wstring color_out_name = UniqueName(L"colorout");
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
      .Width = width,
      .Height = height,
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
                           const std::wstring &name) {
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device_->device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(resource.GetForInit())))) {
      return false;
    }
    if (FAILED(device_->device()->CreateSharedHandle(
            resource.get(), nullptr, GENERIC_ALL, name.c_str(), out_handle))) {
      return false;
    }
    *out_texture = ComOwn(GpuTexture::InitFromResource(device_, resource));
    return true;
  };
  if (!create_shared(&color_in_, &color_in_handle_, color_in_name) ||
      !create_shared(&color_out_, &color_out_handle_, color_out_name)) {
    LOG(AixLog::Severity::error) << "DLSS: shared texture creation failed.\n";
    Stop();
    return false;
  }
  color_in_->SetName("dlss_color_in");
  color_out_->SetName("dlss_color_out");

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
  shared_->render_width = width;
  shared_->render_height = height;
  shared_->output_width = width;
  shared_->output_height = height;
  shared_->mode = static_cast<uint32_t>(mode);
  wcsncpy_s(shared_->color_in_name, color_in_name.c_str(), _TRUNCATE);
  wcsncpy_s(shared_->color_out_name, color_out_name.c_str(), _TRUNCATE);
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
  if (!CreateProcessW(helper.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &helper_process_)) {
    LOG(AixLog::Severity::error)
        << "DLSS: could not launch " << std::string(helper.begin(), helper.end())
        << ", error " << GetLastError() << "\n";
    Stop();
    return false;
  }

  healthy_ = true;
  pending_history_reset_ = true;
  frame_index_ = 0;
  consecutive_timeouts_ = 0;
  LOG(AixLog::Severity::info)
      << "DLSS: helper launched for " << width << "x" << height << ", mode "
      << static_cast<uint32_t>(mode) << ".\n";
  return true;
}

void DlssClient::CloseSharedObjects() {
  auto close = [](HANDLE *handle) {
    if (*handle) {
      CloseHandle(*handle);
      *handle = nullptr;
    }
  };
  close(&color_in_handle_);
  close(&color_out_handle_);
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
  color_in_.Reset();
  color_out_.Reset();
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

bool DlssClient::SubmitFrameAndWait(float jitter_x, float jitter_y,
                                    bool reset_history) {
  if (!shared_ || !healthy_) return false;

  // The helper reports itself ready once it has opened everything; until then
  // there is nothing to hand a frame to.
  if (!ready_) {
    const auto status = static_cast<DlssIpc::HelperStatus>(shared_->status);
    if (status == DlssIpc::HelperStatus::kReady) {
      ready_ = true;
      LOG(AixLog::Severity::info) << "DLSS: helper reported ready.\n";
    } else if (status != DlssIpc::HelperStatus::kStarting) {
      LOG(AixLog::Severity::error)
          << "DLSS: helper failed to start, status " << shared_->status
          << " hr=0x" << std::hex << shared_->hresult << std::dec
          << " -- carrying on without it.\n";
      healthy_ = false;
      return false;
    }
    return false;
  }

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

  // CPU wait, never a queue wait -- see the class comment.
  if (done_fence_->GetCompletedValue() < frame_index_) {
    if (FAILED(done_fence_->SetEventOnCompletion(frame_index_, done_event_))) {
      healthy_ = false;
      return false;
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
      // The history is now discontinuous -- this frame never went through.
      pending_history_reset_ = true;
      return false;
    }
  }
  consecutive_timeouts_ = 0;

  if (shared_->failed_frames != 0) {
    LOG(AixLog::Severity::error)
        << "DLSS: helper reported " << shared_->failed_frames
        << " failed frames, last hr=0x" << std::hex << shared_->last_hresult
        << std::dec << "; disabling.\n";
    healthy_ = false;
    return false;
  }
  return true;
}

}  // namespace Dx8to12
