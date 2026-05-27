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

    u64 BytesAllocated() const noexcept override { return m_Bytes.Load(EMemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return m_Peak.Load(EMemoryOrder::Acquire); }
    const char* Name()   const noexcept override { return "Arena"; }

private:
    // 1 ページの管理ヘッダ
    struct Page {
        Page*       next;     // 単方向リンク
        u8*         base;     // データ領域先頭
        u64         size;     // データ領域サイズ
        TAtomic<u64> used;     // 現在のカーソル位置
    };

    Page* AllocPage(usize size) noexcept;

    FAllocator*    m_Backing  = nullptr;
    usize         m_PageSize = 0;
    TAtomic<Page*> m_Current  {nullptr};   // 現在書き込み中のページ
    Page*         m_Pages    = nullptr;   // 全ページのリスト
    FMutex         m_GrowLock;            // 新ページ確保用
    TAtomic<u64>   m_Bytes {0};
    mutable TAtomic<u64> m_Peak  {0};
};

} // namespace acs
