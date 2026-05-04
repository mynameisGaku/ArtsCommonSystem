// Diligent Engine 共通ヘッダ
// ACS の RhiTypes と Diligent の対応構造体を相互変換する関数を集約。
// .cpp 側からのみインクルードする想定（IRhi* 利用側には漏らさない）。
#pragma once

#include "foundation/Types.h"
#include "render/RhiTypes.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiSampler.h"
#include "render/IRhiShader.h"

// ---- Diligent 公式ヘッダ ----
// 名前空間 / マクロ汚染を最小化するため、利用したい型だけ select-include する。
#define ENGINE_DLL 0
#define D3D12_SUPPORTED 1
// Diligent は内部で <Windows.h> を引くが、ACS の Platform.h で macro #undef は実施済み。
// 念のためここでもガードする。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "EngineFactoryD3D12.h"
#if WITH_RENDER_DILIGENT_VULKAN
#include "EngineFactoryVk.h"
#endif
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "SwapChain.h"
#include "Buffer.h"
#include "Texture.h"
#include "TextureView.h"
#include "Shader.h"
#include "PipelineState.h"
#include "ShaderResourceBinding.h"
#include "Fence.h"
#include "GraphicsTypes.h"
#include "RefCntAutoPtr.hpp"

namespace acs::diligent_detail {

using namespace Diligent;

// Format: ACS Format → Diligent TEXTURE_FORMAT
inline TEXTURE_FORMAT ToDiligent(Format f) noexcept {
    switch (f) {
        case Format::R8G8B8A8_UNorm:       return TEX_FORMAT_RGBA8_UNORM;
        case Format::R8G8B8A8_UNorm_sRGB:  return TEX_FORMAT_RGBA8_UNORM_SRGB;
        case Format::R8G8B8A8_UInt:        return TEX_FORMAT_RGBA8_UINT;
        case Format::B8G8R8A8_UNorm:       return TEX_FORMAT_BGRA8_UNORM;
        case Format::R16G16B16A16_Float:   return TEX_FORMAT_RGBA16_FLOAT;
        case Format::R32G32_Float:         return TEX_FORMAT_RG32_FLOAT;
        case Format::R32G32B32_Float:      return TEX_FORMAT_RGB32_FLOAT;
        case Format::R32G32B32A32_Float:   return TEX_FORMAT_RGBA32_FLOAT;
        case Format::D24_UNorm_S8_UInt:    return TEX_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_Float:            return TEX_FORMAT_D32_FLOAT;
        default:                            return TEX_FORMAT_UNKNOWN;
    }
}

// Topology
inline PRIMITIVE_TOPOLOGY ToDiligent(PrimitiveTopology t) noexcept {
    switch (t) {
        case PrimitiveTopology::PointList:     return PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PrimitiveTopology::LineList:      return PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::LineStrip:     return PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::TriangleList:  return PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip: return PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// CullMode
inline CULL_MODE ToDiligent(CullMode c) noexcept {
    switch (c) {
        case CullMode::None:  return CULL_MODE_NONE;
        case CullMode::Front: return CULL_MODE_FRONT;
        case CullMode::Back:  return CULL_MODE_BACK;
    }
    return CULL_MODE_NONE;
}

// BlendMode → BlendStateDesc 設定（RT0 のみ）
inline void ApplyBlend(BlendMode m, RenderTargetBlendDesc& rt) noexcept {
    switch (m) {
        case BlendMode::Opaque:
            rt.BlendEnable = False;
            break;
        case BlendMode::AlphaBlend:
            rt.BlendEnable    = True;
            rt.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
            rt.DestBlend      = BLEND_FACTOR_INV_SRC_ALPHA;
            rt.BlendOp        = BLEND_OPERATION_ADD;
            rt.SrcBlendAlpha  = BLEND_FACTOR_ONE;
            rt.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
            rt.BlendOpAlpha   = BLEND_OPERATION_ADD;
            break;
        case BlendMode::Additive:
            rt.BlendEnable    = True;
            rt.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
            rt.DestBlend      = BLEND_FACTOR_ONE;
            rt.BlendOp        = BLEND_OPERATION_ADD;
            rt.SrcBlendAlpha  = BLEND_FACTOR_ONE;
            rt.DestBlendAlpha = BLEND_FACTOR_ONE;
            rt.BlendOpAlpha   = BLEND_OPERATION_ADD;
            break;
    }
}

// Sampler
inline FILTER_TYPE ToDiligentFilter(SamplerFilter f) noexcept {
    switch (f) {
        case SamplerFilter::Point:       return FILTER_TYPE_POINT;
        case SamplerFilter::Linear:      return FILTER_TYPE_LINEAR;
        case SamplerFilter::Anisotropic: return FILTER_TYPE_ANISOTROPIC;
    }
    return FILTER_TYPE_LINEAR;
}

inline TEXTURE_ADDRESS_MODE ToDiligentAddress(SamplerAddress a) noexcept {
    switch (a) {
        case SamplerAddress::Wrap:   return TEXTURE_ADDRESS_WRAP;
        case SamplerAddress::Mirror: return TEXTURE_ADDRESS_MIRROR;
        case SamplerAddress::Clamp:  return TEXTURE_ADDRESS_CLAMP;
        case SamplerAddress::Border: return TEXTURE_ADDRESS_BORDER;
    }
    return TEXTURE_ADDRESS_WRAP;
}

// 注意: SamplerDesc は ACS 名前空間にも同名がある。Diligent::SamplerDesc を使うときは
// 必ず完全修飾 (Diligent::SamplerDesc) で呼ぶ。

// Shader stage
inline SHADER_TYPE ToDiligent(ShaderStage s) noexcept {
    switch (s) {
        case ShaderStage::Vertex:  return SHADER_TYPE_VERTEX;
        case ShaderStage::Pixel:   return SHADER_TYPE_PIXEL;
        case ShaderStage::Compute: return SHADER_TYPE_COMPUTE;
    }
    return SHADER_TYPE_UNKNOWN;
}

// ResourceState
inline RESOURCE_STATE ToDiligent(ResourceState s) noexcept {
    switch (s) {
        case ResourceState::Common:              return RESOURCE_STATE_COMMON;
        case ResourceState::RenderTarget:        return RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::Present:             return RESOURCE_STATE_PRESENT;
        case ResourceState::CopySrc:             return RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::CopyDst:             return RESOURCE_STATE_COPY_DEST;
        case ResourceState::UnorderedAccess:     return RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceState::PixelShaderResource: return RESOURCE_STATE_SHADER_RESOURCE;
        case ResourceState::DepthWrite:          return RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::DepthRead:           return RESOURCE_STATE_DEPTH_READ;
    }
    return RESOURCE_STATE_UNKNOWN;
}

// セマンティック名を D3D12 風に正規化（HLSL 側と一致）
inline const char* NormalizeSemantic(const char* s) noexcept {
    return s ? s : "POSITION";
}

} // namespace acs::diligent_detail
