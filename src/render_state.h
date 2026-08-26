#pragma once

#include <d3d12.h>

#include <array>
#include <vector>

#include "d3d8.h"
#include "device_limits.h"
#include "util.h"
#include "utils/asserts.h"

namespace Dx8to12 {
struct RenderState {
  void Reset();

  // Retrieves a D3DRENDERSTATETYPE render state by its index.
  DWORD &GetEnumAtIndex(D3DRENDERSTATETYPE index);

  // This is all the state that is accessed using D3DRENDERSTATETYPE.
  D3DZBUFFERTYPE
  zbuffer_type =
      D3DZB_FALSE;  // TODO: Set to true if EnableAutoDepthStencil is set.
  DWORD zwrite_enable = TRUE;
  D3DFILLMODE fill_mode = D3DFILL_SOLID;
  D3DSHADEMODE shade_mode = D3DSHADE_GOURAUD;
  DWORD alpha_test_enable = FALSE;
  D3DBLEND src_blend = D3DBLEND_ONE;
  D3DBLEND dest_blend = D3DBLEND_ONE;
  D3DCULL cull_mode = D3DCULL_CCW;
  D3DCMPFUNC z_func = D3DCMP_LESSEQUAL;
  DWORD alpha_ref = 0;
  D3DCMPFUNC alpha_func = D3DCMP_ALWAYS;
  DWORD dither_enable = FALSE;
  DWORD alpha_blend_enable = FALSE;
  BOOL fog_enable = FALSE;
  DWORD specular_enable = FALSE;
  D3DCOLOR fog_color = 0;
  D3DFOGMODE fog_table_mode = D3DFOG_NONE;
  float fog_start = 0.f;
  float fog_end = 1.f;
  float fog_density = 1.f;
  DWORD edge_antialias = FALSE;
  LONG z_bias = 0;
  BOOL range_fog_enable = FALSE;
  BOOL stencil_enable = FALSE;
  D3DSTENCILOP stencil_fail = D3DSTENCILOP_KEEP;
  D3DSTENCILOP stencil_zfail = D3DSTENCILOP_KEEP;
  D3DSTENCILOP stencil_pass = D3DSTENCILOP_KEEP;
  D3DCMPFUNC stencil_func = D3DCMP_ALWAYS;
  DWORD stencil_ref = 0;
  DWORD stencil_mask = 0xFFFFFFFF;
  DWORD stencil_write_mask = 0xFFFFFFFF;
  D3DCOLOR texture_factor = D3DCOLOR_ARGB(255, 255, 255, 255);
  DWORD lighting = TRUE;
  D3DCOLOR ambient = 0;
  D3DFOGMODE fog_vertex_mode = D3DFOG_NONE;
  DWORD color_vertex = TRUE;
  BOOL local_viewer = FALSE;
  DWORD normalized_normals = FALSE;
  D3DMATERIALCOLORSOURCE diffuse_material_source = D3DMCS_COLOR1;
  D3DMATERIALCOLORSOURCE specular_material_source = D3DMCS_COLOR2;
  // Real D3D8's documented defaults are D3DMCS_MATERIAL for both of these,
  // not D3DMCS_COLOR2 -- they previously read as COLOR2 here, apparently
  // copy-pasted from specular_material_source just above without correcting
  // the constant. ambient_material_source was already live (feeding
  // LightsCBuffer before tonight's session), so this was a real,
  // pre-existing bug: any object relying on the API default rather than an
  // explicit SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, ...) would have its
  // ambient term multiplied by vertex specular color instead of the
  // material's own ambient value. emissive_material_source only started
  // being read this session (it previously existed but fed nothing), so
  // fixing it here is what keeps its very first use correct rather than
  // reproducing the same copy-paste mistake for a second field.
  D3DMATERIALCOLORSOURCE ambient_material_source = D3DMCS_MATERIAL;
  D3DMATERIALCOLORSOURCE emissive_material_source = D3DMCS_MATERIAL;
  float point_size = 1.f;
  float point_size_min = 0.f;
  BOOL point_sprite_enable = FALSE;
  BOOL point_scale_enable = FALSE;
  float point_scale_a = 1.f;
  float point_scale_b = 0.f;
  float point_scale_c = 0.f;
  DWORD multisample_antialias = TRUE;
  float point_size_max = 64.f;
  DWORD color_write_enable =
      D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
      D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;
  D3DBLENDOP blend_op = D3DBLENDOP_ADD;

