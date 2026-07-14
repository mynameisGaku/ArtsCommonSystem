// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - mimalloc first-class heap アロケータ
// -----------------------------------------------------------------------------
// mimalloc の first-class heap を ACS の FAllocator 契約へ接続する。
// 要求サイズ、件数、ハード予算、サイズ分布は ACS 側で追跡し、mimalloc の
// usable size やプロセス全体統計には依存しない。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

/** 1 つの要求サイズ帯に現在存在する確保の統計。 */
struct MimallocAllocationSizeStatistics {
    /** 現在生存している確保の件数。 */
    u64 allocation_count = 0;

    /** 現在生存している確保の要求バイト合計。 */
    u64 requested_bytes = 0;
};

/** 要求サイズ別の現在値。サイズ境界は利用者が調整せず、全ヒープで共通とする。 */
struct MimallocAllocationHistogram {
    /** 4 KiB 以下の確保。 */
    MimallocAllocationSizeStatistics small;

    /** 4 KiB 超 1 MiB 以下の確保。 */
    MimallocAllocationSizeStatistics medium;

    /** 1 MiB 超の確保。 */
    MimallocAllocationSizeStatistics large;
};

/** mi_heap_visit_blocks から再構築した、追跡カウンタとは独立したヒープ統計。 */
struct MimallocHeapInspectionStatistics {
    /** 列挙した mimalloc ページ領域の数。 */
    u64 area_count = 0;

    /** ページ領域が予約している仮想アドレス空間の合計。 */
    u64 reserved_bytes = 0;

    /** ページ領域がコミットしているバイト数の合計。 */
    u64 committed_bytes = 0;

    /** 列挙した生存ブロックの件数。 */
    u64 allocation_count = 0;

    /** ブロックヘッダから復元した要求バイト合計。 */
    u64 requested_bytes = 0;

    /** mimalloc が報告した生存ブロックの usable size 合計。 */
    u64 usable_bytes = 0;

    /** mimalloc の列挙処理自体が完走したか。 */
    bool visit_succeeded = false;

    /** 全ブロックがこのアロケータの正しいヘッダを持っていたか。 */
    bool metadata_valid = false;

    /** 独立列挙結果が ACS の atomic カウンタと一致したか。 */
    bool matches_authoritative_statistics = false;
};

/**
 * mimalloc v3 first-class heap を 1 つ所有する、汎用スレッドセーフアロケータ。
 *
 * @details
 * Alloc / Free / Realloc は別スレッドから同時に呼べる。要求サイズは独自ヘッダで
 * 保持するため、BytesAllocated は mimalloc の丸め後サイズではなく利用者が要求した
 * 正確な合計を返す。Init / Shutdown / Collect / InspectHeap は保守操作なので、対象
 * アロケータへの Alloc / Free / Realloc が止まった状態で呼ぶこと。
 *
 * ハード予算は atomic な予約カウンタで守る。生存中の要求量に確保処理中の要求量も
 * 加えた値が上限を超える操作は、mimalloc を呼ぶ前に失敗する。
 */
class FMimallocAllocator final : public FAllocator {
public:
    /** 小サイズ帯の上限 (4 KiB、境界を含む)。 */
    static constexpr u64 kSmallAllocationMaximumBytes = 4ull * 1024ull;

    /** 中サイズ帯の上限 (1 MiB、境界を含む)。 */
    static constexpr u64 kMediumAllocationMaximumBytes = 1024ull * 1024ull;

    /** 未初期化状態で構築する。使用前に Init を呼ぶこと。 */
    FMimallocAllocator() noexcept = default;

    /** ヒープを破棄する。未解放確保があれば Logger 非依存の診断を出す。 */
    ~FMimallocAllocator() noexcept override;

    /** ヒープを単独所有するためコピーしない。 */
    FMimallocAllocator(const FMimallocAllocator&) = delete;

    /** ヒープを単独所有するためコピー代入しない。 */
    FMimallocAllocator& operator=(const FMimallocAllocator&) = delete;

    /** ライフサイクルと atomic カウンタの所有者を固定するためムーブしない。 */
    FMimallocAllocator(FMimallocAllocator&&) = delete;

    /** ライフサイクルと atomic カウンタの所有者を固定するためムーブ代入しない。 */
    FMimallocAllocator& operator=(FMimallocAllocator&&) = delete;

    /**
     * first-class heap を作成し、利用可能状態にする。
     *
     * @param BudgetBytes 要求バイト合計の上限。0 は無制限。
     * @return 成功なら空の TResult。多重初期化またはヒープ作成失敗ならエラー。
     */
    TResult<void> Init(u64 BudgetBytes = 0) noexcept;

    /**
     * ヒープを強制収集して破棄し、再 Init 可能な未初期化状態へ戻す。
     *
     * @details 未解放確保は機械可読ログへ記録してから mi_heap_destroy で回収する。
     */
    void Shutdown() noexcept;

    /** Size バイトを Alignment 整列で確保する。不正値、予算超過、OOM は nullptr。 */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override;

    /** このアロケータが払い出した領域を解放する。nullptr は何もしない。 */
    void Free(void* Pointer) noexcept override;

