// SPDX-License-Identifier: Apache-2.0
// DX12 GPU バッファ実装
#pragma once

#include "render/IRhiBuffer.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class FDx12Device;

class FDx12Buffer final : public IRhiBuffer {
public:
    FDx12Buffer() noexcept = default;
    ~FDx12Buffer() noexcept override;

    FHrResult Init(FDx12Device& device, const FBufferDesc& desc) noexcept;

    usize       Size()  const noexcept override { return _size; }
    EBufferUsage Usage() const noexcept override { return _usage; }

    void Update(const void* data, usize size, usize offset = 0) noexcept override;

    // 内部使用: GPU リソース取得 + ビュー作成補助
    ID3D12Resource*           Resource() const noexcept { return _resource; }

    // 現在の GPU 仮想アドレス（フレームリングなら slot 分のオフセットを足す）
    D3D12_GPU_VIRTUAL_ADDRESS Gpu()      const noexcept;

private:
    FDx12Device*     _device       = nullptr;     // フレームスロット問い合わせ用
    ID3D12Resource* _resource     = nullptr;
    void*           _mapped       = nullptr;     // cpu_writable=true なら永続マップ
    usize           _size         = 0;            // 1 フレームスロットあたりサイズ
    usize           _slot_stride  = 0;            // ring 用ストライド（256 align、リング無効なら 0）
    EBufferUsage     _usage        = EBufferUsage::FVertex;
    bool            _cpu_writable = false;
    bool            _frame_cycled = false;       // Uniform + cpu_writable で自動 ON
};

} // namespace acs
