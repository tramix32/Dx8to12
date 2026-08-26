#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <vector>

#include "SimpleMath.h"
#include "d3d8.h"
#include "util.h"
#include "utils/dx_utils.h"

namespace Dx8to12 {

struct ShaderLightMarshall {
  explicit ShaderLightMarshall(const DirectX::SimpleMath::Matrix& view,
                               const D3DLIGHT8& l)
      : diffuse(l.Diffuse),
        specular(l.Specular),
        ambient(l.Ambient),
        position(DirectX::SimpleMath::Vector3::Transform(
            VectorFromD3D(l.Position), view)),
        type(l.Type),
        direction(DirectX::SimpleMath::Vector3::TransformNormal(
            VectorFromD3D(l.Direction), view)),
        range(l.Range),
        falloff(l.Falloff),
        attenuation0(l.Attenuation0),
        attenuation1(l.Attenuation1),
        attenuation2(l.Attenuation2),
        theta(l.Theta),
        phi(l.Phi),
        pad{} {}
  D3DCOLORVALUE diffuse;
  D3DCOLORVALUE specular;
  D3DCOLORVALUE ambient;
  DirectX::SimpleMath::Vector3 position;
  D3DLIGHTTYPE type;
  DirectX::SimpleMath::Vector3 direction;
  float range;
  float falloff;
  float attenuation0, attenuation1, attenuation2;
  float theta;
  float phi;
  float pad[2];
};
static_assert(sizeof(ShaderLightMarshall) == 7 * 16,
              "ShaderLightMarshall size check.");

struct VertexCBuffer {
  DirectX::SimpleMath::Matrix world_view_proj;
  DirectX::SimpleMath::Matrix world_view;
  DirectX::SimpleMath::Vector3 camera_position;
  float pad;
  // 2/viewport.Width, 2/viewport.Height -- used by the fixed-function vertex
  // shader to convert D3DFVF_XYZRHW (pretransformed, screen-space) vertices
  // into NDC. This used to be baked as a compile-time literal into the
  // generated HLSL at CreateVertexShader time, which meant it silently went
  // stale forever for any vertex shader handle created before a later
  // Reset() changed the viewport size (games typically create their shader
  // handles once at startup and reuse them for the rest of the run) --
  // observed as 2D UI (radar, menus) getting positioned/scaled for the
  // resolution the game started at, not whatever it was later changed to.
  // Living here instead means it's refreshed from the *current* viewport
  // every time this cbuffer is (DIRTY_FLAG_TRANSFORMS, at least once a
  // frame), independent of which vertex shader handle is bound.
  DirectX::SimpleMath::Vector2 inv_viewport_size;
  float pad2[2];  // HLSL cbuffer packing: a float2 doesn't share a 16-byte
                  // register with anything after it either.
  // D3DMATRIX texture_coord_transforms[8];
};
struct LightsCBuffer {
  ShaderLightMarshall lights[8];
  int num_lights;
  D3DMATERIALCOLORSOURCE diffuse_material_source;
  D3DMATERIALCOLORSOURCE ambient_material_source;
  D3DMATERIALCOLORSOURCE specular_material_source;
  int specular_enable;
  // D3DRS_LIGHTING. Needed so meshes with no normal stream (where diffuse/
  // specular can't be computed) can still get ambient lighting applied --
  // ambient doesn't depend on the surface normal, but was previously skipped
  // entirely for such meshes since the vertex shader variant for "no normal"
  // never called ComputeLighting at all.
  int lighting_enabled;
  // D3DRS_EMISSIVEMATERIALSOURCE. Lives here (with the other *_material_source
  // fields) rather than next to material_emissive itself in PixelCBuffer,
  // because the source selection has to run in the vertex shader alongside
  // the other lighting terms -- material_emissive's *value* is read from
  // PixelCBuffer (shared ALL-visibility), but which color feeds it is a
  // per-vertex decision.
  D3DMATERIALCOLORSOURCE emissive_material_source;
  int pad;
  D3DCOLORVALUE global_ambient;
};
struct PixelCBuffer {
  D3DCOLORVALUE material_diffuse;
  D3DCOLORVALUE material_ambient;
  D3DCOLORVALUE material_specular;
  D3DCOLORVALUE material_emissive;
  float material_power;
  float alpha_ref;
  float pad[2];
  D3DCOLORVALUE texture_factor;
  // Fog (D3DRS_FOGENABLE/FOGCOLOR/FOGTABLEMODE/FOGVERTEXMODE/FOGSTART/
  // FOGEND/FOGDENSITY). Deliberately routed through this cbuffer instead of
  // shader permutation: the PSO key zeroes every fog field (see the comment
  // at Device::CreatePSO on pso_key.rs.fog_*) specifically so distinct fog
  // parameter values don't fragment the PSO cache, which only works if fog
  // is a per-draw runtime value rather than a compiled-in constant.
  int fog_enable;
  // Effective fog mode: D3DRS_FOGTABLEMODE if set, else D3DRS_FOGVERTEXMODE
  // -- real D3D8 prefers table (per-pixel) fog when the app requests it, and
  // RenderWare-era titles commonly set only one of the two render states.
  D3DFOGMODE fog_mode;
  float fog_start;
  float fog_end;
  float fog_density;
  float pad2[3];
  D3DCOLORVALUE fog_color;
};

struct ConstantRegData {
  uint32_t data[4];
};

struct VertexShaderDeclaration {
  // Creates a VertexShaderDeclaration from an fvf desc passed to
  // SetVertexShader.
  static VertexShaderDeclaration CreateFromFVFDesc(DWORD fvf);

