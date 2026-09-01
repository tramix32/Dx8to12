#pragma once

#include <cstdint>

// Shared memory layout between the x86 shim and the x64 DLAA/DLSS helper.
//
// Deliberately separate from rt_ipc_protocol.h rather than more fields on
// Handshake: the two features have different lifetimes (this one starts when
// TemporalAA is switched on, the RT one when a lighting mode demands it),
// different failure modes, and different version cadences. Sharing one struct
// would mean a change to either forcing a version bump that invalidates the
// other, and one feature's stall showing up as the other's.
//
// Why a helper process at all: Streamline/NGX ships x64-only, and the game is
// a 32-bit process. That is not a limitation of this codebase -- it is why the
// helper exists.
//
// Bump kVersion for every incompatible layout change.
namespace Dx8to12::DlssIpc {

inline constexpr uint32_t kMagic = 0x444C4141;  // 'DLAA'
inline constexpr uint32_t kVersion = 2;

// Frame slots, so the game does not have to wait for the helper inside the
// frame it just handed over. In frame N the game writes inputs to slot
// N % kFrameSlots and presents the result of frame N-1 from the other slot;
// the helper works on N meanwhile.
//
// Two is enough, and the reason is worth stating: slot N%2 is only rewritten
// in frame N+2, and frame N+1 has already confirmed that frame N finished. A
// third slot would only help if the helper regularly took longer than a whole
// frame, which would be a problem to fix rather than to buffer around.
//
// The cost is one frame of latency on the scene (the HUD, drawn afterwards,
// stays current) and twice the shared texture memory.
inline constexpr uint32_t kFrameSlots = 2;

enum class HelperStatus : uint32_t {
  kStarting = 0,
  kReady = 1,
  kAdapterNotFound = 2,
  kDeviceCreateFailed = 3,
  kProtocolMismatch = 4,
  kSharedOpenFailed = 5,
  kStreamlineInitFailed = 6,
  kFeatureUnavailable = 7,
  kEvaluateFailed = 8,
};

// What the helper should do with a frame. Loopback exists so the transport
// can be proven in the real game before Streamline is added to the picture:
// with it, the output must be pixel-identical to the input, so any difference
// on screen is a transport bug and nothing else.
enum class Mode : uint32_t {
  kLoopback = 0,
  kDlaa = 1,
  kDlss = 2,
};

// All four textures are created by x86 and opened by the helper. That
// direction is deliberate: it is the one arrangement that has proven stable
// here (the H4 shadow output uses it), unlike the earlier design where the
// helper created resources the game's device then had to import.
struct Handshake {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  uint32_t shim_process_id = 0;
  uint32_t helper_process_id = 0;
  uint32_t adapter_luid_low = 0;
  int32_t adapter_luid_high = 0;
  uint32_t status = static_cast<uint32_t>(HelperStatus::kStarting);
  int32_t hresult = 0;

  // Shared resource names, filled in by x86 before the helper is launched.
  wchar_t color_in_name[kFrameSlots][128] = {};
  wchar_t depth_in_name[kFrameSlots][128] = {};
  wchar_t mvec_in_name[kFrameSlots][128] = {};
  wchar_t color_out_name[kFrameSlots][128] = {};
  // x86 signals this once a frame's inputs are recorded; the helper waits on
  // it. The helper signals done_fence; x86 waits on it *on the CPU, with a
  // timeout* -- never as a GPU queue wait. A GPU wait on a fence a crashed
  // helper will never signal is exactly how this project produced repeated
  // device removals; a CPU wait can simply give up and fall back.
  wchar_t ready_fence_name[128] = {};
  wchar_t done_fence_name[128] = {};

  // Both fences use the frame index as their value, so there is one counter
  // and no way for the two sides to disagree about which frame is meant.
  uint64_t frame_index = 0;
  uint64_t completed_frame_index = 0;

  uint32_t render_width = 0;
  uint32_t render_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;

  // The sub-pixel offset the frame was rendered with, in pixels. The upscaler
  // has to be told this or it cannot tell jitter apart from motion.
  float jitter_x = 0.f;
  float jitter_y = 0.f;

  uint32_t mode = static_cast<uint32_t>(Mode::kLoopback);
  // 1 = discard temporal history. Set on the first frame, after a device
  // Reset, and on a camera cut -- feeding history across one produces a
  // smeared image that looks like an upscaler bug.
  uint32_t reset_history = 0;

  // Set by the helper. A non-zero failure count is what makes x86 stop
  // waiting on a helper that is alive but not working.
  int32_t last_hresult = 0;
  uint32_t failed_frames = 0;
  uint32_t shutdown_requested = 0;

  // What the helper actually sees after opening each shared texture. Reported
  // back so the loopback stage can prove the sharing worked rather than only
  // that nothing crashed: a handle that opened but resolved to the wrong
  // resource shows up here as the wrong size or format, and a mismatched
  // format is silent corruption rather than an error.
  uint32_t seen_color_in_width = 0;
  uint32_t seen_color_in_height = 0;
  uint32_t seen_color_in_format = 0;
  uint32_t seen_depth_in_width = 0;
  uint32_t seen_depth_in_format = 0;
  uint32_t seen_mvec_in_width = 0;
  uint32_t seen_mvec_in_format = 0;
};

}  // namespace Dx8to12::DlssIpc
