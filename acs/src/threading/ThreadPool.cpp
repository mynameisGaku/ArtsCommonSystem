// SPDX-License-Identifier: Apache-2.0
// ワークスティーリング FThreadPool 実装（Chase-Lev SPMC deque + ノードプール）
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
static_assert((kDequeCapacity & kDequeCapacityMask) == 0,
              "Deque capacity must be a power of two");

/** ワーカー数の上限 (PoolState の threads[] サイズと一致)。 */
constexpr u32 kMaxWorkers = 256;

/** 外部投入キューのノードプールサイズ (同時保留タスクの上限)。 */
constexpr u32 kSubmitNodePoolCount = 4096;

/**
 * Chase-Lev ワークスチール deque (single-producer / multi-consumer)。
 *
 * @details
 * オーナーワーカーだけが末尾で Push/Pop し、他ワーカーは先頭から Steal する。
 * top/bottom はアトミックで、release ストア・acquire ロード・最後の 1 個の CAS
 * arbitration により lock-free に競合を解決する。64B 整列で false sharing を避ける。
 */
struct alignas(64) WorkerDeque {
    /** Steal 側 (先頭) のインデックス。他ワーカーが奪う。 */
    TAtomic<i64> top    {0};

    /** オーナー側 (末尾) のインデックス。オーナーが追加・取り出す。 */
    TAtomic<i64> bottom {0};

    /** タスク本体のリングバッファ (値コピーで格納)。 */
    Task        buffer[kDequeCapacity] {};

