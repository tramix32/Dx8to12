#include <d3dcompiler.h>

#include <algorithm>
#include <sstream>
#include <vector>

#include "aixlog.hpp"
#include "config.h"
#include "device.h"
#include "render_state.h"
#include "utils/dx_utils.h"

namespace Dx8to12 {
using ::std::endl;

constexpr char kPixelHeader[] = R"(
#include "ps_common.hlsl"
#include "lighting.hlsl"
)";

static void GenerateArgValue(int stage_index, const TextureStageState &ts,
                             DWORD color_arg, std::stringstream &ss) {
  ASSERT(!HasFlag(ts.transform_flags, D3DTTFF_PROJECTED));
  // D3DTA_ALPHAREPLICATE: use this argument's alpha channel, broadcast to
  // all four components, instead of its color. A real (if apparently rare --
  // never observed triggering the ASSERT this used to be, across every
  // session so far) D3D8 technique for feeding an alpha value (commonly a
  // texture's alpha channel) into a *color* operation, e.g. tinting by
  // opacity. `1.f - X.aaaa` and `(1.f - X).aaaa` are the same value --
  // subtraction and a single-component-replicating swizzle both apply
  // per-component/independently of the others -- so it doesn't matter
  // whether this wraps the argument before or after D3DTA_COMPLEMENT's
  // "1.f - " prefix; applying it at the very end, after everything else
  // below, keeps this one line the only thing that changes for this flag.
  const bool alpha_replicate = HasFlag(color_arg, D3DTA_ALPHAREPLICATE);
  ss << "(";
  if (HasFlag(color_arg, D3DTA_COMPLEMENT)) ss << "1.f - ";
  switch (color_arg & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE:
      ss << "diffuse_color";
      break;
    case D3DTA_CURRENT:
      ss << "result_color";
      break;
    case D3DTA_TEXTURE:
      if (ts.texcoord_index < 8) {
        ss << "g_texture" << stage_index << ".Sample(g_sampler" << stage_index
           << ", IN.oT" << ts.texcoord_index << ".xy)";
      } else {
        uint32_t sampler_index = ts.texcoord_index & 0xFFFF;
        uint32_t automode = ts.texcoord_index & ~0xFFFF;
        if (ts.transform_flags == D3DTTFF_COUNT2) {
          ss << "g_texture";
        } else {
          ASSERT(ts.transform_flags == D3DTTFF_COUNT3);
          ss << "g_texCube";
        }
        ss << stage_index << ".Sample(g_sampler" << sampler_index << ", ";
        switch (automode) {
          case D3DTSS_TCI_CAMERASPACENORMAL:
            ss << "IN.oViewNormal";
            break;
          case D3DTSS_TCI_CAMERASPACEPOSITION:
            ss << "IN.oViewPos";
            break;
          case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:
            ss << "IN.oViewReflect";
            break;
          default:
            FAIL("Unexpected auto-generated tex coord mode 0x%X", automode);
        }
        if (ts.transform_flags == D3DTTFF_COUNT2) {
          ss << ".xy";
        }
        ss << ")";
      }
      break;
    case D3DTA_TFACTOR:
      ss << "texture_factor";
      break;
    case D3DTA_SPECULAR:
      ss << "specular_color";
      break;
    case D3DTA_TEMP:
      // See D3DTSS_RESULTARG (PSMain's temp_color declaration, and the
      // write-target selection in ApplyOperation below) -- this reads
      // whatever an earlier stage most recently redirected there.
      ss << "temp_color";
      break;
    default:
      FAIL("Unsupported texture stage arg 0x%X", color_arg);
  }
  ss << ")";
  if (alpha_replicate) ss << ".aaaa";
}

