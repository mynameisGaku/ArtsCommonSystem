// SPDX-License-Identifier: Apache-2.0
// DX12 スワップチェイン実装
#include "render/Dx12/Dx12Swapchain.h"
#include "render/Dx12/Dx12Device.h"
#include "platform/Window.h"
#include "memory/UniquePtr.h"

namespace acs {

Dx12Swapchain::~Dx12Swapchain() noexcept {
    // Device が子リソースより長生きする RHI 契約の下で、バックバッファを
    // GPU が参照し終えるまで待ってから COM 所有物を破棄する。
    if (m_Device) m_Device->WaitIdle();
    Reset();
}

void Dx12Swapchain::Reset() noexcept
{
    ReleaseBuffers();
    ACS_SAFE_RELEASE(m_RtvHeap);
    ACS_SAFE_RELEASE(m_Swapchain);
    m_Device = nullptr;
    m_RtvSize = 0;
    m_BufferCount = 0;
    m_Width = 0;
    m_Height = 0;
    m_bVsync = true;
    m_bAllowTearing = false;
}

bool Dx12Swapchain::HasAllBuffers() const noexcept
{
    if (m_BufferCount == 0) return false;
    for (u32 i = 0; i < m_BufferCount; ++i) {
        if (!m_BackBuffers[i]) return false;
    }
    return true;
}

HrResult Dx12Swapchain::Init(Dx12Device& device, const SwapchainConfig& cfg) noexcept {
    HrResult r{};
    if (m_Device) m_Device->WaitIdle();
    Reset();

    if (!device.DxgiFactory() || !device.D3DDevice() || !device.GraphicsQueue() ||
        ToDxgiFormat(cfg.format) == DXGI_FORMAT_UNKNOWN) {
        r.hr = E_INVALIDARG;
        return r;
    }
    m_Device = &device;
    m_BufferCount = (cfg.buffer_count >= 2 && cfg.buffer_count <= kMaxBuffers) ? cfg.buffer_count : 2;
    m_bVsync = cfg.vsync;
    // window 優先、無ければ external_hwnd（エディタ等が用意した生 HWND）を使う。
    m_Width  = cfg.window ? cfg.window->Width()  : cfg.external_width;
    m_Height = cfg.window ? cfg.window->Height() : cfg.external_height;

    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(device.DxgiFactory()->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                                            sizeof(allow_tearing)))) {
        m_bAllowTearing = allow_tearing == TRUE;
    }

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
    sd.Flags = m_bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    IDXGISwapChain1* sc1 = nullptr;
    const HWND hwnd = cfg.window ? static_cast<HWND>(cfg.window->NativeHandle())
                                 : static_cast<HWND>(cfg.external_hwnd);
    if (!hwnd) {
        r.hr = E_INVALIDARG;
        Reset();
        return r;
    }

    r.hr = device.DxgiFactory()->CreateSwapChainForHwnd(
        device.GraphicsQueue(), hwnd, &sd, nullptr, nullptr, &sc1);
    if (r.IsErr() || !sc1) {
        if (r.IsOk()) r.hr = E_FAIL;
        ACS_SAFE_RELEASE(sc1);
        Reset();
        return r;
    }

    // SwapChain1 → SwapChain3 へキャスト（GetCurrentBackBufferIndex を使うため）
    r.hr = sc1->QueryInterface(IID_PPV_ARGS(&m_Swapchain));
    sc1->Release();
    if (r.IsErr() || !m_Swapchain) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
        return r;
    }

    // Alt-Enter での切り替えを抑止
    device.DxgiFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // RTV 用デスクリプタヒープ作成
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = m_BufferCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    r.hr = device.D3DDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_RtvHeap));
    if (r.IsErr() || !m_RtvHeap) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
        return r;
    }
    m_RtvSize = device.D3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    r = AcquireBuffers(device);
    if (r.IsErr()) Reset();
    return r;
}

