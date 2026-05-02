// DX12 デバイス実装
#pragma once

#include "render/IRhiDevice.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device final : public IRhiDevice {
public:
    Dx12Device() noexcept = default;
    ~Dx12Device() noexcept override;

    // 内部使用: DXGI ファクトリ・D3D12 デバイス・コマンドキューを取得
    IDXGIFactory6*    DxgiFactory()     const noexcept { return _factory; }
    ID3D12Device*     D3DDevice()       const noexcept { return _device; }
    ID3D12CommandQueue* GraphicsQueue() const noexcept { return _gfx_queue; }

    const char* BackendName() const noexcept override { return "DX12"; }
    const char* AdapterName() const noexcept override { return _adapter_name; }
    void WaitIdle() noexcept override;

    // 初期化（CreateRhiDevice から呼ばれる）
    HrResult Init(const DeviceConfig& cfg) noexcept;

private:
    IDXGIFactory6*       _factory   = nullptr;
    IDXGIAdapter1*       _adapter   = nullptr;
    ID3D12Device*        _device    = nullptr;
    ID3D12CommandQueue*  _gfx_queue = nullptr;
    ID3D12Fence*         _idle_fence = nullptr;
    HANDLE               _idle_event = nullptr;
    u64                  _idle_value = 0;
    char                 _adapter_name[128]{};
};

} // namespace acs
