// SPDX-License-Identifier: Apache-2.0
// DX12 スワップチェイン実装
#include "render/Dx12/Dx12Swapchain.h"
#include "render/Dx12/Dx12Device.h"
#include "platform/Window.h"
#include "memory/UniquePtr.h"

namespace acs {

Dx12Swapchain::~Dx12Swapchain() noexcept {
    ReleaseBuffers();
    ACS_SAFE_RELEASE(m_RtvHeap);
    ACS_SAFE_RELEASE(m_Swapchain);
}

HrResult Dx12Swapchain::Init(Dx12Device& device, const SwapchainConfig& cfg) noexcept {
    HrResult r{};
    m_Device = &device;
    m_BufferCount = (cfg.buffer_count >= 2 && cfg.buffer_count <= kMaxBuffers) ? cfg.buffer_count : 2;
    m_bVsync = cfg.vsync;
    m_Width  = cfg.window ? cfg.window->Width()  : 0;
    m_Height = cfg.window ? cfg.window->Height() : 0;

    // スワップチェイン記述
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width  = m_Width;
    sd.Height = m_Height;
    sd.Format = ToDxgiFormat(cfg.format);
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = m_BufferCount;
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
    r.hr = sc1->QueryInterface(IID_PPV_ARGS(&m_Swapchain));
    sc1->Release();
    if (r.IsErr()) return r;

    // Alt-Enter での切り替えを抑止
    device.DxgiFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // RTV 用デスクリプタヒープ作成
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = m_BufferCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    r.hr = device.D3DDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_RtvHeap));
    if (r.IsErr()) return r;
    m_RtvSize = device.D3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    return AcquireBuffers(device);
}

void Dx12Swapchain::ReleaseBuffers() noexcept {
    for (u32 i = 0; i < m_BufferCount; ++i) ACS_SAFE_RELEASE(m_BackBuffers[i]);
}

// 各バックバッファ用に ID3D12Resource を取得し、RTV を作成する
HrResult Dx12Swapchain::AcquireBuffers(Dx12Device& device) noexcept {
    HrResult r{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < m_BufferCount; ++i) {
        r.hr = m_Swapchain->GetBuffer(i, IID_PPV_ARGS(&m_BackBuffers[i]));
        if (r.IsErr()) return r;
        device.D3DDevice()->CreateRenderTargetView(m_BackBuffers[i], nullptr, rtv);
        rtv.ptr += m_RtvSize;
    }
    return r;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Swapchain::BackBufferRTV(u32 i) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_RtvSize) * i;
    return rtv;
}

u32 Dx12Swapchain::AcquireNextImage() noexcept {
    return m_Swapchain->GetCurrentBackBufferIndex();
}

void Dx12Swapchain::Present() noexcept {
    UINT sync_interval = m_bVsync ? 1 : 0;
    UINT flags = m_bVsync ? 0 : DXGI_PRESENT_ALLOW_TEARING;
    m_Swapchain->Present(sync_interval, flags);
}

void Dx12Swapchain::Resize(u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) return;
    if (width == m_Width && height == m_Height) return;
    if (!m_Device || !m_Swapchain) return;

    // 進行中の GPU 作業が終わるまで待ってから解放しないと「使用中」エラーになる
    m_Device->WaitIdle();
    ReleaseBuffers();

    HRESULT hr = m_Swapchain->ResizeBuffers(m_BufferCount, width, height,
                                            DXGI_FORMAT_UNKNOWN,
                                            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    if (FAILED(hr)) return;

    m_Width = width;
    m_Height = height;
    AcquireBuffers(*m_Device);
}

// ファクトリ関数: CreateRhiSwapchain の DX12 実装
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiSwapchain>> CreateRhiSwapchain(IRhiDevice& device,
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
    TUniquePtr<IRhiSwapchain> base(sc.Release(), sc.GetAllocator());
    return TResult<TUniquePtr<IRhiSwapchain>>(OkInit, Move(base));
}
#endif

} // namespace acs
