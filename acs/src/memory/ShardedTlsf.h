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

class FShardedTlsfAllocator final : public FAllocator {
public:
    static constexpr u32 kMaxShards = 8;   // 典型的なゲーム CPU (8〜16 コア) を 8-way で分散

    FShardedTlsfAllocator() noexcept = default;
    ~FShardedTlsfAllocator() noexcept override;

    FShardedTlsfAllocator(const FShardedTlsfAllocator&) = delete;
    FShardedTlsfAllocator& operator=(const FShardedTlsfAllocator&) = delete;

    // total_reserve_bytes をシャード数で分割し、各シャードに VM 予約 (reserve/N) を割り当てる。
    // commit_initial_bytes は全体の初期コミット量 (各シャードに分配)。
    // shard_count=0 なら CPU 論理コア数を見て自動決定 (最大 kMaxShards)。
    TResult<void> Init(usize total_reserve_bytes, usize commit_initial_bytes,
                       u32 shard_count = 0) noexcept;
    void Shutdown() noexcept;

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;
    void  Free (void* ptr)                                  noexcept override;
    void* Realloc(void* ptr, usize old_size, usize new_size,
                  usize alignment, FSourceLoc loc) noexcept override;

    u64 BytesAllocated() const noexcept override;
    u64 PeakBytes()      const noexcept override;
    const char* Name()   const noexcept override { return "ShardedTLSF"; }

    u32  ShardCount() const noexcept { return m_ShardCount; }

    // 全シャードを各々ロックして整合検証する (デバッグ/診断用)。
    bool ValidateHeap() noexcept;

private:
    struct Shard {
        FTlsfAllocator alloc;
        FMutex         lock;
    };

    // FTlsfAllocator は m_Blocks[26][32] を持つため 1 個 ~8KB。kMaxShards 個を固定配列で
    // 持つ (~64KB)。動的確保を避けて初期化を単純化する。未使用シャードは Init しない。
    Shard         m_Shards[kMaxShards];
    u32           m_ShardCount = 0;
    bool          m_Inited     = false;
    TAtomic<u32>  m_NextShard {0};   // スレッドへシャードを配る round-robin カウンタ

    int ShardIndexForThread() noexcept;                  // 呼び出しスレッドの割り当て (TLS キャッシュ)
    int ShardIndexForPtr(const void* p) const noexcept;  // ptr の所有シャード (見つからねば -1)
};

} // namespace acs