  std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
  std::array<int, 16> buffer_strides = {};
  std::array<bool, 16> has_inputs = {};
  std::map<int, ConstantRegData> constant_reg_init;

  friend std::ostream& operator<<(std::ostream& os,
                                  const VertexShaderDeclaration& decl);
};

VertexShaderDeclaration ParseShaderDeclaration(const DWORD* declaration);

// A stable per-shader identity, handed out once and never reused -- unlike
// the ID3DBlob* this shader wraps. VertexShader/PixelShader are RefCounted
// (delete this at zero refs), and a programmable shader can be destroyed
// mid-session via DeleteVertexShader/DeletePixelShader; a later, completely
// unrelated shader is then free to land at that exact freed heap address.
// PSOState (render_state.h) used to key its pso_cache_/ps_cache_ lookup on
// the raw blob pointer, which made that scenario indistinguishable from
// "this is the same shader as before" -- a stale cache hit would bind an
// old, already-deleted shader's compiled PSO (wrong vertex transform, wrong
// constant layout, wrong everything) to a draw call using the new one. Same
// identity-reuse (ABA) hazard as the texture-descriptor rebind cache and
// GetRenderTarget's surface cache (both already found and fixed this
// session) -- a monotonic ID assigned at construction has no stale value to
// alias against, unlike a pointer.
inline uint64_t NextShaderId() {
  static uint64_t next_id = 1;
  return next_id++;
}

struct VertexShader : public RefCounted {
  VertexShaderDeclaration decl;
  ComPtr<ID3DBlob> blob;
  DWORD fvf_desc;
  // Copies of the original DX8 token streams passed to CreateVertexShader,
  // for GetVertexShaderDeclaration/GetVertexShaderFunction. Empty for
  // fixed-function shaders (no programmable function, and the declaration
  // was synthesized from an FVF rather than a real token stream).
  std::vector<DWORD> declaration_tokens;
  std::vector<DWORD> function_tokens;
  uint64_t unique_id = NextShaderId();
};

struct PixelShader : public RefCounted {
  ComPtr<ID3DBlob> blob;
  // Copy of the original DX8 token stream passed to CreatePixelShader, for
  // GetPixelShaderFunction.
  std::vector<DWORD> function_tokens;
  // See VertexShader::unique_id above -- same reasoning, same fix, for
  // pixel shaders deleted via DeletePixelShader.
  uint64_t unique_id = NextShaderId();
};

VertexShader CreateFixedFunctionVertexShader(
    const D3D12_VIEWPORT& viewport, const DWORD fvf_desc,
    const VertexShaderDeclaration& declaration);
}  // namespace Dx8to12