static void ApplyOperation(const PixelShaderState &s, const char *components,
                           int stage, D3DTEXTUREOP op, DWORD arg1_source,
                           DWORD arg2_source, std::stringstream &ss) {
  ss << "{" << endl;
  ss << "arg1 = ";
  GenerateArgValue(stage, s.ts[stage], arg1_source, ss);
  ss << ";\n";
  ss << "arg2 = ";
  GenerateArgValue(stage, s.ts[stage], arg2_source, ss);
  ss << ";" << endl;
  // Prepare any temporary arguments.
  switch (op) {
    case D3DTOP_MULTIPLYADD:
    case D3DTOP_LERP: {
      DWORD arg0_source = components[0] == 'a' ? s.ts[stage].alpha_arg0
                                               : s.ts[stage].color_arg0;
      ss << "arg0 = ";
      GenerateArgValue(stage, s.ts[stage], arg0_source, ss);
      ss << ";" << endl;
      break;
    }
    case D3DTOP_BLENDTEXTUREALPHA:
    case D3DTOP_BLENDTEXTUREALPHAPM:
      ASSERT(s.stage_has_texture(stage));
      ss << "alpha = ";
      GenerateArgValue(stage, s.ts[stage], D3DTA_TEXTURE, ss);
      ss << ".a;" << endl;
      break;
    case D3DTOP_BLENDFACTORALPHA:
      ss << "alpha = texture_factor.a;" << endl;
      break;
    case D3DTOP_BLENDCURRENTALPHA:
      ss << "alpha = result_color.a;\n";
      break;
    case D3DTOP_BLENDDIFFUSEALPHA:
      ss << "alpha = diffuse_color.a;" << endl;
      break;
    default:
      break;
  }
  // D3DTSS_RESULTARG redirect (see temp_color's declaration in
  // CreatePixelShaderFromState). Only the *write* target changes -- every
  // GenerateArgValue call above and D3DTOP_BLENDCURRENTALPHA's read just
  // above both reference "current" (result_color) unconditionally, which is
  // correct: they mean the running main-chain accumulator regardless of
  // where *this* stage's own output is about to go.
  const char *write_target =
      s.ts[stage].result_arg == D3DTA_TEMP ? "temp_color" : "result_color";
  ss << write_target << "." << components << " = (";
  switch (op) {
    case D3DTOP_SELECTARG1:
      ss << "arg1";
      break;
    case D3DTOP_SELECTARG2:
      ss << "arg2";
      break;
    case D3DTOP_MODULATE:
      ss << "arg1*arg2";
      break;
    case D3DTOP_MODULATE2X:
      ss << "arg1*arg2*2.f";
      break;
    case D3DTOP_MODULATE4X:
      ss << "arg1*arg2*4.f";
      break;
    case D3DTOP_ADD:
      ss << "arg1+arg2";
      break;
    case D3DTOP_ADDSIGNED:
      ss << "arg1 + arg2  - 0.5f";
      break;
    case D3DTOP_ADDSIGNED2X:
      ss << "(arg1 + arg2 - 0.5f) * 2.f";
      break;
    case D3DTOP_SUBTRACT:
      ss << "arg1 - arg2";
      break;
    case D3DTOP_ADDSMOOTH:
      ss << "arg1 + arg2 - arg1*arg2";
      break;
    case D3DTOP_BLENDDIFFUSEALPHA:
    case D3DTOP_BLENDFACTORALPHA:
    case D3DTOP_BLENDTEXTUREALPHA:
    case D3DTOP_BLENDCURRENTALPHA:
      // Same "Arg1*Alpha + Arg2*(1-Alpha)" formula for all four -- they only
      // differ in where `alpha` comes from (set above). Pre-existing bug fix:
      // BLENDTEXTUREALPHA/BLENDCURRENTALPHA previously used "arg1 +
      // arg2*(1-alpha)" (missing the *alpha on arg1), which is actually the
      // formula for the *premultiplied* variant, D3DTOP_BLENDTEXTUREALPHAPM.
      ss << "arg1*alpha + arg2*(1.f-alpha)";
      break;
    case D3DTOP_BLENDTEXTUREALPHAPM:
      // Arg1 is assumed pre-multiplied by alpha already, so it isn't
      // multiplied by alpha again here.
      ss << "arg1 + arg2*(1.f-alpha)";
      break;
    case D3DTOP_DOTPRODUCT3:
      ss << "saturate(dot(arg1-0.5f, arg2-0.5f)).xxxx";
      break;
    case D3DTOP_MULTIPLYADD:
      // Result = Arg0 + Arg1*Arg2.
      ss << "arg0 + arg1*arg2";
      break;
    case D3DTOP_LERP:
      // Result = Arg1*Arg0 + Arg2*(1-Arg0).
      ss << "arg1*arg0 + arg2*(1.f-arg0)";
      break;
    default:
      FAIL("Unsupported texture op %d", op);
  }
  ss << ")." << components << ";\n}\n";
}

