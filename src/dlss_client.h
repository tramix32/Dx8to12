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

// Everything sl::Constants needs about the camera, computed on this side --
// the only side that has the game's matrices. Plain floats, row-vector
// convention (v * M) like the rest of this codebase and like D3D8.
struct DlssCameraConstants {
  float view_to_clip[16] = {};
  float clip_to_view[16] = {};
  float clip_to_prev_clip[16] = {};
  float prev_clip_to_clip[16] = {};
  float pos[3] = {};
  float right[3] = {};
  float up[3] = {};
  float fwd[3] = {};
  float near_plane = 0.f;
  float far_plane = 0.f;
  float fov = 0.f;
  float aspect = 0.f;
  float mvec_scale[2] = {};
};

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
  // render_* is the resolution the scene is drawn at, output_* the one the
  // result is presented at. Equal for DLAA; render smaller for DLSS, which is
  // the only arrangement that actually saves any time.
  bool Start(uint32_t render_width, uint32_t render_height,
             uint32_t output_width, uint32_t output_height, DlssIpc::Mode mode);
  void Stop();

  // Advances the startup state machine and returns whether the helper is
  // ready to be handed a frame. Must be called every presented frame, and
  // must NOT be gated on ready(): it is what makes ready() ever become true,
  // and it is also what notices a helper that started and then died.
  bool PollReady();

  // Last value PollReady computed. Only meaningful after it has been called.
  bool ready() const { return ready_; }
  // Turns false permanently once the helper has failed or timed out often
  // enough that waiting on it is costing more than it returns.
  bool healthy() const { return healthy_; }
  bool helper_running() const { return helper_process_.hProcess != nullptr; }
  uint32_t render_width() const { return render_width_; }
  uint32_t render_height() const { return render_height_; }
  uint32_t output_width() const { return output_width_; }
  uint32_t output_height() const { return output_height_; }
  // Straight from the shared handshake, so a mod's panel can show why a start
  // failed rather than only that it did.
  uint32_t helper_status() const;
  uint32_t failed_frames() const;
  uint32_t preset() const;
  uint32_t neural_rendering_active() const;
  uint32_t neural_rendering_available() const;
  const char *neural_rendering_runtime() const;

  // The texture the game's scene should be copied into, and the one the
  // result comes back in. Both are x86-owned.
  // The slot this frame's inputs go into. Valid between frames; changes on
  // every SubmitFrame.
  GpuTexture *color_in() const { return color_in_[WriteSlot()].Get(); }
  GpuTexture *depth_in() const { return depth_in_[WriteSlot()].Get(); }
  GpuTexture *mvec_in() const { return mvec_in_[WriteSlot()].Get(); }

  // Hands this frame's inputs to the helper. Call after the copies into the
  // write slot have been recorded *and the command list submitted*, so the
  // fence this signals lands after them. Returns immediately -- the helper
  // works on this frame while the game gets on with the next one.
  // Call before SubmitFrame. Separate from it because the caller computes
  // these from matrices this class has no business knowing about.
  void SetCameraConstants(const DlssCameraConstants &constants);

  bool SubmitFrame(float jitter_x, float jitter_y, bool reset_history);

  // The finished result of the *previous* frame, or nullptr if the helper has
  // not got there yet (in which case the caller presents the scene target
  // unchanged for this frame). Waits only briefly: the helper has had a whole
  // frame already, so a miss means something is wrong rather than slow.
  GpuTexture *AcquirePreviousResult();

  // Discards temporal history on the next frame -- a camera cut or a device
  // Reset makes the previous frame meaningless to an upscaler.
  void RequestHistoryReset() { pending_history_reset_ = true; }

  // A device Reset invalidates more than the previous frame: the upscaler's
  // own resources were built against a device generation that is gone.
  // Discarding history is not enough on its own -- see the comment on
  // device_generation in the protocol.
  void NotifyDeviceReset() {
    ++device_generation_;
    pending_history_reset_ = true;
  }

 private:
  void CloseSharedObjects();

  Device *device_ = nullptr;
  bool ready_ = false;
  bool healthy_ = false;

  HANDLE mapping_ = nullptr;
  DlssIpc::Handshake *shared_ = nullptr;
  PROCESS_INFORMATION helper_process_ = {};

  // Slot the next SubmitFrame will write into. frame_index_ is the last one
  // submitted, so the next frame is frame_index_ + 1.
  size_t WriteSlot() const {
    return static_cast<size_t>((frame_index_ + 1) % DlssIpc::kFrameSlots);
  }

  ComPtr<GpuTexture> color_in_[DlssIpc::kFrameSlots];
  ComPtr<GpuTexture> color_out_[DlssIpc::kFrameSlots];
  ComPtr<GpuTexture> depth_in_[DlssIpc::kFrameSlots];
  ComPtr<GpuTexture> mvec_in_[DlssIpc::kFrameSlots];
  HANDLE color_in_handle_[DlssIpc::kFrameSlots] = {};
  HANDLE color_out_handle_[DlssIpc::kFrameSlots] = {};
  HANDLE depth_in_handle_[DlssIpc::kFrameSlots] = {};
  HANDLE mvec_in_handle_[DlssIpc::kFrameSlots] = {};

  ComPtr<ID3D12Fence> ready_fence_;
  ComPtr<ID3D12Fence> done_fence_;
  HANDLE ready_fence_handle_ = nullptr;
  HANDLE done_fence_handle_ = nullptr;
  HANDLE done_event_ = nullptr;

  uint64_t frame_index_ = 0;
  uint32_t consecutive_timeouts_ = 0;
  // When Start() launched the helper, so a helper that never reports ready
  // is eventually given up on instead of being polled forever.
  ULONGLONG start_tick_ = 0;
  bool pending_history_reset_ = true;
  uint32_t device_generation_ = 0;
  uint32_t render_width_ = 0;
  uint32_t render_height_ = 0;
  uint32_t output_width_ = 0;
  uint32_t output_height_ = 0;
};

}  // namespace Dx8to12
