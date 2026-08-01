// SPDX-License-Identifier: Apache-2.0
// ワークスティーリング CThreadPool 実装（Chase-Lev SPMC deque + ノードプール）
#include "threading/ThreadPool.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "threading/ConditionVar.h"
#include "threading/Thread.h"
#include "threading/MemoryOrder.h"
#include "memory/PoolAllocator.h"
#include "foundation/Platform.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"

#include <intrin.h>

namespace acs {

namespace {

/** ワーカーごとの deque 容量 (2 のべき乗)。 */
constexpr i64 kDequeCapacity     = 4096;

/** deque インデックスを容量内に折り返すビットマスク (容量 - 1)。 */
constexpr i64 kDequeCapacityMask = kDequeCapacity - 1;
static_assert((kDequeCapacity & kDequeCapacityMask) == 0, "Deque capacity must be a power of two");

/** 共有アトミック値の所有群を分離するキャッシュラインのバイト数。 */
constexpr usize kCacheLineBytes = 64u;
static_assert((kCacheLineBytes & (kCacheLineBytes - 1u)) == 0u);

/** ワーカー数の上限 (PoolState の threads[] サイズと一致)。 */
constexpr u32 kMaxWorkers = 256;

/** 外部投入キューのノードプールサイズ (同時保留タスクの上限)。 */
constexpr u32 kSubmitNodePoolCount = 4096;

/** 所有 callable ノードの固定プール件数。 */
constexpr u32 kCallableNodePoolCount = 2048;

/** 非公開 callable ノードを格納できる固定ブロック byte 数。 */
constexpr usize kCallableNodeBlockSize = 128;

/** 1 回の外部投入キュー lock 取得でワーカーへ移す最大件数。 */
constexpr u32 kSubmitDrainBatchSize = 16;

/** ParallelFor が呼び出し stack に直接置く context 件数。 */
constexpr u32 kParallelForInlineContextCapacity = 32;

/** ParallelFor の再利用 block が保持する context 件数。 */
constexpr u32 kParallelForContextBlockCapacity = 64;

/** 同時に貸し出せる ParallelFor context block 数。 */
constexpr u32 kParallelForContextBlockPoolCount = 32;

/**
 * Chase-Lev ワークスチール deque (single-producer / multi-consumer)。
 *
 * @details
 * オーナーワーカーだけが末尾で Push/Pop し、他ワーカーは先頭から Steal する。
 * top/bottom はアトミックで、release ストア・acquire ロード・最後の 1 個の CAS
 * arbitration により lock-free に競合を解決する。64B 整列で false sharing を避ける。
 */
struct alignas(kCacheLineBytes) FWorkerDeque {
    /** Steal 側 (先頭) のインデックス。他ワーカーが奪う。 */
    alignas(kCacheLineBytes) TAtomic<i64> top{0};

    /** オーナー側 (末尾) のインデックス。オーナーが追加・取り出す。 */
    alignas(kCacheLineBytes) TAtomic<i64> bottom{0};

    /** タスク本体のリングバッファ (値コピーで格納)。 */
    FTask buffer[kDequeCapacity]{};

    /**
     * オーナー専用。末尾にタスクを Push する。
     *
     * @details buffer 書き込み後に bottom を release ストアして公開する。
     * @param t 追加するタスク。
     * @return 追加できたら true、deque が満杯なら false。
     */
    bool Push(const FTask& t) noexcept {
        const i64 b = bottom.Load(EMemoryOrder::Relaxed);
        const i64 tt = top.Load(EMemoryOrder::Acquire);
        if (b - tt >= kDequeCapacity) return false;  // 満杯
        buffer[b & kDequeCapacityMask] = t;
        bottom.Store(b + 1, EMemoryOrder::Release);
        return true;
    }

    /**
     * オーナー専用。末尾からタスクを Pop する。
     *
     * @details
     * bottom を先に下げてから HardwareFence で top の読みと順序付けし、残り 1 個の
     * ときだけ Steal と CAS で arbitrate する (Chase-Lev の標準アルゴリズム)。
     * @param out 取り出したタスクの書き込み先。
     * @return 取り出せたら true、deque が空または最後の 1 個を Steal に奪われたら false。
     */
    bool Pop(FTask& out) noexcept {
        const i64 b = bottom.Load(EMemoryOrder::Relaxed) - 1;
        bottom.Store(b, EMemoryOrder::Relaxed);
        HardwareFence();  // bottom 更新と top 読み取りの順序付け
        const i64 tt = top.Load(EMemoryOrder::Relaxed);
        if (tt <= b) {
            out = buffer[b & kDequeCapacityMask];
            if (tt != b) return true;  // 通常ケース
            // 最後の 1 個 — Steal と競合する可能性、CAS で arbitrate
            i64 expected = tt;
            const bool ok = top.CompareExchange(expected, tt + 1);
            bottom.Store(b + 1, EMemoryOrder::Relaxed);
            return ok;
        }
        // deque は空 — 底を元に戻す
        bottom.Store(b + 1, EMemoryOrder::Relaxed);
        return false;
    }

    /**
     * 他ワーカーがオーナーの先頭からタスクを Steal する。
     *
     * @details top を acquire ロード後 HardwareFence で bottom 読みと順序付けし、CAS で奪う。
     * @param out 奪ったタスクの書き込み先。
     * @return 奪えたら true、空または CAS 競合に負けたら false。
     */
    bool Steal(FTask& out) noexcept {
        const i64 tt = top.Load(EMemoryOrder::Acquire);
        HardwareFence();
        const i64 b = bottom.Load(EMemoryOrder::Acquire);
        if (tt < b) {
            out = buffer[tt & kDequeCapacityMask];
            i64 expected = tt;
            return top.CompareExchange(expected, tt + 1);
        }
        return false;
    }
};

static_assert(offsetof(FWorkerDeque, top) % kCacheLineBytes == 0u);
static_assert(offsetof(FWorkerDeque, bottom) % kCacheLineBytes == 0u);
static_assert(offsetof(FWorkerDeque, bottom) - offsetof(FWorkerDeque, top) >= kCacheLineBytes);

/** 外部投入キューの単方向リストノード (プール外スレッドからの Submit を保持)。 */
struct FSubmissionNode {
    /** 次のノード (末尾は nullptr)。 */
    FSubmissionNode* next;

    /** このノードが保持するタスク。 */
    FTask            task;
};

/** FMutex 保護の FIFO 投入キュー (プール外スレッドからの Submit が入る)。 */
struct FSubmissionQueue {
    /** キュー操作を保護する排他ロック。 */
    FMutex            lock;

    /** 先頭ノード (次に drain される、空なら nullptr)。 */
    FSubmissionNode*  head = nullptr;

    /** 末尾ノード (次に Push される位置、空なら nullptr)。 */
    FSubmissionNode*  tail = nullptr;

    /** 現在キューに溜まっているタスク数。 */
    u32              count = 0;
};

/** 公開APIの寿命参照だけを専用cache lineへ収める内部状態。 */
struct alignas(kCacheLineBytes) FApiLifetimeState {
    /** 公開APIがPoolStateを参照している数。解放前の寿命障壁に使う。 */
    TAtomic<u32> api_users{0};

    /** Submitが受付判定から公開完了までにいる数。終了開始時の障壁に使う。 */
    TAtomic<u32> active_submitters{0};
};

static_assert(alignof(FApiLifetimeState) == kCacheLineBytes);
static_assert(sizeof(FApiLifetimeState) == kCacheLineBytes);

/** ParallelFor の 1 チャンク分の実行 context。 */
struct FParallelForContext {
    /** 各 index に対して呼ぶ処理。 */
    void (*body)(u32 index, u32 worker_index, void* user) = nullptr;

