// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — システムアロケータ（Win32 プロセスヒープ）
// -----------------------------------------------------------------------------
// HeapAlloc / HeapFree を使った汎用アロケータ。プロセスヒープは OS が
// 内部でロックを取って直列化するため、スレッドセーフ（HEAP_NO_SERIALIZE
// は意図的に未指定）。
//
// アライメント:
//   HeapAlloc 自体は MEMORY_ALLOCATION_ALIGNMENT (x64 で 16B) しか保証しない。
//   それを超えるアライメントが必要な場合は、内部で「アラインド確保 +
//   ヘッダで元ポインタを覚える」方式（AlignedAlloc）を使う。
//
// 性能注意:
//   HeapAlloc は数百 ns のオーバーヘッドがある。フレーム内で何千回も呼ぶ
//   ようなホットパスでは、専用プールやアリーナを別途用意すべき。
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

/** プロセス内に存在する FSystemAllocator 全体の統計スナップショット。 */
struct FSystemAllocatorProcessStatistics {
    /** 現在生存し、侵入レジストリへ登録されているアロケータ数。 */
    u64 live_allocator_count = 0;

    /** 生存中アロケータが現在保持する実ヒープ確保量の合計。 */
    u64 outstanding_bytes = 0;

    /** 生存中アロケータが現在保持する未解放確保件数の合計。 */
    u64 outstanding_allocation_count = 0;

    /** 未解放確保を残したまま破棄されたアロケータ数のプロセス累積。 */
    u64 destroyed_with_live_allocations_count = 0;

    /** 破棄時に残っていた実ヒープ確保量のプロセス累積。 */
    u64 destroyed_outstanding_bytes = 0;

    /** 破棄時に残っていた未解放確保件数のプロセス累積。 */
    u64 destroyed_outstanding_allocation_count = 0;

    /** 生存中アロケータに未解放確保があるかを返す。 */
    constexpr bool HasOutstandingAllocations() const noexcept
    {
        return outstanding_bytes != 0 || outstanding_allocation_count != 0;
    }

    /** 未解放確保を残したアロケータ破棄を過去に検出したかを返す。 */
    constexpr bool HasDestroyedWithLiveAllocations() const noexcept
    {
        return destroyed_with_live_allocations_count != 0;
    }
};

/**
 * Win32 プロセスヒープ (HeapAlloc/HeapFree) を使う汎用スレッドセーフアロケータ。
 *
 * @details
 * プロセスヒープは OS が内部でロックを取って直列化するためスレッドセーフ
 * (HEAP_NO_SERIALIZE は意図的に未指定)。HeapAlloc は 16B しか整列を保証しないため、
 * それを超えるアライメント要求は内部で「余裕を持って確保 + ヘッダに元ポインタを退避」する
 * 方式で満たす。確保量・ピークはアトミックに集計する。HeapAlloc は数百 ns のコストがあるため、
 * フレーム内で何千回も呼ぶホットパスでは専用プール/アリーナを使うこと。
 * アロケータは自身が払い出した全確保より長く生存しなければならない。異なる確保に対する操作は
 * 並行実行できるが、同じ確保に対する Free/Realloc は呼び出し側で同期すること。
 */
class FSystemAllocator final : public FAllocator {
public:
    /** 既定構築し、プロセス内の侵入レジストリへ登録する。 */
    FSystemAllocator() noexcept;

    /**
     * 侵入レジストリから登録解除して破棄する。
     *
     * @details 未解放確保があれば FLogger に依存しない機械可読ログをデバッグ出力と標準エラーへ出す。
     * 未解放領域は自動回収しないため、破棄後にそのポインタを別のアロケータへ渡してはならない。
     */
    ~FSystemAllocator() noexcept override;

    /** コピー禁止 (侵入レジストリの単一ノードとして管理するため)。 */
    FSystemAllocator(const FSystemAllocator&) = delete;

    /** コピー代入も禁止。 */
    FSystemAllocator& operator=(const FSystemAllocator&) = delete;

    /** ムーブ禁止 (登録済みノードのアドレスを固定するため)。 */
    FSystemAllocator(FSystemAllocator&&) = delete;

    /** ムーブ代入も禁止。 */
    FSystemAllocator& operator=(FSystemAllocator&&) = delete;

