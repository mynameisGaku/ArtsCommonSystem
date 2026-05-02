// DX12 スワップチェイン実装
#include "render/Dx12/Dx12Swapchain.h"
#include "render/Dx12/Dx12Device.h"
#include "platform/Window.h"
#include "memory/UniquePtr.h"

namespace acs {

Dx12Swapchain::~Dx12Swapchain() noexcept {
    ReleaseBuffers();
    ACS_SAFE_RELEASE(_rtv_heap);
    ACS_SAFE_RELEASE(_swapchain);
}

HrResult Dx12Swapchain::Init(Dx12Device& device, const SwapchainConfig& cfg) noexcept {
    HrResult r{};
    _device = &device;
    _buffer_count = (cfg.buffer_count >= 2 && cfg.buffer_count <= kMaxBuffers) ? cfg.buffer_count : 2;
    _vsync = cfg.vsync;
    _width  = cfg.window ? cfg.window->Width()  : 0;
    _height = cfg.window ? cfg.window->Height() : 0;

    // スワップチェイン記述
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width  = _width;
    sd.Height = _height;
    sd.Format = ToDxgiFormat(cfg.format);
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = _buffer_count;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    IDXGISwapChain1* sc1 = nullptr;
    HWND hwnd = cfg.window ? static_cast<HWND>(cfg.window->NativeHandle()) : nullptr;
    if (!hwnd) { r.hr = E_INVALIDARG; return r; }

    r.hr = device.DxgiFactory()->CreateSwapChainForHwnd(
        device.GraphicsQueue(), hwnd, &sd, nullptr, nullptr, &sc1);
    if (r.IsErr()) return r;

    // SwapChain1 → SwapChain3 へキャスト（GetCurrentBackBufferIndex を使うため）
    r.hr = sc1->QueryInterface(IID_PPV_ARGS(&_swapchain));
    sc1->Release();
    if (r.IsErr()) return r;

    // Alt-Enter での切り替えを抑止
    device.DxgiFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // RTV 用デスクリプタヒープ作成
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = _buffer_count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    r.hr = device.D3DDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_rtv_heap));
    if (r.IsErr()) return r;
    _rtv_size = device.D3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    return AcquireBuffers(device);
}

void Dx12Swapchain::ReleaseBuffers() noexcept {
    for (u32 i = 0; i < _buffer_count; ++i) ACS_SAFE_RELEASE(_back_buffers[i]);
}

// 各バックバッファ用に ID3D12Resource を取得し、RTV を作成する
HrResult Dx12Swapchain::AcquireBuffers(Dx12Device& device) noexcept {
    HrResult r{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = _rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < _buffer_count; ++i) {
        r.hr = _swapchain->GetBuffer(i, IID_PPV_ARGS(&_back_buffers[i]));
        if (r.IsErr()) return r;
        device.D3DDevice()->CreateRenderTargetView(_back_buffers[i], nullptr, rtv);
        rtv.ptr += _rtv_size;
    }
    return r;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Swapchain::BackBufferRTV(u32 i) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = _rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(_rtv_size) * i;
    return rtv;
}

u32 Dx12Swapchain::AcquireNextImage() noexcept {
    return _swapchain->GetCurrentBackBufferIndex();
}

void Dx12Swapchain::Present() noexcept {
    UINT sync_interval = _vsync ? 1 : 0;
    UINT flags = _vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING;
    _swapchain->Present(sync_interval, flags);
}

void Dx12Swapchain::Resize(u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) return;
    if (width == _width && height == _height) return;
    if (!_device || !_swapchain) return;

    // 進行中の GPU 作業が終わるまで待ってから解放しないと「使用中」エラーになる
    _device->WaitIdle();
    ReleaseBuffers();

    HRESULT hr = _swapchain->ResizeBuffers(_buffer_count, width, height,
                                            DXGI_FORMAT_UNKNOWN,
                                            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    if (FAILED(hr)) return;

    _width = width;
    _height = height;
    AcquireBuffers(*_device);
}

// ファクトリ関数: CreateRhiSwapchain の DX12 実装
Result<UniquePtr<IRhiSwapchain>> CreateRhiSwapchain(IRhiDevice& device,
                                                    const SwapchainConfig& cfg) noexcept {
    // RTTI 無効のため dynamic_cast は使えない。バックエンド名で判定する
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 10, "CreateRhiSwapchain: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto sc = MakeUnique<Dx12Swapchain>();
    HrResult r = sc->Init(*dxd, cfg);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 11, "Dx12Swapchain::Init failed", static_cast<u32>(r.hr));
    }
    UniquePtr<IRhiSwapchain> base(sc.Release(), sc.GetAllocator());
    return Result<UniquePtr<IRhiSwapchain>>(OkInit, Move(base));
}

} // namespace acs
