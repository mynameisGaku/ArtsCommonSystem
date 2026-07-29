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
    return AllocBlock();
}

void* FPoolAllocator::AllocBlock() noexcept
{
    if (m_Storage == nullptr || m_AllocationStates == nullptr) {
        return nullptr;
    }

    // フリーリストを保護するロック。
    FScopedLock Lock(m_Lock);
    ++m_LockAcquisitions;
    // フリーリスト先頭から取り出すノード。
    FNode* const AllocatedNode = m_FreeHead;
    if (AllocatedNode == nullptr) return nullptr;
    // 取り出すノードに対応する状態配列の位置。
    const usize BlockIndex = static_cast<usize>((reinterpret_cast<uptr>(AllocatedNode) - reinterpret_cast<uptr>(m_Storage)) / m_BlockSize);
    if (BlockIndex >= m_BlockCount || m_AllocationStates[BlockIndex] != 0u) {
        return nullptr;
    }

    m_FreeHead = AllocatedNode->next;
    m_AllocationStates[BlockIndex] = 1u;
    m_Live.FetchAdd(1u);
    return AllocatedNode;
}

usize FPoolAllocator::AllocBatch(void** Output, usize Count) noexcept
{
    if (Output == nullptr || Count == 0u) return 0u;
    // 未取得分を確実に nullptr として返す。
    for (usize Index = 0; Index < Count; ++Index) Output[Index] = nullptr;
    if (m_Storage == nullptr || m_AllocationStates == nullptr) return 0u;

    // バッチ全体で共有するフリーリストロック。
    FScopedLock Lock(m_Lock);
    ++m_LockAcquisitions;
    // 現在までに確保できた件数。
    usize AllocatedCount = 0u;
    while (AllocatedCount < Count && m_FreeHead != nullptr) {
        // 今回取り出すフリーリスト先頭。
        FNode* const AllocatedNode = m_FreeHead;
        // 取り出すノードに対応する状態配列の位置。
        const usize BlockIndex = static_cast<usize>((reinterpret_cast<uptr>(AllocatedNode) - reinterpret_cast<uptr>(m_Storage)) / m_BlockSize);
        // 内部状態の破損を検出した場合は、それ以上リストを変更しない。
        if (BlockIndex >= m_BlockCount || m_AllocationStates[BlockIndex] != 0u) {
            break;
        }

        m_FreeHead = AllocatedNode->next;
        m_AllocationStates[BlockIndex] = 1u;
        Output[AllocatedCount++] = AllocatedNode;
    }
    if (AllocatedCount != 0u) {
        m_Live.FetchAdd(static_cast<u64>(AllocatedCount));
    }
    return AllocatedCount;
}

// 解放
void FPoolAllocator::Free(void* Pointer) noexcept
{
    if (!Contains(Pointer) || m_AllocationStates == nullptr) return;
    // Pointer に対応する状態配列の位置。
    const usize BlockIndex = static_cast<usize>((reinterpret_cast<uptr>(Pointer) - reinterpret_cast<uptr>(m_Storage)) / m_BlockSize);

    // 所有状態とフリーリストを一体で更新するロック。
    FScopedLock Lock(m_Lock);
    ++m_LockAcquisitions;
    if (m_AllocationStates[BlockIndex] != 1u) return;
    m_AllocationStates[BlockIndex] = 0u;
    // 利用者領域を再びフリーリストノードとして使う。
    auto* const FreedNode = static_cast<FNode*>(Pointer);
    FreedNode->next = m_FreeHead;
    m_FreeHead = FreedNode;
    m_Live.FetchSub(1u);
}

usize FPoolAllocator::FreeBatch(void* const* Pointers, usize Count) noexcept
{
    if (Pointers == nullptr || Count == 0u || m_Storage == nullptr || m_AllocationStates == nullptr) {
        return 0u;
    }

    // バッチ全体で共有するフリーリストロック。
    FScopedLock Lock(m_Lock);
    ++m_LockAcquisitions;
    // プールストレージの先頭アドレス。
    const uptr StorageAddress = reinterpret_cast<uptr>(m_Storage);
    // プールストレージの総 byte 数。
    const usize StorageSize = static_cast<usize>(m_BlockSize * m_BlockCount);
    // 現在までに返却できた件数。
    usize FreedCount = 0u;
    // 入力ごとに所属と状態を検証してから返却する。
    for (usize Index = 0; Index < Count; ++Index) {
        // 現在検証する入力ポインタ。
        void* const Pointer = Pointers[Index];
        if (Pointer == nullptr) continue;

        // 現在ポインタの整数表現。
        const uptr PointerAddress = reinterpret_cast<uptr>(Pointer);
        if (PointerAddress < StorageAddress || PointerAddress - StorageAddress >= StorageSize) {
            continue;
        }
        // ストレージ先頭からの byte 差分。
        const usize Offset = static_cast<usize>(PointerAddress - StorageAddress);
        if ((Offset % m_BlockSize) != 0u) continue;
        // Pointer に対応する状態配列の位置。
        const usize BlockIndex = Offset / m_BlockSize;
        if (m_AllocationStates[BlockIndex] != 1u) continue;

        m_AllocationStates[BlockIndex] = 0u;
        // 利用者領域を再びフリーリストノードとして使う。
        auto* const FreedNode = static_cast<FNode*>(Pointer);
        FreedNode->next = m_FreeHead;
        m_FreeHead = FreedNode;
        ++FreedCount;
    }
    if (FreedCount != 0u) {
        m_Live.FetchSub(static_cast<u64>(FreedCount));
    }
    return FreedCount;
}

bool FPoolAllocator::TryBeginDestroyBlock(void* Pointer) noexcept
{
    if (!Contains(Pointer) || m_AllocationStates == nullptr) return false;
    // Pointer に対応する状態配列の位置。
    const usize BlockIndex = static_cast<usize>((reinterpret_cast<uptr>(Pointer) - reinterpret_cast<uptr>(m_Storage)) / m_BlockSize);

    // 払い出し状態を排他的に破棄中へ遷移する。
    FScopedLock Lock(m_Lock);
    ++m_LockAcquisitions;
    if (m_AllocationStates[BlockIndex] != 1u) return false;
    m_AllocationStates[BlockIndex] = 2u;
    return true;
}

void FPoolAllocator::FinishDestroyBlock(void* Pointer) noexcept
{
    ACS_ASSERT(Contains(Pointer));
    // Pointer に対応する状態配列の位置。
    const usize BlockIndex = static_cast<usize>((reinterpret_cast<uptr>(Pointer) - reinterpret_cast<uptr>(m_Storage)) / m_BlockSize);

    // 破棄中状態とフリーリストを一体で更新するロック。
    FScopedLock Lock(m_Lock);
    ++m_LockAcquisitions;
    ACS_ASSERT(m_AllocationStates[BlockIndex] == 2u);
    m_AllocationStates[BlockIndex] = 0u;
    // デストラクタ完了後の領域をフリーリストノードへ戻す。
    auto* const FreedNode = static_cast<FNode*>(Pointer);
    FreedNode->next = m_FreeHead;
    m_FreeHead = FreedNode;
    m_Live.FetchSub(1u);
}

u64 FPoolAllocator::LockAcquisitionCount() const noexcept
{
    // 計測値をフリーリスト更新と同じロックで読み取る。
    FScopedLock Lock(m_Lock);
    return m_LockAcquisitions;
}

} // namespace acs
