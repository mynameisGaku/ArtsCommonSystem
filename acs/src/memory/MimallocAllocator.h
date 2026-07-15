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
#include "threading/ConditionVar.h"
#include "threading/Mutex.h"

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

/** TryReallocateAllocation の所有権取得結果と再確保結果。 */
struct MimallocReallocationResult
{
    /** 成功後の利用者ポインタ。解放成功または失敗時は nullptr。 */
    void* Pointer = nullptr;

    /** 旧確保の Active 状態をこの呼び出しが取得できたなら true。 */
    bool bOwnershipAccepted = false;

    /** 再確保、同一ポインタ維持、または NewSize == 0 の論理解放に成功したなら true。 */
    bool bSucceeded = false;
};

/**
 * mimalloc v3 first-class heap を 1 つ所有する、汎用スレッドセーフアロケータ。
 *
 * @details
 * Alloc / Free / Realloc は別スレッドから同時に呼べる。要求サイズは独自ヘッダで
 * 保持するため、BytesAllocated は mimalloc の丸め後サイズではなく利用者が要求した
 * 正確な合計を返す。Init / Shutdown / Collect / InspectHeap は内部ライフサイクルゲートで
 * Alloc / Free / Realloc と同期するため、別スレッドから並行して呼べる。
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

    /**
     * このアロケータが払い出した領域を解放する。nullptr は何もしない。
     *
     * @details 同一 Pointer に対する並行 Free / Realloc は、ヘッダ状態を先に取得できた 1 操作だけが
     * 所有権を移す。先行操作が完了した後に古い Pointer を再使用する逐次 UAF は契約外。
     */
    void Free(void* Pointer) noexcept override;

    /**
     * 所有権状態を原子的に取得できた場合だけ Pointer を論理解放する。
     *
     * @return この呼び出しが Active から Released へ遷移させたなら true。
     */
    bool TryFreeAllocation(void* Pointer) noexcept;

    /**
     * 確保を NewSize バイトへ変更する。
     *
     * @details 失敗時は旧領域と統計を保持する。OldSize は互換引数であり、実際の
     * コピー量と統計には割り当てヘッダの要求サイズを使う。同一 Pointer の並行変更では
     * ヘッダ状態を取得できた 1 操作だけが成功し、競合した操作は旧領域へ触れずに失敗する。
     */
    void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * 旧確保の所有権取得結果を伴って再確保する。
     *
     * @details 外部追跡層は OwnsAllocation との check-then-act を行わず、この戻り値の
     * bSucceeded が true の場合だけ追跡を更新する。
     */
    MimallocReallocationResult TryReallocateAllocation(void* Pointer, usize NewSize, usize Alignment) noexcept;

    /** 現在生存している確保の要求バイト合計を返す。 */
    u64 BytesAllocated() const noexcept override;

    /** 現在生存している確保の件数を返す。 */
    u64 AllocationCount() const noexcept override;

    /** 過去に同時生存した要求バイト合計の最大値を返す。 */
    u64 PeakBytes() const noexcept override;

    /** 現在の first-class heap 寿命を識別する世代を返す。未初期化なら0。 */
    u64 LifetimeGeneration() const noexcept override;

    /** 識別名を返す。 */
    const char* Name() const noexcept override
    {
        return "Mimalloc";
    }

    /** 初期化済みなら true。 */
    bool IsInitialized() const noexcept;

    /** Init で指定したハード予算を返す。0 は無制限。 */
    u64 HardBudgetBytes() const noexcept;

    /**
     * Pointer が現在の世代のこのアロケータから払い出された利用者ポインタかを検証する。
     *
     * @details
     * Pointer は nullptr、または生存中の FMimallocAllocator が返した正規の先頭ポインタに
     * 限る。任意アドレス、領域内部、Free 完了後の古いポインタを調べる一般的なアドレス範囲
     * API ではない。raw pointer だけでは、解放後に同じアドレスへ別確保が再配置された ABA を
     * 識別できない。
     *
     * @return 正しい所有ポインタなら true。nullptr、他ヒープ、破損ヘッダは false。
     */
    bool OwnsAllocation(const void* Pointer) const noexcept;

    /**
     * 現在のヒープに対して mimalloc の収集処理を実行する。
     *
     * @details 新規公開操作を一時停止し、開始済み操作を待ってから収集する。
     */
    void Collect(bool bForce) noexcept;

    /** 現在生存している確保のサイズ分布を atomic カウンタから取得する。 */
    MimallocAllocationHistogram CaptureAllocationHistogram() const noexcept;

    /**
     * mimalloc のブロック列挙から独立統計を再構築する。
     *
     * @details 新規公開操作を一時停止し、開始済み操作を待ってから列挙する。
     */
    MimallocHeapInspectionStatistics InspectHeap() noexcept;

    /** リンク中の mimalloc 実行時バージョン番号を返す。未初期化でも呼べる。 */
    static int RuntimeVersion() noexcept;

