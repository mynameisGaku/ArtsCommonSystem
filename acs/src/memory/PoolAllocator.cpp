// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FPoolAllocator 実装
// -----------------------------------------------------------------------------
// フリーリストと各ブロックの所有状態を同じロックで保護する。
// 利用者領域とフリーリストノードを共有するため、pop 中のノードを
// 別スレッドへ公開しないことが安全性の前提となる。
// =============================================================================
#include "memory/PoolAllocator.h"
#include "memory/Memory.h"
#include "foundation/Assert.h"
#include "threading/ScopedLock.h"

namespace acs {

FPoolAllocator::FPoolAllocator(usize RequestedBlockSize, usize RequestedBlockCount, usize Alignment,
                               FAllocator* BackingAllocator) noexcept
    : m_BlockSize(static_cast<u64>(RequestedBlockSize)),
      m_BlockCount(static_cast<u64>(RequestedBlockCount)),
      m_Alignment(static_cast<u64>(Alignment)),
      m_Backing(BackingAllocator ? BackingAllocator : &DefaultAllocator())
{
    // ブロックサイズをフリーリストノードが収まる最低サイズに揃える
    if (m_Alignment < sizeof(void*)) m_Alignment = sizeof(void*);
    if ((m_Alignment & (m_Alignment - 1u)) != 0u) {
        m_BlockCount = 0;
        return;
    }
    if (m_BlockSize < sizeof(FNode)) m_BlockSize = sizeof(FNode);
    if (m_BlockSize > (~usize(0)) - (m_Alignment - 1u)) {
        m_BlockCount = 0;
        return;
    }
    m_BlockSize = AlignUp(m_BlockSize, m_Alignment);

    // m_BlockSize * m_BlockCount の乗算ラップを防ぐ（過小確保 → バッファ外アクセス防止）。
    // 失敗時はアロケート失敗と同様に空プール化して構築を打ち切る。
    if (m_BlockCount != 0 && m_BlockSize > (~usize(0)) / m_BlockCount) {
        m_BlockCount = 0;
        return;
    }

    if (m_BlockCount == 0u) return;

    // ストレージ全体を 1 回確保
    const usize Total = static_cast<usize>(m_BlockSize * m_BlockCount);
    m_Storage = static_cast<u8*>(m_Backing->Alloc(Total, m_Alignment, FSourceLoc::Current()));
    if (!m_Storage) {
        m_BlockCount = 0;
        return;
    }

    m_AllocationStates = static_cast<u8*>(
        m_Backing->Alloc(static_cast<usize>(m_BlockCount), alignof(u8), FSourceLoc::Current()));
    if (!m_AllocationStates) {
        m_Backing->Free(m_Storage);
        m_Storage = nullptr;
        m_BlockCount = 0;
        return;
    }
    MemSet(m_AllocationStates, 0, static_cast<usize>(m_BlockCount));

    // 全ブロックを単方向リンクで連結（初期化はシングルスレッド前提）
    FNode* PreviousNode = nullptr;
    for (u64 i = 0; i < m_BlockCount; ++i) {
        FNode* const CurrentNode = reinterpret_cast<FNode*>(m_Storage + i * m_BlockSize);
        CurrentNode->next = PreviousNode;
        PreviousNode = CurrentNode;
    }
    m_FreeHead = PreviousNode;
}

FPoolAllocator::~FPoolAllocator() noexcept
{
    if (m_AllocationStates) m_Backing->Free(m_AllocationStates);
    if (m_Storage) m_Backing->Free(m_Storage);
}

// 確保
void* FPoolAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    if (Size == 0) return nullptr;
    if (Size > m_BlockSize) return nullptr;
    if (Alignment == 0u || (Alignment & (Alignment - 1u)) != 0u || Alignment > m_Alignment) return nullptr;

    FScopedLock Lock(m_Lock);
    FNode* const AllocatedNode = m_FreeHead;
    if (!AllocatedNode) return nullptr;

    const usize BlockIndex = static_cast<usize>(
        (reinterpret_cast<uptr>(AllocatedNode) - reinterpret_cast<uptr>(m_Storage)) / m_BlockSize);
    if (BlockIndex >= m_BlockCount || m_AllocationStates[BlockIndex] != 0u) return nullptr;

    m_FreeHead = AllocatedNode->next;
    m_AllocationStates[BlockIndex] = 1u;
    m_Live.FetchAdd(1u);
    return AllocatedNode;
}

// 解放
void FPoolAllocator::Free(void* Pointer) noexcept
{
    if (!Pointer) return;
    if (!m_Storage || !m_AllocationStates) return;

    const uptr StorageAddress = reinterpret_cast<uptr>(m_Storage);
    const uptr PointerAddress = reinterpret_cast<uptr>(Pointer);
    const usize StorageSize = static_cast<usize>(m_BlockSize * m_BlockCount);
    if (PointerAddress < StorageAddress || PointerAddress - StorageAddress >= StorageSize) return;

    const usize Offset = static_cast<usize>(PointerAddress - StorageAddress);
    if ((Offset % m_BlockSize) != 0u) return;
    const usize BlockIndex = Offset / m_BlockSize;

    FScopedLock Lock(m_Lock);
    if (m_AllocationStates[BlockIndex] == 0u) return;

    m_AllocationStates[BlockIndex] = 0u;
    auto* const FreedNode = static_cast<FNode*>(Pointer);
    FreedNode->next = m_FreeHead;
    m_FreeHead = FreedNode;
    m_Live.FetchSub(1u);
}

} // namespace acs
