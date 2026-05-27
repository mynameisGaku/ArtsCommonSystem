// SPDX-License-Identifier: Apache-2.0
// DX12 GPU バッファ実装
#include "render/Dx12/Dx12Buffer.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"

#include <cstring>

namespace acs {

Dx12Buffer::~Dx12Buffer() noexcept {
    if (m_Mapped && m_Resource) m_Resource->Unmap(0, nullptr);
    ACS_SAFE_RELEASE(m_Resource);
}

HrResult Dx12Buffer::Init(Dx12Device& device, const FBufferDesc& desc) noexcept {
    HrResult r{};
    m_Device = &device;
    m_Size  = desc.size;
    m_Usage = desc.usage;
    m_CpuWritable = desc.cpu_writable;

    // すべての cpu_writable バッファは自動でフレームリング化する。
    // GPU/CPU の並列実行 (kFramesInFlight=2) 中に CPU 側 Update が
    // 実行中の GPU 読み出しと衝突するのを防ぐため。Static (cpu_writable=false)
    // のバッファは 1 度しか書かれないのでリング不要。
    m_FrameCycled = desc.cpu_writable;

    // 1 スロットあたりのストライド（最大の Uniform 要件 256B にアライン）
    if (m_FrameCycled) {
        m_SlotStride = (desc.size + 255u) & ~static_cast<usize>(255u);
    }
    const usize total_size = m_FrameCycled
        ? m_SlotStride * Dx12Device::kFramesInFlight
        : desc.size;

    // バッファリソース記述（行レイアウトのリニアバッファ）
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0;
    rd.Width  = static_cast<UINT64>(total_size);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    // CPU 書込み可なら UPLOAD ヒープ、それ以外は DEFAULT ヒープ
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_RESOURCE_STATES init_state;
    if (desc.cpu_writable) {
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        init_state = D3D12_RESOURCE_STATE_GENERIC_READ;  // UPLOAD は GENERIC_READ 固定
    } else {
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        init_state = D3D12_RESOURCE_STATE_COMMON;
    }
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    r.hr = device.D3DDevice()->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
        IID_PPV_ARGS(&m_Resource));
    if (r.IsErr()) return r;

    // CPU 書込み可なら永続マップしてポインタを保持
    if (desc.cpu_writable) {
        D3D12_RANGE read_range{ 0, 0 };  // 読まない
        r.hr = m_Resource->Map(0, &read_range, &m_Mapped);
        if (r.IsErr()) return r;
    }

    // 初期データを全スロットに複製（フレームリング時は両スロットへ）
    if (desc.initial_data && desc.size > 0 && m_Mapped) {
        if (m_FrameCycled) {
            for (u32 i = 0; i < Dx12Device::kFramesInFlight; ++i) {
                ::memcpy(static_cast<u8*>(m_Mapped) + i * m_SlotStride,
                         desc.initial_data, desc.size);
            }
        } else {
            ::memcpy(m_Mapped, desc.initial_data, desc.size);
        }
    }
    return r;
}

void Dx12Buffer::Update(const void* data, usize size, usize offset) noexcept {
    if (!m_Mapped || !data) return;
    if (offset + size > m_Size) return;
    u8* base = static_cast<u8*>(m_Mapped);
    if (m_FrameCycled) {
        const u32 slot = m_Device ? m_Device->CurrentFrameSlot() : 0;
        base += slot * m_SlotStride;
    }
    ::memcpy(base + offset, data, size);
}

D3D12_GPU_VIRTUAL_ADDRESS Dx12Buffer::Gpu() const noexcept {
    if (!m_Resource) return 0;
    D3D12_GPU_VIRTUAL_ADDRESS addr = m_Resource->GetGPUVirtualAddress();
    if (m_FrameCycled && m_Device) {
        addr += m_Device->CurrentFrameSlot() * m_SlotStride;
    }
    return addr;
}

// ファクトリ
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const FBufferDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 30, "CreateRhiBuffer: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto b = MakeUnique<Dx12Buffer>();
    HrResult r = b->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 31, "Dx12Buffer::Init failed", static_cast<u32>(r.hr));
    TUniquePtr<IRhiBuffer> base(b.Release(), b.GetAllocator());
    return TResult<TUniquePtr<IRhiBuffer>>(OkInit, Move(base));
}
#endif

} // namespace acs
