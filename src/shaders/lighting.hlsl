#define D3DMCS_MATERIAL 0
#define D3DMCS_COLOR1 1
#define D3DMCS_COLOR2 2

#define D3DLIGHT_POINT 1
#define D3DLIGHT_SPOT 2
#define D3DLIGHT_DIRECTIONAL 3

// sizeof(Light) is 7 float4s.
struct Light {
  float4 diffuse;
  float4 specular;
  float4 ambient;
  float3 position;
  int type;
  float3 direction;
  float range;
  float falloff;
  float attentuation0, attentuation1, attentuation2;
  float theta;
  float phi;
  float2 pad;
};

cbuffer Lights : register(b2) {
  Light lights[8];
  int num_lights;
  int diffuse_material_source;
  int ambient_material_source;
  int specular_material_source;
  int specular_enable;
  int lighting_enabled;
  int emissive_material_source;
  int pad1;
  float4 global_ambient;
};

float4 ComputeLighting(float3 view_pos, float3 view_normal,
                       float4 vertex_color1, float4 vertex_color2,
                       out float4 specular_lighting) {
  float4 diffuse_color =
      diffuse_material_source == D3DMCS_MATERIAL
          ? material_diffuse
          : (diffuse_material_source == D3DMCS_COLOR1 ? vertex_color1
                                                      : vertex_color2);
  float4 ambient_color =
      ambient_material_source == D3DMCS_MATERIAL
          ? material_ambient
          : (ambient_material_source == D3DMCS_COLOR1 ? vertex_color1
                                                      : vertex_color2);
  float4 specular_color =
      specular_material_source == D3DMCS_MATERIAL
          ? material_specular
          : (specular_material_source == D3DMCS_COLOR1 ? vertex_color1
                                                       : vertex_color2);
  // material_emissive comes from PixelGlobals (common.hlsl, register b1,
  // ALL-visibility) -- the same cbuffer diffuse/ambient/specular's *_MATERIAL
  // case reads from.
  float4 emissive_color =
      emissive_material_source == D3DMCS_MATERIAL
          ? material_emissive
          : (emissive_material_source == D3DMCS_COLOR1 ? vertex_color1
                                                       : vertex_color2);
  float3 diffuse_lighting = 0;
  float3 ambient_lighting = global_ambient.xyz;
  specular_lighting = float4(0, 0, 0, 1);

  for (int i = 0; i < min(num_lights, 8); ++i) {
    Light light = lights[i];
    float3 dir_to_light;
    float attenuation;

    switch (light.type) {
      case D3DLIGHT_POINT:
      case D3DLIGHT_SPOT: {
        dir_to_light = light.position - view_pos;
        float dist_sq = dot(dir_to_light, dir_to_light);
        if (dist_sq > light.range * light.range)
          attenuation = 0;
        else {
          float dist = sqrt(dist_sq);
          dir_to_light /= dist;
          attenuation =
              saturate(rcp(light.attentuation0 + light.attentuation1 * dist +
                           light.attentuation2 * dist_sq));
        }
        break;
      }
      case D3DLIGHT_DIRECTIONAL:
        dir_to_light = -normalize(light.direction);
        attenuation = 1;
        break;
      default:
        dir_to_light = 0;
        attenuation = 0;
        break;
    }
    if (light.type == D3DLIGHT_SPOT) {
      // D3D8 spotlight cone falloff: full intensity inside the inner cone
      // (half-angle theta/2), zero outside the outer cone (half-angle
      // phi/2), interpolated by Falloff in between. light.direction points
      // FROM the light INTO the scene, so the angle between it and the
      // (light-to-surface) direction is the angle off the spotlight's aim.
      float cos_rho = dot(normalize(light.direction), -dir_to_light);
      float cos_inner = cos(light.theta * 0.5f);
      float cos_outer = cos(light.phi * 0.5f);
      float spot = saturate((cos_rho - cos_outer) /
                            max(cos_inner - cos_outer, 0.0001f));
      attenuation *= pow(spot, max(light.falloff, 0.0001f));
    }

    diffuse_lighting += saturate(dot(view_normal, dir_to_light)) * attenuation *
                        light.diffuse.xyz;
    // Per-light ambient is part of the same Attn * (...) formula as diffuse
    // and specular (D3D8's lighting math, "Iamb*Camb + Idif*Cdif*(N.L) +
    // Ispec*Cspec*(N.H)^Power", all inside the same Attn multiply) -- a
    // point/spot light's ambient contribution should fall off with distance
    // and cone falloff exactly like its diffuse/specular do. This was adding
    // it at full, unattenuated strength regardless of range or spotlight
    // cone, which is wrong for any light with a nonzero Ambient field (most
    // games leave it zero, but not all).
    ambient_lighting += attenuation * light.ambient.xyz;

#if 0
    // TODO: LOCALVIEWER
    float3 H = normalize(normalize(camera_position - world_pos) + dir_to_light);
#else
    float3 H = normalize(float3(0, 0, 1) + dir_to_light);
#endif
    float ndoth = dot(view_normal, H);
    if (ndoth > 0)
      specular_lighting.xyz +=
          light.specular.xyz * pow(abs(ndoth), material_power) * attenuation;
  }

  if (!specular_enable)
    specular_lighting = 0;
  else
    specular_lighting *= specular_color;

  // D3D8's lighting equation sums every term (ambient*Camb + diffuse*Cdif +
  // specular*Cspec + emissive) and clamps *once*, at the very end -- not
  // per-term. Saturating diffuse_lighting/ambient_lighting here individually,
  // before combining them, silently threw away legitimate headroom in any
  // scene bright enough for one term to exceed 1.0 on its own (multiple
  // overlapping lights, a strong single light) -- exactly the kind of thing
  // that would read as a flatter, lower-contrast image without an obvious
  // single wrong pixel to point at.
  //
  // Emissive adds flat, unlit light of its own -- unlike ambient/diffuse it
  // is not modulated by any material color, it *is* the color. Previously
  // material_emissive was uploaded to the GPU (PixelCBuffer) but nothing
  // ever read it, so D3DMATERIAL8::Emissive had no visible effect at all --
  // e.g. self-lit signs/headlights/instrument panels that rely on emissive
  // to stay visible in the dark rendered exactly as dark as their
  // surroundings.
  diffuse_lighting = saturate(diffuse_lighting * diffuse_color.rgb +
                              ambient_lighting * ambient_color.rgb +
                              emissive_color.rgb);

  return float4(diffuse_lighting, diffuse_color.a);
}