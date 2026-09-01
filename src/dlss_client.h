#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "dlss_ipc_protocol.h"
#include "util.h"

struct ID3D12Resource;
struct ID3D12Fence;

namespace Dx8to12 {

class Device;
class GpuTexture;

// Drives the x64 DLAA/DLSS helper process.
//
// Kept separate from RtHelperClient on purpose. The two start for different
// reasons, fail in different ways, and version independently; folding this
// into that class would mean one feature's stall or protocol bump showing up
// as the other's.
//
// The safety property this class is built around: **the game's GPU queue
// never waits on the helper.** x86 signals a fence the helper waits on, but
// reads the result back with a CPU wait that has a timeout. A GPU queue wait
// on a fence a crashed or hung helper will never signal is precisely how this
// project produced repeated device removals and two system hangs; a CPU wait
// can give up, disable the feature, and let the frame finish normally.
class DlssClient {
 public:
  explicit DlssClient(Device *device);
  ~DlssClient();

  DlssClient(const DlssClient &) = delete;
  DlssClient &operator=(const DlssClient &) = delete;

  // Creates the shared resources for this output size and launches the
  // helper. Safe to call again after a device Reset; tears the old one down
  // first. Returns false if anything failed, in which case the caller must
  // carry on without DLAA rather than treat it as fatal.
  bool Start(uint32_t width, uint32_t height, DlssIpc::Mode mode);
  void Stop();

  // True once the helper has reported itself ready. Until then every frame
  // takes the ordinary path.
  bool ready() const { return ready_; }
  // Turns false permanently once the helper has failed or timed out often
  // enough that waiting on it is costing more than it returns.
  bool healthy() const { return healthy_; }

  // The texture the game's scene should be copied into, and the one the
  // result comes back in. Both are x86-owned.
  GpuTexture *color_in() const { return color_in_.Get(); }
  GpuTexture *color_out() const { return color_out_.Get(); }

  // Records nothing itself. Call after the frame's copies into color_in have
  // been recorded and the command list submitted: signals the helper, waits
  // for it on the CPU with a timeout, and returns true if color_out now holds
  // this frame's result. On false the caller must fall back to presenting the
  // scene target unchanged.
  bool SubmitFrameAndWait(float jitter_x, float jitter_y, bool reset_history);

  // Discards temporal history on the next frame -- a camera cut or a device
  // Reset makes the previous frame meaningless to an upscaler.
  void RequestHistoryReset() { pending_history_reset_ = true; }

 private:
  void CloseSharedObjects();

  Device *device_ = nullptr;
  bool ready_ = false;
  bool healthy_ = false;

  HANDLE mapping_ = nullptr;
  DlssIpc::Handshake *shared_ = nullptr;
  PROCESS_INFORMATION helper_process_ = {};

  ComPtr<GpuTexture> color_in_;
  ComPtr<GpuTexture> color_out_;
  HANDLE color_in_handle_ = nullptr;
  HANDLE color_out_handle_ = nullptr;

  ComPtr<ID3D12Fence> ready_fence_;
  ComPtr<ID3D12Fence> done_fence_;
  HANDLE ready_fence_handle_ = nullptr;
  HANDLE done_fence_handle_ = nullptr;
  HANDLE done_event_ = nullptr;

  uint64_t frame_index_ = 0;
  uint32_t consecutive_timeouts_ = 0;
  bool pending_history_reset_ = true;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace Dx8to12
