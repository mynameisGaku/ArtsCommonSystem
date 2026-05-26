// SPDX-License-Identifier: Apache-2.0
// DX12 テクスチャ実装
#pragma once

#include "render/IRhiTexture.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class FDx12Device;

class FDx12Texture final : public IRhiTexture {
public:
    FDx12Texture() noexcept = default;
    ~FDx12Texture() noexcept override;

    FHrResult Init(FDx12Device& device, const FTextureDesc& desc) noexcept;

    u32    Width()       const noexcept override { return _width; }
    u32    Height()      const noexcept override { return _height; }
    EFormat EPixelFormat() const noexcept override { return _format; }

    // 内部使用: SRV の GPU ハンドル（ルート記述子テーブルにバインド）
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle() const noexcept;

    // 内部使用: DSV ハンドル（深度バッファのみ）
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle() const noexcept;
    bool                        IsDepth()       const noexcept { return _is_depth; }
    bool                        HasSrv()        const noexcept { return _srv_slot >= 0; }
    ID3D12Resource*             Resource()      const noexcept { return _resource; }

    // CommandList が状態遷移バリアを発行した後に呼んで反映する
    D3D12_RESOURCE_STATES CurrentState() const noexcept { return _current_state; }
    void SetCurrentState(D3D12_RESOURCE_STATES s) noexcept { _current_state = s; }

private:
    FDx12Device*           _device   = nullptr;
    ID3D12Resource*       _resource = nullptr;
    u32                   _width    = 0;
    u32                   _height   = 0;
    EFormat                _format   = EFormat::Unknown;
    i32                   _srv_slot = -1;
    i32                   _dsv_slot = -1;
    bool                  _is_depth = false;
    D3D12_RESOURCE_STATES _current_state = D3D12_RESOURCE_STATE_COMMON;
};

} // namespace acs
