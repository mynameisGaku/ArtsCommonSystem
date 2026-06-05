// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FShardedTlsfAllocator 実装
// =============================================================================
#include "memory/ShardedTlsf.h"
#include "memory/VirtualMemory.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"
#include "foundation/Move.h"
#include "foundation/Assert.h"
#include "foundation/Compiler.h"
#include "foundation/Platform.h"   // GetSystemInfo

namespace acs {

namespace {

/**
 * 論理コア数を返す (シャード数の自動決定 / clamp 用)。
 *
 * @return 論理プロセッサ数 (0 を返さないよう最低 1)。
 */
u32 DetectLogicalCores() noexcept {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const u32 n = static_cast<u32>(si.dwNumberOfProcessors);
    return n > 0u ? n : 1u;
}

/** スレッドに割り当てるシャード index。全シャード化アロケータで共有する分散ヒント (各々 m_ShardCount で剰余)。 */
ACS_THREAD_LOCAL int t_assigned_shard = -1;

/** thread-local マガジンのサイズクラス数 (block_size 24..520 をカバー)。 */
constexpr u32   kCacheClasses  = 32;

/** 1 サイズクラスあたりの最大キャッシュ数。 */
constexpr u32   kBucketCap     = 64;

/** refill 1 回でまとめて確保するブロック数。 */
constexpr u32   kRefillBatch   = 32;

/** キャッシュ対象の最小 block_size (TLSF 量子化の最小ブロック)。 */
constexpr usize kCacheMinBlock = 24;

/** キャッシュ対象の最大 block_size (= 24 + 16×(kCacheClasses-1) = 520)。 */
constexpr usize kCacheMaxBlock = kCacheMinBlock + 16u * (kCacheClasses - 1);

/**
 * block_size をキャッシュのサイズクラスに変換する。
 *
 * @details TLSF の block_size は 24,40,56,... (=24+16k) なので k をクラスにする。範囲外や非量子サイズは対象外。
 * @param bs TLSF の block_size。
 * @return サイズクラス index (対象外は -1)。
 */
ACS_FORCEINLINE int CacheClassOfBlock(usize bs) noexcept {
    if (bs < kCacheMinBlock || bs > kCacheMaxBlock) return -1;
    const usize d = bs - kCacheMinBlock;
    if ((d & 15u) != 0u) return -1;
    return static_cast<int>(d >> 4);
}

/**
 * 確保要求サイズから TLSF が割り当てる block_size を求める (AdjustRequestSize と同一式)。
 *
 * @param size 確保要求サイズ。
 * @return 対応する block_size。
 */
ACS_FORCEINLINE usize CacheBlockSizeForRequest(usize size) noexcept {
    usize bs = ((size + 15u) & ~usize(15)) + 8u;
    if (bs < kCacheMinBlock) bs = kCacheMinBlock;
    return bs;
}

/** スレッドローカルのマガジン本体 (owner+epoch で 1 アロケータに専有、別アロケータ/再 Init 検出で破棄)。 */
struct FThreadCache {
    /** このマガジンを専有するアロケータ (世代検証用)。 */
    const void* owner = nullptr;

    /** owner の世代 (再 Init で旧世代を検出するため)。 */
    u64         epoch = 0;

    /** サイズクラスごとの free-list 先頭 (単方向リンク)。 */
    void*       head[kCacheClasses]  = {};