  // The rest of these have no effect on rendering (bookkeeping only, to
  // avoid aborting on Set/GetRenderState) -- see ROADMAP.md.
  BOOL clipping = TRUE;
  DWORD clip_plane_enable = 0;
  BOOL last_pixel = TRUE;
  DWORD line_pattern = 0;  // D3DLINEPATTERN, same size as DWORD.
  BOOL z_visible = FALSE;  // Legacy/deprecated even in D3D8.
  BOOL software_vertex_processing = FALSE;
  DWORD multisample_mask = 0xFFFFFFFF;
  D3DPATCHEDGESTYLE patch_edge_style = D3DPATCHEDGE_DISCRETE;
  float patch_segments = 1.f;
  DWORD debug_monitor_token = 0;
  DWORD vertex_blend = 0;  // D3DVERTEXBLENDFLAGS, D3DVBF_DISABLE.
  BOOL indexed_vertex_blend_enable = FALSE;
  float tween_factor = 0.f;
  D3DORDERTYPE position_order = D3DORDER_CUBIC;
  D3DORDERTYPE normal_order = D3DORDER_LINEAR;
  std::array<DWORD, 8> wrap = {};  // D3DRS_WRAP0..7, one per texcoord set.

  CLANG_PUSH_IGNORE_FLOAT_EQUAL
  bool operator==(const RenderState &) const = default;
  CLANG_POP_IGNORE
};

struct TextureStageState {
  void Reset();

  D3DTEXTUREOP color_op = D3DTOP_DISABLE;
  DWORD color_arg1 = D3DTA_TEXTURE;
  DWORD color_arg2 = D3DTA_CURRENT;
  D3DTEXTUREOP alpha_op = D3DTOP_DISABLE;
  DWORD alpha_arg1 = D3DTA_TEXTURE;  // TODO: Is diffuse if no texture is set.
  DWORD alpha_arg2 = D3DTA_CURRENT;
  DWORD texcoord_index = 0;
  D3DTEXTUREADDRESS address_u = D3DTADDRESS_WRAP;
  D3DTEXTUREADDRESS address_v = D3DTADDRESS_WRAP;
  DWORD border_color = 0;
  D3DTEXTUREFILTERTYPE mag_filter = D3DTEXF_POINT;
  D3DTEXTUREFILTERTYPE min_filter = D3DTEXF_POINT;
  D3DTEXTUREFILTERTYPE mip_filter = D3DTEXF_NONE;
  float mipmap_lod_bias = 0.f;
  DWORD max_anisotropy = 1;
  D3DTEXTURETRANSFORMFLAGS transform_flags = D3DTTFF_DISABLE;
  D3DTEXTUREADDRESS address_w = D3DTADDRESS_WRAP;
  // The third argument for D3DTOP_MULTIPLYADD/LERP (both implemented in
  // ApplyOperation, ff_pixel_shader.cpp).
  DWORD color_arg0 = D3DTA_CURRENT;
  DWORD alpha_arg0 = D3DTA_CURRENT;
  // The result-register redirect. Generated by ApplyOperation
  // (ff_pixel_shader.cpp) as a write to `temp_color` instead of the main
  // `result_color` chain when set to D3DTA_TEMP -- see the PSMain preamble
  // in CreatePixelShaderFromState for the full explanation.
  DWORD result_arg = D3DTA_CURRENT;
  // Bookkeeping only -- D3DTOP_BUMPENVMAP/BUMPENVMAPLUMINANCE aren't
  // implemented in ApplyOperation (they perturb the *next* stage's texture
  // coordinates using this stage's bump texture, which doesn't fit
  // ApplyOperation's per-stage-independent formula -- would need real
  // shader-generation restructuring, not attempted blind without a game
  // actually exercising it to verify against). Stored so
  // Set/GetTextureStageState round-trip instead of hitting GetAtIndex's FAIL;
  // ApplyOperation's default case will still FAIL if a stage's color/alpha
  // op is actually set to one of these.
  float bump_env_mat00 = 0.f, bump_env_mat01 = 0.f;
  float bump_env_mat10 = 0.f, bump_env_mat11 = 0.f;
  float bump_env_lscale = 0.f;
  float bump_env_loffset = 0.f;

