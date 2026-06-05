// SPDX-License-Identifier: Apache-2.0
// ページバック式バンプアロケータ（容量を固定せず、満杯になったらページ追加）
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"

namespace acs {

/**
 * ページバック式バンプアロケータ (容量無制限、満杯になればページを足す)。
 *
 * @details
 * 通常確保は現在ページ内をアトミック CAS 1 回で前進させる (lock-free)。ページが満杯に
 * なったときだけ FMutex で排他して backing から新ページを確保する。page_size より大きい
 * 要求には専用ページを割り当てる。個別 Free は非対応 (no-op)。Reset でカーソルを巻き戻すか
 * (ページ保持)、全ページを backing に返す。
 */
class FArenaAllocator final : public FAllocator {
public:
    /**
     * 1 ページあたりのサイズと backing を指定して構築する (ページは遅延確保)。
     *
     * @param page_size 1 ページのデータ領域サイズ (既定 64KiB)。
     * @param backing ページの確保元 (nullptr なら DefaultAllocator)。
     */
    FArenaAllocator(usize page_size = 64 * 1024,
                   FAllocator* backing = nullptr) noexcept;

    /** 全ページを backing に返して破棄する。 */
    ~FArenaAllocator() noexcept override;

    /** コピー禁止 (ページ群を単独所有するため)。 */
    FArenaAllocator(const FArenaAllocator&) = delete;

    /** コピー代入も禁止。 */
    FArenaAllocator& operator=(const FArenaAllocator&) = delete;

    /**
     * 現在ページから alignment 整列で size バイトを切り出す (満杯なら新ページを確保)。
     *
     * @details lock-free CAS による確保。新ページ確保時のみ GrowLock を取る。size==0 や OOM 時は nullptr。
     * @param size 確保するバイト数。
     * @param alignment 要求アライメント (1 未満なら 1 に補正)。
     * @param loc 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;

    /**
     * 個別解放は非対応 (no-op)。
     *
     * @details まとめて Reset で破棄する。
     * @param ptr 無視される。
     */
    void  Free (void* ptr) noexcept override;

    /**
     * 全確保を無効化する。
     *
     * @details
     * release_pages=false なら全ページのカーソルを 0 に巻き戻してページを再利用する
     * (再確保なし)。true なら全ページを backing に返却する。GrowLock を取って排他する。
     * @param release_pages true でページを backing に返却、false でカーソルだけ巻き戻し。
     */
    void Reset(bool release_pages = false) noexcept;

    /**
     * 現在の総割当バイト数を返す。
     *
     * @return 全ページ累計の確保バイト数 (Reset で 0 に戻る)。
     */
    u64 BytesAllocated() const noexcept override { return m_Bytes.Load(EMemoryOrder::Acquire); }

    /**
     * 過去ピークの割当バイト数を返す。
     *
     * @return これまでの最大割当バイト数。
     */
    u64 PeakBytes()      const noexcept override { return m_Peak.Load(EMemoryOrder::Acquire); }

    /**
     * 識別名を返す。
     *
     * @return 文字列 "Arena"。
     */
    const char* Name()   const noexcept override { return "Arena"; }

private:
    /** 1 ページの管理ヘッダ (ページ先頭に置かれ、直後にデータ領域が続く)。 */
    struct Page {
        /** 次ページへの単方向リンク (全ページリスト用)。 */
        Page*       next;

        /** データ領域の先頭 (ヘッダ直後を 64B 整列した位置)。 */
        u8*         base;

        /** データ領域のサイズ (バイト)。 */
        u64         size;

        /** 現在のカーソル位置 (このページ内の確保済みバイト数)。 */
        TAtomic<u64> used;
    };

    /**
     * backing から size バイトのデータ領域を持つ新ページを確保する。
     *
     * @details ヘッダ + データ + 64B 整列の余裕をまとめて 1 回確保する。加算オーバーフローや確保失敗時は nullptr。
     * @param size 新ページのデータ領域サイズ。
     * @return 初期化済み Page (失敗時 nullptr)。
     */
    Page* AllocPage(usize size) noexcept;

    /** ページの確保元アロケータ。 */
    FAllocator*    m_Backing  = nullptr;

    /** 1 ページのデータ領域サイズ。 */
    usize         m_PageSize = 0;

    /** 現在書き込み中のページ (Acquire/Release で公開)。 */
    TAtomic<Page*> m_Current  {nullptr};

    /** 確保した全ページの単方向リスト (Reset での返却に使う)。 */
    Page*         m_Pages    = nullptr;

    /** 新ページ確保とリスト操作を排他するロック。 */
    FMutex         m_GrowLock;

    /** 現在の総割当バイト数。 */
    TAtomic<u64>   m_Bytes {0};

    /** 過去ピークの割当バイト数 (CAS で更新)。 */
    mutable TAtomic<u64> m_Peak  {0};
};

} // namespace acs