    /** サイズクラスごとのキャッシュ済みブロック数。 */
    u32         count[kCacheClasses] = {};
};

/** 呼び出しスレッドの thread-local マガジン。 */
ACS_THREAD_LOCAL FThreadCache t_tcache;

/** Init ごとに新しい世代を配るカウンタ (0 は無効世代)。 */
TAtomic<u64> g_epoch_counter{0};

} // namespace

FShardedTlsfAllocator::~FShardedTlsfAllocator() noexcept { Shutdown(); }

TResult<void> FShardedTlsfAllocator::Init(usize total_reserve_bytes, usize commit_initial_bytes,
                                          u32 shard_count) noexcept {
    if (m_Inited) return ACS_ERR(Memory, 50, "FShardedTlsfAllocator: already initialized");

    u32 n = (shard_count != 0u) ? shard_count : DetectLogicalCores();
    if (n > kMaxShards) n = kMaxShards;
    if (n == 0u) n = 1u;

    const usize gran = VmAllocGranularity();
    const usize page = VmPageSize();

    // 各シャードの予約 / 初期コミットを算出 (粒度整列)。
    usize per_reserve = total_reserve_bytes / n;
    per_reserve = (per_reserve + gran - 1u) & ~(gran - 1u);            // 64KiB 整列
    if (per_reserve < 1u * 1024u * 1024u) per_reserve = 1u * 1024u * 1024u;  // 最低 1MiB/shard

    usize per_commit = commit_initial_bytes / n;
    per_commit = (per_commit + page - 1u) & ~(page - 1u);             // ページ整列
    const usize kMinCommit = 64u * 1024u;
    if (per_commit < kMinCommit)  per_commit = kMinCommit;
    if (per_commit > per_reserve) per_commit = per_reserve;

    for (u32 i = 0; i < n; ++i) {
        auto rr = VmReservation::Reserve(per_reserve);
        if (rr.IsErr()) { Shutdown(); return Err<void>(rr.Error()); }
        auto ir = m_Shards[i].alloc.InitWithReservation(Move(rr.Value()), per_commit);
        if (ir.IsErr()) { Shutdown(); return ir; }
    }
    m_ShardCount = n;
    m_Epoch      = g_epoch_counter.FetchAdd(1) + 1;   // 新しい世代 (再 Init でマガジンを無効化)
    m_Inited     = true;
    return Ok();
}

void FShardedTlsfAllocator::EnableThreadCache() noexcept {
    m_CacheEnabled = true;
}

void FShardedTlsfAllocator::Shutdown() noexcept {
    for (u32 i = 0; i < m_ShardCount; ++i) {
        FScopedLock lk(m_Shards[i].lock);
        m_Shards[i].alloc.Reset();   // 予約解放 + 状態リセット (再 Init 可能に)
    }
    m_ShardCount = 0;
    m_Inited = false;
    m_NextShard.Store(0, EMemoryOrder::Relaxed);
}

int FShardedTlsfAllocator::ShardIndexForThread() noexcept {
    if (t_assigned_shard < 0) {
        t_assigned_shard = static_cast<int>(m_NextShard.FetchAdd(1));
    }
    return t_assigned_shard % static_cast<int>(m_ShardCount);
}

int FShardedTlsfAllocator::ShardIndexForPtr(const void* p) const noexcept {
    // ContainsPtr は予約レンジの O(1) 判定 (ロック不要 — 予約 Base/Capacity は Init 後不変)。
    for (u32 i = 0; i < m_ShardCount; ++i) {
        if (m_Shards[i].alloc.ContainsPtr(p)) return static_cast<int>(i);
    }
    return -1;
}

void* FShardedTlsfAllocator::AllocSharded(usize size, usize alignment, FSourceLoc loc) noexcept {
    if (!m_Inited || size == 0) return nullptr;
    const int start = ShardIndexForThread();
    // 自分のシャードから試し、満杯なら隣へフォールバック (偏り/枯渇でも全体予約まで使える)。
    for (u32 i = 0; i < m_ShardCount; ++i) {
        const u32 idx = static_cast<u32>(start + static_cast<int>(i)) % m_ShardCount;
        Shard& sh = m_Shards[idx];
        FScopedLock lk(sh.lock);
        void* const p = sh.alloc.Alloc(size, alignment, loc);
        if (p) return p;
    }
    return nullptr;   // 全シャード満杯 = 真の OOM
}

void FShardedTlsfAllocator::FreeSharded(void* ptr) noexcept {
    if (!ptr) return;
    const int s = ShardIndexForPtr(ptr);
    if (s < 0) {
        ACS_ASSERT(false && "FShardedTlsfAllocator::Free: 所有シャード不明 (野良/別アロケータ由来)");
        return;
    }
    FScopedLock lk(m_Shards[s].lock);
    m_Shards[s].alloc.Free(ptr);
}

/**
 * 呼び出しスレッドのマガジンを指定 owner×epoch に整合させて返す。
 *
 * @details
 * 別アロケータ/旧世代を掴んでいたら中身を捨てて貼り直す (旧世代のブロックは VM ごと消えているので
 * 捨てて安全)。
 * @param owner 専有させるアロケータ (通常 this)。
 * @param epoch そのアロケータの現在世代。
 * @return 整合済みの thread-local マガジンへの参照。
 */
static ACS_FORCEINLINE FThreadCache& AcquireCache(const void* owner, u64 epoch) noexcept {
    FThreadCache& tc = t_tcache;
    if (tc.owner != owner || tc.epoch != epoch) {
        for (u32 c = 0; c < kCacheClasses; ++c) { tc.head[c] = nullptr; tc.count[c] = 0; }
        tc.owner = owner;
        tc.epoch = epoch;
    }
    return tc;
}

void* FShardedTlsfAllocator::Alloc(usize size, usize alignment, FSourceLoc loc) noexcept {
    if (!m_Inited || size == 0) return nullptr;

    // ---- lock-free hot path: thread-local マガジン ----
    if (m_CacheEnabled && alignment <= 16 && size <= kCacheMaxBlock) {
        const usize bs  = CacheBlockSizeForRequest(size);
        const int   cls = CacheClassOfBlock(bs);
        if (cls >= 0) {
            FThreadCache& tc = AcquireCache(this, m_Epoch);
            if (tc.count[cls] > 0) {                       // ヒット: ロック無しで pop
                void* const p = tc.head[cls];
                tc.head[cls] = *reinterpret_cast<void**>(p);
                --tc.count[cls];
                return p;
            }
            // ミス: バッチ refill (確保したブロックは実 block_size のクラスへ積む)
            for (u32 i = 0; i < kRefillBatch; ++i) {
                void* const p = AllocSharded(bs - 8u, 16, loc);  // request bs-8 → block_size bs
                if (!p) break;
                const int rc = CacheClassOfBlock(FTlsfAllocator::PayloadBlockSize(p));
                if (rc >= 0 && tc.count[rc] < kBucketCap) {
                    *reinterpret_cast<void**>(p) = tc.head[rc];
                    tc.head[rc] = p; ++tc.count[rc];
                } else {
                    FreeSharded(p);   // 想定外サイズ等はキャッシュせず返す
                }
            }
            if (tc.count[cls] > 0) {                        // refill 後に pop
                void* const p = tc.head[cls];
                tc.head[cls] = *reinterpret_cast<void**>(p);
                --tc.count[cls];
                return p;
            }
            // refill が要求クラスを満たせなかった (OOM 近傍) → 直接確保
        }
    }
    return AllocSharded(size, alignment, loc);
}

void FShardedTlsfAllocator::Free(void* ptr) noexcept {
    if (!ptr) return;

    // ---- lock-free hot path: thread-local マガジンへ push ----
    // 安全: 必ず所有確認 (lock-free, O(N) 範囲判定) を先に行ってから header を読む。
    // 他アロケータ/解放後ポインタ (VM 解放済み) の header を触らない。m_Inited も確認。
    if (m_CacheEnabled && m_Inited) {
        const int s = ShardIndexForPtr(ptr);
        if (s < 0) {
            ACS_ASSERT(false && "FShardedTlsfAllocator::Free: 所有シャード不明 (野良/別アロケータ由来)");
            return;
        }
        const usize bs  = FTlsfAllocator::PayloadBlockSize(ptr);  // 所有確認済み → 安全に読める
        const int   cls = CacheClassOfBlock(bs);
        if (cls >= 0) {
            FThreadCache& tc = AcquireCache(this, m_Epoch);
            if (tc.count[cls] >= kBucketCap) {
                // バケット満杯: 半分を実シャードへ返してから push (償却)
                for (u32 i = 0; i < kBucketCap / 2 && tc.head[cls]; ++i) {
                    void* const q = tc.head[cls];
                    tc.head[cls] = *reinterpret_cast<void**>(q);
                    --tc.count[cls];
                    FreeSharded(q);
                }
            }
            *reinterpret_cast<void**>(ptr) = tc.head[cls];
            tc.head[cls] = ptr; ++tc.count[cls];
            return;
        }
        // キャッシュ対象外サイズ: 所有シャードへ直接返す
        FScopedLock lk(m_Shards[s].lock);
        m_Shards[s].alloc.Free(ptr);
        return;
    }
    FreeSharded(ptr);
}

void* FShardedTlsfAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                                     usize alignment, FSourceLoc loc) noexcept {
    if (!m_Inited) return nullptr;
    if (ptr == nullptr) return Alloc(new_size, alignment, loc);
    if (new_size == 0)  { Free(ptr); return nullptr; }

    const int s = ShardIndexForPtr(ptr);
    if (s < 0) {
        ACS_ASSERT(false && "FShardedTlsfAllocator::Realloc: 所有シャード不明");
        return Alloc(new_size, alignment, loc);
    }
    {
        // 同一シャード内で in-place / 同シャード移動を試みる。
        FScopedLock lk(m_Shards[s].lock);
        void* const p = m_Shards[s].alloc.Realloc(ptr, old_size, new_size, alignment, loc);
        if (p) return p;
    }
    // 同シャードで不可 → 別シャードへ移動。ロックを重ねない (デッドロック回避):
    // 先に新規確保 (任意シャード) → コピー → 旧を解放。失敗時は旧を保持 (realloc 契約)。
    void* const np = Alloc(new_size, alignment, loc);
    if (!np) return nullptr;
    MemCopy(np, ptr, old_size < new_size ? old_size : new_size);
    Free(ptr);
    return np;
}

u64 FShardedTlsfAllocator::BytesAllocated() const noexcept {
    u64 total = 0;
    for (u32 i = 0; i < m_ShardCount; ++i) total += m_Shards[i].alloc.BytesAllocated();
    return total;   // 統計の読み取りはロックしない (近似で可)
}

u64 FShardedTlsfAllocator::PeakBytes() const noexcept {
    // 各シャードのピーク合算は厳密な「同時ピーク」ではないが、上限の目安として有用。
    u64 total = 0;
    for (u32 i = 0; i < m_ShardCount; ++i) total += m_Shards[i].alloc.PeakBytes();
    return total;
}

bool FShardedTlsfAllocator::ValidateHeap() noexcept {
    for (u32 i = 0; i < m_ShardCount; ++i) {
        FScopedLock lk(m_Shards[i].lock);
        if (!m_Shards[i].alloc.ValidateHeap()) return false;
    }
    return true;
}

} // namespace acs