    /** body に渡す利用者 context。 */
    void* user = nullptr;

    /** このチャンクの開始 index。 */
    u32 begin = 0;

    /** このチャンクの終了 index。 */
    u32 end = 0;
};

/** ParallelFor context を free-list で再利用する固定長 block。 */
struct FParallelForContextBlock {
    /** 今回の呼び出しが保持する次 block。 */
    FParallelForContextBlock* next;

    /** 連続するチャンク context。 */
    FParallelForContext contexts[kParallelForContextBlockCapacity];
};

/**
 * ワーカーローカルの状態 (deque + スティーリング用 RNG)。
 *
 * @details 64B 整列で隣接ワーカーとの false sharing を避ける。
 */
struct FPoolState;

struct alignas(kCacheLineBytes) FWorker {
    /** このワーカーのワークスチール deque。 */
    FWorkerDeque  deque;

    /** ワーカーインデックス (0..N-1)。 */
    u32          index;

    /** スティーリング先選択用 xorshift32 の状態。 */
    TAtomic<u32>  rng_state;

    /** このワーカーを所有するプール。停止処理中もグローバル参照に依存しないため保持する。 */
    FPoolState* owner = nullptr;

    /** 一括取得時に即時実行する 1 件と deque 満杯時の退避タスク。 */
    FTask submit_batch[kSubmitDrainBatchSize]{};

    /** submit_batch の次の読み出し位置。 */
    u32 submit_batch_head = 0;

    /** submit_batch に入っている件数。 */
    u32 submit_batch_count = 0;
};

/** スレッドプール全体の状態 (シングルトン)。 */
struct FPoolState {
    /** 64B整列前の確保元ポインタ。終了時のHeapFreeへ渡す。 */
    void* state_allocation = nullptr;

    /** ワーカー配列 (64B 整列で確保)。 */
    FWorker*           workers      = nullptr;

    /** workers の確保元が返した解放用ポインタ。 */
    void* worker_allocation = nullptr;

    /** ワーカー数。 */
    u32               worker_count = 0;

    /** 各ワーカーの thread handle。 */
    FThread threads[kMaxWorkers];

    /** 動作フラグ (1=動作中, 0=停止要求)。 */
    TAtomic<u32>       running      {0};

    /** 外部スレッドから新しい仕事を受け付ける間は 1。終了開始時に 0 へ遷移する。 */
    TAtomic<u32> accepting{0};

    /** 実行制御用キャッシュラインから分離した未完了タスク数。 */
    alignas(kCacheLineBytes) TAtomic<u32> outstanding{0};

    /** 公開済みだが、まだいずれの実行者にも取得されていないタスク数。 */
    TAtomic<u32> queued_work{0};

    /** queued_work のうち外部投入 FIFO に残っているタスク数。 */
    TAtomic<u32> external_queued_work{0};

    /** 外部投入キュー。 */
    FSubmissionQueue   submit;

    /** wake_cv とペアで使う、ワーカー park 用のロック。 */
    FMutex             wake_lock;

    /** 仕事が来たときに park 中のワーカーを起こす条件変数。 */
    FConditionVar      wake_cv;

    /** wake_cv で待機中、または待機へ遷移中のワーカー数。 */
    TAtomic<u32> sleeping_workers{0};

    /** 通知済みだが、まだ Wait から戻っていないワーカー数。wake_lock が保護する。 */
    u32 wake_reservations = 0;

    /** 外部投入ノードを HeapAlloc せず取るための固定サイズプール。 */
    CPoolAllocator*    submit_node_pool = nullptr;

    /** 所有 callable ノードを HeapAlloc せず取る固定サイズプール。 */
    CPoolAllocator* callable_node_pool = nullptr;

    /** ParallelFor の overflow context を再利用する固定 block pool。 */
    CPoolAllocator* parallel_for_context_pool = nullptr;

    /** 外部投入キュー lock の取得回数。 */
    TAtomic<u64> submission_lock_acquisitions{0};

    /** 外部投入キュー drain の lock 取得回数。 */
    TAtomic<u64> submission_drain_lock_acquisitions{0};

    /** 外部投入キュー lock の競合回数。 */
    TAtomic<u64> submission_lock_contentions{0};

    /** 外部投入キューから一括取得したタスク数。 */
    TAtomic<u64> external_tasks_drained{0};

    /** worker が park へ入った回数。 */
    TAtomic<u64> worker_parks{0};

    /** 待機者へ発行した NotifyOne 回数。 */
    TAtomic<u64> wake_one_calls{0};

    /** 終了時に発行した NotifyAll 回数。 */
    TAtomic<u64> wake_all_calls{0};

    /** 固定ノードプール枯渇後の HeapAlloc 回数。 */
    TAtomic<u64> submission_heap_fallbacks{0};

    /** inline 領域へ格納した所有 callable の投入数。 */
    TAtomic<u64> callable_inline_submissions{0};

    /** サイズ超過で heap へ格納した所有 callable の投入数。 */
    TAtomic<u64> callable_heap_submissions{0};

    /** 所有 callable ノードプール枯渇後の HeapAlloc 回数。 */
    TAtomic<u64> callable_node_heap_fallbacks{0};

    /** ParallelFor が stack context だけで完了した回数。 */
    TAtomic<u64> parallel_for_inline_calls{0};

    /** ParallelFor が固定 block pool から取得した block 数。 */
    TAtomic<u64> parallel_for_pool_blocks{0};

    /** 現在貸し出している ParallelFor context block 数。 */
    TAtomic<u64> parallel_for_pool_blocks_in_use{0};

    /** 同時に貸し出した ParallelFor context block 数の最大値。 */
    TAtomic<u64> parallel_for_pool_blocks_high_water{0};

    /** ParallelFor が OS heap へ退避した block 数。 */
    TAtomic<u64> parallel_for_heap_blocks{0};

