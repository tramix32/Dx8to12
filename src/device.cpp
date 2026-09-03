#include "device.h"

#include <cmrc/cmrc.hpp>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <intrin.h>
#include <set>
#include <sstream>
#include <utility>

#include "SimpleMath.h"
#include "aixlog.hpp"
#include "buffer.h"
#include "config.h"
#include "dlss_client.h"
#include "dynamic_ring_buffer.h"
#include "raytracing.h"
#include "rt_helper_client.h"
#include "shader_parser.h"
#include "surface.h"
#include "texture.h"
#include "utils/dx_utils.h"
#include "vertex_shader.h"

#ifdef DX8TO12_USE_ALLOCATOR
#include "D3D12MemAlloc.h"
#endif

#undef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL            \
  []() {                              \
    LOG_ERROR() << "Invalid call!\n"; \
    return MAKE_D3DHRESULT(2156);     \
  }()

#define SCOPED_MARKER(annotation) ScopedGpuMarker(cmd_list_.Get(), annotation)

CMRC_DECLARE(Dx8to12_shaders);

namespace Dx8to12 {

// Protocol v12 transports bounded CPU snapshots only. The helper creates all
// DXR input resources on its own x64 device, so this path no longer mutates
// the game's command list or shares geometry allocations across devices.
constexpr bool kEnableExperimentalHelperSceneSubmission = true;

// The single live Device instance, exposed to dx8to12_api.cpp so the C mod
// API can reach it without every caller needing a Device* of their own.
// There is only ever one Device (one D3D12 device/swap chain per process),
// set at the end of Device::Create and cleared in ~Device.
static Device *g_current_device = nullptr;

Device *GetCurrentDeviceForModApi() { return g_current_device; }

#ifdef DX8TO12_ENABLE_MINDEBUG
namespace {

// GTA: Vice City 1.0 / RenderWare call-site diagnostics for the intermittent
// missing-near-road investigation.  RenderDoc established that the affected
// pixel is missing a whole material draw, rather than sampling a corrupt D3D12
// texture.  The local reVC source plus Ghidra identify the renderer involved:
//
//   0x674380  default material render
//   0x674510  MatFX dual-pass render
//   0x674EE0  MatFX environment-map render
//   0x6756F0  MatFX bump-map render
//   0x676460  top-level MatFX mesh callback
//
// All calls below go through tiny RenderWare D3D8 wrappers first, so a normal
// stack trace stops at the same wrapper address for every material.  The x86
// CALL instructions still leave their exact return addresses on the raw
// stack, however.  Match those known return addresses and aggregate them for
// 30 presented frames: this says which RenderWare branch stopped issuing the
// missing draw without producing the multi-gigabyte, timing-altering log that
// the previous per-SetTexture diagnostic did.
constexpr uintptr_t kGtaPreferredImageBase = 0x00400000u;

constexpr std::array<uintptr_t, 29> kRwSetTextureCallSites = {
    0x00674393u, 0x0067454fu, 0x006745feu, 0x00674717u, 0x0067475bu,
    0x0067483bu, 0x006748c1u, 0x006749c4u, 0x00674a2bu, 0x00674a54u,
    0x00674b0bu, 0x00674b4cu, 0x00674b93u, 0x00674c7bu, 0x00674cd5u,
    0x00674e81u, 0x00674fadu, 0x0067502fu, 0x0067523cu, 0x006752aeu,
    0x0067542au, 0x006755dcu, 0x00675743u, 0x006757a0u, 0x006758b4u,
    0x0067596du, 0x00676517u, 0x00678c49u, 0x00678cc1u,
};

constexpr std::array<uintptr_t, 16> kRwIndexedDrawCallSites = {
    0x0067442du, 0x006744e2u, 0x00674ae4u, 0x00674ddeu,
    0x006750cfu, 0x0067521du, 0x0067528fu, 0x00675318u,
    0x0067540du, 0x006754f1u, 0x00675894u, 0x006758fau,
    0x00675935u, 0x006759e0u, 0x00675a4du, 0x00678d8au,
};

constexpr std::array<uintptr_t, 14> kRwPrimitiveDrawCallSites = {
    0x00674443u, 0x006744f8u, 0x00674cc1u, 0x00674df4u, 0x006751b0u,
    0x00675230u, 0x006752a2u, 0x00675420u, 0x00675504u, 0x006758a8u,
    0x00675949u, 0x006759f4u, 0x00675a61u, 0x00678da0u,
};

template <size_t N>
size_t FindGtaCallerOnStack(const std::array<uintptr_t, N> &call_sites) {
  const auto *stack =
      reinterpret_cast<const uintptr_t *>(_AddressOfReturnAddress());
  MEMORY_BASIC_INFORMATION memory = {};
  if (VirtualQuery(stack, &memory, sizeof(memory)) != sizeof(memory) ||
      memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) ||
      (memory.Protect & PAGE_NOACCESS)) {
    return N;
  }

  const uintptr_t region_end = reinterpret_cast<uintptr_t>(memory.BaseAddress) +
                               static_cast<uintptr_t>(memory.RegionSize);
  // Release-mindebug's one-frame spatial snapshot adds a fairly large local
  // frame to DrawIndexedPrimitive.  In that build the RenderWare return
  // address can sit beyond the first 64 DWORDs even though it is still only a
  // couple of call frames above us.  Keep the walk bounded, but large enough
  // that enabling the diagnostic does not make every draw "unmatched".
  constexpr size_t kMaxStackSlots = 256;
  const uintptr_t requested_end =
      reinterpret_cast<uintptr_t>(stack + kMaxStackSlots);
  const auto *scan_end = reinterpret_cast<const uintptr_t *>(
      std::min(region_end, requested_end));
  static const uintptr_t rebase =
      reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) -
      kGtaPreferredImageBase;

  // All arrays are sorted.  Most stack values fail the range check, while a
  // candidate needs only a binary search rather than N comparisons.
  const uintptr_t first_return = call_sites.front() + 5u + rebase;
  const uintptr_t last_return = call_sites.back() + 5u + rebase;
  size_t generic_fallback = N;
  for (const uintptr_t *slot = stack; slot < scan_end; ++slot) {
    if (*slot < first_return || *slot > last_return) continue;
    // Every listed instruction is a five-byte E8 rel32 CALL.
    const uintptr_t preferred_call_site = *slot - 5u - rebase;
    const auto match =
        std::lower_bound(call_sites.begin(), call_sites.end(),
                         preferred_call_site);
    if (match != call_sites.end() && *match == preferred_call_site) {
      const size_t index = static_cast<size_t>(match - call_sites.begin());
      // 0x678cxx/0x678dxx are the common RenderWare D3D8 wrappers. They are
      // nearest on the stack and used by nearly every world draw, so returning
      // them immediately hides the more useful MatFX/default-material call
      // site a little deeper down. Preserve the wrapper as a fallback for
      // ordinary draws, but prefer a concrete renderer branch when present.
      if (preferred_call_site < 0x00678000u) return index;
      if (generic_fallback == N) generic_fallback = index;
    }
  }
  return generic_fallback;
}

struct RwCallDiagnostics {
  std::array<uint32_t, kRwSetTextureCallSites.size()> texture_nonnull = {};
  std::array<uint32_t, kRwSetTextureCallSites.size()> texture_null = {};
  std::array<uint32_t, kRwSetTextureCallSites.size()> texture_input_nonnull = {};
  std::array<uint32_t, kRwIndexedDrawCallSites.size()> indexed_draws = {};
  std::array<uint32_t, kRwIndexedDrawCallSites.size()> indexed_emitted = {};
  std::array<uint32_t, kRwIndexedDrawCallSites.size()> indexed_no_ib = {};
  std::array<uint32_t, kRwIndexedDrawCallSites.size()> indexed_zero_clamp = {};
  std::array<uint32_t, kRwIndexedDrawCallSites.size()> indexed_partial_clamp =
      {};
  std::array<uint32_t, kRwIndexedDrawCallSites.size()> indexed_prepare_failed =
      {};
  std::array<uint32_t, kRwPrimitiveDrawCallSites.size()> primitive_draws = {};
  uint32_t texture_unmatched = 0;
  uint32_t indexed_unmatched = 0;
  uint32_t primitive_unmatched = 0;
  uint32_t indexed_emitted_total = 0;
  uint32_t indexed_no_ib_total = 0;
  uint32_t indexed_zero_clamp_total = 0;
  uint32_t indexed_partial_clamp_total = 0;
  uint32_t indexed_prepare_failed_total = 0;
  uint32_t presented_frames = 0;
  uint64_t first_frame = 0;
};

static RwCallDiagnostics g_rw_call_diagnostics;

// One-frame draw snapshots used to compare the last visible distant LOD with
// the first frame where the near geometry is missing.  Keeping the strings in
// memory and writing them only at Present avoids turning every captured draw
// into synchronous file I/O (which previously changed the timing enough to
// hide high-FPS bugs).
static bool g_rw_snapshot_active = false;
static char g_rw_snapshot_label = '?';
static bool g_rw_snapshot_f8_was_down = false;
static bool g_rw_snapshot_f9_was_down = false;
static uint64_t g_rw_snapshot_frame = 0;
static std::vector<std::string> g_rw_snapshot_lines;
static int16_t g_rw_target_lod_model = -1;
static int16_t g_rw_target_near_model = -1;

static void RememberGtaLodPair(int16_t lod_model) {
  // The beach road IDs are contiguous in the retail IDE. Downtown uses a
  // different allocation, so retain its explicitly proven IPL pair too.
  if (lod_model >= 4259 && lod_model <= 4272) {
    g_rw_target_lod_model = lod_model;
    g_rw_target_near_model = lod_model - 14;
  } else if (lod_model == 2031) {
    g_rw_target_lod_model = 2031;
    g_rw_target_near_model = 2060;
  }
}

static void WriteRwDiagnosticLine(const std::string &line) {
  // This is deliberately separate from AixLog: ordinary LOG(...) calls are
  // compiled out in both release profiles.  The file is opened lazily, once,
  // and flushed only twice per 30 frames.
  static std::ofstream output(CURRENT_SOURCE_DIR "/log.txt",
                              std::ofstream::out | std::ofstream::trunc);
  static bool wrote_header = false;
  if (!output) return;
  if (!wrote_header) {
    output << "DX8TO12-MINDEBUG validation=off compiled=" __DATE__ " "
               __TIME__
#ifdef DX8TO12_PASSTHROUGH_OOB_INDICES
               " oobIndices=passthrough\n";
#else
               " oobIndices=clamped\n";
#endif
    wrote_header = true;
  }
  output << line;
  output.flush();
}

static bool RwSnapshotActive() { return g_rw_snapshot_active; }

static void AppendRwSnapshotLine(std::string line) {
  if (g_rw_snapshot_active) g_rw_snapshot_lines.push_back(std::move(line));
}

struct GtaVec3 {
  float x;
  float y;
  float z;
};

struct GtaColPoint {
  GtaVec3 point;
  float field_c;
  GtaVec3 normal;
  float field_1c;
  uint8_t surface_a;
  uint8_t piece_a;
  uint8_t surface_b;
  uint8_t piece_b;
  uint32_t field_24;
};
static_assert(sizeof(GtaColPoint) == 0x28);

template <typename T>
static bool ReadGtaMemory(uintptr_t address, T *value) {
  SIZE_T bytes_read = 0;
  return ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), value,
                           sizeof(*value), &bytes_read) &&
         bytes_read == sizeof(*value);
}

static std::string DescribeGtaCenterRay() {
  const uintptr_t image_base =
      reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t rebase = image_base - kGtaPreferredImageBase;

  // Retail VC 1.0 CWorld::ProcessLineOfSight. Refuse to call through this
  // address unless the unmodified prologue is present; an unsupported exe is
  // a diagnostic miss, not permission to jump into arbitrary game code.
  constexpr uintptr_t kProcessLineOfSight = 0x004d92d0u;
  constexpr std::array<uint8_t, 10> kExpectedPrologue = {
      0x53, 0x56, 0x57, 0x55, 0x81, 0xec, 0x60, 0x02, 0x00, 0x00};
  std::array<uint8_t, kExpectedPrologue.size()> actual_prologue = {};
  SIZE_T bytes_read = 0;
  if (!ReadProcessMemory(
          GetCurrentProcess(),
          reinterpret_cast<const void *>(kProcessLineOfSight + rebase),
          actual_prologue.data(), actual_prologue.size(), &bytes_read) ||
      bytes_read != actual_prologue.size() ||
      actual_prologue != kExpectedPrologue) {
    return "RWRAY unsupported-exe\n";
  }

  GtaVec3 camera = {};
  GtaVec3 forward = {};
  if (!ReadGtaMemory(0x007e46b8u + rebase, &camera) ||
      !ReadGtaMemory(0x007e4698u + rebase, &forward) ||
      !std::isfinite(camera.x) || !std::isfinite(camera.y) ||
      !std::isfinite(camera.z) || !std::isfinite(forward.x) ||
      !std::isfinite(forward.y) || !std::isfinite(forward.z)) {
    return "RWRAY camera-unavailable\n";
  }

  const float forward_length = std::sqrt(forward.x * forward.x +
                                         forward.y * forward.y +
                                         forward.z * forward.z);
  if (forward_length < 0.5f || forward_length > 1.5f) {
    std::ostringstream bad_forward;
    bad_forward << "RWRAY bad-forward camera=(" << camera.x << ","
                << camera.y << "," << camera.z << ") forward=(" << forward.x
                << "," << forward.y << "," << forward.z << ")\n";
    return bad_forward.str();
  }

  const float ray_length = 2000.f / forward_length;
  const GtaVec3 target = {camera.x + forward.x * ray_length,
                          camera.y + forward.y * ray_length,
                          camera.z + forward.z * ray_length};
  GtaColPoint collision = {};
  uintptr_t entity = 0;
  using ProcessLineOfSightFn = bool(__cdecl *)(
      const GtaVec3 &, const GtaVec3 &, GtaColPoint &, uintptr_t &, bool, bool,
      bool, bool, bool, bool, bool, bool);
  const auto process_line_of_sight = reinterpret_cast<ProcessLineOfSightFn>(
      kProcessLineOfSight + rebase);
  // Roads are buildings. Include objects and dummies as well, but exclude
  // moving actors so Tommy/a vehicle between the camera and the road cannot
  // steal the diagnostic hit.
  const bool hit = process_line_of_sight(
      camera, target, collision, entity, true, false, false, true, true,
      false, false, false);

  std::ostringstream line;
  line << "RWRAY hit=" << hit << " camera=(" << camera.x << "," << camera.y
       << "," << camera.z << ") forward=(" << forward.x << "," << forward.y
       << "," << forward.z << ")";
  if (!hit || entity == 0) {
    line << " entity=0\n";
    return line.str();
  }

  uint8_t type_and_status = 0;
  int16_t model = -1;
  uintptr_t rw_object = 0;
  GtaVec3 entity_position = {};
  const bool entity_readable =
      ReadGtaMemory(entity + 0x50u, &type_and_status) &&
      ReadGtaMemory(entity + 0x5cu, &model) &&
      ReadGtaMemory(entity + 0x4cu, &rw_object) &&
      ReadGtaMemory(entity + 0x34u, &entity_position);
  line << " point=(" << collision.point.x << "," << collision.point.y << ","
       << collision.point.z << ") entity=0x" << std::hex << entity << std::dec;
  if (!entity_readable) {
    line << " entity-unreadable\n";
    return line.str();
  }
  line << " type=" << static_cast<int>(type_and_status & 7u)
       << " model=" << model << " epos=(" << entity_position.x << ","
       << entity_position.y << "," << entity_position.z << ") rw=0x"
       << std::hex << rw_object << std::dec;

  // The operand of `mov ebx, 0x94ddd0` in LoadAllRequestedModels is patched
  // by limit adjusters when they relocate CStreaming::ms_aInfoForModel. Read
  // that live operand instead of assuming the retail array address. Each
  // CStreamingInfo is 0x14 bytes; load state and flags are at +8/+9.
  uintptr_t streaming_base = 0;
  uint32_t streaming_base32 = 0;
  if (ReadGtaMemory(0x0040b6e3u + rebase, &streaming_base32))
    streaming_base = streaming_base32;
  uint8_t load_state = 0xff;
  uint8_t stream_flags = 0xff;
  if (model >= 0 && streaming_base != 0) {
    const uintptr_t stream_info =
        streaming_base + static_cast<uintptr_t>(model) * 0x14u;
    ReadGtaMemory(stream_info + 8u, &load_state);
    ReadGtaMemory(stream_info + 9u, &stream_flags);
  }
  line << " streamBase=0x" << std::hex << streaming_base
       << " state=0x" << static_cast<int>(load_state)
       << " flags=0x" << static_cast<int>(stream_flags) << std::dec << "\n";
  return line.str();
}

// A CEntity normally remains in a caller's saved register/spill slot while
// RenderWare emits its atomic.  For the one draw which really covers the
// screen centre, inspect that bounded part of the current stack and retain
// only values which look like a live VC entity at the atomic's world
// translation.  This gives us the exact model ID without touching the
// renderer or changing streaming state.
static std::string DescribeGtaCenterDrawEntities(float world_x, float world_y,
                                                 float world_z) {
  const uintptr_t image_base =
      reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t rebase = image_base - kGtaPreferredImageBase;
  const uintptr_t image_end = image_base + 0x00614000u;
  const auto *stack =
      reinterpret_cast<const uintptr_t *>(_AddressOfReturnAddress());
  MEMORY_BASIC_INFORMATION memory = {};
  if (VirtualQuery(stack, &memory, sizeof(memory)) != sizeof(memory) ||
      memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) ||
      (memory.Protect & PAGE_NOACCESS)) {
    return "RWCENTER-STACK unavailable\n";
  }
  constexpr size_t kMaxStackSlots = 2048;
  const uintptr_t region_end = reinterpret_cast<uintptr_t>(memory.BaseAddress) +
                               static_cast<uintptr_t>(memory.RegionSize);
  const auto *scan_end = reinterpret_cast<const uintptr_t *>(std::min(
      region_end, reinterpret_cast<uintptr_t>(stack + kMaxStackSlots)));

  uintptr_t streaming_base = 0;
  uint32_t streaming_base32 = 0;
  if (ReadGtaMemory(0x0040b6e3u + rebase, &streaming_base32))
    streaming_base = streaming_base32;

  std::ostringstream line;
  line << "RWCENTER-STACK target=(" << world_x << "," << world_y << ","
       << world_z << ") matches=";
  size_t matches = 0;
  std::array<uintptr_t, 16> seen = {};
  for (const uintptr_t *slot = stack; slot < scan_end; ++slot) {
    const uintptr_t entity = *slot;
    if (entity < 0x01000000u ||
        std::find(seen.begin(), seen.end(), entity) != seen.end())
      continue;
    uintptr_t vtable = 0;
    uint8_t type_and_status = 0;
    int16_t model = -1;
    uintptr_t rw_object = 0;
    GtaVec3 position = {};
    if (!ReadGtaMemory(entity, &vtable) || vtable < image_base ||
        vtable >= image_end || !ReadGtaMemory(entity + 0x50u, &type_and_status) ||
        !ReadGtaMemory(entity + 0x5cu, &model) ||
        !ReadGtaMemory(entity + 0x4cu, &rw_object) ||
        !ReadGtaMemory(entity + 0x34u, &position))
      continue;
    const uint8_t type = type_and_status & 7u;
    if (type == 0 || type > 5 || model < 0 || model > 20000 ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z))
      continue;
    const float dx = position.x - world_x;
    const float dy = position.y - world_y;
    const float dz = position.z - world_z;
    // Atomic and entity transforms are ordinarily exact.  Ten metres still
    // admits parented/interior objects without filling the compact log.
    if (dx * dx + dy * dy + dz * dz > 100.f) continue;
    if (matches < seen.size()) seen[matches] = entity;
    ++matches;
    // Remember the actual centre-hit pair so frame B probes the same road
    // even if the player has moved to another block.
    RememberGtaLodPair(model);
    uint8_t load_state = 0xff;
    uint8_t stream_flags = 0xff;
    if (streaming_base != 0) {
      const uintptr_t stream_info =
          streaming_base + static_cast<uintptr_t>(model) * 0x14u;
      ReadGtaMemory(stream_info + 8u, &load_state);
      ReadGtaMemory(stream_info + 9u, &stream_flags);
    }
    line << " [slot=" << (slot - stack) << " ent=0x" << std::hex << entity
         << " model=" << std::dec << model << " type="
         << static_cast<int>(type) << " pos=(" << position.x << ","
         << position.y << "," << position.z << ") rw=0x" << std::hex
         << rw_object << " state=0x" << static_cast<int>(load_state)
         << " flags=0x" << static_cast<int>(stream_flags) << std::dec << "]";
  }
  line << "\n";
  return line.str();
}

// The matched IPL pair is nb_road02 (4246) and its distant replacement
// LODroad02 (4260).  Report their live streaming records for both snapshots;
// this is deliberately a read-only probe of the array Limit Adjuster may move.
static std::string DescribeGtaTargetRoadStreaming() {
  const uintptr_t image_base =
      reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t rebase = image_base - kGtaPreferredImageBase;
  uint32_t streaming_base32 = 0;
  if (!ReadGtaMemory(0x0040b6e3u + rebase, &streaming_base32) ||
      streaming_base32 == 0) {
    return "RWMODEL target streamBase=unavailable\n";
  }
  if (g_rw_target_lod_model < 0 || g_rw_target_near_model < 0)
    return "RWMODEL target=unknown\n";
  std::ostringstream line;
  line << "RWMODEL target streamBase=0x" << std::hex << streaming_base32;
  for (const int16_t model : {g_rw_target_near_model, g_rw_target_lod_model}) {
    const uintptr_t info = static_cast<uintptr_t>(streaming_base32) +
                           static_cast<uintptr_t>(model) * 0x14u;
    uint8_t state = 0xff;
    uint8_t flags = 0xff;
    ReadGtaMemory(info + 8u, &state);
    ReadGtaMemory(info + 9u, &flags);
    line << " m" << std::dec << model << "=s0x" << std::hex
         << static_cast<int>(state) << "/f0x" << static_cast<int>(flags);
  }
  line << std::dec << "\n";
  return line.str();
}

static std::string DescribeGtaTargetVisibleEntities() {
  if (g_rw_target_lod_model < 0 || g_rw_target_near_model < 0)
    return "RWENTITY target=unknown\n";
  const uintptr_t image_base =
      reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t rebase = image_base - kGtaPreferredImageBase;
  // GTA VC 1.0 CRenderer::ms_aVisibleEntityPtrs and its count. Both locations
  // are read-only diagnostic inputs; all individual pointers are validated
  // before their fields are inspected.
  constexpr uintptr_t kVisibleEntities = 0x007d54f8u;
  constexpr uintptr_t kVisibleCount = 0x00a0d1e4u;
  uint32_t count = 0;
  if (!ReadGtaMemory(kVisibleCount + rebase, &count) || count > 1000u)
    return "RWENTITY target visible-list=unavailable\n";
  std::ostringstream line;
  line << "RWENTITY target visibleCount=" << count << " near=" << g_rw_target_near_model
       << " lod=" << g_rw_target_lod_model;
  size_t matches = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uintptr_t entity = 0;
    if (!ReadGtaMemory(kVisibleEntities + rebase + i * sizeof(entity),
                       &entity) || entity == 0)
      continue;
    int16_t model = -1;
    uintptr_t rw_object = 0;
    uint8_t flags = 0;
    GtaVec3 pos = {};
    if (!ReadGtaMemory(entity + 0x5cu, &model) ||
        (model != g_rw_target_near_model && model != g_rw_target_lod_model) ||
        !ReadGtaMemory(entity + 0x4cu, &rw_object) ||
        !ReadGtaMemory(entity + 0x50u, &flags) ||
        !ReadGtaMemory(entity + 0x34u, &pos))
      continue;
    line << " [i=" << i << " m=" << model << " ent=0x" << std::hex
         << entity << " rw=0x" << rw_object << std::dec << " type="
         << static_cast<int>(flags & 7u) << " pos=(" << pos.x << ","
         << pos.y << "," << pos.z << ")]";
    ++matches;
  }
  line << " matches=" << matches << "\n";
  return line.str();
}

// VC 1.0's CModelInfo::ms_modelInfoPtrs operand.  Keep only the diagnosed
// near-road model in the fade state for one frame: SetupBigBuildingVisibility
// otherwise removes its LOD as soon as this byte reaches 255, even though the
// near mesh leaves the observed hole.  This is a reversible in-process
// compatibility probe, not a game-file patch.
//
// Gated behind its own CMake option (default OFF) rather than plain
// ENABLE_MINDEBUG: it writes to the game's own memory every single frame, so
// leaving it on silently changed game state underneath every measurement in
// every release-mindebug build.  A baseline run has to observe the unmodified
// game, so this now has to be asked for explicitly.
static void KeepGtaTargetRoadLodVisible() {
#ifndef DX8TO12_KEEP_TARGET_LOD
  return;
#else
  if (g_rw_target_near_model < 0) return;
  const uintptr_t image_base =
      reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t rebase = image_base - kGtaPreferredImageBase;
  uint32_t model_infos32 = 0;
  if (!ReadGtaMemory(0x0055f7e3u + rebase, &model_infos32) ||
      model_infos32 < 0x01000000u)
    return;
  uint32_t model_info32 = 0;
  const uintptr_t slot = static_cast<uintptr_t>(model_infos32) +
                         static_cast<uintptr_t>(g_rw_target_near_model) * 4u;
  if (!ReadGtaMemory(slot, &model_info32) || model_info32 < 0x01000000u)
    return;
  constexpr uintptr_t kSimpleModelInfoAlpha = 0x41u;
  const uint8_t fading_alpha = 0;
  SIZE_T written = 0;
  WriteProcessMemory(GetCurrentProcess(),
                     reinterpret_cast<void *>(static_cast<uintptr_t>(model_info32) +
                                               kSimpleModelInfoAlpha),
                     &fading_alpha, sizeof(fading_alpha), &written);
#endif  // DX8TO12_KEEP_TARGET_LOD
}

static void PollRwSnapshotHotkeys(uint64_t frame) {
  // A snapshot armed at the previous Present has now collected exactly the
  // frame that just ended. Flush it before accepting another request.
  if (g_rw_snapshot_active) {
    WriteRwDiagnosticLine(DescribeGtaTargetRoadStreaming());
    WriteRwDiagnosticLine(DescribeGtaTargetVisibleEntities());
    std::ostringstream header;
    header << "=== RWDRAW-SNAPSHOT " << g_rw_snapshot_label
           << " frame=" << g_rw_snapshot_frame
           << " lines=" << g_rw_snapshot_lines.size() << " ===\n";
    WriteRwDiagnosticLine(header.str());
    for (const std::string &line : g_rw_snapshot_lines)
      WriteRwDiagnosticLine(line);
    std::ostringstream footer;
    footer << "=== RWDRAW-SNAPSHOT " << g_rw_snapshot_label << " END ===\n";
    WriteRwDiagnosticLine(footer.str());
    g_rw_snapshot_lines.clear();
    g_rw_snapshot_active = false;
  }

  const SHORT f8_state = GetAsyncKeyState(VK_F8);
  const SHORT f9_state = GetAsyncKeyState(VK_F9);
  const bool f8_down = (f8_state & 0x8000) != 0;
  const bool f9_down = (f9_state & 0x8000) != 0;
  const bool f8_pressed = (f8_state & 1) != 0 ||
                          (f8_down && !g_rw_snapshot_f8_was_down);
  const bool f9_pressed = (f9_state & 1) != 0 ||
                          (f9_down && !g_rw_snapshot_f9_was_down);
  if (f8_pressed || f9_pressed) {
    g_rw_snapshot_label = f8_pressed ? 'A' : 'B';
    g_rw_snapshot_frame = frame + 1;
    g_rw_snapshot_lines.clear();
    g_rw_snapshot_lines.reserve(1024);
    g_rw_snapshot_active = true;
    std::ostringstream armed;
    armed << "RWDRAW-SNAPSHOT-ARMED " << g_rw_snapshot_label
          << " targetFrame=" << g_rw_snapshot_frame << "\n";
    WriteRwDiagnosticLine(armed.str());
  }
  g_rw_snapshot_f8_was_down = f8_down;
  g_rw_snapshot_f9_was_down = f9_down;
}

static void RecordRwTextureCall(bool input_nonnull, bool has_texture) {
  const size_t caller = FindGtaCallerOnStack(kRwSetTextureCallSites);
  if (caller == kRwSetTextureCallSites.size()) {
    ++g_rw_call_diagnostics.texture_unmatched;
  } else if (has_texture) {
    ++g_rw_call_diagnostics.texture_nonnull[caller];
  } else {
    ++g_rw_call_diagnostics.texture_null[caller];
  }
  if (caller < kRwSetTextureCallSites.size() && input_nonnull)
    ++g_rw_call_diagnostics.texture_input_nonnull[caller];
}

static size_t RecordRwDrawCall(bool indexed) {
  if (indexed) {
    const size_t caller = FindGtaCallerOnStack(kRwIndexedDrawCallSites);
    if (caller == kRwIndexedDrawCallSites.size())
      ++g_rw_call_diagnostics.indexed_unmatched;
    else
      ++g_rw_call_diagnostics.indexed_draws[caller];
    return caller;
  } else {
    const size_t caller = FindGtaCallerOnStack(kRwPrimitiveDrawCallSites);
    if (caller == kRwPrimitiveDrawCallSites.size())
      ++g_rw_call_diagnostics.primitive_unmatched;
    else
      ++g_rw_call_diagnostics.primitive_draws[caller];
    return caller;
  }
}

enum class RwIndexedEvent { Emitted, NoIndexBuffer, ZeroClamp, PartialClamp,
                            PrepareFailed };

static void RecordRwIndexedEvent(size_t caller, RwIndexedEvent event) {
  RwCallDiagnostics &stats = g_rw_call_diagnostics;
  std::array<uint32_t, kRwIndexedDrawCallSites.size()> *site_counts = nullptr;
  uint32_t *total = nullptr;
  switch (event) {
    case RwIndexedEvent::Emitted:
      site_counts = &stats.indexed_emitted;
      total = &stats.indexed_emitted_total;
      break;
    case RwIndexedEvent::NoIndexBuffer:
      site_counts = &stats.indexed_no_ib;
      total = &stats.indexed_no_ib_total;
      break;
    case RwIndexedEvent::ZeroClamp:
      site_counts = &stats.indexed_zero_clamp;
      total = &stats.indexed_zero_clamp_total;
      break;
    case RwIndexedEvent::PartialClamp:
      site_counts = &stats.indexed_partial_clamp;
      total = &stats.indexed_partial_clamp_total;
      break;
    case RwIndexedEvent::PrepareFailed:
      site_counts = &stats.indexed_prepare_failed;
      total = &stats.indexed_prepare_failed_total;
      break;
  }
  ++*total;
  if (caller < kRwIndexedDrawCallSites.size()) ++(*site_counts)[caller];
}

template <size_t N>
static void AppendRwSiteCounts(std::ostringstream &line,
                               const std::array<uintptr_t, N> &sites,
                               const std::array<uint32_t, N> &counts) {
  bool any = false;
  for (size_t i = 0; i < N; ++i) {
    if (counts[i] == 0) continue;
    line << (any ? "," : "") << "+0x" << std::hex
         << (sites[i] - kGtaPreferredImageBase) << std::dec << "="
         << counts[i];
    any = true;
  }
}

static void FlushRwCallDiagnostics(uint64_t frame) {
  RwCallDiagnostics &stats = g_rw_call_diagnostics;
  if (stats.presented_frames == 0) stats.first_frame = frame;
  if (++stats.presented_frames < 30) return;

  // GTA VC MatFX globals identified in Ghidra: 0x78A654 gates the final
  // environment-map pass in the bump+env branch, while 0x78A648 selects a
  // related bump path.  Read them from the game's module when available so a
  // high-FPS repro can be correlated with the missing third draw.
  uintptr_t image_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto read_gta_global = [image_base](uintptr_t preferred) -> uint32_t {
    const auto *address = reinterpret_cast<const uint32_t *>(
        image_base + (preferred - kGtaPreferredImageBase));
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) ||
        (mbi.Protect & PAGE_GUARD))
      return 0xFFFFFFFFu;
    return *address;
  };
  {
    std::ostringstream matfx;
    matfx << "RWMATFX-GLOBAL frame=" << frame << " g78a648=0x" << std::hex
          << read_gta_global(0x0078A648u) << " g78a654=0x"
          << read_gta_global(0x0078A654u) << std::dec << "\n";
    WriteRwDiagnosticLine(matfx.str());
  }

  std::ostringstream texture_line;
  texture_line << "RWTEX-CALLS frames=" << stats.first_frame << "-" << frame
               << " sites=[";
  bool any = false;
  for (size_t i = 0; i < kRwSetTextureCallSites.size(); ++i) {
    if (stats.texture_nonnull[i] == 0 && stats.texture_null[i] == 0) continue;
    texture_line << (any ? "," : "") << "+0x" << std::hex
                 << (kRwSetTextureCallSites[i] - kGtaPreferredImageBase)
                 << std::dec << ":tex=" << stats.texture_nonnull[i]
                 << ":null=" << stats.texture_null[i]
                 << ":input=" << stats.texture_input_nonnull[i];
    any = true;
  }
  texture_line << "] unmatched=" << stats.texture_unmatched << "\n";
  WriteRwDiagnosticLine(texture_line.str());

  std::ostringstream draw_line;
  draw_line << "RWDRAW-CALLS frames=" << stats.first_frame << "-" << frame
            << " indexedAttempt=[";
  AppendRwSiteCounts(draw_line, kRwIndexedDrawCallSites, stats.indexed_draws);
  draw_line << "] indexedEmit=[";
  AppendRwSiteCounts(draw_line, kRwIndexedDrawCallSites,
                     stats.indexed_emitted);
  draw_line << "] noIB=[";
  AppendRwSiteCounts(draw_line, kRwIndexedDrawCallSites, stats.indexed_no_ib);
  draw_line << "] zeroClamp=[";
  AppendRwSiteCounts(draw_line, kRwIndexedDrawCallSites,
                     stats.indexed_zero_clamp);
  draw_line << "] partialClamp=[";
  AppendRwSiteCounts(draw_line, kRwIndexedDrawCallSites,
                     stats.indexed_partial_clamp);
  draw_line << "] prepareFail=[";
  AppendRwSiteCounts(draw_line, kRwIndexedDrawCallSites,
                     stats.indexed_prepare_failed);
  draw_line << "] primitiveAttempt=[";
  AppendRwSiteCounts(draw_line, kRwPrimitiveDrawCallSites,
                     stats.primitive_draws);
  draw_line << "] outcomeTotals=emit:" << stats.indexed_emitted_total
            << ":noIB:" << stats.indexed_no_ib_total
            << ":zero:" << stats.indexed_zero_clamp_total
            << ":partial:" << stats.indexed_partial_clamp_total
            << ":prepareFail:" << stats.indexed_prepare_failed_total
            << " unmatchedIndexed=" << stats.indexed_unmatched
            << " unmatchedPrimitive=" << stats.primitive_unmatched << "\n";
  WriteRwDiagnosticLine(draw_line.str());

  stats = {};
}

}  // namespace
#endif  // DX8TO12_ENABLE_MINDEBUG

// Diagnostic sink other translation units can reach in release-mindebug
// builds, where ordinary LOG(...) compiles to nothing (pch.h /
// DX8TO12_DISABLE_LOGGING) and the F9 UI dump is permanently off. Deliberately
// shares log.txt with the draw-outcome diagnostics above so a finding can be
// correlated against them by frame number.
void WriteMindebugDiagnosticLine(const std::string &line) {
#ifdef DX8TO12_ENABLE_MINDEBUG
  WriteRwDiagnosticLine(line);
#else
  (void)line;
#endif
}

// static_assert(sizeof(void *) == 4, "Does not support 64-bit.");

// DXGI_SWAP_EFFECT_FLIP_DISCARD swap chains only accept a handful of formats,
// none of which lack an alpha channel. D3DFMT_X8R8G8B8 -- by far the most
// common DX8 backbuffer format -- maps to DXGI_FORMAT_B8G8R8X8_UNORM via
// DXGIFromD3DFormat, which CreateSwapChainForHwnd/ResizeBuffers reject
// outright (DXGI_ERROR_INVALID_CALL), aborting device creation. Swap in the
// alpha variant for the swap chain itself; games never read/write backbuffer
// alpha through the X8 format anyway.
// D3D8's swap effect says whether the app may rely on the back buffer still
// holding the previous frame after Present. D3DSWAPEFFECT_DISCARD explicitly
// does not; FLIP and COPY (and COPY_VSYNC) do. Hardcoding FLIP_DISCARD for
// all of them means an app that legitimately renders only the part of the
// screen that changed -- which menus commonly do -- gets whatever happened to
// be left in that buffer instead of its previous frame, showing up as
// flickering remnants of older frames.
static DXGI_SWAP_EFFECT ToDxgiSwapEffect(D3DSWAPEFFECT d3d8_swap_effect) {
  switch (d3d8_swap_effect) {
    case D3DSWAPEFFECT_FLIP:
    case D3DSWAPEFFECT_COPY:
    case D3DSWAPEFFECT_COPY_VSYNC:
      // Preserves back buffer contents across Present, unlike FLIP_DISCARD.
      return DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    case D3DSWAPEFFECT_DISCARD:
    default:
      return DXGI_SWAP_EFFECT_FLIP_DISCARD;
  }
}

static DXGI_FORMAT ToFlipModelSwapChainFormat(DXGI_FORMAT format) {
  if (format == DXGI_FORMAT_B8G8R8X8_UNORM) return DXGI_FORMAT_B8G8R8A8_UNORM;
  return format;
}

// Backs IDirect3DDevice8::CreateAdditionalSwapChain. Only a thin wrapper: its
// backbuffers are plain GpuTextures/GpuSurfaces, so the app renders to them
// through the normal Device::SetRenderTarget/Draw*/etc. path -- this class
// only needs to own the second DXGI swap chain and know how to flush +
// present it, reusing Device's existing command-submission machinery rather
// than duplicating it.
class AdditionalSwapChain : public IDirect3DSwapChain8, public RefCounted {
 public:
  AdditionalSwapChain(Device *device, ComPtr<IDXGISwapChain3> swap_chain,
                      std::vector<ComPtr<GpuTexture>> back_buffers)
      : device_(device),
        swap_chain_(std::move(swap_chain)),
        back_buffers_(std::move(back_buffers)),
        current_index_(swap_chain_->GetCurrentBackBufferIndex()) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObj) override {
    if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain8) {
      *ppvObj = static_cast<IDirect3DSwapChain8 *>(this);
      AddRef();
      return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return RefCounted::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override {
    return RefCounted::Release();
  }

  HRESULT STDMETHODCALLTYPE Present(CONST RECT *pSourceRect,
                                    CONST RECT *pDestRect,
                                    HWND hDestWindowOverride,
                                    CONST RGNDATA *pDirtyRegion) override {
    // Same simplifications as Device::Present: no partial-rect presentation.
    ASSERT(pSourceRect == nullptr && pDestRect == nullptr &&
           pDirtyRegion == nullptr);
    (void)hDestWindowOverride;
    device_->TransitionTexture(back_buffers_[current_index_].get(), 0,
                               D3D12_RESOURCE_STATE_PRESENT);
    // Flush the shared command list (without presenting the *primary* swap
    // chain), then present this one specifically.
    device_->SubmitAndWait(false);
    ASSERT_HR(swap_chain_->Present(
        device_->sync_interval(),
        device_->sync_interval() == 0 && device_->tearing_supported()
            ? DXGI_PRESENT_ALLOW_TEARING
            : 0));
    current_index_ = swap_chain_->GetCurrentBackBufferIndex();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer,
                                          D3DBACKBUFFER_TYPE Type,
                                          IDirect3DSurface8 **ppBackBuffer)
      override {
    ASSERT(Type == D3DBACKBUFFER_TYPE_MONO);
    if (BackBuffer >= back_buffers_.size()) return D3DERR_INVALIDCALL;
    *ppBackBuffer = new GpuSurface(device_, back_buffers_[BackBuffer].get(), 0);
    return S_OK;
  }

 private:
  Device *device_;
  ComPtr<IDXGISwapChain3> swap_chain_;
  std::vector<ComPtr<GpuTexture>> back_buffers_;
  UINT current_index_;
};

// Implements the standard D3D8 "query size, then fetch" pattern used by
// GetVertexShaderDeclaration/GetVertexShaderFunction/GetPixelShaderFunction:
// if pData is null, just report the required size; otherwise copy up to
// whatever size the caller already put in *pSizeOfData.
static HRESULT CopyOutTokenBuffer(const std::vector<DWORD> &tokens,
                                  void *pData, DWORD *pSizeOfData) {
  const DWORD available_bytes = safe_cast<DWORD>(tokens.size() * sizeof(DWORD));
  if (pData == nullptr) {
    *pSizeOfData = available_bytes;
    return S_OK;
  }
  const DWORD bytes_to_copy = std::min(*pSizeOfData, available_bytes);
  memcpy(pData, tokens.data(), bytes_to_copy);
  *pSizeOfData = bytes_to_copy;
  return S_OK;
}

Device::DirtyFlags &operator|=(Device::DirtyFlags &a, Device::DirtyFlags b) {
  a = static_cast<Device::DirtyFlags>(static_cast<uint32_t>(a) |
                                      static_cast<uint32_t>(b));
  return a;
}

Device::DirtyFlags &operator^=(Device::DirtyFlags &a, Device::DirtyFlags b) {
  a = static_cast<Device::DirtyFlags>(static_cast<uint32_t>(a) ^
                                      static_cast<uint32_t>(b));
  return a;
}

Device::Device(IDirect3D8 *direct3d8)
    : ref_count_(1), direct3d8_(ComWrap(direct3d8)) {
  // Set some default state for the first texture stage.
  texture_stage_states_[0].color_op = D3DTOP_MODULATE;
  texture_stage_states_[0].alpha_op = D3DTOP_SELECTARG1;
  for (size_t i = 0; i < texture_stage_states_.size(); ++i) {
    texture_stage_states_[i].texcoord_index = static_cast<DWORD>(i);
  }
  for (int i = 0; i < 256; ++i) {
    const WORD identity = static_cast<WORD>(i * 257);
    gamma_ramp_.red[i] = identity;
    gamma_ramp_.green[i] = identity;
    gamma_ramp_.blue[i] = identity;
  }
}

HRESULT STDMETHODCALLTYPE Device::QueryInterface(REFIID riid, void **ppvObj) {
  if (ppvObj == nullptr)
    return E_POINTER;
  else if (riid == IID_IDirect3DDevice8 || riid == __uuidof(IUnknown)) {
    AddRef();
    *ppvObj = static_cast<IDirect3DDevice8 *>(this);
    return S_OK;
  } else {
    // Querying for an interface this object doesn't implement is normal COM
    // usage (e.g. defensive interface probing by middleware) -- it isn't an
    // error condition worth crashing over.
    *ppvObj = nullptr;
    return E_NOINTERFACE;
  }
}

static void __stdcall DebugInfoQueueMessageCallback(
    D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id, LPCSTR pDescription, void *pContext) {
  ASSERT(pDescription);
  AixLog::Severity log_severity;
  switch (severity) {
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
      log_severity = AixLog::Severity::debug;
      break;
    case D3D12_MESSAGE_SEVERITY_INFO:
      log_severity = AixLog::Severity::info;
      break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
      log_severity = AixLog::Severity::warning;
      break;
    case D3D12_MESSAGE_SEVERITY_ERROR:
      log_severity = AixLog::Severity::error;
      break;
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
      log_severity = AixLog::Severity::fatal;
      break;
  }
  OutputDebugStringA(pDescription);
  LOG(log_severity) << pDescription << "\n";
#ifdef DX8TO12_ENABLE_MINDEBUG
  if (severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
    std::ostringstream line;
    line << "D3D12-MESSAGE severity=" << static_cast<int>(severity)
         << " id=" << static_cast<int>(id) << " " << pDescription << "\n";
    WriteRwDiagnosticLine(line.str());
  }
#endif
#ifdef DX8TO12_ENABLE_VALIDATION
  // DIAGNOSTIC: a SET_DESCRIPTOR_TABLE_INVALID error has been showing up
  // every frame with a heap/handle that never matches anything either of
  // PrepareDrawCall's two SetGraphicsRootDescriptorTable call sites log
  // (ROOTTABLE-SRV/SAMPLER), and no mod ever registers a render callback
  // (MODRENDERCALLBACK-REGISTERED never fires) -- ruling out both our own
  // binding code and the native mod-render-callback API. This callback runs
  // synchronously on the same thread that made the offending call
  // (D3D12_MESSAGE_CALLBACK_FLAG_NONE, not deferred), so a stack trace here
  // should show the real caller even if it's an injected overlay DLL.
  if (strstr(pDescription, "is different from currently set descriptor heap")) {
    static int lines = 0;
    if (lines < 50) {
      ++lines;
      void *frames[24] = {};
      USHORT count = CaptureStackBackTrace(0, 24, frames, nullptr);
      std::ostringstream dump;
      dump << "SETDESCTABLE-STACK (" << count << " frames):\n";
      for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char module_path[MAX_PATH] = {};
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(frames[i]), &module) &&
            GetModuleFileNameA(module, module_path, sizeof(module_path))) {
          const uintptr_t offset =
              reinterpret_cast<uintptr_t>(frames[i]) -
              reinterpret_cast<uintptr_t>(module);
          dump << "  #" << i << " " << frames[i] << " " << module_path
               << "+0x" << std::hex << offset << std::dec << "\n";
        } else {
          dump << "  #" << i << " " << frames[i] << " <unresolved module>\n";
        }
      }
      LOG(AixLog::Severity::error) << dump.str();
    }
  }
#endif
  // Only CORRUPTION (actual GPU/driver memory corruption -- vanishingly rare
  // and always worth stopping for) is fatal. ERROR-severity messages used to
  // abort too, which is right for catching *our own* bugs during
  // development, but wrong for a game the user is actually trying to play:
  // third-party overlays (RTSS/Afterburner-style FPS OSDs, screenshot tools)
  // hook Present/ExecuteCommandLists and can trip the validation layer with
  // false positives that have nothing to do with this codebase -- observed
  // in practice as "PSO deleted while still referenced by the command list"
  // exactly when such an overlay was active, reproducibly gone once it was
  // closed. Logging (still visible in log.txt for real bugs) without
  // aborting lets the game keep running through those instead of hard
  // crashing over someone else's hook.
  if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
    FAIL("D3D12 Error:\r\n%s", pDescription);
  }
}

bool Device::Create(HWND window, ComPtr<IDXGIFactory2> factory,
                    ComPtr<IDXGIAdapter> adapter, int adapter_index,
                    const D3DPRESENT_PARAMETERS &presentParams) {
  window_ = window;
  dxgi_factory_ = std::move(factory);

  {
    ComPtr<IDXGIFactory5> factory5;
    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(dxgi_factory_->QueryInterface(
            IID_PPV_ARGS(factory5.GetForInit()))) &&
        SUCCEEDED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
            sizeof(allow_tearing)))) {
      tearing_supported_ = allow_tearing;
    }
  }

  LOG(INFO) << "Creating device.\n";
  {
    // What the app actually asked for vs. the window it handed us. Windowed
    // is currently ignored entirely -- real D3D8 changed the display mode for
    // a fullscreen request, so the window ended up exactly covering the
    // screen; we do neither, which leaves the app sizing its window from
    // assumptions that no longer hold.
    RECT client_rect{};
    RECT window_rect{};
    GetClientRect(window, &client_rect);
    GetWindowRect(window, &window_rect);
    LOG(INFO) << "PresentParams: Windowed=" << presentParams.Windowed
              << " BackBuffer=" << std::dec << presentParams.BackBufferWidth
              << "x" << presentParams.BackBufferHeight
              << " BackBufferCount=" << presentParams.BackBufferCount
              << " hDeviceWindow=" << presentParams.hDeviceWindow
              << " | window=" << window << " client="
              << (client_rect.right - client_rect.left) << "x"
              << (client_rect.bottom - client_rect.top) << " windowRect=("
              << window_rect.left << "," << window_rect.top << ")-("
              << window_rect.right << "," << window_rect.bottom << ")"
              << " virtualScreen=" << GetSystemMetrics(SM_CXVIRTUALSCREEN)
              << "x" << GetSystemMetrics(SM_CYVIRTUALSCREEN)
              << " primary=" << GetSystemMetrics(SM_CXSCREEN) << "x"
              << GetSystemMetrics(SM_CYSCREEN) << "\n";
  }
#if defined(DX8TO12_ENABLE_VALIDATION) || \
    defined(DX8TO12_ENABLE_D3D12_DEBUG_LAYER)
  ID3D12Debug *debug_iface = nullptr;
  ASSERT_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_iface)));
  ASSERT_HR(
      debug_iface->QueryInterface(IID_PPV_ARGS(debug_interface_.GetForInit())));
  debug_iface->Release();
  // NOTE: EnableDebugLayer() is deliberately NOT called here. It has to run
  // before any D3D12 device exists (including the cached probe devices
  // GetProbeDeviceFor creates for CheckDeviceType/CheckDeviceFormat), so it
  // lives in the Direct3D8 constructor -- see the comment there.
  // debug_interface_->SetEnableSynchronizedCommandQueueValidation(TRUE);
  // GPU-based validation is much heavier than the regular debug layer (shader
  // instrumentation on every draw/copy) -- it's the likely cause of very low
  // FPS even in menus, and its validation runs asynchronously relative to the
  // CPU submission that triggered it, which can surface as a message
  // hundreds of ms after the actual call (observed: a "Command lists must be
  // successfully closed" error logged ~350ms after the last real
  // ExecuteCommandLists, with nothing logged in between) -- misleading when
  // chasing a crash via checkpoint logging, since the real cause isn't the
  // most recently logged call. EnableDebugLayer() alone (kept, no perf cost
  // even close to GBV's) already catches the vast majority of real bugs this
  // project has actually been fixed from (resource-state validation, leaked
  // descriptors, etc.) -- turn this back on only if specifically chasing a
  // GPU-side corruption/UAV-hazard bug that plain validation can't see.
  // debug_interface_->SetEnableGPUBasedValidation(TRUE);
  // debug_interface_->SetEnableAutoName(TRUE);
#endif

  adapter_ = std::move(adapter);
  adapter_index_ = adapter_index;
  ASSERT(adapter_);
  {
    // DXGI_ERROR_DEVICE_HUNG/DEVICE_RESET (0x887A0006/0x887A0007) here mean
    // the OS already has the adapter marked as needing to be reopened --
    // typically a transient leftover from another D3D12 app (e.g. RenderDoc,
    // or this same process on a previous run) closing right before this
    // call. There's nothing about *this* call that caused it, but retrying
    // after a short wait commonly clears it without needing a manual
    // graphics-driver restart, so do that a few times before giving up.
    constexpr int kMaxAttempts = 5;
    HRESULT hr = S_OK;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      hr = D3D12CreateDevice(adapter_.get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(d3d12_device_.GetForInit()));
      if (hr == S_OK) break;
      if ((hr != DXGI_ERROR_DEVICE_HUNG && hr != DXGI_ERROR_DEVICE_RESET) ||
          attempt == kMaxAttempts) {
        break;
      }
      LOG(AixLog::Severity::error)
          << "D3D12CreateDevice failed with " << std::hex << hr << std::dec
          << " (attempt " << attempt << "/" << kMaxAttempts
          << ") -- retrying shortly.\n";
      Sleep(400);
    }
    if (hr != S_OK) {
      FAIL("Failed to create device: %d", hr);
      return false;
    }
  }
  {
    DXGI_ADAPTER_DESC adapter_desc = {};
    if (SUCCEEDED(adapter_->GetDesc(&adapter_desc))) {
      char name[128] = {};
      size_t converted = 0;
      wcstombs_s(&converted, name, sizeof(name), adapter_desc.Description,
                 _TRUNCATE);
      LOG(INFO) << "D3D12 device created on adapter: " << name
                << " (VRAM "
                << (adapter_desc.DedicatedVideoMemory >> 20) << " MB, "
                << "DXGI index " << adapter_index_ << ")\n";
    }
  }
  {
    // When an adapter reports an unexpected feature tier, distinguish a
    // genuinely capable D3D12 runtime from a redirected/local replacement
    // before drawing conclusions about the GPU or the compile-time SDK.
    wchar_t runtime_path[MAX_PATH] = {};
    HMODULE runtime_module = GetModuleHandleW(L"d3d12.dll");
    if (runtime_module != nullptr) {
      GetModuleFileNameW(runtime_module, runtime_path,
                         MAX_PATH);
    }
    LARGE_INTEGER driver_version = {};
    const HRESULT driver_hr = adapter_->CheckInterfaceSupport(
        __uuidof(ID3D12Device), &driver_version);
    ComPtr<ID3D12Device5> device5;
    const HRESULT device5_hr = d3d12_device_->QueryInterface(
        IID_PPV_ARGS(device5.GetForInit()));
    char runtime_path_utf8[MAX_PATH * 3] = {};
    size_t converted = 0;
    wcstombs_s(&converted, runtime_path_utf8, sizeof(runtime_path_utf8),
               runtime_path, _TRUNCATE);
    LOG(INFO) << "D3D12 runtime=" << runtime_path_utf8
              << " ID3D12Device5=" << std::hex << device5_hr
              << " adapter-driver=" << driver_hr << ":0x"
              << driver_version.QuadPart << std::dec << "\n";
  }
  // TODO: Pass in adapter output.
  // ASSERT_HR(adapter_->EnumOutputs(0, adapter_output_.GetForInit()));

// Create info queue.
#if defined(DX8TO12_ENABLE_VALIDATION) || \
    defined(DX8TO12_ENABLE_D3D12_DEBUG_LAYER)
  if (SUCCEEDED(d3d12_device_->QueryInterface(
          IID_PPV_ARGS(info_queue_.GetForInit()))))
    info_queue_->RegisterMessageCallback(DebugInfoQueueMessageCallback,
                                         D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                         nullptr, &info_queue_cookie_);
#endif

  // D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12;
  // ASSERT_HR(d3d12_device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,
  //                                              &options12,
  //                                              sizeof(options12)));
  // ASSERT(options12.EnhancedBarriersSupported);

  {
    // Probed once here (not lazily) so raytracing_supported() is answerable
    // immediately after device creation -- both LightingMode's config
    // validation (config.cpp) and a mod's Dx8to12_GetRaytracingSupported call
    // need a real answer before the first frame, not "unknown until the
    // first raytracing pass tries to run".
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    const HRESULT options5_hr = d3d12_device_->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    LOG(INFO) << "CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5) returned "
              << std::hex << options5_hr << std::dec
              << ", RaytracingTier=" << options5.RaytracingTier << "\n";
    if (SUCCEEDED(options5_hr)) {
      raytracing_supported_ =
          options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
    }
    LOG(INFO) << "Raytracing tier: " << options5.RaytracingTier
              << (raytracing_supported_ ? " (supported)\n"
                                        : " (not supported)\n");
  }

  ASSERT_HR(Init(presentParams));
  // H1: an x64 companion can expose native NVIDIA DXR even though this x86
  // process reports Tier 0. Start it only after Init: H2 needs this side's
  // command queue/list to create and signal shared GPU resources.
  if (!raytracing_supported_) {
    rt_helper_client_ = std::make_unique<RtHelperClient>(this);
  }
  g_current_device = this;
  LOG(INFO) << "Create: done, returning to Direct3D8::CreateDevice()\n";
  return true;
}

namespace {
// Present() previously always used SyncInterval=1 (vsync forced on)
// regardless of what the app actually requested, leaving the GPU idle
// waiting for vblank between frames -- observed in practice as low
// GPU/CPU utilization alongside a mediocre framerate. Map the app's real
// request instead.
UINT SyncIntervalFromD3DPresentInterval(DWORD present_interval) {
  switch (present_interval) {
    case D3DPRESENT_INTERVAL_IMMEDIATE:
      return 0;
    case D3DPRESENT_INTERVAL_TWO:
      return 2;
    case D3DPRESENT_INTERVAL_THREE:
      return 3;
    case D3DPRESENT_INTERVAL_FOUR:
      return 4;
    case D3DPRESENT_INTERVAL_DEFAULT:
    case D3DPRESENT_INTERVAL_ONE:
    default:
      return 1;
  }
}

// Makes `window` exactly cover the monitor it's on. Only used for a
// fullscreen device request -- see kResizeWindowForFullscreen for why this is
// needed at all.
void FitWindowToItsMonitor(HWND window) {
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{.cbSize = sizeof(MONITORINFO)};
  if (!GetMonitorInfoW(monitor, &monitor_info)) return;

  const RECT &bounds = monitor_info.rcMonitor;
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;

  RECT current{};
  if (GetWindowRect(window, &current) && current.left == bounds.left &&
      current.top == bounds.top && current.right == bounds.right &&
      current.bottom == bounds.bottom) {
    return;  // Already exactly right; don't disturb the app's window.
  }

  LOG(INFO) << "Fullscreen requested: resizing window to its monitor, "
            << std::dec << width << "x" << height << " at (" << bounds.left
            << "," << bounds.top << ").\n";
  // SWP_NOSENDCHANGING keeps Windows from asking the app's WndProc to vet the
  // new position first; it still gets WM_WINDOWPOSCHANGED/WM_SIZE afterwards.
  SetWindowPos(window, nullptr, bounds.left, bounds.top, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}
}  // namespace

HRESULT Device::Init(const D3DPRESENT_PARAMETERS &presentParams) {
  if (kResizeWindowForFullscreen && !presentParams.Windowed && window_) {
    FitWindowToItsMonitor(window_);
  }
  fence_values_ = {};
  next_fence_ = 1;
  sync_interval_ = SyncIntervalFromD3DPresentInterval(
      presentParams.FullScreen_PresentationInterval);

  srv_heap_ = DescriptorPoolHeap(
      d3d12_device_.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxNumSrvs);
  rtv_heap_ = DescriptorPoolHeap(d3d12_device_.get(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxNumRtvs);
  dsv_heap_ = DescriptorPoolHeap(d3d12_device_.get(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kMaxNumRtvs);
  sampler_heap_ =
      DescriptorPoolHeap(d3d12_device_.get(),
                         D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kMaxSamplerStates);

  dynamic_ring_buffer_ = std::make_unique<DynamicRingBuffer>(
      d3d12_device_.get(), kDynamicRingBufferSize);

  dynamic_ring_buffer_->SetCurrentFrame(CurrentFrame());

#ifdef DX8TO12_USE_ALLOCATOR
  {
    D3D12MA::ALLOCATOR_DESC desc{.pDevice = d3d12_device_.get(),
                                 .PreferredBlockSize = 2 * 1024 * 1024,
                                 .pAdapter = adapter_.get()};
    ASSERT_HR(D3D12MA::CreateAllocator(&desc, allocator_.GetForInit()));
  }
#endif

  if (presentParams.EnableAutoDepthStencil) {
    LOG(INFO) << "Auto depth stencil.\n";
    D3DFORMAT depth_format = presentParams.AutoDepthStencilFormat;
    if (depth_format == D3DFMT_UNKNOWN) depth_format = D3DFMT_D32;
    ASSERT(depth_format == D3DFMT_D16 || depth_format == D3DFMT_D32 ||
           depth_format == D3DFMT_D24S8 || depth_format == D3DFMT_D24X8 ||
           depth_format == D3DFMT_D24X4S4);
    depth_stencil_tex_ = ComOwn(static_cast<GpuTexture *>(BaseTexture::Create(
        this, TextureKind::Texture2d, presentParams.BackBufferWidth,
        presentParams.BackBufferHeight, 1, 1, D3DUSAGE_DEPTHSTENCIL,
        depth_format, D3DPOOL_DEFAULT)));
    // Per the D3D8 spec, D3DRS_ZENABLE's default value is D3DZB_TRUE when
    // EnableAutoDepthStencil is set (D3DZB_FALSE otherwise, which is
    // RenderState's default member value already). A game that doesn't
    // explicitly SetRenderState(D3DRS_ZENABLE, ...) -- reasonable, since it
    // asked for an auto depth-stencil buffer specifically to get this
    // default -- would otherwise silently render with depth testing off:
    // no crash, just badly wrong draw order/z-fighting.
    render_state_.zbuffer_type = D3DZB_TRUE;
  }

  viewport_.Width = static_cast<float>(presentParams.BackBufferWidth);
  viewport_.Height = static_cast<float>(presentParams.BackBufferHeight);

  caps_ = GetDefaultCaps(static_cast<UINT>(adapter_index_));

  // Create command queue.
  D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
      .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
      .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
      .NodeMask = 0};
  ASSERT_HR(d3d12_device_->CreateCommandQueue(
      &cmd_queue_desc, IID_PPV_ARGS(cmd_queue_.GetForInit())));
  for (auto &allocator : cmd_allocators_) {
    ASSERT_HR(d3d12_device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetForInit())));
  }
  ASSERT_HR(d3d12_device_->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocators_[0].get(), nullptr,
      IID_PPV_ARGS(cmd_list_.GetForInit())));
  dirty_flags_ ^= DIRTY_FLAG_CMD_LIST_CLOSED;
  ASSERT_HR(d3d12_device_->CreateFence(
      0, D3D12_FENCE_FLAG_NONE,
      IID_PPV_ARGS(cmd_list_done_fence_.GetForInit())));
  cmd_list_done_event_handle_ =
      CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  ASSERT(cmd_list_done_event_handle_ != INVALID_HANDLE_VALUE);

  // Create the swap chain.
  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{
      .Width = presentParams.BackBufferWidth,
      .Height = presentParams.BackBufferHeight,
      .Format = ToFlipModelSwapChainFormat(
          DXGIFromD3DFormat(presentParams.BackBufferFormat)),
      .SampleDesc = {.Count = 1, .Quality = 0},
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = kNumBackBuffers,
      // Stretch, not DXGI_SCALING_NONE: the back buffer is sized to whatever
      // resolution the game asked for, which routinely differs from the size
      // of the window it's presenting into. NONE means "don't scale" -- DXGI
      // puts the buffer in the window's top-left corner at 1:1 and leaves the
      // rest blank, which is what made a game running at, say, 1920x1080 on a
      // 2560x1440 window render into just part of the screen. Real D3D8
      // changed the display mode for fullscreen instead, so the picture
      // always filled the screen; STRETCH is the flip-model equivalent.
      .Scaling = DXGI_SCALING_STRETCH,
      .SwapEffect = ToDxgiSwapEffect(presentParams.SwapEffect),
      .Flags = tearing_supported_
                   ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
                   : 0u,
  };
  LOG(INFO) << "Swap chain: app requested D3DSWAPEFFECT " << std::dec
            << presentParams.SwapEffect << " -> DXGI swap effect "
            << swap_chain_desc.SwapEffect << ".\n";
  // Don't crash if creating the swap chain fails. This might happen during
  // device reset.
  ComPtr<IDXGISwapChain1> swap_chain1;
  HR_OR_RETURN(dxgi_factory_->CreateSwapChainForHwnd(
      cmd_queue_.get(), window_, &swap_chain_desc, nullptr, nullptr,
      swap_chain1.GetForInit()));
  ASSERT_HR(swap_chain1->QueryInterface(swap_chain_.GetForInit()));

  // Without this, DXGI keeps monitoring `window_` itself -- intercepting
  // Alt+Enter and reacting to window state changes -- which real D3D8 never
  // did. A game written against real D3D8 doesn't expect DXGI to be
  // synchronously interacting with its window at all, and unexpected
  // reentrancy into the game's own WndProc during our swap chain setup is a
  // plausible source of otherwise-unexplained corruption/crashes shortly
  // afterward. Opt out of all of DXGI's automatic window handling.
  ASSERT_HR(dxgi_factory_->MakeWindowAssociation(
      window_, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER |
                   DXGI_MWA_NO_PRINT_SCREEN));

  current_back_buffer_ = swap_chain_->GetCurrentBackBufferIndex();

  // Create the back buffer.
  ASSERT(presentParams.BackBufferCount <= 1);
  ASSERT(back_buffers_.empty());
  for (uint32_t i = 0; i < swap_chain_desc.BufferCount; ++i) {
    ComPtr<ID3D12Resource> back_buffer_resource;
    ASSERT_HR(swap_chain_->GetBuffer(
        i, IID_PPV_ARGS(back_buffer_resource.GetForInit())));
    GpuTexture *back_buffer =
        GpuTexture::InitFromResource(this, back_buffer_resource);
    back_buffers_.push_back(ComOwn(back_buffer));
  }

  D3DPRESENT_PARAMETERS params = presentParams;
  ASSERT_HR(Reset(&params));

  InitRootSignatures();

  // Real D3D8 devices come out of CreateDevice with a default vertex format
  // already active (D3DFVF_XYZ -- untransformed position only), not with no
  // format set at all. Confirmed by comparing against d3d8to9 (a known-good
  // D3D8-on-D3D9 wrapper), which explicitly calls SetFVF(D3DFVF_XYZ) right
  // after constructing its device wrapper, before returning it to the app.
  // Without this, bound_vertex_shader_ defaults to 0 with no corresponding
  // entry in vertex_shaders_ (SetVertexShader was never actually called),
  // which is a real, observable difference from every other implementation
  // a game might have been tested against -- some games rely on a default
  // format being active before they ever call SetVertexShader/SetFVF
  // themselves.
  ASSERT_HR(SetVertexShader(D3DFVF_XYZ));

  LOG(INFO) << "Init: done, returning to Create()\n";
  return S_OK;
}

Device::~Device() {
  WaitForFrame(next_fence_ - 1);
  if (g_current_device == this) g_current_device = nullptr;
}

DXGI_FORMAT Device::backbuffer_format() const {
  return back_buffers_.at(current_back_buffer_).get()->resource_desc().Format;
}

bool Device::raytracing_supported() const {
  return raytracing_supported_ ||
         (rt_helper_client_ && rt_helper_client_->ready());
}

void* Device::rt_shadow_output_resource() const {
  return rt_helper_client_ ? rt_helper_client_->shadow_output_resource() : nullptr;
}

void* Device::rt_shadow_done_fence() const {
  return rt_helper_client_ ? rt_helper_client_->shadow_done_fence() : nullptr;
}

uint64_t Device::rt_shadow_done_fence_value() const {
  return rt_helper_client_ ? rt_helper_client_->shadow_done_fence_value() : 0;
}

uint32_t Device::rt_shadow_output_width() const {
  return rt_helper_client_ ? rt_helper_client_->shadow_output_width() : 0;
}

uint32_t Device::rt_shadow_output_height() const {
  return rt_helper_client_ ? rt_helper_client_->shadow_output_height() : 0;
}

uint32_t Device::rt_shadow_output_format() const {
  return rt_helper_client_ ? rt_helper_client_->shadow_output_format() : DXGI_FORMAT_UNKNOWN;
}

#ifdef DX8TO12_TEMPORAL_JITTER
namespace {
// Radical inverse in `base` -- the Halton sequence. Low-discrepancy rather
// than random: a temporal upscaler wants the samples inside a pixel to cover
// it evenly over a short window, which random offsets only do on average and
// often clump in practice.
float HaltonSample(uint32_t index, uint32_t base) {
  float result = 0.f;
  float fraction = 1.f;
  while (index > 0) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}
}  // namespace

// Amplitude of the offset in pixels. 1.0 (i.e. +-0.5 px) is the correct value
// for an upscaler. Temporarily raise this to something absurd like 16 to make
// the jitter unmistakable on screen -- the only way to tell "applied but too
// subtle to notice" apart from "not applied at all", which is not otherwise
// observable from inside the game.
static constexpr float kJitterPixelScale = 1.0f;

void Device::AdvanceTemporalJitter() {
  // Runtime switch, not just a build flag -- the whole point of the INI/mod
  // API surface is that a player can turn this on without a special build.
  // Zeroing the offset makes the injection site in PrepareDrawCall a no-op
  // (it adds 0), so there is no second place that has to know about this.
  if (!GetConfig().temporal_jitter) {
    jitter_pixels_ = {};
    return;
  }
  // Halton(2,3), the sequence Streamline's own samples use. Index is 1-based:
  // Halton(0) is 0 for every base, i.e. a wasted frame with no offset.
  // The 16-entry loop is the usual phase count for DLSS-class upscalers.
  ++jitter_index_;
  const uint32_t index = (jitter_index_ % 16u) + 1u;
  jitter_pixels_.x = (HaltonSample(index, 2) - 0.5f) * kJitterPixelScale;
  jitter_pixels_.y = (HaltonSample(index, 3) - 0.5f) * kJitterPixelScale;
}
#endif  // DX8TO12_TEMPORAL_JITTER

#ifdef DX8TO12_SCENE_TARGET
namespace {
// Rendering the scene offscreen and copying it back costs a full-screen copy
// every frame, so only do it when something downstream actually consumes the
// offscreen image: a temporal upscaler, or the debug view that draws onto it.
// Motion vectors alone do not need it -- they read the depth buffer.
bool SceneTargetWanted() {
  const Config &config = GetConfig();
  return config.temporal_aa != 0 || config.motion_vector_debug;
}
}  // namespace

void Device::ResolveScenePass() {
  // Idempotent: the scene pass ends exactly once per frame, at whichever
  // point first needs the backbuffer to actually hold the frame.
  if (!scene_pass_active_) return;
  scene_pass_active_ = false;
  if (!scene_color_tex_) return;

  GpuTexture *backbuffer = back_buffers_.at(current_back_buffer_).Get();
  // CopyResource needs identical dimensions. They differ whenever the scene
  // is being rendered smaller than the output, in which case the upscaler --
  // not this copy -- is what puts an image on the backbuffer, and there is
  // nothing sensible to do here. Presenting the previous backbuffer for a
  // frame beats a validation error and a corrupt one.
  if (backbuffer->resource_desc().Width != scene_color_tex_->resource_desc().Width ||
      backbuffer->resource_desc().Height != scene_color_tex_->resource_desc().Height) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      LOG_ERROR() << "ResolveScenePass: scene is "
                  << scene_color_tex_->resource_desc().Width << "x"
                  << scene_color_tex_->resource_desc().Height
                  << " but the backbuffer is "
                  << backbuffer->resource_desc().Width << "x"
                  << backbuffer->resource_desc().Height
                  << "; clearing instead of copying (the upscaler owns this "
                     "frame's output).\n";
    }
    // Cleared rather than left alone. A backbuffer nothing wrote holds
    // whatever the swap chain last had there, which shows up as torn strips
    // of colour on black -- exactly what a loading screen or a menu frame
    // that never reached the upscaler looks like.
    TransitionTexture(backbuffer, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    cmd_list_->ClearRenderTargetView(backbuffer->rtv_handle(), black, 0,
                                     nullptr);
    dirty_flags_ |= DIRTY_FLAG_OM;
    dirty_flags_ |= DIRTY_FLAG_PSO;
    return;
  }
  TransitionTexture(scene_color_tex_.Get(), 0,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
  TransitionTexture(backbuffer, 0, D3D12_RESOURCE_STATE_COPY_DEST);
  // Same format, same dimensions, one mip, no MSAA -- CopyResource is exact,
  // so at 1:1 this stage needs no shader and no fullscreen pass.
  cmd_list_->CopyResource(backbuffer->resource(), scene_color_tex_->resource());
  // The backbuffer is now in COPY_DEST with no RTV bound, and
  // CurrentColorTarget() has just changed what it returns. Force the next
  // BeginScene to rebind and re-transition.
  dirty_flags_ |= DIRTY_FLAG_OM;
  dirty_flags_ |= DIRTY_FLAG_PSO;
}
#endif

void Device::FlushScenePassForBackbufferRead() {
#ifdef DX8TO12_SCENE_TARGET
  // A game reading the backbuffer mid-frame (lock, or CopyRects from it)
  // expects it to contain everything drawn so far. Ending the scene pass
  // early costs this frame its offscreen scene -- correctness over the
  // upscaler, which can simply skip a frame the game chose to read back.
  ResolveScenePass();
#endif
}

#ifdef DX8TO12_MOTION_VECTORS
void Device::InitMotionVectorPass() {
  // A dedicated root signature: one CBV for the matrices, one SRV table for
  // the depth buffer. No sampler -- the shader Load()s the depth texel
  // directly, which is what you want anyway (filtering across a depth
  // discontinuity would blend two unrelated surfaces into one bogus depth).
  D3D12_DESCRIPTOR_RANGE depth_range{
      .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
      .NumDescriptors = 1,
      .BaseShaderRegister = 0,
      .RegisterSpace = 0,
      .OffsetInDescriptorsFromTableStart = 0};
  D3D12_ROOT_PARAMETER params[2] = {
      {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
       .Descriptor = {.ShaderRegister = 0},
       .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL},
      {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
       .DescriptorTable = {.NumDescriptorRanges = 1,
                           .pDescriptorRanges = &depth_range},
       .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL},
  };
  D3D12_ROOT_SIGNATURE_DESC sig_desc{
      .NumParameters = 2,
      .pParameters = params,
      .NumStaticSamplers = 0,
      .pStaticSamplers = nullptr,
      // No ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT: the fullscreen triangle is
      // generated from SV_VertexID, so there is no vertex buffer at all.
      .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> sig_blob, sig_error;
  HRESULT hr = D3D12SerializeRootSignature(
      &sig_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, sig_blob.GetForInit(),
      sig_error.GetForInit());
  if (FAILED(hr)) {
    FAIL("Motion vector root signature failed:\r\n%s",
         sig_error ? static_cast<const char *>(sig_error->GetBufferPointer())
                   : "(no message)");
  }
  ASSERT_HR(d3d12_device_->CreateRootSignature(
      0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
      IID_PPV_ARGS(mvec_root_sig_.GetForInit())));

  cmrc::embedded_filesystem fs = cmrc::Dx8to12_shaders::get_filesystem();
  cmrc::file source = fs.open("motion_vectors.hlsl");
  const std::string code(source.begin(), source.end());
  auto compile = [&code](const char *entry, const char *target) {
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT chr = D3DCompile(
        code.data(), code.size(), "motion_vectors.hlsl", nullptr, nullptr,
        entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
        blob.GetForInit(), errors.GetForInit());
    if (FAILED(chr)) {
      FAIL("motion_vectors.hlsl (%s) failed to compile:\r\n%s", entry,
           errors ? static_cast<const char *>(errors->GetBufferPointer())
                  : "(no message)");
    }
    return blob;
  };
  ComPtr<ID3DBlob> vs_blob = compile("VSMain", "vs_5_0");
  ComPtr<ID3DBlob> ps_blob = compile("PSMain", "ps_5_0");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{
      .pRootSignature = mvec_root_sig_.get(),
      .VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()},
      .PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()},
      .BlendState = {.RenderTarget = {{.RenderTargetWriteMask =
                                           D3D12_COLOR_WRITE_ENABLE_ALL}}},
      .SampleMask = UINT_MAX,
      .RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID,
                          .CullMode = D3D12_CULL_MODE_NONE,
                          // The fullscreen triangle deliberately extends past
                          // the near plane's corners; clipping it would cut
                          // the covered area.
                          .DepthClipEnable = FALSE},
      .DepthStencilState = {.DepthEnable = FALSE, .StencilEnable = FALSE},
      .InputLayout = {nullptr, 0},
      .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
      // Two targets: motion vectors, and the depth converted to a plain
      // colour format for the cross-process upscaler (see the comment on
      // MotionVectorTargets in motion_vectors.hlsl).
      .NumRenderTargets = 2,
      .RTVFormats = {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT},
      .DSVFormat = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1, .Quality = 0}};
  ASSERT_HR(d3d12_device_->CreateGraphicsPipelineState(
      &pso_desc, IID_PPV_ARGS(mvec_pso_.GetForInit())));

#ifdef DX8TO12_MOTION_VECTORS_DEBUG
  // Identical inputs, drawn onto the scene target instead. A second PSO
  // rather than a second pass over the motion buffer: the debug shader
  // recomputes the vector from the same depth and matrices, so it verifies
  // exactly the math PSMain runs rather than a copy of its output.
  ComPtr<ID3DBlob> debug_ps_blob = compile("PSDebug", "ps_5_0");
  pso_desc.PS = {debug_ps_blob->GetBufferPointer(),
                 debug_ps_blob->GetBufferSize()};
  // PSDebug writes colour only, unlike PSMain's two targets.
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
  // The scene target is what the debug view draws onto, so take the format
  // from it rather than recomputing the swap chain's -- this is also why
  // InitMotionVectorPass runs from Reset, after the target exists.
  ASSERT(scene_color_tex_);
  pso_desc.RTVFormats[0] = scene_color_tex_->resource_desc().Format;
  ASSERT_HR(d3d12_device_->CreateGraphicsPipelineState(
      &pso_desc, IID_PPV_ARGS(mvec_debug_pso_.GetForInit())));
#endif
  LOG(INFO) << "InitMotionVectorPass: done\n";
}

void Device::CaptureFrameCamera() {
  // First draw of the frame wins. Vice City renders the world first and the
  // HUD last, so reading transforms_ at Present time would hand the motion
  // vector pass whatever matrix the HUD left behind instead of the camera.
#ifdef DX8TO12_ENABLE_VALIDATION
  // "First draw of the frame is the world camera" is an assumption, and a
  // static camera producing non-zero motion says it is wrong. Track the last
  // draw's matrix too: if first and last differ, the frame contains more than
  // one camera and picking either end of it is arbitrary. Validation-only --
  // GetViewProjMatrix does two hash lookups, which has no business running on
  // every draw of a release build.
  {
    float last[16];
    if (GetViewProjMatrix(last)) {
      memcpy(&frame_view_proj_last_, last, sizeof(frame_view_proj_last_));
      ++draws_seen_this_frame_;
    }
  }
#endif
  if (frame_view_proj_captured_) return;
  float matrix[16];
  if (!GetViewProjMatrix(matrix)) return;
  memcpy(&frame_view_proj_, matrix, sizeof(frame_view_proj_));
  // The view and projection separately too: sl::Constants wants the
  // projection on its own, and the camera's position and axes come from the
  // view matrix. Neither survives the product.
  auto transform_or_identity = [this](D3DTRANSFORMSTATETYPE state) {
    DirectX::SimpleMath::Matrix result;
    const auto it = transforms_.find(state);
    if (it != transforms_.end()) memcpy(&result, &it->second, sizeof(result));
    return result;
  };
  frame_view_ = transform_or_identity(D3DTS_VIEW);
  frame_proj_ = transform_or_identity(D3DTS_PROJECTION);
  frame_view_proj_captured_ = true;
}

namespace {
// Largest absolute element-wise difference between two matrices -- one number
// that says "these are the same camera" or "these are not".
float MaxMatrixDelta(const DirectX::SimpleMath::Matrix &a,
                     const DirectX::SimpleMath::Matrix &b) {
  const float *pa = reinterpret_cast<const float *>(&a);
  const float *pb = reinterpret_cast<const float *>(&b);
  float worst = 0.f;
  for (int i = 0; i < 16; ++i) worst = std::max(worst, std::abs(pa[i] - pb[i]));
  return worst;
}
}  // namespace

void Device::RecordMotionVectorPass() {
  if (!GetConfig().motion_vectors) return;
#ifdef DX8TO12_SCENE_TARGET
  // Only meaningful while the scene pass is open, and only once per frame:
  // it advances prev_view_proj_, so running it twice would compare a frame
  // against itself. The scene pass now ends at the first 2D draw (see
  // EndScenePassIfDrawIsUi), which is where this normally runs from; the
  // call at Present is then a no-op.
  if (!scene_pass_active_) return;
#endif
  if (!motion_vector_tex_ || !depth_copy_tex_ || !mvec_pso_ ||
      !frame_view_proj_captured_) {
    return;
  }
  // The depth the scene was actually rendered into, which is not the game's
  // own once the scene renders at a reduced resolution. Reading the game's
  // there means reading a buffer nothing wrote this frame -- which produces
  // motion vectors for geometry that never moved, and so ghosting on static
  // scenery.
  GpuTexture *scene_depth = CurrentDepthTarget();
  if (!scene_depth) return;
  // Nothing to compare against on the first frame after startup or a Reset.
  DirectX::SimpleMath::Matrix inv_view_proj;
  frame_view_proj_.Invert(inv_view_proj);
  // SimpleMath's Invert reports nothing: on a singular matrix XMMatrixInverse
  // fills the result with infinities. Inspecting the output is the only
  // signal available, and it catches a NaN camera at the same time.
  bool invertible = true;
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(reinterpret_cast<const float *>(&inv_view_proj)[i])) {
      invertible = false;
      break;
    }
  }
  if (!has_prev_view_proj_ || !invertible) {
    prev_view_proj_ = frame_view_proj_;
    has_prev_view_proj_ = invertible;
    return;
  }

  const uint32_t width =
      static_cast<uint32_t>(motion_vector_tex_->resource_desc().Width);
  const uint32_t height = motion_vector_tex_->resource_desc().Height;

  struct MotionVectorCBuffer {
    float inv_view_proj[16];
    float prev_view_proj[16];
    float render_size[2];
    float debug_scale;
    float pad;
  };
  // The upload ring is shared with texture copies, which leave it in a copy
  // state; a CBV read needs it back in the draw-readable one.
  TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                              D3D12_RESOURCE_STATE_INDEX_BUFFER);
  DynamicRingBuffer::Allocation alloc =
      dynamic_ring_buffer_->Allocate(sizeof(MotionVectorCBuffer));
  MotionVectorCBuffer *cbuffer = reinterpret_cast<MotionVectorCBuffer *>(
      dynamic_ring_buffer_->GetCpuPtrFor(alloc));
  memcpy(cbuffer->inv_view_proj, &inv_view_proj,
         sizeof(cbuffer->inv_view_proj));
  memcpy(cbuffer->prev_view_proj, &prev_view_proj_,
         sizeof(cbuffer->prev_view_proj));
  cbuffer->render_size[0] = static_cast<float>(width);
  cbuffer->render_size[1] = static_cast<float>(height);
  // Motion of this many pixels saturates the debug colour. 16 turned out to
  // saturate over the whole screen on any real camera movement, which hides
  // the shape of the field and, worse, hides its sign -- the one property
  // that is invisible here but breaks DLAA later.
  cbuffer->debug_scale = 64.f;
  cbuffer->pad = 0.f;

  // Detach the writable DSV *before* the depth resource becomes an SRV.
  // Transitioning a resource that is still bound in OM as a writable depth
  // target is invalid D3D12 and removes the device on release drivers -- the
  // same hazard already documented at the mod-callback site below.
  TransitionTexture(motion_vector_tex_.Get(), 0,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
  TransitionTexture(depth_copy_tex_.Get(), 0,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
  // FALSE, not TRUE: the two RTVs come from separate allocations in the heap
  // and are not a contiguous range.
  const D3D12_CPU_DESCRIPTOR_HANDLE mvec_rtvs[2] = {
      motion_vector_tex_->rtv_handle(), depth_copy_tex_->rtv_handle()};
  cmd_list_->OMSetRenderTargets(2, mvec_rtvs, FALSE, nullptr);
  TransitionTexture(scene_depth, 0,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  const D3D12_VIEWPORT viewport{.TopLeftX = 0.f,
                                .TopLeftY = 0.f,
                                .Width = static_cast<float>(width),
                                .Height = static_cast<float>(height),
                                .MinDepth = 0.f,
                                .MaxDepth = 1.f};
  const D3D12_RECT scissor{.left = 0,
                           .top = 0,
                           .right = static_cast<LONG>(width),
                           .bottom = static_cast<LONG>(height)};
  cmd_list_->RSSetViewports(1, &viewport);
  cmd_list_->RSSetScissorRects(1, &scissor);
  cmd_list_->SetGraphicsRootSignature(mvec_root_sig_.get());
  cmd_list_->SetPipelineState(mvec_pso_.get());
  cmd_list_->SetGraphicsRootConstantBufferView(
      0, dynamic_ring_buffer_->GetGpuPtrFor(alloc));
  cmd_list_->SetGraphicsRootDescriptorTable(
      1, srv_heap_.GetGPUHandleFor(scene_depth->srv_handle()));
  cmd_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd_list_->DrawInstanced(3, 1, 0, 0);

#ifdef DX8TO12_MOTION_VECTORS_DEBUG
  if (GetConfig().motion_vector_debug && scene_color_tex_ && mvec_debug_pso_) {
    TransitionTexture(scene_color_tex_.Get(), 0,
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv = scene_color_tex_->rtv_handle();
    cmd_list_->OMSetRenderTargets(1, &scene_rtv, TRUE, nullptr);
    cmd_list_->SetPipelineState(mvec_debug_pso_.get());
    // Right half only. Covering the whole screen makes the menu unusable --
    // there is nothing left to click on -- and a split view is the better
    // diagnostic anyway: the scene and its motion field are visible at the
    // same time, so a wrong vector can be matched against what it belongs to.
    const D3D12_RECT debug_scissor{.left = static_cast<LONG>(width / 2),
                                   .top = 0,
                                   .right = static_cast<LONG>(width),
                                   .bottom = static_cast<LONG>(height)};
    cmd_list_->RSSetScissorRects(1, &debug_scissor);
    cmd_list_->DrawInstanced(3, 1, 0, 0);
    // BeginScene restores the game's own scissor, but do not rely on a call
    // that only happens because something else set DIRTY_FLAG_OM.
    cmd_list_->RSSetScissorRects(1, &scissor);
  }
#endif

  TransitionTexture(scene_depth, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  MarkResourceAsUsed(InternalPtr(motion_vector_tex_.Get()));
  MarkResourceAsUsed(InternalPtr(depth_copy_tex_.Get()));

  // This pass bound its own root signature, PSO, topology and render target.
  // Every cache the normal draw path keeps about those is now stale.
  root_sig_bound_ = false;
  last_set_pso_ = nullptr;
  last_prim_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  dirty_texture_stage_mask_ = 0xFF;
  dirty_sampler_stage_mask_ = 0xFF;
  dirty_flags_ |= DIRTY_FLAG_OM;
  dirty_flags_ |= DIRTY_FLAG_PSO;

#ifdef DX8TO12_ENABLE_VALIDATION
  {
    static int mvec_log_frames = 0;
    if ((mvec_log_frames++ % 30) == 0) {
      const float *cur = reinterpret_cast<const float *>(&frame_view_proj_);
      LOG(AixLog::Severity::error)
          << "MVEC frame=" << next_fence_ << " draws=" << draws_seen_this_frame_
          // How much this frame's camera moved since the last one. A still
          // camera must make this ~0; anything else is the bug.
          << " deltaPrev=" << MaxMatrixDelta(frame_view_proj_, prev_view_proj_)
          // How much the camera changed *within* this frame, first draw to
          // last. Non-zero means the frame has several cameras and "first
          // draw wins" is picking one arbitrarily.
          << " deltaInFrame="
          << MaxMatrixDelta(frame_view_proj_, frame_view_proj_last_)
          << " vp=[" << cur[0] << "," << cur[5] << "," << cur[10] << ","
          << cur[12] << "," << cur[13] << "," << cur[14] << "]\n";
    }
  }
  draws_seen_this_frame_ = 0;
#endif

  prev_view_proj_ = frame_view_proj_;
  has_prev_view_proj_ = true;
}
#endif  // DX8TO12_MOTION_VECTORS

void Device::FlushCommandListNoFence() {
  ASSERT_HR(cmd_list_->Close());
  ID3D12CommandList *lists[] = {cmd_list_.Get()};
  cmd_queue_->ExecuteCommandLists(1, lists);
  // Deliberately NOT cmd_allocators_[...]->Reset(): that allocator still
  // backs the commands just submitted, which the GPU has not finished. A
  // command list, unlike its allocator, may be reset the moment it has been
  // submitted, and several lists may share one allocator in sequence.
  ASSERT_HR(cmd_list_->Reset(cmd_allocators_[current_back_buffer_].get(),
                             nullptr));
  // A freshly reset list has no descriptor heaps bound -- the same hazard
  // SubmitAndWait documents at length after its own reset.
  ID3D12DescriptorHeap *heaps[] = {srv_heap_.heap(), sampler_heap_.heap()};
  cmd_list_->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
  dirty_flags_ |= DIRTY_FLAG_OM;
  dirty_flags_ |= DIRTY_FLAG_PSO;
  last_prim_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  root_sig_bound_ = false;
  last_set_pso_ = nullptr;
  last_stencil_ref_ = -1;
  dirty_texture_stage_mask_ = 0xFF;
  dirty_sampler_stage_mask_ = 0xFF;
  last_vbuffer_view_count_ = 0;
}

#ifdef DX8TO12_SCENE_TARGET
void Device::RunDlaaExchange() {
  GpuTexture *color_in = dlss_client_->color_in();
  GpuTexture *backbuffer = back_buffers_.at(current_back_buffer_).Get();
  if (!color_in || !scene_color_tex_) {
    ResolveScenePass();
    return;
  }

  // Hand the three inputs over exactly as rendered. Copies rather than
  // rendering straight into the shared textures: those are created by the
  // DlssClient when TemporalAA is switched on, while the scene target and the
  // motion vector pass exist independently of it and must keep working when
  // it is off.
  auto hand_over = [this](GpuTexture *source, GpuTexture *shared) {
    if (!source || !shared) return;
    TransitionTexture(source, 0, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionTexture(shared, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd_list_->CopyResource(shared->resource(), source->resource());
    // Back to COMMON before the helper touches it. Cross-process state has no
    // shared tracker to consult, so both sides agree on COMMON as the state a
    // resource is in whenever it is not being used by one of them.
    TransitionTexture(shared, 0, D3D12_RESOURCE_STATE_COMMON);
  };
  hand_over(scene_color_tex_.Get(), color_in);
#ifdef DX8TO12_MOTION_VECTORS
  hand_over(depth_copy_tex_.Get(), dlss_client_->depth_in());
  hand_over(motion_vector_tex_.Get(), dlss_client_->mvec_in());
#endif

  // The copy has to have executed before the helper is told the frame is
  // ready, so submit here rather than at the end of the frame.
  FlushCommandListNoFence();

#ifdef DX8TO12_MOTION_VECTORS
  {
    // Everything sl::Constants wants about the camera. Derived here because
    // this is the only side that has the game's matrices at all.
    DlssCameraConstants constants;
    DirectX::SimpleMath::Matrix inv_proj, inv_view, inv_view_proj,
        inv_prev_view_proj;
    frame_proj_.Invert(inv_proj);
    frame_view_.Invert(inv_view);
    frame_view_proj_.Invert(inv_view_proj);
    prev_view_proj_.Invert(inv_prev_view_proj);
    const DirectX::SimpleMath::Matrix clip_to_prev =
        inv_view_proj * prev_view_proj_;
    const DirectX::SimpleMath::Matrix prev_to_clip =
        inv_prev_view_proj * frame_view_proj_;
    // Row-vector (v * M) throughout this codebase, matching D3D8. If the
    // upscaler reads them as column-vector every matrix is effectively
    // transposed, and history gets reprojected by the wrong transform --
    // which looks like content sheared into a skewed quad rather than merely
    // blurred. TransposeUpscalerMatrices exists to settle that by experiment
    // instead of by guessing at another library's convention.
    const bool transpose = GetConfig().transpose_upscaler_matrices;
    auto store = [transpose](float out[16],
                             const DirectX::SimpleMath::Matrix &m) {
      for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
          out[row * 4 + col] = transpose ? m.m[col][row] : m.m[row][col];
        }
      }
    };
    store(constants.view_to_clip, frame_proj_);
    store(constants.clip_to_view, inv_proj);
    store(constants.clip_to_prev_clip, clip_to_prev);
    store(constants.prev_clip_to_clip, prev_to_clip);
    // Rows of the inverse view matrix are the camera's own axes and position.
    for (int i = 0; i < 3; ++i) {
      constants.right[i] = inv_view.m[0][i];
      constants.up[i] = inv_view.m[1][i];
      constants.fwd[i] = inv_view.m[2][i];
      constants.pos[i] = inv_view.m[3][i];
    }
    // Recovered from the projection rather than guessed: D3D8 never tells us
    // the near/far planes or the field of view directly, but a standard
    // perspective projection encodes all of them.
    const float m00 = frame_proj_.m[0][0];
    const float m11 = frame_proj_.m[1][1];
    const float m22 = frame_proj_.m[2][2];
    const float m32 = frame_proj_.m[3][2];
    constants.fov = m11 != 0.f ? 2.f * std::atan(1.f / m11) : 1.f;
    constants.aspect = m00 != 0.f ? m11 / m00 : 1.f;
    constants.near_plane = m22 != 0.f ? -m32 / m22 : 0.1f;
    constants.far_plane = (m22 - 1.f) != 0.f
                              ? m22 * constants.near_plane / (m22 - 1.f)
                              : 1000.f;
    // The motion vector pass writes pixels of the render target; DLSS wants
    // them normalised. If DLAA ghosts or over-sharpens in motion, this scale
    // (or its sign) is the first thing to suspect -- it is the one part of
    // the constants that cannot be derived, only matched to a convention.
    const uint32_t width =
        static_cast<uint32_t>(motion_vector_tex_->resource_desc().Width);
    const uint32_t height = motion_vector_tex_->resource_desc().Height;
    const float mvec_multiplier = GetConfig().mvec_scale_multiplier;
    constants.mvec_scale[0] =
        width ? mvec_multiplier / static_cast<float>(width) : 1.f;
    constants.mvec_scale[1] =
        height ? mvec_multiplier / static_cast<float>(height) : 1.f;
    // Refuse to hand over a camera that does not describe a real view.
    //
    // Two ways that happens. The aspect ratio is derived as m11/m00, and this
    // game's projection has a negative m00 -- so what was being sent was a
    // negative aspect, every frame, since this code was written. And for a
    // few frames after a device Reset the game has not re-established its
    // transforms, so near, far and the field of view come out of a degenerate
    // matrix as zero, infinite or reversed.
    //
    // The upscaler has no defence against either; it accepts them, and its
    // internal state does not recover afterwards. Skipping the frame costs
    // one un-upscaled image, which is invisible, and the history reset tells
    // it there is a gap rather than leaving it to blend across one.
    constants.aspect = std::abs(constants.aspect);
    const bool camera_is_sane =
        std::isfinite(constants.near_plane) &&
        std::isfinite(constants.far_plane) && std::isfinite(constants.fov) &&
        std::isfinite(constants.aspect) && constants.near_plane > 0.f &&
        constants.far_plane > constants.near_plane && constants.fov > 0.f &&
        constants.fov < 3.15f && constants.aspect > 0.f;
    if (!camera_is_sane) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        LOG_ERROR() << "DLSS: refusing a degenerate camera (near="
                    << constants.near_plane << " far=" << constants.far_plane
                    << " fov=" << constants.fov
                    << " aspect=" << constants.aspect
                    << "); presenting this frame un-upscaled.\n";
      }
      dlss_client_->RequestHistoryReset();
      ResolveScenePass();
      return;
    }
    dlss_client_->SetCameraConstants(constants);
  }
#endif
  dlss_client_->SubmitFrame(
#ifdef DX8TO12_TEMPORAL_JITTER
      // JitterSign: the projection is offset by +jitter, but the upscaler may
      // define this value as the correction for that offset rather than the
      // offset itself. Nothing in either API says which.
      GetConfig().jitter_sign * jitter_pixels_.x,
      GetConfig().jitter_sign * jitter_pixels_.y,
#else
      0.f, 0.f,
#endif
      /*reset_history=*/false);

  // Present the *previous* frame's result. Waiting for this frame's would put
  // the whole helper round trip back on the critical path, which measured
  // ~1.6ms with the GPU sitting idle -- pure serialisation, not work.
  GpuTexture *color_out = dlss_client_->AcquirePreviousResult();

  if (!color_out) {
    ++frames_fallback_;
    // Normal on the first frame (there is no previous one) and on any frame
    // the helper missed. Falls back to the plain resolve, which itself knows
    // to do nothing when the scene and the backbuffer are different sizes.
    // Called before clearing scene_pass_active_, because that flag is exactly
    // what ResolveScenePass early-returns on.
    ResolveScenePass();
    return;
  }
  scene_pass_active_ = false;
  ++frames_upscaled_;

  TransitionTexture(color_out, 0, D3D12_RESOURCE_STATE_COPY_SOURCE);
  TransitionTexture(backbuffer, 0, D3D12_RESOURCE_STATE_COPY_DEST);
  cmd_list_->CopyResource(backbuffer->resource(), color_out->resource());
  // Leave it as the helper expects to find it next frame.
  TransitionTexture(color_out, 0, D3D12_RESOURCE_STATE_COMMON);
  dirty_flags_ |= DIRTY_FLAG_OM;
  dirty_flags_ |= DIRTY_FLAG_PSO;
}
#endif

#ifdef DX8TO12_SCENE_TARGET
void Device::EndScenePassIfDrawIsUi(bool draw_is_pretransformed) {
  if (!scene_pass_active_) return;
  if (!draw_is_pretransformed) {
    frame_had_3d_draw_ = true;
    return;
  }
  // A 2D draw before any 3D one is not the HUD arriving -- it is a fade, a
  // letterbox or a loading screen, and the scene has not been rendered yet.
  if (!frame_had_3d_draw_) return;

#ifdef DX8TO12_MOTION_VECTORS
  // Must run while the scene's depth buffer still holds this frame's scene,
  // i.e. before the pass below ends it.
  RecordMotionVectorPass();
#endif
  // From here the frame is finished as far as the upscaler is concerned:
  // resolve it onto the backbuffer, and let everything after this draw onto
  // that, at output resolution and with no temporal processing.
  if (dlss_client_ && dlss_client_->PollReady()) {
    RunDlaaExchange();
  } else {
    ResolveScenePass();
  }
}
#endif

#if defined(DX8TO12_SCENE_TARGET) || defined(DX8TO12_MOTION_VECTORS)
void Device::PollGraphicsHotkey() {
  // GetAsyncKeyState rather than the game's input: this has to work while the
  // game has focus and runs its own message loop.

  // F8 tears the upscaler down and starts it again from nothing -- a new
  // helper process, new shared resources, a new feature.
  //
  // It exists to answer one question that four attempted fixes could not:
  // when the picture is stuck black, is the bad state inside the upscaler or
  // in what this side keeps sending it? Pressing this recovers the picture
  // only in the first case. Either answer rules out half of what is left,
  // which is more than another guess has managed.
  //
  // It is also a real recovery: whatever the cause, a player with a black
  // screen would rather press a key than restart the game.
#ifdef DX8TO12_SCENE_TARGET
  {
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !upscaler_restart_hotkey_was_down_ && dlss_client_) {
      LOG(AixLog::Severity::error)
          << "F8: restarting the upscaler from scratch.\n";
      const uint32_t render_w = dlss_client_->render_width();
      const uint32_t render_h = dlss_client_->render_height();
      const uint32_t out_w = dlss_client_->output_width();
      const uint32_t out_h = dlss_client_->output_height();
      dlss_client_->Stop();
      if (render_w && out_w) {
        dlss_client_->Start(render_w, render_h, out_w, out_h,
                            GetConfig().temporal_aa == 2 ? DlssIpc::Mode::kDlss
                                                         : DlssIpc::Mode::kDlaa);
      }
    }
    upscaler_restart_hotkey_was_down_ = f8;
  }
#endif
#ifdef DX8TO12_DRAW_STATE_CACHE
  // F6, separate from F5: this is a CPU-side optimisation and the upscaler is
  // a GPU-side one, so mixing them into one cycle would mean never measuring
  // either alone.
  {
    const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6 && !draw_cache_hotkey_was_down_) {
      const bool now = !GetConfig().draw_state_cache;
      SetConfigValueBool("DrawStateCache", now);
      LOG(AixLog::Severity::error)
          << "=== F6: draw state cache now " << (now ? "ON" : "OFF") << " ===\n";
    }
    draw_cache_hotkey_was_down_ = f6;
  }
#endif
  {
    // F7: near-plane clipping. Its own key for the same reason as F6 -- it
    // changes what renders, not how fast, so bundling it into another cycle
    // would make both harder to attribute.
    const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !clip_hotkey_was_down_) {
      const bool now = !GetConfig().near_plane_clipping;
      SetConfigValueBool("NearPlaneClipping", now);
      LOG(AixLog::Severity::error)
          << "=== F7: near plane clipping now " << (now ? "ON" : "OFF")
          << " ===\n";
    }
    clip_hotkey_was_down_ = f7;
  }

  const bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
  const bool pressed = down && !graphics_hotkey_was_down_;
  graphics_hotkey_was_down_ = down;
  if (!pressed) return;

  // Off -> DLAA (1:1) -> DLSS (quality) -> Off. Three points is what an A/B
  // actually needs: no upscaler, upscaler at the same resolution, and
  // upscaler reconstructing from fewer pixels.
  Config &config = GetConfig();
  const char *name = nullptr;
  if (config.temporal_aa == 0) {
    SetConfigValueInt("TemporalAA", 1);
    SetConfigValueFloat("RenderScale", 1.0f);
    name = "DLAA (1:1)";
  } else if (config.render_scale > 0.95f) {
    SetConfigValueInt("TemporalAA", 2);
    SetConfigValueFloat("RenderScale", 0.667f);
    name = "DLSS quality (0.667)";
  } else {
    SetConfigValueInt("TemporalAA", 0);
    SetConfigValueFloat("RenderScale", 1.0f);
    name = "off";
  }
  LOG(AixLog::Severity::error)
      << "=== F5: temporal upscaling now " << name << " ===\n";

  // The resources are sized from the scale, so a change means rebuilding
  // them -- and they may still be referenced by frames the GPU has not
  // finished. Wait for the last submitted frame before freeing anything.
  //
  // Deliberately not WaitForFrame(): with the command list open, that routes
  // into SubmitAndWait, which is the caller's caller here. This waits on the
  // already-submitted work directly, which is all these resources can be
  // held by.
  if (next_fence_ > 1 &&
      cmd_list_done_fence_->GetCompletedValue() < next_fence_ - 1) {
    ASSERT_HR(cmd_list_done_fence_->SetEventOnCompletion(
        next_fence_ - 1, cmd_list_done_event_handle_));
    WaitForSingleObjectEx(cmd_list_done_event_handle_, 5000, FALSE);
  }
  RecreateSceneScaleResources();
#ifdef DX8TO12_MOTION_VECTORS
  // History across a resolution change is meaningless.
  has_prev_view_proj_ = false;
#endif
}

void Device::RecreateSceneScaleResources() {
  // Everything here is sized from the render scale, so changing that scale
  // means rebuilding all of it. Split out of Reset so a runtime change (the
  // F5 toggle) can rebuild without a device Reset -- these are all our own
  // resources; the game has no idea they exist.
  scene_render_scale_ = GetConfig().temporal_aa != 0
                            ? std::clamp(GetConfig().render_scale, 0.5f, 1.0f)
                            : 1.0f;
  scene_render_width_ = std::max(
      1u, static_cast<uint32_t>(scene_output_width_ * scene_render_scale_));
  scene_render_height_ = std::max(
      1u, static_cast<uint32_t>(scene_output_height_ * scene_render_scale_));
  LOG(AixLog::Severity::info)
      << "Scene resources: " << scene_render_width_ << "x"
      << scene_render_height_ << " (scale " << scene_render_scale_
      << "), output " << scene_output_width_ << "x" << scene_output_height_
      << "\n";

#ifdef DX8TO12_SCENE_TARGET
  scene_color_tex_.Reset();
  scene_depth_tex_.Reset();
  scene_color_tex_ = ComOwn(static_cast<GpuTexture *>(BaseTexture::Create(
      this, TextureKind::Texture2d, scene_render_width_, scene_render_height_,
      1, 1, D3DUSAGE_RENDERTARGET, scene_color_format_, D3DPOOL_DEFAULT)));
  scene_color_tex_->SetName("scene_color_tex");
  // A depth buffer of the scene's own size, and only when the scene is
  // actually smaller. The game's depth_stencil_tex_ is shared with its own
  // render targets (radar, menu blur, mirrors), which still draw at output
  // resolution; binding a smaller depth buffer under those would clip them.
  if (scene_render_scale_ != 1.0f && scene_depth_format_ != D3DFMT_UNKNOWN) {
    scene_depth_tex_ = ComOwn(static_cast<GpuTexture *>(BaseTexture::Create(
        this, TextureKind::Texture2d, scene_render_width_,
        scene_render_height_, 1, 1, D3DUSAGE_DEPTHSTENCIL, scene_depth_format_,
        D3DPOOL_DEFAULT)));
    scene_depth_tex_->SetName("scene_depth_tex");
  }
#endif

#ifdef DX8TO12_MOTION_VECTORS
  motion_vector_tex_.Reset();
  depth_copy_tex_.Reset();
  // R16G16_FLOAT has no D3DFORMAT this codebase maps, and inventing one would
  // put a format the game can never ask for into the conversion tables.
  // Create the resource directly and wrap it the way the back buffers are
  // wrapped -- InitFromResource gives it an RTV and an SRV.
  const D3D12_HEAP_PROPERTIES heap_props{.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC desc{
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
      // The scene's resolution, not the output's: these describe the scene
      // the upscaler is reconstructing from.
      .Width = scene_render_width_,
      .Height = scene_render_height_,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = DXGI_FORMAT_R16G16_FLOAT,
      .SampleDesc = {.Count = 1, .Quality = 0},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> mvec_resource;
  // No optimized clear value: the fullscreen triangle writes every pixel, so
  // this target is never cleared.
  ASSERT_HR(d3d12_device_->CreateCommittedResource(
      &heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
      nullptr, IID_PPV_ARGS(mvec_resource.GetForInit())));
  motion_vector_tex_ = ComOwn(GpuTexture::InitFromResource(this, mvec_resource));
  motion_vector_tex_->SetName("motion_vector_tex");

  D3D12_RESOURCE_DESC depth_desc = desc;
  depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
  ComPtr<ID3D12Resource> depth_resource;
  ASSERT_HR(d3d12_device_->CreateCommittedResource(
      &heap_props, D3D12_HEAP_FLAG_NONE, &depth_desc,
      D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(depth_resource.GetForInit())));
  depth_copy_tex_ = ComOwn(GpuTexture::InitFromResource(this, depth_resource));
  depth_copy_tex_->SetName("depth_copy_tex");
#endif
}
#endif

GpuTexture *Device::CurrentDepthTarget() {
#ifdef DX8TO12_SCENE_TARGET
  // Only substitute for the game's own depth while the scene pass is drawing
  // into the scene target at a different size. Anything the game renders to
  // its own target keeps its own depth buffer, at its own resolution.
  if (scene_pass_active_ && !bound_render_target_ && scene_depth_tex_ &&
      scene_render_scale_ != 1.f && bound_depth_target_) {
    return scene_depth_tex_.Get();
  }
#endif
  return bound_depth_target_.Get();
}

D3D12_VIEWPORT Device::EffectiveViewport() {
  D3D12_VIEWPORT result = viewport_;
#ifdef DX8TO12_SCENE_TARGET
  if (scene_pass_active_ && !bound_render_target_ && scene_render_scale_ != 1.f) {
    result.TopLeftX *= scene_render_scale_;
    result.TopLeftY *= scene_render_scale_;
    result.Width *= scene_render_scale_;
    result.Height *= scene_render_scale_;
  }
#endif
  return result;
}

GpuTexture *Device::CurrentColorTarget() {
  // A game-set render target always wins: that is the game explicitly drawing
  // somewhere other than its backbuffer (radar map, mirror, menu blur), and
  // it must keep working exactly as before.
  if (bound_render_target_) return bound_render_target_.Get();
#ifdef DX8TO12_SCENE_TARGET
  if (scene_pass_active_ && scene_color_tex_) return scene_color_tex_.Get();
#endif
  return back_buffers_.at(current_back_buffer_).Get();
}

bool Device::RequestDepthBufferAccess(bool enable) {
  depth_buffer_access_requested_ = enable;
  return true;
}

void* Device::depth_buffer_srv_resource() {
  if (!depth_buffer_access_requested_ || !bound_depth_target_) return nullptr;
  return bound_depth_target_->resource();
}

uint64_t Device::depth_buffer_srv_gpu_handle() {
  if (!depth_buffer_access_requested_ || !bound_depth_target_) return 0;
  return srv_heap_.GetGPUHandleFor(bound_depth_target_->srv_handle()).ptr;
}

uint32_t Device::depth_buffer_srv_format() {
  if (!depth_buffer_access_requested_ || !bound_depth_target_) return DXGI_FORMAT_UNKNOWN;
  return DepthSrvFormatFromTypeless(bound_depth_target_->resource_desc().Format);
}

bool Device::GetViewProjMatrix(float out_matrix[16]) const {
  if (!out_matrix) return false;
  static const DirectX::SimpleMath::Matrix kIdentity;
  auto transform_or_identity = [this](D3DTRANSFORMSTATETYPE state) {
    const auto it = transforms_.find(state);
    DirectX::SimpleMath::Matrix result = kIdentity;
    if (it != transforms_.end()) memcpy(&result, &it->second, sizeof(result));
    return result;
  };
  const DirectX::SimpleMath::Matrix view_proj =
      transform_or_identity(D3DTS_VIEW) * transform_or_identity(D3DTS_PROJECTION);
  memcpy(out_matrix, &view_proj, sizeof(view_proj));
  return true;
}

void Device::GetUpscalerStatus(Dx8to12_UpscalerStatus *out) const {
  if (!out) return;
  *out = Dx8to12_UpscalerStatus{};
  out->mode = GetConfig().temporal_aa;
#if defined(DX8TO12_SCENE_TARGET) && defined(DX8TO12_MOTION_VECTORS)
  out->compiled_in = 1;
  out->output_width = scene_output_width_;
  out->output_height = scene_output_height_;
  out->render_width = scene_render_width_;
  out->render_height = scene_render_height_;
  if (dlss_client_) {
    out->helper_running = dlss_client_->helper_running() ? 1 : 0;
    out->ready = dlss_client_->ready() ? 1 : 0;
    out->healthy = dlss_client_->healthy() ? 1 : 0;
    out->helper_status = static_cast<int>(dlss_client_->helper_status());
    out->failed_frames = dlss_client_->failed_frames();
    out->preset = static_cast<int>(dlss_client_->preset());
    // The client's own sizes are authoritative once it is running: they are
    // what the shared textures were actually created at, which is not the
    // same as the current setting if the setting changed since.
    out->render_width = dlss_client_->render_width();
    out->render_height = dlss_client_->render_height();
    out->output_width = dlss_client_->output_width();
    out->output_height = dlss_client_->output_height();
  }
#endif
}

// Everything GetUpscalerStatus reports, plus the neural rendering fields, into
// a struct that carries its own size. Fields beyond the caller's size are not
// written -- which is the entire reason this variant exists.
void Device::GetUpscalerStatusEx(Dx8to12_UpscalerStatusEx *out) const {
  if (!out) return;
  const int size = out->struct_size;
  if (size < static_cast<int>(sizeof(int))) return;

  Dx8to12_UpscalerStatus base = {};
  GetUpscalerStatus(&base);

  Dx8to12_UpscalerStatusEx full = {};
  full.struct_size = size;
  full.compiled_in = base.compiled_in;
  full.helper_running = base.helper_running;
  full.ready = base.ready;
  full.healthy = base.healthy;
  full.mode = base.mode;
  full.preset = base.preset;
  full.helper_status = base.helper_status;
  full.failed_frames = base.failed_frames;
  full.render_width = base.render_width;
  full.render_height = base.render_height;
  full.output_width = base.output_width;
  full.output_height = base.output_height;
  full.frames_upscaled = frames_upscaled_;
  full.frames_fallback = frames_fallback_;
  full.frames_bypassed = frames_bypassed_;
#ifdef DX8TO12_SCENE_TARGET
  if (dlss_client_) {
    full.neural_rendering_active =
        static_cast<int>(dlss_client_->neural_rendering_active());
    full.neural_rendering_available =
        static_cast<int>(dlss_client_->neural_rendering_available());
    strncpy_s(full.neural_rendering_runtime,
              sizeof(full.neural_rendering_runtime),
              dlss_client_->neural_rendering_runtime(), _TRUNCATE);
  }
#endif

  const size_t copy = static_cast<size_t>(size) < sizeof(full)
                          ? static_cast<size_t>(size)
                          : sizeof(full);
  memcpy(out, &full, copy);
}

int Device::GetActiveLightCount() const {
  return static_cast<int>(enabled_lights_.size());
}

bool Device::GetActiveLight(int index, Dx8to12_LightInfo* out) const {
  if (!out || index < 0) return false;
  // enabled_lights_ is an unordered_set -- snapshot into a stable, sorted
  // order so a mod iterating 0..GetActiveLightCount()-1 doesn't see the
  // order silently shuffle between calls within the same frame.
  std::array<DWORD, kMaxActiveLights> sorted_indices;
  int count = 0;
  for (DWORD light_index : enabled_lights_) sorted_indices[count++] = light_index;
  std::sort(sorted_indices.begin(), sorted_indices.begin() + count);
  if (index >= count) return false;
  const auto it = lights_.find(sorted_indices[index]);
  if (it == lights_.end()) return false;
  const D3DLIGHT8& light = it->second;
  *out = Dx8to12_LightInfo{
      .type = static_cast<int>(light.Type),
      .diffuse = {light.Diffuse.r, light.Diffuse.g, light.Diffuse.b, light.Diffuse.a},
      .specular = {light.Specular.r, light.Specular.g, light.Specular.b, light.Specular.a},
      .ambient = {light.Ambient.r, light.Ambient.g, light.Ambient.b, light.Ambient.a},
      .position = {light.Position.x, light.Position.y, light.Position.z},
      .direction = {light.Direction.x, light.Direction.y, light.Direction.z},
      .range = light.Range,
      .falloff = light.Falloff,
      .attenuation0 = light.Attenuation0,
      .attenuation1 = light.Attenuation1,
      .attenuation2 = light.Attenuation2,
      .theta = light.Theta,
      .phi = light.Phi,
  };
  return true;
}

bool Device::GetRtDirectionalLight(D3DVECTOR* direction) const {
  if (!direction) return false;
  for (const DWORD index : enabled_lights_) {
    const auto it = lights_.find(index);
    if (it != lights_.end() && it->second.Type == D3DLIGHT_DIRECTIONAL) {
      *direction = it->second.Direction;
      return true;
    }
  }
  return false;
}

uint32_t Device::RtCurrentNormalByteOffset() {
  const auto it = vertex_shaders_.find(bound_vertex_shader_);
  if (it == vertex_shaders_.end()) return UINT_MAX;
  const VertexShader* shader = it->second.Get();
  if (!shader) return UINT_MAX;
  for (const D3D12_INPUT_ELEMENT_DESC& element : shader->decl.input_elements) {
    if (element.SemanticName && strcmp(element.SemanticName, "NORMAL") == 0 &&
        element.InputSlot == 0 &&
        element.Format == DXGI_FORMAT_R32G32B32_FLOAT) {
      return element.AlignedByteOffset;
    }
  }
  return UINT_MAX;
}

void Device::RegisterModRenderCallback(ModRenderCallback callback) {
  std::lock_guard lock(mod_render_callbacks_mutex_);
  if (std::find(mod_render_callbacks_.begin(), mod_render_callbacks_.end(),
                callback) != mod_render_callbacks_.end()) {
    return;
  }
  // DIAGNOSTIC: identify which loaded module actually registered this --
  // see the ROOTTABLE-SRV/SAMPLER diagnostics in PrepareDrawCall, which
  // proved a SetGraphicsRootDescriptorTable-heap-mismatch error wasn't
  // coming from our own binding code, pointing at an external mod using
  // this exact API instead.
#ifdef DX8TO12_ENABLE_VALIDATION
  {
    HMODULE module = nullptr;
    char module_path[MAX_PATH] = {};
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(callback), &module) &&
        GetModuleFileNameA(module, module_path, sizeof(module_path))) {
      LOG(AixLog::Severity::error)
          << "MODRENDERCALLBACK-REGISTERED callback=" << (void *)callback
          << " module=" << module_path << "\n";
    } else {
      LOG(AixLog::Severity::error)
          << "MODRENDERCALLBACK-REGISTERED callback=" << (void *)callback
          << " module=<unresolved>\n";
    }
  }
#endif
  mod_render_callbacks_.push_back(callback);
}

void Device::UnregisterModRenderCallback(ModRenderCallback callback) {
  std::lock_guard lock(mod_render_callbacks_mutex_);
  std::erase(mod_render_callbacks_, callback);
}

bool Device::RegisterPixelShaderInjection(
    Dx8to12_PixelShaderInjectionFn callback) {
  if (!callback) return false;
  std::lock_guard lock(pixel_shader_injection_mutex_);
  if (pixel_shader_injection_callback_ &&
      pixel_shader_injection_callback_ != callback) {
    return false;
  }
  if (pixel_shader_injection_callback_ == callback) return true;
  pixel_shader_injection_callback_ = callback;
  ++pixel_shader_injection_generation_;
  return true;
}

bool Device::UnregisterPixelShaderInjection(
    Dx8to12_PixelShaderInjectionFn callback) {
  std::lock_guard lock(pixel_shader_injection_mutex_);
  if (!callback || pixel_shader_injection_callback_ != callback) return false;
  pixel_shader_injection_callback_ = nullptr;
  ++pixel_shader_injection_generation_;
  return true;
}

bool Device::GetPixelShaderInjectionState(uint64_t* generation) const {
  std::lock_guard lock(pixel_shader_injection_mutex_);
  if (generation) *generation = pixel_shader_injection_generation_;
  return pixel_shader_injection_callback_ != nullptr;
}

size_t Device::InvokePixelShaderInjection(
    const Dx8to12_PixelShaderInjectionContext* context,
    char* out_hlsl_snippet, size_t capacity) const {
  // Keep the module callback registered (and therefore valid) for the whole
  // invocation. Unregister on another thread waits here before its caller is
  // allowed to unload the ASI containing the function pointer.
  std::lock_guard lock(pixel_shader_injection_mutex_);
  return pixel_shader_injection_callback_
             ? pixel_shader_injection_callback_(context, out_hlsl_snippet,
                                                capacity)
             : 0;
}

void Device::InvalidatePixelShaderCache() {
  std::lock_guard lock(pixel_shader_injection_mutex_);
  ++pixel_shader_injection_generation_;
}

HRESULT STDMETHODCALLTYPE
Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  TRACE_ENTRY(pPresentationParameters);
  sync_interval_ = SyncIntervalFromD3DPresentInterval(
      pPresentationParameters->FullScreen_PresentationInterval);
  if (!(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED)) {
    LOG(INFO) << "Resetting device: Submitting commands..\n";
    SubmitAndWait(false);
    LOG(INFO) << "Reset: WaitForFrame(next_fence_ - 1)\n";
    WaitForFrame(next_fence_ - 1);
    LOG(INFO) << "Reset: cmd_list_->Close()\n";
    ASSERT_HR(cmd_list_->Close());
    dirty_flags_ |= DIRTY_FLAG_CMD_LIST_CLOSED;
  } else {
    LOG(INFO) << "Resetting device. Commands already submitted.\n";
  }
  LOG(INFO) << "Reset: releasing old back buffers/depth-stencil\n";
  // These caches (see the comment on their declaration in device.h) hold
  // their own ref on the depth-stencil/render-target texture via the
  // GpuSurface they wrap, on top of the refs checked below. Left in place
  // across a Reset, that extra ref keeps the old texture's total_ref_count()
  // at 2 forever, which silently defeats the asserts right after this (the
  // user can Ignore through the message box) and, worse, means the old
  // GpuTexture's destructor -- and the DSV/RTV descriptor it frees -- never
  // runs. Repeated Reset() calls then permanently burn one dsv_heap_/
  // rtv_heap_ slot each, eventually exhausting the 32-slot pool.
  cached_render_target_surface_.Reset();
  cached_render_target_surface_key_ = 0;
  ++bound_render_target_generation_;
  cached_depth_stencil_surface_.Reset();
  cached_depth_stencil_surface_key_ = nullptr;
  bound_render_target_.Reset();
  bound_depth_target_.Reset();
  ASSERT(depth_stencil_tex_->total_ref_count() == 1);
  for (auto &rtv : back_buffers_) {
    ASSERT(rtv->total_ref_count() == 1);
  }
  back_buffers_.clear();
  depth_stencil_tex_.Reset();
  DXGI_FORMAT new_format = ToFlipModelSwapChainFormat(
      DXGIFromD3DFormat(pPresentationParameters->BackBufferFormat));

  // Deliberately not calling IDXGISwapChain::ResizeTarget here: for a
  // windowed swap chain (this codebase never calls SetFullscreenState, so
  // that's always the case), ResizeTarget actually moves/resizes the target
  // *window* itself via SetWindowPos -- real D3D8 never touched the app's
  // window like that. That SetWindowPos synchronously dispatches
  // WM_WINDOWPOSCHANGING/CHANGED/SIZE to the window's own WndProc, and a
  // real crash log (GTA: Vice City) showed exactly this: our code calling
  // into the game's WndProc during this Reset, which then null-derefs
  // because the game evidently isn't ready to handle a resize message this
  // early in its own init sequence. ResizeBuffers alone is enough to make
  // the swap chain match the window's existing size -- we're not the one
  // deciding the window should move or resize.
  LOG(INFO) << "Reset: swap_chain_->ResizeBuffers()\n";
  {
    const HRESULT resize_hr = swap_chain_->ResizeBuffers(
        kNumBackBuffers, pPresentationParameters->BackBufferWidth,
        pPresentationParameters->BackBufferHeight, new_format,
        tearing_supported_
            ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
            : 0u);
    if (FAILED(resize_hr)) {
      // The two realistic causes need completely different fixes and a bare
      // "SUCCEEDED(_hr) failed" box distinguishes neither: DXGI_ERROR_INVALID_CALL
      // (0x887A0001) means something still holds a reference to a back
      // buffer, while DXGI_ERROR_DEVICE_REMOVED (0x887A0005) means the device
      // died *earlier* -- a bad barrier or a GPU fault -- and Reset is merely
      // the first call to notice. In the latter case the device's own removal
      // reason is the code that actually identifies the fault.
      const HRESULT removed_hr = d3d12_device_->GetDeviceRemovedReason();
      LOG_ERROR() << "Reset: ResizeBuffers failed hr=" << std::hex << resize_hr
                  << " removedReason=" << removed_hr << std::dec << "\n";
      FAIL("ResizeBuffers(%ux%u fmt=%d) failed.\nhr = 0x%08lX\n"
           "GetDeviceRemovedReason = 0x%08lX",
           pPresentationParameters->BackBufferWidth,
           pPresentationParameters->BackBufferHeight,
           static_cast<int>(new_format),
           static_cast<unsigned long>(resize_hr),
           static_cast<unsigned long>(removed_hr));
    }
  }
  LOG(INFO) << "Reset: swap_chain_->ResizeBuffers() done\n";

  DXGI_SWAP_CHAIN_DESC swap_chain_desc;
  ASSERT_HR(swap_chain_->GetDesc(&swap_chain_desc));

  if (pPresentationParameters->EnableAutoDepthStencil) {
    LOG(INFO) << "Reset: creating depth-stencil texture\n";
    D3DFORMAT depth_format = pPresentationParameters->AutoDepthStencilFormat;
    if (depth_format == D3DFMT_UNKNOWN) depth_format = D3DFMT_D32;
    ASSERT(depth_format == D3DFMT_D16 || depth_format == D3DFMT_D32 ||
           depth_format == D3DFMT_D24S8 || depth_format == D3DFMT_D24X8 ||
           depth_format == D3DFMT_D24X4S4);
    depth_stencil_tex_ = ComOwn(static_cast<GpuTexture *>(BaseTexture::Create(
        this, TextureKind::Texture2d, pPresentationParameters->BackBufferWidth,
        pPresentationParameters->BackBufferHeight, 1, 1, D3DUSAGE_DEPTHSTENCIL,
        depth_format, D3DPOOL_DEFAULT)));
    depth_stencil_tex_->SetName("depth_stencil_tex");
    bound_depth_target_ = InternalPtr(depth_stencil_tex_.Get());
    // See the matching comment in Init(): D3DRS_ZENABLE defaults to
    // D3DZB_TRUE when EnableAutoDepthStencil is set.
    render_state_.zbuffer_type = D3DZB_TRUE;
  }

  LOG(INFO) << "Reset: re-acquiring " << swap_chain_desc.BufferCount
            << " back buffer(s)\n";
  ASSERT(back_buffers_.empty());
  for (uint32_t i = 0; i < swap_chain_desc.BufferCount; ++i) {
    ComPtr<ID3D12Resource> back_buffer_resource;
    ASSERT_HR(swap_chain_->GetBuffer(
        i, IID_PPV_ARGS(back_buffer_resource.GetForInit())));
    GpuTexture *back_buffer =
        GpuTexture::InitFromResource(this, back_buffer_resource);
    back_buffer->SetName(std::string("back_buffer_") + std::to_string(i));
    back_buffers_.push_back(ComOwn(back_buffer));
  }
  LOG(INFO) << "Reset: back buffers re-acquired\n";

#if defined(DX8TO12_SCENE_TARGET) || defined(DX8TO12_MOTION_VECTORS)
  // Remembered so the same resources can be rebuilt when RenderScale changes
  // at runtime, without a device Reset the game never asked for.
  scene_output_width_ = pPresentationParameters->BackBufferWidth;
  scene_output_height_ = pPresentationParameters->BackBufferHeight;
  scene_color_format_ = DXGIToD3DFormat(new_format);
  scene_depth_format_ = D3DFMT_UNKNOWN;
  if (pPresentationParameters->EnableAutoDepthStencil) {
    scene_depth_format_ = pPresentationParameters->AutoDepthStencilFormat;
    if (scene_depth_format_ == D3DFMT_UNKNOWN) scene_depth_format_ = D3DFMT_D32;
  }
  RecreateSceneScaleResources();
  // The PSO cache keys on the bound render target's DXGI format and never
  // evicts (see CreatePSO). If the D3DFORMAT round-trip above ever landed on
  // a different DXGI format than the swap chain's, every PSO would be built
  // twice and -- worse -- the RTV format a PSO declares would not match the
  // resource actually bound. Fail loudly here instead.
  ASSERT(back_buffers_.at(0)->resource_desc().Format == new_format);
#endif
#ifdef DX8TO12_SCENE_TARGET
  // Reset() ends a frame's worth of state; the next frame starts with the
  // scene pass open again -- if anything wants it. With it closed, every
  // downstream site (CurrentColorTarget, ResolveScenePass) falls back to the
  // backbuffer on its own, so this one assignment is the whole switch.
  scene_pass_active_ = SceneTargetWanted();
  frame_had_3d_draw_ = false;
#endif
#ifdef DX8TO12_MOTION_VECTORS
  // The camera of the frame before a Reset says nothing about the frame
  // after it -- a resolution change or device loss is a discontinuity.
  has_prev_view_proj_ = false;
  frame_view_proj_captured_ = false;
  // And say the same thing to the upscaler, which keeps a history of its own
  // that this side cannot see or correct. RequestHistoryReset was written for
  // exactly this -- its comment names a device Reset as one of the two things
  // that invalidate the previous frame -- but nothing ever called it from
  // here.
  //
  // The cost of that omission is not a stale frame or two. Minimising the
  // window loses the device, and while it is gone the game presents nothing,
  // so what accumulates in the upscaler's history is darkness. Auto exposure
  // then adapts to that, and on restore the whole scene comes back black
  // apart from the brightest emissive sprites, which bloom across the
  // screen -- with no way out, because the exposure it is now stuck at is
  // derived from the very history that needs discarding.
  if (dlss_client_) dlss_client_->NotifyDeviceReset();
  // Rebuilt here rather than once at Init: the debug PSO's render target
  // format comes from the scene target, which was just recreated.
  InitMotionVectorPass();
#endif

  current_back_buffer_ = swap_chain_->GetCurrentBackBufferIndex();

  LOG(INFO) << "Reset: final allocator/cmd list reset\n";
  ASSERT_HR(cmd_allocators_[current_back_buffer_]->Reset());
  ASSERT_HR(
      cmd_list_->Reset(cmd_allocators_[current_back_buffer_].get(), nullptr));
  {
    ID3D12DescriptorHeap *heaps[] = {srv_heap_.heap(), sampler_heap_.heap()};
    cmd_list_->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);
  }
  dirty_flags_ ^= DIRTY_FLAG_CMD_LIST_CLOSED;
  last_prim_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  // A fresh command list has no root signature, root arguments, or pipeline
  // state bound.
  root_sig_bound_ = false;
  last_set_pso_ = nullptr;
  last_stencil_ref_ = -1;
  dirty_texture_stage_mask_ = 0xFF;
  dirty_sampler_stage_mask_ = 0xFF;
  last_vbuffer_view_count_ = 0;
  // Everything the renderer thinks is bound was dropped along with the old
  // command list.
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
  ++swap_chain_generation_;

  LOG(INFO) << "Reset: done\n";
  return S_OK;
}

D3DCAPS8 Device::GetDefaultCaps(UINT adapter_index) {
  D3DCAPS8 caps{
      .DeviceType = D3DDEVTYPE_HAL,
      .AdapterOrdinal = adapter_index,
      .Caps = 0,  // D3DCAPS_READ_SCANLINE or D3DCAPS_OVERLAY.
      // Do not advertise managed-resource support.  The DX12 backend keeps
      // managed textures as explicit GPU resources (kDisableManagedResources
      // is true), so claiming CANMANAGERESOURCE makes RenderWare choose its
      // driver-managed streaming path even though that contract is not
      // implemented here.  The working D3D8->D3D11 port omits this bit and
      // therefore uses the explicit SYSTEMMEM + UpdateTexture path.
      .Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_DYNAMICTEXTURES,
      .Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD,
      .PresentationIntervals =
          D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE |
          D3DPRESENT_INTERVAL_TWO | D3DPRESENT_INTERVAL_THREE |
          D3DPRESENT_INTERVAL_FOUR,

      .CursorCaps = D3DCURSORCAPS_COLOR,

      .DevCaps =
          D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
          D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY |
          D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
          D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
          D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX |
          D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_CANBLTSYSTONONLOCAL |
          D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE,

      .PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE |
                           D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                           D3DPMISCCAPS_COLORWRITEENABLE |
                           D3DPMISCCAPS_CLIPPLANESCALEDPOINTS |
                           D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_BLENDOP,

      .RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX |
                    D3DPRASTERCAPS_ANTIALIASEDGES |
                    D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS |
                    D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY |
                    D3DPRASTERCAPS_COLORPERSPECTIVE,

      .ZCmpCaps = 0xFF,
      .SrcBlendCaps = 0x1FFF,
      .DestBlendCaps = 0x1FFF,
      .AlphaCmpCaps = 0xFF,
      .ShadeCaps = 0xFFFFFFFF,
      // Deliberately not advertising D3DPTEXTURECAPS_VOLUMEMAP/
      // MIPVOLUMEMAP: CreateVolumeTexture is an unimplemented stub, and a
      // compliant game that checks capabilities before using a feature
      // (rather than just trying it) would otherwise get a clean "yes,
      // supported" answer here and then abort on the actual create call.
      .TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA |
                     D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPMAP |
                     D3DPTEXTURECAPS_MIPCUBEMAP,
      .TextureFilterCaps =
          D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
          D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT |
          D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
          D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC,
      // .CubeTextureFilterCaps =.VolumeTextureFilterCaps =.TextureFilterCaps,
      .TextureAddressCaps = 0xFF,
      .VolumeTextureAddressCaps = 0xFF,

      .LineCaps = 0,

      .MaxTextureWidth = 8182,
      .MaxTextureHeight = 8192,
      .MaxVolumeExtent = 2048,

      .MaxTextureRepeat = 128,
      .MaxTextureAspectRatio = 8192,
      .MaxAnisotropy = 16,
      .MaxVertexW = 1410065408,

      .GuardBandLeft = -FLT_MAX,
      .GuardBandTop = -FLT_MAX,
      .GuardBandRight = FLT_MAX,
      .GuardBandBottom = FLT_MAX,
      .ExtentsAdjust = 0,
      .StencilCaps = 0x1FF,

      .FVFCaps = D3DFVFCAPS_DONOTSTRIPELEMENTS |
                 D3DFVFCAPS_TEXCOORDCOUNTMASK,  // Do we need PSIZE?
      .TextureOpCaps = 0xFFFFFFFF,
      .MaxTextureBlendStages = 8,
      .MaxSimultaneousTextures = 8,

      .VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 |
                              D3DVTXPCAPS_DIRECTIONALLIGHTS |
                              D3DVTXPCAPS_POSITIONALLIGHTS,
      .MaxActiveLights = kMaxActiveLights,
      .MaxUserClipPlanes = 8,
      // 0, not 4: CreateFixedFunctionVertexShader (vertex_shader.cpp)
      // explicitly rejects D3DFVF_XYZB1..5 (FVF-based vertex blending) --
      // advertising real blend-matrix support here would invite a
      // compliant game doing fixed-function skinning to hit that assert
      // instead of falling back to shader-based skinning.
      .MaxVertexBlendMatrices = 0,
      .MaxVertexBlendMatrixIndex = 0,  // ??

      .MaxPointSize = 1.f,

      .MaxPrimitiveCount = 0xFFFFFF,
      .MaxVertexIndex = 0xFFFFFF,  // Completely arbitrary.
      .MaxStreams = 16,
      .MaxStreamStride = 0xFF,

      .VertexShaderVersion = D3DVS_VERSION(1, 1),
      .MaxVertexShaderConst = kNumVsConstRegs,
      .PixelShaderVersion = D3DPS_VERSION(1, 3),
      .MaxPixelShaderValue = 65504.f};

  caps.CubeTextureFilterCaps = caps.VolumeTextureFilterCaps =
      caps.TextureFilterCaps;
  return caps;
}

void Device::InitRootSignatures() {
  LOG(INFO) << "InitRootSignatures: start\n";
  std::vector<D3D12_ROOT_PARAMETER> root_params{
      {
          // Cbuffer 0: Transforms cbuffer.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 0},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
      },
      {
          // Cbuffer 1: Material cbuffer.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 1},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
      },
      {
          // CBuffer 2: Lights cbuffer. ALL (not just VERTEX) visibility --
          // LightingMode == PerPixel's generated pixel shader
          // (ff_pixel_shader.cpp) calls the same lighting.hlsl ComputeLighting
          // this cbuffer feeds, from the pixel stage instead of (or as well
          // as) the vertex stage.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 2},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
      },
      {
          // CBuffer 3: Programmable vs constants.
          .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
          .Descriptor = {.ShaderRegister = 10},
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
      },
  };
  textures_start_bindslot_ = root_params.size();
  // Add all kMaxTexStages textures.
  std::array<D3D12_DESCRIPTOR_RANGE, kMaxTexStages> srv_ranges;
  std::array<D3D12_DESCRIPTOR_RANGE, kMaxTexStages> sampler_ranges;
  for (unsigned int i = 0; i < kMaxTexStages; ++i) {
    srv_ranges[i] = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                     .NumDescriptors = 1,
                     .BaseShaderRegister = i,
                     .OffsetInDescriptorsFromTableStart = 0};
    sampler_ranges[i] = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
                         .NumDescriptors = 1,
                         .BaseShaderRegister = i};
    root_params.push_back(D3D12_ROOT_PARAMETER{
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
        .DescriptorTable = {.NumDescriptorRanges = 1,
                            .pDescriptorRanges = &srv_ranges[i]},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    });
  }
  // And all samplers.
  for (unsigned int i = 0; i < kMaxTexStages; ++i) {
    root_params.push_back(D3D12_ROOT_PARAMETER{
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
        .DescriptorTable = {.NumDescriptorRanges = 1,
                            .pDescriptorRanges = &sampler_ranges[i]},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    });
  }

  D3D12_ROOT_SIGNATURE_DESC sig_desc{
      .NumParameters = static_cast<UINT>(root_params.size()),
      .pParameters = root_params.data(),
      .NumStaticSamplers = 0,
      .pStaticSamplers = nullptr,
      .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};

  LOG(INFO) << "InitRootSignatures: D3D12SerializeRootSignature\n";
  ComPtr<ID3DBlob> sig_blob, error_blob;
  HRESULT hr = D3D12SerializeRootSignature(
      &sig_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, sig_blob.GetForInit(),
      error_blob.GetForInit());
  if (hr != S_OK) {
    FAIL("Could not create root signature:\r\n%s",
         (const char *)error_blob->GetBufferPointer());
  }

  LOG(INFO) << "InitRootSignatures: CreateRootSignature\n";
  ASSERT_HR(d3d12_device_->CreateRootSignature(
      0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
      IID_PPV_ARGS(main_root_sig_.GetForInit())));

  // Create the cbuffers.
  LOG(INFO) << "InitRootSignatures: creating cbuffers\n";
  vs_cbuffer_ = ComOwn(new DynamicBuffer());
  vs_cbuffer_->InitAsBuffer(this, sizeof(VertexCBuffer), Dx8::Usage::Dynamic,
                            D3DPOOL_SYSTEMMEM);
  lights_cbuffer_ = ComOwn(new DynamicBuffer());
  lights_cbuffer_->InitAsBuffer(this, sizeof(LightsCBuffer),
                                Dx8::Usage::Dynamic, D3DPOOL_SYSTEMMEM);
  ps_cbuffer_ = ComOwn(new DynamicBuffer());
  ps_cbuffer_->InitAsBuffer(this, sizeof(PixelCBuffer), Dx8::Usage::Dynamic,
                            D3DPOOL_SYSTEMMEM);

  vs_creg_cbuffer_ = ComOwn(new DynamicBuffer());
  vs_creg_cbuffer_->InitAsBuffer(this, sizeof(float[4]) * kNumVsConstRegs,
                                 Dx8::Usage::Dynamic, D3DPOOL_SYSTEMMEM);
  bound_vs_cregs_.resize(kNumVsConstRegs);

  ps_creg_cbuffer_ = ComOwn(new DynamicBuffer());
  ps_creg_cbuffer_->InitAsBuffer(this, sizeof(float[4]) * kNumPsConstRegs,
                                 Dx8::Usage::Dynamic, D3DPOOL_SYSTEMMEM);
  LOG(INFO) << "InitRootSignatures: done\n";
}

HRESULT STDMETHODCALLTYPE Device::GetDeviceCaps(D3DCAPS8 *pCaps) {
  *pCaps = caps_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::TestCooperativeLevel() { return S_OK; }

UINT STDMETHODCALLTYPE Device::GetAvailableTextureMem() {
  // Real drivers report actual free VRAM; we don't track GPU memory usage,
  // so report a generously large fixed budget. Games generally treat this as
  // a rough quality/streaming heuristic, not an exact figure.
  // Match the D3D8->D3D11 port.  A small fixed budget makes Vice City's
  // RenderWare streamer evict/reload mip levels aggressively; the shim does
  // not actually manage a 256 MiB VRAM budget, so report the conventional
  // unknown/very-large value instead.
  return UINT_MAX;
}

HRESULT STDMETHODCALLTYPE Device::GetCreationParameters(
    D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  // CreateDevice (direct3d8.cpp) asserts DeviceType == D3DDEVTYPE_HAL and
  // BehaviorFlags == D3DCREATE_HARDWARE_VERTEXPROCESSING on every call, so
  // those are safe to report as constants here.
  *pParameters = D3DDEVICE_CREATION_PARAMETERS{
      .AdapterOrdinal = static_cast<UINT>(adapter_index_),
      .DeviceType = D3DDEVTYPE_HAL,
      .hFocusWindow = window_,
      .BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING};
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetCursorProperties(
    UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) {
  // Games render their own software cursor almost universally; the
  // hardware-cursor bitmap itself isn't wired up, but accepting the call
  // instead of aborting is enough for the common case.
  (void)XHotSpot;
  (void)YHotSpot;
  (void)pCursorBitmap;
  return S_OK;
}

void STDMETHODCALLTYPE Device::SetCursorPosition(int X, int Y, DWORD Flags) {
  (void)Flags;
  SetCursorPos(X, Y);
}

BOOL STDMETHODCALLTYPE Device::ShowCursor(BOOL bShow) {
  // IDirect3DDevice8::ShowCursor returns the *previous* visibility state,
  // unlike Win32's ShowCursor (a display counter, not idempotent) -- track
  // our own boolean and only touch the Win32 counter on an actual change so
  // repeated same-value calls don't drift it.
  BOOL previous = cursor_visible_;
  if (static_cast<bool>(bShow) != cursor_visible_) {
    ::ShowCursor(bShow);
    cursor_visible_ = bShow;
  }
  return previous;
}

HRESULT STDMETHODCALLTYPE
Device::GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type,
                      IDirect3DSurface8 **ppBackBuffer) {
  TRACE_ENTRY(Type, ppBackBuffer);
  ASSERT(Type == D3DBACKBUFFER_TYPE_MONO);
  ASSERT(BackBuffer == 0);
  ASSERT(ppBackBuffer);
  // COM identity: see the comment on cached_backbuffer_surface_ (device.h).
  void *key = back_buffers_[0].get();
  if (!cached_backbuffer_surface_ || cached_backbuffer_surface_key_ != key) {
    cached_backbuffer_surface_ = ComOwn<BaseSurface>(
        new BackbufferSurface(this, BackBuffer, back_buffers_[0].get()));
    cached_backbuffer_surface_key_ = key;
  }
  cached_backbuffer_surface_->AddRef();
  *ppBackBuffer = cached_backbuffer_surface_.get();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface) {
  TRACE_ENTRY(ppZStencilSurface);
  void *key = depth_stencil_tex_.Get();
  if (!cached_depth_stencil_surface_ ||
      cached_depth_stencil_surface_key_ != key) {
    cached_depth_stencil_surface_ =
        ComOwn<BaseSurface>(new GpuSurface(this, depth_stencil_tex_.Get(), 0));
    cached_depth_stencil_surface_key_ = key;
  }
  cached_depth_stencil_surface_->AddRef();
  *ppZStencilSurface = cached_depth_stencil_surface_.get();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateTexture(UINT Width, UINT Height,
                                                UINT Levels, DWORD Usage,
                                                D3DFORMAT Format, D3DPOOL Pool,
                                                IDirect3DTexture8 **ppTexture) {
  TRACE_ENTRY(Width, Height, Levels, Usage, Format, Pool, ppTexture);
  *ppTexture = BaseTexture::Create(this, TextureKind::Texture2d, Width, Height,
                                   1, Levels, Usage, Format, Pool);
  // BaseTexture::Create's only silent-failure path is an invalid usage/pool
  // combo (D3DUSAGE_DYNAMIC without D3DPOOL_DEFAULT). Was previously
  // `return *ppTexture != nullptr;` -- inverted: that's 0 (S_OK) exactly
  // when creation *failed* (null) and a nonzero/failure-looking value when
  // it *succeeded*, so a caller checking SUCCEEDED()/FAILED() on the
  // returned HRESULT could never actually detect either outcome correctly
  // (0 and 1 both satisfy SUCCEEDED()) -- a game could easily end up
  // treating a failed creation as successful and later binding a null
  // texture wherever it expected one.
  return *ppTexture != nullptr ? S_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture8 **ppCubeTexture) {
  ASSERT(!(Usage & D3DUSAGE_DYNAMIC));
  *ppCubeTexture =
      BaseTexture::Create(this, TextureKind::Cube, EdgeLength, EdgeLength, 6,
                          Levels, Usage, Format, Pool);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable,
    IDirect3DSurface8 **ppSurface) {
  TRACE_ENTRY(Width, Height, Format, MultiSample, Lockable, ppSurface);
  if (MultiSample != D3DMULTISAMPLE_NONE) {
    // Matches CheckDeviceMultiSampleType: the pipeline never builds a
    // multisampled PSO, so fall back to a regular single-sample target
    // instead of failing outright.
    LOG_ERROR() << "Multisampled render targets are not supported; creating "
                  "a single-sample target instead.\n";
  }
  // Render targets are always D3DPOOL_DEFAULT; Lockable only affects whether
  // the resulting surface supports Lock/Unlock. GpuSurface::LockRect already
  // handles D3DPOOL_DEFAULT via a GPU readback, so both cases work the same.
  BaseTexture *texture = BaseTexture::Create(
      this, TextureKind::Texture2d, Width, Height, 1, 1, D3DUSAGE_RENDERTARGET,
      Format, D3DPOOL_DEFAULT);
  if (!texture) return D3DERR_INVALIDCALL;
  // The app never sees the texture object directly, only the surface -- drop
  // our initial ref once the surface (created below) has taken its own, so
  // the texture's lifetime is tied purely to the surface's.
  ComPtr<BaseTexture> owned_texture = ComOwn(texture);
  return owned_texture->GetSurfaceLevel(0, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface) {
  TRACE_ENTRY(Width, Height, Format, MultiSample, ppSurface);
  if (MultiSample != D3DMULTISAMPLE_NONE) {
    LOG_ERROR() << "Multisampled depth-stencil surfaces are not supported; "
                  "creating a single-sample surface instead.\n";
  }
  BaseTexture *texture = BaseTexture::Create(
      this, TextureKind::Texture2d, Width, Height, 1, 1, D3DUSAGE_DEPTHSTENCIL,
      Format, D3DPOOL_DEFAULT);
  if (!texture) return D3DERR_INVALIDCALL;
  ComPtr<BaseTexture> owned_texture = ComOwn(texture);
  return owned_texture->GetSurfaceLevel(0, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateImageSurface(
    UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface) {
  TRACE_ENTRY(Width, Height, Format, ppSurface);
  // Image surfaces are always plain system memory (D3DPOOL_SYSTEMMEM); they
  // exist to be filled by the app and pushed to a real resource via
  // CopyRects/UpdateTexture, not to be usable as a render target or texture.
  BaseTexture *texture =
      BaseTexture::Create(this, TextureKind::Texture2d, Width, Height, 1, 1, 0,
                          Format, D3DPOOL_SYSTEMMEM);
  if (!texture) return D3DERR_INVALIDCALL;
  ComPtr<BaseTexture> owned_texture = ComOwn(texture);
  return owned_texture->GetSurfaceLevel(0, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DSwapChain8 **pSwapChain) {
  TRACE_ENTRY(pPresentationParameters, pSwapChain);
  // D3D8 lets an additional swap chain target a different window than the
  // device's primary one (D3DPRESENT_PARAMETERS::hDeviceWindow); fall back to
  // the primary window if the caller didn't specify one, matching how the
  // primary swap chain itself is created.
  HWND target_window = pPresentationParameters->hDeviceWindow
                           ? pPresentationParameters->hDeviceWindow
                           : window_;

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{
      .Width = pPresentationParameters->BackBufferWidth,
      .Height = pPresentationParameters->BackBufferHeight,
      .Format = ToFlipModelSwapChainFormat(
          DXGIFromD3DFormat(pPresentationParameters->BackBufferFormat)),
      .SampleDesc = {.Count = 1, .Quality = 0},
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = kNumBackBuffers,
      // Stretch, not DXGI_SCALING_NONE: the back buffer is sized to whatever
      // resolution the game asked for, which routinely differs from the size
      // of the window it's presenting into. NONE means "don't scale" -- DXGI
      // puts the buffer in the window's top-left corner at 1:1 and leaves the
      // rest blank, which is what made a game running at, say, 1920x1080 on a
      // 2560x1440 window render into just part of the screen. Real D3D8
      // changed the display mode for fullscreen instead, so the picture
      // always filled the screen; STRETCH is the flip-model equivalent.
      .Scaling = DXGI_SCALING_STRETCH,
      .SwapEffect = ToDxgiSwapEffect(pPresentationParameters->SwapEffect),
      .Flags = tearing_supported_
                   ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
                   : 0u,
  };
  // Logged unconditionally (not TRACE_ENTRY, which release builds compile
  // out): an extra swap chain is a rare, notable event -- each one shows up
  // as its own framerate reading in overlay tools and holds its own set of
  // back buffers -- so it's worth being able to tell from a release log
  // whether one actually got created.
  LOG(INFO) << "CreateAdditionalSwapChain: creating an additional swap chain "
               "for window "
            << target_window << ".\n";
  ComPtr<IDXGISwapChain1> swap_chain1;
  HR_OR_RETURN(dxgi_factory_->CreateSwapChainForHwnd(
      cmd_queue_.get(), target_window, &swap_chain_desc, nullptr, nullptr,
      swap_chain1.GetForInit()));
  ComPtr<IDXGISwapChain3> swap_chain3;
  ASSERT_HR(swap_chain1->QueryInterface(swap_chain3.GetForInit()));
  // See the matching comment in Init() -- opt out of DXGI's automatic
  // window monitoring for this window too.
  ASSERT_HR(dxgi_factory_->MakeWindowAssociation(
      target_window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER |
                         DXGI_MWA_NO_PRINT_SCREEN));

  std::vector<ComPtr<GpuTexture>> back_buffers;
  for (uint32_t i = 0; i < swap_chain_desc.BufferCount; ++i) {
    ComPtr<ID3D12Resource> resource;
    ASSERT_HR(
        swap_chain3->GetBuffer(i, IID_PPV_ARGS(resource.GetForInit())));
    back_buffers.push_back(ComOwn(GpuTexture::InitFromResource(this, resource)));
  }

  *pSwapChain = new AdditionalSwapChain(this, std::move(swap_chain3),
                                        std::move(back_buffers));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                           IDirect3DVertexBuffer8 **ppVertexBuffer) {
  ASSERT(!(Usage & D3DUSAGE_SOFTWAREPROCESSING));
  // Buffer *buffer = new Buffer();
  Buffer *buffer =
      HasFlag(Usage, D3DUSAGE_DYNAMIC) ? new DynamicBuffer() : new Buffer();
  buffer->InitAsVertexBuffer(this, static_cast<size_t>(Length), Usage, Pool,
                             FVF);
  *ppVertexBuffer = buffer;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format,
                          D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer) {
  ASSERT(!(Usage & D3DUSAGE_SOFTWAREPROCESSING));
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32) {
    LOG_ERROR() << "Invalid Format for CreateIndexBuffer: " << Format << "\n";
    return D3DERR_INVALIDCALL;
  }
  Buffer *buffer =
      HasFlag(Usage, D3DUSAGE_DYNAMIC) ? new DynamicBuffer() : new Buffer();
  buffer->InitAsIndexBuffer(this, static_cast<size_t>(Length), Usage, Format,
                            Pool);
  *ppIndexBuffer = buffer;
  return S_OK;
}

void Device::TransitionTexture(GpuTexture *texture, uint32_t subresource,
                               D3D12_RESOURCE_STATES state_after) {
  if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    const uint32_t count = static_cast<uint32_t>(texture->resource_desc().MipLevels) *
                           static_cast<uint32_t>(texture->resource_desc().DepthOrArraySize);
    for (uint32_t i = 0; i < count; ++i)
      TransitionTexture(texture, i, state_after);
    return;
  }
  if (texture->current_state(subresource) == state_after) return;
#ifdef DX8TO12_ENABLE_VALIDATION
  // AixLog's severity filtering happens per-sink at dispatch time, not at
  // this call site -- an unguarded LOG() here would pay full temporary-
  // object-construction and stream-formatting cost (std::hex, three
  // operator<< calls) on every single state-changing transition even when
  // the TRACE severity is filtered out and nothing ends up written, exactly
  // the cost TRACE_ENTRY already had to be gated against elsewhere in this
  // file. TransitionTexture is hot enough (called for close to every
  // resource state change, so multiple times per draw in typical scenes)
  // that this was worth gating explicitly rather than assuming the sink
  // threshold alone would make it free.
  // std::dec at the end: see the matching comment in buffer.cpp (search
  // "Using backing buffer") -- AixLog's shared std::clog streambuf means an
  // unreset std::hex here leaks into every other LOG() call process-wide,
  // even though this specific line is TRACE-level and normally filtered out
  // before ever reaching a sink.
  LOG(TRACE) << "Transitioning " << std::hex << texture << "From "
             << texture->current_state(subresource) << " to " << state_after << std::dec
             << "\n";
#endif

  D3D12_RESOURCE_BARRIER barrier{
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {.pResource = texture->resource(),
                     .Subresource = subresource,
                     .StateBefore = texture->current_state(subresource),
                     .StateAfter = state_after}};
  cmd_list_->ResourceBarrier(1, &barrier);
  texture->set_state(subresource, state_after);
  MarkResourceAsUsed(InternalPtr(texture));
#ifdef DX8TO12_ENABLE_VALIDATION
  LogBarrierStats(/*is_texture=*/true);
#endif
}

void Device::TransitionDynamicRingBuffer(D3D12_RESOURCE_STATES state_after) {
  DynamicRingBuffer *ring = dynamic_ring_buffer_.get();
  if (!ring || ring->current_state() == state_after) return;
  D3D12_RESOURCE_BARRIER barrier{
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {.pResource = ring->GetBackingResource(),
                     .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     .StateBefore = ring->current_state(),
                     .StateAfter = state_after}};
  cmd_list_->ResourceBarrier(1, &barrier);
  ring->set_state(state_after);
}

void Device::TransitionBuffer(Buffer *buffer,
                              D3D12_RESOURCE_STATES state_after) {
  if (buffer->current_state() == state_after) return;
  D3D12_RESOURCE_BARRIER barrier = CreateBufferTransition(
      buffer->resource(), buffer->current_state(), state_after);
  cmd_list_->ResourceBarrier(1, &barrier);
  buffer->set_state(state_after);
#ifdef DX8TO12_ENABLE_VALIDATION
  LogBarrierStats(/*is_texture=*/false);
#endif
}

#ifdef DX8TO12_ENABLE_VALIDATION
// DIAGNOSTIC: plan/oportowanie.md section 8.1/8.3 -- "measure barrier and
// copy counts" before considering barrier batching (section 6.1/8.3#8).
// Both TransitionTexture/TransitionBuffer already skip a no-op transition
// (same current/target state), so every call counted here is a real,
// individual ResourceBarrier submission -- exactly what batching multiple
// barriers into one ResourceBarrier() call (D3D12 accepts an array) would
// collapse, if there's enough of them clustered together to be worth it.
void Device::LogBarrierStats(bool is_texture) {
  static uint64_t tex_barriers = 0, buf_barriers = 0;
  (is_texture ? tex_barriers : buf_barriers)++;
  const uint64_t total = tex_barriers + buf_barriers;
  if (total % 5000 == 0) {
    LOG(AixLog::Severity::error)
        << "BARRIER-STATS frame=" << CurrentFrame()
        << " texBarriers=" << tex_barriers << " bufBarriers=" << buf_barriers
        << " total=" << total << "\n";
  }
}
#endif

void Device::CopyBuffer(Buffer *dest, int64_t dest_offset,
                        ID3D12Resource *src, int64_t src_offset,
                        int64_t num_bytes) {
  TransitionBuffer(dest, D3D12_RESOURCE_STATE_COPY_DEST);
  cmd_list_->CopyBufferRegion(dest->resource(),
                              static_cast<UINT64>(dest_offset), src,
                              static_cast<UINT64>(src_offset),
                              static_cast<UINT64>(num_bytes));
  TransitionBuffer(dest, D3D12_RESOURCE_STATE_COMMON);
}

void Device::CopyBufferToTexture(
    GpuTexture *dest, uint32_t dest_subresource, ID3D12Resource *src,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT src_footprint, uint32_t dest_x,
    uint32_t dest_y) {
  TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dest_location{
      .pResource = dest->resource(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = dest_subresource};
  D3D12_TEXTURE_COPY_LOCATION src_location{
      .pResource = src,
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = src_footprint};

  TransitionTexture(dest, dest_subresource, D3D12_RESOURCE_STATE_COPY_DEST);

  cmd_list_->CopyTextureRegion(&dest_location, dest_x, dest_y, 0,
                               &src_location, nullptr);
  // TODO: Transition away from copy destination back to whatever state the
  // texture was in, instead of transitioning back to common.
  TransitionTexture(dest, dest_subresource,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
  MarkResourceAsUsed(InternalPtr(dest));
  // TODO: Mark src as used as well.
}

void Device::MarkBufferForPersist(Buffer *buffer) {
  if (buffer->is_marked_for_persist()) return;
  buffer->set_marked_for_persist(true);
  buffers_to_persist_.push_back(ComWrap(buffer));
}

HRESULT STDMETHODCALLTYPE Device::CopyRects(
    IDirect3DSurface8 *pSourceSurface, CONST RECT *pSourceRectsArray,
    UINT cRects, IDirect3DSurface8 *pDestinationSurface,
    CONST POINT *pDestPointsArray) {
  TRACE_ENTRY(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface,
              pDestPointsArray);

  ASSERT(static_cast<BaseSurface *>(pDestinationSurface)->kind() ==
         SurfaceKind::Gpu);
  GpuSurface *dest_surface = static_cast<GpuSurface *>(pDestinationSurface);

  SurfaceKind source_kind = static_cast<BaseSurface *>(pSourceSurface)->kind();
  if (source_kind == SurfaceKind::Gpu ||
      source_kind == SurfaceKind::Backbuffer) {
    // GPU-to-GPU: no CPU staging needed, just a direct region copy per rect.
    GpuTexture *src_texture;
    uint32_t src_subresource;
    if (source_kind == SurfaceKind::Gpu) {
      GpuSurface *src_gpu_surface = static_cast<GpuSurface *>(pSourceSurface);
      src_texture = src_gpu_surface->texture();
      src_subresource = src_gpu_surface->subresource();
    } else {
      // Copying *out of* the backbuffer: it has to contain the frame first.
      // No-op unless the scene target is compiled in and still open.
      FlushScenePassForBackbufferRead();
      src_texture =
          static_cast<BackbufferSurface *>(pSourceSurface)->texture();
      src_subresource = 0;
    }
    const D3D12_RESOURCE_DESC &src_desc = src_texture->resource_desc();
    RECT whole_surface_rect{.left = 0,
                            .top = 0,
                            .right = static_cast<LONG>(src_desc.Width),
                            .bottom = static_cast<LONG>(src_desc.Height)};
    const bool copy_whole_surface = pSourceRectsArray == nullptr;
    const UINT num_rects = copy_whole_surface ? 1 : cRects;

    const D3D12_RESOURCE_DESC &dst_desc =
        dest_surface->texture()->resource_desc();
    // CopyRects' rect/point math throughout this function operates in raw
    // texel coordinates; block-compressed (DXT/S3TC) resources need those
    // converted to (and D3D12-required to be aligned to) 4x4-block
    // coordinates instead, which isn't implemented -- CopyRects onto/from
    // static compressed texture data essentially never happens in practice
    // (it's almost always used for render-target/backbuffer blits and
    // dynamic surface updates), so this is a loud, specific failure rather
    // than guessed-at block math that could silently corrupt the copy.
    if (IsBlockCompressedFormat(src_desc.Format) ||
        IsBlockCompressedFormat(dst_desc.Format)) {
      FAIL(
          "CopyRects: block-compressed (DXT/S3TC) source/destination "
          "surfaces are not supported.");
    }
    // D3D12 requires an exact (or explicitly-equivalent, per the small set
    // the validation layer recognizes -- BC[1|4], BC[2|3|5|6|7],
    // R9G9B9E5_SHAREDEXP) format match for a direct texture-to-texture
    // CopyTextureRegion. It rejects e.g. B8G8R8A8 <-> B8G8R8X8 even though
    // they're byte-for-byte identical layouts (alpha vs. an unused padding
    // channel) -- exactly the backbuffer-vs-render-target mismatch this hit
    // in practice. Route through an intermediate buffer in that case: a
    // texture-to-buffer copy doesn't care about the source's pixel format
    // (the destination is just raw bytes), and the following buffer-to-
    // texture copy only needs to agree with the *destination* texture's own
    // format, which it does by construction.
    const bool needs_staging = src_desc.Format != dst_desc.Format;

    const D3D12_RESOURCE_STATES src_prior_state =
        src_texture->current_state(src_subresource);
    const D3D12_RESOURCE_STATES ring_prior_state =
        dynamic_ring_buffer_->current_state();
#ifdef DX8TO12_ENABLE_VALIDATION
    // TRACE_ENTRY logs at TRACE severity, which the file sink filters out, so
    // "no debug-layer errors from the staging path" is indistinguishable from
    // "the staging path never ran". This says which. Rate-limited because the
    // path fired ~10k times in 46 seconds when it was broken.
    if (needs_staging) {
      static int staging_copies = 0;
      ++staging_copies;
      if (staging_copies <= 5 || staging_copies % 500 == 0) {
        LOG(AixLog::Severity::error)
            << "COPYRECTS-STAGING n=" << staging_copies
            << " srcFmt=" << src_desc.Format << " dstFmt=" << dst_desc.Format
            << " fromBackbuffer="
            << (source_kind == SurfaceKind::Backbuffer ? 1 : 0)
            << " ringPriorState=" << std::hex << ring_prior_state << std::dec
            << "\n";
      }
    }
#endif
    TransitionTexture(src_texture, src_subresource,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionTexture(dest_surface->texture(), dest_surface->subresource(),
                      D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst_location{
        .pResource = dest_surface->texture()->resource(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = dest_surface->subresource()};
    D3D12_TEXTURE_COPY_LOCATION src_location{
        .pResource = src_texture->resource(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = src_subresource};
    const int src_format_size = DXGIFormatSize(src_desc.Format);
    for (UINT i = 0; i < num_rects; ++i) {
      const RECT &rect =
          copy_whole_surface ? whole_surface_rect : pSourceRectsArray[i];
      const POINT dest_point = pDestPointsArray
                                    ? pDestPointsArray[i]
                                    : POINT{.x = rect.left, .y = rect.top};
      if (!needs_staging) {
        D3D12_BOX src_box{.left = static_cast<UINT>(rect.left),
                          .top = static_cast<UINT>(rect.top),
                          .front = 0,
                          .right = static_cast<UINT>(rect.right),
                          .bottom = static_cast<UINT>(rect.bottom),
                          .back = 1};
        cmd_list_->CopyTextureRegion(&dst_location,
                                     safe_cast<uint32_t>(dest_point.x),
                                     safe_cast<uint32_t>(dest_point.y), 0,
                                     &src_location, &src_box);
        continue;
      }
      const uint32_t rect_width =
          static_cast<uint32_t>(rect.right - rect.left);
      const uint32_t rect_height =
          static_cast<uint32_t>(rect.bottom - rect.top);
      const uint32_t row_bytes =
          rect_width * static_cast<uint32_t>(src_format_size);
      const uint32_t staged_row_pitch = safe_cast<uint32_t>(
          AlignUp(static_cast<int>(row_bytes),
                  D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      DynamicRingBuffer::Allocation staging_alloc =
          dynamic_ring_buffer()->Allocate(
              static_cast<size_t>(staged_row_pitch) * rect_height,
              D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      D3D12_TEXTURE_COPY_LOCATION staging_location{
          .pResource = dynamic_ring_buffer_->GetBackingResource(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
          .PlacedFootprint = {
              .Offset = safe_cast<uint64_t>(staging_alloc.offset),
              .Footprint = {.Format = src_desc.Format,
                            .Width = rect_width,
                            .Height = rect_height,
                            .Depth = 1,
                            .RowPitch = staged_row_pitch}}};
      D3D12_BOX src_box{.left = static_cast<UINT>(rect.left),
                        .top = static_cast<UINT>(rect.top),
                        .front = 0,
                        .right = static_cast<UINT>(rect.right),
                        .bottom = static_cast<UINT>(rect.bottom),
                        .back = 1};
      // The ring buffer is a copy *destination* here -- the only place in the
      // codebase where that's true. Every other use (CopyBufferToTexture,
      // Buffer's dynamic upload, the RT helper) transitions it to
      // COPY_SOURCE; without the matching COPY_DEST transition the resource
      // is still in VERTEX_AND_CONSTANT_BUFFER|INDEX_BUFFER when the copy
      // lands on it. Measured: 10434 debug-layer errors in a 46-second
      // session ("invalid for use as a destination resource"), i.e. this
      // fires constantly, not in some rare corner. It's a CUSTOM/L0 heap, so
      // it is legally transitionable -- the barriers were simply missing.
      TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
      cmd_list_->CopyTextureRegion(&staging_location, 0, 0, 0, &src_location,
                                   &src_box);
      D3D12_TEXTURE_COPY_LOCATION staging_src_location = staging_location;
      staging_src_location.PlacedFootprint.Footprint.Format = dst_desc.Format;
      TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
      cmd_list_->CopyTextureRegion(&dst_location,
                                   safe_cast<uint32_t>(dest_point.x),
                                   safe_cast<uint32_t>(dest_point.y), 0,
                                   &staging_src_location, nullptr);
    }
    if (needs_staging) {
      // Leave the ring buffer as the rest of the frame expects to find it;
      // PrepareDrawCall only transitions it back on its own next call.
      TransitionDynamicRingBuffer(ring_prior_state);
    }
    TransitionTexture(src_texture, src_subresource, src_prior_state);
    TransitionTexture(dest_surface->texture(), dest_surface->subresource(),
                      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    MarkResourceAsUsed(InternalPtr(src_texture));
    MarkResourceAsUsed(InternalPtr(dest_surface));
    return S_OK;
  }

  ASSERT(source_kind == SurfaceKind::Cpu);
  CpuSurface *source_surface = static_cast<CpuSurface *>(pSourceSurface);

  const D3D12_SUBRESOURCE_FOOTPRINT &source_footprint =
      source_surface->footprint().Footprint;
  const uint32_t compact_pitch =
      safe_cast<uint32_t>(source_surface->compact_pitch());
  // See the matching guard/comment on the GPU-source path above.
  if (IsBlockCompressedFormat(source_footprint.Format)) {
    FAIL(
        "CopyRects: block-compressed (DXT/S3TC) source surface is not "
        "supported.");
  }
  const int format_size = DXGIFormatSize(source_footprint.Format);

  // No source rects means "copy the whole surface", per the D3D8 docs; a
  // single synthetic rect covering it lets the general per-rect path below
  // handle both cases identically.
  RECT whole_surface_rect{.left = 0,
                          .top = 0,
                          .right = static_cast<LONG>(source_footprint.Width),
                          .bottom = static_cast<LONG>(source_footprint.Height)};
  const bool copy_whole_surface = pSourceRectsArray == nullptr;
  const UINT num_rects = copy_whole_surface ? 1 : cRects;

  for (UINT i = 0; i < num_rects; ++i) {
    const RECT &rect =
        copy_whole_surface ? whole_surface_rect : pSourceRectsArray[i];
    const uint32_t rect_width = static_cast<uint32_t>(rect.right - rect.left);
    const uint32_t rect_height = static_cast<uint32_t>(rect.bottom - rect.top);
    const POINT dest_point = pDestPointsArray
                                  ? pDestPointsArray[i]
                                  : POINT{.x = rect.left, .y = rect.top};

    // Allocate space in our ring buffer and move just this rect's source
    // data, row by row (the source rect's rows aren't contiguous in the
    // backing CPU surface unless the rect is the full width).
    const uint32_t row_bytes = rect_width * static_cast<uint32_t>(format_size);
    const uint32_t dest_row_pitch =
        safe_cast<uint32_t>(AlignUp(static_cast<int>(row_bytes),
                                    D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const size_t num_bytes =
        static_cast<size_t>(dest_row_pitch) * rect_height;
    DynamicRingBuffer::Allocation ring_alloc =
        dynamic_ring_buffer()->Allocate(
            num_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    char *source_ring_ptr = dynamic_ring_buffer()->GetCpuPtrFor(ring_alloc);
    const char *rect_src_ptr = source_surface->GetPtr() +
                               rect.top * compact_pitch +
                               rect.left * format_size;
    for (uint32_t row = 0; row < rect_height; ++row) {
      memcpy(source_ring_ptr + row * dest_row_pitch,
             rect_src_ptr + row * compact_pitch, row_bytes);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT src_placed_footprint{
        .Offset = safe_cast<uint64_t>(ring_alloc.offset),
        .Footprint = {.Format = source_footprint.Format,
                      .Width = rect_width,
                      .Height = rect_height,
                      .Depth = 1,
                      .RowPitch = dest_row_pitch}};

    CopyBufferToTexture(dest_surface->texture(), dest_surface->subresource(),
                        dynamic_ring_buffer_->GetBackingResource(),
                        src_placed_footprint,
                        safe_cast<uint32_t>(dest_point.x),
                        safe_cast<uint32_t>(dest_point.y));
  }

  MarkResourceAsUsed(InternalPtr(dest_surface));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture,
                      IDirect3DBaseTexture8 *pDestinationTexture) {
  TRACE_ENTRY(pSourceTexture, pDestinationTexture);
  BaseTexture *source = dynamic_cast<BaseTexture *>(pSourceTexture);
  ASSERT(source->GetSurfaceDesc(0).Pool == D3DPOOL_SYSTEMMEM);
  BaseTexture *dest = dynamic_cast<BaseTexture *>(pDestinationTexture);
  ASSERT(dest->GetSurfaceDesc(0).Pool != D3DPOOL_SYSTEMMEM);
  // Transition dest.
  TransitionTexture(static_cast<GpuTexture *>(dest),
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST);
  static_cast<CpuTexture *>(source)->CopyToGpuTexture(
      static_cast<GpuTexture *>(dest));
  // Transition dest.
  TransitionTexture(static_cast<GpuTexture *>(dest),
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
  MarkResourceAsUsed(InternalPtr(source));
  MarkResourceAsUsed(InternalPtr(dest));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetViewport(const D3DVIEWPORT8 *pViewport) {
  viewport_.TopLeftX = static_cast<float>(pViewport->X);
  viewport_.TopLeftY = static_cast<float>(pViewport->Y);
  viewport_.Width = static_cast<float>(pViewport->Width);
  viewport_.Height = static_cast<float>(pViewport->Height);
  viewport_.MinDepth = pViewport->MinZ;
  viewport_.MaxDepth = pViewport->MaxZ;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetViewport(D3DVIEWPORT8 *pViewport) {
  pViewport->X = static_cast<DWORD>(viewport_.TopLeftX);
  pViewport->Y = static_cast<DWORD>(viewport_.TopLeftY);
  pViewport->Width = static_cast<DWORD>(viewport_.Width);
  pViewport->Height = static_cast<DWORD>(viewport_.Height);
  pViewport->MinZ = viewport_.MinDepth;
  pViewport->MaxZ = viewport_.MaxDepth;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTransform(D3DTRANSFORMSTATETYPE State,
                                               CONST D3DMATRIX *pMatrix) {
  if (State > 511 || State < D3DTS_VIEW ||
      (State > D3DTS_PROJECTION && State < D3DTS_TEXTURE0)) {
    LOG_ERROR() << "Invalid SetTransform index: " << State << "\n";
    return D3DERR_INVALIDCALL;
  }
  if (State == D3DTS_VIEW) {
    // Lights are uploaded to the GPU in view-space, so we must update them if
    // the view matrix changes.
    dirty_flags_ |= DIRTY_FLAG_LIGHTS;
  }
  transforms_[State] = *pMatrix;
  dirty_flags_ |= DIRTY_FLAG_TRANSFORMS;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetTransform(D3DTRANSFORMSTATETYPE State,
                                               D3DMATRIX *pMatrix) {
  if (State > 511 || State < D3DTS_VIEW ||
      (State > D3DTS_PROJECTION && State < D3DTS_TEXTURE0)) {
    LOG_ERROR() << "Invalid SetTransform index: " << State << "\n";
    return D3DERR_INVALIDCALL;
  }
  if (transforms_.contains(State)) {
    *pMatrix = transforms_[State];
  } else {
    static DirectX::SimpleMath::Matrix identity;
    memcpy(pMatrix, &identity, sizeof(identity));
  }
  return S_OK;
}

D3DMATRIX Device::GetTransform(D3DTRANSFORMSTATETYPE state) {
  D3DMATRIX matrix;
  ASSERT_HR(GetTransform(state, &matrix));
  return matrix;
}

HRESULT STDMETHODCALLTYPE
Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State,
                          CONST D3DMATRIX *pMatrix) {
  D3DMATRIX current = GetTransform(State);
  DirectX::SimpleMath::Matrix lhs, rhs;
  memcpy(&lhs, pMatrix, sizeof(lhs));
  memcpy(&rhs, &current, sizeof(rhs));
  // Row-vector convention (matches D3D8): applying pMatrix first, then the
  // state's existing matrix.
  DirectX::SimpleMath::Matrix result = lhs * rhs;
  D3DMATRIX result_d3d;
  memcpy(&result_d3d, &result, sizeof(result_d3d));
  return SetTransform(State, &result_d3d);
}

HRESULT STDMETHODCALLTYPE Device::SetMaterial(const D3DMATERIAL8 *pMaterial) {
  material_ = *pMaterial;
  dirty_flags_ |= DIRTY_FLAG_PS_CBUFFER;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetMaterial(D3DMATERIAL8 *pMaterial) {
  *pMaterial = material_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetLight(DWORD Index,
                                           CONST D3DLIGHT8 *light) {
  lights_[Index] = *light;
  if (enabled_lights_.contains(Index)) {
    dirty_flags_ |= DIRTY_FLAG_LIGHTS;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetLight(DWORD Index, D3DLIGHT8 *light) {
  if (!lights_.contains(Index)) return D3DERR_INVALIDCALL;
  *light = lights_[Index];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetLightEnable(DWORD Index, BOOL *pEnable) {
  *pEnable = enabled_lights_.contains(Index);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::LightEnable(DWORD Index, BOOL Enable) {
  if (!lights_.contains(Index)) {
    // Create the default light if it does not already exist.
    lights_[Index] = D3DLIGHT8{.Type = D3DLIGHT_DIRECTIONAL,
                               .Diffuse = {1, 1, 1, 0},
                               .Direction = {0.f, 0.f, 1.f}};
  }
  if (Enable) {
    if (enabled_lights_.size() >= caps_.MaxActiveLights) {
      LOG_ERROR() << "Trying to enable more than " << caps_.MaxActiveLights
                  << " lights.\n";
      return D3DERR_INVALIDCALL;
    } else {
      enabled_lights_.insert(Index);
    }
  } else {
    enabled_lights_.erase(Index);
  }
  dirty_flags_ |= DIRTY_FLAG_LIGHTS;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetClipPlane(DWORD Index,
                                               CONST float *pPlane) {
  // Bookkeeping only -- no GPU-side user clip plane implementation, so this
  // does not actually affect rendering. See the clip_planes_ comment.
  if (Index >= clip_planes_.size()) return D3DERR_INVALIDCALL;
  memcpy(clip_planes_[Index].data(), pPlane, sizeof(float) * 4);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetClipPlane(DWORD Index, float *pPlane) {
  if (Index >= clip_planes_.size()) return D3DERR_INVALIDCALL;
  memcpy(pPlane, clip_planes_[Index].data(), sizeof(float) * 4);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::SetClipStatus(CONST D3DCLIPSTATUS8 *pClipStatus) {
  clip_status_ = *pClipStatus;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetClipStatus(D3DCLIPSTATUS8 *pClipStatus) {
  *pClipStatus = clip_status_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::ValidateDevice(DWORD *pNumPasses) {
#ifdef DX8TO12_VALIDATE_DEVICE_ALWAYS_FAIL
  // Compatibility A/B for GTA: Vice City's RenderWare MatFX selector.  The
  // working D3D8-to-D3D11 reference returns D3DERR_INVALIDCALL here, while our
  // unconditional one-pass success makes RenderWare select optimized texture-
  // stage combinations that this fixed-function translator has never actually
  // validated.  The GTA binary calls this method from the MatFX setup paths
  // (device vtable +0x100) and explicitly falls back when it fails.
  if (pNumPasses) *pNumPasses = 0;
  return D3DERR_INVALIDCALL;
#else
  // We never need more than a single pass to render the current state.
  *pNumPasses = 1;
  return S_OK;
#endif
}

HRESULT STDMETHODCALLTYPE Device::SetRenderState(D3DRENDERSTATETYPE State,
                                                 DWORD Value) {
  // Redundant-set early-out. RenderWare (and D3D8-era engines generally)
  // re-set the same render state value many times per frame rather than
  // tracking what's already current, so this is a very common case. Skipping
  // it avoids dirtying cbuffers that would then be re-uploaded for no reason.
  // Bitwise comparison is correct here even for the float-typed states (this
  // accessor hands them back reinterpreted as DWORD): identical bits mean an
  // identical value, and the only false negatives (e.g. -0.0f vs +0.0f) fall
  // through to the old behavior rather than skipping a real change.
  DWORD &state_slot = render_state_.GetEnumAtIndex(State);
  if (state_slot == Value) return S_OK;
  // DIAGNOSTIC: a mesh chunk was confirmed via RenderDoc's PixelHistory to
  // be legitimately backface-culled (not a missing/dropped draw call) while
  // sitting inside a run of otherwise-identical CullMode=Back draws, right
  // before a later run of the same batch switches to CullMode=NoCull -- so
  // the game clearly does vary D3DRS_CULLMODE within this sequence. This
  // logs every real change so the next repro can show whether that specific
  // draw actually got the cull mode the game had just set, or a stale one.
#ifdef DX8TO12_ENABLE_VALIDATION
  if (State == D3DRS_CULLMODE) {
    static uint64_t cullmode_seq = 0;
    ++cullmode_seq;
    LOG(AixLog::Severity::error)
        << "SETCULLMODE seq=" << cullmode_seq << " frame=" << CurrentFrame()
        << " was=" << state_slot << " now=" << Value << "\n";
  }
#endif
  state_slot = Value;
  // Every render state is part of the PSO key by default (PSOState embeds
  // the whole RenderState) -- *except* the fields CreatePSO's own key-
  // normalization block (its "Zero out/normalize..." comment) explicitly
  // resets to a fixed value before hashing/looking up the cache, because
  // they don't influence the built D3D12_GRAPHICS_PIPELINE_STATE_DESC or
  // which shader gets selected -- they're consumed at draw time from a
  // cbuffer by whichever PSO is *already* bound (see the PS_CBUFFER/LIGHTS
  // cases in the switch below). Setting DIRTY_FLAG_PSO for those anyway
  // forced a full CreatePSO hash+compare (over a kilobyte of RenderState+
  // PixelShaderState, per CreatePSO's own comment) on the next draw for a
  // PSO that was always going to come back byte-identical -- exactly the
  // kind of state a foliage/decal-heavy scene changes constantly
  // (D3DRS_ALPHAREF per-material, fog toggles, TEXTUREFACTOR). This list
  // must stay a subset of CreatePSO's normalization block, not grow beyond
  // it -- excluding something PSO-relevant here would mean a stale PSO gets
  // reused instead of rebuilt, which is a correctness bug (wrong pipeline
  // state silently applied), not just a missed optimization. When in
  // doubt, leave a state out of this list; the default below still marks
  // PSO dirty.
  switch (State) {
    case D3DRS_TEXTUREFACTOR:
    case D3DRS_AMBIENT:
    case D3DRS_DIFFUSEMATERIALSOURCE:
    case D3DRS_SPECULARMATERIALSOURCE:
    case D3DRS_AMBIENTMATERIALSOURCE:
    case D3DRS_EMISSIVEMATERIALSOURCE:
    case D3DRS_ALPHAREF:
    case D3DRS_DITHERENABLE:
    case D3DRS_FOGENABLE:
    case D3DRS_FOGCOLOR:
    case D3DRS_FOGTABLEMODE:
    case D3DRS_FOGSTART:
    case D3DRS_FOGEND:
    case D3DRS_FOGDENSITY:
    case D3DRS_RANGEFOGENABLE:
    case D3DRS_FOGVERTEXMODE:
    case D3DRS_COLORVERTEX:
    case D3DRS_STENCILREF:
    case D3DRS_LOCALVIEWER:
    case D3DRS_NORMALIZENORMALS:
      break;
    default:
      dirty_flags_ |= DIRTY_FLAG_PSO;
      break;
  }
  switch (State) {
    case D3DRS_TEXTUREFACTOR:
    case D3DRS_ALPHAREF:
    case D3DRS_FOGENABLE:
    case D3DRS_FOGCOLOR:
    case D3DRS_FOGTABLEMODE:
    case D3DRS_FOGVERTEXMODE:
    case D3DRS_FOGSTART:
    case D3DRS_FOGEND:
    case D3DRS_FOGDENSITY:
      dirty_flags_ |= DIRTY_FLAG_PS_CBUFFER;
      break;
    case D3DRS_LIGHTING:
    case D3DRS_COLORVERTEX:
    case D3DRS_DIFFUSEMATERIALSOURCE:
    case D3DRS_AMBIENTMATERIALSOURCE:
    case D3DRS_SPECULARMATERIALSOURCE:
    case D3DRS_EMISSIVEMATERIALSOURCE:
    case D3DRS_AMBIENT:
    case D3DRS_SPECULARENABLE:
    case D3DRS_NORMALIZENORMALS:
      dirty_flags_ |= DIRTY_FLAG_LIGHTS;
      break;
    default:
      break;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetRenderState(D3DRENDERSTATETYPE State,
                                                 DWORD *pValue) {
  *pValue = render_state_.GetEnumAtIndex(State);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  if (Stage >= texture_stage_states_.size()) return D3DERR_INVALIDCALL;
  *pValue = texture_stage_states_[Stage].GetAtIndex(static_cast<size_t>(Type));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  if (Stage >= texture_stage_states_.size()) return D3DERR_INVALIDCALL;
  // Redundant-set early-out -- same reasoning as SetRenderState above. This
  // one matters extra because a sampler-affecting state redundantly re-set
  // would dirty DIRTY_FLAG_PS_SAMPLERS, which costs a full 8-stage sampler
  // rebind on the next draw.
  DWORD &stage_slot =
      texture_stage_states_[Stage].GetAtIndex(static_cast<size_t>(Type));
  if (stage_slot == Value) return S_OK;
  stage_slot = Value;
  // Only the fields PixelShaderState::PixelShaderState (render_state.cpp)
  // actually copies into ts[i] feed fixed-function pixel shader generation --
  // color/alpha op+args+arg0, the texcoord index/transform flags, and the
  // result-register redirect. The addressing/filtering/anisotropy/border-
  // color/LOD-bias states below drive
  // the *sampler* only (see the D3DTSS_ADDRESSU..MAXANISOTROPY branch), never
  // shader selection, so marking DIRTY_FLAG_PSO for those forced a PSO
  // cache hash+lookup (over a kilobyte of RenderState+PixelShaderState, per
  // CreatePSO's call-site comment) on every SetTextureStageState regardless
  // of which field changed -- most commonly hit through filtering/anisotropy
  // toggles that never once change what shader a draw needs.
  switch (Type) {
    case D3DTSS_COLOROP:
    case D3DTSS_COLORARG0:
    case D3DTSS_COLORARG1:
    case D3DTSS_COLORARG2:
    case D3DTSS_ALPHAOP:
    case D3DTSS_ALPHAARG0:
    case D3DTSS_ALPHAARG1:
    case D3DTSS_ALPHAARG2:
    case D3DTSS_TEXCOORDINDEX:
    case D3DTSS_TEXTURETRANSFORMFLAGS:
    case D3DTSS_RESULTARG:
      dirty_flags_ |= DIRTY_FLAG_PSO;
      break;
    default:
      break;
  }
  if ((Type >= D3DTSS_ADDRESSU && Type <= D3DTSS_MAXANISOTROPY) ||
      Type == D3DTSS_ADDRESSW) {
    dirty_flags_ |= DIRTY_FLAG_PS_SAMPLERS;
    dirty_sampler_stage_mask_ |= (1u << Stage);
  }
  return S_OK;
}

namespace {
// Mirrors the exhaustive lists of state types handled by
// RenderState::GetEnumAtIndex / TextureStageState::GetAtIndex (excluding
// D3DRS_WRAP0..7, which are handled separately below since they're a
// contiguous range).
constexpr D3DRENDERSTATETYPE kAllRenderStateTypes[] = {
    D3DRS_ZENABLE,
    D3DRS_ZWRITEENABLE,
    D3DRS_SHADEMODE,
    D3DRS_FILLMODE,
    D3DRS_ALPHATESTENABLE,
    D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,
    D3DRS_CULLMODE,
    D3DRS_ZFUNC,
    D3DRS_ALPHAREF,
    D3DRS_ALPHAFUNC,
    D3DRS_DITHERENABLE,
    D3DRS_ALPHABLENDENABLE,
    D3DRS_FOGENABLE,
    D3DRS_SPECULARENABLE,
    D3DRS_FOGCOLOR,
    D3DRS_FOGTABLEMODE,
    D3DRS_FOGSTART,
    D3DRS_FOGEND,
    D3DRS_FOGDENSITY,
    D3DRS_EDGEANTIALIAS,
    D3DRS_ZBIAS,
    D3DRS_RANGEFOGENABLE,
    D3DRS_STENCILENABLE,
    D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL,
    D3DRS_STENCILPASS,
    D3DRS_STENCILFUNC,
    D3DRS_STENCILREF,
    D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK,
    D3DRS_TEXTUREFACTOR,
    D3DRS_LIGHTING,
    D3DRS_AMBIENT,
    D3DRS_FOGVERTEXMODE,
    D3DRS_COLORVERTEX,
    D3DRS_LOCALVIEWER,
    D3DRS_NORMALIZENORMALS,
    D3DRS_DIFFUSEMATERIALSOURCE,
    D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE,
    D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_POINTSIZE,
    D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSCALE_A,
    D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C,
    D3DRS_MULTISAMPLEANTIALIAS,
    D3DRS_POINTSIZE_MAX,
    D3DRS_COLORWRITEENABLE,
    D3DRS_BLENDOP,
    D3DRS_CLIPPING,
    D3DRS_CLIPPLANEENABLE,
    D3DRS_LASTPIXEL,
    D3DRS_LINEPATTERN,
    D3DRS_ZVISIBLE,
    D3DRS_SOFTWAREVERTEXPROCESSING,
    D3DRS_MULTISAMPLEMASK,
    D3DRS_PATCHEDGESTYLE,
    D3DRS_PATCHSEGMENTS,
    D3DRS_DEBUGMONITORTOKEN,
    D3DRS_VERTEXBLEND,
    D3DRS_INDEXEDVERTEXBLENDENABLE,
    D3DRS_TWEENFACTOR,
    D3DRS_POSITIONORDER,
    D3DRS_NORMALORDER,
    D3DRS_WRAP0,
    D3DRS_WRAP1,
    D3DRS_WRAP2,
    D3DRS_WRAP3,
    D3DRS_WRAP4,
    D3DRS_WRAP5,
    D3DRS_WRAP6,
    D3DRS_WRAP7,
};

constexpr D3DTEXTURESTAGESTATETYPE kAllTextureStageStateTypes[] = {
    D3DTSS_COLOROP,          D3DTSS_COLORARG1,      D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP,          D3DTSS_ALPHAARG1,      D3DTSS_ALPHAARG2,
    D3DTSS_TEXCOORDINDEX,    D3DTSS_ADDRESSU,       D3DTSS_ADDRESSV,
    D3DTSS_BORDERCOLOR,      D3DTSS_MAGFILTER,      D3DTSS_MINFILTER,
    D3DTSS_MIPFILTER,        D3DTSS_MIPMAPLODBIAS,  D3DTSS_MAXANISOTROPY,
    D3DTSS_TEXTURETRANSFORMFLAGS,                   D3DTSS_ADDRESSW,
    D3DTSS_COLORARG0,        D3DTSS_ALPHAARG0,      D3DTSS_RESULTARG,
    D3DTSS_BUMPENVMAT00,     D3DTSS_BUMPENVMAT01,   D3DTSS_BUMPENVMAT10,
    D3DTSS_BUMPENVMAT11,     D3DTSS_BUMPENVLSCALE,  D3DTSS_BUMPENVLOFFSET,
};
}  // namespace

Device::StateSnapshot Device::CaptureCurrentState() const {
  return StateSnapshot{
      .render_state = render_state_,
      .texture_stage_states = texture_stage_states_,
      .transforms = transforms_,
      .material = material_,
      .lights = lights_,
      .enabled_lights = enabled_lights_,
      .bound_textures = bound_textures_,
      .bound_vertex_shader = bound_vertex_shader_,
      .bound_pixel_shader = bound_pixel_shader_,
      .bound_vs_cregs = bound_vs_cregs_,
  };
}

Device::StateBlock Device::CaptureFullStateBlock() const {
  StateBlock block;
  RenderState rs = render_state_;
  for (D3DRENDERSTATETYPE type : kAllRenderStateTypes) {
    block.render_state[type] = rs.GetEnumAtIndex(type);
  }
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    TextureStageState tss = texture_stage_states_[stage];
    for (D3DTEXTURESTAGESTATETYPE type : kAllTextureStageStateTypes) {
      block.texture_stage_states[stage][type] =
          tss.GetAtIndex(static_cast<size_t>(type));
    }
  }
  block.transforms = transforms_;
  block.material = material_;
  block.lights = lights_;
  block.enabled_lights = enabled_lights_;
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    block.bound_textures[stage] = bound_textures_[stage];
  }
  block.bound_vertex_shader = bound_vertex_shader_;
  block.bound_pixel_shader = bound_pixel_shader_;
  for (UINT i = 0; i < bound_vs_cregs_.size(); ++i) {
    block.bound_vs_cregs[i] = bound_vs_cregs_[i];
  }
  return block;
}

Device::StateBlock Device::CaptureStateBlockDelta(
    const StateSnapshot &before) const {
  StateBlock block;

  RenderState before_rs = before.render_state;
  RenderState after_rs = render_state_;
  for (D3DRENDERSTATETYPE type : kAllRenderStateTypes) {
    DWORD before_value = before_rs.GetEnumAtIndex(type);
    DWORD after_value = after_rs.GetEnumAtIndex(type);
    if (before_value != after_value) block.render_state[type] = after_value;
  }

  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    TextureStageState before_tss = before.texture_stage_states[stage];
    TextureStageState after_tss = texture_stage_states_[stage];
    for (D3DTEXTURESTAGESTATETYPE type : kAllTextureStageStateTypes) {
      DWORD before_value = before_tss.GetAtIndex(static_cast<size_t>(type));
      DWORD after_value = after_tss.GetAtIndex(static_cast<size_t>(type));
      if (before_value != after_value)
        block.texture_stage_states[stage][type] = after_value;
    }
  }

  for (const auto &[type, matrix] : transforms_) {
    auto before_it = before.transforms.find(type);
    if (before_it == before.transforms.end() ||
        memcmp(&before_it->second, &matrix, sizeof(matrix)) != 0) {
      block.transforms[type] = matrix;
    }
  }

  if (memcmp(&before.material, &material_, sizeof(material_)) != 0) {
    block.material = material_;
  }

  for (const auto &[index, light] : lights_) {
    auto before_it = before.lights.find(index);
    if (before_it == before.lights.end() ||
        memcmp(&before_it->second, &light, sizeof(light)) != 0) {
      block.lights[index] = light;
    }
  }

  if (before.enabled_lights != enabled_lights_) {
    block.enabled_lights = enabled_lights_;
  }

  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    if (!(before.bound_textures[stage] == bound_textures_[stage])) {
      block.bound_textures[stage] = bound_textures_[stage];
    }
  }

  if (before.bound_vertex_shader != bound_vertex_shader_) {
    block.bound_vertex_shader = bound_vertex_shader_;
  }
  if (before.bound_pixel_shader != bound_pixel_shader_) {
    block.bound_pixel_shader = bound_pixel_shader_;
  }

  for (UINT i = 0; i < bound_vs_cregs_.size(); ++i) {
    const auto &after_value = bound_vs_cregs_[i];
    if (i >= before.bound_vs_cregs.size() ||
        before.bound_vs_cregs[i] != after_value) {
      block.bound_vs_cregs[i] = after_value;
    }
  }

  return block;
}

void Device::ApplyState(const StateBlock &block) {
  for (const auto &[type, value] : block.render_state) {
    render_state_.GetEnumAtIndex(type) = value;
  }
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    for (const auto &[type, value] : block.texture_stage_states[stage]) {
      texture_stage_states_[stage].GetAtIndex(static_cast<size_t>(type)) =
          value;
    }
  }
  for (const auto &[type, matrix] : block.transforms) {
    transforms_[type] = matrix;
  }
  if (block.material) material_ = *block.material;
  for (const auto &[index, light] : block.lights) {
    lights_[index] = light;
  }
  if (block.enabled_lights) enabled_lights_ = *block.enabled_lights;
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    if (block.bound_textures[stage]) {
      bound_textures_[stage] = *block.bound_textures[stage];
    }
  }
  if (block.bound_vertex_shader) bound_vertex_shader_ = *block.bound_vertex_shader;
  if (block.bound_pixel_shader) bound_pixel_shader_ = *block.bound_pixel_shader;
  for (const auto &[index, value] : block.bound_vs_cregs) {
    bound_vs_cregs_.at(index) = value;
  }
  // Force everything above to actually get re-bound/re-uploaded before the
  // next draw call, since we just changed it out from under the renderer.
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
  // DIRTY_FLAG_ALL_RESOURCES alone is not enough for textures/samplers: the
  // actual root-descriptor-table rebind loop (PrepareDrawCall) gates on the
  // separate dirty_texture_stage_mask_/dirty_sampler_stage_mask_ bitmasks
  // (added later than this function, to fix a different bug -- see their
  // comment in device.h), which SetTexture/SetTextureStageState keep in
  // sync but this function never touched. bound_textures_/
  // texture_stage_states_ just got reassigned directly above, completely
  // bypassing both of those setters, so without this the two masks stay
  // however a *previous*, unrelated draw last left them -- typically all
  // zero, right after any prior draw's own rebind cleared them. The dirty
  // *flag* would then be set, but the rebind loop's own per-stage mask check
  // would skip every single stage, so bound_textures_ ends up logically
  // correct while the GPU-visible descriptor tables silently keep pointing
  // at whatever was bound before this call -- confirmed via RenderDoc as the
  // cause of a UI panel rendering with the player's clothing/skin texture
  // instead of its own (that panel's real texture binding was restored via
  // a state block, exactly this path).
  dirty_texture_stage_mask_ = 0xFF;
  dirty_sampler_stage_mask_ = 0xFF;
}

HRESULT STDMETHODCALLTYPE Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                                   DWORD *pToken) {
  // Simplification: always captures every state regardless of Type
  // (D3DSBT_ALL/D3DSBT_PIXELSTATE/D3DSBT_VERTEXSTATE don't get the precise
  // real-D3D8 partitioning), but -- unlike Begin/EndStateBlock below --
  // capturing everything really is correct semantics for this API: it snapshots
  // the live state at this exact point in time to restore later.
  *pToken = next_state_block_token_++;
  state_blocks_[*pToken] = CaptureFullStateBlock();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::BeginStateBlock() {
  if (recording_state_block_) return D3DERR_INVALIDCALL;
  recording_state_block_ = true;
  state_block_recording_start_ = CaptureCurrentState();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::EndStateBlock(DWORD *pToken) {
  if (!recording_state_block_) return D3DERR_INVALIDCALL;
  recording_state_block_ = false;
  // Only the states actually Set() (i.e. changed) during the Begin/End
  // window are captured, matching real D3D8 semantics -- ApplyStateBlock()
  // must not clobber unrelated state a game changed in between recording and
  // applying the block.
  *pToken = next_state_block_token_++;
  state_blocks_[*pToken] = CaptureStateBlockDelta(state_block_recording_start_);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::ApplyStateBlock(DWORD Token) {
  auto it = state_blocks_.find(Token);
  if (it == state_blocks_.end()) return D3DERR_INVALIDCALL;
  ApplyState(it->second);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CaptureStateBlock(DWORD Token) {
  auto it = state_blocks_.find(Token);
  if (it == state_blocks_.end()) return D3DERR_INVALIDCALL;
  // Per the D3D8 docs, this refreshes the values of the states already
  // recorded in this block from the current live state -- it does not add
  // or remove which states are tracked.
  StateBlock &block = it->second;
  RenderState live_rs = render_state_;
  for (auto &[type, value] : block.render_state) {
    value = live_rs.GetEnumAtIndex(type);
  }
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    TextureStageState live_tss = texture_stage_states_[stage];
    for (auto &[type, value] : block.texture_stage_states[stage]) {
      value = live_tss.GetAtIndex(static_cast<size_t>(type));
    }
  }
  for (auto &[type, matrix] : block.transforms) {
    matrix = transforms_.contains(type) ? transforms_.at(type) : D3DMATRIX{};
  }
  if (block.material) block.material = material_;
  for (auto &[index, light] : block.lights) {
    if (lights_.contains(index)) light = lights_.at(index);
  }
  if (block.enabled_lights) block.enabled_lights = enabled_lights_;
  for (int stage = 0; stage < kMaxTexStages; ++stage) {
    if (block.bound_textures[stage]) {
      block.bound_textures[stage] = bound_textures_[stage];
    }
  }
  if (block.bound_vertex_shader) block.bound_vertex_shader = bound_vertex_shader_;
  if (block.bound_pixel_shader) block.bound_pixel_shader = bound_pixel_shader_;
  for (auto &[index, value] : block.bound_vs_cregs) {
    value = bound_vs_cregs_.at(index);
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DeleteStateBlock(DWORD Token) {
  if (state_blocks_.erase(Token) == 0) return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTexture(DWORD Stage,
                                             IDirect3DBaseTexture8 *pTexture) {
  TRACE_ENTRY(Stage, pTexture);
  if (Stage >= bound_textures_.size()) return D3DERR_INVALIDCALL;
  // Profiled hot path (WPA CPU sampling, see the session that added this
  // comment): SetTexture alone accounted for more sampled CPU time than
  // DrawPrimitive and SetStreamSource combined, almost entirely from doing
  // *two* separate dynamic_casts of the same pointer. BaseTexture multiply-
  // inherits IDirect3DTexture8/IDirect3DCubeTexture8 (both of which derive
  // from IDirect3DBaseTexture8), so casting from the IDirect3DBaseTexture8*
  // the app hands us requires a real dynamic_cast (a static_cast across that
  // diamond isn't well-defined) -- but that's only true for *this* first
  // step. BaseTexture -> GpuTexture is a plain single-inheritance downcast
  // (same pattern already used in CopyRects below), so reuse the one
  // dynamic_cast's result via static_cast instead of paying for RTTI twice.
  BaseTexture *base_texture = dynamic_cast<BaseTexture *>(pTexture);
  if (base_texture) {
    ASSERT(base_texture->GetSurfaceDesc(0).Pool != D3DPOOL_SYSTEMMEM);
  }
  GpuTexture *texture = static_cast<GpuTexture *>(base_texture);
  // Redundant-set early-out. Re-binding the texture that's already bound to
  // this stage would otherwise dirty DIRTY_FLAG_PS_TEXTURES for no change at
  // all. Note InternalPtr::Get() asserts non-null, and both sides are
  // legitimately null routinely here (a game unbinding an already-unbound
  // stage), so read the current binding through the bool conversion rather
  // than Get().
  GpuTexture *const current_texture =
      bound_textures_[Stage] ? bound_textures_[Stage].Get() : nullptr;
  // Count the RenderWare material branch even for a redundant SetTexture.
  // The game's own call is the evidence we need; whether this shim can skip
  // re-applying an identical binding is a separate implementation detail.
#ifdef DX8TO12_ENABLE_MINDEBUG
  if (Stage == 0)
    RecordRwTextureCall(pTexture != nullptr, texture != nullptr);
#endif
  if (current_texture == texture) return S_OK;
  bound_textures_[Stage] = InternalPtr(texture);
  dirty_flags_ |= DIRTY_FLAG_PS_TEXTURES;
  dirty_texture_stage_mask_ |= (1u << Stage);
  // Whether a stage *has* a texture at all feeds PixelShaderState (via
  // stage_has_texture -- see its constructor in render_state.cpp), which is
  // part of the PSO key. Swapping which specific texture is bound does not:
  // the generated pixel shader only branches on presence/absence per stage,
  // never on which texture object or format is there. Gating DIRTY_FLAG_PSO
  // on that presence/absence actually flipping -- rather than setting it on
  // every SetTexture unconditionally -- matters because it also drives
  // CreatePSO's full RenderState+PixelShaderState hash/lookup (over a
  // kilobyte, per the comment at its call site), and a scene with many
  // objects swapping between different *already-bound-somewhere* textures
  // every draw (traffic, pedestrians) was paying that on every single one
  // for a PSO that, in the overwhelmingly common case, was already cached
  // and unchanged.
  if (!current_texture != !texture) dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetTexture(DWORD Stage,
                                             IDirect3DBaseTexture8 **ppTexture) {
  if (Stage >= bound_textures_.size()) return D3DERR_INVALIDCALL;
  // InternalPtr::Get() asserts non-null; an unbound stage (no texture ever
  // SetTexture'd there, or explicitly cleared) is a routine, legitimate
  // state, not a caller error, so check via the bool conversion first --
  // same reasoning as the identical fix already applied in SetTexture.
  GpuTexture *texture =
      bound_textures_[Stage] ? bound_textures_[Stage].Get() : nullptr;
  *ppTexture = static_cast<IDirect3DTexture8 *>(texture);
  if (texture) texture->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetRenderTarget(
    IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil) {
  if (pRenderTarget) {
    SCOPED_MARKER("SetRenderTarget");
    if (bound_render_target_) {
      // Transition out of render target into common.
      TransitionTexture(bound_render_target_.Get(), 0,
                        D3D12_RESOURCE_STATE_COMMON);
    }

    BaseSurface *base_surface = static_cast<BaseSurface *>(pRenderTarget);
    GpuTexture *texture = nullptr;
    D3D12_RESOURCE_DESC resource_desc = {};
    switch (base_surface->kind()) {
      case SurfaceKind::Gpu:
        texture = static_cast<GpuSurface *>(base_surface)->texture();
        resource_desc = texture->resource_desc();
        // Real D3D8 games commonly render to an off-screen target with a
        // different format than the backbuffer (radar map, menu blur/
        // reflection effects, etc.) -- CreatePSO reads the *actual* bound
        // render target's format (see its RTVFormats comment) rather than
        // assuming it always matches the backbuffer, so this no longer needs
        // to be true.
        TransitionTexture(texture, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        break;
      case SurfaceKind::Backbuffer:
        ASSERT(static_cast<BackbufferSurface *>(base_surface)->index() == 0);
        texture = nullptr;
        resource_desc = back_buffers_.at(0)->resource_desc();
        break;
      case SurfaceKind::Cpu:
        LOG_ERROR() << "Cannot set SYSTEMMEM surface as render target.\n";
        return D3DERR_INVALIDCALL;
    }
    bound_render_target_ = InternalPtr(texture);
    // See bound_render_target_generation_ (device.h) -- this is what
    // GetRenderTarget's cache keys off instead of the texture pointer
    // itself, to avoid an ABA hazard from a freed-and-reused GpuTexture
    // address.
    ++bound_render_target_generation_;

    // Reset viewport to the size of this one.
    D3DVIEWPORT8 viewport{.Width = safe_cast<DWORD>(resource_desc.Width),
                          .Height = resource_desc.Height,
                          .MaxZ = 1.f};
    ASSERT_HR(SetViewport(&viewport));
  }
  if (pNewZStencil) {
    SCOPED_MARKER("SetDepthTarget");
    BaseSurface *base_surface = dynamic_cast<BaseSurface *>(pNewZStencil);
    GpuTexture *texture = nullptr;
    switch (base_surface->kind()) {
      case SurfaceKind::Gpu:
        texture = static_cast<GpuSurface *>(base_surface)->texture();
        // A custom depth-stencil surface (from CreateDepthStencilSurface) is
        // as legitimate a target here as the implicit one -- e.g. paired
        // with an off-screen color render target for a menu/mirror/reflection
        // effect. Games routinely bind these together, and the previous
        // ASSERT below only ever allowed depth_stencil_tex_ itself, hard-
        // failing on any such effect. GpuTexture already starts in
        // DEPTH_WRITE for D3DUSAGE_DEPTHSTENCIL textures, but transition
        // explicitly (a no-op if already correct) in case it was reused for
        // something else in between.
        TransitionTexture(texture, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        break;
      case SurfaceKind::Backbuffer:
        ASSERT(static_cast<BackbufferSurface *>(base_surface)->index() == 0);
        texture = depth_stencil_tex_.Get();
        break;
      case SurfaceKind::Cpu:
        LOG_ERROR() << "Cannot set SYSTEMMEM surface as render target.\n";
        return D3DERR_INVALIDCALL;
    }
    bound_depth_target_ = InternalPtr(texture);
  } else {
    bound_depth_target_.Reset();
  }
  dirty_flags_ |= DIRTY_FLAG_OM;
  // Render target / depth target formats are part of the PSO key.
  dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Device::GetRenderTarget(IDirect3DSurface8 **ppRenderTarget) {
  const uint64_t key = bound_render_target_generation_;
  if (!cached_render_target_surface_ || cached_render_target_surface_key_ != key) {
    if (bound_render_target_) {
      cached_render_target_surface_ = ComOwn<BaseSurface>(
          new GpuSurface(this, bound_render_target_.Get(), 0));
    } else {
      cached_render_target_surface_ = ComOwn<BaseSurface>(
          new BackbufferSurface(this, 0, back_buffers_[0].get()));
    }
    cached_render_target_surface_key_ = key;
  }
  cached_render_target_surface_->AddRef();
  *ppRenderTarget = cached_render_target_surface_.get();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexShader(const DWORD *pDeclaration,
                                                     const DWORD *pFunction,
                                                     DWORD *pHandle,
                                                     DWORD Usage) {
  auto decl = ParseShaderDeclaration(pDeclaration);

  VertexShader shader;
  if (pFunction == nullptr) {
    shader = CreateFixedFunctionVertexShader(viewport_, 0, decl);
  } else {
    shader = ParseProgrammableVertexShader(decl, pFunction);
  }

  // Keep a copy of the original declaration token stream for
  // GetVertexShaderDeclaration. Cap the scan for the D3DVSD_END() terminator
  // so a malformed/corrupt declaration produces a clear, logged failure
  // instead of walking off into unmapped memory looking for a token that
  // isn't there.
  static constexpr ptrdiff_t kMaxDeclarationTokens = 512;
  const DWORD *decl_end = pDeclaration;
  while (*decl_end != D3DVSD_END()) {
    if (decl_end - pDeclaration >= kMaxDeclarationTokens) {
      FAIL(
          "CreateVertexShader: declaration token stream did not terminate "
          "with D3DVSD_END() within %td tokens; pDeclaration=%p is likely "
          "invalid.",
          kMaxDeclarationTokens, pDeclaration);
    }
    ++decl_end;
  }
  ++decl_end;  // Include the END token itself.
  shader.declaration_tokens.assign(pDeclaration, decl_end);

  ASSERT(next_shader_handle_ < UINT32_MAX);
  DWORD handle = next_shader_handle_++;
  ASSERT(handle >= kFirstShaderHandle);
  vertex_shaders_[handle] = InternalPtr(new VertexShader(std::move(shader)));
  *pHandle = handle;

  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreatePixelShader(const DWORD *pFunction,
                                                    DWORD *pHandle) {
  if (!pFunction) return D3DERR_INVALIDCALL;
  PixelShader shader = ParsePixelShader(pFunction);
  ASSERT(next_shader_handle_ < UINT32_MAX);
  *pHandle = next_shader_handle_++;
  pixel_shaders_[*pHandle] = InternalPtr(new PixelShader(std::move(shader)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DeleteVertexShader(DWORD Handle) {
  ASSERT(Handle >= kFirstShaderHandle);
  auto found = vertex_shaders_.erase(Handle);
  ASSERT(found != 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DeletePixelShader(DWORD Handle) {
  auto found = pixel_shaders_.erase(Handle);
  ASSERT(found != 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShader(DWORD handle) {
  if (handle < kFirstShaderHandle) {
    // This is a fixed-function shader. Create it if it does not already
    // exist.
    if (!vertex_shaders_.contains(handle)) {
      vertex_shaders_[handle] =
          InternalPtr(new VertexShader(CreateFixedFunctionVertexShader(
              viewport_, handle,
              VertexShaderDeclaration::CreateFromFVFDesc(handle))));
    }
  } else {
    ASSERT(vertex_shaders_.contains(handle));
  }
  bound_vertex_shader_ = handle;
  dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShader(DWORD *pHandle) {
  *pHandle = bound_vertex_shader_;
  return S_OK;
}

void Device::InvalidatePsoCache() {
  // Wait for work already on the GPU before freeing anything. Command lists
  // that have been submitted still reference the pipeline states they were
  // recorded with, and releasing one out from under the GPU crashes inside
  // the display driver -- observed as an access violation in nvwgf2um.dll
  // called from D3D12Core, moments after this was first wired to a hotkey.
  //
  // Not WaitForFrame(): with the command list open that re-enters
  // SubmitAndWait. This waits directly on the last submitted frame, which is
  // the only thing that can still hold these.
  if (next_fence_ > 1 && cmd_list_done_fence_ &&
      cmd_list_done_fence_->GetCompletedValue() < next_fence_ - 1) {
    ASSERT_HR(cmd_list_done_fence_->SetEventOnCompletion(
        next_fence_ - 1, cmd_list_done_event_handle_));
    WaitForSingleObjectEx(cmd_list_done_event_handle_, 5000, FALSE);
  }
  pso_cache_.clear();
  // The cached "currently set" PSO pointer now names a freed object, and the
  // next draw must not compare against it.
  last_pso_.Reset();
  last_set_pso_ = nullptr;
  dirty_flags_ |= DIRTY_FLAG_PSO;
  LOG(AixLog::Severity::info) << "InvalidatePsoCache: cleared.\n";
}

void Device::OnLightingModeChanged() {
  // Every already-compiled fixed-function vertex shader was built with the
  // old mode's PER_PIXEL_LIGHTING define (or lack of it) baked in as a
  // compile-time #if, not a runtime branch -- there's no way to fix one up in
  // place, it has to be recompiled. Replaced rather than erased: CreatePSO
  // does `vertex_shaders_.at(bound_vertex_shader_)` on every draw and
  // SetVertexShader only recreates a handle it doesn't already have, so
  // erasing entries here would leave a currently-bound low handle missing
  // until the app happened to call SetVertexShader again -- crashing the very
  // next draw in between. Regenerating in place keeps every existing handle
  // valid throughout.
  for (auto &[handle, shader] : vertex_shaders_) {
    if (handle >= kFirstShaderHandle) continue;  // Programmable, not FF.
    auto fresh = CreateFixedFunctionVertexShader(
        viewport_, handle, VertexShaderDeclaration::CreateFromFVFDesc(handle));
    // Field-by-field, not VertexShader's (implicit) assignment operator:
    // VertexShader inherits RefCounted's live ref-count state, which must
    // stay this object's own -- it's still the same InternalPtr slot every
    // other draw call's bound_vertex_shader_ points at -- rather than being
    // overwritten by `fresh`'s (a separate, freshly-constructed object).
    VertexShader *existing = shader.Get();
    existing->decl = std::move(fresh.decl);
    existing->blob = std::move(fresh.blob);
    existing->fvf_desc = fresh.fvf_desc;
    existing->unique_id = fresh.unique_id;
    // declaration_tokens/function_tokens: both empty for every
    // fixed-function shader (see VertexShader::declaration_tokens' comment),
    // so existing's (already empty) are already correct -- nothing to copy.
  }
  // Fixed-function pixel shaders, unlike the above, aren't looked up by a
  // handle anything else holds onto between draws -- CreatePSO derives their
  // cache key (PixelShaderState) fresh from render_state_ every draw, so it's
  // safe to just drop every entry: the very next draw regenerates whichever
  // one it needs, under the new mode, from CreatePixelShaderFromState (which
  // also reads Config::lighting_mode fresh).
  ps_cache_.clear();
  // pso_cache_ is deliberately left alone: every entry's ps id is
  // CachedPixelShader::id (or PixelShader::unique_id), a monotonic counter
  // (NextShaderId(), vertex_shader.h) -- the freshly (re)compiled shaders
  // above get new ids, so a stale pso_cache_ entry referencing an old id
  // simply never matches a new draw's key again. Same reasoning the class
  // comment on PSOState already documents for shader hot-reload; this cache
  // has never evicted entries even for that case, so not evicting here either
  // is consistent, not a new leak.
  LOG(INFO) << "OnLightingModeChanged: regenerated "
            << vertex_shaders_.size() << " vertex shader(s), cleared "
            << "the fixed-function pixel shader cache.\n";
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderDeclaration(
    DWORD Handle, void *pData, DWORD *pSizeOfData) {
  if (Handle < kFirstShaderHandle || !vertex_shaders_.contains(Handle))
    return D3DERR_INVALIDCALL;
  return CopyOutTokenBuffer(vertex_shaders_.at(Handle)->declaration_tokens,
                            pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderFunction(
    DWORD Handle, void *pData, DWORD *pSizeOfData) {
  if (Handle < kFirstShaderHandle || !vertex_shaders_.contains(Handle))
    return D3DERR_INVALIDCALL;
  return CopyOutTokenBuffer(vertex_shaders_.at(Handle)->function_tokens,
                            pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShader(DWORD Handle) {
  if (Handle != 0 && !pixel_shaders_.contains(Handle))
    return D3DERR_INVALIDCALL;
  bound_pixel_shader_ = Handle;
  dirty_flags_ |= DIRTY_FLAG_PSO;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShader(DWORD *pHandle) {
  *pHandle = bound_pixel_shader_;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShaderFunction(DWORD Handle,
                                                         void *pData,
                                                         DWORD *pSizeOfData) {
  if (!pixel_shaders_.contains(Handle)) return D3DERR_INVALIDCALL;
  return CopyOutTokenBuffer(pixel_shaders_.at(Handle)->function_tokens, pData,
                            pSizeOfData);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstant(
    DWORD Register, CONST void *pConstantData, DWORD ConstantCount) {
  if ((Register + ConstantCount) > kNumVsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;

  memcpy(&bound_vs_cregs_.at(Register), pConstantData,
         ConstantCount * sizeof(float[4]));
  dirty_flags_ |= DIRTY_FLAG_VS_CBUFFER;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstant(
    DWORD Register, void *pConstantData, DWORD ConstantCount) {
  if ((Register + ConstantCount) > kNumVsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;

  memcpy(pConstantData, &bound_vs_cregs_.at(Register),
         ConstantCount * sizeof(float[4]));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstant(
    DWORD Register, CONST void *pConstantData, DWORD ConstantCount) {
  // Bookkeeping only -- bound_ps_cregs_ is not currently wired into the
  // pixel shader's constant buffer (programmable_ps.hlsl reads the *vertex*
  // shader's constant array via the shared b10 cbuffer), so ps.1.x shaders
  // referencing these registers will not see the app's values yet. Storing
  // them anyway means at least GetPixelShaderConstant round-trips correctly
  // and nothing aborts.
  if ((Register + ConstantCount) > kNumPsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;
  memcpy(&bound_ps_cregs_.at(Register), pConstantData,
         ConstantCount * sizeof(float[4]));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstant(
    DWORD Register, void *pConstantData, DWORD ConstantCount) {
  if ((Register + ConstantCount) > kNumPsConstRegs || pConstantData == nullptr)
    return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &bound_ps_cregs_.at(Register),
         ConstantCount * sizeof(float[4]));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride) {
  TRACE_ENTRY(StreamNumber, pStreamData, Stride);
  if (StreamNumber >= kMaxVertexStreams) return D3DERR_INVALIDCALL;
  if (Stride > caps_.MaxStreamStride) return D3DERR_INVALIDCALL;
  Buffer *buffer = static_cast<Buffer *>(pStreamData);
  bound_vertex_streams_[StreamNumber] = InternalPtr(buffer);
  bound_vertex_stream_strides_[StreamNumber] = Stride;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) {
  if (StreamNumber >= kMaxVertexStreams) return D3DERR_INVALIDCALL;
  // InternalPtr::Get() asserts non-null; an unbound stream is routine, not
  // a caller error -- same fix as GetTexture/SetTexture (this file).
  Buffer *buffer = bound_vertex_streams_[StreamNumber]
                       ? bound_vertex_streams_[StreamNumber].Get()
                       : nullptr;
  *ppStreamData = buffer;
  if (buffer) buffer->AddRef();
  *pStride = bound_vertex_stream_strides_[StreamNumber];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetIndices(IDirect3DIndexBuffer8 *pIndexData,
                                             UINT BaseVertexIndex) {
  // DIAGNOSTIC: bound_index_buffer_ has exactly one writer -- this function
  // -- and nothing else in this class ever clears it (verified by grepping
  // every use site). So DRAW-NO-INDEXBUFFER firing can only mean the app
  // itself called SetIndices(NULL, ...) and then issued a draw before
  // re-setting a real buffer, or never called SetIndices at all before that
  // draw. Logging every transition to NULL here (paired with the frame
  // number DRAW-NO-INDEXBUFFER already logs) settles which one it is on the
  // first repro, instead of needing a follow-up build to find out.
#ifdef DX8TO12_ENABLE_VALIDATION
  if (bound_index_buffer_ && pIndexData == nullptr) {
    static int null_indices_lines = 0;
    if (null_indices_lines < 100) {
      ++null_indices_lines;
      LOG(AixLog::Severity::error)
          << "SETINDICES-NULL frame=" << CurrentFrame() << "\n";
    }
  }
#endif
  bound_index_buffer_ = InternalPtr(static_cast<Buffer *>(pIndexData));
  bound_base_vertex_ = BaseVertexIndex;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetIndices(
    IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) {
  // InternalPtr::Get() asserts non-null. No index buffer bound is routine
  // (confirmed this session: the game really does call SetIndices(NULL, ...)
  // sometimes, via the SETINDICES-NULL diagnostic), not a caller error --
  // same fix as GetTexture/SetTexture (this file).
  Buffer *buffer = bound_index_buffer_ ? bound_index_buffer_.Get() : nullptr;
  *ppIndexData = buffer;
  if (buffer) buffer->AddRef();
  *pBaseVertexIndex = bound_base_vertex_;
  return S_OK;
}

namespace {
// Real D3D8 has no separate alpha-blend state at all -- D3DRS_SRCBLEND/
// DESTBLEND/BLENDOP apply uniformly to every channel, alpha included
// (D3DRS_SEPARATEALPHABLENDENABLE and the SRCBLENDALPHA/DESTBLENDALPHA/
// BLENDOPALPHA states are a D3D9 addition this API doesn't have). The PSO
// below used to hardcode SrcBlendAlpha/DestBlendAlpha/BlendOpAlpha to
// ONE/ZERO/ADD (an unconditional passthrough) regardless of what the app
// set -- correct for on-screen backbuffer draws (nothing reads the
// presented alpha), but wrong for anything rendering to an off-screen
// target whose alpha channel is later sampled (menu blur/reflection
// effects, per the RTV-format comment a few lines below -- this project
// already assumes those exist). D3D12 requires the alpha slot's blend
// factor to make sense as a scalar (it rejects D3D12_BLEND_SRC_COLOR/
// DEST_COLOR/their INV_ variants there outright), so the four COLOR-family
// D3DBLEND values can't just be copied across like the others are for
// .SrcBlend/.DestBlend below -- each degenerates to its ALPHA counterpart
// instead, since "use the source color as a per-channel factor" applied to
// the alpha channel's own equation is exactly "use the source alpha".
D3D12_BLEND D3D8BlendToAlphaBlend(D3DBLEND blend) {
  switch (blend) {
    case D3DBLEND_SRCCOLOR:
      return D3D12_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCCOLOR:
      return D3D12_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTCOLOR:
      return D3D12_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTCOLOR:
      return D3D12_BLEND_INV_DEST_ALPHA;
    default:
      // Every other D3DBLEND value (ZERO, ONE, SRCALPHA, INVSRCALPHA,
      // DESTALPHA, INVDESTALPHA, SRCALPHASAT) is numerically identical to
      // its D3D12_BLEND counterpart -- the same fact .SrcBlend/.DestBlend
      // below already rely on -- and is already legal in the alpha slot as-
      // is (SRCALPHASAT is a scalar min(As,1-Ad) factor, not a per-channel
      // one, so it's valid for alpha too; only the four *_COLOR variants
      // above are D3D12-illegal there).
      return static_cast<D3D12_BLEND>(blend);
  }
}
}  // namespace

ComPtr<ID3D12PipelineState> Device::CreatePSO(D3DPRIMITIVETYPE d3d8_prim_type) {
  std::array<bool, kMaxTexStages> stage_has_texture = {};
  for (int i = 0; i < 8; ++i) {
    stage_has_texture[i] = bound_textures_[i];
    if (!stage_has_texture[i]) break;
  }
  // DIAGNOSTIC: stage 0 wants to sample a texture (ColorArg1/AlphaArg1 ==
  // D3DTA_TEXTURE) but none is bound -- ff_pixel_shader.cpp's
  // PixelShaderState constructor treats this exactly like ColorOp ==
  // DISABLE, silently producing a shader with no texture reference at all.
  // That is indistinguishable, from the compiled shader alone, from a
  // legitimate untextured/flat-color draw -- which is why RenderDoc alone
  // couldn't tell them apart. If this fires for the flat quad the game
  // shows, the game itself is missing a SetTexture call it should have
  // made, not this renderer dropping one.
#ifdef DX8TO12_ENABLE_VALIDATION
  if (!stage_has_texture[0] &&
      (texture_stage_states_[0].color_arg1 == D3DTA_TEXTURE ||
       texture_stage_states_[0].alpha_arg1 == D3DTA_TEXTURE)) {
    // Was capped at 50 -- confirmed tonight (braktekstur11.rdc, EID 358/1779)
    // that a flat-colored "missing texture" quad's compiled PS declares zero
    // texture resources, i.e. this is exactly the untextured-PSO case this
    // diagnostic exists to catch -- but the cap meant only the first 50
    // occurrences (all during startup/loading, per an earlier session's
    // finding) ever got logged, silently going blind for the rest of the
    // session, precisely where an actual gameplay repro would fire it.
    // Every legitimate call site sets a texture before drawing, so whatever
    // called *through* d3d8.dll to land here (game code, or a mod hooking a
    // D3D8 API) is the culprit. This fires ~4x/frame -- too often to log a
    // full 24-frame stack every time without flooding log.txt -- so resolve
    // just the first frame outside d3d8.dll (skipping our own internal call
    // chain, which is identical on every occurrence) and dedupe on that: one
    // compact line per occurrence for frequency counting via grep, and the
    // full stack only the first time a given external caller is seen.
    {
      static HMODULE self_module = [] {
        HMODULE m = nullptr;
        // An ordinary free function's address -- any address known to live
        // inside d3d8.dll, used purely to identify "our own module" for the
        // skip-filter below. Was _ReturnAddress(), which turned out
        // unreliable inside a lazily-initialized static lambda (confirmed
        // live in the SETTEX0-NULL-CALLER diagnostic below: it resolved to
        // this module's own code as if it were the external caller, every
        // single time) -- MSVC's magic-statics once-guard can route the
        // "return address" through a compiler-generated init helper instead
        // of the real call site.
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&DebugInfoQueueMessageCallback), &m);
        return m;
      }();
      void *frames[24] = {};
      USHORT count = CaptureStackBackTrace(0, 24, frames, nullptr);
      void *caller = nullptr;
      HMODULE caller_module = nullptr;
      char caller_path[MAX_PATH] = {};
      uintptr_t caller_offset = 0;
      for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(frames[i]), &module) &&
            module != self_module) {
          caller = frames[i];
          caller_module = module;
          GetModuleFileNameA(module, caller_path, sizeof(caller_path));
          caller_offset = reinterpret_cast<uintptr_t>(frames[i]) -
                          reinterpret_cast<uintptr_t>(module);
          break;
        }
      }
      // colorarg2/alpha_arg1/alpha_arg2 added to distinguish a *legitimately*
      // still-disabled stage (SELECTARG2 with Arg2 also D3DTA_TEXTURE-but-
      // missing -- correct per the arg-usage fix) from a case the arg-usage
      // fix should have silenced but didn't.
      LOG(AixLog::Severity::error)
          << "PSO-WANTS-TEX0-BUT-NONE-BOUND frame=" << CurrentFrame()
          << " colorop=" << texture_stage_states_[0].color_op
          << " colorarg1=" << texture_stage_states_[0].color_arg1
          << " colorarg2=" << texture_stage_states_[0].color_arg2
          << " alphaop=" << texture_stage_states_[0].alpha_op
          << " alphaarg1=" << texture_stage_states_[0].alpha_arg1
          << " alphaarg2=" << texture_stage_states_[0].alpha_arg2
          << " caller=" << caller_path << "+0x" << std::hex << caller_offset
          << std::dec << "\n";
      static std::set<void *> seen_callers;
      if (caller && seen_callers.insert(caller).second) {
        std::ostringstream dump;
        dump << "PSO-WANTS-TEX0-BUT-NONE-BOUND-STACK (new caller "
             << caller_path << "+0x" << std::hex << caller_offset << std::dec
             << ", " << count << " frames):\n";
        for (USHORT i = 0; i < count; ++i) {
          HMODULE module = nullptr;
          char module_path[MAX_PATH] = {};
          if (GetModuleHandleExA(
                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                  reinterpret_cast<LPCSTR>(frames[i]), &module) &&
              GetModuleFileNameA(module, module_path, sizeof(module_path))) {
            const uintptr_t offset =
                reinterpret_cast<uintptr_t>(frames[i]) -
                reinterpret_cast<uintptr_t>(module);
            dump << "  #" << i << " " << frames[i] << " " << module_path
                 << "+0x" << std::hex << offset << std::dec << "\n";
          } else {
            dump << "  #" << i << " " << frames[i] << " <unresolved module>\n";
          }
        }
        LOG(AixLog::Severity::error) << dump.str();
      }
    }
  }
#endif
  ASSERT(bound_vertex_shader_ != 0);
  VertexShader *vertex_shader = vertex_shaders_.at(bound_vertex_shader_).Get();
  // If no pixel shader is bound, generate a fixed-function shader.
  ComPtr<ID3DBlob> pixel_shader;
  uint64_t pixel_shader_id;
  if (bound_pixel_shader_ == 0) {
    const bool injection_has_normal =
        vertex_shader->decl.has_inputs[D3DVSDE_NORMAL];
    const bool injection_has_view_pos =
        !HasFlag(vertex_shader->fvf_desc, D3DFVF_XYZRHW);
    uint64_t injection_generation = 0;
    const bool injection_enabled =
        GetPixelShaderInjectionState(&injection_generation);
    if (applied_pixel_shader_injection_generation_ != injection_generation) {
      ps_cache_.clear();
      applied_pixel_shader_injection_generation_ = injection_generation;
    }
    // Try to find the fixed-function pixel shader in our cache.
    PixelShaderState key(render_state_, stage_has_texture.data(),
                         texture_stage_states_.data());
    key.injection_has_normal = injection_has_normal;
    key.injection_has_view_pos = injection_has_view_pos;
    auto iter = ps_cache_.find(key);
    if (iter != ps_cache_.end()) {
      pixel_shader = iter->second.blob;
      pixel_shader_id = iter->second.id;
    } else {
      CachedPixelShader entry{.blob = CreatePixelShaderFromState(
          key, injection_has_normal, injection_has_view_pos,
          injection_enabled ? this : nullptr)};
      pixel_shader = entry.blob;
      pixel_shader_id = entry.id;
      if (!kDisablePixelShaderCache)
        ps_cache_.emplace_hint(iter, key, std::move(entry));
    }
  } else {
    auto iter = pixel_shaders_.find(bound_pixel_shader_);
    ASSERT(iter != pixel_shaders_.end());
    pixel_shader = iter->second->blob;
    pixel_shader_id = iter->second->unique_id;
  }

  // Matches the render target BeginScene actually binds via
  // OMSetRenderTargets -- the bound target if one's set, otherwise the
  // current back buffer. Games commonly render to an off-screen target with
  // a different format than the backbuffer (radar map, menu blur/reflection
  // effects, etc.), so the PSO's declared output format has to track
  // whichever target is actually bound rather than assuming it's always the
  // backbuffer's format.
  const DXGI_FORMAT current_rtv_format =
      CurrentColorTarget()->resource_desc().Format;
  GpuTexture *const current_depth_target = CurrentDepthTarget();

  // Now that we know our pixel shader, try to look into the PSO cache.
  PSOState pso_key{
      .rs = render_state_,
      .vs = vertex_shader->unique_id,
      .ps = pixel_shader_id,
      .prim_type = d3d8_prim_type,
      .dsv_format = bound_depth_target_
                        ? DepthDsvFormatFromTypeless(
                              bound_depth_target_->resource_desc().Format)
                        : DXGI_FORMAT_UNKNOWN,
      .rtv_format = current_rtv_format,
      .near_plane_clipping = GetConfig().near_plane_clipping};

  // Zero out/normalize every RenderState field that doesn't actually affect
  // the D3D12_GRAPHICS_PIPELINE_STATE_DESC built below, isn't fed into any
  // cbuffer at draw time, and doesn't influence which vertex/pixel shader
  // gets selected (those already get their own distinct `vs`/`ps` blob
  // pointers above, which the PSO key captures) -- otherwise every distinct
  // value any of these fields ever take (e.g. D3DRS_ALPHAREF, commonly
  // varied per-material for alpha-cutout objects like foliage/fences)
  // produces a spurious *new* cache entry for what is, byte-for-byte, an
  // identical PSO. Confirmed dead for PSO/shader-selection purposes by
  // grepping every other use site in this file. This was previously mostly
  // commented out (i.e. not actually applied) -- verified via a real,
  // long GTA: Vice City session that accumulated 75,000+ live D3D12
  // objects by the end (`pso_cache_`/`ps_cache_` never evict), degrading
  // performance over time and eventually crashing outright.
  pso_key.rs.texture_factor = 0;
  pso_key.rs.ambient = 0;
  pso_key.rs.diffuse_material_source = pso_key.rs.specular_material_source =
      pso_key.rs.ambient_material_source = pso_key.rs.emissive_material_source =
          D3DMCS_MATERIAL;
  pso_key.rs.alpha_ref = 0;
  pso_key.rs.dither_enable = 0;
  pso_key.rs.fog_enable = 0;
  pso_key.rs.fog_color = 0;
  pso_key.rs.fog_table_mode = D3DFOG_NONE;
  pso_key.rs.fog_start = 0;
  pso_key.rs.fog_end = 0;
  pso_key.rs.fog_density = 0;
  pso_key.rs.range_fog_enable = 0;
  pso_key.rs.fog_vertex_mode = D3DFOG_NONE;
  pso_key.rs.color_vertex = 0;
  // D3DRS_STENCILREF is dynamic (OMSetStencilRef, set per-draw below,
  // outside the PSO) -- unlike the other stencil states just above it in
  // DepthStencilState, D3D12 doesn't bake the reference value into the PSO
  // at all, so leaving it in the key would fragment the cache with one
  // identical PSO per distinct ref value ever used (stencil ref is commonly
  // varied per-object for masking techniques).
  pso_key.rs.stencil_ref = 0;
  pso_key.rs.local_viewer = FALSE;
  pso_key.rs.normalized_normals = FALSE;

  auto pso_cache_iter = pso_cache_.find(pso_key);
#ifdef DX8TO12_ENABLE_VALIDATION
  // DIAGNOSTIC: plan/oportowanie.md section 8.3 item 7 -- cache hit/miss and
  // compile counters. pso_cache_/ps_cache_ never evict (see the comment near
  // their declarations), so a session that compiles many distinct PSOs
  // could in principle grow without bound; this is the cheap first check
  // before considering an actual eviction policy (which risks the same
  // stale-PSO-still-referenced-by-an-in-flight-command-list hazard this
  // session has already hit once with programmable shader identity).
  {
    static uint64_t hits = 0, misses = 0;
    (pso_cache_iter != pso_cache_.end() ? hits : misses)++;
    if ((hits + misses) % 2000 == 0) {
      LOG(AixLog::Severity::error)
          << "PSOCACHE-STATS frame=" << CurrentFrame() << " hits=" << hits
          << " misses=" << misses << " distinctPSOs=" << pso_cache_.size()
          << "\n";
    }
  }
#endif
  if (pso_cache_iter != pso_cache_.end()) {
    return pso_cache_iter->second;
  }

  // LOG(INFO) << "Num PSOs: " << std::dec << pso_cache_.size() << "\n";

  ASSERT(render_state_.zbuffer_type <= 1);

  D3D12_PRIMITIVE_TOPOLOGY_TYPE d3d12_prim_type;
  switch (d3d8_prim_type) {
    case D3DPT_POINTLIST:
      d3d12_prim_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;
    case D3DPT_LINELIST:
    case D3DPT_LINESTRIP:
      // The PSO's topology *type* only distinguishes point/line/triangle/
      // patch, not list vs strip -- that's set per-draw via
      // IASetPrimitiveTopology, so list and strip share a PSO type.
      d3d12_prim_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    case D3DPT_TRIANGLELIST:
    case D3DPT_TRIANGLESTRIP:
      d3d12_prim_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
    default:
      FAIL("Unimplemented primitive type %d", d3d8_prim_type);
  }
  ASSERT(render_state_.src_blend <= D3DBLEND_SRCALPHASAT);
  ASSERT(render_state_.dest_blend <= D3DBLEND_SRCALPHASAT);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{
      .pRootSignature = main_root_sig_.get(),
      .VS = {.pShaderBytecode = vertex_shader->blob->GetBufferPointer(),
             .BytecodeLength = vertex_shader->blob->GetBufferSize()},
      .PS = {.pShaderBytecode = pixel_shader->GetBufferPointer(),
             .BytecodeLength = pixel_shader->GetBufferSize()},
      .BlendState =
          {.RenderTarget = {{
               .BlendEnable = render_state_.alpha_blend_enable != 0,
               .SrcBlend = static_cast<D3D12_BLEND>(render_state_.src_blend),
               .DestBlend = static_cast<D3D12_BLEND>(render_state_.dest_blend),
               .BlendOp = static_cast<D3D12_BLEND_OP>(render_state_.blend_op),
               // See D3D8BlendToAlphaBlend's comment above: real D3D8 blends
               // alpha with the same factors/op as color, not a fixed
               // passthrough.
               .SrcBlendAlpha = D3D8BlendToAlphaBlend(render_state_.src_blend),
               .DestBlendAlpha =
                   D3D8BlendToAlphaBlend(render_state_.dest_blend),
               .BlendOpAlpha =
                   static_cast<D3D12_BLEND_OP>(render_state_.blend_op),
               .LogicOp = D3D12_LOGIC_OP_NOOP,
               .RenderTargetWriteMask =
                   safe_cast<uint8_t>(render_state_.color_write_enable),
           }}},
      .SampleMask = UINT_MAX,
      .RasterizerState =
          {
              .FillMode = static_cast<D3D12_FILL_MODE>(render_state_.fill_mode),
              .CullMode = render_state_.cull_mode != D3DCULL_NONE
                              ? D3D12_CULL_MODE_BACK
                              : D3D12_CULL_MODE_NONE,
              .FrontCounterClockwise = render_state_.cull_mode == D3DCULL_CW,
              // D3DRS_ZBIAS is a legacy 0-16 integer scale (MSDN: "a larger
              // value indicates a greater bias" toward the camera -- higher
              // values win the depth test against coplanar/near-coplanar
              // geometry, the classic use being a decal drawn flush against
              // a surface without z-fighting it). There's no single correct
              // D3D8->D3D12 unit conversion (MSDN never specified one --
              // real D3D8 drivers already varied per-vendor here), so this
              // is a reasonable small negative scale (toward the camera)
              // rather than the previously-hardcoded 0, which silently
              // dropped the bias entirely and left this class of decal
              // exposed to z-fighting/occlusion mismatches against the
              // surface it's meant to sit on top of.
              .DepthBias = -static_cast<INT>(render_state_.z_bias) * 16,
              .DepthBiasClamp = 0.f,
              // D3D8 always clips to the near plane -- it has no render state
              // to turn that off. D3D12 does, and a zero-initialised
              // D3D12_RASTERIZER_DESC leaves it FALSE, which is the opposite
              // of both D3D8's behaviour and D3D12's own documented default
              // state. Geometry crossing the near plane was therefore not
              // clipped but depth-clamped, so a triangle with a vertex behind
              // the camera stretched across the screen instead of being cut.
              // Observed in Vice City as bush foliage smearing over the radar
              // and hiding the rotating map inside it (the icons and the ring,
              // drawn afterwards, stayed visible -- which is what showed the
              // map was being covered rather than going missing).
              //
              // Driven by D3DRS_CLIPPING, which is what D3D8 actually exposes
              // -- an earlier version of this hardcoded TRUE on the claim
              // that D3D8 had no such state. It does. The state was already
              // tracked here and simply never reached a PSO, the same way
              // stencil once didn't, so a game turning clipping off for
              // geometry it has pre-clipped itself was ignored and had that
              // geometry clipped anyway. Measured: forcing it on produced
              // roughly three times as many dropped frames as leaving it off.
              //
              // NearPlaneClipping remains as a global override for bisecting;
              // render_state_ is already part of the PSO key, so honouring the
              // game's own state needs no extra key field.
              .DepthClipEnable =
                  (render_state_.clipping && GetConfig().near_plane_clipping)
                      ? TRUE
                      : FALSE,
              .MultisampleEnable = render_state_.multisample_antialias != 0,
              .AntialiasedLineEnable = render_state_.edge_antialias != 0,
          },
      .DepthStencilState =
          {
              .DepthEnable = render_state_.zbuffer_type && bound_depth_target_,
              .DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(
                  render_state_.zwrite_enable != 0),
              .DepthFunc =
                  static_cast<D3D12_COMPARISON_FUNC>(render_state_.z_func),
              // Render states were tracked (SetRenderState/GetRenderState
              // round-tripped correctly) but never actually reached a PSO --
              // stencil testing was silently a no-op regardless of what the
              // app requested, e.g. any stencil-masked multipass effect
              // (mirrors, shadow volumes, portal/decal masking) drew as if
              // D3DRS_STENCILENABLE were always FALSE.
              //
              // Also gated on the bound depth target's format actually
              // *having* a stencil plane. D3DFMT_D32/D3DFMT_D16 both map to
              // stencil-less DXGI formats (D32_FLOAT/D16_UNORM -- see
              // DXGIFromD3DFormat, dx_utils.cpp), and D3D12 flatly rejects
              // StencilEnable=true against a DSV format with no stencil
              // component. GTA: Vice City always ends up on D24S8-family
              // (which does carry stencil bits here, X8/X4S4's "unused
              // bits" notwithstanding -- see the D3DFMT_D24* case in
              // DXGIFromD3DFormat), so this hasn't been observed to fire in
              // practice, but D3DRS_STENCILENABLE is app-controlled and
              // independent of which depth format got created -- nothing
              // stops a game from setting it while a stencil-less depth
              // buffer happens to be bound, and the debug layer would flag
              // it immediately (dev build) while a release build's driver
              // behavior for the same PSO is undefined rather than merely
              // "test always passes".
              // bound_depth_target_'s resource_desc().Format is typeless (see
              // DepthTypelessFromConcrete) -- compare against the typeless
              // value a D24S8-family depth buffer actually has now, not the
              // old concrete DXGI_FORMAT_D24_UNORM_S8_UINT (which this
              // typeless resource's Format can never equal again, silently
              // forcing StencilEnable false unconditionally).
              .StencilEnable =
                  render_state_.stencil_enable != 0 &&
                  bound_depth_target_ &&
                  bound_depth_target_->resource_desc().Format ==
                      DXGI_FORMAT_R24G8_TYPELESS,
              .StencilReadMask =
                  static_cast<UINT8>(render_state_.stencil_mask),
              .StencilWriteMask =
                  static_cast<UINT8>(render_state_.stencil_write_mask),
              // D3D8 has one stencil op set applying to both polygon
              // winding orders -- two-sided stencil (independent front/back
              // ops) is a D3D9 addition (D3DRS_TWOSIDEDSTENCILMODE /
              // CCW_STENCIL*) this codebase doesn't track, so front and back
              // share the same D3D8 state here, matching D3D8 semantics
              // exactly rather than leaving BackFace at D3D12's permissive
              // KEEP/ALWAYS default (which would silently pass the stencil
              // test on back-facing geometry regardless of what the app
              // configured).
              .FrontFace =
                  {
                      .StencilFailOp = static_cast<D3D12_STENCIL_OP>(
                          render_state_.stencil_fail),
                      .StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(
                          render_state_.stencil_zfail),
                      .StencilPassOp = static_cast<D3D12_STENCIL_OP>(
                          render_state_.stencil_pass),
                      .StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(
                          render_state_.stencil_func),
                  },
              .BackFace =
                  {
                      .StencilFailOp = static_cast<D3D12_STENCIL_OP>(
                          render_state_.stencil_fail),
                      .StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(
                          render_state_.stencil_zfail),
                      .StencilPassOp = static_cast<D3D12_STENCIL_OP>(
                          render_state_.stencil_pass),
                      .StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(
                          render_state_.stencil_func),
                  },
          },
      .InputLayout = {.pInputElementDescs =
                          vertex_shader->decl.input_elements.data(),
                      .NumElements = static_cast<UINT>(
                          vertex_shader->decl.input_elements.size())},
      .PrimitiveTopologyType = d3d12_prim_type,
      .NumRenderTargets = 1,
      .RTVFormats = {current_rtv_format},
      // bound_depth_target_'s resource_desc().Format is typeless (see
      // DepthTypelessFromConcrete/BaseTexture::Create) -- a PSO's DSVFormat
      // must be the concrete DSV-compatible format instead, or D3D12
      // rejects every draw with "the depth stencil format does not match
      // that specified by the current pipeline state".
      // CurrentDepthTarget, not bound_depth_target_: it is what BeginScene
      // actually binds, and a PSO whose DSVFormat disagrees with the bound
      // DSV makes D3D12 reject every draw.
      .DSVFormat = current_depth_target
                       ? DepthDsvFormatFromTypeless(
                             current_depth_target->resource_desc().Format)
                       : DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1, .Quality = 0}};
  ComPtr<ID3D12PipelineState> pso;
  ASSERT_HR(d3d12_device_->CreateGraphicsPipelineState(
      &desc, IID_PPV_ARGS(pso.GetForInit())));
  if (!kDisablePsoCache)
    pso_cache_.emplace_hint(pso_cache_iter, std::move(pso_key), pso);
  return pso;
}

HRESULT STDMETHODCALLTYPE Device::BeginScene() {
  TRACE_ENTRY();
#ifdef DX8TO12_ENABLE_MINDEBUG
  KeepGtaTargetRoadLodVisible();
#endif
  // Set viewports. Scaled into the scene's resolution when it is rendering
  // smaller than the output; viewport_ itself stays as the game set it.
  const D3D12_VIEWPORT effective = EffectiveViewport();
  cmd_list_->RSSetViewports(1, &effective);
  D3D12_RECT scissors = {.left = 0,
                         .top = 0,
                         .right = static_cast<LONG>(effective.Width),
                         .bottom = static_cast<LONG>(effective.Height)};
  cmd_list_->RSSetScissorRects(1, &scissors);

  ID3D12DescriptorHeap *heaps[] = {srv_heap_.heap(), sampler_heap_.heap()};
  cmd_list_->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);

  GpuTexture *render_target = CurrentColorTarget();
  // Must be chosen together with the colour target: D3D12 renders only where
  // both cover, so a mismatched pair silently clips the frame.
  GpuTexture *depth_target = CurrentDepthTarget();

  // Transition the back buffer from present (or common) to render target.
  TransitionTexture(render_target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Set the default render targets.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_target->rtv_handle();
  D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
  if (depth_target) {
    TransitionTexture(depth_target, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    dsv_handle = depth_target->dsv_handle();
    MarkResourceAsUsed(InternalPtr(depth_target));
  }
  cmd_list_->OMSetRenderTargets(1, &rtv_handle, 1,
                                depth_target ? &dsv_handle : nullptr);
  MarkResourceAsUsed(InternalPtr(render_target));
  dirty_flags_ ^= DIRTY_FLAG_OM;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE Device::EndScene() { return S_OK; }

HRESULT STDMETHODCALLTYPE Device::Clear(DWORD Count, CONST D3DRECT *pRects,
                                        DWORD Flags, D3DCOLOR Color, float Z,
                                        DWORD Stencil) {
  // Small-buffer optimized: D3D8 Clear rect counts are tiny in practice (a
  // full-screen clear passes none at all), so the common case shouldn't heap
  // allocate. Falls back to the vector only for an unexpectedly large Count.
  std::array<D3D12_RECT, 8> rect_inline;
  std::vector<D3D12_RECT> rect_storage;
  D3D12_RECT *rects = nullptr;
  if (pRects) {
    D3D12_RECT *rect_dest = rect_inline.data();
    if (Count > rect_inline.size()) {
      rect_storage.resize(Count);
      rect_dest = rect_storage.data();
    }
    // The game states its rects in its own coordinates. When the scene is
    // being rendered smaller than the game thinks, they have to be scaled
    // with it or a partial clear lands in the wrong place.
    const float scale =
        (viewport_.Width > 0.f) ? EffectiveViewport().Width / viewport_.Width
                                : 1.f;
    for (DWORD i = 0; i < Count; ++i) {
      rect_dest[i] = {
          .left = static_cast<LONG>(pRects[i].x1 * scale),
          .top = static_cast<LONG>(pRects[i].y1 * scale),
          .right = static_cast<LONG>(pRects[i].x2 * scale),
          .bottom = static_cast<LONG>(pRects[i].y2 * scale)};
    }
    rects = rect_dest;
  }

  if (Flags & D3DCLEAR_TARGET) {
    // Clear can be called before BeginScene - so make sure to transition the
    // render taret.
    GpuTexture *render_target = CurrentColorTarget();
    TransitionTexture(render_target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float color[4] = {((Color >> 16) & 0xFF) / 255.f,
                      ((Color >> 8) & 0xFF) / 255.f, (Color & 0xFF) / 255.f,
                      ((Color >> 24) & 0xFF) / 255.f};
    cmd_list_->ClearRenderTargetView(render_target->rtv_handle(), color,
                                     static_cast<UINT>(rects ? Count : 0),
                                     rects);
  }
  if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
    // The one BeginScene will actually bind, which is not the game's own
    // while the scene renders at a reduced resolution. Clearing the other one
    // would leave the depth actually in use holding the previous frame.
    GpuTexture *depth_target = CurrentDepthTarget();
    if (!depth_target) {
      LOG_ERROR()
          << "Do not have any depth stencil texture allocated to clear.\n";
      return D3DERR_INVALIDCALL;
    }
    TransitionTexture(depth_target, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CLEAR_FLAGS clear_flags = {};
    if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
    cmd_list_->ClearDepthStencilView(
        depth_target->dsv_handle(), clear_flags, Z,
        static_cast<UINT8>(Stencil), static_cast<UINT>(rects ? Count : 0),
        rects);
  }
  return S_OK;
}

HRESULT Device::PrepareDrawCall(D3DPRIMITIVETYPE PrimitiveType,
                                int start_vertex, int num_vertices) {
  // The dynamic upload ring is shared by texture copies and draw buffers.
  // Restore the draw-readable state before binding any of its VB/IB/CB views.
  TransitionDynamicRingBuffer(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                              D3D12_RESOURCE_STATE_INDEX_BUFFER);
#ifdef DX8TO12_MOTION_VECTORS
  CaptureFrameCamera();
#endif
#ifdef DX8TO12_SCENE_TARGET
  {
    // The HUD must be drawn onto the finished frame, not through the
    // upscaler with it -- see EndScenePassIfDrawIsUi.
    const auto shader_it = vertex_shaders_.find(bound_vertex_shader_);
    const bool pretransformed =
        shader_it != vertex_shaders_.end() &&
        HasFlag(shader_it->second->fvf_desc, D3DFVF_XYZRHW);
#ifdef DX8TO12_ENABLE_VALIDATION
    // Every 2D draw the game makes, whichever entry point it came through.
    // The existing UI dump only instruments DrawPrimitiveUP and
    // DrawIndexedPrimitiveUP, and the radar goes through neither -- it
    // produced zero lines while the radar was visibly glitching. This is the
    // one place all of them pass, since it is where the 3D-to-2D transition
    // is detected.
    if (pretransformed && ui_dump_enabled_) {
      static uint64_t ui_draw_seq = 0;
      // With the screen-space bounding box, so a draw can be identified by
      // where it lands rather than guessed at from its texture pointer. Every
      // earlier attempt to find the radar's map tiles searched one entry
      // point at a time and missed them; this is the one place all 2D draws
      // pass through, whatever path and topology they arrived on.
      float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
      if (bound_vertex_streams_[0] && num_vertices > 0) {
        Buffer *vb = static_cast<Buffer *>(bound_vertex_streams_[0].Get());
        const UINT stride = bound_vertex_stream_strides_[0];
        const uint64_t byte_offset =
            static_cast<uint64_t>(start_vertex) * stride;
        const uint64_t byte_size =
            static_cast<uint64_t>(num_vertices) * stride;
        if (stride >= 8 &&
            byte_offset + byte_size <= vb->resource_desc().Width &&
            byte_offset <= INT_MAX && byte_size <= INT_MAX) {
          if (const char *verts = vb->DebugCpuPtr(
                  static_cast<int>(byte_offset), static_cast<int>(byte_size))) {
            for (int i = 0; i < num_vertices; ++i) {
              float pos[2];
              memcpy(pos, verts + static_cast<size_t>(i) * stride, sizeof(pos));
              if (!std::isfinite(pos[0]) || !std::isfinite(pos[1])) continue;
              min_x = std::min(min_x, pos[0]);
              max_x = std::max(max_x, pos[0]);
              min_y = std::min(min_y, pos[1]);
              max_y = std::max(max_y, pos[1]);
            }
          }
        }
      }
      LOG(AixLog::Severity::error)
          << "UI2D seq=" << ++ui_draw_seq << " frame=" << CurrentFrame()
          << " prim=" << PrimitiveType << " startVert=" << start_vertex
          << " numVerts=" << num_vertices << " tex0="
          << (bound_textures_[0] ? bound_textures_[0].Get() : nullptr)
          << " fvf=0x" << std::hex << shader_it->second->fvf_desc << std::dec
          << " bbox=(" << min_x << "," << min_y << ")-(" << max_x << ","
          << max_y << ")\n";
    }
#endif
    EndScenePassIfDrawIsUi(pretransformed);
  }
#endif
  if (PrimitiveType > D3DPT_TRIANGLEFAN) {
    LOG_ERROR() << "Invalid primitive type " << PrimitiveType << "\n";
    return D3DERR_INVALIDCALL;
  }
  ASSERT(PrimitiveType !=
         D3DPT_TRIANGLEFAN);  // We don't actually support fans.

  // Configure the output-merger stage if anything reset it (like flushes).
  if (dirty_flags_ & DIRTY_FLAG_OM) {
    BeginScene();
  }

  // Most draws in a row reuse the same primitive type (e.g. a long run of
  // D3DPT_TRIANGLELIST calls) -- skip the redundant IASetPrimitiveTopology
  // call rather than reissuing it on every single draw.
  {
    D3D12_PRIMITIVE_TOPOLOGY topology =
        static_cast<D3D12_PRIMITIVE_TOPOLOGY>(PrimitiveType);
    if (topology != last_prim_topology_) {
      cmd_list_->IASetPrimitiveTopology(topology);
      last_prim_topology_ = topology;
    }
  }

  ASSERT(bound_vertex_shader_ != 0);
  VertexShader *vertex_shader = vertex_shaders_.at(bound_vertex_shader_).Get();
  if (bound_vertex_shader_ >= kFirstShaderHandle) {
    MarkResourceAsUsed(InternalPtr(vertex_shader));
  }
  if (bound_pixel_shader_) {
    MarkResourceAsUsed(
        InternalPtr(pixel_shaders_.at(bound_pixel_shader_).Get()));
  }

  std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexStreams> vbuffer_views = {};
  size_t max_index = 0;
  for (size_t i = 0; i < bound_vertex_streams_.size(); ++i) {
    if (vertex_shader->decl.buffer_strides[i] > 0) {
      auto &d3d_buffer = bound_vertex_streams_[i];
      if (d3d_buffer) {
        Buffer *buffer = static_cast<Buffer *>(d3d_buffer.Get());
        TransitionBuffer(buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        int stride = vertex_shader->decl.buffer_strides[i];
        // Size the view to the whole buffer, not to the draw's declared
        // vertex range. DrawIndexedPrimitive's MinIndex/NumVertices are only
        // *hints* in D3D8 -- real hardware ignores them, so games fill them
        // in loosely and their indices are free to reference vertices well
        // outside that range. GTA: Vice City's 2D text batching does exactly
        // that: it reports MinIndex=0/NumVertices=384 while drawing from
        // startIndex=9516, whose indices reach vertex ~6300. Deriving
        // SizeInBytes from those hints made the view stop after 384 vertices,
        // so the GPU read past its end and rendered garbage geometry -- seen
        // as a text quad blowing up to cover the screen with the whole font
        // atlas. The view has to span every vertex an index could name, which
        // is the entire buffer.
        vbuffer_views[i] = {
            .BufferLocation = buffer->GetGpuPtr(),
            .SizeInBytes =
                safe_cast<UINT>(buffer->resource_desc().Width),
            .StrideInBytes = static_cast<UINT>(stride)};
        if (i > max_index) max_index = i;
        MarkResourceAsUsed(bound_vertex_streams_[i]);
      } else {
        // FAIL("Shader requires bound buffer at slot %d, but none are bound.",
        // i);
      }
    }
  }

  // Skip the rebind when nothing about the stream bindings changed. The
  // views do vary per draw whenever the vertex range does (SizeInBytes is
  // derived from start_vertex/num_vertices), so this only helps runs of
  // draws sharing the same range -- but those runs are common, and the
  // comparison is far cheaper than the driver call it avoids.
  const size_t vbuffer_view_count = max_index + 1;
  if (!CacheDrawStateBindings() || vbuffer_view_count != last_vbuffer_view_count_ ||
      memcmp(last_vbuffer_views_.data(), vbuffer_views.data(),
             vbuffer_view_count * sizeof(vbuffer_views[0])) != 0) {
    cmd_list_->IASetVertexBuffers(0, static_cast<UINT>(vbuffer_view_count),
                                  vbuffer_views.data());
    memcpy(last_vbuffer_views_.data(), vbuffer_views.data(),
           vbuffer_view_count * sizeof(vbuffer_views[0]));
    last_vbuffer_view_count_ = vbuffer_view_count;
  }

  // Only rebuild the PSO cache key when something it depends on actually
  // changed. CreatePSO's key (PSOState + PixelShaderState) copies and hashes
  // the entire RenderState and all 8 TextureStageStates -- over a kilobyte
  // -- and then compares the same again on a cache hit, so running it for
  // every draw call dominated state-change cost in draw-heavy frames even
  // though consecutive draws almost always share identical state. The
  // primitive type is part of the key but arrives as a per-draw argument
  // rather than device state, so it's compared separately.
  // DIAGNOSTIC: cheap contiguous has-texture mask, mirroring the `break`-on-
  // first-gap loop in CreatePSO. Kept outside the rebuild branch so it can
  // be compared against a cache *hit* too, not just recomputed when a PSO
  // is actually (re)built.
  uint32_t current_texture_mask = 0;
  for (int i = 0; i < kMaxTexStages; ++i) {
    if (!bound_textures_[i]) break;
    current_texture_mask |= (1u << i);
  }
  if (!CacheDrawStateBindings() || (dirty_flags_ & DIRTY_FLAG_PSO) ||
      PrimitiveType != last_pso_prim_type_) {
    last_pso_ = CreatePSO(PrimitiveType);
    last_pso_prim_type_ = PrimitiveType;
    last_pso_texture_mask_ = current_texture_mask;
    if (dirty_flags_ & DIRTY_FLAG_PSO) dirty_flags_ ^= DIRTY_FLAG_PSO;
  }
#ifdef DX8TO12_ENABLE_VALIDATION
  else if (last_pso_texture_mask_ != current_texture_mask) {
    static int stale_pso_lines = 0;
    if (stale_pso_lines < 50) {
      ++stale_pso_lines;
      LOG(AixLog::Severity::error)
          << "STALE-PSO-TEXMASK bakedMask=0x" << std::hex
          << last_pso_texture_mask_ << " liveMask=0x" << current_texture_mask
          << std::dec << " prim=" << PrimitiveType << "\n";
    }
  }
#endif
  if (!CacheDrawStateBindings() || last_pso_.get() != last_set_pso_) {
    cmd_list_->SetPipelineState(last_pso_.get());
    last_set_pso_ = last_pso_.get();
  }
  // D3DRS_STENCILREF -- see the comment on pso_key.rs.stencil_ref above for
  // why this is set here instead of living in the PSO.
  {
    const int stencil_ref =
        static_cast<int>(render_state_.stencil_ref & 0xFF);
    if (!CacheDrawStateBindings() || stencil_ref != last_stencil_ref_) {
      cmd_list_->OMSetStencilRef(static_cast<UINT>(stencil_ref));
      last_stencil_ref_ = stencil_ref;
    }
  }
  // MarkResourceAsUsed(pso);
  using ::DirectX::SimpleMath::Matrix;
  // Only the transform and lighting cbuffer updates below consume this, and
  // both are gated on their own dirty flag -- computing it unconditionally
  // meant an unordered_map lookup plus a 64-byte copy and conversion on
  // every single draw call, with the result thrown away for the large
  // majority of them (a typical frame changes transforms/lights far less
  // often than it draws).
  Matrix view;
  if (dirty_flags_ & (DIRTY_FLAG_TRANSFORMS | DIRTY_FLAG_LIGHTS)) {
    view = MatrixFromD3D(GetTransform(D3DTS_VIEW));
  }

  // Set the vertex cbuffer.
  if (dirty_flags_ & DIRTY_FLAG_TRANSFORMS) {
    VertexCBuffer *cbuffer;
    ASSERT_HR(vs_cbuffer_->Lock(0, sizeof(VertexCBuffer), (BYTE **)&cbuffer,
                                D3DLOCK_DISCARD));
    Matrix proj = MatrixFromD3D(GetTransform(D3DTS_PROJECTION));
    Matrix world = MatrixFromD3D(GetTransform(D3DTS_WORLD));
#ifdef DX8TO12_TEMPORAL_JITTER
    // Nudge the camera by a sub-pixel amount, in clip space. D3D8 is
    // row-vector (clip = pos * P), so a term added to row 2 of the projection
    // is scaled by view-space z -- which for a standard perspective matrix is
    // exactly clip.w. The result is clip.xy += jitter_ndc * clip.w, i.e. a
    // constant shift in NDC after the perspective divide, at every depth.
    //
    // Only world_view_proj is jittered. world_view feeds lighting, and
    // transforms_ itself is left untouched so Device::GetViewProjMatrix (and
    // therefore mods reconstructing world position from depth) keeps seeing
    // the true, unjittered camera.
    // Divided by the *scene's* width, not the game's: the offset is defined
    // in the pixels actually being rendered, which is what the upscaler is
    // told about. Using the output width would make the jitter too small to
    // do its job whenever the scene renders smaller.
    const D3D12_VIEWPORT jitter_viewport = EffectiveViewport();
    if (jitter_viewport.Width > 0.f && jitter_viewport.Height > 0.f) {
      proj.m[2][0] += 2.f * jitter_pixels_.x / jitter_viewport.Width;
      // NDC y runs opposite to pixel y.
      proj.m[2][1] += -2.f * jitter_pixels_.y / jitter_viewport.Height;
    }
#endif
    cbuffer->world_view_proj = world * view * proj;
    cbuffer->world_view = world * view;
    cbuffer->camera_position = DirectX::SimpleMath::Vector3(0, 0, 0);
    cbuffer->inv_viewport_size = DirectX::SimpleMath::Vector2(
        2.f / viewport_.Width, 2.f / viewport_.Height);
    ASSERT_HR(vs_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_TRANSFORMS;
  }
  if (dirty_flags_ & DIRTY_FLAG_VS_CBUFFER) {
    // TODO: Only copy changed constants.
    BYTE *cbuffer;
    ASSERT_HR(vs_creg_cbuffer_->Lock(
        0, bound_vs_cregs_.size() * sizeof(bound_vs_cregs_[0]), &cbuffer,
        D3DLOCK_DISCARD));
    memcpy(cbuffer, bound_vs_cregs_.data(),
           bound_vs_cregs_.size() * sizeof(bound_vs_cregs_[0]));
    ASSERT_HR(vs_creg_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_VS_CBUFFER;
  }
  if (dirty_flags_ & DIRTY_FLAG_LIGHTS) {
    LightsCBuffer *cbuffer;
    ASSERT_HR(lights_cbuffer_->Lock(0, sizeof(LightsCBuffer),
                                    reinterpret_cast<BYTE **>(&cbuffer),
                                    D3DLOCK_DISCARD));
    int i = 0;
    ASSERT(enabled_lights_.size() <= kMaxActiveLights);
    for (auto light_index : enabled_lights_) {
      // ASSERT(render_state_.lighting);
      cbuffer->lights[i] = ShaderLightMarshall(view, lights_[light_index]);
      ++i;
    }
    cbuffer->num_lights = i;
    cbuffer->diffuse_material_source =
        render_state_.color_vertex ? render_state_.diffuse_material_source
                                   : D3DMCS_MATERIAL;
    cbuffer->ambient_material_source =
        render_state_.color_vertex ? render_state_.ambient_material_source
                                   : D3DMCS_MATERIAL;
    cbuffer->specular_material_source =
        render_state_.color_vertex ? render_state_.specular_material_source
                                   : D3DMCS_MATERIAL;
    cbuffer->emissive_material_source =
        render_state_.color_vertex ? render_state_.emissive_material_source
                                   : D3DMCS_MATERIAL;
    cbuffer->specular_enable = render_state_.specular_enable;
    cbuffer->lighting_enabled = render_state_.lighting;
    cbuffer->normalize_normals = render_state_.normalized_normals;
    cbuffer->global_ambient = Dx8::Color(render_state_.ambient).ToValue();
    ASSERT_HR(lights_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_LIGHTS;
  }
  if (dirty_flags_ & DIRTY_FLAG_PS_CBUFFER) {
    // And pixel cbuffer.
    PixelCBuffer *cbuffer;
    ASSERT_HR(ps_cbuffer_->Lock(0, sizeof(PixelCBuffer), (BYTE **)&cbuffer,
                                D3DLOCK_DISCARD));
    cbuffer->material_diffuse = material_.Diffuse;
    cbuffer->material_ambient = material_.Ambient;
    cbuffer->material_specular = material_.Specular;
    cbuffer->material_emissive = material_.Emissive;
    cbuffer->material_power = material_.Power;

    cbuffer->alpha_ref = (render_state_.alpha_ref & 0xFF) / 255.f;
    cbuffer->texture_factor =
        Dx8::Color(render_state_.texture_factor).ToValue();
    // Fog. See the fog_enable comment on PixelCBuffer (vertex_shader.h) for
    // why this is a runtime cbuffer value instead of a PSO/shader variant --
    // pso_key.rs.fog_* is deliberately zeroed in CreatePSO's key so these
    // values never fragment the PSO cache.
    cbuffer->fog_enable = render_state_.fog_enable;
    cbuffer->fog_mode = render_state_.fog_table_mode != D3DFOG_NONE
                            ? render_state_.fog_table_mode
                            : render_state_.fog_vertex_mode;
    cbuffer->fog_start = render_state_.fog_start;
    cbuffer->fog_end = render_state_.fog_end;
    cbuffer->fog_density = render_state_.fog_density;
    cbuffer->fog_color = Dx8::Color(render_state_.fog_color).ToValue();
    ASSERT_HR(ps_cbuffer_->Unlock());
    dirty_flags_ ^= DIRTY_FLAG_PS_CBUFFER;
  }
  // The root signature only actually needs (re)binding once per command
  // list, not once per draw. Re-binding it is not free, and per the D3D12
  // spec it also *invalidates every root argument* -- so the old
  // unconditional call here was, strictly speaking, invalidating the CBVs
  // and descriptor tables set just below it on the previous draw and relying
  // on them being re-set again. Bind it once per command list instead, and
  // invalidate our own root-argument caches whenever we do.
  if (!CacheDrawStateBindings() || !root_sig_bound_) {
    cmd_list_->SetGraphicsRootSignature(main_root_sig_.get());
    root_sig_bound_ = true;
    last_root_cbvs_.fill(0);
    // Descriptor tables are root arguments too, so they need re-issuing for
    // the same reason -- every stage, since binding the root signature drops
    // all of them at once regardless of which stage last changed.
    dirty_flags_ |= DIRTY_FLAG_PS_TEXTURES;
    dirty_flags_ |= DIRTY_FLAG_PS_SAMPLERS;
    dirty_texture_stage_mask_ = 0xFF;
    dirty_sampler_stage_mask_ = 0xFF;
  }

  // Set all the necessary roots. These addresses only change when the
  // underlying cbuffer is re-locked with D3DLOCK_DISCARD (which hands back a
  // fresh ring-buffer allocation) -- which is exactly what the dirty-flag
  // blocks above do, and only when something actually changed. On every
  // other draw all four are identical to what's already bound.
  auto set_root_cbv = [&](UINT slot, GpuPtr gpu_ptr) {
    const D3D12_GPU_VIRTUAL_ADDRESS address = gpu_ptr;
    if (CacheDrawStateBindings() && last_root_cbvs_[slot] == address) return;
    last_root_cbvs_[slot] = address;
    cmd_list_->SetGraphicsRootConstantBufferView(slot, address);
  };
  set_root_cbv(0, vs_cbuffer_->GetGpuPtr());
  set_root_cbv(1, ps_cbuffer_->GetGpuPtr());
  set_root_cbv(2, lights_cbuffer_->GetGpuPtr());
  set_root_cbv(3, vs_creg_cbuffer_->GetGpuPtr());

  // Keep every currently-bound texture's keep-alive ref fresh for this
  // frame's back-buffer slot, regardless of whether the GPU-visible
  // descriptor table binding itself needs to be re-issued below. D3D8
  // texture bindings are sticky -- a game can (and routinely does) draw many
  // times, across many frames, off a single SetTexture call, relying on the
  // binding staying in effect without repeating it. MarkResourceAsUsed used
  // to only run inside the "rebind the descriptor table" branch, which only
  // fires on the draw right after SetTexture actually changes something; on
  // every later draw reusing the same sticky binding, the bound texture was
  // never re-marked as used for that frame's slot. Once this session's
  // MarkResourceAsUsed dedup (see slot_generation_) started skipping the
  // *previously unconditional* AddRef instead of just being redundant with
  // it, a sticky-bound texture could have its keep-alive ref lapse (and its
  // SRV descriptor slot get freed and reused by a different texture) while
  // the GPU was still actively rendering from that same descriptor slot --
  // observed as another texture's contents flashing in (a font atlas filling
  // a menu background, the sky flickering). MarkResourceAsUsed is itself
  // already a cheap same-generation dedup check, so doing this unconditionally
  // every draw is fine.
  for (int i = 0; i < kMaxTexStages; ++i) {
    if (bound_textures_[i]) {
      // D3D11 implicitly resolves this hazard when a resource is sampled.
      // DX12 requires the explicit transition: a texture may have been left
      // in COMMON (or a copy/render-target state) after streaming or an
      // off-screen pass.  Without this barrier the SRV table can be valid yet
      // sampling is undefined, which showed up in Vice City as the near mip
      // disappearing while the distant LOD remained visible.
      TransitionTexture(bound_textures_[i].Get(),
                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      MarkResourceAsUsed(bound_textures_[i]);
    }
  }

  if (dirty_flags_ & DIRTY_FLAG_PS_TEXTURES) {
    // And all the textures -- but only the stages dirty_texture_stage_mask_
    // actually marked touched. See its comment (device.h) for why this is a
    // mask set at mutation time rather than inferred by comparing cached
    // texture identities against the current binding.
    const uint32_t mask =
        CacheDrawStateBindings() ? dirty_texture_stage_mask_ : 0xFFu;
#ifdef DX8TO12_ENABLE_VALIDATION
    // DIAGNOSTIC: an external analysis of the reverted single-descriptor-
    // table experiment (see git branch descriptor-table-perf-investigation)
    // pointed out the real cost wasn't the copy API chosen -- it was
    // rebuilding all kMaxTexStages slots on every dirty event regardless of
    // how many stages actually changed. Before attempting a partial-update
    // or cached-table redesign (both of which reintroduce the same class of
    // stale-descriptor lifetime risk this session has spent all night
    // chasing elsewhere), measure whether that premise even holds for this
    // game: how many of the 8 stages does a typical dirty event actually
    // touch? Histogram bucketed by popcount(mask), logged periodically
    // rather than per-event to avoid SETTEX0-scale log volume for something
    // that's just a distribution question.
    {
      static uint64_t histogram[kMaxTexStages + 1] = {};
      static uint64_t total_events = 0;
      int changed = 0;
      for (uint32_t m = mask; m; m &= (m - 1)) ++changed;
      ++histogram[changed];
      ++total_events;
      if (total_events % 5000 == 0) {
        std::ostringstream dump;
        dump << "TEXDIRTY-HISTOGRAM frame=" << CurrentFrame()
             << " totalEvents=" << total_events << " counts=[";
        for (int i = 0; i <= kMaxTexStages; ++i) {
          dump << histogram[i] << (i < kMaxTexStages ? "," : "");
        }
        dump << "]\n";
        LOG(AixLog::Severity::error) << dump.str();
      }
    }
#endif
    for (int i = 0; i < kMaxTexStages; ++i) {
      if (!(mask & (1u << i))) continue;
      GpuTexture *tex =
          bound_textures_[i] ? bound_textures_[i].Get() : nullptr;
      if (tex) {
        const auto gpu_handle = srv_heap_.GetGPUHandleFor(tex->srv_handle());
#ifdef DX8TO12_ENABLE_VALIDATION
        {
          static int lines = 0;
          if (lines < 2000) {
            ++lines;
            LOG(AixLog::Severity::error)
                << "ROOTTABLE-SRV frame=" << CurrentFrame() << " stage=" << i
                << " slot=" << (textures_start_bindslot_ + i)
                << " handle=0x" << std::hex << gpu_handle.ptr
                << " srv_heap=0x" << srv_heap_.heap() << std::dec << "\n";
          }
        }
#endif
        cmd_list_->SetGraphicsRootDescriptorTable(textures_start_bindslot_ + i,
                                                  gpu_handle);
      }
    }
    dirty_texture_stage_mask_ = 0;
    dirty_flags_ ^= DIRTY_FLAG_PS_TEXTURES;
  }

  if (dirty_flags_ & DIRTY_FLAG_PS_SAMPLERS) {
    // Set the samplers -- only the stages dirty_sampler_stage_mask_ marked
    // touched. Same reasoning as the texture loop just above.
    const uint32_t mask =
        CacheDrawStateBindings() ? dirty_sampler_stage_mask_ : 0xFFu;
    for (int i = 0; i < kMaxTexStages; ++i) {
      if (!(mask & (1u << i))) continue;
      SamplerDesc desc(texture_stage_states_[i]);
      auto iter = sampler_cache_.find(desc);
#ifdef DX8TO12_ENABLE_VALIDATION
      // DIAGNOSTIC: same plan item as PSOCACHE-STATS above, for the sampler
      // cache -- sampler_heap_ is a fixed-size pool (kMaxSamplerStates=64,
      // device_limits.h) that never evicts either, so this also answers
      // "is the distinct-sampler-state count anywhere near that limit."
      {
        static uint64_t hits = 0, misses = 0;
        (iter != sampler_cache_.end() ? hits : misses)++;
        if ((hits + misses) % 2000 == 0) {
          LOG(AixLog::Severity::error)
              << "SAMPLERCACHE-STATS frame=" << CurrentFrame()
              << " hits=" << hits << " misses=" << misses
              << " distinctSamplers=" << sampler_cache_.size() << "\n";
        }
      }
#endif
      if (iter == sampler_cache_.end()) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = sampler_heap_.Allocate();
        d3d12_device_->CreateSampler(&desc, cpu_handle);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle =
            sampler_heap_.GetGPUHandleFor(cpu_handle);
        iter = sampler_cache_.insert(iter, std::pair(desc, gpu_handle));
      }
      ASSERT(iter->second.ptr != 0);
#ifdef DX8TO12_ENABLE_VALIDATION
      {
        static int lines = 0;
        if (lines < 2000) {
          ++lines;
          LOG(AixLog::Severity::error)
              << "ROOTTABLE-SAMPLER frame=" << CurrentFrame() << " stage=" << i
              << " slot=" << (textures_start_bindslot_ + kMaxTexStages + i)
              << " handle=0x" << std::hex << iter->second.ptr
              << " sampler_heap=0x" << sampler_heap_.heap() << std::dec
              << "\n";
        }
      }
#endif
      cmd_list_->SetGraphicsRootDescriptorTable(
          textures_start_bindslot_ + kMaxTexStages + i, iter->second);
    }
    dirty_sampler_stage_mask_ = 0;
    dirty_flags_ ^= DIRTY_FLAG_PS_SAMPLERS;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                                 UINT StartVertex,
                                                 UINT PrimitiveCount) {
  ++draw_calls_this_frame_;
#ifdef DX8TO12_ENABLE_MINDEBUG
  const size_t rw_primitive_caller = RecordRwDrawCall(false);
#endif
  // D3D12 has no fan topology. Emulate it with a generated index list (0,
  // i+1, i+2 for each triangle) drawn as a triangle list against the
  // already-bound vertex buffer, the same trick DrawPrimitiveUP already uses
  // by rewriting the vertex data directly -- here we can't rewrite the
  // (GPU-side, already bound) vertex buffer, so we index into it instead.
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    const UINT vertex_count = PrimitiveCount + 2;
    const UINT index_count = 3 * PrimitiveCount;
    std::vector<uint16_t> indices;
    indices.reserve(index_count);
    for (UINT i = 0; i < PrimitiveCount; ++i) {
      indices.push_back(static_cast<uint16_t>(StartVertex));
      indices.push_back(static_cast<uint16_t>(StartVertex + i + 1));
      indices.push_back(static_cast<uint16_t>(StartVertex + i + 2));
    }
    const size_t index_bytes = index_count * sizeof(uint16_t);
    DynamicRingBuffer::Allocation index_alloc =
        dynamic_ring_buffer()->Allocate(index_bytes);
    memcpy(dynamic_ring_buffer()->GetCpuPtrFor(index_alloc), indices.data(),
           index_bytes);
    D3D12_INDEX_BUFFER_VIEW ib_view{
        .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(index_alloc),
        .SizeInBytes = safe_cast<UINT>(index_bytes),
        .Format = DXGI_FORMAT_R16_UINT};

#ifdef DX8TO12_ENABLE_VALIDATION
    // Triangle fans are, in Vice City, essentially only the radar: the map
    // tiles are clipped against the circle and each clipped polygon comes
    // through here. So this logs the geometry the game actually submitted --
    // the question being whether the wedge that goes missing on screen is
    // already missing from these vertices (the game's own clipping) or is
    // present here and lost afterwards (ours).
    if (ui_dump_enabled_ && bound_vertex_streams_[0]) {
      Buffer *vb = static_cast<Buffer *>(bound_vertex_streams_[0].Get());
      const UINT stride = bound_vertex_stream_strides_[0];
      const uint64_t byte_offset = static_cast<uint64_t>(StartVertex) * stride;
      const uint64_t byte_size = static_cast<uint64_t>(vertex_count) * stride;
      const char *verts =
          (stride >= 8 && byte_offset + byte_size <= vb->resource_desc().Width &&
           byte_offset <= INT_MAX && byte_size <= INT_MAX)
              ? vb->DebugCpuPtr(static_cast<int>(byte_offset),
                                static_cast<int>(byte_size))
              : nullptr;
      std::ostringstream dump;
      dump << "FANDUMP frame=" << CurrentFrame() << " prims=" << PrimitiveCount
           << " startVert=" << StartVertex << " stride=" << stride << " tex0="
           << (bound_textures_[0] ? bound_textures_[0].Get() : nullptr)
           << " pos=[";
      if (verts) {
        for (UINT i = 0; i < vertex_count; ++i) {
          float pos[2];
          memcpy(pos, verts + static_cast<size_t>(i) * stride, sizeof(pos));
          dump << "(" << pos[0] << "," << pos[1] << ")";
        }
      } else {
        dump << "unreadable";
      }
      dump << "]\n";
      LOG(AixLog::Severity::error) << dump.str();
    }
#endif
    HR_OR_RETURN(
        PrepareDrawCall(D3DPT_TRIANGLELIST, StartVertex, vertex_count));
    cmd_list_->IASetIndexBuffer(&ib_view);
    cmd_list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    return S_OK;
  }

  int vertex_count;
  switch (PrimitiveType) {
    case D3DPT_POINTLIST:
      vertex_count = PrimitiveCount;
      break;
    case D3DPT_LINELIST:
      vertex_count = 2 * PrimitiveCount;
      break;
    case D3DPT_LINESTRIP:
      vertex_count = 1 + PrimitiveCount;
      break;
    case D3DPT_TRIANGLELIST:
      vertex_count = 3 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      vertex_count = 2 + PrimitiveCount;
      break;
    default:
      FAIL("TODO: Count number of vertices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }
#ifdef DX8TO12_ENABLE_MINDEBUG
  if (RwSnapshotActive() && bound_index_buffer_) {
    const auto world_it = transforms_.find(D3DTS_WORLD);
    std::ostringstream line;
    line << "RWPRIMSNAP label=" << g_rw_snapshot_label << " caller=";
    if (rw_primitive_caller < kRwPrimitiveDrawCallSites.size()) {
      line << "+0x" << std::hex
           << (kRwPrimitiveDrawCallSites[rw_primitive_caller] -
               kGtaPreferredImageBase)
           << std::dec;
    } else {
      line << "unmatched";
    }
    line << " prim=" << PrimitiveType << " pc=" << PrimitiveCount
         << " start=" << StartVertex << " vc=" << vertex_count
         << " vb="
         << (bound_vertex_streams_[0] ? bound_vertex_streams_[0].Get()
                                      : nullptr);
    if (world_it != transforms_.end()) {
      line << " wt=(" << world_it->second._41 << "," << world_it->second._42
           << "," << world_it->second._43 << ")";
    }
    line << "\n";
    AppendRwSnapshotLine(line.str());
  }
#endif
  // Coverage only: the glitch hunt has so far instrumented only the indexed
  // path, on the strength of an earlier dump that showed the menu going
  // through it. If the offending draw actually comes through here, that dump
  // was not the whole picture and the detector has been looking in the wrong
  // place -- which the counters will show rather than leave to assumption.
#ifdef DX8TO12_ENABLE_VALIDATION
  {
    static int dp_total = 0;
    static uint64_t dp_last_frame = 0;
    ++dp_total;
    if (CurrentFrame() - dp_last_frame >= 600) {
      dp_last_frame = CurrentFrame();
      LOG(AixLog::Severity::error)
          << "DRAWPRIM-COVERAGE " << std::dec
          << "nonIndexedDraws=" << dp_total << "\n";
    }
  }
#endif
  HR_OR_RETURN(PrepareDrawCall(PrimitiveType, StartVertex, vertex_count));
  cmd_list_->DrawInstanced(vertex_count, 1, StartVertex, 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  ++draw_calls_this_frame_;
  if (!bound_vertex_shader_) {
    LOG_ERROR() << "Cannot use DrawPrimitiveUP without a vertex shader.\n";
    return D3DERR_INVALIDCALL;
  }

  // Rewrite triangle fans as triangle lists.
  std::vector<uint8_t> rewritten_fan;
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    rewritten_fan.reserve(3 * PrimitiveCount * VertexStreamZeroStride);

    auto insert_vertex = [&](uint32_t index) {
      const uint8_t *pStart =
          static_cast<const uint8_t *>(pVertexStreamZeroData) +
          index * VertexStreamZeroStride;
      std::copy(pStart, pStart + VertexStreamZeroStride,
                std::back_inserter(rewritten_fan));
    };

    for (uint32_t i = 0; i < PrimitiveCount; ++i) {
      insert_vertex(0);
      insert_vertex(i + 1);
      insert_vertex(i + 2);
    }
    pVertexStreamZeroData = rewritten_fan.data();
    PrimitiveType = D3DPT_TRIANGLELIST;
  }

  int vertex_count;
  switch (PrimitiveType) {
    case D3DPT_LINELIST:
      vertex_count = 2 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLELIST:
      vertex_count = 3 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      vertex_count = 2 + PrimitiveCount;
      break;
    default:
      FAIL("TODO: Count number of vertices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }

  // Allocate some ring buffer memory.
  // DIAGNOSTIC for the "whole font atlas stretched across the screen" glitch
  // (see plan/ROADMAP.md): 2D UI is drawn through this path as small
  // D3DFVF_XYZRHW quads whose vertices are already in screen coordinates, so
  // a text glyph should cover a few dozen pixels. When the glitch happens the
  // same draw covers the entire screen, which means its vertex data is wrong
  // before we ever touch it -- or wrong because we handed it the wrong
  // memory. Log the actual incoming coordinates when a small UP draw claims
  // most of the screen; that distinguishes the two without needing a graphics
  // debugger (the glitch never survives a capture).
  if (vertex_count <= 6 && VertexStreamZeroStride >= 2 * sizeof(float)) {
    float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
    for (int i = 0; i < vertex_count; ++i) {
      const float *pos = reinterpret_cast<const float *>(
          static_cast<const uint8_t *>(pVertexStreamZeroData) +
          static_cast<size_t>(i) * VertexStreamZeroStride);
      min_x = std::min(min_x, pos[0]);
      max_x = std::max(max_x, pos[0]);
      min_y = std::min(min_y, pos[1]);
      max_y = std::max(max_y, pos[1]);
    }
    const float covered_w = max_x - min_x;
    const float covered_h = max_y - min_y;
    if (covered_w > 0.8f * viewport_.Width &&
        covered_h > 0.8f * viewport_.Height) {
      static int oversized_up_draws = 0;
      if (oversized_up_draws < 16) {
        ++oversized_up_draws;
        LOG(AixLog::Severity::error)
            << "OVERSIZED-UI-DRAW: prim=" << PrimitiveType << " verts="
            << vertex_count << " stride=" << VertexStreamZeroStride
            << " bounds=(" << min_x << "," << min_y << ")-(" << max_x << ","
            << max_y << ") viewport=" << viewport_.Width << "x"
            << viewport_.Height << " tex0="
            << (bound_textures_[0] ? bound_textures_[0].Get() : nullptr)
            << "\n";
      }
    }

    // Full per-draw dump while the F9 toggle is on, so a correct glyph quad
    // and a corrupted one can be compared side by side. This logs the vertex
    // data exactly as the game handed it over, before it's copied anywhere:
    // if the coordinates are already wrong here the corruption is upstream of
    // this shim, and if they're sane here but the draw still covers the
    // screen, it's ours.
    if (ui_dump_enabled_) {
      std::ostringstream dump;
      dump << "UIDUMP prim=" << PrimitiveType << " verts=" << vertex_count
           << " stride=" << VertexStreamZeroStride << " tex0="
           << (bound_textures_[0] ? bound_textures_[0].Get() : nullptr)
           << " vs=" << bound_vertex_shader_ << " pos=[";
      for (int i = 0; i < vertex_count; ++i) {
        const float *pos = reinterpret_cast<const float *>(
            static_cast<const uint8_t *>(pVertexStreamZeroData) +
            static_cast<size_t>(i) * VertexStreamZeroStride);
        dump << "(" << pos[0] << "," << pos[1] << ") ";
      }
      dump << "]\n";
      LOG(AixLog::Severity::error) << dump.str();
    }
  }

  size_t num_bytes = vertex_count * VertexStreamZeroStride;
  DynamicRingBuffer::Allocation alloc =
      dynamic_ring_buffer()->Allocate(num_bytes);
  memcpy(dynamic_ring_buffer()->GetCpuPtrFor(alloc), pVertexStreamZeroData,
         num_bytes);
  D3D12_VERTEX_BUFFER_VIEW vbuffer_view{
      .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(alloc),
      .SizeInBytes = safe_cast<UINT>(num_bytes),
      .StrideInBytes = VertexStreamZeroStride};

  ASSERT_HR(SetStreamSource(0, nullptr, 0));
  HR_OR_RETURN(PrepareDrawCall(PrimitiveType, 0, vertex_count));
  // Overwrite whatever vertex buffer the prepare set. This bypasses
  // PrepareDrawCall's vertex-buffer-view cache, so that cache has to be
  // invalidated -- otherwise a later regular (non-UP) draw whose views
  // happen to match the cached entry would skip its rebind and wrongly keep
  // rendering from this call's scratch ring-buffer data.
  cmd_list_->IASetVertexBuffers(0, 1, &vbuffer_view);
  last_vbuffer_view_count_ = 0;
  cmd_list_->DrawInstanced(vertex_count, 1, 0, 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices,
    UINT startIndex, UINT primCount) {
  ++draw_calls_this_frame_;
#ifdef DX8TO12_ENABLE_MINDEBUG
  const size_t rw_indexed_caller = RecordRwDrawCall(true);
#endif
  if (!bound_index_buffer_) {
    // DIAGNOSTIC: a ground-tile draw is silently missing from some frames --
    // no crash, no D3D12 trace (RenderDoc can't show a call that never
    // reaches the command list), just an absent draw. This is the only
    // early-return in the whole Draw* family that didn't already log before
    // bailing (DrawPrimitiveUP/DrawIndexedPrimitiveUP both do for their
    // equivalent "nothing bound" guard) -- if the game calls
    // DrawIndexedPrimitive right after SetIndices(NULL, ...), or before ever
    // calling SetIndices at all, this is exactly what a "the draw just isn't
    // there" symptom with zero other evidence would look like.
#ifdef DX8TO12_ENABLE_VALIDATION
    static int no_ib_lines = 0;
    if (no_ib_lines < 200) {
      ++no_ib_lines;
      LOG(AixLog::Severity::error)
          << "DRAW-NO-INDEXBUFFER frame=" << CurrentFrame()
          << " prim=" << PrimitiveType << " minIndex=" << minIndex
          << " numVerts=" << NumVertices << " startIndex=" << startIndex
          << " primCount=" << primCount << "\n";
    }
#endif
#ifdef DX8TO12_ENABLE_MINDEBUG
    RecordRwIndexedEvent(rw_indexed_caller,
                         RwIndexedEvent::NoIndexBuffer);
#endif
    return D3DERR_INVALIDCALL;
  }

  int index_count;
  switch (PrimitiveType) {
    case D3DPT_POINTLIST:
      index_count = primCount;
      break;
    case D3DPT_LINELIST:
      index_count = 2 * primCount;
      break;
    case D3DPT_LINESTRIP:
      index_count = 1 + primCount;
      break;
    case D3DPT_TRIANGLELIST:
      index_count = 3 * primCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      index_count = 2 + primCount;
      break;
    default:
      FAIL("TODO: Count number of vertices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }

  // F9 dump on the path the 2D UI actually uses -- neither DrawPrimitiveUP
  // nor DrawIndexedPrimitiveUP logged anything for the menu, so it draws from
  // a real vertex buffer through here. Records which texture each small quad
  // gets: the glitch under investigation shows the font atlas where another
  // texture belongs (see plan/ROADMAP.md), so comparing a clean dump against a
  // glitched one should show the same draw with a different texture bound.
  // No primCount filter: RenderWare batches many 2D sprites (whole strings of
  // text) into a single draw, so filtering for small quads excluded exactly
  // the draws worth seeing. A line cap keeps the log manageable instead.
  // Targeted replacement for the old blanket per-draw dump. Logging *every*
  // draw cost ~145k lines and slowed the CPU enough to close the race window
  // this is trying to catch -- the glitch reliably vanished while the dump ran,
  // which made the instrument useless for the one thing it was built for. This
  // check instead stays quiet unless a draw is about to read vertex memory
  // nothing wrote this frame, so it costs a binary search per draw and prints
  // only when there is something to see. It also runs unconditionally rather
  // than behind F9, so no keypress is needed to catch the glitch.
  // Separate caps per condition, not one shared budget -- "oversized" turned
  // out to be dominated by a legitimate, harmless effect (a screen-covering
  // quad with a tiny 64x64 texture, smoothly animated frame to frame --
  // almost certainly rain/weather, not a bug) that burned through the entire
  // shared 300-line cap in the first ~2600 frames of a session, leaving zero
  // budget left to catch anything for the rest of a multi-thousand-frame
  // play session. "stale" and "nonfinite" are the conditions that actually
  // indicate memory corruption (reading un-written or NaN/Inf vertex data)
  // and are rare/meaningful when they fire, so they get the generous
  // budgets; "oversized" alone gets a small one since a handful of samples
  // is enough to confirm/reject the "it's just the weather effect" theory
  // without crowding out the other two.
#ifdef DX8TO12_ENABLE_VALIDATION
  static int stale_lines = 0, oversized_lines = 0, nonfinite_lines = 0;
  // Coverage counters for the check below. A detector that silently examines
  // nothing is indistinguishable in the log from one that found nothing wrong,
  // and that has already sent this investigation down a blind alley twice. So
  // measure how much of the draw stream it actually looks at.
  static int cov_total = 0, cov_dynamic = 0, cov_scannable = 0, cov_reported = 0;
  static uint64_t cov_last_frame = 0;
  ++cov_total;
  if (CurrentFrame() - cov_last_frame >= 600) {
    cov_last_frame = CurrentFrame();
    LOG(AixLog::Severity::error)
        << "BADDRAW-COVERAGE " << std::dec << "indexedDraws=" << cov_total
        << " dynamicVB=" << cov_dynamic << " scannable=" << cov_scannable
        << " reported=" << cov_reported << "\n";
  }
  // Scan as long as *any* category still has budget left -- each condition
  // below is checked and reported against its own cap individually, so a
  // maxed-out "oversized" no longer blocks "stale"/"nonfinite" detection for
  // the rest of the session.
  if ((stale_lines < 2000 || oversized_lines < 20 || nonfinite_lines < 2000) &&
      bound_vertex_streams_[0]) {
    Buffer *vb = static_cast<Buffer *>(bound_vertex_streams_[0].Get());
    const int stride =
        static_cast<int>(bound_vertex_stream_strides_[0]);
    if (vb->IsDynamic() && stride > 0 && NumVertices > 0) {
      ++cov_dynamic;
      // DIAGNOSTIC: a water-simulation tile (dynamic buffer, stride 36,
      // fixed-function vertex shader) was confirmed via RenderDoc to render
      // in a wildly wrong place relative to 6 sibling tiles drawn the same
      // frame -- comparing all 7 tiles' actual clip-space transforms showed
      // the wrong one landing an order of magnitude off from where its
      // neighbors clustered. That comparison used RenderDoc's final
      // committed state; this logs the same comparison live, tagged by
      // sequence so every tile drawn this way can be lined up frame-by-frame
      // -- if one tile's D3DTS_WORLD turns out identical to a *different*
      // tile's (or to whatever the previous, unrelated object drew with),
      // that's a stale-transform bug; if all 7 are always genuinely
      // distinct, the cause is elsewhere (most likely the game's own
      // tile-placement logic, not this renderer).
      if (stride == 36) {
        static int water_xform_lines = 0;
        static uint64_t water_xform_seq = 0;
        if (water_xform_lines < 20000) {
          ++water_xform_lines;
          ++water_xform_seq;
          D3DMATRIX world = GetTransform(D3DTS_WORLD);
          LOG(AixLog::Severity::error)
              << "WATERXFORM seq=" << water_xform_seq
              << " frame=" << CurrentFrame() << " startIndex=" << startIndex
              << " translate=(" << world._41 << "," << world._42 << ","
              << world._43 << ") row0=(" << world._11 << "," << world._12
              << "," << world._13 << ")\n";
        }
      }
      const int64_t first_vertex =
          static_cast<int64_t>(bound_base_vertex_) + minIndex;
      const int64_t byte_offset = first_vertex * stride;
      const int64_t byte_size = static_cast<int64_t>(NumVertices) * stride;
      const bool in_bounds =
          byte_offset >= 0 && byte_offset + byte_size <=
                                  static_cast<int64_t>(vb->resource_desc().Width);
      const char *verts =
          in_bounds ? vb->DebugCpuPtr(static_cast<int>(byte_offset),
                                      static_cast<int>(byte_size))
                    : nullptr;
      const bool stale =
          in_bounds && !vb->IsRangeWrittenThisFrame(
                           static_cast<int>(byte_offset),
                           static_cast<int>(byte_size));
      // Read the vertices the GPU is about to read. The stale-memory check
      // above came back clean while the glitch was on screen, which rules out
      // "the data was never written" and leaves "the data is wrong" -- so look
      // at it. RenderWare's 2D drawing supplies pre-transformed (XYZRHW)
      // vertices, i.e. these floats are screen pixels, and a UI quad spanning
      // the whole viewport is by definition the glitch: the screenshot shows
      // one skewed quad carrying the entire font atlas across the menu.
      if (verts) ++cov_scannable;
      bool covers_screen = false;
      bool non_finite = false;
      float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
      if (verts && stride >= 2 * static_cast<int>(sizeof(float))) {
        // Sample a handful of vertices spread across the range rather than
        // reading them all. A buffer flagged D3DUSAGE_WRITEONLY lives in
        // write-combined memory, where CPU reads are pathologically slow, and
        // a scan costly enough to slow the frame down would close the very
        // race window this is meant to catch. Opposite corners of a
        // screen-spanning quad show up in any few samples.
        const UINT kSamples = 8;
        const UINT scanned = std::min<UINT>(NumVertices, kSamples);
        const UINT step = std::max<UINT>(1, NumVertices / kSamples);
        for (UINT i = 0; i < scanned; ++i) {
          float pos[2];
          memcpy(pos, verts + static_cast<size_t>(i) * step * stride,
                 sizeof(pos));
          if (!std::isfinite(pos[0]) || !std::isfinite(pos[1])) {
            non_finite = true;
            continue;
          }
          min_x = std::min(min_x, pos[0]);
          max_x = std::max(max_x, pos[0]);
          min_y = std::min(min_y, pos[1]);
          max_y = std::max(max_y, pos[1]);
        }
        covers_screen = min_x <= max_x &&
                        (max_x - min_x) > 0.8f * viewport_.Width &&
                        (max_y - min_y) > 0.8f * viewport_.Height;
      }
      // Second stage, run only when the cheap batch-level scan already
      // triggered. Batch bounds alone are a bad test: RenderWare packs ~192
      // glyphs into one draw, and the text of a full menu legitimately spans
      // the screen, so the first version reported those as suspects too. What
      // actually matters is whether a *single* quad covers the screen. Doing
      // this per draw would mean far more write-combined reads than the frame
      // can afford, but as a second stage it costs nothing until something is
      // already wrong.
      int bad_quad = -1;
      float q[4][2] = {};
      float uv[4][2] = {};
      if (covers_screen && verts && stride >= 28) {
        covers_screen = false;
        const UINT quads = std::min<UINT>(NumVertices / 4, 64);
        for (UINT qi = 0; qi < quads && !covers_screen; ++qi) {
          float qx0 = FLT_MAX, qy0 = FLT_MAX, qx1 = -FLT_MAX, qy1 = -FLT_MAX;
          for (UINT v = 0; v < 4; ++v) {
            const char *p = verts + (static_cast<size_t>(qi) * 4 + v) * stride;
            memcpy(q[v], p, sizeof(q[v]));
            // XYZRHW(16) + diffuse(4) + specular(4)... or + UV(8); at
            // stride 28 the last 8 bytes are the texture coordinates.
            memcpy(uv[v], p + stride - 8, sizeof(uv[v]));
            if (!std::isfinite(q[v][0]) || !std::isfinite(q[v][1])) continue;
            qx0 = std::min(qx0, q[v][0]);
            qx1 = std::max(qx1, q[v][0]);
            qy0 = std::min(qy0, q[v][1]);
            qy1 = std::max(qy1, q[v][1]);
          }
          if (qx0 <= qx1 && (qx1 - qx0) > 0.8f * viewport_.Width &&
              (qy1 - qy0) > 0.8f * viewport_.Height) {
            covers_screen = true;
            bad_quad = static_cast<int>(qi);
          }
        }
      }
      const bool report_stale = stale && stale_lines < 2000;
      const bool report_oversized = covers_screen && oversized_lines < 20;
      const bool report_nonfinite = non_finite && nonfinite_lines < 2000;
      if (report_stale || report_oversized || report_nonfinite) {
        if (report_stale) ++stale_lines;
        if (report_oversized) ++oversized_lines;
        if (report_nonfinite) ++nonfinite_lines;
        ++cov_reported;
        GpuTexture *tex0 =
            bound_textures_[0] ? bound_textures_[0].Get() : nullptr;
        std::ostringstream dump;
        dump << "BADDRAW"
             << (stale ? " stale" : "") << (covers_screen ? " oversized" : "")
             << (non_finite ? " nonfinite" : "") << " prim=" << PrimitiveType
             << " prims=" << primCount << " minIndex=" << minIndex
             << " numVerts=" << NumVertices << " startIndex=" << startIndex
             << " baseVertex=" << bound_base_vertex_ << " stride=" << stride
             << " byteOff=" << byte_offset << " byteSize=" << byte_size
             << " vbBytes=" << vb->resource_desc().Width << " tex0=" << tex0;
        if (tex0) {
          const D3D12_RESOURCE_DESC &desc = tex0->resource_desc();
          dump << " tex0size=" << std::dec << desc.Width << "x" << desc.Height;
        }
        // The first vertex's raw floats separate "read uninitialised memory"
        // from "read good data and transformed it wrong": plausible screen
        // coordinates here would mean the geometry is fine and the fault lies
        // downstream, in the transform or the stride.
        dump << " vs=" << bound_vertex_shader_ << " ps=" << bound_pixel_shader_
             << " viewport=" << viewport_.Width << "x" << viewport_.Height
             << " vb{" << vb->DebugState() << "}";
        if (tex0) {
          const D3D12_RESOURCE_DESC &d = tex0->resource_desc();
          dump << " tex0fmt=" << static_cast<int>(d.Format)
               << " tex0mips=" << d.MipLevels;
        }
        if (bad_quad >= 0) {
          dump << " badQuad=" << bad_quad;
          for (int v = 0; v < 4; ++v) {
            dump << " q" << v << "=(" << q[v][0] << "," << q[v][1] << ")uv=("
                 << uv[v][0] << "," << uv[v][1] << ")";
          }
        }
        if (min_x <= max_x) {
          dump << " bounds=(" << min_x << "," << min_y << ")-(" << max_x << ","
               << max_y << ")";
        }
        // The first few vertices in full. Whether these are plausible screen
        // pixels or nonsense is what separates "wrong geometry" from "right
        // geometry, wrong texture bound".
        if (verts && stride >= 4 * static_cast<int>(sizeof(float))) {
          const UINT scanned = std::min<UINT>(NumVertices, 4);
          for (UINT i = 0; i < scanned; ++i) {
            float f[4];
            memcpy(f, verts + static_cast<size_t>(i) * stride, sizeof(f));
            dump << " v" << i << "=[" << f[0] << "," << f[1] << "," << f[2]
                 << "," << f[3] << "]";
          }
        }
        dump << "\n";
        LOG(AixLog::Severity::error) << dump.str();
      }
    }
  }
#endif

  // DIAGNOSTIC: this draw's screen-space bbox, computed with the exact same
  // world*view*proj this frame's actual PrepareDrawCall/vertex shader use --
  // not a hand-rolled reconstruction done offline against a RenderDoc
  // capture, which turned out to have its own pitfalls (matrix convention
  // mistakes, degenerate bounds from near-plane vertices). A RenderDoc
  // PixelHistory can show a screen pixel has no draw touching it; this is
  // what lets that be cross-referenced against whether *any* draw this
  // frame even aimed at that pixel, using the shim's own ground truth.
#ifdef DX8TO12_ENABLE_VALIDATION
  if (bound_vertex_streams_[0] && bound_index_buffer_) {
    Buffer *vb0 = static_cast<Buffer *>(bound_vertex_streams_[0].Get());
    const int stride0 = static_cast<int>(bound_vertex_stream_strides_[0]);
    // Skip the screen-space RHW UI format (stride 28) -- its x/y are already
    // pixel coordinates, not local-space positions to transform.
    if (stride0 >= 12 && stride0 != 28) {
      // minIndex/NumVertices describe the *range* of vertex-buffer entries
      // the index buffer may reference for this draw (offset by
      // bound_base_vertex_, same as the actual index->vertex lookup), not
      // vertex 0..3 -- the first pass here only sampled the first 4 raw
      // buffer entries regardless of minIndex, which for any draw that
      // isn't already at the start of its vertex buffer silently measured
      // the wrong geometry (or, for a >4-vertex mesh, only ever its first
      // triangle's corner instead of the whole draw's true extent). A
      // sanity check against a pixel with obviously-rendering, unmistakable
      // road texture came back with zero covering draws, which is what
      // caught this rather than the actual hole under investigation.
      const UINT base = minIndex + bound_base_vertex_;
      const UINT scanned = std::min<UINT>(NumVertices, 512);
      const char *verts0 =
          vb0->DebugCpuPtr(static_cast<int>(base) * stride0,
                            static_cast<int>(scanned) * stride0);
      if (verts0) {
        using ::DirectX::SimpleMath::Matrix;
        using ::DirectX::SimpleMath::Vector4;
        Matrix wvp = MatrixFromD3D(GetTransform(D3DTS_WORLD)) *
                     MatrixFromD3D(GetTransform(D3DTS_VIEW)) *
                     MatrixFromD3D(GetTransform(D3DTS_PROJECTION));
        float min_sx = FLT_MAX, min_sy = FLT_MAX;
        float max_sx = -FLT_MAX, max_sy = -FLT_MAX;
        bool any_visible = false;
        for (UINT i = 0; i < scanned; ++i) {
          float f[3];
          memcpy(f, verts0 + static_cast<size_t>(i) * stride0, sizeof(f));
          Vector4 clip =
              Vector4::Transform(Vector4(f[0], f[1], f[2], 1.f), wvp);
          if (clip.w <= 1e-4f) continue;
          any_visible = true;
          const float sx = (clip.x / clip.w * 0.5f + 0.5f) * viewport_.Width;
          const float sy =
              (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * viewport_.Height;
          min_sx = std::min(min_sx, sx);
          max_sx = std::max(max_sx, sx);
          min_sy = std::min(min_sy, sy);
          max_sy = std::max(max_sy, sy);
        }
        static uint64_t drawbbox_seq = 0;
        if (any_visible) {
          ++drawbbox_seq;
          GpuTexture *tex0 =
              bound_textures_[0] ? bound_textures_[0].Get() : nullptr;
          LOG(AixLog::Severity::error)
              << "DRAWBBOX seq=" << drawbbox_seq << " frame=" << CurrentFrame()
              << " tex0=" << tex0 << " cullmode=" << render_state_.cull_mode
              << " bbox=(" << min_sx << "," << min_sy << ")-(" << max_sx
              << "," << max_sy << ")\n";
        }
      }
    }
  }
#endif

  const HRESULT prepare_result = PrepareDrawCall(
      PrimitiveType, minIndex + bound_base_vertex_, NumVertices);
  if (!SUCCEEDED(prepare_result)) {
#ifdef DX8TO12_ENABLE_MINDEBUG
    RecordRwIndexedEvent(rw_indexed_caller,
                         RwIndexedEvent::PrepareFailed);
#endif
    return prepare_result;
  }

  TransitionBuffer(bound_index_buffer_.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER);
  // Whole buffer, for the same reason the vertex buffer view above uses it.
  D3D12_INDEX_BUFFER_VIEW ib_view{
      .BufferLocation = bound_index_buffer_->GetGpuPtr(),
      .SizeInBytes =
          safe_cast<UINT>(bound_index_buffer_->resource_desc().Width),
      .Format = bound_index_buffer_->index_buffer_fmt()};
  MarkResourceAsUsed(bound_index_buffer_);
  cmd_list_->IASetIndexBuffer(&ib_view);

  // Determine how much of the draw fits in the bound index buffer. The game
  // occasionally issues a draw whose (startIndex + index_count) reaches past
  // the buffer it bound -- confirmed live via the D3D12 debug layer
  // (COMMAND_LIST_DRAW_INDEX_BUFFER_TOO_SMALL, EXECUTION WARNING #213) on
  // otherwise-ordinary draws, not just at any one known-bad spot. Real D3D8
  // (no GPU-side bounds validation at all) just reads whatever bytes happen
  // to follow the buffer in the allocator's memory and carries on -- usually
  // harmless garbage, sometimes not, but never a rejected/dropped draw. D3D12
  // is stricter: reading past the bound view's declared size is undefined
  // behavior, and on this driver has been observed to make the whole draw
  // (or the out-of-range tail of it) not rasterize at all. The original
  // workaround truncated the count. A GTA VC capture now shows this happens
  // on a stable 37 draws per frame exactly where a road material disappears,
  // while the working D3D11 port submits the original count. Keep both paths
  // available for a controlled compatibility A/B rather than silently
  // changing the clean release before the result is known.
  const UINT index_size =
      bound_index_buffer_->index_buffer_fmt() == DXGI_FORMAT_R32_UINT ? 4 : 2;
  const UINT indices_in_buffer =
      safe_cast<UINT>(bound_index_buffer_->resource_desc().Width) /
      index_size;
  UINT clamped_index_count = static_cast<UINT>(index_count);
  if (startIndex >= indices_in_buffer) {
    clamped_index_count = 0;
  } else if (startIndex + clamped_index_count > indices_in_buffer) {
    clamped_index_count = indices_in_buffer - startIndex;
  }
#ifdef DX8TO12_PASSTHROUGH_OOB_INDICES
  const UINT submitted_index_count = static_cast<UINT>(index_count);
#else
  const UINT submitted_index_count = clamped_index_count;
#endif
  // Phase 2 RT bookkeeping deliberately observes only stable indexed triangle
  // lists.  Dynamic buffers point at a changing ring allocation and are
  // omitted until a later phase has a safe per-frame geometry upload path.
  if (native_raytracing_supported() && GetConfig().lighting_mode >= 2 &&
      PrimitiveType == D3DPT_TRIANGLELIST && submitted_index_count >= 3 &&
      bound_vertex_streams_[0] && !bound_vertex_streams_[0]->IsDynamic() &&
      !bound_index_buffer_->IsDynamic()) {
    if (!raytracing_scene_) raytracing_scene_ = std::make_unique<RaytracingScene>(this);
    const UINT material_srv_index =
        bound_textures_[0] ? srv_heap_.GetIndexFor(bound_textures_[0]->srv_handle())
                           : UINT_MAX;
    raytracing_scene_->RecordIndexedTriangleList(
        bound_vertex_streams_[0], bound_index_buffer_,
        bound_vertex_stream_strides_[0], startIndex, submitted_index_count,
        bound_base_vertex_, GetTransform(D3DTS_WORLD), material_srv_index);
  }
#ifdef DX8TO12_ENABLE_VALIDATION
  if (clamped_index_count != static_cast<UINT>(index_count)) {
    {
      LOG(AixLog::Severity::error)
          << "DRAW-INDEX-CLAMPED frame=" << CurrentFrame()
          << " requested=" << index_count << " startIndex=" << startIndex
          << " indicesInBuffer=" << indices_in_buffer
          << " clampedTo=" << clamped_index_count << "\n";
    }
  }
#endif

#ifdef DX8TO12_ENABLE_MINDEBUG
  if (clamped_index_count == 0) {
    RecordRwIndexedEvent(rw_indexed_caller, RwIndexedEvent::ZeroClamp);
  } else {
    if (clamped_index_count != static_cast<UINT>(index_count)) {
      RecordRwIndexedEvent(rw_indexed_caller,
                           RwIndexedEvent::PartialClamp);
    }
  }
  if (submitted_index_count != 0)
    RecordRwIndexedEvent(rw_indexed_caller, RwIndexedEvent::Emitted);

  if (RwSnapshotActive()) {
    auto texture_ptr = [&](int stage) -> GpuTexture * {
      return bound_textures_[stage] ? bound_textures_[stage].Get() : nullptr;
    };
    auto append_texture = [&](std::ostringstream &line, int stage) {
      GpuTexture *texture = texture_ptr(stage);
      line << " t" << stage << "=" << texture;
      if (texture) {
        const D3D12_RESOURCE_DESC &desc = texture->resource_desc();
        line << ":" << desc.Width << "x" << desc.Height << ":m"
             << desc.MipLevels << ":f" << static_cast<int>(desc.Format);
      }
    };

    uint64_t world_hash = 1469598103934665603ull;
    auto world_it = transforms_.find(D3DTS_WORLD);
    if (world_it != transforms_.end()) {
      const auto *bytes = reinterpret_cast<const uint8_t *>(&world_it->second);
      for (size_t i = 0; i < sizeof(world_it->second); ++i) {
        world_hash ^= bytes[i];
        world_hash *= 1099511628211ull;
      }
    }

    const TextureStageState &s0 = texture_stage_states_[0];
    const TextureStageState &s1 = texture_stage_states_[1];
    const TextureStageState &s2 = texture_stage_states_[2];

    // Locate this draw on screen from the indices and vertices actually bound
    // to D3D12.  The earlier DRAWBBOX diagnostic used minIndex/NumVertices as
    // a contiguous range.  That is legal metadata, but it can include many
    // vertices this draw never references and makes unrelated road chunks
    // appear to cover the same pixel.  Walking the submitted index span gives
    // us a useful key for comparing the road under the crosshair before and
    // after it disappears.
    bool bbox_available = false;
    bool bbox_covers_center = false;
    bool triangle_covers_center = false;
    float center_depth = FLT_MAX;
    UINT center_triangle_count = 0;
    float bbox_min_x = FLT_MAX;
    float bbox_min_y = FLT_MAX;
    float bbox_max_x = -FLT_MAX;
    float bbox_max_y = -FLT_MAX;
    float bbox_min_z = FLT_MAX;
    float bbox_max_z = -FLT_MAX;
    UINT bbox_vertices = 0;
    Buffer *vb0 = bound_vertex_streams_[0]
                      ? static_cast<Buffer *>(bound_vertex_streams_[0].Get())
                      : nullptr;
    Buffer *ib0 = bound_index_buffer_.Get();
    const UINT stride0 = bound_vertex_stream_strides_[0];
    const UINT snapshot_index_size =
        ib0->index_buffer_fmt() == DXGI_FORMAT_R32_UINT ? 4u : 2u;
    const uint64_t ib_byte_offset =
        static_cast<uint64_t>(startIndex) * snapshot_index_size;
    const uint64_t ib_byte_size =
        static_cast<uint64_t>(submitted_index_count) * snapshot_index_size;
    const char *indices = nullptr;
    if (ib_byte_offset + ib_byte_size <= ib0->resource_desc().Width &&
        ib_byte_offset <= INT_MAX && ib_byte_size <= INT_MAX) {
      indices = ib0->DebugCpuPtr(static_cast<int>(ib_byte_offset),
                                 static_cast<int>(ib_byte_size));
    }
    if (vb0 && indices && stride0 >= 12u && stride0 != 28u) {
      using ::DirectX::SimpleMath::Matrix;
      using ::DirectX::SimpleMath::Vector4;
      struct ProjectedVertex {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        bool valid = false;
      };
      const Matrix wvp = MatrixFromD3D(GetTransform(D3DTS_WORLD)) *
                         MatrixFromD3D(GetTransform(D3DTS_VIEW)) *
                         MatrixFromD3D(GetTransform(D3DTS_PROJECTION));
      // Keep indices consecutive because the center test below reconstructs
      // actual triangles. GTA's streamed world meshes are comfortably below
      // this cap; it exists only to keep a pathological call from changing
      // the high-FPS timing we are trying to observe.
      const UINT processed_indices =
          std::min<UINT>(submitted_index_count, 16384u);
      std::vector<ProjectedVertex> projected(processed_indices);
      for (UINT position = 0; position < processed_indices; ++position) {
        UINT index = 0;
        if (snapshot_index_size == 4u) {
          memcpy(&index, indices + static_cast<size_t>(position) * 4u,
                 sizeof(index));
        } else {
          uint16_t short_index = 0;
          memcpy(&short_index, indices + static_cast<size_t>(position) * 2u,
                 sizeof(short_index));
          index = short_index;
        }
        const uint64_t vertex =
            static_cast<uint64_t>(bound_base_vertex_) + index;
        const uint64_t vb_byte_offset = vertex * stride0;
        if (vb_byte_offset + 12u > vb0->resource_desc().Width ||
            vb_byte_offset > INT_MAX) {
          continue;
        }
        const char *vertex_data =
            vb0->DebugCpuPtr(static_cast<int>(vb_byte_offset), 12);
        if (!vertex_data) continue;
        float position_xyz[3];
        memcpy(position_xyz, vertex_data, sizeof(position_xyz));
        if (!std::isfinite(position_xyz[0]) ||
            !std::isfinite(position_xyz[1]) ||
            !std::isfinite(position_xyz[2])) {
          continue;
        }
        const Vector4 clip = Vector4::Transform(
            Vector4(position_xyz[0], position_xyz[1], position_xyz[2], 1.f),
            wvp);
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
            !std::isfinite(clip.z) || !std::isfinite(clip.w) ||
            clip.w <= 1e-4f) {
          continue;
        }
        const float ndc_x = clip.x / clip.w;
        const float ndc_y = clip.y / clip.w;
        const float ndc_z = clip.z / clip.w;
        const float screen_x = viewport_.TopLeftX +
            (ndc_x * 0.5f + 0.5f) * viewport_.Width;
        const float screen_y = viewport_.TopLeftY +
            (1.f - (ndc_y * 0.5f + 0.5f)) * viewport_.Height;
        bbox_min_x = std::min(bbox_min_x, screen_x);
        bbox_max_x = std::max(bbox_max_x, screen_x);
        bbox_min_y = std::min(bbox_min_y, screen_y);
        bbox_max_y = std::max(bbox_max_y, screen_y);
        bbox_min_z = std::min(bbox_min_z, ndc_z);
        bbox_max_z = std::max(bbox_max_z, ndc_z);
        projected[position] = {screen_x, screen_y, ndc_z, true};
        ++bbox_vertices;
      }
      bbox_available = bbox_vertices != 0;
      if (bbox_available) {
        const float center_x = viewport_.TopLeftX + viewport_.Width * 0.5f;
        const float center_y = viewport_.TopLeftY + viewport_.Height * 0.5f;
        bbox_covers_center = bbox_min_x <= center_x && bbox_max_x >= center_x &&
                             bbox_min_y <= center_y && bbox_max_y >= center_y;
        auto test_triangle = [&](UINT ia, UINT ib, UINT ic) {
          const ProjectedVertex &a = projected[ia];
          const ProjectedVertex &b = projected[ib];
          const ProjectedVertex &c = projected[ic];
          if (!a.valid || !b.valid || !c.valid) return;
          const float area = (b.x - a.x) * (c.y - a.y) -
                             (b.y - a.y) * (c.x - a.x);
          if (std::abs(area) < 1e-6f) return;
          const float w0 = ((b.x - center_x) * (c.y - center_y) -
                            (b.y - center_y) * (c.x - center_x)) /
                           area;
          const float w1 = ((c.x - center_x) * (a.y - center_y) -
                            (c.y - center_y) * (a.x - center_x)) /
                           area;
          const float w2 = 1.f - w0 - w1;
          constexpr float kEdgeTolerance = -1e-5f;
          if (w0 < kEdgeTolerance || w1 < kEdgeTolerance ||
              w2 < kEdgeTolerance) {
            return;
          }
          triangle_covers_center = true;
          ++center_triangle_count;
          center_depth =
              std::min(center_depth, w0 * a.z + w1 * b.z + w2 * c.z);
        };
        if (PrimitiveType == D3DPT_TRIANGLELIST) {
          for (UINT i = 0; i + 2 < processed_indices; i += 3)
            test_triangle(i, i + 1, i + 2);
        } else if (PrimitiveType == D3DPT_TRIANGLESTRIP) {
          for (UINT i = 0; i + 2 < processed_indices; ++i)
            test_triangle(i, i + 1, i + 2);
        }
      }
    }

    std::ostringstream line;
    line << "RWSNAP label=" << g_rw_snapshot_label
         << " caller=";
    if (rw_indexed_caller < kRwIndexedDrawCallSites.size()) {
      line << "+0x" << std::hex
           << (kRwIndexedDrawCallSites[rw_indexed_caller] -
               kGtaPreferredImageBase)
           << std::dec;
    } else {
      line << "unmatched";
    }
    line << " prim=" << PrimitiveType << " pc=" << primCount
         << " min=" << minIndex << " nv=" << NumVertices
         << " si=" << startIndex << " ic=" << submitted_index_count
         << " base=" << bound_base_vertex_
         << " vb="
         << (bound_vertex_streams_[0] ? bound_vertex_streams_[0].Get()
                                      : nullptr)
         << " stride=" << bound_vertex_stream_strides_[0]
         << " ib=" << bound_index_buffer_.Get();
    if (vb0) line << " vbState={" << vb0->DebugState() << "}";
    if (ib0) line << " ibState={" << ib0->DebugState() << "}";
    if (bbox_available) {
      line << " bbox=(" << bbox_min_x << "," << bbox_min_y << ")-("
           << bbox_max_x << "," << bbox_max_y << ")"
           << " bz=" << bbox_min_z << ":" << bbox_max_z
           << " bboxCenter=" << bbox_covers_center
           << " center=" << triangle_covers_center
           << " hz=" << (triangle_covers_center ? center_depth : -1.f)
           << " ht=" << center_triangle_count << " bvn=" << bbox_vertices;
    } else {
      line << " bbox=unavailable bboxCenter=0 center=0 hz=-1 ht=0 bvn=0";
    }
    append_texture(line, 0);
    append_texture(line, 1);
    append_texture(line, 2);
    line << " vs=" << bound_vertex_shader_ << " ps=" << bound_pixel_shader_
         << " wh=0x" << std::hex << world_hash << std::dec;
    if (world_it != transforms_.end()) {
      line << " wt=(" << world_it->second._41 << "," << world_it->second._42
           << "," << world_it->second._43 << ")";
    } else {
      line << " wt=(0,0,0)";
    }
    line
         << " rs=z" << static_cast<int>(render_state_.zbuffer_type)
         << ":zw" << render_state_.zwrite_enable
         << ":zf" << static_cast<int>(render_state_.z_func)
         << ":c" << static_cast<int>(render_state_.cull_mode)
         << ":at" << render_state_.alpha_test_enable
         << ":af" << static_cast<int>(render_state_.alpha_func)
         << ":ar" << render_state_.alpha_ref
         << ":ab" << render_state_.alpha_blend_enable
         << ":sb" << static_cast<int>(render_state_.src_blend)
         << ":db" << static_cast<int>(render_state_.dest_blend)
         << ":cw0x" << std::hex << render_state_.color_write_enable << std::dec
         << " s0=" << static_cast<int>(s0.color_op) << "/"
         << s0.color_arg1 << "/" << s0.color_arg2 << "/"
         << static_cast<int>(s0.alpha_op) << "/" << s0.alpha_arg1 << "/"
         << s0.alpha_arg2 << "/tc" << s0.texcoord_index
         << " s1=" << static_cast<int>(s1.color_op) << "/"
         << s1.color_arg1 << "/" << s1.color_arg2 << "/"
         << static_cast<int>(s1.alpha_op) << "/" << s1.alpha_arg1 << "/"
         << s1.alpha_arg2 << "/tc" << s1.texcoord_index
         << " s2=" << static_cast<int>(s2.color_op) << "/"
         << s2.color_arg1 << "/" << s2.color_arg2 << "/"
         << static_cast<int>(s2.alpha_op) << "/" << s2.alpha_arg1 << "/"
         << s2.alpha_arg2 << "/tc" << s2.texcoord_index << "\n";
    AppendRwSnapshotLine(line.str());
    if (triangle_covers_center && world_it != transforms_.end()) {
      AppendRwSnapshotLine(DescribeGtaCenterDrawEntities(
          world_it->second._41, world_it->second._42, world_it->second._43));
    }
  }
#endif

  cmd_list_->DrawIndexedInstanced(submitted_index_count, 1, startIndex,
                                  bound_base_vertex_, 0);
  if (rt_helper_client_ && rt_helper_client_->ready()) {
    rt_helper_client_->ObserveIndexedDraw(
        PrimitiveType,
        bound_vertex_streams_[0] ? bound_vertex_streams_[0].Get() : nullptr,
        bound_index_buffer_ ? bound_index_buffer_.Get() : nullptr);
  }
  // H3 capture copies CPU-visible geometry into a bounded file mapping. It
  // never adds a resource barrier/copy to the game's D3D12 command list.
  if (kEnableExperimentalHelperSceneSubmission &&
      GetConfig().lighting_mode >= 2 && rt_helper_client_ &&
      rt_helper_client_->ready() &&
      PrimitiveType == D3DPT_TRIANGLELIST && bound_vertex_streams_[0] &&
      bound_index_buffer_ &&
      !bound_vertex_streams_[0]->IsDynamic() && !bound_index_buffer_->IsDynamic()) {
    rt_helper_client_->RecordStaticTriangle(
        bound_vertex_streams_[0].Get(), bound_index_buffer_.Get(),
        bound_vertex_stream_strides_[0], startIndex, submitted_index_count,
        bound_base_vertex_, minIndex, NumVertices, GetTransform(D3DTS_WORLD),
        bound_textures_[0] ? srv_heap_.GetIndexFor(bound_textures_[0]->srv_handle())
                           : UINT_MAX,
        static_cast<UINT>(bound_index_buffer_->index_buffer_fmt()),
        RtCurrentNormalByteOffset());
  }
  if (kEnableExperimentalHelperSceneSubmission &&
      GetConfig().lighting_mode >= 2 && rt_helper_client_ &&
      rt_helper_client_->ready() &&
      PrimitiveType == D3DPT_TRIANGLELIST && bound_vertex_streams_[0] &&
      bound_index_buffer_ &&
      bound_vertex_streams_[0]->IsDynamic() && bound_index_buffer_->IsDynamic() &&
      (CurrentFrame() & 3u) == 0) {
    rt_helper_client_->RecordDynamicTriangle(
        static_cast<DynamicBuffer*>(bound_vertex_streams_[0].Get()),
        static_cast<DynamicBuffer*>(bound_index_buffer_.Get()),
        bound_vertex_stream_strides_[0], startIndex, submitted_index_count,
        bound_base_vertex_, minIndex, NumVertices, GetTransform(D3DTS_WORLD),
        bound_textures_[0] ? srv_heap_.GetIndexFor(bound_textures_[0]->srv_handle()) : UINT_MAX,
        static_cast<UINT>(bound_index_buffer_->index_buffer_fmt()),
        RtCurrentNormalByteOffset());
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
    UINT NumVertexIndices, UINT PrimitiveCount, CONST void *pIndexData,
    D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData,
    UINT VertexStreamZeroStride) {
  ++draw_calls_this_frame_;
  TRACE_ENTRY(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount,
             pIndexData, IndexDataFormat, pVertexStreamZeroData,
             VertexStreamZeroStride);
  if (!bound_vertex_shader_) {
    LOG_ERROR()
        << "Cannot use DrawIndexedPrimitiveUP without a vertex shader.\n";
    return D3DERR_INVALIDCALL;
  }
  if (IndexDataFormat != D3DFMT_INDEX16 && IndexDataFormat != D3DFMT_INDEX32) {
    LOG_ERROR() << "Invalid IndexDataFormat for DrawIndexedPrimitiveUP: "
                << IndexDataFormat << "\n";
    return D3DERR_INVALIDCALL;
  }
  if (pIndexData == nullptr || pVertexStreamZeroData == nullptr ||
      VertexStreamZeroStride == 0) {
    LOG_ERROR() << "Invalid DrawIndexedPrimitiveUP arguments: pIndexData="
                << pIndexData << " pVertexStreamZeroData="
                << pVertexStreamZeroData
                << " VertexStreamZeroStride=" << VertexStreamZeroStride
                << "\n";
    return D3DERR_INVALIDCALL;
  }
  // Not supported: PrepareDrawCall rejects fans outright, and rewriting a fan
  // index list (as opposed to DrawPrimitiveUP's flat vertex list) isn't
  // implemented.
  ASSERT(PrimitiveType != D3DPT_TRIANGLEFAN);

  int index_count;
  switch (PrimitiveType) {
    case D3DPT_LINELIST:
      index_count = 2 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLELIST:
      index_count = 3 * PrimitiveCount;
      break;
    case D3DPT_TRIANGLESTRIP:
      index_count = 2 + PrimitiveCount;
      break;
    default:
      FAIL("TODO: Count number of indices for PrimitiveType of %d",
           PrimitiveType);
      break;
  }

  // Upload the vertex data. MinVertexIndex/NumVertexIndices describe the
  // range of vertices this call actually touches, but pVertexStreamZeroData
  // is indexed from element 0 (indices in pIndexData are absolute, not
  // relative to MinVertexIndex), so we have to bring along everything up to
  // the top of that range.
  // Same diagnostic as DrawPrimitiveUP, on the path RenderWare's 2D drawing
  // actually takes (RwIm2DRenderIndexedPrimitive lands here, not on the
  // non-indexed one). Reports the vertex data exactly as the game supplied
  // it, before it's copied anywhere. See plan/ROADMAP.md for the glitch this is
  // chasing.
  if (VertexStreamZeroStride >= 2 * sizeof(float) && NumVertexIndices <= 64) {
    float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
    for (UINT i = 0; i < NumVertexIndices; ++i) {
      const float *pos = reinterpret_cast<const float *>(
          static_cast<const uint8_t *>(pVertexStreamZeroData) +
          static_cast<size_t>(MinVertexIndex + i) * VertexStreamZeroStride);
      min_x = std::min(min_x, pos[0]);
      max_x = std::max(max_x, pos[0]);
      min_y = std::min(min_y, pos[1]);
      max_y = std::max(max_y, pos[1]);
    }
    const bool covers_screen = (max_x - min_x) > 0.8f * viewport_.Width &&
                               (max_y - min_y) > 0.8f * viewport_.Height;
    if (covers_screen) {
      static int oversized_indexed_up_draws = 0;
      if (oversized_indexed_up_draws < 16) {
        ++oversized_indexed_up_draws;
        LOG(AixLog::Severity::error)
            << "OVERSIZED-UI-DRAW(indexed): prim=" << PrimitiveType
            << " verts=" << NumVertexIndices << " prims=" << PrimitiveCount
            << " stride=" << VertexStreamZeroStride << " bounds=(" << min_x
            << "," << min_y << ")-(" << max_x << "," << max_y
            << ") viewport=" << viewport_.Width << "x" << viewport_.Height
            << " tex0="
            << (bound_textures_[0] ? bound_textures_[0].Get() : nullptr)
            << "\n";
      }
    }
    if (ui_dump_enabled_) {
      std::ostringstream dump;
      dump << "UIDUMP(indexed) prim=" << PrimitiveType
           << " verts=" << NumVertexIndices << " prims=" << PrimitiveCount
           << " stride=" << VertexStreamZeroStride << " tex0="
           << (bound_textures_[0] ? bound_textures_[0].Get() : nullptr)
           << " pos=[";
      for (UINT i = 0; i < NumVertexIndices && i < 8; ++i) {
        const float *pos = reinterpret_cast<const float *>(
            static_cast<const uint8_t *>(pVertexStreamZeroData) +
            static_cast<size_t>(MinVertexIndex + i) * VertexStreamZeroStride);
        dump << "(" << pos[0] << "," << pos[1] << ") ";
      }
      dump << "]\n";
      LOG(AixLog::Severity::error) << dump.str();
    }
  }

  const size_t num_vertices_to_upload = MinVertexIndex + NumVertexIndices;
  const size_t vertex_bytes = num_vertices_to_upload * VertexStreamZeroStride;
  DynamicRingBuffer::Allocation vertex_alloc =
      dynamic_ring_buffer()->Allocate(vertex_bytes);
  memcpy(dynamic_ring_buffer()->GetCpuPtrFor(vertex_alloc),
         pVertexStreamZeroData, vertex_bytes);
  D3D12_VERTEX_BUFFER_VIEW vbuffer_view{
      .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(vertex_alloc),
      .SizeInBytes = safe_cast<UINT>(vertex_bytes),
      .StrideInBytes = VertexStreamZeroStride};

  // Upload the index data.
  const DXGI_FORMAT index_format = DXGIFromD3DFormat(IndexDataFormat);
  const size_t index_bytes =
      static_cast<size_t>(index_count) * DXGIFormatSize(index_format);
  DynamicRingBuffer::Allocation index_alloc =
      dynamic_ring_buffer()->Allocate(index_bytes);
  memcpy(dynamic_ring_buffer()->GetCpuPtrFor(index_alloc), pIndexData,
         index_bytes);
  D3D12_INDEX_BUFFER_VIEW ib_view{
      .BufferLocation = dynamic_ring_buffer()->GetGpuPtrFor(index_alloc),
      .SizeInBytes = safe_cast<UINT>(index_bytes),
      .Format = index_format};

  ASSERT_HR(SetStreamSource(0, nullptr, 0));
  HR_OR_RETURN(PrepareDrawCall(PrimitiveType, static_cast<int>(MinVertexIndex),
                               static_cast<int>(NumVertexIndices)));
  // Overwrite whatever vertex/index buffer the prepare set. See the matching
  // note in DrawPrimitiveUP for why the view cache must be invalidated here.
  cmd_list_->IASetVertexBuffers(0, 1, &vbuffer_view);
  last_vbuffer_view_count_ = 0;
  cmd_list_->IASetIndexBuffer(&ib_view);
  cmd_list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
  return S_OK;
}

void Device::PollUiDumpHotkey() {
#ifdef DX8TO12_ENABLE_MINDEBUG
  PollRwSnapshotHotkeys(CurrentFrame());
#endif
#ifdef DX8TO12_ENABLE_VALIDATION
  // Edge-triggered so holding the key doesn't toggle every frame. Uses
  // GetAsyncKeyState rather than the app's input: this has to work while the
  // game has focus and is running its own message loop.
  const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
  if (down && !ui_dump_key_was_down_) {
    ui_dump_enabled_ = !ui_dump_enabled_;
    // Cap the run so a forgotten toggle can't fill the disk. 1000 frames is
    // only about 2.8 seconds on a 360Hz display, which is the shortest window
    // that's realistically long enough to react and catch the glitch.
    ui_dump_frames_left_ = ui_dump_enabled_ ? 1000 : 0;
    LOG(AixLog::Severity::error)
        << "=== UI DUMP " << (ui_dump_enabled_ ? "STARTED" : "STOPPED")
        << " (F9) ===\n";
  }
  ui_dump_key_was_down_ = down;

  if (ui_dump_enabled_) {
    if (--ui_dump_frames_left_ <= 0) {
      ui_dump_enabled_ = false;
      LOG(AixLog::Severity::error)
          << "=== UI DUMP STOPPED (frame budget reached) ===\n";
    }
  }
#endif
}

HRESULT STDMETHODCALLTYPE Device::Present(CONST RECT *pSourceRect,
                                          CONST RECT *pDestRect,
                                          HWND hDestWindowOverride,
                                          CONST RGNDATA *pDirtyRegion) {
  TRACE_ENTRY(hDestWindowOverride);
  ASSERT(hDestWindowOverride == nullptr || hDestWindowOverride == window_);
  PollUiDumpHotkey();
#if defined(DX8TO12_SCENE_TARGET) || defined(DX8TO12_MOTION_VECTORS)
  // At Present, i.e. on a frame boundary: rebuilding the scene resources
  // mid-frame would pull them out from under draws already recorded.
  PollGraphicsHotkey();
#endif
  SubmitAndWait(true);
  return S_OK;
}

// Only used during reset. Does not clean up fence state.
void Device::SubmitAndWait(bool should_present) {
  if (should_present && rt_helper_client_ && !rt_helper_client_->ready() &&
      CurrentFrame() >= 300) {
    rt_helper_client_->Start();
  }
  // TEMP DIAGNOSTIC: see perf_wait_ticks_accum_ comment in device.h.
  LARGE_INTEGER perf_now;
  QueryPerformanceCounter(&perf_now);
  if (should_present) {
    if (perf_last_frame_ticks_ != 0) {
      perf_frame_ticks_accum_ += perf_now.QuadPart - perf_last_frame_ticks_;
      perf_wait_ticks_accum_ += perf_wait_ticks_this_frame_;
      ++perf_frame_sample_count_;
// Also in a DX8TO12_PERF_LOG build. Measuring a feature's cost on a dev build
// measures the debug layer instead -- that alone roughly halves the frame
// rate, which is far more than anything being measured here.
#if defined(DX8TO12_ENABLE_VALIDATION) || defined(DX8TO12_PERF_LOG)
      if (perf_frame_sample_count_ >= 120) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double avg_frame_ms = 1000.0 * perf_frame_ticks_accum_ /
                              perf_frame_sample_count_ / freq.QuadPart;
        double avg_wait_ms = 1000.0 * perf_wait_ticks_accum_ /
                             perf_frame_sample_count_ / freq.QuadPart;
        LOG(AixLog::Severity::error)
            << "Perf: avg frame=" << avg_frame_ms
            << "ms (fps=" << (1000.0 / avg_frame_ms)
            << ") avg GPU-fence-wait=" << avg_wait_ms << "ms ("
            << (100.0 * avg_wait_ms / avg_frame_ms) << "% of frame)"
            // Per window, not since start: a black frame is an event, and a
            // running total cannot say whether one happened just now.
            << " frames upscaled=" << (frames_upscaled_ - perf_last_upscaled_)
            << " fallback=" << (frames_fallback_ - perf_last_fallback_)
            << " bypassed=" << (frames_bypassed_ - perf_last_bypassed_)
            << "\n";
        perf_last_upscaled_ = frames_upscaled_;
        perf_last_fallback_ = frames_fallback_;
        perf_last_bypassed_ = frames_bypassed_;
        perf_frame_ticks_accum_ = 0;
        perf_wait_ticks_accum_ = 0;
        perf_frame_sample_count_ = 0;
      }
#endif
    }
    if (perf_last_frame_ticks_ != 0) {
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      last_frame_ms_ = 1000.0 *
                       static_cast<double>(perf_now.QuadPart -
                                           perf_last_frame_ticks_) /
                       static_cast<double>(freq.QuadPart);
    }
    perf_last_frame_ticks_ = perf_now.QuadPart;
    perf_wait_ticks_this_frame_ = 0;
  }

  ASSERT(!(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED));

  // Let mod-API render callbacks draw on top of this frame's backbuffer
  // while it's still bound and the command list is still open -- see
  // RegisterModRenderCallback. BeginScene only actually runs lazily, on a
  // game's first draw/Clear call of the frame (DIRTY_FLAG_OM) -- a frame
  // presented with zero game draws (e.g. right after Reset(), or a blank
  // loading-screen frame) would otherwise reach here with no render target
  // ever bound and the backbuffer still sitting in D3D12_RESOURCE_STATE_
  // PRESENT, not RENDER_TARGET. A mod callback recording into that raw,
  // uninitialized command list is a well-known way to crash the GPU driver
  // (observed: an access violation inside nvwgf2um.dll while recording a
  // mod's DrawIndexedInstanced). Force BeginScene here so the contract
  // MODDING.md documents -- backbuffer bound as the active render target --
  // actually holds on every presented frame, not just ones the game itself
  // drew into.
  if (should_present) {
#ifdef DX8TO12_MOTION_VECTORS
    // Before the scene resolve: this reads the depth buffer as the game's
    // own draws left it, and the debug view writes onto the scene target
    // while it is still the thing that gets copied to the backbuffer.
    RecordMotionVectorPass();
#endif
#ifdef DX8TO12_SCENE_TARGET
    {
      // Start, restart or stop the helper purely from the setting, so
      // toggling TemporalAA at runtime works without a device Reset. A
      // resolution change restarts it too -- the shared textures are sized
      // once, at Start.
      const int mode = GetConfig().temporal_aa;
      const auto &backbuffer_desc =
          back_buffers_.at(current_back_buffer_)->resource_desc();
      const uint32_t width = static_cast<uint32_t>(backbuffer_desc.Width);
      const uint32_t height = backbuffer_desc.Height;
      // Restart on a render-resolution change too: the shared textures are
      // sized once, at Start.
      if (mode != dlss_started_mode_ || width != dlss_started_width_ ||
          height != dlss_started_height_ ||
          scene_render_width_ != dlss_started_render_width_) {
        dlss_started_render_width_ = scene_render_width_;
        if (dlss_client_) dlss_client_->Stop();
        dlss_started_mode_ = mode;
        dlss_started_width_ = width;
        dlss_started_height_ = height;
        if (mode != 0) {
          if (!dlss_client_) dlss_client_ = std::make_unique<DlssClient>(this);
          const DlssIpc::Mode helper_mode = mode == 2 ? DlssIpc::Mode::kDlss
                                                      : DlssIpc::Mode::kDlaa;
          LOG(AixLog::Severity::info)
              << "DLSS: starting helper, TemporalAA=" << mode << " -> mode "
              << static_cast<uint32_t>(helper_mode) << ".\n";
          dlss_client_->Start(scene_render_width_, scene_render_height_, width,
                              height, helper_mode);
        }
      }
    }
#endif
#ifdef DX8TO12_SCENE_TARGET
    // Everything from here on -- mod callbacks, HUD overlays, the PRESENT
    // transition -- must see the real backbuffer, exactly as it did before
    // the scene target existed. MODDING.md promises mods the backbuffer, in
    // its format, at output resolution, and they only rebuild their PSOs when
    // GetSwapChainGeneration changes; handing them anything else here would
    // invalidate their pipeline state with no signal at all.
    //
    // Before the BeginScene below, not after: the copy leaves the backbuffer
    // in COPY_DEST with no RTV bound, and ResolveScenePass sets
    // DIRTY_FLAG_OM so that BeginScene unconditionally rebinds it as a
    // render target.
    // PollReady, not ready(): it is what advances the startup state machine
    // and what notices a helper that died. Gating it on ready() made the
    // check unreachable -- the helper launched, never reported ready, and
    // nothing ever said so, because the code that would have noticed only
    // ran once it already had.
    // frame_had_3d_draw_, not just scene_pass_active_. A frame the game drew
    // entirely in 2D -- the menu, a loading screen, a fade -- has no depth
    // worth reprojecting and no motion vectors that mean anything, so handing
    // it to a temporal upscaler asks it to reconstruct from a history that
    // does not correspond to it. What comes back is a heavily flickering
    // image, and in the main menu that leaves the player unable to see the
    // entry they are selecting.
    //
    // EndScenePassIfDrawIsUi already declines to end the pass on a 2D draw
    // that no 3D draw preceded, which covers "2D after 3D". This is the other
    // half of the same rule: a frame with no 3D draw at all never belongs to
    // the upscaler either. Whether it was reached depended on whether the
    // helper happened to become ready while the menu was still up, which is
    // why this came and went between launches of the same build.
    // Not while the window is hidden. A minimised game still renders, and
    // what it renders is black; feeding that to a temporal upscaler fills its
    // history and its auto exposure with darkness, and the picture that comes
    // back on restore is black apart from the brightest sprites.
    //
    // Rebuilding the feature on the device Reset alone did not fix this,
    // because the Reset arrives when the device is *lost* -- while the window
    // is still hidden -- so the rebuilt feature was immediately fed the same
    // black frames and collapsed again. Withholding them is what makes the
    // rebuild stick, and the history reset on the way back covers the frames
    // it never saw.
    const bool window_visible =
        !window_ || (!IsIconic(window_) && IsWindowVisible(window_));
    if (!window_visible && dlss_client_) {
      dlss_client_->RequestHistoryReset();
    }
    // Coming back from hidden, restart the upscaler outright rather than
    // trying to mend it in place.
    //
    // Mending it in place is what the previous four attempts did, and it does
    // not work: freeing the feature's resources and re-applying its options
    // leaves state behind that still produces a black frame. A full restart
    // does clear it -- that is what the F8 hotkey established, by recovering
    // a picture that had been stuck black.
    //
    // It has to happen *here*, on the way back, not on the device Reset. The
    // Reset arrives while the window is still hidden, so a restart there is
    // immediately followed by the very frames that ruin it again. That timing
    // is why the in-place rebuild appeared to fire correctly and change
    // nothing.
    //
    // The cost is a couple of seconds without upscaling while the helper
    // comes up, during which the scene is presented as it was rendered. That
    // is invisible next to a picture that never comes back.
    if (window_visible && !window_was_visible_ && dlss_client_ &&
        dlss_client_->helper_running()) {
      const uint32_t render_w = dlss_client_->render_width();
      const uint32_t render_h = dlss_client_->render_height();
      const uint32_t out_w = dlss_client_->output_width();
      const uint32_t out_h = dlss_client_->output_height();
      if (render_w && out_w) {
        LOG(AixLog::Severity::error)
            << "Window shown again: restarting the upscaler, which does not "
               "survive the game losing its device.\n";
        dlss_client_->Stop();
        dlss_client_->Start(render_w, render_h, out_w, out_h,
                            GetConfig().temporal_aa == 2
                                ? DlssIpc::Mode::kDlss
                                : DlssIpc::Mode::kDlaa);
      }
    }
    window_was_visible_ = window_visible;
    if (scene_pass_active_ && frame_had_3d_draw_ && window_visible &&
        dlss_client_ && dlss_client_->PollReady()) {
      RunDlaaExchange();
    } else {
      // A frame the upscaler never saw is a hole in its history; say so
      // rather than letting the next 3D frame blend against one that came
      // before an unknown gap.
      if (scene_pass_active_ && !frame_had_3d_draw_ && dlss_client_) {
        dlss_client_->RequestHistoryReset();
        ++frames_bypassed_;
      }
      ResolveScenePass();
    }
#if defined(DX8TO12_ENABLE_VALIDATION) || defined(DX8TO12_PERF_LOG)
    // Whether the game is drawing a world at all, reported independently of
    // the upscaler -- deliberately, because it is the one thing that tells
    // these two apart. A frame with no 3D draw and a broken upscaler look
    // identical from outside the process: black, with the HUD still on top.
    // Only the transitions are logged; the state itself would be thousands of
    // identical lines.
    if (frame_had_3d_draw_) world_has_been_drawn_ = true;
    if (world_has_been_drawn_ &&
        frame_had_3d_draw_ != last_frame_had_3d_draw_ &&
        world_transition_notices_ < 20) {
      ++world_transition_notices_;
      LOG(AixLog::Severity::error)
          << (frame_had_3d_draw_
                  ? "World geometry is being submitted again.\n"
                  : "The game stopped submitting 3D draws entirely. A black "
                    "screen from here is an empty scene, not the upscaler.\n");
    }
    last_frame_had_3d_draw_ = frame_had_3d_draw_;
#endif
#endif
    if (dirty_flags_ & DIRTY_FLAG_OM) {
      BeginScene();
    }
    if (rt_helper_client_ && rt_helper_client_->ready()) {
      rt_helper_client_->PollSmokeTest();
    }
    // Build after every game draw has been collected but before any mod
    // callback can consume the TLAS.  This phase records no DispatchRays and
    // therefore cannot change the visible frame.
    if (raytracing_scene_ && GetConfig().lighting_mode >= 2) {
      raytracing_scene_->BuildForFrame();
    } else if (raytracing_scene_) {
      // A mode switch can happen between draws and Present.  Do not carry
      // geometry observed under the old mode into a later RT frame.
      raytracing_scene_->DiscardFrame();
    }
    // Do not invoke arbitrary mod code while holding the mutex: a callback
    // may legitimately unregister itself.  The snapshot makes registration
    // from another thread safe without extending the lock over foreign code.
    std::vector<ModRenderCallback> callbacks;
    {
      std::lock_guard lock(mod_render_callbacks_mutex_);
      callbacks = mod_render_callbacks_;
    }
    // Level 1 mod-API scene metadata: only pay for the extra barrier pair
    // when a mod has actually asked to read the depth buffer (see
    // RequestDepthBufferAccess) -- bound_depth_target_ otherwise sits in
    // DEPTH_WRITE for the whole frame with nothing else ever transitioning
    // it away from that state (Clear()/BeginScene's OMSetRenderTargets both
    // assume it), so this pair must be symmetric and go through
    // TransitionTexture to keep GpuTexture::current_state_ authoritative for
    // next frame.
    const bool depth_readable_for_callbacks =
        depth_buffer_access_requested_ && bound_depth_target_;
    if (depth_readable_for_callbacks) {
      // The normal DSV is writable. It cannot remain bound while the same
      // resource is transitioned to an SRV state. Mod callbacks render to
      // the color target only, so detach the DSV for this callback section;
      // merely changing the resource state while leaving the writable DSV
      // in OM is invalid D3D12 and can remove the device on release drivers.
      GpuTexture* render_target = CurrentColorTarget();
      const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_target->rtv_handle();
      cmd_list_->OMSetRenderTargets(1, &rtv_handle, TRUE, nullptr);
      TransitionTexture(bound_depth_target_.Get(), 0,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (ModRenderCallback callback : callbacks) {
      // Rebind Dx8to12's own heaps before each callback -- an earlier
      // callback in this same list may have bound its own descriptor heap
      // (e.g. to build an SRV for a borrowed resource like the H4 shadow
      // mask, see MODDING.md's H4 section: "the mod must create its
      // SRV/PSO resources itself") and left it bound afterward. Without
      // this, a later mod that only defensively rebinds against Dx8to12's
      // own prior state (the documented contract) -- not against whatever
      // a *different* mod's callback did moments earlier -- would silently
      // inherit the wrong heap: the same SET_DESCRIPTOR_TABLE_INVALID/
      // garbage-render class of bug MODDING.md already documents, just
      // triggered mod-to-mod instead of by Dx8to12's own state.
      ID3D12DescriptorHeap *heaps[] = {srv_heap_.heap(), sampler_heap_.heap()};
      cmd_list_->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);
      callback(cmd_list_.Get());
    }
    if (depth_readable_for_callbacks) {
      TransitionTexture(bound_depth_target_.Get(), 0,
                        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
  }

  // Transition back buffer to present.
  if (should_present) {
    TransitionTexture(back_buffers_[current_back_buffer_].get(), 0,
                      D3D12_RESOURCE_STATE_PRESENT);
  }

  // Persist any dynamic buffers.
  for (auto buffer : buffers_to_persist_) {
    buffer->PersistDynamicChanges();
    buffer->set_marked_for_persist(false);
  }
  buffers_to_persist_.clear();

  // Close the command list, then execute it.
  ASSERT_HR(cmd_list_->Close());
  dirty_flags_ |= DIRTY_FLAG_CMD_LIST_CLOSED;
  ID3D12CommandList *cmd_list = cmd_list_.Get();
  cmd_queue_->ExecuteCommandLists(1, &cmd_list);
  if (rt_helper_client_ && rt_helper_client_->ready()) {
    rt_helper_client_->OnX86Submission();
  }
  // Present!
  if (should_present) {
    ASSERT_HR(swap_chain_->Present(
        sync_interval_, sync_interval_ == 0 && tearing_supported_
                             ? DXGI_PRESENT_ALLOW_TEARING
                             : 0));
#ifdef DX8TO12_ENABLE_MINDEBUG
    FlushRwCallDiagnostics(next_fence_);
#endif
  }

  // DIAGNOSTIC: see draw_calls_this_frame_ comment in device.h. Logged
  // against the frame number that's ending (next_fence_ before increment),
  // tagged with should_present so a mid-frame flush (e.g. a lockable-surface
  // GPU readback) doesn't get mistaken for a real presented frame boundary
  // when cross-referencing against a RenderDoc capture.
#ifdef DX8TO12_ENABLE_VALIDATION
  LOG(AixLog::Severity::error)
      << "FRAME-DRAWCOUNT frame=" << next_fence_
      << " present=" << (should_present ? 1 : 0)
      << " draws=" << draw_calls_this_frame_ << "\n";
#endif
  draw_calls_this_frame_ = 0;

#ifdef DX8TO12_TEMPORAL_JITTER
  // One new sub-pixel offset per *presented* frame. Deliberately not on a
  // mid-frame flush: those are still the same frame as far as the camera is
  // concerned, and moving the camera inside a frame would tear the image
  // between the draws before and after the flush.
  if (should_present) AdvanceTemporalJitter();
#endif

  // Grab a new fence value, set it at the end of the command queue execution.
  fence_values_.at(current_back_buffer_) = next_fence_++;
#ifdef DX8TO12_FORCE_GPU_IDLE
  const uint64_t submitted_fence = fence_values_[current_back_buffer_];
#endif
  ASSERT_HR(cmd_queue_->Signal(cmd_list_done_fence_.get(),
                               fence_values_[current_back_buffer_]));

  // A setting a mod changed at runtime belongs in dx8to12.ini too, so it
  // survives into the next session -- the INI and the mod API are two doors
  // onto one state. Rate-limited inside, and a no-op when nothing changed.
  if (should_present) FlushConfigIfDirty(/*force=*/false);

  // Update our back buffer index.
  current_back_buffer_ = swap_chain_->GetCurrentBackBufferIndex();

#ifdef DX8TO12_SCENE_TARGET
  // A new frame begins: the game's draws go offscreen again. Only after a
  // real Present -- a mid-frame flush is still the same frame, and the scene
  // pass there was either already resolved (a backbuffer read) or must stay
  // open.
  if (should_present) {
    // Re-evaluated per frame, so a mod toggling the setting takes effect on
    // the next frame rather than needing a device Reset.
    scene_pass_active_ = SceneTargetWanted();
    frame_had_3d_draw_ = false;
    // CurrentColorTarget() changes answer here, and the freshly-reset
    // command list has no RTV bound anyway.
    dirty_flags_ |= DIRTY_FLAG_OM;
    dirty_flags_ |= DIRTY_FLAG_PSO;
  }
#endif

#ifdef DX8TO12_MOTION_VECTORS
  // Re-arm the "first draw of the frame wins" capture. Only on a presented
  // frame: a mid-frame flush has not changed the camera.
  if (should_present) frame_view_proj_captured_ = false;
#endif

#ifdef DX8TO12_FORCE_GPU_IDLE
  // Diagnostic A/B: remove every possible CPU/GPU lifetime overlap. If the
  // disappearing mesh survives this, it cannot be caused by a descriptor,
  // texture, upload-ring allocation, or dynamic buffer being reused while
  // the GPU still consumes the previous frame.
  WaitForFrame(submitted_fence);
#endif

  // Wait for it.
  WaitForFrame(fence_values_[current_back_buffer_]);

  // Reset the command list for the next frame.
  ASSERT_HR(cmd_allocators_[current_back_buffer_]->Reset());
  ASSERT_HR(
      cmd_list_->Reset(cmd_allocators_[current_back_buffer_].get(), nullptr));
  {
    // A freshly-reset command list has no descriptor heaps bound. Only
    // BeginScene() binds them (see there), and BeginScene() is only called
    // again here if should_present && DIRTY_FLAG_OM is set (see above) --
    // which is never true for a mid-frame flush (SubmitAndWait(false), e.g.
    // BaseSurface::LockGpuReadback). Without this, every draw issued for the
    // rest of that frame after a mid-frame flush uses SetGraphicsRootDescriptorTable
    // against heaps the fresh command list never had bound, which the D3D12
    // debug layer flags as "the descriptor heap containing handle ... is
    // different from currently set descriptor heap" (EXECUTION ERROR #708
    // SET_DESCRIPTOR_TABLE_INVALID) -- confirmed live, reproduced from the
    // menu's blur/reflection effect (which does exactly this kind of
    // lockable-surface readback). This is undefined behavior on the GPU side,
    // not just a validation-layer nag -- explains why the resulting bad draws
    // were inconsistent/intermittent rather than a hard crash.
    ID3D12DescriptorHeap *heaps[] = {srv_heap_.heap(), sampler_heap_.heap()};
    cmd_list_->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);
  }
  dirty_flags_ ^= DIRTY_FLAG_CMD_LIST_CLOSED;
  dirty_flags_ |= DIRTY_FLAG_ALL_RESOURCES;
  // Resetting the command list drops all IA state (topology included), so
  // the next draw must re-set it regardless of what PrepareDrawCall's own
  // redundant-set check (last_prim_topology_) thinks is currently bound.
  last_prim_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  // Likewise for the root signature, root arguments, and pipeline state.
  root_sig_bound_ = false;
  last_set_pso_ = nullptr;
  last_stencil_ref_ = -1;
  dirty_texture_stage_mask_ = 0xFF;
  dirty_sampler_stage_mask_ = 0xFF;
  last_vbuffer_view_count_ = 0;
}

void Device::WaitForFrame(uint64_t frame_number) {
  ASSERT(frame_number <= next_fence_);

  if (cmd_list_done_fence_->GetCompletedValue() < frame_number) {
    // Is this a frame that we're currently building?
    if (frame_number + 1 == next_fence_ &&
        !(dirty_flags_ & DIRTY_FLAG_CMD_LIST_CLOSED)) {
      // SubmitAndWait will call us again to wait for the frame, but at that
      // point fence_values_[current_back_buffer_] will have incremented.
      SubmitAndWait(false);
    } else {
      ASSERT_HR(cmd_list_done_fence_->SetEventOnCompletion(
          frame_number, cmd_list_done_event_handle_));
      // TEMP DIAGNOSTIC: see perf_wait_ticks_accum_ comment in device.h.
      LARGE_INTEGER wait_start, wait_end;
      QueryPerformanceCounter(&wait_start);
      DWORD wait_result =
          WaitForSingleObjectEx(cmd_list_done_event_handle_, 60 * 1000, FALSE);
      QueryPerformanceCounter(&wait_end);
      perf_wait_ticks_this_frame_ += wait_end.QuadPart - wait_start.QuadPart;
      if (wait_result != WAIT_OBJECT_0) {
        // The fence never signaled -- most likely the GPU driver hung and
        // got TDR-reset (device removed), or genuinely never finished this
        // workload. Previously this return value was ignored entirely: on
        // timeout, execution fell straight through to FreeFrameResources()
        // below as if the wait had succeeded, freeing/reusing resources the
        // GPU might still (think it) owns -- silent corruption instead of a
        // diagnosable failure, observed in practice as an unresponsive
        // ~60s "freeze" followed by the process dying with no error shown.
        HRESULT removed_reason = d3d12_device_->GetDeviceRemovedReason();
        FAIL(
            "WaitForFrame: fence %llu never signaled after 60s (wait_result="
            "0x%X). GetDeviceRemovedReason=0x%X -- the GPU driver likely "
            "hung or was TDR-reset.",
            static_cast<unsigned long long>(frame_number), wait_result,
            removed_reason);
      }
    }
  }

  // Free any frame resources.
  FreeFrameResources(frame_number);
}

void Device::FreeFrameResources(uint64_t frame_number) {
  for (size_t i = 0; i < frame_resources_to_free_.size(); ++i) {
    if (fence_values_[i] <= frame_number) {
      frame_resources_to_free_[i].clear();
      // Invalidate every resource's "already marked for this slot" stamp --
      // see RefCounted::last_marked_generation_ / MarkResourceAsUsed.
      ++slot_generation_[i];
    }
  }

  dynamic_ring_buffer_->HasCompletedFrame(frame_number);
  dynamic_ring_buffer_->SetCurrentFrame(CurrentFrame());

  // Descriptor slots follow exactly the same lifetime rule as the frame
  // resources above: safe to reuse only once the GPU has finished every frame
  // whose command list could still name them.
  for (DescriptorPoolHeap *heap :
       {&srv_heap_, &rtv_heap_, &dsv_heap_, &sampler_heap_}) {
    heap->ReleaseCompleted(frame_number);
    heap->SetCurrentFrame(CurrentFrame());
  }
}

uint64_t Device::CurrentFrame() const { return next_fence_; }

}  // namespace Dx8to12
