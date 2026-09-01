#include "dx_utils.h"

#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>

#include "config.h"
#include "render_state.h"

namespace Dx8to12 {
static D3D12_FILTER_TYPE ConvertFilterType(D3DTEXTUREFILTERTYPE d3d8_type) {
  switch (d3d8_type) {
    case D3DTEXF_NONE:
      // FAIL("TODO: D3DTEXF_NONE for disabling mips.");
      // LOG(WARNING) << "TODO: D3DTEXF_NONE for disabling mips.\n";
      return D3D12_FILTER_TYPE_POINT;
    case D3DTEXF_POINT:
      return D3D12_FILTER_TYPE_POINT;
    case D3DTEXF_LINEAR:
      return D3D12_FILTER_TYPE_LINEAR;
    default:
      FAIL("Unexpected filter type %d", d3d8_type);
  }
}

static D3D12_FILTER EncodeFilter(D3DTEXTUREFILTERTYPE min_filter,
                                 D3DTEXTUREFILTERTYPE mag_filter,
                                 D3DTEXTUREFILTERTYPE mip_filter) {
  if (min_filter == D3DTEXF_ANISOTROPIC || mag_filter == D3DTEXF_ANISOTROPIC ||
      mip_filter == D3DTEXF_ANISOTROPIC) {
    return D3D12_ENCODE_ANISOTROPIC_FILTER(
        D3D12_FILTER_REDUCTION_TYPE_STANDARD);
  } else {
    return D3D12_ENCODE_BASIC_FILTER(
        ConvertFilterType(min_filter), ConvertFilterType(mag_filter),
        ConvertFilterType(mip_filter), D3D12_FILTER_REDUCTION_TYPE_STANDARD);
  }
}

SamplerDesc::SamplerDesc(const TextureStageState &ts) {
  D3DCOLORVALUE border = Dx8::Color(ts.border_color).ToValue();
  // AnisotropicOverride (dx8to12.ini / Dx8to12_SetSettingInt, see config.h):
  // -1 (default) leaves every sampler exactly as the app requested it --
  // was documented and exposed via the mod API but never actually
  // consumed anywhere in the sampler-building path until now, so setting
  // it previously had zero effect regardless of value. A positive value
  // forces real anisotropic filtering on every sampler, which is the
  // standard fix for the aliasing/streaking a linearly-filtered,
  // single-mip texture shows when a tiled surface (a wall, a road) is
  // viewed at a grazing angle -- confirmed via a RenderDoc capture showing
  // exactly that: FilterMode.Linear/Linear/Linear, MaxAnisotropy=0, on a
  // 256x256 texture with only 1 mip level.
  const int aniso_override = GetConfig().anisotropic_override;
  const bool force_anisotropic = aniso_override > 0;
  *this = D3D12_SAMPLER_DESC{
      .Filter = force_anisotropic
                    ? D3D12_ENCODE_ANISOTROPIC_FILTER(
                          D3D12_FILTER_REDUCTION_TYPE_STANDARD)
                    : EncodeFilter(ts.min_filter, ts.mag_filter,
                                   ts.mip_filter),
      // Luckily D3D12_TEXTURE_ADDRESS_MODE maps directly.
      .AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(ts.address_u),
      .AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(ts.address_v),
      .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      // .AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(ts.address_w),
      .MipLODBias = std::clamp(ts.mipmap_lod_bias, -16.f, 15.99f),
      .MaxAnisotropy = force_anisotropic
                           ? static_cast<UINT>(aniso_override)
                           : ts.max_anisotropy,
      .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
      .BorderColor = {border.r, border.g, border.b, border.a},
      .MinLOD = 0.f,
      // D3DTEXF_NONE for the mip filter means "don't blend between mip
      // levels" -- clamp to the single most-detailed level. Otherwise allow
      // the full chain; the GPU picks the appropriate level per-pixel based
      // on screen-space derivatives, same as real D3D8 hardware would.
      // Forcing anisotropic doesn't change this -- a single-mip texture
      // still only has level 0 to sample from either way, anisotropic
      // filtering just samples it with a wider, angle-aware footprint
      // instead of plain bilinear.
      .MaxLOD = ts.mip_filter == D3DTEXF_NONE ? 0.f : D3D12_FLOAT32_MAX,
  };
}

bool SamplerDesc::operator==(const SamplerDesc &other) const {
  return memcmp(this, &other, sizeof(SamplerDesc)) == 0;
}

D3DFORMAT DXGIToD3DFormat(DXGI_FORMAT dxgi_format) {
  switch (dxgi_format) {
    case DXGI_FORMAT_B8G8R8X8_UNORM:
      return D3DFMT_R8G8B8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return D3DFMT_A8R8G8B8;
    case DXGI_FORMAT_B5G6R5_UNORM:
      return D3DFMT_R5G6B5;
    case DXGI_FORMAT_B4G4R4A4_UNORM:
      return D3DFMT_A4R4G4B4;
    case DXGI_FORMAT_B5G5R5A1_UNORM:
      return D3DFMT_A1R5G5B5;
    case DXGI_FORMAT_R32G32_FLOAT:
      return D3DFMT_R3G3B2;
    case DXGI_FORMAT_D32_FLOAT:
      return D3DFMT_D32;
    case DXGI_FORMAT_D16_UNORM:
      return D3DFMT_D16;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return D3DFMT_D24S8;
    // Depth-stencil resources are created typeless (see
    // DepthTypelessFromConcrete/BaseTexture::Create) -- the only textures
    // that ever carry these formats are depth-stencil ones, so report back
    // the concrete D3D8 depth format they represent. Covers call sites like
    // BaseTexture::GetSurfaceDesc that read resource_desc_.Format generically
    // without knowing it came from a depth-stencil resource.
    case DXGI_FORMAT_R32_TYPELESS:
      return D3DFMT_D32;
    case DXGI_FORMAT_R16_TYPELESS:
      return D3DFMT_D16;
    case DXGI_FORMAT_R24G8_TYPELESS:
      return D3DFMT_D24S8;
    case DXGI_FORMAT_A8_UNORM:
      return D3DFMT_A8;
    case DXGI_FORMAT_R16_UINT:
      return D3DFMT_INDEX16;
    case DXGI_FORMAT_R32_UINT:
      return D3DFMT_INDEX32;
    case DXGI_FORMAT_BC1_UNORM:
      return D3DFMT_DXT1;
    // BC2/BC3 don't distinguish the premultiplied-alpha DXT2/DXT4 variants
    // from their non-premultiplied DXT3/DXT5 counterparts -- premultiplied
    // vs. not is purely an app-side contract about what the encoded alpha
    // means, not something recoverable from the DXGI format alone. This
    // reverse mapping (reachable via GetSurfaceDesc/GetLevelDesc reporting
    // a format back to the app) picks the more common non-premultiplied
    // variant; it never affects how the texture is actually sampled, since
    // that's driven by DXGIFromD3DFormat (the forward direction, which reads
    // the app's real original format at CreateTexture time).
    case DXGI_FORMAT_BC2_UNORM:
      return D3DFMT_DXT3;
    case DXGI_FORMAT_BC3_UNORM:
      return D3DFMT_DXT5;
    default:
      FAIL("Unimplemented DXGI_FORMAT %d\n", dxgi_format);
  }
}

DXGI_FORMAT DXGIFromD3DFormat(D3DFORMAT d3d_format) {
  switch (d3d_format) {
    case D3DFMT_X8R8G8B8:
      return DXGI_FORMAT_B8G8R8X8_UNORM;
    case D3DFMT_A8R8G8B8:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_R5G6B5:
      return DXGI_FORMAT_B5G6R5_UNORM;
    case D3DFMT_A4R4G4B4:
      return DXGI_FORMAT_B4G4R4A4_UNORM;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
      return DXGI_FORMAT_B5G5R5A1_UNORM;
    case D3DFMT_R3G3B2:
      return DXGI_FORMAT_R32G32_FLOAT;
    case D3DFMT_A8:
      return DXGI_FORMAT_A8_UNORM;
    case D3DFMT_D32:
      return DXGI_FORMAT_D32_FLOAT;
    case D3DFMT_D16:
      return DXGI_FORMAT_D16_UNORM;
    case D3DFMT_INDEX16:
      return DXGI_FORMAT_R16_UINT;
    case D3DFMT_INDEX32:
      return DXGI_FORMAT_R32_UINT;
    case D3DFMT_V8U8:
      return DXGI_FORMAT_R8G8_SNORM;
    case D3DFMT_Q8W8V8U8:
      return DXGI_FORMAT_R8G8B8A8_SNORM;
    case D3DFMT_V16U16:
      return DXGI_FORMAT_R16G16_SNORM;
    case D3DFMT_L8:
      // NOTE: prevents a crash (DXGIFormatSize previously had no case for
      // this either) and lets the texture actually be created/uploaded
      // correctly, but sampling it in a shader still just returns the
      // luminance value in the red channel with 0 in green/blue --
      // GenerateArgValue (ff_pixel_shader.cpp) doesn't know this texture is
      // luminance-semantic and needs an .rrr swizzle. Correctness gap, not
      // attempted here since it needs per-stage format info threaded into
      // shader codegen, which isn't tracked anywhere currently.
      return DXGI_FORMAT_R8_UNORM;
    case D3DFMT_A8L8:
      // Same luminance-swizzle gap as D3DFMT_L8 above (here R=luminance,
      // G=alpha, needs .rrrg not raw .rgba).
      return DXGI_FORMAT_R8G8_UNORM;
    case D3DFMT_P8:
    case D3DFMT_A4L4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8:
    case D3DFMT_L6V5U5:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_W11V11U10:
    case D3DFMT_A2W10V10U10:
    case D3DFMT_UYVY:
    case D3DFMT_YUY2:
      return DXGI_FORMAT_UNKNOWN;

    case D3DFMT_DXT1:
      return DXGI_FORMAT_BC1_UNORM;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
      // BC2 doesn't distinguish DXT2's premultiplied alpha from DXT3's
      // straight alpha -- that's purely an app-side interpretation of what
      // the encoded alpha channel means, not something the GPU format
      // itself encodes differently. An app using DXT2 already committed to
      // premultiplied-alpha blending on its own end; we just store/sample
      // the same bits BC2 would for DXT3.
      return DXGI_FORMAT_BC2_UNORM;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
      // Same premultiplied-alpha caveat as DXT2/DXT3 above, for BC3.
      return DXGI_FORMAT_BC3_UNORM;

    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
      // The single most common D3D8-era depth-stencil format on real
      // hardware (X8/X4S4 just leave some bits unused; DXGI has no
      // depth-only 24-bit format to map those to separately, so they share
      // this too). Previously unmapped entirely -- CheckDeviceFormat would
      // report it unsupported, so a game probing formats (as GTA VC does)
      // would fall back to whatever format *was* supported (D3DFMT_D32),
      // silently landing on 32-bit float depth precision it never actually
      // asked for and that real period hardware mostly didn't have,
      // changing depth-test outcomes for anything drawn near-coplanar with
      // other geometry (e.g. ground decals) versus the original game.
      return DXGI_FORMAT_D24_UNORM_S8_UINT;

    case D3DFMT_R8G8B8:
      return DXGI_FORMAT_UNKNOWN;
    default:
      FAIL("Unimplemented D3DFORMAT %d\n", d3d_format);
  }
}

DXGI_FORMAT DepthTypelessFromConcrete(DXGI_FORMAT concrete_depth) {
  switch (concrete_depth) {
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
      return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24G8_TYPELESS;
    default:
      FAIL("Unhandled concrete depth DXGI_FORMAT %d\n", concrete_depth);
  }
}

DXGI_FORMAT DepthDsvFormatFromTypeless(DXGI_FORMAT typeless) {
  switch (typeless) {
    case DXGI_FORMAT_R32_TYPELESS:
      return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
      return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    default:
      FAIL("Unhandled typeless depth DXGI_FORMAT %d\n", typeless);
  }
}

DXGI_FORMAT DepthSrvFormatFromTypeless(DXGI_FORMAT typeless) {
  switch (typeless) {
    case DXGI_FORMAT_R32_TYPELESS:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default:
      FAIL("Unhandled typeless depth DXGI_FORMAT %d\n", typeless);
  }
}

int DXGIFormatSize(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return 4;
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
      return 2;
    // Typeless depth-stencil resource formats (see DepthTypelessFromConcrete)
    // -- byte size per texel is identical to their concrete counterpart
    // regardless of which view (DSV/SRV) is used to interpret it.
    case DXGI_FORMAT_R32_TYPELESS:
      return 4;  // matches DXGI_FORMAT_D32_FLOAT
    case DXGI_FORMAT_R24G8_TYPELESS:
      return 4;  // matches DXGI_FORMAT_D24_UNORM_S8_UINT
    case DXGI_FORMAT_R16_TYPELESS:
      return 2;  // matches DXGI_FORMAT_D16_UNORM
    case DXGI_FORMAT_B8G8R8X8_UNORM:
      // This is tricky. We need to make sure DX8 can never lock R8G8B8
      // textures.
      return 4;
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    case DXGI_FORMAT_R8G8_UNORM:
      return 2;
    // The motion vector target (DX8TO12_MOTION_VECTORS). No D3D8 game can ask
    // for this format -- it is deliberately absent from DXGIToD3DFormat and
    // DXGIFromD3DFormat -- but every BaseTexture, internal ones included,
    // sizes its footprints through here.
    case DXGI_FORMAT_R16G16_FLOAT:
      return 4;
    default:
      FAIL("Unexpected format %d", format);
  }
}

bool IsBlockCompressedFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
      return true;
    default:
      return false;
  }
}

int DXGIBlockSize(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
      return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
      return 16;
    default:
      FAIL("DXGIBlockSize: %d is not a block-compressed format", format);
  }
}

ScopedGpuMarker::ScopedGpuMarker(ID3D12GraphicsCommandList *cmd_list,
                                 const char *annotation)
    : cmd_list_(cmd_list) {
  // LTO should hopefully remove the useless strlens.
  cmd_list->BeginEvent(1, annotation, strlen(annotation) + 1);
}

ScopedGpuMarker::~ScopedGpuMarker() { cmd_list_->EndEvent(); }
}  // namespace Dx8to12