    /**
     * 確保を NewSize バイトへ変更する。
     *
     * @details 失敗時は旧領域と統計を保持する。OldSize は互換引数であり、実際の
     * コピー量と統計には割り当てヘッダの要求サイズを使う。
     */
    void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept override;

    /** 現在生存している確保の要求バイト合計を返す。 */
    u64 BytesAllocated() const noexcept override
    {
        return m_RequestedBytes.Load(EMemoryOrder::Acquire);
    }

    /** 現在生存している確保の件数を返す。 */
    u64 AllocationCount() const noexcept override
    {
        return m_AllocationCount.Load(EMemoryOrder::Acquire);
    }

    /** 過去に同時生存した要求バイト合計の最大値を返す。 */
    u64 PeakBytes() const noexcept override
    {
        return m_PeakRequestedBytes.Load(EMemoryOrder::Acquire);
    }

    /** 現在の first-class heap 寿命を識別する世代を返す。未初期化なら0。 */
    u64 LifetimeGeneration() const noexcept override
    {
        return m_Generation;
    }

    /** 識別名を返す。 */
    const char* Name() const noexcept override
    {
        return "Mimalloc";
    }

    /** 初期化済みなら true。 */
    bool IsInitialized() const noexcept
    {
        return m_Heap != nullptr;
    }

    /** Init で指定したハード予算を返す。0 は無制限。 */
    u64 HardBudgetBytes() const noexcept
    {
        return m_HardBudgetBytes;
    }

    /**
     * Pointer が現在の世代のこのアロケータから払い出された利用者ポインタかを検証する。
     *
     * @details
     * Pointer は nullptr、または生存中の FMimallocAllocator が返した正規の先頭ポインタに
     * 限る。任意アドレスや領域内部のポインタを調べる一般的なアドレス範囲 API ではない。
     *
     * @return 正しい所有ポインタなら true。nullptr、他ヒープ、破損ヘッダは false。
     */
    bool OwnsAllocation(const void* Pointer) const noexcept;

    /**
     * 現在のヒープに対して mimalloc の収集処理を実行する。
     *
     * @details Alloc / Free / Realloc が停止した保守点で呼ぶこと。
     */
    void Collect(bool bForce) noexcept;

    /** 現在生存している確保のサイズ分布を atomic カウンタから取得する。 */
    MimallocAllocationHistogram CaptureAllocationHistogram() const noexcept;

    /**
     * mimalloc のブロック列挙から独立統計を再構築する。
     *
     * @details mimalloc の列挙契約に従い、Alloc / Free / Realloc が停止した状態で呼ぶこと。
     */
    MimallocHeapInspectionStatistics InspectHeap() noexcept;

    /** リンク中の mimalloc 実行時バージョン番号を返す。未初期化でも呼べる。 */
    static int RuntimeVersion() noexcept;

private:
    /** 予算カウンタへ Amount を CAS 予約する。 */
    bool TryReserveBudget(u64 Amount) noexcept;

    /** 予算カウンタから Amount を返却する。 */
    void ReleaseBudget(u64 Amount) noexcept;

    /** 成功した確保を要求量・件数・ピーク・ヒストグラムへ反映する。 */
    void RecordAllocation(u64 RequestedBytes) noexcept;

    /** 解放した確保を要求量・件数・ヒストグラムへ反映する。 */
    void RecordFree(u64 RequestedBytes) noexcept;

    /** Realloc 成功時の要求量とヒストグラムを更新する。件数は変えない。 */
    void RecordReallocation(u64 OldRequestedBytes, u64 NewRequestedBytes) noexcept;

    /** 同時生存要求バイトのピークを CAS で更新する。 */
    void UpdatePeak(u64 Candidate) noexcept;

    /** first-class mi_heap_t。公開ヘッダへ mimalloc 型を漏らさないため void* で保持する。 */
    void* m_Heap = nullptr;

    /** Init ごとに変わる所有世代。古いヘッダの誤受理を防ぐ。 */
    u64 m_Generation = 0;

    /** 0 は無制限、それ以外は要求バイト合計の厳密な上限。 */
    u64 m_HardBudgetBytes = 0;

    /** 生存中に加え、mimalloc 呼び出し中の仮予約も含む予算消費量。 */
    TAtomic<u64> m_BudgetReservedBytes{0};

    /** 現在生存している要求バイト合計。 */
    TAtomic<u64> m_RequestedBytes{0};

    /** 過去の同時生存要求バイト最大値。 */
    TAtomic<u64> m_PeakRequestedBytes{0};

    /** 現在生存している確保件数。 */
    TAtomic<u64> m_AllocationCount{0};

    /** 小サイズ確保の現在件数。 */
    TAtomic<u64> m_SmallAllocationCount{0};

    /** 小サイズ確保の現在要求バイト合計。 */
    TAtomic<u64> m_SmallRequestedBytes{0};

    /** 中サイズ確保の現在件数。 */
    TAtomic<u64> m_MediumAllocationCount{0};

    /** 中サイズ確保の現在要求バイト合計。 */
    TAtomic<u64> m_MediumRequestedBytes{0};

    /** 大サイズ確保の現在件数。 */
    TAtomic<u64> m_LargeAllocationCount{0};

    /** 大サイズ確保の現在要求バイト合計。 */
    TAtomic<u64> m_LargeRequestedBytes{0};
};

} // namespace acs