  DWORD &GetAtIndex(size_t index);

  CLANG_PUSH_IGNORE_FLOAT_EQUAL
  bool operator==(const TextureStageState &) const = default;
  CLANG_POP_IGNORE
};

// This object is used purely to cache pipeline state objects.
//
// Deliberately does NOT store input_elements: it's fully determined by which
// VertexShader is bound (each VertexShader's decl.input_elements is fixed at
// shader-creation time and never varies per-draw), and `vs` (that shader's
// compiled blob pointer) already uniquely identifies it -- one VertexShader
// per blob, never two different declarations sharing a blob pointer. Since
// this key is rebuilt on every single draw call (CreatePSO has no way to
// know in advance whether anything PSO-relevant changed), copying/hashing
// the full input_elements vector here would mean a fresh heap allocation
// every draw purely to answer a question `vs` alone already answers.
struct PSOState {
  RenderState rs;
  // NextShaderId() (vertex_shader.h) values, not raw ID3DBlob* -- a
  // deleted-and-recreated programmable shader (DeleteVertexShader/
  // DeletePixelShader) can land its replacement's blob at the exact heap
  // address the old one just freed, which made a stale pso_cache_ entry
  // compare equal to a *different* shader and hand back the wrong compiled
  // PSO for it. See VertexShader::unique_id's comment for the full
  // reasoning; this is the same identity-reuse hazard already fixed this
  // session for the texture-descriptor rebind cache and GetRenderTarget.
  uint64_t vs;
  uint64_t ps;
  D3DPRIMITIVETYPE prim_type;
  DXGI_FORMAT dsv_format;
  DXGI_FORMAT rtv_format;
  bool operator==(const PSOState &other) const {
    return vs == other.vs && ps == other.ps && rs == other.rs &&
           prim_type == other.prim_type && dsv_format == other.dsv_format &&
           rtv_format == other.rtv_format;
  }
};

// Compactly encapsulates all state used to generate a pixel shader. Used a key
// to cache fixed-function pixel shaders.
struct PixelShaderState {
  PixelShaderState(const RenderState &rs,
                   const bool stage_has_texture[kMaxTexStages],
                   const TextureStageState texture_stage_states[kMaxTexStages]);

  bool color_vertex : 1;
  D3DMATERIALCOLORSOURCE diffuse_material_source : 2;
  // TODO: Any stage after a non-active stage is also not active.
  uint8_t stage_has_texture_flag : 8;
  uint8_t alpha_func_minus1 : 3;
  // TODO: Only take into account active stages.
  std::array<TextureStageState, kMaxTexStages> ts;

  bool stage_has_texture(int stage) const {
    ASSERT(stage < kMaxTexStages);
    return HasFlag(stage_has_texture_flag, 1 << stage);
  }

  D3DCMPFUNC alpha_func() const {
    return static_cast<D3DCMPFUNC>(alpha_func_minus1 + 1);
  }

  bool operator==(const PixelShaderState &) const = default;
};

static_assert(kMaxTexStages == 8, "Unexpected number of texture stages.");

}  // namespace Dx8to12

template <>
struct ::std::hash<Dx8to12::RenderState> {
  size_t operator()(Dx8to12::RenderState const &) const;
};

template <>
struct ::std::hash<Dx8to12::PSOState> {
  size_t operator()(Dx8to12::PSOState const &) const;
};

template <>
struct ::std::hash<Dx8to12::PixelShaderState> {
  size_t operator()(Dx8to12::PixelShaderState const &) const;
};
