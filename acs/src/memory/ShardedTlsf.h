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

/**
 * ロック競合を排した N-way シャード化 TLSF アロケータ (mimalloc の per-thread ヒープ相当)。
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
     * (最低 1MiB/shard)。commit_initial_bytes も同様に分配する。多重 Init はエラー。
     * @param total_reserve_bytes 全シャード合計の VM 予約サイズ。
     * @param commit_initial_bytes 全シャード合計の初期コミット量。
     * @param shard_count シャード数 (0 なら論理コア数から自動決定、最大 kMaxShards)。
     * @return 成功なら空の TResult、予約/初期化失敗ならエラー (途中まではロールバック)。
     */
    TResult<void> Init(usize total_reserve_bytes, usize commit_initial_bytes,
                       u32 shard_count = 0) noexcept;

    /** 全シャードをロックして VM 予約を解放し、未初期化状態へ戻す (再 Init 可能)。 */
    void Shutdown() noexcept;

    /**
     * size バイトを alignment 整列で確保する。
     *
     * @details
     * thread-cache 有効かつ小サイズなら lock-free マガジンから払い出し、ミス時はバッチ refill する。
     * それ以外は割り当てシャードへ向かい、満杯なら隣のシャードへフォールバックする。全シャード満杯で nullptr。
     * @param size 確保するバイト数。
     * @param alignment 要求アライメント。
     * @param loc 診断用の呼び出し位置。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;

    /**
     * ptr を解放する。
     *
     * @details
     * 所有シャードを O(N) の範囲判定で特定してから解放する。thread-cache 有効かつ対象サイズなら
     * マガジンへ push (満杯時は半分を実シャードへ償却返却)。所有不明ポインタは安全に無視する。
     * @param ptr このアロケータが払い出した領域 (nullptr 可)。
     */
    void  Free (void* ptr)                                  noexcept override;

    /**
     * ptr を new_size に再確保する。
     *
     * @details
     * まず所有シャード内で in-place / 同シャード移動を試み、不可なら別シャードへ移動する
     * (新規確保 + コピー + 旧解放、ロックは重ねずデッドロック回避)。失敗時は旧領域を保持する。
     * @param ptr 既存の確保 (nullptr なら新規確保)。
     * @param old_size 旧サイズ (移動時のコピー量決定に使う)。
     * @param new_size 新サイズ (0 なら解放)。
     * @param alignment 要求アライメント。
     * @param loc 診断用の呼び出し位置。
     * @return 再確保した領域 (失敗時や new_size==0 のとき nullptr)。
     */
    void* Realloc(void* ptr, usize old_size, usize new_size,
                  usize alignment, FSourceLoc loc) noexcept override;

    /**
     * 全シャードの使用バイト数の合計を返す。
     *
     * @details ロックを取らない近似値 (統計用)。
     * @return 全シャード合計の使用バイト数。
     */
    u64 BytesAllocated() const noexcept override;

    /**
     * 全シャードのピークバイト数の合計を返す。
     *
     * @details 各シャードのピーク合算であり厳密な「同時ピーク」ではないが、上限の目安として有用。
     * @return 全シャード合計のピークバイト数。
     */
    u64 PeakBytes()      const noexcept override;

    /**
     * 識別名を返す。
     *
     * @return 文字列 "ShardedTLSF"。
     */
    const char* Name()   const noexcept override { return "ShardedTLSF"; }

    /**
     * 現在のシャード数を返す。
     *
     * @return Init で決定したシャード数 (未初期化なら 0)。
     */
    u32  ShardCount() const noexcept { return m_ShardCount; }

    /**
     * thread-local マガジン (lock-free hot path) を有効化する。
     *
     * @details
     * 小サイズの alloc/free がスレッドローカル free-list ヒットで完結し、ロック/アトミックを
     * 一切踏まなくなる (refill/flush 時のみバッチで内部シャードを叩く)。TLS マガジンは 1 アロケータ
     * 専有なので、プロセス内で 1 つの FShardedTlsfAllocator にのみ有効化すること (通常は最ホットな
     * Default セグメント)。
     */
    void EnableThreadCache() noexcept;

    /**
     * thread-local マガジンが有効かを返す。
     *
     * @return 有効なら true。
     */
    bool ThreadCacheEnabled() const noexcept { return m_CacheEnabled; }

    /**
     * 現在の世代 (epoch) を返す。
     *
     * @details Init ごとに更新され、マガジンが旧世代 (再 Init で消えた VM) を掴むのを検出するのに使う。
     * @return 現在の epoch (未初期化なら 0)。
     */
    u64  Epoch() const noexcept { return m_Epoch; }

    /**
     * 全シャードをそれぞれロックして整合性を検証する (デバッグ/診断用)。
     *
     * @return 全シャードが整合していれば true、いずれか破損なら false。
     */
    bool ValidateHeap() noexcept;

private:
    /** 1 シャード分の TLSF アロケータと専用ロック。 */
    struct Shard {
        /** このシャードの TLSF アロケータ (専用 VM 予約を所有)。 */
        FTlsfAllocator alloc;

        /** このシャードへの確保/解放を直列化するロック。 */
        FMutex         lock;
    };

    /** シャードの固定配列 (動的確保を避ける、未使用分は Init しない)。 */
    Shard         m_Shards[kMaxShards];

    /** 実際に初期化したシャード数。 */
    u32           m_ShardCount   = 0;

    /** Init 済みか。 */
    bool          m_Inited       = false;

    /** thread-local マガジンを使うか。 */
    bool          m_CacheEnabled = false;

    /** 現在の世代 (Init ごとに更新、マガジンの世代検証用)。 */
    u64           m_Epoch        = 0;

    /** スレッドへシャードを配る round-robin カウンタ。 */
    TAtomic<u32>  m_NextShard {0};

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
     * @param p 判定対象のポインタ。
     * @return 所有シャード index (見つからなければ -1)。
     */
    int ShardIndexForPtr(const void* p) const noexcept;

    /**
     * ロックを取る素のシャード確保経路 (マガジンの裏側 / 非キャッシュ経路)。
     *
     * @details 割り当てシャードから試し、満杯なら隣のシャードへフォールバックする。
     * @param size 確保するバイト数。
     * @param alignment 要求アライメント。
     * @param loc 診断用の呼び出し位置。
     * @return 確保した領域 (全シャード満杯なら nullptr)。
     */
    void* AllocSharded(usize size, usize alignment, FSourceLoc loc) noexcept;

    /**
     * ロックを取る素のシャード解放経路 (マガジンの裏側 / 非キャッシュ経路)。
     *
     * @details 所有シャードを特定してそのシャードのみロックして解放する。
     * @param ptr 解放する領域 (nullptr 可)。
     */
    void  FreeSharded (void* ptr) noexcept;
};

} // namespace acs