    /** 他の共有状態とcache lineを共有しないAPI寿命参照。必ず末尾へ置く。 */
    FApiLifetimeState api_lifetime{};
};

/** 三所有群のキャッシュライン配置をコンパイル時に固定する。 */
static_assert(alignof(FPoolState) >= kCacheLineBytes);
static_assert(offsetof(FPoolState, outstanding) % kCacheLineBytes == 0u);
static_assert(offsetof(FPoolState, api_lifetime) % kCacheLineBytes == 0u);
static_assert(offsetof(FPoolState, api_lifetime) + sizeof(FApiLifetimeState) == sizeof(FPoolState));
static_assert(offsetof(FPoolState, running) / kCacheLineBytes == offsetof(FPoolState, accepting) / kCacheLineBytes);
static_assert(offsetof(FPoolState, running) / kCacheLineBytes != offsetof(FPoolState, outstanding) / kCacheLineBytes);
static_assert(offsetof(FPoolState, outstanding) / kCacheLineBytes == offsetof(FPoolState, queued_work) / kCacheLineBytes);
static_assert(offsetof(FPoolState, outstanding) / kCacheLineBytes == offsetof(FPoolState, external_queued_work) / kCacheLineBytes);
static_assert(offsetof(FPoolState, outstanding) / kCacheLineBytes != offsetof(FPoolState, api_lifetime) / kCacheLineBytes);
static_assert(offsetof(FPoolState, api_lifetime) / kCacheLineBytes != offsetof(FPoolState, submit) / kCacheLineBytes);
static_assert(offsetof(FPoolState, api_lifetime) / kCacheLineBytes != offsetof(FPoolState, wake_lock) / kCacheLineBytes);
static_assert(offsetof(FPoolState, api_lifetime) / kCacheLineBytes != offsetof(FPoolState, wake_cv) / kCacheLineBytes);
static_assert(offsetof(FPoolState, api_lifetime) / kCacheLineBytes != offsetof(FPoolState, sleeping_workers) / kCacheLineBytes);
static_assert(offsetof(FPoolState, api_lifetime) / kCacheLineBytes != offsetof(FPoolState, wake_reservations) / kCacheLineBytes);
static_assert(offsetof(FApiLifetimeState, api_users) / kCacheLineBytes == offsetof(FApiLifetimeState, active_submitters) / kCacheLineBytes);

/**
 * 64B境界へ整列したPoolStateを確保する。
 *
 * @return 構築済みstate。確保失敗時はnullptr。
 */
FPoolState* CreatePoolState() noexcept
{
    /** 整列余白を含む確保byte数。 */
    constexpr usize kAllocationBytes = sizeof(FPoolState) + kCacheLineBytes - 1u;
    /** HeapFreeへ渡す元ポインタ。 */
    void* const allocation = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, kAllocationBytes);
    if (!allocation) return nullptr;
    /** PoolStateを構築する64B境界アドレス。 */
    const uptr aligned_address = (reinterpret_cast<uptr>(allocation) + kCacheLineBytes - 1u) & ~uptr{kCacheLineBytes - 1u};
    ACS_ASSERT((aligned_address & (kCacheLineBytes - 1u)) == 0u);
    /** 整列済み領域で寿命を開始したPoolState。 */
    FPoolState* const state = ::new (reinterpret_cast<void*>(aligned_address)) FPoolState();
    state->state_allocation = allocation;
    return state;
}

/**
 * PoolStateを破棄して整列前の確保元を解放する。
 *
 * @param state 破棄するstate。nullptrは無視する。
 */
void DestroyPoolState(FPoolState* state) noexcept
{
    if (!state) return;
    /** placement new前にHeapAllocが返した解放対象。 */
    void* const allocation = state->state_allocation;
    state->~FPoolState();
    ::HeapFree(::GetProcessHeap(), 0, allocation);
}

/** プール全体の状態へのグローバルポインタ (null = 未初期化)。 */
FPoolState* g_pool = nullptr;

/** g_pool の差し替えと公開 API の参照取得を直列化する。 */
FMutex g_lifecycle_lock;

/** Init/Shutdown 同士を直列化する。長い終了待機中に Submit はこのロックを使わない。 */
FMutex g_init_shutdown_lock;

/** ワーカースレッドだけが自分のインデックスを持つ TLS (ワーカー外は kNotAWorker)。 */
ACS_THREAD_LOCAL u32 tls_worker_index = CThreadPool::kNotAWorker;

/** 現在実行しているタスクの所有プール。外部スレッドが Wait 中に実行する場合も設定する。 */
ACS_THREAD_LOCAL FPoolState* tls_executing_pool = nullptr;

/** 所有 callable のユーザー定義構築・破棄処理へ入っている深さ。 */
ACS_THREAD_LOCAL u32 tls_callable_lifecycle_depth = 0;

/**
 * 公開 API の実行中だけ PoolState の寿命を保持する小さなピン。
 *
 * @details g_pool 読み取りと api_users 加算を同じロック区間で行うため、Shutdown が
 * g_pool を外してから状態を解放する処理と競合しない。
 */
class FPoolPin {
public:
    FPoolPin() noexcept
    {
        FScopedLock lock(g_lifecycle_lock);
        m_Pool = g_pool;
        if (m_Pool) m_Pool->api_lifetime.api_users.FetchAdd(1);
    }

    ~FPoolPin() noexcept
    {
        if (m_Pool) m_Pool->api_lifetime.api_users.FetchSub(1);
    }

    FPoolPin(const FPoolPin&) = delete;
    FPoolPin& operator=(const FPoolPin&) = delete;

    FPoolState* Get() const noexcept
    {
        return m_Pool;
    }

private:
    FPoolState* m_Pool = nullptr;
};

/**
 * スティーリング先選択用の小さな PRNG (xorshift32)。
 *
 * @param s 進める乱数状態 (0 のときはシードに補正)。
 * @return 生成した 32bit 乱数。
 */
ACS_FORCEINLINE u32 XorShift32(TAtomic<u32>& s) noexcept {
    u32 x = s.Load(EMemoryOrder::Relaxed);
    if (x == 0) x = 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.Store(x, EMemoryOrder::Relaxed);
    return x;
}

/**
 * 投入ノードを 1 つ確保する (プール優先、枯渇時のみ HeapAlloc にフォールバック)。
 *
 * @return 確保したノード、確保失敗時は nullptr。
 */
FSubmissionNode* AcquireSubmitNode(FPoolState* pool) noexcept
{
    if (pool->submit_node_pool) {
        void* const p = pool->submit_node_pool->Alloc(sizeof(FSubmissionNode), alignof(FSubmissionNode), FSourceLoc::Current());
        if (p) return static_cast<FSubmissionNode*>(p);
    }
    // フォールバック: プール枯渇時は Heap から取る
    pool->submission_heap_fallbacks.FetchAdd(1);
    return static_cast<FSubmissionNode*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(FSubmissionNode)));
}

/**
 * 投入ノードを返却する (プール範囲内ならプールへ、それ以外なら Heap へ)。
 *
 * @param n 返却するノード (nullptr は無視)。
 */
void ReleaseSubmitNode(FPoolState* state, FSubmissionNode* n) noexcept
{
    if (!n) return;
    CPoolAllocator* pool = state->submit_node_pool;
    if (pool && pool->Contains(n)) {
        pool->Free(n);
    } else {
        ::HeapFree(::GetProcessHeap(), 0, n);
    }
}

/**
 * ParallelFor context block を固定 free-list から取得し、枯渇時だけ heap へ退避する。
 *
 * @param pool block の所有元。
 * @return 初期化済み block。確保失敗時は nullptr。
 */
FParallelForContextBlock* AcquireParallelForContextBlock(FPoolState* pool) noexcept
{
    /** 固定 pool または heap が返した未初期化領域。 */
    void* memory = nullptr;
    /** 固定 pool から取得した場合は true。 */
    bool from_pool = false;
    if (pool->parallel_for_context_pool) {
        memory = pool->parallel_for_context_pool->Alloc(sizeof(FParallelForContextBlock), alignof(FParallelForContextBlock), FSourceLoc::Current());
    }
    if (memory) {
        from_pool = true;
        pool->parallel_for_pool_blocks.FetchAdd(1u);
    } else {
        memory = ::HeapAlloc(::GetProcessHeap(), 0, sizeof(FParallelForContextBlock));
        if (!memory) return nullptr;
        pool->parallel_for_heap_blocks.FetchAdd(1u);
    }
    /** 未初期化領域で block の寿命を開始する。 */
    FParallelForContextBlock* const block = ::new (memory) FParallelForContextBlock;
    block->next = nullptr;
    if (from_pool) {
        /** 今回の貸し出しを含む同時使用数。 */
        const u64 in_use = pool->parallel_for_pool_blocks_in_use.FetchAdd(1u) + 1u;
        /** 記録済み最大値。 */
        u64 high_water = pool->parallel_for_pool_blocks_high_water.Load(EMemoryOrder::Acquire);
        while (in_use > high_water && !pool->parallel_for_pool_blocks_high_water.CompareExchange(high_water, in_use)) {}
    }
    return block;
}

/**
 * ParallelFor context block を構築元へ返す。
 *
 * @param pool block の所有元。
 * @param block 返却する block。
 */
