// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "memory/ArenaAllocatorDiagnostics.h"
#include "memory/Allocator.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"

namespace acs {

/**
 * ページバック式バンプアロケータ (容量無制限、満杯になればページを足す)。
 *
 * @details
 * 通常確保は現在ページ内をアトミック CAS 1 回で前進させる (lock-free)。ページが満杯に
 * なったときだけ FMutex で排他して backing から新ページを確保する。PageSize より大きい
 * 要求には専用ページを割り当てる。個別 Free は非対応 (no-op)。Reset で世代を進めて page を
 * 遅延再利用するか、全ページを backing に返す。Reset は開始済みの Alloc が完了するまで待ち、
 * Reset 開始後の Alloc は nullptr で拒否する。
 */
class CArenaAllocator final : public IAllocator {
public:
    /**
     * 1 ページあたりのサイズと backing を指定して構築する (ページは遅延確保)。
     *
     * @param PageSize 1 ページのデータ領域サイズ (既定 64KiB)。
     * @param BackingAllocator ページの確保元 (nullptr なら DefaultAllocator)。
     */
    CArenaAllocator(usize PageSize = 64 * 1024, IAllocator* BackingAllocator = nullptr) noexcept;

    /** 全ページを backing に返して破棄する。 */
    ~CArenaAllocator() noexcept override;

    /** コピー禁止 (ページ群を単独所有するため)。 */
    CArenaAllocator(const CArenaAllocator&) = delete;

    /** コピー代入も禁止。 */
    CArenaAllocator& operator=(const CArenaAllocator&) = delete;

    /**
     * 現在ページから alignment 整列で size バイトを切り出す (満杯なら新ページを確保)。
     *
     * @details
     * lock-free CAS による確保。新ページ確保時のみ GrowLock を取る。size==0、OOM、または
     * Reset 実行中は nullptr。Reset と競合して nullptr になった場合は次のフレームで再試行できる。
     * @param Size 確保するバイト数。
     * @param Alignment 要求アライメント (1 未満なら 1 に補正)。
     * @param Location 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * 同じ size と alignment の領域を 1 回の cursor 予約からまとめて返す。
     *
     * @param Output Count 件の結果を書き込む配列。失敗時は全要素を nullptr にする。
     * @param Count 必要な領域数。
     * @param Size 1 領域の要求 byte 数。
     * @param Alignment 各領域の要求 alignment。
     * @param Location 診断用の呼び出し位置。
     * @return 全領域を確保できた場合は true。
     */
    bool AllocBatch(void** Output, usize Count, usize Size, usize Alignment, FSourceLoc Location = FSourceLoc::Current()) noexcept;

    /**
     * 個別解放は非対応 (no-op)。
     *
     * @details まとめて Reset で破棄する。
     * @param Pointer 無視される。
     */
    void Free(void* Pointer) noexcept override;

    /**
     * 指定範囲が、最後の Reset 以降に arena が使用済みにしたページ領域へ完全に含まれるか調べる。
     *
     * @details
     * allocation-start の判定は行わない。呼び出し側がインバンドヘッダを安全に読む前の範囲検査に使う。
     * Reset とは内部の操作ゲートで直列化し、ページ一覧は GrowLock の保持中だけ走査する。
     * @param Pointer 検査する範囲の先頭。
     * @param Size 検査するバイト数。
     * @return 範囲全体が現在の使用済みページ領域内なら true。
     */
    bool ContainsCurrentAllocationRange(const void* Pointer, usize Size) noexcept;

    /**
     * 全確保を無効化する。
     *
     * @details
     * bReleasePages=false なら世代を進めて先頭ページだけを初期化し、残りは再公開直前に
     * GrowLock 内で遅延初期化する。通常経路は保持ページ数に依存しない。true なら全ページを
     * backing に返却する。新しい Alloc の入場を閉じ、
     * 開始済みの Alloc が完了してから GrowLock を取るため、並行実行でもページへ競合アクセスしない。
     * Reset 後はそれ以前に払い出した全ポインタが無効になるため、呼び出し側はデータ利用者を別途停止すること。
     * @param bReleasePages true でページを backing に返却、false でカーソルだけ巻き戻し。
     */
    void Reset(bool bReleasePages = false) noexcept;

    /**
     * 現在の batch と世代 reset 診断値を返す。
     *
     * @return 各 atomic 値を読み取った診断 snapshot。
     */
    FArenaAllocatorDiagnostics Diagnostics() const noexcept;

    /**
     * 現在の総割当バイト数を返す。
     *
     * @return 全ページ累計の確保バイト数 (Reset で 0 に戻る)。
     */
    u64 BytesAllocated() const noexcept override
    {
        return m_Bytes.Load(EMemoryOrder::Acquire);
    }

