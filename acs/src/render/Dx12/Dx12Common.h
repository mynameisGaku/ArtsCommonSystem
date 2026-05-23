// SPDX-License-Identifier: Apache-2.0
// DX12 バックエンド共通ヘッダ（COM ポインタ、変換ユーティリティ）
#pragma once

#include "foundation/Types.h"
#include "foundation/Platform.h"
#include "render/RhiTypes.h"

#include <d3d12.h>
#include <dxgi1_6.h>

namespace acs {

// 失敗時に HRESULT を保持する小さな結果ラッパ（既存 Result との橋渡し）
struct HrResult {
    HRESULT hr = S_OK;
    bool IsOk()  const noexcept { return SUCCEEDED(hr); }
    bool IsErr() const noexcept { return FAILED(hr); }
};

// EFormat → DXGI_FORMAT 変換
inline DXGI_FORMAT ToDxgiFormat(EFormat f) noexcept {
    switch (f) {
        case EFormat::R8G8B8A8_UNorm:        return DXGI_FORMAT_R8G8B8A8_UNORM;
        case EFormat::R8G8B8A8_UNorm_sRGB:   return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case EFormat::R8G8B8A8_UInt:         return DXGI_FORMAT_R8G8B8A8_UINT;
        case EFormat::B8G8R8A8_UNorm:        return DXGI_FORMAT_B8G8R8A8_UNORM;
        case EFormat::R16G16_Float:          return DXGI_FORMAT_R16G16_FLOAT;
        case EFormat::R16G16B16A16_Float:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case EFormat::R11G11B10_Float:       return DXGI_FORMAT_R11G11B10_FLOAT;
        case EFormat::R32G32_Float:          return DXGI_FORMAT_R32G32_FLOAT;
        case EFormat::R32G32B32_Float:       return DXGI_FORMAT_R32G32B32_FLOAT;
        case EFormat::R32G32B32A32_Float:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case EFormat::D24_UNorm_S8_UInt:     return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case EFormat::D32_Float:             return DXGI_FORMAT_D32_FLOAT;
        default:                             return DXGI_FORMAT_UNKNOWN;
    }
}

// EResourceState → D3D12_RESOURCE_STATES 変換
inline D3D12_RESOURCE_STATES ToD3D12State(EResourceState s) noexcept {
    switch (s) {
        case EResourceState::Common:               return D3D12_RESOURCE_STATE_COMMON;
        case EResourceState::RenderTarget:         return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case EResourceState::Present:              return D3D12_RESOURCE_STATE_PRESENT;
        case EResourceState::CopySrc:              return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case EResourceState::CopyDst:              return D3D12_RESOURCE_STATE_COPY_DEST;
        case EResourceState::UnorderedAccess:      return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case EResourceState::PixelShaderResource:  return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case EResourceState::DepthWrite:           return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case EResourceState::DepthRead:            return D3D12_RESOURCE_STATE_DEPTH_READ;
        default:                                   return D3D12_RESOURCE_STATE_COMMON;
    }
}

// COM ポインタを安全に解放するマクロ
#define ACS_SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

} // namespace acs