void ReleaseParallelForContextBlock(FPoolState* pool, FParallelForContextBlock* block) noexcept
{
    if (!block) return;
    /** 貸出数を free-list 公開前に減らす固定 pool 所有領域なら true。 */
    const bool from_pool = pool->parallel_for_context_pool && pool->parallel_for_context_pool->Contains(block);
    block->~FParallelForContextBlock();
    if (from_pool) {
        pool->parallel_for_pool_blocks_in_use.FetchSub(1u);
        pool->parallel_for_context_pool->Free(block);
    } else {
        ::HeapFree(::GetProcessHeap(), 0, block);
    }
}

/**
 * 外部投入キューから最大 kSubmitDrainBatchSize 件を 1 回で取り出す。
 *
 * @details ロックは TryLock で取り、取れなければ即 false (ブロックしない)。
 * @param worker 取得したタスクを保持するワーカー。
 * @return 1 件以上取り出せたら true。
 */
bool TryDrainSubmitBatch(FPoolState* pool, FWorker& worker) noexcept
{
    /** 複数 producer が共有する外部投入 FIFO。 */
    FSubmissionQueue& q = pool->submit;
    if (!q.lock.TryLock()) {
        pool->submission_lock_contentions.FetchAdd(1);
        return false;
    }
    pool->submission_lock_acquisitions.FetchAdd(1);
    pool->submission_drain_lock_acquisitions.FetchAdd(1);

    /** lock 解放後に返却するノード列の先頭。 */
    FSubmissionNode* free_head = nullptr;
    /** lock 解放後に返却するノード列の末尾。 */
    FSubmissionNode* free_tail = nullptr;
    /** lock 保持中に取り出したタスク列。 */
    FTask drained[kSubmitDrainBatchSize]{};
    /** 今回ワーカーへ移した件数。 */
    u32 count = 0;
    while (q.head && count < kSubmitDrainBatchSize) {
        FSubmissionNode* const node = q.head;
        q.head = node->next;
        node->next = nullptr;
        drained[count++] = node->task;
        if (free_tail) free_tail->next = node;
        else free_head = node;
        free_tail = node;
    }
    if (!q.head) q.tail = nullptr;
    q.count -= count;
    // キュー状態と公開件数を同じ lock 区間で更新し、古い件数を見たワーカーの空振り drain を抑える。
    pool->external_queued_work.FetchSub(count);
    q.lock.Unlock();

    /** 固定プールへ返却中のノード。 */
    FSubmissionNode* node = free_head;
    while (node) {
        /** 返却前に退避する次ノード。 */
        FSubmissionNode* const next = node->next;
        ReleaseSubmitNode(pool, node);
        node = next;
    }
    if (count == 0) return false;

    // 先頭 1 件は直ちに実行し、残りは他ワーカーが steal できる deque へ逆順で積む。
    // deque が満杯になった場合だけ未公開分をローカルバッチへ退避する。
    /** deque へ積めた最小 task index。これ未満はローカルバッチへ残す。 */
    u32 lowest_pushed_index = count;
    for (u32 i = count; i > 1; --i) {
        /** 今回 deque へ積む task index。 */
        const u32 task_index = i - 1;
        if (!worker.deque.Push(drained[task_index])) break;
        lowest_pushed_index = task_index;
    }

    worker.submit_batch_head = 0;
    worker.submit_batch_count = 0;
    for (u32 i = 0; i < lowest_pushed_index; ++i) {
        worker.submit_batch[worker.submit_batch_count++] = drained[i];
    }
    pool->queued_work.FetchSub(worker.submit_batch_count);
    pool->external_tasks_drained.FetchAdd(count);
    return true;
}

/** ワーカーの外部投入バッチから 1 件取り出す。 */
bool PopSubmitBatch(FWorker& worker, FTask& out) noexcept
{
    if (worker.submit_batch_head >= worker.submit_batch_count) return false;
    out = worker.submit_batch[worker.submit_batch_head++];
    if (worker.submit_batch_head == worker.submit_batch_count) {
        worker.submit_batch_head = 0;
        worker.submit_batch_count = 0;
    }
    return true;
}

/**
 * 外部 Wait 実行者向けに、投入キューから 1 件だけ取得する。
 *
 * @details ワーカーの一括キャッシュへ隠さず、呼び出し元が直ちに実行する。
 */
bool TryDrainSubmitOne(FPoolState* pool, FTask& out) noexcept
{
    /** 複数実行者が共有する外部投入 FIFO。 */
    FSubmissionQueue& q = pool->submit;
    if (!q.lock.TryLock()) {
        pool->submission_lock_contentions.FetchAdd(1);
        return false;
    }
    pool->submission_lock_acquisitions.FetchAdd(1);
    pool->submission_drain_lock_acquisitions.FetchAdd(1);
    /** 取得対象の先頭ノード。 */
    FSubmissionNode* node = q.head;
    if (node) {
        out = node->task;
        q.head = node->next;
        if (!q.head) q.tail = nullptr;
        --q.count;
        // キュー状態と公開件数を同じ lock 区間で更新する。
        pool->external_queued_work.FetchSub(1);
    }
    q.lock.Unlock();
    if (!node) return false;
    ReleaseSubmitNode(pool, node);
    pool->queued_work.FetchSub(1);
    pool->external_tasks_drained.FetchAdd(1);
    return true;
}

/**
 * 自分以外のワーカーからタスクの Steal を試みる (ランダム開始 + 巡回)。
 *
 * @param self_index 呼び出し元ワーカーのインデックス (このワーカーは奪わない)。
 * @param out 奪ったタスクの書き込み先。
 * @return いずれかのワーカーから奪えたら true。
 */
bool TrySteal(FPoolState* pool, u32 self_index, FTask& out) noexcept
{
    const u32 n = pool->worker_count;
    if (n <= 1) return false;
    FWorker& self = pool->workers[self_index];
    u32 victim = XorShift32(self.rng_state) % n;
    for (u32 i = 0; i < n; ++i) {
        if (victim != self_index) {
            if (pool->workers[victim].deque.Steal(out)) {
                pool->queued_work.FetchSub(1);
                return true;
            }
        }
        victim = (victim + 1) % n;
    }
    return false;
}

/**
 * 待機者が存在する場合だけ 1 ワーカーを起こす。
 *
 * @details queued_work の公開後に wake_lock を取る。ワーカーは同じ lock の内側で
 * queued_work を再確認してから待つため、通知が先行しても lost wake にならない。
 */
void WakeOneIfSleeping(FPoolState* pool) noexcept
{
    if (pool->sleeping_workers.Load(EMemoryOrder::Acquire) == 0) return;
    FScopedLock lock(pool->wake_lock);
    /** 現在 Wait 中または Wait へ遷移中のワーカー数。 */
    const u32 sleeping = pool->sleeping_workers.Load(EMemoryOrder::Relaxed);
    if (pool->wake_reservations < sleeping) {
        ++pool->wake_reservations;
        pool->wake_one_calls.FetchAdd(1);
        pool->wake_cv.NotifyOne();
    }
}

/** 終了処理で全待機ワーカーを起こす。 */
void WakeAllWorkers(FPoolState* pool) noexcept
{
    FScopedLock lock(pool->wake_lock);
    /** 現在 Wait 中または Wait へ遷移中のワーカー数。 */
    const u32 sleeping = pool->sleeping_workers.Load(EMemoryOrder::Relaxed);
    if (sleeping != 0) {
        pool->wake_reservations = sleeping;
        pool->wake_all_calls.FetchAdd(1);
        pool->wake_cv.NotifyAll();
    }
}

/**
 * タスクを実行し、完了通知 (counter->Done) も行う。
 *
 * @param t 実行するタスク (fn が null なら実行をスキップ)。
 * @param worker_index 実行中のワーカーインデックス (fn に渡す)。
 */
