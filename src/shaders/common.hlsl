#define NUM_VS_CONST_REGS 96
#define NUM_VS_TEMP_REGS 12

#define NUM_PS_CONST_REGS 8
#define NUM_PS_TEMP_REGS 6

cbuffer Globals : register(b0) {
  float4x4 world_view_proj;
  float4x4 world_view;
  float3 camera_position;
  float pad;
  // 2/viewport.Width, 2/viewport.Height -- see the comment on
  // VertexCBuffer::inv_viewport_size (vertex_shader.h) for why this lives
  // here (refreshed from the current viewport every frame) instead of as a
  // shader-compile-time constant.
  float2 inv_viewport_size;
  // float4x4 texture_coord_transforms[8];
};

cbuffer PixelGlobals : register(b1) {
  // Material data.
  float4 material_diffuse;
  float4 material_ambient;
  float4 material_specular;
  float4 material_emissive;
  float material_power;
  float alpha_ref;
  float4 texture_factor;
  // Fog. See the comment on PixelCBuffer::fog_enable (vertex_shader.h) for
  // why these are runtime cbuffer values rather than shader-permutation
  // constants.
  int fog_enable;
  int fog_mode;  // D3DFOGMODE: NONE=0, EXP=1, EXP2=2, LINEAR=3.
  float fog_start;
  float fog_end;
  float fog_density;
  float4 fog_color;
};

#define D3DFOG_NONE 0
#define D3DFOG_EXP 1
#define D3DFOG_EXP2 2
#define D3DFOG_LINEAR 3

// Vertex fog factor from eye-space distance: 1 = object's own color, 0 =
// fully replaced by fog_color. Matches the D3D8/9 fixed-function fog
// equations (Direct3D docs, "Fog Formulas").
float ComputeFogFactor(float eye_dist) {
  if (!fog_enable) return 1.f;
  if (fog_mode == D3DFOG_EXP) return saturate(exp(-fog_density * eye_dist));
  if (fog_mode == D3DFOG_EXP2) {
    float d = fog_density * eye_dist;
    return saturate(exp(-d * d));
  }
  if (fog_mode == D3DFOG_LINEAR) {
    return saturate((fog_end - eye_dist) / max(fog_end - fog_start, 1e-5f));
  }
  // D3DFOG_NONE with fog_enable set: D3D8 allows FOGENABLE with both
  // FOGTABLEMODE and FOGVERTEXMODE at NONE, which real hardware treats as no
  // fog rather than a degenerate case.
  return 1.f;
}

struct FFVertexOutput {
  float4 oPos : SV_POSITION;
  float4 oD0 : COLOR0;
  float4 oD1 : COLOR1;
  float4 oT0 : TEXCOORD0;
  float4 oT1 : TEXCOORD1;
  float4 oT2 : TEXCOORD2;
  float4 oT3 : TEXCOORD3;
  float4 oT4 : TEXCOORD4;
  float4 oT5 : TEXCOORD5;
  float4 oT6 : TEXCOORD6;
  float4 oT7 : TEXCOORD7;

  float3 oViewNormal : NORMAL0;
  float3 oViewReflect : NORMAL1;
  float3 oViewPos : NORMAL2;

  float oFog : FOG;
};

// #define FFVertexOutput VertexOutput

// Some helper functions to make generation of programmable shaders easier.
float4 mydot4(float4 a, float4 b) { return dot(a, b).xxxx; }
float4 mydot3(float3 a, float3 b) { return dot(a, b).xxxx; }
float4 mylerp(float4 s, float4 a, float4 b) { return lerp(a, b, s); }