    /**
     * プロセス内に存在する全 FSystemAllocator の統計を取得する。
     *
     * @details 動的確保を行わず、侵入レジストリを共有ロックして各アトミック統計を集計する。
     * 並行して確保・解放している間のバイト数と件数は弱整合スナップショットとなるため、
     * リーク有無の確定判定には全ワーカを停止した静止点で呼び出すこと。
     * @return 現在値と、未解放を伴う過去の破棄の累積値。
     */
    static FSystemAllocatorProcessStatistics CaptureProcessStatistics() noexcept;

    /**
     * Size バイトを Alignment 整列で確保する。
     *
     * @details 16B 超の Alignment はヘッダ退避方式で満たす。Size==0 や確保失敗時は nullptr。
     * @param Size 確保するバイト数。
     * @param Alignment 要求アライメント (2 のべき乗)。
     * @param Location 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * Pointer を解放する。
     *
     * @details ヘッダから所有者・構築世代・元ポインタを検証して HeapFree に渡す。別インスタンスや
     * 過去世代のポインタ、破損ヘッダは解放せず、機械可読エラーを出す。nullptr は no-op。
     * @param Pointer このインスタンスの Alloc で得た正確な生存ポインタ (nullptr 可)。
     */
    void Free(void* Pointer) noexcept override;

    /**
     * Pointer を NewSize に再確保する。
     *
     * @details
     * HeapReAlloc はヘッダ退避方式と相性が悪いため、新規確保・コピー・旧領域解放を行う。
     * コピー量にはヘッダへ記録した要求サイズを使い、誤った OldSize による範囲外読み取りを
     * 防ぐ。別インスタンスのポインタは拒否し、旧領域を保持する。NewSize==0 の解放が
     * 拒否または失敗した場合だけは、所有権喪失を防ぐため元の Pointer を返す。
     * @param Pointer このインスタンスの Alloc で得た正確な生存ポインタ (nullptr なら新規確保)。
     * @param OldSize 呼び出し側が認識している旧サイズ。本実装は安全のためコピー量に使用しない。
     * @param NewSize 新サイズ (0 なら解放)。
     * @param Alignment 新領域のアライメント。
     * @param Location 診断用の呼び出し位置。
     * @return NewSize が非 0 なら新しい確保 (失敗時 nullptr、旧領域は保持)。NewSize が 0 なら
     * 解放成功時 nullptr、解放拒否または失敗時は所有権を保持する元の Pointer。
     */
    void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * 現在の総割当バイト数を返す。
     *
     * @return 生存中の確保の合計バイト数 (ヘッダ・アライメント余裕込みの実ヒープ確保量)。
     */
    u64 BytesAllocated() const noexcept override
    {
        return m_Bytes.Load(EMemoryOrder::Acquire);
    }

    /**
     * 現在生存している確保の件数を返す。
     *
     * @return Free されていない確保の件数。
     */
    u64 AllocationCount() const noexcept override
    {
        return m_AllocationCount.Load(EMemoryOrder::Acquire);
    }

    /**
     * 過去のピーク割当バイト数を返す。
     *
     * @return これまでの最大割当バイト数。
     */
    u64 PeakBytes() const noexcept override
    {
        return m_Peak.Load(EMemoryOrder::Acquire);
    }

    /** このインスタンスの構築寿命を識別する非0世代を返す。 */
    u64 LifetimeGeneration() const noexcept override
    {
        return m_Generation;
    }

    /**
     * 識別名を返す。
     *
     * @return 文字列 "System"。
     */
    const char* Name() const noexcept override
    {
        return "System";
    }

private:
    /** アドレス再利用後の別インスタンスを識別するプロセス内構築世代。 */
    u64 m_Generation = 0;

    /** 現在の総割当バイト数 (実ヒープ確保量、アトミック集計)。 */
    mutable TAtomic<u64> m_Bytes{0};

    /** 過去ピークの割当バイト数 (CAS で更新)。 */
    mutable TAtomic<u64> m_Peak{0};

    /** 現在生存している確保の件数 (アトミック集計)。 */
    mutable TAtomic<u64> m_AllocationCount{0};

    /** プロセス内侵入レジストリの次ノード。動的確保を避けるため本体に保持する。 */
    FSystemAllocator* m_RegistryNext = nullptr;

    /** 侵入レジストリへ登録済みなら true。 */
    bool m_RegistryRegistered = false;
};

} // namespace acs