    /**
     * オーナー専用。末尾にタスクを Push する。
     *
     * @details buffer 書き込み後に bottom を release ストアして公開する。
     * @param t 追加するタスク。
     * @return 追加できたら true、deque が満杯なら false。
     */
    bool Push(const Task& t) noexcept {
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
    bool Pop(Task& out) noexcept {
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
    bool Steal(Task& out) noexcept {
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

/** 外部投入キューの単方向リストノード (プール外スレッドからの Submit を保持)。 */
struct SubmissionNode {
    /** 次のノード (末尾は nullptr)。 */
    SubmissionNode* next;

    /** このノードが保持するタスク。 */
    Task            task;
};

/** FMutex 保護の FIFO 投入キュー (プール外スレッドからの Submit が入る)。 */
struct SubmissionQueue {
    /** キュー操作を保護する排他ロック。 */
    FMutex            lock;

    /** 先頭ノード (次に drain される、空なら nullptr)。 */
    SubmissionNode*  head = nullptr;

    /** 末尾ノード (次に Push される位置、空なら nullptr)。 */
    SubmissionNode*  tail = nullptr;

    /** 現在キューに溜まっているタスク数。 */
    u32              count = 0;
};

/**
 * ワーカーローカルの状態 (deque + スティーリング用 RNG)。
 *
 * @details 64B 整列で隣接ワーカーとの false sharing を避ける。
 */
struct PoolState;

struct alignas(64) Worker {
    /** このワーカーのワークスチール deque。 */
    WorkerDeque  deque;

    /** ワーカーインデックス (0..N-1)。 */
    u32          index;

    /** スティーリング先選択用 xorshift32 の状態。 */
    TAtomic<u32>  rng_state;

    /** このワーカーを所有するプール。停止処理中もグローバル参照に依存しないため保持する。 */
    PoolState* owner = nullptr;
};

/** スレッドプール全体の状態 (シングルトン)。 */
struct PoolState {
    /** ワーカー配列 (64B 整列で確保)。 */
    Worker*           workers      = nullptr;

    /** workers のために HeapAlloc が返した元ポインタ (解放時は整列後ポインタではなくこちらを使う)。 */
    void* worker_allocation = nullptr;

    /** ワーカー数。 */
    u32               worker_count = 0;

    /** 各ワーカーのスレッドハンドル。 */
    FThread            threads[kMaxWorkers];

    /** 動作フラグ (1=動作中, 0=停止要求)。 */
    TAtomic<u32>       running      {0};

    /** 外部スレッドから新しい仕事を受け付ける間は 1。終了開始時に 0 へ遷移する。 */
    TAtomic<u32> accepting{0};

    /** 受理済みで、まだコールバックから戻っていないタスク数。 */
    TAtomic<u32> outstanding{0};

    /** 公開 API がこの PoolState を参照している数。解放前のライフタイム障壁に使う。 */
    TAtomic<u32> api_users{0};

    /** Submit が受付判定から公開完了までの区間にいる呼び出し数。終了開始時の障壁に使う。 */
    TAtomic<u32> active_submitters{0};

    /** 外部投入キュー。 */
    SubmissionQueue   submit;

    /** wake_cv とペアで使う、ワーカー park 用のロック。 */
    FMutex             wake_lock;

    /** 仕事が来たときに park 中のワーカーを起こす条件変数。 */
    ConditionVar      wake_cv;

    /** 外部投入ノードを HeapAlloc せず取るための固定サイズプール。 */
    FPoolAllocator*    submit_node_pool = nullptr;
};

/** プール全体の状態へのグローバルポインタ (null = 未初期化)。 */
PoolState* g_pool = nullptr;

/** g_pool の差し替えと公開 API の参照取得を直列化する。 */
FMutex g_lifecycle_lock;

/** Init/Shutdown 同士を直列化する。長い終了待機中に Submit はこのロックを使わない。 */
FMutex g_init_shutdown_lock;

/** ワーカースレッドだけが自分のインデックスを持つ TLS (ワーカー外は kNotAWorker)。 */
ACS_THREAD_LOCAL u32 tls_worker_index = FThreadPool::kNotAWorker;

/** 現在実行しているタスクの所有プール。外部スレッドが Wait 中に実行する場合も設定する。 */
ACS_THREAD_LOCAL PoolState* tls_executing_pool = nullptr;

/**
 * 公開 API の実行中だけ PoolState の寿命を保持する小さなピン。
 *
 * @details g_pool 読み取りと api_users 加算を同じロック区間で行うため、Shutdown が
 * g_pool を外してから状態を解放する処理と競合しない。
 */
class PoolPin {
public:
    PoolPin() noexcept
    {
        FScopedLock lock(g_lifecycle_lock);
        m_Pool = g_pool;
        if (m_Pool) m_Pool->api_users.FetchAdd(1);
    }

    ~PoolPin() noexcept
    {
        if (m_Pool) m_Pool->api_users.FetchSub(1);
    }

    PoolPin(const PoolPin&) = delete;
    PoolPin& operator=(const PoolPin&) = delete;

    PoolState* Get() const noexcept
    {
        return m_Pool;
    }

private:
    PoolState* m_Pool = nullptr;
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
SubmissionNode* AcquireSubmitNode(PoolState* pool) noexcept
{
    if (pool->submit_node_pool) {
        void* const p = pool->submit_node_pool->Alloc(sizeof(SubmissionNode), alignof(SubmissionNode),
                                                      FSourceLoc::Current());
        if (p) return static_cast<SubmissionNode*>(p);
    }
    // フォールバック: プール枯渇時は Heap から取る
    return static_cast<SubmissionNode*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(SubmissionNode)));
}

/**
 * 投入ノードを返却する (プール範囲内ならプールへ、それ以外なら Heap へ)。
 *
 * @param n 返却するノード (nullptr は無視)。
 */
void ReleaseSubmitNode(PoolState* state, SubmissionNode* n) noexcept
{
    if (!n) return;
    FPoolAllocator* pool = state->submit_node_pool;
    if (pool && pool->Contains(n)) {
        pool->Free(n);
    } else {
        ::HeapFree(::GetProcessHeap(), 0, n);
    }
}

/**
 * 外部投入キューから 1 件取り出す。
 *
 * @details ロックは TryLock で取り、取れなければ即 false (ブロックしない)。
 * @param out 取り出したタスクの書き込み先。
 * @return 取り出せたら true。
 */
bool TryDrainSubmit(PoolState* pool, Task& out) noexcept
{
    SubmissionQueue& q = pool->submit;
    if (!q.lock.TryLock()) return false;
    bool got = false;
    SubmissionNode* free_me = nullptr;
    if (q.head) {
        SubmissionNode* const n = q.head;
        out = n->task;
        q.head = n->next;
        if (!q.head) q.tail = nullptr;
        --q.count;
        free_me = n;
        got = true;
    }
    q.lock.Unlock();
    if (free_me) ReleaseSubmitNode(pool, free_me);
    return got;
}

/**
 * 自分以外のワーカーからタスクの Steal を試みる (ランダム開始 + 巡回)。
 *
 * @param self_index 呼び出し元ワーカーのインデックス (このワーカーは奪わない)。
 * @param out 奪ったタスクの書き込み先。
 * @return いずれかのワーカーから奪えたら true。
 */
bool TrySteal(PoolState* pool, u32 self_index, Task& out) noexcept
{
    const u32 n = pool->worker_count;
    if (n <= 1) return false;
    Worker& self = pool->workers[self_index];
    u32 victim = XorShift32(self.rng_state) % n;
    for (u32 i = 0; i < n; ++i) {
        if (victim != self_index) {
            if (pool->workers[victim].deque.Steal(out)) return true;
        }
        victim = (victim + 1) % n;
    }
    return false;
}

/**
 * タスクを実行し、完了通知 (counter->Done) も行う。
 *
 * @param t 実行するタスク (fn が null なら実行をスキップ)。
 * @param worker_index 実行中のワーカーインデックス (fn に渡す)。
 */
ACS_FORCEINLINE void Execute(PoolState* pool, const Task& t, u32 worker_index) noexcept
{
    PoolState* const previous_pool = tls_executing_pool;
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
 * running が真の間、自 deque pop → 外部キュー drain → 他ワーカー steal の順に
 * 仕事を探し、すべて空なら wake_cv で短時間 park する (NotifyOne で起きる)。
 * @param arg 担当する Worker へのポインタ。
 */
void WorkerMain(void* arg) noexcept {
    Worker* w = static_cast<Worker*>(arg);
    PoolState* const pool = w->owner;
    tls_worker_index = w->index;
    w->rng_state.Store(0x9E3779B9u ^ (w->index * 2654435761u), EMemoryOrder::Relaxed);

    while (pool->running.Load(EMemoryOrder::Acquire)) {
        Task t {};
        if (w->deque.Pop(t)) {
            Execute(pool, t, w->index);
            continue;
        }
        if (TryDrainSubmit(pool, t)) {
            Execute(pool, t, w->index);
            continue;
        }
        if (TrySteal(pool, w->index, t)) {
            Execute(pool, t, w->index);
            continue;
        }

        // 全て空なら CV で短時間スリープ（NotifyOne で起きる）
        FScopedLock lk(pool->wake_lock);
        pool->wake_cv.WaitFor(pool->wake_lock, 2);
    }
    tls_worker_index = FThreadPool::kNotAWorker;
}

} // namespace

/** プールを初期化し、ワーカースレッドを起動する。 */
TResult<void> FThreadPool::Init(u32 worker_count) noexcept {
    FScopedLock operation_lock(g_init_shutdown_lock);
    FScopedLock lifecycle_lock(g_lifecycle_lock);
    if (g_pool != nullptr) return ACS_ERR(Threading, 2, "FThreadPool already initialized");
    if (worker_count == 0) worker_count = HardwareConcurrency();
    if (worker_count > kMaxWorkers) worker_count = kMaxWorkers;

    // PoolState をプロセスヒープから 0 クリア確保
    void* mem = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PoolState));
    if (!mem) return ACS_ERR(Memory, 2, "FThreadPool state alloc failed");
    g_pool = ::new (mem) PoolState();

    // ワーカー配列を 64B 境界整列で確保
    usize total = sizeof(Worker) * worker_count + 64;
    void* wmem = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!wmem) {
        g_pool->~PoolState();
        ::HeapFree(::GetProcessHeap(), 0, g_pool);
        g_pool = nullptr;
        return ACS_ERR(Memory, 3, "FThreadPool worker alloc failed");
    }
    uptr aligned = (reinterpret_cast<uptr>(wmem) + 63) & ~uptr{63};
    g_pool->worker_allocation = wmem;
    g_pool->workers = reinterpret_cast<Worker*>(aligned);
    for (u32 i = 0; i < worker_count; ++i) {
        ::new (&g_pool->workers[i]) Worker();
        g_pool->workers[i].index = i;
        g_pool->workers[i].owner = g_pool;
    }
    g_pool->worker_count = worker_count;

