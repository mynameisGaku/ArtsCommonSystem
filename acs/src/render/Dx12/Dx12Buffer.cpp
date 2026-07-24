// SPDX-License-Identifier: Apache-2.0
// DX12 GPU バッファ実装
#include "render/Dx12/Dx12Buffer.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"

#include <cstring>

namespace acs {

/** 永続マップを解除し GPU リソースを解放する。 */
FDx12Buffer::~FDx12Buffer() noexcept {
    Reset();
}

void FDx12Buffer::Reset() noexcept
{
    if (m_Mapped && m_Resource) m_Resource->Unmap(0, nullptr);
    FDx12Device* device = m_Device;
    ID3D12Resource* resource = m_Resource;
    m_Mapped = nullptr;
    m_Device = nullptr;
    m_Resource = nullptr;
    if (device != nullptr) {
        // The resource may already be encoded in the currently open command
        // list. Device retirement keeps it alive through the submission fence
        // without blocking ordinary editor mutation.
        device->RetireResource(resource);
    } else {
        ACS_SAFE_RELEASE(resource);
    }
    m_Size = 0;
    m_SlotStride = 0;
    m_Usage = EBufferUsage::Vertex;
    m_bCpuWritable = false;
    m_bFrameCycled = false;
}

/** desc に従って GPU バッファを確保し、必要なら永続マップして初期データを複製する。 */
FHrResult FDx12Buffer::Init(FDx12Device& device, const FBufferDesc& desc) noexcept {
    FHrResult r{};
    Reset();

    if (!device.D3DDevice() || !device.GraphicsQueue() || desc.size == 0) {
        r.hr = E_INVALIDARG;
        return r;
    }

    // 256 バイト切り上げとフレーム数倍の計算を先に検証し、折り返した
    // 小さいサイズで GPU リソースを作ってしまうことを防ぐ。
    const usize max_size = static_cast<usize>(-1);
    if (desc.cpu_writable && desc.size > max_size - 255u) {
        r.hr = E_INVALIDARG;
        return r;
    }

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
        if (m_SlotStride > max_size / FDx12Device::kFramesInFlight) {
            r.hr = E_INVALIDARG;
            Reset();
            return r;
        }
    }
    const usize total_size = m_bFrameCycled
        ? m_SlotStride * FDx12Device::kFramesInFlight
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
    if (r.IsErr() || !m_Resource) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
        return r;
    }

    // CPU 書込み可なら永続マップしてポインタを保持
    if (desc.cpu_writable) {
        D3D12_RANGE read_range{ 0, 0 };  // 読まない
        r.hr = m_Resource->Map(0, &read_range, &m_Mapped);
        if (r.IsErr() || !m_Mapped) {
            if (r.IsOk()) r.hr = E_FAIL;
            Reset();
            return r;
        }
    }

    // 初期データを全スロットに複製（フレームリング時は両スロットへ）
    if (desc.initial_data && desc.size > 0 && m_Mapped) {
        if (m_bFrameCycled) {
            for (u32 i = 0; i < FDx12Device::kFramesInFlight; ++i) {
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
        r.hr = device.D3DDevice()->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &urd,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&staging));
        if (r.IsErr() || !staging) {
            if (r.IsOk()) r.hr = E_FAIL;
            ACS_SAFE_RELEASE(staging);
            Reset();
            return r;
        }

        void* staging_data = nullptr;
        D3D12_RANGE read_range{0, 0};
        r.hr = staging->Map(0, &read_range, &staging_data);
        if (r.IsErr() || !staging_data) {
            if (r.IsOk()) r.hr = E_FAIL;
            if (staging_data) staging->Unmap(0, nullptr);
            ACS_SAFE_RELEASE(staging);
            Reset();
            return r;
        }
        ::memcpy(staging_data, desc.initial_data, desc.size);
        staging->Unmap(0, nullptr);

        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* command_list = nullptr;
        r.hr = device.D3DDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (r.IsErr()) {
            ACS_SAFE_RELEASE(allocator);
            ACS_SAFE_RELEASE(staging);
            Reset();
            return r;
        }
        r.hr = device.D3DDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                                     IID_PPV_ARGS(&command_list));
        if (r.IsErr()) {
            ACS_SAFE_RELEASE(command_list);
            ACS_SAFE_RELEASE(allocator);
            ACS_SAFE_RELEASE(staging);
            Reset();
            return r;
        }

        // DEFAULT バッファは COMMON で作成済み。コピー時の暗黙昇格と
        // コマンド完了後の減衰を使い、ステージング解放前に fence を待つ。
        command_list->CopyBufferRegion(m_Resource, 0, staging, 0, desc.size);
        r.hr = command_list->Close();
        if (r.IsOk()) {
            const u64 fence_value =
                device.ExecuteOneOffGraphicsCommandList(command_list);
            if (fence_value == 0) {
                r.hr = E_FAIL;
                // Execute may already have succeeded even though Signal did
                // not. Without a completion fence, releasing these transient
                // objects could invalidate in-flight copy commands. Transfer
                // them to the device-loss leak-safe path by dropping only our
                // CPU bookkeeping references.
                command_list = nullptr;
                allocator = nullptr;
                staging = nullptr;
            } else {
                device.WaitForFenceValue(fence_value);
            }
        }

        ACS_SAFE_RELEASE(command_list);
        ACS_SAFE_RELEASE(allocator);
        ACS_SAFE_RELEASE(staging);
        if (r.IsErr()) {
            Reset();
            return r;
        }
    }
    return r;
}

/** 現在フレームスロットの領域へ CPU からデータを書き込む。 */
void FDx12Buffer::Update(const void* data, usize size, usize offset) noexcept {
    if (!m_Mapped || !data) return;
    if (offset > m_Size || size > m_Size - offset) return;
    u8* base = static_cast<u8*>(m_Mapped);
    if (m_bFrameCycled) {
        const u32 slot = m_Device ? m_Device->CurrentFrameSlot() : 0;
        base += slot * m_SlotStride;
    }
    ::memcpy(base + offset, data, size);
}

/** フレームリングのスロットオフセットを加味した現在の GPU 仮想アドレスを返す。 */
D3D12_GPU_VIRTUAL_ADDRESS FDx12Buffer::Gpu() const noexcept {
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
    FDx12Device* dxd = static_cast<FDx12Device*>(&device);
    auto b = MakeUnique<FDx12Buffer>();
    const FHrResult r = b->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 31, "Dx12Buffer::Init failed", static_cast<u32>(r.hr));
    TUniquePtr<IRhiBuffer> base(b.Release(), b.GetAllocator());
    return TResult<TUniquePtr<IRhiBuffer>>(OkInit, Move(base));
}
#endif

} // namespace acs
