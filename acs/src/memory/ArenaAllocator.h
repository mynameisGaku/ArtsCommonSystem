// =============================================================================
// ACS Memory — アリーナ（ページバック式バンプ）アロケータ
// -----------------------------------------------------------------------------
// LinearAllocator と似ているが、容量を固定せず「ページが足りなくなったら
// 追加ページを backing から取って繋げる」方式。サイズが事前に分からない
// 場面（パース、ノード木の構築等）で便利。
//
// Reset(false) で全ページを再利用、Reset(true) でページを backing に返す。
//
// スレッド安全性:
//   - 同一ページ内のバンプはアトミック CAS（マルチスレッド OK）
//   - 新ページ確保（Grow パス）は Mutex で直列化
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"

namespace acs {

class ArenaAllocator final : public Allocator {
public:
    // 1 ページのサイズ（既定 64KB）。page_size より大きい確保要求は
    // 専用のページが 1 つ作られる。
    ArenaAllocator(usize page_size = 64 * 1024,
                   Allocator* backing = nullptr) noexcept;
    ~ArenaAllocator() noexcept override;

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free (void* ptr) noexcept override;  // no-op

    // 全確保済みメモリを「無効」にする。release_pages=true ならページも解放。
    // false なら次の Alloc で再利用される（OS への返却なし、高速）。
    void Reset(bool release_pages = false) noexcept;

    u64 BytesAllocated() const noexcept override { return _bytes.Load(MemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(MemoryOrder::Acquire); }
    const char* Name()   const noexcept override { return "Arena"; }

private:
    // 1 ページの管理ヘッダ
    struct Page {
        Page*       next;     // ページの単方向リンク
        u8*         base;     // ページのデータ領域先頭
        u64         size;     // ページのデータ領域サイズ
        Atomic<u64> used;     // 現在のカーソル位置
    };

    Page* AllocPage(usize size) noexcept;

    Allocator*    _backing  = nullptr;
    usize         _page_size = 0;
    Atomic<Page*> _current  {nullptr};   // 現在書き込み中のページ
    Page*         _pages    = nullptr;   // 全ページのリスト
    Mutex         _grow_lock;            // 新ページ確保用ロック
    Atomic<u64>   _bytes {0};
    mutable Atomic<u64> _peak  {0};
};

} // namespace acs
