#include "common.hlsl"
#include "lighting.hlsl"

FFVertexOutput VSMain(VertexInput IN) {
  FFVertexOutput OUT = (FFVertexOutput)0;
  float4 vertex_diffuse = material_diffuse;
  float4 vertex_specular = material_specular;
  float4 specular_lighting = 0;

#ifdef HAS_DIFFUSE
  vertex_diffuse = IN.input_reg5;
#endif
#ifdef HAS_SPECULAR
  vertex_specular = IN.input_reg6;
#endif

#ifndef HAS_TRANSFORM
  OUT.oPos = mul(world_view_proj, float4(IN.input_reg0, 1.f));

#ifdef HAS_NORMAL
  // TODO: Don't normalize if normalized_normals is set.
  float3 view_normal =
      normalize(mul(world_view, float4(IN.input_reg3, 0.f)).xyz);
  float3 view_pos = mul(world_view, float4(IN.input_reg0, 1.f)).xyz;
  OUT.oViewPos = view_pos;
  OUT.oViewNormal = view_normal;
  OUT.oViewReflect = normalize(reflect(view_pos, view_normal));
  // This call used to be unconditional, so geometry with a normal stream
  // got lit even with D3DRS_LIGHTING == FALSE -- the no-normal branch below
  // already respected the flag (it has its own lighting_enabled check), but
  // this, the far more common branch (any mesh with normals -- most world
  // geometry, including roads), did not. A model authored to be unlit (flat
  // baked vertex colors, D3DRS_LIGHTING off) got re-lit by whatever the
  // scene's lights/ambient happened to be instead of showing its own colors.
  if (lighting_enabled) {
    vertex_diffuse = ComputeLighting(view_pos, view_normal, vertex_diffuse,
                                     vertex_specular, specular_lighting);
  }
  OUT.oFog = ComputeFogFactor(length(view_pos));
#else
  // No normal stream, so per-light diffuse/specular can't be computed (their
  // formulas need a surface orientation) -- but ambient lighting doesn't
  // depend on the normal at all, and D3DRS_LIGHTING still applies it on real
  // D3D8. Passing a zero normal makes ComputeLighting's dot(normal, ...)
  // terms zero out diffuse/specular on their own, leaving just ambient.
  // Without this, meshes with baked-black/dark vertex colors and no normals
  // (common for unlit-looking static geometry) rendered solid black instead
  // of getting lit by the scene's ambient term.
  float3 view_pos = mul(world_view, float4(IN.input_reg0, 1.f)).xyz;
  if (lighting_enabled) {
    float4 unused_specular;
    vertex_diffuse = ComputeLighting(view_pos, float3(0, 0, 0), vertex_diffuse,
                                     vertex_specular, unused_specular);
  }
  OUT.oFog = ComputeFogFactor(length(view_pos));
#endif
#else
  // Pretransformed (XYZRHW) vertices are already in screen space, with no
  // notion of eye-space distance to fog by -- and real D3D8 does not fog
  // these unless the app supplies a per-vertex fog value via D3DFVF_XYZRHW's
  // packed fog/specular field, which this shim doesn't forward. 1 = the
  // vertex's own color, unaffected -- otherwise this stayed at the
  // zero-initialized OUT's default of 0 (fully replaced by fog_color), and
  // any 2D UI drawn while fog was still enabled from a prior 3D scene (fog
  // render states are sticky, not reset between draws) would render solid
  // fog-colored quads instead of menu content.
  OUT.oFog = 1.f;
  OUT.oPos = IN.input_reg0;
  OUT.oPos.xy = (OUT.oPos.xy + 0.5f) * inv_viewport_size - 1.f;
  OUT.oPos.y *= -1.f;
  // D3DFVF_XYZRHW vertices are already in screen space -- real D3D8/9
  // hardware does NOT perspective-divide them (RHW is only a per-vertex
  // interpolation weight, never applied to position). D3D12's SV_Position
  // always divides by .w though, and .w here still holds whatever raw RHW
  // value the app supplied (often, but not always, 1 -- some UI/menu quads
  // use a non-1 per-vertex RHW to fake a stylized tilt on real D3D8, which
  // that hardware ignores for position but ours would not without this).
  // Force it to 1 so the automatic divide is a no-op.
  OUT.oPos.w = 1.f;

#ifndef HAS_DIFFUSE
  vertex_diffuse = float4(1, 1, 1, 1);
#endif
#ifndef HAS_SPECULAR
  specular_lighting = 0;
#endif
#endif

  OUT.oD0 = vertex_diffuse;
  OUT.oD1 = specular_lighting;

// Forward texture coordinates.
#ifdef HAS_T0
  OUT.oT0 = float4(IN.input_reg7, 0.f, 0.f);
#endif
#ifdef HAS_T1
  OUT.oT1 = float4(IN.input_reg8, 0.f, 0.f);
#endif
#ifdef HAS_T2
  OUT.oT2 = float4(IN.input_reg9, 0.f, 0.f);
#endif
#ifdef HAS_T3
  OUT.oT3 = float4(IN.input_reg10, 0.f, 0.f);
#endif
#ifdef HAS_T4
  OUT.oT4 = float4(IN.input_reg11, 0.f, 0.f);
#endif
#ifdef HAS_T5
  OUT.oT5 = float4(IN.input_reg12, 0.f, 0.f);
#endif
#ifdef HAS_T6
  OUT.oT6 = float4(IN.input_reg13, 0.f, 0.f);
#endif
#ifdef HAS_T7
  OUT.oT7 = float4(IN.input_reg14, 0.f, 0.f);
#endif

  return OUT;
}