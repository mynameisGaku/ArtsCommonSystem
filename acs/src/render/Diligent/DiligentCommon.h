// SPDX-License-Identifier: Apache-2.0
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

// Diligent 公式ヘッダ。名前空間 / マクロ汚染を最小化するため、利用したい型だけ
// select-include する。
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

/**
 * ACS の EFormat を Diligent の TEXTURE_FORMAT へ変換する。
 *
 * @param f 変換元の ACS テクスチャフォーマット。
 * @return 対応する Diligent フォーマット (未対応なら TEX_FORMAT_UNKNOWN)。
 */
inline TEXTURE_FORMAT ToDiligent(EFormat f) noexcept {
    switch (f) {
        case EFormat::R8G8B8A8_UNorm:       return TEX_FORMAT_RGBA8_UNORM;
        case EFormat::R8G8B8A8_UNorm_sRGB:  return TEX_FORMAT_RGBA8_UNORM_SRGB;
        case EFormat::R8G8B8A8_UInt:        return TEX_FORMAT_RGBA8_UINT;
        case EFormat::B8G8R8A8_UNorm:       return TEX_FORMAT_BGRA8_UNORM;
        case EFormat::R16G16_Float:         return TEX_FORMAT_RG16_FLOAT;
        case EFormat::R16G16B16A16_Float:   return TEX_FORMAT_RGBA16_FLOAT;
        case EFormat::R11G11B10_Float:      return TEX_FORMAT_R11G11B10_FLOAT;
        case EFormat::R32G32_Float:         return TEX_FORMAT_RG32_FLOAT;
        case EFormat::R32G32B32_Float:      return TEX_FORMAT_RGB32_FLOAT;
        case EFormat::R32G32B32A32_Float:   return TEX_FORMAT_RGBA32_FLOAT;
        case EFormat::D24_UNorm_S8_UInt:    return TEX_FORMAT_D24_UNORM_S8_UINT;
        case EFormat::D32_Float:            return TEX_FORMAT_D32_FLOAT;
        default:                            return TEX_FORMAT_UNKNOWN;
    }
}

/**
 * ACS の EPrimitiveTopology を Diligent の PRIMITIVE_TOPOLOGY へ変換する。
 *
 * @param t 変換元のプリミティブトポロジ。
 * @return 対応する Diligent トポロジ (既定は TRIANGLE_LIST)。
 */
