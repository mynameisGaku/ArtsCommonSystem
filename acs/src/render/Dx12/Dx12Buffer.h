// SPDX-License-Identifier: Apache-2.0
// DX12 GPU バッファ実装
#pragma once

#include "render/IRhiBuffer.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device;

class Dx12Buffer final : public IRhiBuffer {
public:
    Dx12Buffer() noexcept = default;
    ~Dx12Buffer() noexcept override;

    HrResult Init(Dx12Device& device, const FBufferDesc& desc) noexcept;

    usize       Size()  const noexcept override { return m_Size; }
    EBufferUsage Usage() const noexcept override { return m_Usage; }

    void Update(const void* data, usize size, usize offset = 0) noexcept override;

    // 内部使用: GPU リソース取得 + ビュー作成補助
    ID3D12Resource*           Resource() const noexcept { return m_Resource; }

    // 現在の GPU 仮想アドレス（フレームリングなら slot 分のオフセットを足す）
    D3D12_GPU_VIRTUAL_ADDRESS Gpu()      const noexcept;

private:
    Dx12Device*     m_Device       = nullptr;     // フレームスロット問い合わせ用
    ID3D12Resource* m_Resource     = nullptr;
    void*           m_Mapped       = nullptr;     // cpu_writable=true なら永続マップ
    usize           m_Size         = 0;            // 1 フレームスロットあたりサイズ
    usize           m_SlotStride  = 0;            // ring 用ストライド（256 align、リング無効なら 0）
    EBufferUsage     m_Usage        = EBufferUsage::Vertex;
    bool            m_bCpuWritable = false;
    bool            m_bFrameCycled = false;       // Uniform + cpu_writable で自動 ON
};

} // namespace acs
