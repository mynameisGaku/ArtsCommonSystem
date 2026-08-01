// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — CShardedTlsfAllocator
// -----------------------------------------------------------------------------
// マルチスレッド確保のロック競合を排除するシャード化 TLSF。
//
// 設計:
//   ・N 個の独立した CTlsfAllocator シャード (各々 VM 予約 + 専用ロック) に分散。
//   ・確保: スレッドごとに割り当てたシャードへ (満杯なら隣のシャードへフォールバック)。
//           スレッド数 <= シャード数なら実質ロック競合ゼロ。
//   ・解放: ポインタのアドレスから所有シャードを O(N) で特定し、そのシャードのみロック。
//   ・既存の検証済み CTlsfAllocator をそのまま部品にする (安全性ガード / auto-grow /
//     in-place realloc を全シャードで継承)。
//
// これは mimalloc の per-thread ヒープに相当する利点 (中央ロックの排除) を、ACS の
// VM 予約ベース・自前実装で実現するもの。RE ENGINE 系の VM アロケータ路線に沿う。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "memory/Tlsf.h"
#include "threading/Mutex.h"
#include "threading/Atomic.h"
#include "threading/ConditionVar.h"

namespace acs {

namespace memory_detail {
struct FShardedTlsfThreadCache;
}

/**
 * ロック競合を抑えた N-way シャード化 TLSF アロケータ (mimalloc の per-thread ヒープ相当)。
 *
 * @details
 * 独立した CTlsfAllocator シャード (各々 VM 予約 + 専用ロック) に分散する。確保はスレッドごとに
 * 割り当てたシャードへ向かい (満杯なら隣へフォールバック)、解放はポインタアドレスから所有シャードを
 * O(N) で特定してそのシャードのみロックする。スレッド数 <= シャード数なら実質ロック競合ゼロ。
 * EnableThreadCache で小サイズ用の thread-local マガジン (lock-free hot path) を有効化できる。
 * 各シャードは検証済み CTlsfAllocator なので安全ガード・auto-grow・in-place realloc を継承する。
 */
class CShardedTlsfAllocator final : public IAllocator {
public:
    /** シャード数の上限 (典型的なゲーム CPU を 8-way で分散)。 */
    static constexpr u32 kMaxShards = 8;

    /** 未初期化状態で構築する (使用前に Init を呼ぶこと)。 */
    CShardedTlsfAllocator() noexcept = default;

    /** 破棄する (Shutdown を呼んで全シャードの VM 予約を解放する)。 */
    ~CShardedTlsfAllocator() noexcept override;

    /** コピー禁止 (シャード群と VM 予約を単独所有するため)。 */
    CShardedTlsfAllocator(const CShardedTlsfAllocator&) = delete;

    /** コピー代入も禁止。 */
    CShardedTlsfAllocator& operator=(const CShardedTlsfAllocator&) = delete;

    /**
     * シャードを構築し、各シャードに VM 予約と初期コミットを割り当てて初期化する。
     *
     * @details
     * total_reserve_bytes をシャード数で割り、粒度整列して各シャードに reserve/N を割り当てる
     * (最低 1MiB/shard)。commit_initial_bytes も同様に分配する。整列加算のオーバーフローと多重 Init は
     * VM 予約前にエラーとして拒否し、未初期化状態を維持する。
     * @param TotalReserveBytes 全シャード合計の VM 予約サイズ。
     * @param CommitInitialBytes 全シャード合計の初期コミット量。
     * @param RequestedShardCount シャード数 (0 なら論理コア数から自動決定、最大 kMaxShards)。
     * @return 成功なら空の TResult、予約/初期化失敗ならエラー (途中まではロールバック)。
     */
    TResult<void> Init(usize TotalReserveBytes, usize CommitInitialBytes, u32 RequestedShardCount = 0) noexcept;

    /**
     * 全シャードをロックして VM 予約を解放し、未初期化状態へ戻す。
     *
     * @details VM 解放に失敗したシャードは保持し、機械可読ログへ記録する。再 Init の前に
     * Shutdown を再度呼ぶと解放を再試行する。Shutdown 開始後は新しい公開操作を拒否し、
     * すでに開始済みの公開操作が完了してから VM を解放する。
     */
    void Shutdown() noexcept;