    // 外部投入ノード用のプールを構築（HeapAlloc syscall を回避）
    void* pool_mem = ::HeapAlloc(::GetProcessHeap(), 0, sizeof(FPoolAllocator));
    if (!pool_mem) {
        ::HeapFree(::GetProcessHeap(), 0, wmem);
        g_pool->~PoolState();
        ::HeapFree(::GetProcessHeap(), 0, g_pool);
        g_pool = nullptr;
        return ACS_ERR(Memory, 7, "FThreadPool submit pool alloc failed");
    }
    g_pool->submit_node_pool = ::new (pool_mem) FPoolAllocator(
        sizeof(SubmissionNode), kSubmitNodePoolCount, alignof(SubmissionNode));

    g_pool->accepting.Store(1, EMemoryOrder::Release);
    g_pool->running.Store(1, EMemoryOrder::Release);

    // ワーカースレッド起動
    for (u32 i = 0; i < worker_count; ++i) {
        ThreadConfig cfg {};
        cfg.name = L"acs::FThreadPool worker";
        auto r = FThread::Spawn(&WorkerMain, &g_pool->workers[i], cfg);
        if (r.IsErr()) {
            // 失敗時のロールバック
            g_pool->running.Store(0, EMemoryOrder::Release);
            g_pool->wake_cv.NotifyAll();
            for (u32 j = 0; j < i; ++j) g_pool->threads[j].Join();
            g_pool->submit_node_pool->~FPoolAllocator();
            ::HeapFree(::GetProcessHeap(), 0, g_pool->submit_node_pool);
            ::HeapFree(::GetProcessHeap(), 0, wmem);
            g_pool->~PoolState();
            ::HeapFree(::GetProcessHeap(), 0, g_pool);
            g_pool = nullptr;
            return Err<void>(r.Error());
        }
        g_pool->threads[i] = Move(r.Value());
    }
    return Ok();
}

