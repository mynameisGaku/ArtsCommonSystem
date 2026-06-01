// SPDX-License-Identifier: Apache-2.0
// DX12 テクスチャ実装
#pragma once

#include "render/IRhiTexture.h"
#include "render/Dx12/Dx12Common.h"
#include "container/Array.h"

namespace acs {

class Dx12Device;

class Dx12Texture final : public IRhiTexture {
public:
    Dx12Texture() noexcept = default;
    ~Dx12Texture() noexcept override;

    HrResult Init(Dx12Device& device, const FTextureDesc& desc) noexcept;

    u32    Width()       const noexcept override { return m_Width; }
    u32    Height()      const noexcept override { return m_Height; }
    EFormat EPixelFormat() const noexcept override { return m_Format; }
    u32    MipLevels()   const noexcept override { return m_MipLevels; }
    u32    ArraySize()   const noexcept override { return m_ArraySize; }
    bool   IsCubemap()   const noexcept override { return m_IsCubemap; }

    // 内部使用: SRV の GPU ハンドル（ルート記述子テーブルにバインド）
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle() const noexcept;

    // 内部使用: DSV ハンドル（深度バッファのみ）
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle() const noexcept;
    // 内部使用: RTV ハンドル（オフスクリーン RT のみ）。引数なし版は先頭 (slice0/mip0) を返す。
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle() const noexcept;
    // 内部使用: per-slice RTV ハンドル (per_slice_rtv=true で作成した cubemap 面 / 配列スライス / mip)。
    // index = slice * mip_levels + mip。範囲外は null ハンドルを返す。
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandleForSlice(u32 slice, u32 mip) const noexcept;
    bool                        IsDepth()       const noexcept { return m_IsDepth; }
    // 深度バッファが stencil 成分を持つか (D24_UNorm_S8_UInt)。clear で stencil
    // フラグを足すか、マスク描画で使えるかの判定に使う。
    bool                        HasStencil()    const noexcept {
        return m_IsDepth && m_Format == EFormat::D24_UNorm_S8_UInt;
    }
    bool                        HasSrv()        const noexcept { return m_SrvSlot >= 0; }
    bool                        HasRtv()        const noexcept { return !m_RtvSlots.IsEmpty(); }
    ID3D12Resource*             Resource()      const noexcept { return m_Resource; }

    // CommandList が状態遷移バリアを発行した後に呼んで反映する
    D3D12_RESOURCE_STATES CurrentState() const noexcept { return m_CurrentState; }
    void SetCurrentState(D3D12_RESOURCE_STATES s) noexcept { m_CurrentState = s; }

private:
    Dx12Device*           m_Device   = nullptr;
    ID3D12Resource*       m_Resource = nullptr;
    u32                   m_Width    = 0;
    u32                   m_Height   = 0;
    EFormat                m_Format   = EFormat::Unknown;
    u32                   m_MipLevels = 1;
    u32                   m_ArraySize = 1;
    bool                  m_IsCubemap = false;
    i32                   m_SrvSlot = -1;
    i32                   m_DsvSlot = -1;
    // RTV スロット群。単一 RT は 1 個、per_slice_rtv は array_size*mip_levels 個
    // (index = slice*mip_levels + mip)。空なら RT ではない。
    TArray<i32>           m_RtvSlots;
    bool                  m_IsDepth = false;
    D3D12_RESOURCE_STATES m_CurrentState = D3D12_RESOURCE_STATE_COMMON;
};

} // namespace acs