    /**
     * size バイトを alignment 整列で確保する。
     *
     * @details
     * thread-cache 有効かつ小サイズなら lock-free マガジンから払い出し、ミス時はバッチ refill する。
     * それ以外は割り当てシャードへ向かい、満杯なら隣のシャードへフォールバックする。全シャード満杯で nullptr。
     * @param Size 確保するバイト数。
     * @param Alignment 要求アライメント。
     * @param Location 診断用の呼び出し位置。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * pointer を解放する。
     *
     * @details
     * 所有シャードを O(N) の範囲判定で特定し、そのシャードをロックして正規の確保開始位置と状態を
     * 検証してから解放する。thread-cache 有効かつ対象サイズならマガジンへ push (満杯時は半分を
     * 実シャードへ償却返却)。所有不明ポインタは安全に無視する。
     * @param Pointer このアロケータが払い出した領域 (nullptr 可)。
     */
    void Free(void* Pointer) noexcept override;

    /**
     * pointer を new_size に再確保する。
     *
     * @details
     * まず所有シャード内で in-place / 同シャード移動を試み、不可なら別シャードへ移動する。
     * 新規確保後に旧シャードを再ロックし、所有状態の再確認・実 payload 容量までのコピー・
     * 旧解放を同じロック区間で行う。シャードロックは重ねない。失敗時は旧領域を保持する。
     * new_size==0 は所有権を検証してから解放し、不正ポインタの場合は pointer 自身を返して未解放を通知する。
     * @param Pointer 既存の確保 (nullptr なら新規確保)。
     * @param OldSize 呼び出し側が認識する旧サイズ。コピー量は実 payload 容量以下へ制限する。
     * @param NewSize 新サイズ (0 なら解放)。
     * @param Alignment 要求アライメント。
     * @param Location 診断用の呼び出し位置。
     * @return 再確保した領域。失敗時は nullptr。new_size==0 は解放成功時 nullptr、不正ポインタなら pointer。
     */
    void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * 全シャードの使用バイト数の合計を返す。
     *
     * @details 各シャードを順番にロックしてデータ競合なく読み取る。
     * @return 全シャード合計の使用バイト数。
     */
    u64 BytesAllocated() const noexcept override;

    /**
     * 現在クライアントが保持している確保の件数を返す。
     *
     * @details thread-local マガジン内の再利用待ちブロックは未解放件数に含めない。
     * @return Free されていない公開 Alloc の成功件数。
     */
    u64 AllocationCount() const noexcept override;

    /**
     * 全シャードのピークバイト数の合計を返す。
     *
     * @details 各シャードのピーク合算であり厳密な「同時ピーク」ではないが、上限の目安として有用。
     * @return 全シャード合計のピークバイト数。
     */
    u64 PeakBytes() const noexcept override;

    /**
     * 識別名を返す。
     *
     * @return 文字列 "ShardedTLSF"。
     */
    const char* Name() const noexcept override
    {
        return "ShardedTLSF";
    }

    /**
     * 現在のシャード数を返す。
     *
     * @return Init で決定したシャード数 (未初期化なら 0)。
     */
    u32 ShardCount() const noexcept;

    /**
     * thread-local マガジンを有効化する。
     *
     * @details
     * 小サイズ Alloc のキャッシュヒットは lock-free。Free は 1 shard をロックして正規の確保開始位置と
     * 状態を検証してから、payload 外の固定長ポインタ配列へ格納する。同一スレッドが別の
     * CShardedTlsfAllocator を使う場合は、切替時に旧マガジンを旧 owner へ返してから新 owner へ
     * 貼り直す。スレッド終了時も、生存中かつ同一世代の owner へ残存ブロックを返す。
     */
    void EnableThreadCache() noexcept;

    /**
     * 現在のスレッドが保持する小サイズキャッシュを、このアロケータへ返す。
     *
     * @details Shutdown が受付を閉じた後の呼び出しは何もせずに戻る。
     */
    void FlushCurrentThreadCache() noexcept;

