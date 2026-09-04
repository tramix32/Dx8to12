// Camera-only motion vectors, reconstructed from the depth buffer.
//
// For every pixel: unproject it into world space using this frame's inverse
// view-projection, re-project that world point with the *previous* frame's
// view-projection, and write where the pixel used to be. This is exactly the
// information a temporal upscaler needs to find last frame's sample for a
// given pixel.
//
// It is camera-only by construction: a world point is assumed to have been
// in the same place last frame, so anything that actually moved (cars,
// pedestrians) gets the vector of the static geometry behind it and will
// ghost. Fixing that needs per-object previous transforms and is deliberately
// out of scope here -- see the plan's "czego ten plan świadomie nie obejmuje".
//
// Both matrices are the *unjittered* camera. Jitter is a sub-pixel sampling
// offset, not camera motion; feeding a jittered matrix in here would turn the
// jitter itself into spurious per-pixel motion.

cbuffer MotionVectorGlobals : register(b0) {
  // Inverse of this frame's view*proj.
  float4x4 inv_view_proj;
  // Previous frame's view*proj.
  float4x4 prev_view_proj;
  // Scene render target size in pixels.
  float2 render_size;
  // Divisor for the debug visualisation only: motion of this many pixels
  // saturates the colour. Ignored by PSMain.
  float debug_scale;
  float mvec_pad;
};

// The depth-stencil resource is created typeless precisely so it can carry an
// SRV alongside its DSV (see BaseTexture::Create), which is what makes this
// pass possible without a separate depth copy.
Texture2D<float> depth_tex : register(t0);

struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

// One oversized triangle rather than a quad: no vertex or index buffer, no
// input layout, and no seam down the middle where two triangles would meet.
VSOut VSMain(uint vertex_id : SV_VertexID) {
  VSOut output;
  output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.pos = float4(output.uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
  return output;
}

// Returns the offset, in pixels, from this pixel to where the same world
// point sat in the previous frame.
float2 ComputeMotion(float2 pixel_xy) {
  const float depth = depth_tex.Load(int3((int2)pixel_xy, 0));
  const float2 uv = pixel_xy / render_size;

  // Nothing was drawn here, so there is no world point to have moved. Sky,
  // distant geometry and anything the game left at the clear value all land
  // on the far plane, where the unprojection below is numerically hopeless:
  // w tends to zero, the reconstructed position explodes, and reprojecting it
  // subtracts two huge nearly-equal numbers. What survives is not the small
  // motion that was wanted but the rounding error, which is noise.
  //
  // The w guards further down do not catch this. They stop division by zero;
  // they do nothing about w merely being tiny, which is the case that ruins
  // the precision while staying comfortably above any epsilon. Rejecting on
  // depth is the check that actually matches the situation.
  if (depth >= 0.9999f) return float2(0.f, 0.f);

  // Pixel -> NDC. NDC y runs opposite to pixel y.
  const float4 clip = float4(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f, depth, 1.f);

  // Matrix-first mul, matching every other shader here: HLSL packs cbuffer
  // matrices column-major, so a row-major matrix uploaded from the CPU
  // arrives transposed and mul(M, v) is the row-vector multiply.
  float4 world = mul(inv_view_proj, clip);
  if (abs(world.w) < 1e-7f) return float2(0.f, 0.f);
  world /= world.w;

  const float4 prev_clip = mul(prev_view_proj, world);
  // Behind the previous camera: there is no valid previous pixel, and
  // dividing through would fold it back onto the screen mirrored.
  if (prev_clip.w <= 1e-7f) return float2(0.f, 0.f);
  const float2 prev_ndc = prev_clip.xy / prev_clip.w;
  const float2 prev_uv = float2(prev_ndc.x * 0.5f + 0.5f, 0.5f - prev_ndc.y * 0.5f);

  const float2 motion = (prev_uv - uv) * render_size;

  // A pixel cannot legitimately have come from outside the frame it is being
  // reconstructed from, so anything beyond that is arithmetic that went wrong
  // rather than movement that happened. Zero is the honest answer: it tells
  // the upscaler "no history for this pixel", which it handles, where a wild
  // vector tells it to go and fetch one from nowhere, which it does not.
  if (!all(isfinite(motion)) || any(abs(motion) > render_size)) {
    return float2(0.f, 0.f);
  }
  return motion;
}

// Two targets, not one. The upscaler needs the depth buffer as well as the
// motion vectors, but it lives in another process and cannot open a
// depth-stencil resource as a plain readable texture -- and CopyResource
// cannot convert D24_UNORM_S8 into anything it could. Emitting depth here as
// a second render target converts it for free: the value is already being
// read for the motion vector, so this costs one extra write and no extra pass.
struct MotionVectorTargets {
  float2 motion : SV_Target0;
  float depth : SV_Target1;
};

MotionVectorTargets PSMain(VSOut input) {
  MotionVectorTargets output;
  output.motion = ComputeMotion(input.pos.xy);
  // Raw device depth, not linearised: that is what DLSS expects, and
  // linearising here would throw away precision it relies on.
  output.depth = depth_tex.Load(int3((int2)input.pos.xy, 0));
  return output;
}

// Debug visualisation, drawn over the scene instead of the motion buffer.
// Without this the pass is invisible: a R16G16_FLOAT buffer nobody samples
// yet cannot be checked by looking at the game.
float4 PSDebug(VSOut input) : SV_Target {
  const float2 motion = ComputeMotion(input.pos.xy);
  const float2 scaled = clamp(motion / max(debug_scale, 1e-3f), -1.f, 1.f);
  // Neutral grey where nothing moved, so a static camera is unmistakable:
  // red/green shift left-right and up-down respectively.
  return float4(scaled.x * 0.5f + 0.5f, scaled.y * 0.5f + 0.5f, 0.5f, 1.f);
}

// Scene colour scaled onto the backbuffer, for every frame the upscaler does
// not produce.
//
// Without this there is no such path at all: at a reduced render scale the
// scene target and the backbuffer are different sizes, CopyResource refuses
// that, and the resolve cleared to black instead. So any moment the upscaler
// was not ready -- startup, a restart after the window came back -- the
// screen went black, and if it never became ready the game stayed black.
//
// Bilinear, which is what an ordinary upscale wants. The sampler is declared
// static in the root signature, so this costs no descriptor.
Texture2D<float4> scene_tex : register(t0);
SamplerState scene_sampler : register(s0);

float4 PSScaleBlit(VSOut input) : SV_Target {
  return scene_tex.SampleLevel(scene_sampler, input.uv, 0.f);
}