ACS_FORCEINLINE void Execute(FPoolState* pool, const FTask& t, u32 worker_index) noexcept
{
    FPoolState* const previous_pool = tls_executing_pool;
    tls_executing_pool = pool;
    if (t.fn) t.fn(t.user, worker_index);
    if (t.counter) t.counter->Done();
    tls_executing_pool = previous_pool;
    pool->outstanding.FetchSub(1);
}

/**
 * ワーカースレッドのメインループ。
 *
 * @details
 * running が真の間、自 deque pop → 外部キュー一括取得 → 他ワーカー steal の順に
 * 仕事を探し、すべて空なら wake_cv で park する。
 * @param arg 担当する Worker へのポインタ。
 */
void WorkerMain(void* arg) noexcept {
    /** このスレッドへ割り当てられたワーカー状態。 */
    FWorker* w = static_cast<FWorker*>(arg);
    /** ワーカーの寿命を所有するプール。 */
    FPoolState* const pool = w->owner;
    tls_worker_index = w->index;
    w->rng_state.Store(0x9E3779B9u ^ (w->index * 2654435761u), EMemoryOrder::Relaxed);

    while (pool->running.Load(EMemoryOrder::Acquire)) {
        FTask t {};
        if (PopSubmitBatch(*w, t)) {
            Execute(pool, t, w->index);
            continue;
        }
        if (w->deque.Pop(t)) {
            pool->queued_work.FetchSub(1);
            Execute(pool, t, w->index);
            continue;
        }
        if (pool->external_queued_work.Load(EMemoryOrder::Acquire) != 0 && TryDrainSubmitBatch(pool, *w) && PopSubmitBatch(*w, t)) {
            Execute(pool, t, w->index);
            continue;
        }
        if (TrySteal(pool, w->index, t)) {
            Execute(pool, t, w->index);
            continue;
        }

        // 公開と待機遷移を同じ lock で直列化し、lost wake を防ぐ。
        FScopedLock lk(pool->wake_lock);
        if (pool->running.Load(EMemoryOrder::Acquire) == 0) break;
        if (pool->queued_work.Load(EMemoryOrder::Acquire) != 0) continue;
        pool->sleeping_workers.FetchAdd(1);
        pool->worker_parks.FetchAdd(1);
        pool->wake_cv.Wait(pool->wake_lock);
        if (pool->wake_reservations != 0) --pool->wake_reservations;
        pool->sleeping_workers.FetchSub(1);
    }
    tls_worker_index = CThreadPool::kNotAWorker;
}

} // namespace

/** プールを初期化し、ワーカースレッドを起動する。 */
TResult<void> CThreadPool::Init(u32 worker_count) noexcept {
    FScopedLock operation_lock(g_init_shutdown_lock);
    FScopedLock lifecycle_lock(g_lifecycle_lock);
    if (g_pool != nullptr) return ACS_ERR(Threading, 2, "CThreadPool already initialized");
    if (worker_count == 0) worker_count = HardwareConcurrency();
    if (worker_count > kMaxWorkers) worker_count = kMaxWorkers;

    // 共有アトミック値のキャッシュライン境界を保証したPoolStateを確保する。
    g_pool = CreatePoolState();
    if (!g_pool) return ACS_ERR(Memory, 2, "CThreadPool state alloc failed");

    // ワーカー配列を 64B 境界整列で確保
    usize total = sizeof(FWorker) * worker_count + kCacheLineBytes - 1u;
    void* wmem = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!wmem) {
        DestroyPoolState(g_pool);
        g_pool = nullptr;
        return ACS_ERR(Memory, 3, "CThreadPool worker alloc failed");
    }
    uptr aligned = (reinterpret_cast<uptr>(wmem) + kCacheLineBytes - 1u) & ~uptr{kCacheLineBytes - 1u};
    g_pool->worker_allocation = wmem;
    g_pool->workers = reinterpret_cast<FWorker*>(aligned);
    for (u32 i = 0; i < worker_count; ++i) {
        ::new (&g_pool->workers[i]) FWorker();
        g_pool->workers[i].index = i;
        g_pool->workers[i].owner = g_pool;
    }
    g_pool->worker_count = worker_count;

    // 外部投入ノード用のプールを構築（HeapAlloc syscall を回避）
    void* pool_mem = ::HeapAlloc(::GetProcessHeap(), 0, sizeof(CPoolAllocator));
    if (!pool_mem) {
        ::HeapFree(::GetProcessHeap(), 0, wmem);
        DestroyPoolState(g_pool);
        g_pool = nullptr;
        return ACS_ERR(Memory, 7, "CThreadPool submit pool alloc failed");
    }
    g_pool->submit_node_pool = ::new (pool_mem) CPoolAllocator(sizeof(FSubmissionNode), kSubmitNodePoolCount, alignof(FSubmissionNode));

    // 所有 callable 用ノードも固定プールへ置き、通常投入時の HeapAlloc をなくす。
    void* callable_pool_mem =
        ::HeapAlloc(::GetProcessHeap(), 0, sizeof(CPoolAllocator));
    if (!callable_pool_mem) {
        g_pool->submit_node_pool->~CPoolAllocator();
        ::HeapFree(::GetProcessHeap(), 0, g_pool->submit_node_pool);
        ::HeapFree(::GetProcessHeap(), 0, wmem);
        DestroyPoolState(g_pool);
        g_pool = nullptr;
        return ACS_ERR(Memory, 8, "CThreadPool callable pool alloc failed");
    }
    g_pool->callable_node_pool = ::new (callable_pool_mem) CPoolAllocator(kCallableNodeBlockSize, kCallableNodePoolCount, alignof(std::max_align_t));

    /** ParallelFor 固定 block pool object の配置先。 */
    void* const parallel_for_pool_memory = ::HeapAlloc(::GetProcessHeap(), 0, sizeof(CPoolAllocator));
    if (parallel_for_pool_memory) {
        g_pool->parallel_for_context_pool = ::new (parallel_for_pool_memory) CPoolAllocator(sizeof(FParallelForContextBlock), kParallelForContextBlockPoolCount, alignof(FParallelForContextBlock));
    }

    g_pool->accepting.Store(1, EMemoryOrder::Release);
    g_pool->running.Store(1, EMemoryOrder::Release);

    // ワーカースレッド起動
    for (u32 i = 0; i < worker_count; ++i) {
        FThreadConfig cfg {};
        cfg.name = L"acs::CThreadPool worker";
        auto r = FThread::Spawn(&WorkerMain, &g_pool->workers[i], cfg);
        if (r.IsErr()) {
            // 失敗時のロールバック
            g_pool->running.Store(0, EMemoryOrder::Release);
            WakeAllWorkers(g_pool);
            for (u32 j = 0; j < i; ++j) g_pool->threads[j].Join();
            g_pool->submit_node_pool->~CPoolAllocator();
            ::HeapFree(::GetProcessHeap(), 0, g_pool->submit_node_pool);
            g_pool->callable_node_pool->~CPoolAllocator();
            ::HeapFree(::GetProcessHeap(), 0, g_pool->callable_node_pool);
            if (g_pool->parallel_for_context_pool) {
                g_pool->parallel_for_context_pool->~CPoolAllocator();
                ::HeapFree(::GetProcessHeap(), 0, g_pool->parallel_for_context_pool);
            }
            ::HeapFree(::GetProcessHeap(), 0, wmem);
            DestroyPoolState(g_pool);
            g_pool = nullptr;
            return Err<void>(r.Error());
        }
        g_pool->threads[i] = Move(r.Value());
    }
    return Ok();
}

