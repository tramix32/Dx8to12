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
  // See the comment on FFVertexOutput::oPerPixelLightingEligible
  // (common.hlsl) -- this whole branch is real (non-pretransformed) 3D
  // geometry, the only kind D3D8 fixed-function lighting ever applies to.
  OUT.oPerPixelLightingEligible = 1.f;

#ifdef HAS_NORMAL
  // D3DRS_NORMALIZENORMALS defaults to FALSE -- real D3D8 fixed-function
  // only renormalizes here when the app explicitly asks for it. Left
  // unnormalized (the common case), a scaled world matrix scales the
  // transformed normal's length too, which the lighting math in
  // ComputeLighting then picks up as a genuine (if hardware-authentic)
  // brightness change -- normalizing unconditionally was quietly hiding
  // that on every scaled object.
  float3 view_normal = mul(world_view, float4(IN.input_reg3, 0.f)).xyz;
  if (normalize_normals) view_normal = normalize(view_normal);
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
#ifndef PER_PIXEL_LIGHTING
  // Under PER_PIXEL_LIGHTING, ff_pixel_shader.cpp's generated PSMain does
  // this same ComputeLighting call itself, per pixel, using the interpolated
  // (and there, renormalized) oViewPos/oViewNormal below -- doing it here too
  // would light the geometry twice. lighting_enabled is still read there at
  // pixel-shader time, so D3DRS_LIGHTING's on/off semantics are unchanged;
  // only *where* the lit color gets computed moves.
  if (lighting_enabled) {
    vertex_diffuse = ComputeLighting(view_pos, view_normal, vertex_diffuse,
                                     vertex_specular, specular_lighting);
  }
#endif
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
  // Needed so PER_PIXEL_LIGHTING's pixel-shader ComputeLighting call (which
  // has no other way to get this branch's position) can still compute
  // correct point/spot-light attenuation distance even without a normal --
  // previously unset here since nothing in the non-per-pixel path after this
  // point read it.
  OUT.oViewPos = view_pos;
#ifndef PER_PIXEL_LIGHTING
  // See the PER_PIXEL_LIGHTING comment in the HAS_NORMAL branch above --
  // same reasoning. oViewNormal is left at its zero-initialized default here
  // (no normal stream to derive it from), which the pixel shader's
  // ComputeLighting call treats the same way this one does: a zero normal
  // zeroes out diffuse/specular, leaving ambient/emissive only.
  if (lighting_enabled) {
    float4 unused_specular;
    vertex_diffuse = ComputeLighting(view_pos, float3(0, 0, 0), vertex_diffuse,
                                     vertex_specular, unused_specular);
  }
#endif
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
#ifdef PER_PIXEL_LIGHTING
  // ComputeLighting in the pixel shader still needs the original COLOR2 /
  // material-specular input when D3DMCS_COLOR2 is selected. Passing the
  // (not yet computed) specular_lighting value here made that source always
  // zero in per-pixel and RT modes.
  OUT.oD1 = vertex_specular;
#else
  OUT.oD1 = specular_lighting;
#endif

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