/** 全受理済みタスクを排出してからワーカーを停止・Join し、プールのリソースを解放する。 */
void FThreadPool::Shutdown() noexcept {
    // 実行中タスクから自分自身を Join すると永久待機になる。外部所有者が改めて終了する。
    if (tls_executing_pool != nullptr) return;

    FScopedLock operation_lock(g_init_shutdown_lock);

    PoolState* pool = nullptr;
    {
        FScopedLock lifecycle_lock(g_lifecycle_lock);
        pool = g_pool;
        if (!pool) return;
        // ここから外部 Submit は拒否する。既に走っているタスクが作る子タスクだけは
        // 依存グラフを途中で切らないため、Submit 側で引き続き受理する。
        pool->accepting.Store(0, EMemoryOrder::Release);
    }

    pool->wake_cv.NotifyAll();

    // 受付判定を通過済みの Submit がキュー公開または失敗巻き戻しを終えるまで待つ。
    while (pool->active_submitters.Load(EMemoryOrder::Acquire) != 0)
        Yield();

    // outstanding はコールバックから戻った後に減る。従って 0 ならキュー・deque・
    // 実行中タスクのいずれにも受理済み仕事が残っていない。
    while (pool->outstanding.Load(EMemoryOrder::Acquire) != 0) {
        pool->wake_cv.NotifyAll();
        SleepMs(1);
    }

    pool->running.Store(0, EMemoryOrder::Release);
    pool->wake_cv.NotifyAll();
    for (u32 i = 0; i < pool->worker_count; ++i)
        pool->threads[i].Join();

    // 以降の公開 API が古い状態を取得できないようグローバル参照を先に外す。
    {
        FScopedLock lifecycle_lock(g_lifecycle_lock);
        if (g_pool == pool) g_pool = nullptr;
    }
    while (pool->api_users.Load(EMemoryOrder::Acquire) != 0)
        Yield();

    // outstanding == 0 なら通常は空。防御的にノードだけを回収する。
    {
        FScopedLock lk(pool->submit.lock);
        SubmissionNode* n = pool->submit.head;
        while (n) {
            SubmissionNode* nx = n->next;
            ReleaseSubmitNode(pool, n);
            n = nx;
        }
        pool->submit.head = nullptr;
        pool->submit.tail = nullptr;
        pool->submit.count = 0;
    }

    // ノードプール破棄
    if (pool->submit_node_pool) {
        pool->submit_node_pool->~FPoolAllocator();
        ::HeapFree(::GetProcessHeap(), 0, pool->submit_node_pool);
        pool->submit_node_pool = nullptr;
    }

    // ワーカー破棄
    Worker* base = pool->workers;
    for (u32 i = 0; i < pool->worker_count; ++i)
        base[i].~Worker();
    pool->workers = nullptr;
    pool->worker_count = 0;

    // HeapFree は HeapAlloc が返した元ポインタに対してのみ有効。64B 整列後の
    // workers を渡すと失敗し、Init/Shutdown のたびにワーカー配列が残り続ける。
    ::HeapFree(::GetProcessHeap(), 0, pool->worker_allocation);
    pool->worker_allocation = nullptr;

    pool->~PoolState();
    ::HeapFree(::GetProcessHeap(), 0, pool);
}

