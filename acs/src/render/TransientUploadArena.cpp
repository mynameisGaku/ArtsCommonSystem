// SPDX-License-Identifier: Apache-2.0
#include "render/TransientUploadArena.h"

#include "render/IRhiDevice.h"

namespace acs {

/** 親バッファと公開範囲を設定する。 */
void CTransientUploadArena::FSlice::Configure(IRhiBuffer& buffer, usize offset, usize size) noexcept
{
    m_Buffer = &buffer;
    m_Offset = offset;
    m_Size = size;
}

/** 公開範囲内へデータを書き込む。 */
void CTransientUploadArena::FSlice::Update(const void* data, usize size, usize offset) noexcept
{
    if (m_Buffer == nullptr || data == nullptr || offset > m_Size || size > m_Size - offset) return;
    m_Buffer->Update(data, size, m_Offset + offset);
}

/** backendがbindする実バッファを返す。 */
IRhiBuffer& CTransientUploadArena::FSlice::BindingBuffer() noexcept
{
    return m_Buffer->BindingBuffer();
}

/** 実バッファ先頭からのbind offsetを返す。 */
usize CTransientUploadArena::FSlice::BindingOffset() const noexcept
{
    return m_Buffer->BindingOffset() + m_Offset;
}

/** 一時定数の大きさと初期個数を設定して最初の共有ページを作る。 */
TResult<void> CTransientUploadArena::Init(IRhiDevice& device, usize allocation_size, u32 initial_capacity) noexcept
{
    Reset();
    /** アライン計算に使う最大値。 */
    constexpr usize kMaxSize = static_cast<usize>(-1);
    if (allocation_size == 0u || initial_capacity == 0u || allocation_size > kMaxSize - (AllocationAlignment() - 1u)) {
        return ACS_ERR(Render, 790, "Transient upload arena configuration is invalid");
    }
    m_Device = &device;
    m_AllocationSize = allocation_size;
    m_Stride = (allocation_size + AllocationAlignment() - 1u) & ~(AllocationAlignment() - 1u);
    if (!AddPage(initial_capacity)) {
        Reset();
        return ACS_ERR(Render, 791, "Transient upload arena initial page allocation failed");
    }
    return Ok();
}

/** 全ページを解放して未初期化状態へ戻す。 */
void CTransientUploadArena::Reset() noexcept
{
    m_Pages.Empty();
    m_Device = nullptr;
    m_AllocationSize = 0u;
    m_Stride = 0u;
    m_Capacity = 0u;
    m_Cursor = 0u;
    m_ActivePage = 0u;
    m_ReservedBytes = 0u;
}

/** 次フレームの必要数を予約し、使用位置だけを定数時間で先頭へ戻す。 */
bool CTransientUploadArena::BeginFrame(u32 required_allocations) noexcept
{
    /** 必要容量の確保結果。 */
    const bool capacity_ready = Reserve(required_allocations);
    m_Cursor = 0u;
    m_ActivePage = 0u;
    return capacity_ready;
}

/** 必要数まで共有ページを追加する。 */
bool CTransientUploadArena::Reserve(u32 required_allocations) noexcept
{
    if (required_allocations == ~u32{0}) return false;
    if (m_Device == nullptr || m_Stride == 0u) return false;
    if (required_allocations <= m_Capacity) return true;
    while (m_Capacity < required_allocations) {
        /** 今回追加する論理slice数。 */
        const u32 page_capacity = NextPageCapacity(required_allocations);
        if (page_capacity == 0u || !AddPage(page_capacity)) return false;
    }
    return true;
}

/** 次の論理sliceへ定数を書き、通常のIRhiBufferとして返す。 */
IRhiBuffer* CTransientUploadArena::Upload(const void* data, usize size) noexcept
{
    if (data == nullptr || size == 0u || size > m_AllocationSize || m_Cursor == ~u32{0}) return nullptr;
    if (m_Cursor >= m_Capacity && !Reserve(m_Cursor + 1u)) return nullptr;
    /** 今回使う論理slice番号。 */
    const u32 allocation_index = m_Cursor;
    /** 今回使う論理slice。 */
    IRhiBuffer* const allocation = Get(allocation_index);
    if (allocation == nullptr) return nullptr;
    allocation->Update(data, size);
    ++m_Cursor;
    return allocation;
}

/** 指定した論理sliceを返す。 */
IRhiBuffer* CTransientUploadArena::Get(u32 allocation_index) noexcept
{
    if (allocation_index >= m_Capacity || m_Pages.IsEmpty()) return nullptr;
    /** 検索を始めるページ番号。 */
    u32 page_index = allocation_index >= m_Pages[m_ActivePage].first_allocation ? m_ActivePage : 0u;
    while (page_index < m_Pages.Num()) {
        /** 検査中の共有ページ。 */
        FPage& page = m_Pages[page_index];
        /** ページ内の論理slice番号。 */
        const u32 local_index = allocation_index - page.first_allocation;
        if (local_index < page.slices.Num()) {
            m_ActivePage = page_index;
            return &page.slices[local_index];
        }
        ++page_index;
    }
    return nullptr;
}

/** 指定した論理sliceをconst arenaから返す。 */
IRhiBuffer* CTransientUploadArena::Get(u32 allocation_index) const noexcept
{
    if (allocation_index >= m_Capacity) return nullptr;
    for (usize page_index = 0u; page_index < m_Pages.Num(); ++page_index) {
        /** 検査中の共有ページ。 */
        const FPage& page = m_Pages[page_index];
        /** ページ内の論理slice番号。 */
        const u32 local_index = allocation_index - page.first_allocation;
        if (local_index < page.slices.Num()) return const_cast<FSlice*>(&page.slices[local_index]);
    }
    return nullptr;
}

/** 指定個数を持つページを末尾へ追加する。 */
bool CTransientUploadArena::AddPage(u32 allocation_count) noexcept
{
    if (m_Device == nullptr || allocation_count == 0u || m_Stride == 0u) return false;
    /** サイズ計算に使う最大値。 */
    constexpr usize kMaxSize = static_cast<usize>(-1);
    if (static_cast<usize>(allocation_count) > kMaxSize / m_Stride || allocation_count > (~u32{0}) - m_Capacity) return false;
    /** 新ページの総バイト数。 */
    const usize page_bytes = m_Stride * static_cast<usize>(allocation_count);
    if (page_bytes > kMaxSize - m_ReservedBytes) return false;
    /** 新ページのRHI記述。 */
    FBufferDesc description{};
    description.size = page_bytes;
    description.usage = EBufferUsage::Uniform;
    description.cpu_writable = true;
    /** 新ページの実GPUバッファ生成結果。 */
    auto created = CreateRhiBuffer(*m_Device, description);
    if (created.IsErr()) return false;

    /** 公開前に完成させる新ページ。 */
    FPage page{};
    page.buffer = Move(created.Value());
    page.first_allocation = m_Capacity;
    if (!page.slices.TrySetNum(allocation_count)) return false;
    for (u32 index = 0u; index < allocation_count; ++index) {
        page.slices[index].Configure(*page.buffer, m_Stride * static_cast<usize>(index), m_AllocationSize);
    }
    if (!m_Pages.TryAdd(Move(page))) return false;
    m_Capacity += allocation_count;
    m_ReservedBytes += page_bytes;
    return true;
}

/** 要求値以上へ幾何成長させる次ページ個数を返す。 */
u32 CTransientUploadArena::NextPageCapacity(u32 required_allocations) const noexcept
{
    /** まだ不足している論理slice数。 */
    const u32 missing = required_allocations - m_Capacity;
    /** 既存容量の半分を使う幾何成長量。 */
    const u32 geometric = m_Capacity > 1u ? m_Capacity / 2u : 1u;
    return missing > geometric ? missing : geometric;
}

} // namespace acs
