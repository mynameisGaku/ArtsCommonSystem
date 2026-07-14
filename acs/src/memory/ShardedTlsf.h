// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FShardedTlsfAllocator
// -----------------------------------------------------------------------------
// マルチスレッド確保のロック競合を排除するシャード化 TLSF。
//
// 設計:
//   ・N 個の独立した FTlsfAllocator シャード (各々 VM 予約 + 専用ロック) に分散。
//   ・確保: スレッドごとに割り当てたシャードへ (満杯なら隣のシャードへフォールバック)。
//           スレッド数 <= シャード数なら実質ロック競合ゼロ。
//   ・解放: ポインタのアドレスから所有シャードを O(N) で特定し、そのシャードのみロック。
//   ・既存の検証済み FTlsfAllocator をそのまま部品にする (安全性ガード / auto-grow /
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

namespace acs {

namespace memory_detail {
struct FShardedTlsfThreadCache;
}

/**
 * ロック競合を抑えた N-way シャード化 TLSF アロケータ (mimalloc の per-thread ヒープ相当)。
 *
 * @details
 * 独立した FTlsfAllocator シャード (各々 VM 予約 + 専用ロック) に分散する。確保はスレッドごとに
 * 割り当てたシャードへ向かい (満杯なら隣へフォールバック)、解放はポインタアドレスから所有シャードを
 * O(N) で特定してそのシャードのみロックする。スレッド数 <= シャード数なら実質ロック競合ゼロ。
 * EnableThreadCache で小サイズ用の thread-local マガジン (lock-free hot path) を有効化できる。
 * 各シャードは検証済み FTlsfAllocator なので安全ガード・auto-grow・in-place realloc を継承する。
 */
class FShardedTlsfAllocator final : public FAllocator {
public:
    /** シャード数の上限 (典型的なゲーム CPU を 8-way で分散)。 */
    static constexpr u32 kMaxShards = 8;

    /** 未初期化状態で構築する (使用前に Init を呼ぶこと)。 */
    FShardedTlsfAllocator() noexcept = default;

    /** 破棄する (Shutdown を呼んで全シャードの VM 予約を解放する)。 */
    ~FShardedTlsfAllocator() noexcept override;

    /** コピー禁止 (シャード群と VM 予約を単独所有するため)。 */
    FShardedTlsfAllocator(const FShardedTlsfAllocator&) = delete;

    /** コピー代入も禁止。 */
    FShardedTlsfAllocator& operator=(const FShardedTlsfAllocator&) = delete;

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
     * Shutdown を再度呼ぶと解放を再試行する。
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
     * まず所有シャード内で in-place / 同シャード移動を試み、不可なら別シャードへ移動する
     * (新規確保 + コピー + 旧解放、ロックは重ねずデッドロック回避)。失敗時は旧領域を保持する。
     * new_size==0 は所有権を検証してから解放し、不正ポインタの場合は pointer 自身を返して未解放を通知する。
     * @param Pointer 既存の確保 (nullptr なら新規確保)。
     * @param OldSize 旧サイズ (移動時のコピー量決定に使う)。
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
    u64 AllocationCount() const noexcept override
    {
        return m_AllocationCount.Load(EMemoryOrder::Acquire);
    }

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
    u32 ShardCount() const noexcept
    {
        return m_ShardCount;
    }

    /**
     * thread-local マガジンを有効化する。
     *
     * @details
     * 小サイズ Alloc のキャッシュヒットは lock-free。Free は 1 shard をロックして正規の確保開始位置と
     * 状態を検証してから、payload 外の固定長ポインタ配列へ格納する。同一スレッドが別の
     * FShardedTlsfAllocator を使う場合は、切替時に旧マガジンを旧 owner へ返してから新 owner へ
     * 貼り直す。スレッド終了時も、生存中かつ同一世代の owner へ残存ブロックを返す。
     */
    void EnableThreadCache() noexcept;

    /** 現在のスレッドが保持する小サイズキャッシュを、このアロケータへ返す。 */
    void FlushCurrentThreadCache() noexcept;

    /**
     * thread-local マガジンが有効かを返す。
     *
     * @return 有効なら true。
     */
    bool ThreadCacheEnabled() const noexcept
    {
        return m_CacheEnabled;
    }

    /**
     * 現在の世代 (epoch) を返す。
     *
     * @details Init ごとに更新され、マガジンが旧世代 (再 Init で消えた VM) を掴むのを検出するのに使う。
     * @return 現在の epoch (未初期化なら 0)。
     */
    u64 Epoch() const noexcept
    {
        return m_Epoch;
    }

    /**
     * 全シャードをそれぞれロックして整合性を検証する (デバッグ/診断用)。
     *
     * @return 全シャードが整合していれば true、いずれか破損なら false。
     */
    bool ValidateHeap() noexcept;

private:
    /** thread-local マガジン実装だけに寿命レジストリへのアクセスを許可する。 */
    friend struct memory_detail::FShardedTlsfThreadCache;

    /** 1 シャード分の TLSF アロケータと専用ロック。 */
    struct Shard {
        /** このシャードの TLSF アロケータ (専用 VM 予約を所有)。 */
        FTlsfAllocator alloc;

        /** このシャードへの確保/解放を直列化するロック。 */
        mutable FMutex lock;
    };

    /** シャードの固定配列 (動的確保を避ける、未使用分は Init しない)。 */
    Shard m_Shards[kMaxShards];

    /** 実際に初期化したシャード数。 */
    u32 m_ShardCount = 0;

    /** Init 済みか。 */
    bool m_Inited = false;

    /** thread-local マガジンを使うか。 */
    bool m_CacheEnabled = false;

    /** 現在の世代 (Init ごとに更新、マガジンの世代検証用)。 */
    u64 m_Epoch = 0;

    /** スレッドへシャードを配る round-robin カウンタ。 */
    TAtomic<u32> m_NextShard{0};

    /** クライアントが現在保持している確保の件数。内部キャッシュ用ブロックは除外する。 */
    TAtomic<u64> m_AllocationCount{0};

    /** 生存中アロケータの侵入リストにおける次要素。動的確保を避けるため本体に保持する。 */
    FShardedTlsfAllocator* m_ThreadCacheRegistryNext = nullptr;

    /** thread-local マガジンの寿命レジストリへ登録済みなら true。 */
    bool m_ThreadCacheLifetimeRegistered = false;

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

} // namespace acs
