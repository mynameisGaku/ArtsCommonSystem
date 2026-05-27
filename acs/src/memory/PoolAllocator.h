// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — 固定サイズブロックプール（Treiber スタック方式）
// -----------------------------------------------------------------------------
// 同サイズの小オブジェクトを大量に確保・解放するパターンに特化。
// 例: パーティクル、ノード、コンポーネント、タスクオブジェクト。
//
// アルゴリズム:
//   - 起動時に N 個分の連続バッファを確保
//   - フリーリストを「単方向リンクスタック」として管理
//   - Alloc = head を CAS で取り出し、Free = head に CAS で push
//
// ABA 問題対策:
//   Treiber スタックの古典的問題を、ポインタ上位ビットに ABA タグを
//   埋め込む方式で回避（Windows x64 のユーザ空間ポインタは下位 47 ビット
//   しか使わないので、上位 17 ビットがタグ用に空いている）。
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

class FPoolAllocator final : public FAllocator {
public:
    // 1 ブロックのサイズ（最低 sizeof(Node)=8B にラウンドアップ）
    // ブロック総数を block_count、整列を alignment で指定。
    FPoolAllocator(usize block_size, usize block_count,
                  usize alignment = kDefaultAlignment,
                  FAllocator* backing = nullptr) noexcept;
    ~FPoolAllocator() noexcept override;

    FPoolAllocator(const FPoolAllocator&) = delete;
    FPoolAllocator& operator=(const FPoolAllocator&) = delete;

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;
    void  Free (void* ptr)                                  noexcept override;

    // ブロックサイズ / 総数の取得
    u64 BlockSize()      const noexcept { return m_BlockSize; }
    u64 BlockCount()     const noexcept { return m_BlockCount; }

    // 現在の使用量 = 生存ブロック数 × ブロックサイズ
    u64 BytesAllocated() const noexcept override {
        return m_Live.Load(EMemoryOrder::Acquire) * m_BlockSize;
    }
    const char* Name()   const noexcept override { return "TPool"; }

    // ptr がこのプールから払い出されたものか判定（Heap フォールバックとの区別用）
    bool Contains(const void* ptr) const noexcept {
        if (!m_Storage || !ptr) return false;
        const u8* p = static_cast<const u8*>(ptr);
        const u8* end = m_Storage + m_BlockSize * m_BlockCount;
        return p >= m_Storage && p < end;
    }

private:
    // フリーリストノード（フリーブロックの先頭にオーバーレイ配置）
    struct Node {
        Node* next;
    };

    // ABA タグ付きポインタ（16B、未使用だが将来 DCAS への切替時用）
    struct alignas(16) TaggedPtr {
        Node* ptr;
        u64   tag;
    };

    u8*               m_Storage    = nullptr;     // ブロック配列の先頭
    u64               m_BlockSize = 0;
    u64               m_BlockCount= 0;
    u64               m_Alignment  = 0;
    FAllocator*        m_Backing    = nullptr;     // m_Storage の確保元
    TAtomic<u64>       m_Live {0};                 // 現在使用中のブロック数
    // フリーリストの head + ABA タグ を 1 つの 64bit にパック
    TAtomic<u64>       m_HeadPacked {0};
};

} // namespace acs