/** 起動中のワーカー数を返す (未初期化なら 0)。 */
u32 FThreadPool::WorkerCount() noexcept {
    PoolPin pin;
    return pin.Get() ? pin.Get()->worker_count : 0;
}

/** 呼び出しスレッドのワーカーインデックスを返す (非ワーカーは kNotAWorker)。 */
u32 FThreadPool::CurrentWorkerIndex() noexcept {
    return tls_worker_index;
}

/** タスクを投入する (自ワーカーなら deque、外部ならグローバルキュー経由)。 */
TResult<void> FThreadPool::Submit(const Task& t) noexcept {
    PoolPin pin;
    PoolState* const pool = pin.Get();
    if (!pool) return ACS_ERR(Threading, 3, "FThreadPool not initialized");
    if (!t.fn)   return ACS_ERR(Threading, 4, "Task fn is null");

    pool->active_submitters.FetchAdd(1);
    const bool is_child_submit = tls_executing_pool == pool;
    if (pool->accepting.Load(EMemoryOrder::Acquire) == 0 && !is_child_submit) {
        pool->active_submitters.FetchSub(1);
        return ACS_ERR(Threading, 8, "FThreadPool is shutting down");
    }

    if (t.counter) t.counter->Add(1);
    pool->outstanding.FetchAdd(1);

    // 自ワーカーの deque へ Push を試みる（ローカリティ最適）
    const u32 wi = tls_worker_index;
    if (wi != kNotAWorker && wi < pool->worker_count && is_child_submit) {
        if (pool->workers[wi].deque.Push(t)) {
            pool->active_submitters.FetchSub(1);
            pool->wake_cv.NotifyOne();
            return Ok();
        }
        // 自 deque が満杯ならグローバルキューにフォールバック
    }

    // 外部キューにエンキュー（HeapAlloc 回避のためノードプールから取る）
    SubmissionNode* node = AcquireSubmitNode(pool);
    if (!node) {
        if (t.counter) t.counter->Done();
        pool->outstanding.FetchSub(1);
        pool->active_submitters.FetchSub(1);
        return ACS_ERR(Memory, 4, "Submission node alloc failed");
    }
    node->next = nullptr;
    node->task = t;
    {
        FScopedLock lk(pool->submit.lock);
        if (pool->submit.tail)
            pool->submit.tail->next = node;
        else
            pool->submit.head = node;
        pool->submit.tail = node;
        ++pool->submit.count;
    }
    pool->active_submitters.FetchSub(1);
    pool->wake_cv.NotifyOne();
    return Ok();
}