    /**
     * thread-local マガジンが有効かを返す。
     *
     * @return 有効なら true。
     */
    bool ThreadCacheEnabled() const noexcept;

    /**
     * 現在の世代 (epoch) を返す。
     *
     * @details Init ごとに更新され、マガジンが旧世代 (再 Init で消えた VM) を掴むのを検出するのに使う。
     * @return 現在の epoch (未初期化なら 0)。
     */
    u64 Epoch() const noexcept;

    /**
     * 全シャードをそれぞれロックして整合性を検証する (デバッグ/診断用)。
     *
     * @return 全シャードが整合していれば true、いずれか破損なら false。
     */
    bool ValidateHeap() noexcept;

private:
    /** thread-local マガジン実装だけに寿命レジストリへのアクセスを許可する。 */
    friend struct memory_detail::FShardedTlsfThreadCache;

    /** 公開操作の入場登録をスコープ終了時に必ず解除する。 */
    class FLifecycleOperation;

    /** ライフサイクルゲートの最上位ビット。立っている間だけ新規公開操作を受け付ける。 */
    static constexpr u64 kLifecycleAcceptingBit = u64(1) << 63u;

    /** ライフサイクルゲート下位 32 bit の実行中操作件数部分。 */
    static constexpr u64 kLifecycleOperationCountMask = 0xFFFFFFFFull;

    /** 再初期化を古い入場 CAS と区別するライフサイクル世代部分。 */
    static constexpr u64 kLifecycleGenerationMask = ~(kLifecycleAcceptingBit | kLifecycleOperationCountMask);

    /** ライフサイクル世代を 1 進める値。 */
    static constexpr u64 kLifecycleGenerationIncrement = u64(1) << 32u;

    /** 1 シャード分の TLSF アロケータと専用ロック。 */
    struct FShard {
        /** このシャードの TLSF アロケータ (専用 VM 予約を所有)。 */
        CTlsfAllocator alloc;

        /** このシャードへの確保/解放を直列化するロック。 */
        mutable FMutex lock;
    };

    /** シャードの固定配列 (動的確保を避ける、未使用分は Init しない)。 */
    FShard m_Shards[kMaxShards];

    /** 実際に初期化したシャード数。 */
    u32 m_ShardCount = 0;

    /** Init 済みか。 */
    bool m_Inited = false;

    /** thread-local マガジンを使うか。Enable と公開操作の並行読み書きを許可する。 */
    TAtomic<u32> m_CacheEnabled{0u};

    /** 現在の世代 (Init ごとに更新、マガジンの世代検証用)。 */
    u64 m_Epoch = 0;

    /** スレッドへシャードを配る round-robin カウンタ。 */
    TAtomic<u32> m_NextShard{0};

    /** クライアントが現在保持している確保の件数。内部キャッシュ用ブロックは除外する。 */
    TAtomic<u64> m_AllocationCount{0};

    /** 受付ビット・世代・実行中公開操作件数を一体で更新するライフサイクルゲート。 */
    mutable TAtomic<u64> m_LifecycleGate{0u};

    /** Init / Shutdown と thread-cache 設定変更を直列化する。 */
    mutable FMutex m_LifecycleControlLock;

    /** 実行中公開操作の完了条件を確認する間だけ保持する待機専用ロック。 */
    mutable FMutex m_LifecycleDrainLock;

    /** Shutdown が最後の実行中公開操作の完了通知を待つ条件変数。 */
    mutable FConditionVar m_LifecycleDrainedCondition;

    /** 生存中アロケータの侵入リストにおける次要素。動的確保を避けるため本体に保持する。 */
    CShardedTlsfAllocator* m_ThreadCacheRegistryNext = nullptr;

    /** thread-local マガジンの寿命レジストリへ登録済みなら true。 */
    bool m_ThreadCacheLifetimeRegistered = false;

    /** Running 中の公開操作として入場できれば true を返す。 */
    bool TryBeginLifecycleOperation() const noexcept;