    /**
     * 最後の Reset 以降に払い出した領域の件数を返す。
     *
     * @details 個別 Free は非対応なので、Reset するまで件数は減らない。
     * @return 現在の一括寿命に属する確保件数。
     */
    u64 AllocationCount() const noexcept override
    {
        return m_AllocationCount.Load(EMemoryOrder::Acquire);
    }

    /**
     * 過去ピークの割当バイト数を返す。
     *
     * @return これまでの最大割当バイト数。
     */
    u64 PeakBytes() const noexcept override
    {
        return m_Peak.Load(EMemoryOrder::Acquire);
    }

    /**
     * 識別名を返す。
     *
     * @return 文字列 "Arena"。
     */
    const char* Name() const noexcept override
    {
        return "Arena";
    }

private:
    /** 1 ページの管理ヘッダ (ページ先頭に置かれ、直後にデータ領域が続く)。 */
    struct FPage {
        /** 次ページへの単方向リンク (全ページリスト用)。 */
        FPage* next;

        /** データ領域の先頭 (ヘッダ直後を 64B 整列した位置)。 */
        u8* base;

        /** データ領域のサイズ (バイト)。 */
        u64 size;

        /** 現在のカーソル位置 (このページ内の確保済みバイト数)。 */
        TAtomic<u64> used;

        /** この page の used が属する Reset 世代。 */
        u64 generation;
    };

    /**
     * backing から size バイトのデータ領域を持つ新ページを確保する。
     *
     * @details ヘッダ + データ + 64B 整列の余裕をまとめて 1 回確保する。加算オーバーフローや確保失敗時は nullptr。
     * @param Size 新ページのデータ領域サイズ。
     * @return 初期化済み FPage (失敗時 nullptr)。
     */
    FPage* AllocPage(usize Size) noexcept;

    /**
     * 1 回の cursor 更新で連続領域を予約し、利用者向け統計を指定件数分だけ加算する。
     *
     * @param ReservedSize cursor 上で予約する byte 数。
     * @param Alignment 予約先頭の alignment。
     * @param AccountedBytes BytesAllocated に加算する利用者 byte 数。
     * @param AccountedAllocations AllocationCount に加算する利用者領域数。
     * @return 予約先頭。失敗時は nullptr。
     */
    ACS_FORCEINLINE void* ReserveRegion(usize ReservedSize, usize Alignment, usize AccountedBytes, usize AccountedAllocations) noexcept;

    /** Reset と競合しない確保操作として入場できれば true を返す。 */
    bool TryBeginAllocation() noexcept;

    /** TryBeginAllocation に成功した確保操作の完了を通知する。 */
    void EndAllocation() noexcept;

    /** ページの確保元アロケータ。 */
    IAllocator* m_Backing = nullptr;

    /** 1 ページのデータ領域サイズ。 */
    usize m_PageSize = 0;

    /** 現在書き込み中のページ (Acquire/Release で公開)。 */
    TAtomic<FPage*> m_Current{nullptr};

    /** 確保した全ページの単方向リスト (Reset での返却に使う)。 */
    FPage* m_Pages = nullptr;

    /** 新ページ確保とリスト操作を排他するロック。 */
    mutable FMutex m_GrowLock;

    /** 1 の間は Reset がページ群を操作中で、新しい Alloc を拒否する。 */
    TAtomic<u32> m_ResetInProgress{0};

    /** Reset が完了を待つ、入場済み Alloc の件数。 */
    TAtomic<u32> m_ActiveAllocations{0};

    /** 現在の総割当バイト数。 */
    TAtomic<u64> m_Bytes{0};

    /** 最後の Reset 以降に成功した確保件数。 */
    TAtomic<u64> m_AllocationCount{0};

    /** 過去ピークの割当バイト数 (CAS で更新)。 */
    mutable TAtomic<u64> m_Peak{0};

    /** 現在の Reset 世代。GrowLock または Reset gate の内側で更新する。 */
    u64 m_Generation = 1u;

    /** 最後の Reset 以降に成功した batch 確保回数。 */
    TAtomic<u64> m_BatchAllocationCount{0};

    /** 最後の Reset 以降に batch で返した領域数。 */
    TAtomic<u64> m_BatchSuballocationCount{0};

    /** 直前の Reset が直接参照した page 数。 */
    TAtomic<u64> m_LastResetPageVisits{0};

    /** 最後の Reset 以降に遅延初期化した page 数。 */
    TAtomic<u64> m_LazyPageResetCount{0};
};

/** 移行期間中に旧名を受け付ける互換別名。 */
using FArenaAllocator = CArenaAllocator;

} // namespace acs
