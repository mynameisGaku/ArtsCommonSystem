// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FRelocatableAllocator (ハンドルベース再配置/デフラグ可能アロケータ)
// -----------------------------------------------------------------------------
// 生ポインタは「動かせない」ため、デフラグ (compaction) には間接参照が必須。
// 利用側は確保時に **ハンドル** を受け取り、Resolve(handle) で現在のポインタを得る。
// Compact() は生存ブロックを前方に詰めて外部断片化を解消し、各ハンドルの参照先を更新する
// (Compact 後はポインタが変わるので Resolve を取り直すこと)。
//
// 用途: 長時間セッションで確保/解放を繰り返す可変サイズデータ (アセットのストリーミング
// バッファ、テキスト/シリアライズ作業領域 等) を、断片化させずに詰め直したいケース。
//
// スレッド安全性: **本クラスは内部同期しない**。Compact は全ポインタを無効化するため、
// Resolve したポインタを保持したまま他スレッドが Compact すると破綻する。単一スレッドで
// 使うか、利用側で「Resolve したポインタを握っている間は Compact しない」ことを保証すること。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

class FAllocator;

// 再配置可能確保のハンドル。index = エントリ番号、generation = 世代 (use-after-free 検出)。
struct FRelocHandle {
    u32  index      = 0;
    u32  generation = 0;   // 0 = 無効
    bool IsValid() const noexcept { return generation != 0u; }
};
inline bool operator==(FRelocHandle a, FRelocHandle b) noexcept {
    return a.index == b.index && a.generation == b.generation;
}
inline bool operator!=(FRelocHandle a, FRelocHandle b) noexcept { return !(a == b); }

class FRelocatableAllocator {
public:
    FRelocatableAllocator() noexcept = default;
    ~FRelocatableAllocator() noexcept;

    FRelocatableAllocator(const FRelocatableAllocator&) = delete;
    FRelocatableAllocator& operator=(const FRelocatableAllocator&) = delete;

    // capacity_bytes のアリーナ + max_handles 個のハンドルテーブルを backing から確保する。
    // backing=nullptr なら DefaultAllocator を使う。
    TResult<void> Init(usize capacity_bytes, u32 max_handles, FAllocator* backing = nullptr) noexcept;
    void Shutdown() noexcept;

    // size バイトを確保しハンドルを返す。失敗時は無効ハンドル (IsValid()==false)。
    // 末尾に入らないが詰めれば入る場合は自動的に Compact してから確保する。
    FRelocHandle Alloc(usize size, usize alignment = 16) noexcept;
    void  Free(FRelocHandle h) noexcept;

    // ハンドルの現在のポインタ。無効/解放済みなら nullptr。**次の Compact まで有効**。
    void* Resolve(FRelocHandle h) const noexcept;
    usize SizeOf(FRelocHandle h)  const noexcept;
    bool  ValidateHandle(FRelocHandle h) const noexcept;

    // 生存ブロックを前方へ詰めて断片化を解消する。戻り値 = 回収した high-water バイト。
    // 全ハンドルの参照先を更新する (呼び出し後は Resolve を取り直すこと)。
    usize Compact() noexcept;

    usize Capacity()  const noexcept { return m_Capacity; }
    usize Used()      const noexcept { return m_LiveBytes; }   // 生存ペイロード総量
    usize HighWater() const noexcept { return m_Cursor; }      // bump カーソル (gap 込み)
    u32   LiveCount() const noexcept { return m_LiveCount; }

private:
    struct Entry {
        u8*  ptr;          // payload (アリーナ内、Compact で更新)
        u32  size;
        u32  align;
        u32  generation;
        bool live;
        u32  next_free;    // 空きエントリの単方向リスト
    };
    bool ResolveEntry(FRelocHandle h, Entry*& out) const noexcept;

    u8*         m_Base       = nullptr;   // アリーナ先頭
    usize       m_Capacity   = 0;
    usize       m_Cursor     = 0;         // bump 位置 (= high water)
    usize       m_LiveBytes  = 0;
    Entry*      m_Entries     = nullptr;
    u32         m_MaxHandles = 0;
    u32         m_FreeHead   = 0xFFFFFFFFu;
    u32         m_LiveCount  = 0;
    u32*        m_Order      = nullptr;    // Compact 用作業配列 (max_handles)
    FAllocator* m_Backing    = nullptr;
};

} // namespace acs