private:
    /** 公開操作の入場登録をスコープ終了時に必ず解除する。 */
    class FLifecycleOperation;

    /** ライフサイクルゲートの最上位ビット。立っている間だけ新規公開操作を受け付ける。 */
    static constexpr u64 kLifecycleAcceptingBit = u64(1) << 63u;

    /** ライフサイクルゲート下位 32 bit の実行中操作件数部分。 */
    static constexpr u64 kLifecycleOperationCountMask = 0xFFFFFFFFull;

    /** 停止・再開を古い入場 CAS と区別するライフサイクル世代部分。 */
    static constexpr u64 kLifecycleGenerationMask = ~(kLifecycleAcceptingBit | kLifecycleOperationCountMask);

    /** ライフサイクル世代を 1 進める値。 */
    static constexpr u64 kLifecycleGenerationIncrement = u64(1) << 32u;

    /** Running 中の公開操作として入場できれば true を返す。 */
    bool TryBeginLifecycleOperation() const noexcept;

    /** 入場済み公開操作の完了を記録し、必要なら保守操作を起こす。 */
    void EndLifecycleOperation() const noexcept;

    /** 新規入場を閉じ、開始済み公開操作がすべて完了するまで待つ。制御ロック保持中に呼ぶ。 */
    void CloseLifecycleGateAndWait() noexcept;

    /** 完全初期化後、進めた世代で新規公開操作の受付を開始する。制御ロック保持中に呼ぶ。 */
    void OpenLifecycleGate() noexcept;

    /** 入場済み Alloc の本体。 */
    void* AllocateAfterLifecycleAdmission(usize Size, usize Alignment) noexcept;

    /** 入場済み Free の本体。猶予回収の要否を bCollectionRequested へ返す。 */
    bool FreeAfterLifecycleAdmission(void* Pointer, bool& bCollectionRequested) noexcept;

    /** 入場済み Realloc の本体。猶予回収の要否を bCollectionRequested へ返す。 */
    MimallocReallocationResult ReallocateAfterLifecycleAdmission(void* Pointer, usize NewSize, usize Alignment,
                                                                 bool& bCollectionRequested) noexcept;

    /** 解放済み生ブロックを猶予リストへ積み、回収閾値へ達したら true。 */
    bool RetireAllocation(void* AllocationBase, u64 RequestedBytes) noexcept;

    /** 閾値到達後、公開操作の外側からゲートを停止して猶予リストを物理解放する。 */
    void CollectRetiredAllocationsIfNeeded() noexcept;

    /** ゲート停止・drain 完了中に猶予リストをすべて物理解放する。 */
    void FreeRetiredAllocationsWhileLifecycleIsPaused() noexcept;

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

    /**
     * mimalloc first-class heap への並行アクセスを直列化する。
     *
     * @details mi_heap_new が返す heap は複数スレッドからの同時 malloc を許さない
     * (所有スレッド専用)。ライフサイクルゲートで入場した複数スレッドが同時に
     * mi_heap_malloc / mi_heap_contains / mi_usable_size を呼ぶ経路は必ずこのロックで守る。
     * 収集・列挙・猶予解放はゲート停止 (drain 完了) 下で単一スレッド実行のため対象外。
     */
    mutable FMutex m_HeapLock;

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

    /** 受付ビット・世代・実行中公開操作件数を一体で更新するライフサイクルゲート。 */
    mutable TAtomic<u64> m_LifecycleGate{0u};

    /** Init / Shutdown / Collect / Inspect と猶予回収を直列化する。 */
    mutable FMutex m_LifecycleControlLock;

    /** 実行中公開操作の完了条件を確認する間だけ保持する待機専用ロック。 */
    mutable FMutex m_LifecycleDrainLock;

    /** 保守操作が最後の実行中公開操作の完了通知を待つ条件変数。 */
    mutable ConditionVar m_LifecycleDrainedCondition;

    /** 解放済み生ブロックの侵入リストと集計値を守る。 */
    FMutex m_RetiredAllocationLock;

    /** 解放済み生ブロックの侵入リスト先頭。内部 AllocationHeader* を void* で隠す。 */
    void* m_RetiredAllocationHead = nullptr;

    /** 猶予中の生ブロック件数。m_RetiredAllocationLock で保護する。 */
    u64 m_RetiredAllocationCount = 0u;

    /** 猶予中の要求バイト合計。m_RetiredAllocationLock で保護する。 */
    u64 m_RetiredRequestedBytes = 0u;

    /** 猶予回収閾値へ到達したら 1。公開操作終了後の回収要求に使う。 */
    TAtomic<u32> m_RetiredCollectionRequested{0u};

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