    /** 入場済み公開操作の完了を記録し、必要なら Shutdown を起こす。 */
    void EndLifecycleOperation() const noexcept;

    /** 新規入場を閉じ、開始済み公開操作がすべて完了するまで待つ。制御ロック保持中に呼ぶ。 */
    void CloseLifecycleGateAndWait() noexcept;

    /** 完全初期化後に新規公開操作の受付を開始する。制御ロック保持中に呼ぶ。 */
    void OpenLifecycleGate() noexcept;

    /** ゲート停止中に全シャードを解放する。全予約を解放できた場合は true。 */
    bool ResetShardsAfterLifecycleClose() noexcept;

    /** 入場済み Alloc の本体。Shutdown が受付を閉じた後も開始済み操作を完了させる。 */
    void* AllocateAfterLifecycleAdmission(usize Size, usize Alignment, FSourceLoc Location) noexcept;

    /** 入場済み Free の本体。 */
    void FreeAfterLifecycleAdmission(void* Pointer) noexcept;

    /** 入場済み Realloc の本体。内部の確保・解放ではライフサイクルへ重複入場しない。 */
    void* ReallocateAfterLifecycleAdmission(void* Pointer, usize OldSize, usize NewSize, usize Alignment,
                                            FSourceLoc Location) noexcept;

    /**
     * 現在スレッドのマガジンを返す本体。
     *
     * @details 公開操作として入場済みか、制御ロック保持中かつ受付停止・drain 完了後にだけ呼ぶ。
     */
    void FlushCurrentThreadCacheWhileLifecycleIsStable() noexcept;

    /** 現在世代を thread-local マガジンの寿命レジストリへ登録する。 */
    void RegisterThreadCacheLifetime() noexcept;

    /** 寿命レジストリから外し、以後の遅延返却が VM に触れないようにする。 */
    void UnregisterThreadCacheLifetime() noexcept;

    /** owner と世代がまだ生存中の場合だけ、指定マガジンの全ブロックを返却する。 */
    static void ReturnThreadCacheIfLifetimeIsActive(memory_detail::FShardedTlsfThreadCache& Cache) noexcept;

    /** 寿命レジストリのロック保持中に、指定マガジンを実シャードへ返却する。 */
    void ReturnThreadCacheBlocks(memory_detail::FShardedTlsfThreadCache& Cache) noexcept;

    /**
     * 呼び出しスレッドに割り当てるシャード index を返す。
     *
     * @details 初回に round-robin で割り当て TLS にキャッシュする。
     * @return シャード index ([0, m_ShardCount))。
     */
    int ShardIndexForThread() noexcept;

    /**
     * ptr を所有するシャードの index を返す。
     *
     * @details 各シャードの予約レンジ判定 (ロック不要) を O(N) で走査する。
     * @param Pointer 判定対象のポインタ。
     * @return 所有シャード index (見つからなければ -1)。
     */
    int ShardIndexForPtr(const void* Pointer) const noexcept;

    /**
     * ロックを取る素のシャード確保経路 (マガジンの裏側 / 非キャッシュ経路)。
     *
     * @details 割り当てシャードから試し、満杯なら隣のシャードへフォールバックする。
     * @param Size 確保するバイト数。
     * @param Alignment 要求アライメント。
     * @param Location 診断用の呼び出し位置。
     * @return 確保した領域 (全シャード満杯なら nullptr)。
     */
    void* AllocSharded(usize Size, usize Alignment, FSourceLoc Location) noexcept;

    /**
     * ロックを取る素のシャード解放経路 (マガジンの裏側 / 非キャッシュ経路)。
     *
     * @details 所有シャードを特定してそのシャードのみロックして解放する。
     * @param Pointer 解放する領域 (nullptr 可)。
     * @return 実際に解放できた場合は true、不正または重複解放なら false。
     */
    bool FreeSharded(void* Pointer) noexcept;
};

/** 移行期間中に旧名を受け付ける互換別名。 */
using FShardedTlsfAllocator = CShardedTlsfAllocator;

} // namespace acs
