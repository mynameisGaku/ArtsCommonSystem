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

    HrResult Init(Dx12Device& device, const BufferDesc& desc) noexcept;

    usize       Size()  const noexcept override { return _size; }
    BufferUsage Usage() const noexcept override { return _usage; }

    void Update(const void* data, usize size, usize offset = 0) noexcept override;

    // 内部使用: GPU リソース取得 + ビュー作成補助
    ID3D12Resource*           Resource() const noexcept { return _resource; }
    D3D12_GPU_VIRTUAL_ADDRESS Gpu()      const noexcept { return _resource ? _resource->GetGPUVirtualAddress() : 0; }

private:
    ID3D12Resource* _resource     = nullptr;
    void*           _mapped       = nullptr;     // cpu_writable=true なら永続マップ
    usize           _size         = 0;
    BufferUsage     _usage        = BufferUsage::Vertex;
    bool            _cpu_writable = false;
};

} // namespace acs