/** 全受理済みタスクを排出してからワーカーを停止・Join し、プールのリソースを解放する。 */
void CThreadPool::Shutdown() noexcept {
    // 実行中タスクから自分自身を Join すると永久待機になる。外部所有者が改めて終了する。
    // callable の構築・破棄中も、その処理を保持する api_users を自分で待てないため同様とする。
    if (tls_executing_pool != nullptr || tls_callable_lifecycle_depth != 0) {
        return;
    }

    FScopedLock operation_lock(g_init_shutdown_lock);

    FPoolState* pool = nullptr;
    {
        FScopedLock lifecycle_lock(g_lifecycle_lock);
        pool = g_pool;
        if (!pool) return;
        // ここから外部 Submit は拒否する。既に走っているタスクが作る子タスクだけは
        // 依存グラフを途中で切らないため、Submit 側で引き続き受理する。
        pool->accepting.Store(0, EMemoryOrder::Release);
    }

    WakeAllWorkers(pool);

    // 受付判定を通過済みの Submit がキュー公開または失敗巻き戻しを終えるまで待つ。
    while (pool->api_lifetime.active_submitters.Load(EMemoryOrder::Acquire) != 0)
        Yield();

    // outstanding はコールバックから戻った後に減る。従って 0 ならキュー・deque・
    // 実行中タスクのいずれにも受理済み仕事が残っていない。
    while (pool->outstanding.Load(EMemoryOrder::Acquire) != 0) {
        SleepMs(1);
    }

    pool->running.Store(0, EMemoryOrder::Release);
    WakeAllWorkers(pool);
    for (u32 i = 0; i < pool->worker_count; ++i)
        pool->threads[i].Join();

    // 以降の公開 API が古い状態を取得できないようグローバル参照を先に外す。
    {
        FScopedLock lifecycle_lock(g_lifecycle_lock);
        if (g_pool == pool) g_pool = nullptr;
    }
    while (pool->api_lifetime.api_users.Load(EMemoryOrder::Acquire) != 0)
        Yield();

    // outstanding == 0 なら通常は空。防御的にノードだけを回収する。
    {
        FScopedLock lk(pool->submit.lock);
        FSubmissionNode* n = pool->submit.head;
        while (n) {
            FSubmissionNode* nx = n->next;
            ReleaseSubmitNode(pool, n);
            n = nx;
        }
        pool->submit.head = nullptr;
        pool->submit.tail = nullptr;
        pool->submit.count = 0;
    }

    // ノードプール破棄
    if (pool->submit_node_pool) {
        pool->submit_node_pool->~CPoolAllocator();
        ::HeapFree(::GetProcessHeap(), 0, pool->submit_node_pool);
        pool->submit_node_pool = nullptr;
    }

    // 所有 callable ノードプール破棄
    if (pool->callable_node_pool) {
        pool->callable_node_pool->~CPoolAllocator();
        ::HeapFree(::GetProcessHeap(), 0, pool->callable_node_pool);
        pool->callable_node_pool = nullptr;
    }

    // ParallelFor context block pool を破棄する。
    if (pool->parallel_for_context_pool) {
        pool->parallel_for_context_pool->~CPoolAllocator();
        ::HeapFree(::GetProcessHeap(), 0, pool->parallel_for_context_pool);
        pool->parallel_for_context_pool = nullptr;
    }

    // ワーカー破棄
    FWorker* base = pool->workers;
    for (u32 i = 0; i < pool->worker_count; ++i)
        base[i].~FWorker();
    pool->workers = nullptr;
    pool->worker_count = 0;

    // HeapFree は HeapAlloc が返した元ポインタに対してのみ有効。64B 整列後の
    // workers を渡すと失敗し、Init/Shutdown のたびにワーカー配列が残り続ける。
    ::HeapFree(::GetProcessHeap(), 0, pool->worker_allocation);
    pool->worker_allocation = nullptr;

    DestroyPoolState(pool);
}

/** 起動中のワーカー数を返す (未初期化なら 0)。 */
u32 CThreadPool::WorkerCount() noexcept {
    FPoolPin pin;
    return pin.Get() ? pin.Get()->worker_count : 0;
}

/** 呼び出しスレッドのワーカーインデックスを返す (非ワーカーは kNotAWorker)。 */
u32 CThreadPool::CurrentWorkerIndex() noexcept {
    return tls_worker_index;
}

/** 所有 callable ノードを固定プールから確保する。 */
CThreadPool::FCallableTaskStorage* CThreadPool::AcquireCallableTaskStorage() noexcept
{
    static_assert(sizeof(FCallableTaskStorage) <= kCallableNodeBlockSize, "callable ノードの固定プール block が不足しています");

    /** 構築完了まで寿命を固定する対象プール。 */
    FPoolState* pool = nullptr;
    {
        FScopedLock lifecycle_lock(g_lifecycle_lock);
        pool = g_pool;
        if (!pool) return nullptr;
        // 構築中に Shutdown がノードプールを解放しないよう寿命を保持する。
        pool->api_lifetime.api_users.FetchAdd(1);
    }

    void* memory = nullptr;
    if (pool->callable_node_pool) {
        memory = pool->callable_node_pool->Alloc(sizeof(FCallableTaskStorage), alignof(FCallableTaskStorage), FSourceLoc::Current());
    }
    if (!memory) {
        pool->callable_node_heap_fallbacks.FetchAdd(1);
        memory = ::HeapAlloc(::GetProcessHeap(), 0, sizeof(FCallableTaskStorage));
    }
    if (!memory) {
        pool->api_lifetime.api_users.FetchSub(1);
        return nullptr;
    }

    /** 初期化した callable 所有ノード。 */
    auto* const storage = ::new (memory) FCallableTaskStorage();
    storage->owner = pool;
    ++tls_callable_lifecycle_depth;
    return storage;
}

/** 構築前に失敗した所有 callable ノードを返却する。 */
void CThreadPool::AbandonCallableTaskStorage(FCallableTaskStorage* storage) noexcept
{
    if (!storage) return;
    auto* const pool = static_cast<FPoolState*>(storage->owner);
    ACS_ASSERT(tls_callable_lifecycle_depth != 0);
    --tls_callable_lifecycle_depth;
    storage->~FCallableTaskStorage();
    if (pool->callable_node_pool && pool->callable_node_pool->Contains(storage)) {
        pool->callable_node_pool->Free(storage);
    } else {
        ::HeapFree(::GetProcessHeap(), 0, storage);
    }
    pool->api_lifetime.api_users.FetchSub(1);
}

/** 構築済み所有 callable を通常の FTask 経路へ公開する。 */
TResult<void> CThreadPool::PublishCallableTaskStorage(FCallableTaskStorage* storage, FCompletionCounter* counter) noexcept
{
    /** callable 所有ノードの確保元プール。 */
    auto* const pool = static_cast<FPoolState*>(storage->owner);
    ACS_ASSERT(tls_callable_lifecycle_depth != 0);
    --tls_callable_lifecycle_depth;
    /** worker がノードを解放する前に退避した本体確保方式。 */
    const bool heap_callable = storage->heap_callable;
    /** 従来 ABI へ接続した公開タスク。 */
    const FTask task{&CThreadPool::CallableTaskThunk, storage, counter};
    /** 外部投入 FIFO への公開結果。 */
    TResult<void> result = Submit(task);
    if (result.IsOk()) {
        // Submit 後は worker が storage を即時解放できるため、事前に退避した値だけを使う。
        if (heap_callable)
            pool->callable_heap_submissions.FetchAdd(1);
        else
            pool->callable_inline_submissions.FetchAdd(1);
        pool->api_lifetime.api_users.FetchSub(1);
        return result;
    }

    // 投入失敗では worker に所有権が渡っていないので、構築元が破棄する。
    ++tls_callable_lifecycle_depth;
    if (storage->destroy) storage->destroy(storage->object);
    --tls_callable_lifecycle_depth;
    storage->destroy = nullptr;
    storage->object = nullptr;
    storage->~FCallableTaskStorage();
    if (pool->callable_node_pool && pool->callable_node_pool->Contains(storage)) {
        pool->callable_node_pool->Free(storage);
    } else {
        ::HeapFree(::GetProcessHeap(), 0, storage);
    }
    pool->api_lifetime.api_users.FetchSub(1);
    return result;
}

