// SPDX-License-Identifier: Apache-2.0
// ページバック式バンプアロケータ（容量を固定せず、満杯になったらページ追加）
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"

namespace acs {

class FArenaAllocator final : public FAllocator {
public:
    // 1 ページあたりのサイズを指定（既定 64KB）
    FArenaAllocator(usize page_size = 64 * 1024,
                   FAllocator* backing = nullptr) noexcept;
    ~FArenaAllocator() noexcept override;

    FArenaAllocator(const FArenaAllocator&) = delete;
    FArenaAllocator& operator=(const FArenaAllocator&) = delete;

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;
    void  Free (void* ptr) noexcept override;  // no-op

    // 全確保を「無効」にする。release_pages=true なら backing にページを返却
    void Reset(bool release_pages = false) noexcept;

    u64 BytesAllocated() const noexcept override { return _bytes.Load(EMemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(EMemoryOrder::Acquire); }
    const char* Name()   const noexcept override { return "Arena"; }

private:
    // 1 ページの管理ヘッダ
    struct FPage {
        FPage*       next;     // 単方向リンク
        u8*         base;     // データ領域先頭
        u64         size;     // データ領域サイズ
        TAtomic<u64> used;     // 現在のカーソル位置
    };

    FPage* AllocPage(usize size) noexcept;

    FAllocator*    _backing  = nullptr;
    usize         _page_size = 0;
    TAtomic<FPage*> _current  {nullptr};   // 現在書き込み中のページ
    FPage*         _pages    = nullptr;   // 全ページのリスト
    FMutex         _grow_lock;            // 新ページ確保用
    TAtomic<u64>   _bytes {0};
    mutable TAtomic<u64> _peak  {0};
};

} // namespace acs
