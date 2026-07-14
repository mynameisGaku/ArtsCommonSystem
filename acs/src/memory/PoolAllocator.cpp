// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FPoolAllocator 実装
// -----------------------------------------------------------------------------
// 64-bit パック値: 上位 17 bit に ABA タグ、下位 47 bit にポインタ。
// x64 ユーザ空間ポインタが 47 bit であることを利用して、64bit CAS 1 回で
// ポインタとタグを同時に更新する（DCAS 不要）。
// =============================================================================
#include "memory/PoolAllocator.h"
#include "memory/Memory.h"
#include "foundation/Assert.h"

namespace acs {

namespace {
/** ポインタ部のマスク (x64 ユーザ空間ポインタの下位 47bit)。 */
constexpr u64 kPtrMask = (1ull << 47) - 1ull;

/** ABA タグ部のマスク (上位 17bit)。 */
constexpr u64 kTagMask = ~kPtrMask;

/** タグ部のシフト量 (ポインタ部のビット幅)。 */
constexpr u32 kTagShift = 47;

/**
 * ポインタと ABA タグを 1 ワードにパックする。
 *
 * @param Self 未使用 (将来の per-instance 状態参照用に予約)。
 * @param Pointer フリーブロックへのポインタ (下位 47bit のみ使用)。
 * @param Tag 世代タグ (下位 17bit のみ使用)。
 * @return パックした 64bit 値。
 */
ACS_FORCEINLINE u64 Pack(FPoolAllocator* /*Self*/, void* Pointer, u64 Tag) noexcept
{
    return (reinterpret_cast<u64>(Pointer) & kPtrMask) | ((Tag & ((1ull << 17) - 1)) << kTagShift);
}

/**
 * パック値からポインタ部を取り出す。
 *
 * @param Value パック済み 64bit 値。
 * @return 復元したポインタ。
 */
ACS_FORCEINLINE void* UnpackPtr(u64 Value) noexcept
{
    return reinterpret_cast<void*>(Value & kPtrMask);
}

/**
 * パック値から ABA タグ部を取り出す。
 *
 * @param Value パック済み 64bit 値。
 * @return 復元した世代タグ。
 */
ACS_FORCEINLINE u64 UnpackTag(u64 Value) noexcept
{
    return (Value & kTagMask) >> kTagShift;
}
} // namespace

FPoolAllocator::FPoolAllocator(usize RequestedBlockSize, usize RequestedBlockCount, usize Alignment,
                               FAllocator* BackingAllocator) noexcept
    : m_BlockSize(static_cast<u64>(RequestedBlockSize)),
      m_BlockCount(static_cast<u64>(RequestedBlockCount)),
      m_Alignment(static_cast<u64>(Alignment)),
      m_Backing(BackingAllocator ? BackingAllocator : &DefaultAllocator())
{
    // ブロックサイズをフリーリストノードが収まる最低サイズに揃える
    if (m_Alignment < sizeof(void*)) m_Alignment = sizeof(void*);
    if (m_BlockSize < sizeof(Node)) m_BlockSize = sizeof(Node);
    m_BlockSize = AlignUp(m_BlockSize, m_Alignment);

    // m_BlockSize * m_BlockCount の乗算ラップを防ぐ（過小確保 → バッファ外アクセス防止）。
    // 失敗時はアロケート失敗と同様に空プール化して構築を打ち切る。
    if (m_BlockCount != 0 && m_BlockSize > (~usize(0)) / m_BlockCount) {
        m_BlockCount = 0;
        return;
    }

    // ストレージ全体を 1 回確保
    const usize Total = static_cast<usize>(m_BlockSize * m_BlockCount);
    m_Storage = static_cast<u8*>(m_Backing->Alloc(Total, m_Alignment, FSourceLoc::Current()));
    if (!m_Storage) {
        m_BlockCount = 0;
        return;
    }

    // 全ブロックを単方向リンクで連結（初期化はシングルスレッド前提）
    Node* PreviousNode = nullptr;
    for (u64 i = 0; i < m_BlockCount; ++i) {
        Node* const CurrentNode = reinterpret_cast<Node*>(m_Storage + i * m_BlockSize);
        CurrentNode->next = PreviousNode;
        PreviousNode = CurrentNode;
    }
    const u64 PackedHead = Pack(this, PreviousNode, 0);
    m_HeadPacked.Store(PackedHead, EMemoryOrder::Release);
}

FPoolAllocator::~FPoolAllocator() noexcept
{
    if (m_Storage) m_Backing->Free(m_Storage);
}

// 確保（Treiber スタックの pop）
void* FPoolAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    if (Size == 0) return nullptr;
    if (Size > m_BlockSize) return nullptr;
    if (Alignment > m_Alignment) return nullptr;

    while (true) {
        const u64 Head = m_HeadPacked.Load(EMemoryOrder::Acquire);
        Node* const TopNode = static_cast<Node*>(UnpackPtr(Head));
        if (!TopNode) return nullptr; // プール枯渇
        const u64 Tag = UnpackTag(Head);
        // TopNode->next を読む（ABA タグで CAS が確実に失敗するため安全）
        Node* const NextNode = TopNode->next;
        const u64 DesiredHead = Pack(this, NextNode, Tag + 1); // タグ +1 で世代を進める
        u64 ExpectedHead = Head;
        if (m_HeadPacked.CompareExchange(ExpectedHead, DesiredHead)) {
            m_Live.FetchAdd(1);
            return TopNode;
        }
        // CAS 失敗ならリトライ
    }
}

// 解放（Treiber スタックの push）
void FPoolAllocator::Free(void* Pointer) noexcept
{
    if (!Pointer) return;
    Node* const FreedNode = static_cast<Node*>(Pointer);
    while (true) {
        const u64 Head = m_HeadPacked.Load(EMemoryOrder::Acquire);
        Node* const TopNode = static_cast<Node*>(UnpackPtr(Head));
        const u64 Tag = UnpackTag(Head);
        FreedNode->next = TopNode;
        const u64 DesiredHead = Pack(this, FreedNode, Tag + 1);
        u64 ExpectedHead = Head;
        if (m_HeadPacked.CompareExchange(ExpectedHead, DesiredHead)) {
            m_Live.FetchSub(1);
            return;
        }
    }
}

} // namespace acs