/** worker 上で所有 callable を実行し、必ず破棄してノードを返却する。 */
void CThreadPool::CallableTaskThunk(void* user, u32 worker_index) noexcept
{
    /** 実行と破棄の型情報を持つ所有ノード。 */
    auto* const storage = static_cast<FCallableTaskStorage*>(user);
    /** ノードを返却する確保元プール。 */
    auto* const pool = static_cast<FPoolState*>(storage->owner);
    storage->invoke(storage->object, worker_index);
    storage->destroy(storage->object);
    storage->invoke = nullptr;
    storage->destroy = nullptr;
    storage->object = nullptr;
    storage->~FCallableTaskStorage();
    if (pool->callable_node_pool && pool->callable_node_pool->Contains(storage)) {
        pool->callable_node_pool->Free(storage);
    } else {
        ::HeapFree(::GetProcessHeap(), 0, storage);
    }
}

/** 現在の同期・割り当て診断値を返す。 */
FThreadPoolDiagnostics CThreadPool::Diagnostics() noexcept
{
    /** 診断取得中の PoolState 寿命を固定する pin。 */
    FPoolPin pin;
    /** 診断対象のプール。 */
    FPoolState* const pool = pin.Get();
    if (!pool) return {};
    /** 読み取った診断値。 */
    FThreadPoolDiagnostics diagnostics{};
    diagnostics.submission_lock_acquisitions = pool->submission_lock_acquisitions.Load(EMemoryOrder::Acquire);
    diagnostics.submission_drain_lock_acquisitions = pool->submission_drain_lock_acquisitions.Load(EMemoryOrder::Acquire);
    diagnostics.submission_lock_contentions = pool->submission_lock_contentions.Load(EMemoryOrder::Acquire);
    diagnostics.external_tasks_drained = pool->external_tasks_drained.Load(EMemoryOrder::Acquire);
    diagnostics.worker_parks = pool->worker_parks.Load(EMemoryOrder::Acquire);
    diagnostics.wake_one_calls = pool->wake_one_calls.Load(EMemoryOrder::Acquire);
    diagnostics.wake_all_calls = pool->wake_all_calls.Load(EMemoryOrder::Acquire);
    diagnostics.submission_heap_fallbacks = pool->submission_heap_fallbacks.Load(EMemoryOrder::Acquire);
    diagnostics.callable_inline_submissions = pool->callable_inline_submissions.Load(EMemoryOrder::Acquire);
    diagnostics.callable_heap_submissions = pool->callable_heap_submissions.Load(EMemoryOrder::Acquire);
    diagnostics.callable_node_heap_fallbacks = pool->callable_node_heap_fallbacks.Load(EMemoryOrder::Acquire);
    diagnostics.queued_work = pool->queued_work.Load(EMemoryOrder::Acquire);
    return diagnostics;
}

/** ParallelFor の一時 context 格納診断値を返す。 */
FParallelForDiagnostics CThreadPool::CaptureParallelForDiagnostics() noexcept
{
    /** 診断取得中の PoolState 寿命を固定する pin。 */
    FPoolPin pin;
    /** 診断対象のプール。 */
    FPoolState* const pool = pin.Get();
    if (!pool) return {};
    /** 読み取った ParallelFor 診断値。 */
    FParallelForDiagnostics diagnostics{};
    diagnostics.inline_calls = pool->parallel_for_inline_calls.Load(EMemoryOrder::Acquire);
    diagnostics.pool_blocks = pool->parallel_for_pool_blocks.Load(EMemoryOrder::Acquire);
    diagnostics.pool_blocks_in_use = pool->parallel_for_pool_blocks_in_use.Load(EMemoryOrder::Acquire);
    diagnostics.pool_blocks_high_water = pool->parallel_for_pool_blocks_high_water.Load(EMemoryOrder::Acquire);
    diagnostics.heap_blocks = pool->parallel_for_heap_blocks.Load(EMemoryOrder::Acquire);
    return diagnostics;
}

/** ThreadPool と ParallelFor の診断カウンタを 0 に戻す。 */
void CThreadPool::ResetDiagnostics() noexcept
{
    FPoolPin pin;
    FPoolState* const pool = pin.Get();
    if (!pool) return;
    pool->parallel_for_inline_calls.Store(0, EMemoryOrder::Release);
    pool->parallel_for_pool_blocks.Store(0, EMemoryOrder::Release);
    /** Reset 中も有効な貸し出し数。 */
    const u64 parallel_for_blocks_in_use = pool->parallel_for_pool_blocks_in_use.Load(EMemoryOrder::Acquire);
    pool->parallel_for_pool_blocks_high_water.Store(parallel_for_blocks_in_use, EMemoryOrder::Release);
    pool->parallel_for_heap_blocks.Store(0, EMemoryOrder::Release);
    pool->submission_lock_acquisitions.Store(0, EMemoryOrder::Release);
    pool->submission_drain_lock_acquisitions.Store(0, EMemoryOrder::Release);
    pool->submission_lock_contentions.Store(0, EMemoryOrder::Release);
    pool->external_tasks_drained.Store(0, EMemoryOrder::Release);
    pool->worker_parks.Store(0, EMemoryOrder::Release);
    pool->wake_one_calls.Store(0, EMemoryOrder::Release);
    pool->wake_all_calls.Store(0, EMemoryOrder::Release);
    pool->submission_heap_fallbacks.Store(0, EMemoryOrder::Release);
    pool->callable_inline_submissions.Store(0, EMemoryOrder::Release);
    pool->callable_heap_submissions.Store(0, EMemoryOrder::Release);
    pool->callable_node_heap_fallbacks.Store(0, EMemoryOrder::Release);
}

/** タスクを投入する (自ワーカーなら deque、外部ならグローバルキュー経由)。 */
TResult<void> CThreadPool::Submit(const FTask& t) noexcept {
    FPoolPin pin;
    FPoolState* const pool = pin.Get();
    if (!pool) return ACS_ERR(Threading, 3, "CThreadPool not initialized");
    if (!t.fn)   return ACS_ERR(Threading, 4, "Task fn is null");

    pool->api_lifetime.active_submitters.FetchAdd(1);
    const bool is_child_submit = tls_executing_pool == pool;
    if (pool->accepting.Load(EMemoryOrder::Acquire) == 0 && !is_child_submit) {
        pool->api_lifetime.active_submitters.FetchSub(1);
        return ACS_ERR(Threading, 8, "CThreadPool is shutting down");
    }

    if (t.counter) t.counter->Add(1);
    pool->outstanding.FetchAdd(1);

    // 自ワーカーの deque へ Push を試みる（ローカリティ最適）
    const u32 wi = tls_worker_index;
    if (wi != kNotAWorker && wi < pool->worker_count && is_child_submit) {
        pool->queued_work.FetchAdd(1);
        if (pool->workers[wi].deque.Push(t)) {
            pool->api_lifetime.active_submitters.FetchSub(1);
            WakeOneIfSleeping(pool);
            return Ok();
        }
        pool->queued_work.FetchSub(1);
        // 自 deque が満杯ならグローバルキューにフォールバック
    }

    // 外部キューにエンキュー（HeapAlloc 回避のためノードプールから取る）
    FSubmissionNode* node = AcquireSubmitNode(pool);
    if (!node) {
        if (t.counter) t.counter->Done();
        pool->outstanding.FetchSub(1);
        pool->api_lifetime.active_submitters.FetchSub(1);
        return ACS_ERR(Memory, 4, "Submission node alloc failed");
    }
    node->next = nullptr;
    node->task = t;
    {
        if (!pool->submit.lock.TryLock()) {
            pool->submission_lock_contentions.FetchAdd(1);
            pool->submit.lock.Lock();
        }
        pool->submission_lock_acquisitions.FetchAdd(1);
        pool->queued_work.FetchAdd(1);
        pool->external_queued_work.FetchAdd(1);
        if (pool->submit.tail)
            pool->submit.tail->next = node;
        else
            pool->submit.head = node;
        pool->submit.tail = node;
        ++pool->submit.count;
        pool->submit.lock.Unlock();
    }
    pool->api_lifetime.active_submitters.FetchSub(1);
    WakeOneIfSleeping(pool);
    return Ok();
}