inline PRIMITIVE_TOPOLOGY ToDiligent(EPrimitiveTopology t) noexcept {
    switch (t) {
        case EPrimitiveTopology::PointList:     return PRIMITIVE_TOPOLOGY_POINT_LIST;
        case EPrimitiveTopology::LineList:      return PRIMITIVE_TOPOLOGY_LINE_LIST;
        case EPrimitiveTopology::LineStrip:     return PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case EPrimitiveTopology::TriangleList:  return PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case EPrimitiveTopology::TriangleStrip: return PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

/**
 * ACS の ECullMode を Diligent の CULL_MODE へ変換する。
 *
 * @param c 変換元のカリングモード。
 * @return 対応する Diligent カリングモード (既定は CULL_MODE_NONE)。
 */
inline CULL_MODE ToDiligent(ECullMode c) noexcept {
    switch (c) {
        case ECullMode::None:  return CULL_MODE_NONE;
        case ECullMode::Front: return CULL_MODE_FRONT;
        case ECullMode::Back:  return CULL_MODE_BACK;
    }
    return CULL_MODE_NONE;
}

/**
 * ACS の EBlendMode を Diligent の RenderTargetBlendDesc (RT0) へ反映する。
 *
 * @param m 適用するブレンドモード。
 * @param rt 設定先の RT0 ブレンド記述。
 */
inline void ApplyBlend(EBlendMode m, RenderTargetBlendDesc& rt) noexcept {
    switch (m) {
        case EBlendMode::Opaque:
            rt.BlendEnable = False;
            break;
        case EBlendMode::AlphaBlend:
            rt.BlendEnable    = True;
            rt.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
            rt.DestBlend      = BLEND_FACTOR_INV_SRC_ALPHA;
            rt.BlendOp        = BLEND_OPERATION_ADD;
            rt.SrcBlendAlpha  = BLEND_FACTOR_ONE;
            rt.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
            rt.BlendOpAlpha   = BLEND_OPERATION_ADD;
            break;
        case EBlendMode::Additive:
            rt.BlendEnable    = True;
            rt.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
            rt.DestBlend      = BLEND_FACTOR_ONE;
            rt.BlendOp        = BLEND_OPERATION_ADD;
            rt.SrcBlendAlpha  = BLEND_FACTOR_ONE;
            rt.DestBlendAlpha = BLEND_FACTOR_ONE;
            rt.BlendOpAlpha   = BLEND_OPERATION_ADD;
            break;
        case EBlendMode::Multiply:
            rt.BlendEnable    = True;
            rt.SrcBlend       = BLEND_FACTOR_ZERO;
            rt.DestBlend      = BLEND_FACTOR_SRC_COLOR;
            rt.BlendOp        = BLEND_OPERATION_ADD;
            rt.SrcBlendAlpha  = BLEND_FACTOR_ZERO;
            rt.DestBlendAlpha = BLEND_FACTOR_ONE;
            rt.BlendOpAlpha   = BLEND_OPERATION_ADD;
            break;
        case EBlendMode::AdditivePreserveAlpha:
            rt.BlendEnable    = True;
            rt.SrcBlend       = BLEND_FACTOR_ONE;
            rt.DestBlend      = BLEND_FACTOR_ONE;
            rt.BlendOp        = BLEND_OPERATION_ADD;
            rt.SrcBlendAlpha  = BLEND_FACTOR_ZERO;
            rt.DestBlendAlpha = BLEND_FACTOR_ONE;
            rt.BlendOpAlpha   = BLEND_OPERATION_ADD;
            break;
    }
}

/**
 * ACS の ESamplerFilter を Diligent の FILTER_TYPE へ変換する。
 *
 * @param f 変換元のサンプラフィルタ。
 * @return 対応する Diligent フィルタ (既定は FILTER_TYPE_LINEAR)。
 */
inline FILTER_TYPE ToDiligentFilter(ESamplerFilter f) noexcept {
    switch (f) {
        case ESamplerFilter::Point:       return FILTER_TYPE_POINT;
        case ESamplerFilter::Linear:      return FILTER_TYPE_LINEAR;
        case ESamplerFilter::Anisotropic: return FILTER_TYPE_ANISOTROPIC;
    }
    return FILTER_TYPE_LINEAR;
}

/**
 * ACS の ESamplerAddress を Diligent の TEXTURE_ADDRESS_MODE へ変換する。
 *
 * @param a 変換元のサンプラアドレッシングモード。
 * @return 対応する Diligent アドレスモード (既定は TEXTURE_ADDRESS_WRAP)。
 */
inline TEXTURE_ADDRESS_MODE ToDiligentAddress(ESamplerAddress a) noexcept {
    switch (a) {
        case ESamplerAddress::Wrap:   return TEXTURE_ADDRESS_WRAP;
        case ESamplerAddress::Mirror: return TEXTURE_ADDRESS_MIRROR;
        case ESamplerAddress::Clamp:  return TEXTURE_ADDRESS_CLAMP;
        case ESamplerAddress::Border: return TEXTURE_ADDRESS_BORDER;
    }
    return TEXTURE_ADDRESS_WRAP;
}

// 注意: ACS 側の記述子は FSamplerDesc。Diligent 側の SamplerDesc を使うときは
// 必ず完全修飾 (Diligent::SamplerDesc) で呼ぶ。

/**
 * ACS の EShaderStage を Diligent の SHADER_TYPE へ変換する。
 *
 * @param s 変換元のシェーダステージ。
 * @return 対応する Diligent シェーダタイプ (未対応なら SHADER_TYPE_UNKNOWN)。
 */
inline SHADER_TYPE ToDiligent(EShaderStage s) noexcept {
    switch (s) {
        case EShaderStage::Vertex:  return SHADER_TYPE_VERTEX;
        case EShaderStage::Pixel:   return SHADER_TYPE_PIXEL;
        case EShaderStage::Compute: return SHADER_TYPE_COMPUTE;
    }
    return SHADER_TYPE_UNKNOWN;
}

/**
 * ACS の EResourceState を Diligent の RESOURCE_STATE へ変換する。
 *
 * @param s 変換元のリソース状態。
 * @return 対応する Diligent リソース状態 (未対応なら RESOURCE_STATE_UNKNOWN)。
 */
inline RESOURCE_STATE ToDiligent(EResourceState s) noexcept {
    switch (s) {
        case EResourceState::Common:              return RESOURCE_STATE_COMMON;
        case EResourceState::RenderTarget:        return RESOURCE_STATE_RENDER_TARGET;
        case EResourceState::Present:             return RESOURCE_STATE_PRESENT;
        case EResourceState::CopySrc:             return RESOURCE_STATE_COPY_SOURCE;
        case EResourceState::CopyDst:             return RESOURCE_STATE_COPY_DEST;
        case EResourceState::UnorderedAccess:     return RESOURCE_STATE_UNORDERED_ACCESS;
        case EResourceState::PixelShaderResource: return RESOURCE_STATE_SHADER_RESOURCE;
        case EResourceState::DepthWrite:          return RESOURCE_STATE_DEPTH_WRITE;
        case EResourceState::DepthRead:           return RESOURCE_STATE_DEPTH_READ;
    }
    return RESOURCE_STATE_UNKNOWN;
}

/**
 * セマンティック名を D3D12 風に正規化する (HLSL 側と一致させる)。
 *
 * @param s 入力セマンティック名 (nullptr 可)。
 * @return s が非 null ならそのまま、null なら "POSITION"。
 */
inline const char* NormalizeSemantic(const char* s) noexcept {
    return s ? s : "POSITION";
}

} // namespace acs::diligent_detail