// Mod-API pixel shader injection (Dx8to12_PixelShaderInjectionFn, device.h):
// runs the callback (if any) once for this (re)compile and returns whatever
// HLSL it produced, clamped defensively to the buffer actually handed to it
// so a misbehaving mod can't make this read past its own stack buffer.
static std::string GetInjectedSnippet(const PixelShaderState &s,
                                      bool has_normal, bool has_view_pos,
                                      const Device* injection_device) {
  if (!injection_device) return {};
  int active_stage_count = 0;
  for (int i = 0; i < kMaxTexStages; ++i) {
    if (s.ts[i].color_op != D3DTOP_DISABLE) ++active_stage_count;
  }
  const Dx8to12_PixelShaderInjectionContext context{
      .has_normal = has_normal ? 1 : 0,
      .has_view_pos = has_view_pos ? 1 : 0,
      .texture_stage_count = active_stage_count};
  // Generous but bounded -- this only ever runs once per distinct shader
  // permutation (not per frame/draw), so there's no cost pressure to size
  // it tighter.
  std::vector<char> buffer(16384);
  const size_t written = injection_device->InvokePixelShaderInjection(
      &context, buffer.data(), buffer.size());
  // This is the public ABI contract documented in device.h/MODDING.md. A
  // value larger than the supplied buffer is not a truncated snippet (which
  // could turn valid HLSL into a different, invalid program); it means the
  // callback did not produce a usable result.
  if (written == 0 || written > buffer.size()) return {};
  return std::string(buffer.data(), written);
}

