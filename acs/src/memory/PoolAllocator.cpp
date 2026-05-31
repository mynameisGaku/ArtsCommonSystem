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
// ビット配置の定数
constexpr u64 kPtrMask = (1ull << 47) - 1ull;
constexpr u64 kTagMask = ~kPtrMask;
constexpr u32 kTagShift = 47;

// (ptr, tag) を 1 ワードに詰める
ACS_FORCEINLINE u64 Pack(FPoolAllocator* /*self*/, void* p, u64 tag) noexcept {
    return (reinterpret_cast<u64>(p) & kPtrMask) | ((tag & ((1ull << 17) - 1)) << kTagShift);
}
ACS_FORCEINLINE void* UnpackPtr(u64 v) noexcept {
    return reinterpret_cast<void*>(v & kPtrMask);
}
ACS_FORCEINLINE u64 UnpackTag(u64 v) noexcept {
    return (v & kTagMask) >> kTagShift;
}
} // namespace

FPoolAllocator::FPoolAllocator(usize block_size, usize block_count,
                             usize alignment, FAllocator* backing) noexcept
    : m_BlockSize(static_cast<u64>(block_size))
    , m_BlockCount(static_cast<u64>(block_count))
    , m_Alignment(static_cast<u64>(alignment))
    , m_Backing(backing ? backing : &DefaultAllocator()) {

    // ブロックサイズをフリーリストノードが収まる最低サイズに揃える
    if (m_Alignment < sizeof(void*)) m_Alignment = sizeof(void*);
    if (m_BlockSize < sizeof(Node)) m_BlockSize = sizeof(Node);
    m_BlockSize = AlignUp(m_BlockSize, m_Alignment);

    // m_BlockSize * m_BlockCount の乗算ラップを防ぐ（過小確保 → バッファ外アクセス防止）。
    // 失敗時はアロケート失敗と同様に空プール化して構築を打ち切る。
    if (m_BlockCount != 0 && m_BlockSize > (~usize(0)) / m_BlockCount) { m_BlockCount = 0; return; }

    // ストレージ全体を 1 回確保
    usize total = static_cast<usize>(m_BlockSize * m_BlockCount);
    m_Storage = static_cast<u8*>(m_Backing->Alloc(total, m_Alignment, FSourceLoc::Current()));
    if (!m_Storage) {
        m_BlockCount = 0;
        return;
    }

    // 全ブロックを単方向リンクで連結（初期化はシングルスレッド前提）
    Node* prev = nullptr;
    for (u64 i = 0; i < m_BlockCount; ++i) {
        Node* n = reinterpret_cast<Node*>(m_Storage + i * m_BlockSize);
        n->next = prev;
        prev = n;
    }
    u64 packed = Pack(this, prev, 0);
    m_HeadPacked.Store(packed, EMemoryOrder::Release);
}

FPoolAllocator::~FPoolAllocator() noexcept {
    if (m_Storage) m_Backing->Free(m_Storage);
}

// 確保（Treiber スタックの pop）
void* FPoolAllocator::Alloc(usize size, usize alignment, FSourceLoc /*loc*/) noexcept {
    if (size == 0) return nullptr;
    if (size > m_BlockSize) return nullptr;
    if (alignment > m_Alignment) return nullptr;

    while (true) {
        u64 head = m_HeadPacked.Load(EMemoryOrder::Acquire);
        Node* top = static_cast<Node*>(UnpackPtr(head));
        if (!top) return nullptr;  // プール枯渇
        u64 tag = UnpackTag(head);
        // top->next を読む（ABA タグで CAS が確実に失敗するため安全）
        Node* next = top->next;
        u64 desired = Pack(this, next, tag + 1);  // タグ +1 で世代を進める
        u64 expected = head;
        if (m_HeadPacked.CompareExchange(expected, desired)) {
            m_Live.FetchAdd(1);
            return top;
        }
        // CAS 失敗ならリトライ
    }
}

// 解放（Treiber スタックの push）
void FPoolAllocator::Free(void* ptr) noexcept {
    if (!ptr) return;
    Node* n = static_cast<Node*>(ptr);
    while (true) {
        u64 head = m_HeadPacked.Load(EMemoryOrder::Acquire);
        Node* top = static_cast<Node*>(UnpackPtr(head));
        u64 tag = UnpackTag(head);
        n->next = top;
        u64 desired = Pack(this, n, tag + 1);
        u64 expected = head;
        if (m_HeadPacked.CompareExchange(expected, desired)) {
            m_Live.FetchSub(1);
            return;
        }
    }
}

} // namespace acs
