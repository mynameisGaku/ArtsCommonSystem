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

class PoolAllocator final : public Allocator {
public:
    // 1 ブロックのサイズ（最低 sizeof(Node)=8B にラウンドアップ）
    // ブロック総数を block_count、整列を alignment で指定。
    PoolAllocator(usize block_size, usize block_count,
                  usize alignment = kDefaultAlignment,
                  Allocator* backing = nullptr) noexcept;
    ~PoolAllocator() noexcept override;

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free (void* ptr)                                  noexcept override;

    // ブロックサイズ / 総数の取得
    u64 BlockSize()      const noexcept { return _block_size; }
    u64 BlockCount()     const noexcept { return _block_count; }

    // 現在の使用量 = 生存ブロック数 × ブロックサイズ
    u64 BytesAllocated() const noexcept override {
        return _live.Load(MemoryOrder::Acquire) * _block_size;
    }
    const char* Name()   const noexcept override { return "Pool"; }

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

    u8*               _storage    = nullptr;     // ブロック配列の先頭
    u64               _block_size = 0;
    u64               _block_count= 0;
    u64               _alignment  = 0;
    Allocator*        _backing    = nullptr;     // _storage の確保元
    Atomic<u64>       _live {0};                 // 現在使用中のブロック数
    // フリーリストの head + ABA タグ を 1 つの 64bit にパック
    Atomic<u64>       _head_packed {0};
};

} // namespace acs