ComPtr<ID3DBlob> CreatePixelShaderFromState(
    const PixelShaderState &s, bool has_normal, bool has_view_pos,
    const Device* injection_device) {
  using ::std::endl;
  const std::string injected_snippet =
      GetInjectedSnippet(s, has_normal, has_view_pos, injection_device);
  std::stringstream ss;
  ss << std::dec;
  ss << kPixelHeader;
  ss << "float4 PSMain(FFVertexOutput IN) : SV_Target {\n";
  ss << "float4 diffuse_color = IN.oD0;" << endl;
  ss << "float4 specular_color = IN.oD1;" << endl;
  // LightingMode == PerPixel (1): ff_vertex_shader.hlsl left oD0/oD1 as the
  // raw, un-lit vertex/material color instead of vertex-lighting them (see
  // its PER_PIXEL_LIGHTING branches) -- do that lighting here instead, once
  // per pixel, using the interpolated (and here, renormalized) view-space
  // position/normal. oPerPixelLightingEligible is 0 for pretransformed
  // (XYZRHW/2D UI) vertices, which real D3D8 never lights regardless of
  // D3DRS_LIGHTING -- the vertex shader can tell those apart via its
  // HAS_TRANSFORM permutation define, this generated shader can't, hence the
  // interpolant. Re-reads Dx8to12::GetConfig() at generation time, same as
  // vertex_shader.cpp's PER_PIXEL_LIGHTING define -- both are only ever
  // (re)compiled after Device::OnLightingModeChanged clears the caches that
  // would otherwise serve a stale-mode shader.
  if (Dx8to12::GetConfig().lighting_mode >= 1) {
    ss << "if (IN.oPerPixelLightingEligible > 0.5f && lighting_enabled) {"
       << endl;
    // A per-vertex normal interpolated across a triangle is not unit length
    // even when each vertex's was -- renormalizing here (impossible to do at
    // the vertex stage, since there's nothing to renormalize yet) is what
    // actually makes per-pixel lighting look smoother than per-vertex rather
    // than just being the same math run more often. Guarded against a zero
    // vector (the no-normal-stream case, where oViewNormal is left at its
    // zero-initialized default) to avoid an rsqrt(0) NaN -- ComputeLighting
    // treats a zero normal as "no diffuse/specular, ambient only", same as
    // the vertex-lit path's own zero-normal case.
    ss << "  float3 pp_n = IN.oViewNormal;" << endl;
    ss << "  float pp_len_sq = dot(pp_n, pp_n);" << endl;
    ss << "  pp_n = pp_len_sq > 1e-8f ? pp_n * rsqrt(pp_len_sq) : pp_n;"
       << endl;
    ss << "  float4 pp_specular;" << endl;
    ss << "  diffuse_color = ComputeLighting(IN.oViewPos, pp_n, IN.oD0, "
          "IN.oD1, pp_specular);"
       << endl;
    ss << "  specular_color = pp_specular;" << endl;
    ss << "}" << endl;
  }

  // Mod-API injection point: sees the final diffuse_color/specular_color
  // (post per-pixel-lighting, if enabled) and IN (FFVertexOutput), plus
  // everything lighting.hlsl/ps_common.hlsl already #include (kPixelHeader).
  // Runs before any texture stage, i.e. before result_color/temp_color exist
  // -- it may only read/write diffuse_color/specular_color, not result_color.
  if (!injected_snippet.empty()) {
    ss << "// --- Dx8to12_PixelShaderInjectionFn begin ---\n"
       << injected_snippet << "\n// --- Dx8to12_PixelShaderInjectionFn end ---\n";
  }

  ss << "float4 result_color = diffuse_color;" << endl;
  // D3DTSS_RESULTARG: a stage can redirect its output to this side register
  // (D3DTA_TEMP) instead of the implicit "current" (result_color) chain that
  // feeds the next stage -- e.g. computing an auxiliary value mid-chain
  // without disturbing the main accumulator, then referencing it explicitly
  // via D3DTA_TEMP from a later stage. Zero-initialized: D3D8 doesn't define
  // what D3DTA_TEMP reads as before any stage has written it, and a game
  // that reads it in that state already has undefined behavior on real
  // hardware too -- zero is as reasonable a value as any to give it here.
  ss << "float4 temp_color = 0;" << endl;
  ss << "float4 arg0, arg1, arg2;" << endl;
  ss << "float alpha;" << endl;

  // Color and alpha are independent per-stage chains in real D3D8: setting a
  // stage's ColorOp to D3DTOP_DISABLE terminates only the color chain at
  // that stage, not the alpha chain (and vice versa) -- a common technique
  // is ColorOp=DISABLE (color stays whatever the previous stage/diffuse
  // left it) paired with AlphaOp=SELECTARG1/AlphaArg1=TEXTURE at the same
  // stage, to mask a flat-colored quad's shape using only a texture's alpha
  // channel (e.g. a soft round particle sprite). Previously this loop broke
  // out entirely -- skipping that stage's AlphaOp too -- the instant
  // ColorOp hit DISABLE, which silently dropped the alpha mask and left
  // result_color.a at diffuse (usually ~1.0), rendering particles as solid
  // squares instead of their intended shape.
  bool color_active = true;
  bool alpha_active = true;
  for (int i = 0; i < kMaxTexStages && (color_active || alpha_active); ++i) {
    // ASSERT(s.ts[i].texcoord_index < 8);
    ASSERT(!HasFlag(s.ts[i].transform_flags, D3DTTFF_PROJECTED));
    // D3DTOP_DISABLE (1) is the lowest legal D3DTEXTUREOP value -- a stage
    // that was never actually configured by the game (e.g. an alpha chain
    // that outlives the color chain, reaching stages the game only ever
    // set ColorOp for) can read back as a raw 0, which isn't a valid op at
    // all. Real hardware/drivers evidently treat that the same as DISABLE
    // rather than erroring, so do the same here instead of FAILing on it.
    if (color_active) {
      if (s.ts[i].color_op <= D3DTOP_DISABLE) {
        color_active = false;
      } else {
        ApplyOperation(s, "xyz", i, s.ts[i].color_op, s.ts[i].color_arg1,
                       s.ts[i].color_arg2, ss);
      }
    }
    if (alpha_active) {
      if (s.ts[i].alpha_op <= D3DTOP_DISABLE) {
        alpha_active = false;
      } else {
        ApplyOperation(s, "a", i, s.ts[i].alpha_op, s.ts[i].alpha_arg1,
                       s.ts[i].alpha_arg2, ss);
      }
    }
  }
  // Fog blend. Real D3D8 fog blends into color *after* the texture-stage
  // chain and does not touch alpha, hence rgb-only and placed here, ahead of
  // the alpha test below (which reads result_color.a, untouched by this).
  // IN.oFog is 1 (this pixel's own color, unaffected) unless the vertex
  // shader computed an actual fog factor -- see ComputeFogFactor in
  // common.hlsl and its callers in ff_vertex_shader.hlsl.
  ss << "result_color.rgb = lerp(fog_color.rgb, result_color.rgb, "
       "IN.oFog);"
     << endl;
  if (s.alpha_func() != D3DCMP_ALWAYS) {
    ss << "if (!(result_color.a ";
    switch (s.alpha_func()) {
      case D3DCMP_NEVER:
        ss << "!= result_color.a";
        break;
      case D3DCMP_LESS:
        ss << "< alpha_ref";
        break;
      case D3DCMP_LESSEQUAL:
        ss << "<= alpha_ref";
        break;
      case D3DCMP_GREATER:
        ss << "> alpha_ref";
        break;
      case D3DCMP_EQUAL:
        ss << "== alpha_ref";
        break;
      case D3DCMP_NOTEQUAL:
        ss << "!= alpha_ref";
        break;
      case D3DCMP_GREATEREQUAL:
        ss << ">= alpha_ref";
        break;
      default:
        FAIL("Unexpected alpha func %d", s.alpha_func());
    }
    ss << ")) discard;" << endl;
  }
  ss << "return result_color;" << endl << "}" << endl;

  const std::string code = ss.str();
  auto includer = CreateShaderIncluder();

  ComPtr<ID3DBlob> result_blob;
  ID3DBlob *errorBlob = nullptr;
  HRESULT hr = D3DCompile(code.c_str(), code.size(), "ff_pixel_shader", nullptr,
                          includer.get(), "PSMain", "ps_5_0",
                          D3DCOMPILE_DEBUG | D3DCOMPILE_ENABLE_STRICTNESS |
                              D3DCOMPILE_WARNINGS_ARE_ERRORS,
                          0, result_blob.GetForInit(), &errorBlob);
  if (hr != S_OK && !injected_snippet.empty()) {
    // The mod's injected fragment doesn't compile -- that's the mod's own
    // bug, not Dx8to12's: log it and recompile without the injection so the
    // rest of the game keeps rendering normally (see MODDING.md's "Pixel
    // shader injection" section). Bounded to one retry: the recursive call
    // passes a null injection_callback, so injected_snippet is empty there
    // and this branch can't fire again.
    LOG_ERROR() << "Dx8to12_PixelShaderInjectionFn produced HLSL that failed "
                  "to compile -- disabling the injection for this shader "
                  "permutation and falling back to the un-injected version:"
                << endl
                << code << endl
                << (errorBlob ? (const char *)errorBlob->GetBufferPointer()
                              : "<no error text>")
                << "\n";
    return CreatePixelShaderFromState(s, has_normal, has_view_pos, nullptr);
  }
  if (hr != S_OK) {
    ASSERT(errorBlob);
    ASSERT(reinterpret_cast<const char *>(
               errorBlob->GetBufferPointer())[errorBlob->GetBufferSize() - 1] ==
           0);
    LOG_ERROR() << "Error when compiling shader:" << endl
                << code << endl
                << (const char *)errorBlob->GetBufferPointer() << "\n";
    FAIL("Error when compiling shader:\r\n%s\r\n---\r\n%s", code.c_str(),
         (const char *)errorBlob->GetBufferPointer());
  }
  ASSERT(errorBlob == nullptr);
  LOG(TRACE) << "Successfully created pixel shader.\n";
  return result_blob;
}

}  // namespace Dx8to12
