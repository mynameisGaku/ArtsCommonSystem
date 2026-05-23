// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — PoolAllocator 実装
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
ACS_FORCEINLINE u64 Pack(PoolAllocator* /*self*/, void* p, u64 tag) noexcept {
    return (reinterpret_cast<u64>(p) & kPtrMask) | ((tag & ((1ull << 17) - 1)) << kTagShift);
}
ACS_FORCEINLINE void* UnpackPtr(u64 v) noexcept {
    return reinterpret_cast<void*>(v & kPtrMask);
}
ACS_FORCEINLINE u64 UnpackTag(u64 v) noexcept {
    return (v & kTagMask) >> kTagShift;
}
} // namespace

PoolAllocator::PoolAllocator(usize block_size, usize block_count,
                             usize alignment, Allocator* backing) noexcept
    : _block_size(static_cast<u64>(block_size))
    , _block_count(static_cast<u64>(block_count))
    , _alignment(static_cast<u64>(alignment))
    , _backing(backing ? backing : &DefaultAllocator()) {

    // ブロックサイズをフリーリストノードが収まる最低サイズに揃える
    if (_alignment < sizeof(void*)) _alignment = sizeof(void*);
    if (_block_size < sizeof(Node)) _block_size = sizeof(Node);
    _block_size = AlignUp(_block_size, _alignment);

    // ストレージ全体を 1 回確保
    usize total = static_cast<usize>(_block_size * _block_count);
    _storage = static_cast<u8*>(_backing->Alloc(total, _alignment, SourceLoc::Current()));
    if (!_storage) {
        _block_count = 0;
        return;
    }

    // 全ブロックを単方向リンクで連結（初期化はシングルスレッド前提）
    Node* prev = nullptr;
    for (u64 i = 0; i < _block_count; ++i) {
        Node* n = reinterpret_cast<Node*>(_storage + i * _block_size);
        n->next = prev;
        prev = n;
    }
    u64 packed = Pack(this, prev, 0);
    _head_packed.Store(packed, EMemoryOrder::Release);
}

PoolAllocator::~PoolAllocator() noexcept {
    if (_storage) _backing->Free(_storage);
}

// 確保（Treiber スタックの pop）
void* PoolAllocator::Alloc(usize size, usize alignment, SourceLoc /*loc*/) noexcept {
    if (size == 0) return nullptr;
    if (size > _block_size) return nullptr;
    if (alignment > _alignment) return nullptr;

    while (true) {
        u64 head = _head_packed.Load(EMemoryOrder::Acquire);
        Node* top = static_cast<Node*>(UnpackPtr(head));
        if (!top) return nullptr;  // プール枯渇
        u64 tag = UnpackTag(head);
        // top->next を読む（ABA タグで CAS が確実に失敗するため安全）
        Node* next = top->next;
        u64 desired = Pack(this, next, tag + 1);  // タグ +1 で世代を進める
        u64 expected = head;
        if (_head_packed.CompareExchange(expected, desired)) {
            _live.FetchAdd(1);
            return top;
        }
        // CAS 失敗ならリトライ
    }
}

// 解放（Treiber スタックの push）
void PoolAllocator::Free(void* ptr) noexcept {
    if (!ptr) return;
    Node* n = static_cast<Node*>(ptr);
    while (true) {
        u64 head = _head_packed.Load(EMemoryOrder::Acquire);
        Node* top = static_cast<Node*>(UnpackPtr(head));
        u64 tag = UnpackTag(head);
        n->next = top;
        u64 desired = Pack(this, n, tag + 1);
        u64 expected = head;
        if (_head_packed.CompareExchange(expected, desired)) {
            _live.FetchSub(1);
            return;
        }
    }
}

} // namespace acs