/** counter が 0 になるまで待機する (待機中もスティーリング、無作業なら指数バックオフ)。 */
void FThreadPool::Wait(CompletionCounter& counter) noexcept {
    PoolPin pin;
    PoolState* const pool = pin.Get();
    if (!pool) return;
    u32 self = tls_worker_index;
    u32 idle_iters = 0;  // 連続で仕事を見つけられなかった回数

    while (!counter.Finished()) {
        Task t {};
        bool got = false;
        if (self != kNotAWorker) {
            if (self < pool->worker_count && pool->workers[self].deque.Pop(t)) got = true;
        }
        if (!got && TryDrainSubmit(pool, t)) got = true;
        if (!got && self != kNotAWorker && self < pool->worker_count && TrySteal(pool, self, t)) got = true;
        if (!got && self == kNotAWorker) {
            // 外部スレッドからの Wait — 全ワーカー deque を順に Steal 試行
            for (u32 i = 0; i < pool->worker_count; ++i) {
                if (pool->workers[i].deque.Steal(t)) {
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
/** ParallelFor の 1 チャンク分の実行コンテキスト。 */
struct PFContext {
    /** 各インデックスに対して呼ぶユーザー関数 (i, worker_index, user)。 */
    void (*body)(u32 i, u32 worker_index, void* user);

    /** body に渡すユーザーデータ。 */
    void* user;

    /** このチャンクの開始インデックス (含む)。 */
    u32 begin;

    /** このチャンクの終了インデックス (含まない)。 */
    u32 end;
};

/**
 * 1 チャンク [begin, end) を順次実行する TaskFn アダプタ。
 *
 * @param arg PFContext へのポインタ。
 * @param worker_index 実行中のワーカーインデックス。
 */
void PFRangeFn(void* arg, u32 worker_index) noexcept {
    auto* r = static_cast<PFContext*>(arg);
    for (u32 i = r->begin; i < r->end; ++i) r->body(i, worker_index, r->user);
}
} // namespace

/** 範囲 [begin, end) を grain で分割して並列実行し、完了まで待つ。 */
TResult<void> FThreadPool::ParallelFor(u32 begin, u32 end, u32 grain,
                                     void (*body)(u32, u32, void*), void* user) noexcept {
    if (WorkerCount() == 0) return ACS_ERR(Threading, 5, "FThreadPool not initialized");
    if (!body)   return ACS_ERR(Threading, 6, "ParallelFor body is null");
    if (begin >= end) return Ok();
    if (grain == 0)   grain = 1;

    const u32 total = end - begin;
    const u32 chunks = (total + grain - 1) / grain;

    // チャンクごとの PFContext を 1 つの連続ブロックで確保
    auto* ranges = static_cast<PFContext*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(PFContext) * chunks));
    if (!ranges) return ACS_ERR(Memory, 5, "ParallelFor range alloc failed");

    CompletionCounter counter;
    for (u32 c = 0; c < chunks; ++c) {
        ranges[c].body  = body;
        ranges[c].user  = user;
        ranges[c].begin = begin + c * grain;
        ranges[c].end   = ranges[c].begin + grain;
        if (ranges[c].end > end) ranges[c].end = end;

        const Task t { &PFRangeFn, &ranges[c], &counter };
        auto r = Submit(t);
        if (r.IsErr()) {
            // 投入失敗 — 既に投入済みのものを待ってから返す
            Wait(counter);
            ::HeapFree(::GetProcessHeap(), 0, ranges);
            return r;
        }
    }
    Wait(counter);
    ::HeapFree(::GetProcessHeap(), 0, ranges);
    return Ok();
}

} // namespace acs
