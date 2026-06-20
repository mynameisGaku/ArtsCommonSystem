// SPDX-License-Identifier: Apache-2.0
// DX12 GPU バッファ実装
#include "render/Dx12/Dx12Buffer.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"

#include <cstring>

namespace acs {

/** 永続マップを解除し GPU リソースを解放する。 */
Dx12Buffer::~Dx12Buffer() noexcept {
    if (m_Mapped && m_Resource) m_Resource->Unmap(0, nullptr);
    ACS_SAFE_RELEASE(m_Resource);
}

/** desc に従って GPU バッファを確保し、必要なら永続マップして初期データを複製する。 */
HrResult Dx12Buffer::Init(Dx12Device& device, const FBufferDesc& desc) noexcept {
    HrResult r{};
    m_Device = &device;
    m_Size  = desc.size;
    m_Usage = desc.usage;
    m_bCpuWritable = desc.cpu_writable;

    // すべての cpu_writable バッファは自動でフレームリング化する。
    // GPU/CPU の並列実行 (kFramesInFlight=2) 中に CPU 側 Update が
    // 実行中の GPU 読み出しと衝突するのを防ぐため。Static (cpu_writable=false)
    // のバッファは 1 度しか書かれないのでリング不要。
    m_bFrameCycled = desc.cpu_writable;

    // 1 スロットあたりのストライド（最大の Uniform 要件 256B にアライン）
    if (m_bFrameCycled) {
        m_SlotStride = (desc.size + 255u) & ~static_cast<usize>(255u);
    }
    const usize total_size = m_bFrameCycled
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
        if (m_bFrameCycled) {
            for (u32 i = 0; i < Dx12Device::kFramesInFlight; ++i) {
                ::memcpy(static_cast<u8*>(m_Mapped) + i * m_SlotStride,
                         desc.initial_data, desc.size);
            }
        } else {
            ::memcpy(m_Mapped, desc.initial_data, desc.size);
        }
    }

    // 静的 (DEFAULT ヒープ、cpu_writable=false) + initial_data: DEFAULT ヒープは map できないため、
    // ステージング UPLOAD バッファへコピー → CopyBufferRegion で DEFAULT へ転送 → GPU 完了待ち。
    // これが無いと静的バッファは «空» のままで描画されない (旧バグ。static mesh が全て不可視だった)。
    if (!desc.cpu_writable && desc.initial_data && desc.size > 0 && m_Resource) {
        ID3D12Resource* staging = nullptr;
        D3D12_HEAP_PROPERTIES uh{}; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        uh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN; uh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        D3D12_RESOURCE_DESC urd = rd; urd.Width = static_cast<UINT64>(desc.size);
        if (SUCCEEDED(device.D3DDevice()->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &urd,
                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))) && staging) {
            void* sp = nullptr; D3D12_RANGE rr{ 0, 0 };
            if (SUCCEEDED(staging->Map(0, &rr, &sp)) && sp) {
                ::memcpy(sp, desc.initial_data, desc.size);
                staging->Unmap(0, nullptr);
            }
            ID3D12CommandAllocator*    ca = nullptr;
            ID3D12GraphicsCommandList* uc = nullptr;
            if (SUCCEEDED(device.D3DDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) &&
                SUCCEEDED(device.D3DDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&uc)))) {
                // DEFAULT バッファは COMMON で作成済 → CopyBufferRegion で暗黙に COPY_DEST へ昇格、実行後 COMMON へ減衰。
                uc->CopyBufferRegion(m_Resource, 0, staging, 0, desc.size);
                uc->Close();
                ID3D12CommandList* lists[] = { uc };
                device.GraphicsQueue()->ExecuteCommandLists(1, lists);
                device.WaitIdle();   // コピー完了を待ってから staging を解放
            }
            ACS_SAFE_RELEASE(uc); ACS_SAFE_RELEASE(ca); ACS_SAFE_RELEASE(staging);
        }
    }
    return r;
}

/** 現在フレームスロットの領域へ CPU からデータを書き込む。 */
void Dx12Buffer::Update(const void* data, usize size, usize offset) noexcept {
    if (!m_Mapped || !data) return;
    if (offset + size > m_Size) return;
    u8* base = static_cast<u8*>(m_Mapped);
    if (m_bFrameCycled) {
        const u32 slot = m_Device ? m_Device->CurrentFrameSlot() : 0;
        base += slot * m_SlotStride;
    }
    ::memcpy(base + offset, data, size);
}

/** フレームリングのスロットオフセットを加味した現在の GPU 仮想アドレスを返す。 */
D3D12_GPU_VIRTUAL_ADDRESS Dx12Buffer::Gpu() const noexcept {
    if (!m_Resource) return 0;
    D3D12_GPU_VIRTUAL_ADDRESS addr = m_Resource->GetGPUVirtualAddress();
    if (m_bFrameCycled && m_Device) {
        addr += m_Device->CurrentFrameSlot() * m_SlotStride;
    }
    return addr;
}

#if !WITH_RENDER_DILIGENT
/**
 * DX12 用に IRhiBuffer を生成するファクトリ。
 *
 * @details
 * RTTI 無効のためバックエンド名で DX12 を判定し、Dx12Buffer を構築・初期化して返す。
 * Diligent バックエンド有効時は別実装が提供される。
 * @param device 生成元のデバイス (DX12 でなければエラー)。
 * @param desc 構築するバッファの記述。
 * @return 生成したバッファを保持する TResult、判定・初期化失敗ならエラー。
 */
TResult<TUniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const FBufferDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 30, "CreateRhiBuffer: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto b = MakeUnique<Dx12Buffer>();
    const HrResult r = b->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 31, "Dx12Buffer::Init failed", static_cast<u32>(r.hr));
    TUniquePtr<IRhiBuffer> base(b.Release(), b.GetAllocator());
    return TResult<TUniquePtr<IRhiBuffer>>(OkInit, Move(base));
}
#endif

} // namespace acs