/** counter が 0 になるまで待機する (待機中もスティーリング、無作業なら指数バックオフ)。 */
void CThreadPool::Wait(FCompletionCounter& counter) noexcept {
    FPoolPin pin;
    FPoolState* const pool = pin.Get();
    if (!pool) return;
    u32 self = tls_worker_index;
    u32 idle_iters = 0;  // 連続で仕事を見つけられなかった回数

    while (!counter.Finished()) {
        FTask t {};
        bool got = false;
        if (self != kNotAWorker) {
            if (self < pool->worker_count && pool->workers[self].deque.Pop(t)) {
                pool->queued_work.FetchSub(1);
                got = true;
            }
        }
        if (!got && self != kNotAWorker && self < pool->worker_count && PopSubmitBatch(pool->workers[self], t)) {
            got = true;
        }
        if (!got && pool->external_queued_work.Load(EMemoryOrder::Acquire) != 0 && TryDrainSubmitOne(pool, t)) {
            got = true;
        }
        if (!got && self != kNotAWorker && self < pool->worker_count && TrySteal(pool, self, t)) got = true;
        if (!got && self == kNotAWorker) {
            // 外部スレッドからの Wait — 全ワーカー deque を順に Steal 試行
            for (u32 i = 0; i < pool->worker_count; ++i) {
                if (pool->workers[i].deque.Steal(t)) {
                    pool->queued_work.FetchSub(1);
                    got = true;
                    break;
                }
            }
        }

        if (got) {
            Execute(pool, t, self == kNotAWorker ? 0 : self);
            idle_iters = 0;  // 仕事があったのでカウントリセット
        } else {
            // 指数バックオフ: 短いスピンから始めて、見つからないほど長く待つ
            ++idle_iters;
            if (idle_iters < 16) {
                SpinHint();                  // PAUSE / YIELD で電力節約
            } else if (idle_iters < 64) {
                ::SwitchToThread();          // 同優先度スレッドに譲る
            } else {
                ::Sleep(0);                  // タイムスライスを返す
                if (idle_iters > 1024) idle_iters = 64;  // 上限を抑える
            }
        }
    }
}

namespace {
/**
 * 1 チャンク [begin, end) を順次実行する TaskFn アダプタ。
 *
 * @param arg FParallelForContext へのポインタ。
 * @param worker_index 実行中のワーカーインデックス。
 */
void PFRangeFn(void* arg, u32 worker_index) noexcept {
    /** 実行するチャンク context。 */
    auto* r = static_cast<FParallelForContext*>(arg);
    /** body へ渡すチャンク内 index。 */
    for (u32 i = r->begin; i < r->end; ++i) r->body(i, worker_index, r->user);
}

/**
 * 呼び出しが保持する ParallelFor context block 列をすべて返す。
 *
 * @param pool block の所有元。
 * @param first 返却する block 列の先頭。
 */
void ReleaseParallelForContextBlocks(FPoolState* pool, FParallelForContextBlock* first) noexcept
{
    /** 現在返却する block。 */
    FParallelForContextBlock* block = first;
    while (block) {
        /** 破棄前に退避する次 block。 */
        FParallelForContextBlock* const next = block->next;
        ReleaseParallelForContextBlock(pool, block);
        block = next;
    }
}
} // namespace

/** 範囲 [begin, end) を grain で分割して並列実行し、完了まで待つ。 */
TResult<void> CThreadPool::ParallelFor(u32 begin, u32 end, u32 grain, void (*body)(u32, u32, void*), void* user) noexcept {
    /** context pool を含む PoolState の寿命を呼び出し完了まで固定する。 */
    FPoolPin pin;
    /** ParallelFor を実行する pool。 */
    FPoolState* const pool = pin.Get();
    if (!pool || pool->worker_count == 0) return ACS_ERR(Threading, 5, "CThreadPool not initialized");
    if (!body)   return ACS_ERR(Threading, 6, "ParallelFor body is null");
    if (begin >= end) return Ok();
    if (grain == 0)   grain = 1;

    /** 処理する総 index 数。 */
    const u32 total = end - begin;
    /** overflow を起こさず求めたチャンク数。 */
    const u32 chunks = 1u + (total - 1u) / grain;

    /** 通常規模の呼び出しが確保を行わずに使う stack context。 */
    FParallelForContext inline_contexts[kParallelForInlineContextCapacity]{};
    /** overflow context block 列の先頭。 */
    FParallelForContextBlock* first_block = nullptr;
    /** overflow context block 列の末尾。 */
    FParallelForContextBlock* last_block = nullptr;

    if (chunks <= kParallelForInlineContextCapacity) {
        pool->parallel_for_inline_calls.FetchAdd(1u);
    } else {
        /** 必要な固定長 block 数。 */
        const u32 block_count = 1u + (chunks - 1u) / kParallelForContextBlockCapacity;
        /** 取得する固定長 block の index。 */
        for (u32 block_index = 0u; block_index < block_count; ++block_index) {
            /** 今回取得した再利用 block。 */
            FParallelForContextBlock* const block = AcquireParallelForContextBlock(pool);
            if (!block) {
                ReleaseParallelForContextBlocks(pool, first_block);
                return ACS_ERR(Memory, 5, "ParallelFor range block alloc failed");
            }
            if (last_block) last_block->next = block;
            else first_block = block;
            last_block = block;
        }
    }

    /** 全投入済みチャンクの完了通知先。 */
    FCompletionCounter counter;
    /** overflow 時に現在参照している block。 */
    FParallelForContextBlock* current_block = first_block;
    /** 投入する ParallelFor チャンク index。 */
    for (u32 c = 0; c < chunks; ++c) {
        if (first_block && c != 0u && (c % kParallelForContextBlockCapacity) == 0u) {
            current_block = current_block->next;
        }
        /** 現在チャンクの寿命を保持する context。 */
        FParallelForContext* const range = first_block
            ? &current_block->contexts[c % kParallelForContextBlockCapacity]
            : &inline_contexts[c];
        range->body = body;
        range->user = user;
        range->begin = begin + c * grain;
        range->end = range->begin + grain;
        if (range->end > end) range->end = end;

        /** 現在チャンクを実行する task。 */
        const FTask task{&PFRangeFn, range, &counter};
        /** task の投入結果。 */
        TResult<void> result = Submit(task);
        if (result.IsErr()) {
            // 投入失敗 — 既に投入済みのものを待ってから返す
            Wait(counter);
            ReleaseParallelForContextBlocks(pool, first_block);
            return result;
        }
    }
    Wait(counter);
    ReleaseParallelForContextBlocks(pool, first_block);
    return Ok();
}

} // namespace acs