void Dx12Swapchain::ReleaseBuffers() noexcept {
    for (u32 i = 0; i < kMaxBuffers; ++i)
        ACS_SAFE_RELEASE(m_BackBuffers[i]);
}

// 各バックバッファ用に ID3D12Resource を取得し、RTV を作成する
HrResult Dx12Swapchain::AcquireBuffers(Dx12Device& device) noexcept {
    HrResult r{};
    if (!m_Swapchain || !m_RtvHeap || !device.D3DDevice() || m_BufferCount == 0) {
        r.hr = E_FAIL;
        return r;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < m_BufferCount; ++i) {
        // GetBuffer は失敗時に out ポインタを書き換えない場合があるため明示初期化
        m_BackBuffers[i] = nullptr;
        r.hr = m_Swapchain->GetBuffer(i, IID_PPV_ARGS(&m_BackBuffers[i]));
        if (r.IsErr()) {
            // 途中失敗時は取得済みバッファを解放し全 null に戻す。
            // 半端な状態を残すと BackBuffer() が null を返し描画側で参照外れになる。
            ACS_SAFE_RELEASE(m_BackBuffers[i]);  // GetBuffer が部分書き込みした可能性に備える
            ReleaseBuffers();
            return r;
        }
        device.D3DDevice()->CreateRenderTargetView(m_BackBuffers[i], nullptr, rtv);
        rtv.ptr += m_RtvSize;
    }
    return r;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Swapchain::BackBufferRTV(u32 i) const noexcept {
    if (!m_RtvHeap || i >= m_BufferCount || !m_BackBuffers[i]) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_RtvSize) * i;
    return rtv;
}

u32 Dx12Swapchain::AcquireNextImage() noexcept {
    return m_Swapchain ? m_Swapchain->GetCurrentBackBufferIndex() : 0;
}

void Dx12Swapchain::Present() noexcept {
    if (!m_Swapchain || !HasAllBuffers()) return;
    const UINT sync_interval = m_bVsync ? 1 : 0;
    const UINT flags = (!m_bVsync && m_bAllowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    (void)m_Swapchain->Present(sync_interval, flags);
}

bool Dx12Swapchain::Resize(u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) return true;          // 無効サイズ要求は no-op (成功扱い)
    if (!m_Device || !m_Swapchain) return false;         // リサイズ不能
    if (width == m_Width && height == m_Height) {
        if (HasAllBuffers()) return true;
        return AcquireBuffers(*m_Device).IsOk();
    }

    // 進行中の GPU 作業が終わるまで待ってから解放しないと「使用中」エラーになる
    m_Device->WaitIdle();
    ReleaseBuffers();

    const HRESULT hr = m_Swapchain->ResizeBuffers(m_BufferCount, width, height, DXGI_FORMAT_UNKNOWN,
                                                  m_bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        // ResizeBuffers が失敗した場合、旧スワップチェイン自体は有効なので
        // 旧バックバッファを再取得し、失敗前より悪い空状態を残さない。
        (void)AcquireBuffers(*m_Device);
        return false;
    }

    // バッファ再取得が成功した場合にのみ寸法を確定させる。
    // 失敗時に幅/高さを更新すると BackBuffer() が null を返し描画側で参照外れになる。
    HrResult ar = AcquireBuffers(*m_Device);
    if (ar.IsErr()) {
        // ResizeBuffers 自体は成功しているため実体の寸法は新値である。
        // 公開寸法も合わせ、次の同寸法 Resize で再取得を試せるようにする。
        m_Width = width;
        m_Height = height;
        return false;
    }

    m_Width = width;
    m_Height = height;
    return true;
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
    const HrResult r = sc->Init(*dxd, cfg);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 11, "Dx12Swapchain::Init failed", static_cast<u32>(r.hr));
    }
    TUniquePtr<IRhiSwapchain> base(sc.Release(), sc.GetAllocator());
    return TResult<TUniquePtr<IRhiSwapchain>>(OkInit, Move(base));
}
#endif

} // namespace acs
