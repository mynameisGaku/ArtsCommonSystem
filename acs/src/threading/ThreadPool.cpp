// SPDX-License-Identifier: Apache-2.0
// ワークスティーリング ThreadPool 実装（Chase-Lev SPMC deque + ノードプール）
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

// ワーカーごとの deque サイズ（2 のべき乗）
constexpr i64 kDequeCapacity     = 4096;
constexpr i64 kDequeCapacityMask = kDequeCapacity - 1;
static_assert((kDequeCapacity & kDequeCapacityMask) == 0,
              "Deque capacity must be a power of two");

// ワーカー数の上限（PoolState の threads[] サイズと一致）
constexpr u32 kMaxWorkers = 256;

// 外部投入キューのノードプールサイズ（同時保留上限）
constexpr u32 kSubmitNodePoolCount = 4096;

// Chase-Lev SPMC deque（オーナーは Push/Pop、他スレッドは Steal）
struct alignas(64) WorkerDeque {
    Atomic<i64> top    {0};                 // Steal が奪う側
    Atomic<i64> bottom {0};                 // オーナーが追加・取り出す側
    Task        buffer[kDequeCapacity] {};  // タスク本体（値コピー）

    // オーナー専用: 末尾に Push（buffer 書き込み後に release で公開）
    bool Push(const Task& t) noexcept {
        i64 b = bottom.Load(MemoryOrder::Relaxed);
        i64 tt = top.Load(MemoryOrder::Acquire);
        if (b - tt >= kDequeCapacity) return false;  // 満杯
        buffer[b & kDequeCapacityMask] = t;
        bottom.Store(b + 1, MemoryOrder::Release);
        return true;
    }

    // オーナー専用: 末尾から Pop
    bool Pop(Task& out) noexcept {
        i64 b = bottom.Load(MemoryOrder::Relaxed) - 1;
        bottom.Store(b, MemoryOrder::Relaxed);
        HardwareFence();  // bottom 更新と top 読み取りの順序付け
        i64 tt = top.Load(MemoryOrder::Relaxed);
        if (tt <= b) {
            out = buffer[b & kDequeCapacityMask];
            if (tt != b) return true;  // 通常ケース
            // 最後の 1 個 — Steal と競合する可能性、CAS で arbitrate
            i64 expected = tt;
            bool ok = top.CompareExchange(expected, tt + 1);
            bottom.Store(b + 1, MemoryOrder::Relaxed);
            return ok;
        }
        // deque は空 — 底を元に戻す
        bottom.Store(b + 1, MemoryOrder::Relaxed);
        return false;
    }

    // 他ワーカー: 先頭から Steal
    bool Steal(Task& out) noexcept {
        i64 tt = top.Load(MemoryOrder::Acquire);
        HardwareFence();
        i64 b = bottom.Load(MemoryOrder::Acquire);
        if (tt < b) {
            out = buffer[tt & kDequeCapacityMask];
            i64 expected = tt;
            return top.CompareExchange(expected, tt + 1);
        }
        return false;
    }
};

// 外部投入キュー（プール外スレッドからの Submit が入る Mutex 保護のリスト）
struct SubmissionNode {
    SubmissionNode* next;
    Task            task;
};

struct SubmissionQueue {
    Mutex            lock;
    SubmissionNode*  head = nullptr;
    SubmissionNode*  tail = nullptr;
    u32              count = 0;
};

// ワーカーローカル状態（deque + RNG）
struct alignas(64) Worker {
    WorkerDeque  deque;
    u32          index;
    Atomic<u32>  rng_state;   // スティーリング先選択用 xorshift32
};

// プール全体の状態
struct PoolState {
    Worker*           workers      = nullptr;
    u32               worker_count = 0;
    Thread            threads[kMaxWorkers];
    Atomic<u32>       running      {0};                    // 1=動作中, 0=停止
    SubmissionQueue   submit;
    Mutex             wake_lock;
    ConditionVar      wake_cv;

    // 外部投入ノードを HeapAlloc せず、固定プールから取る
    PoolAllocator*    submit_node_pool = nullptr;
};

PoolState* g_pool = nullptr;

// ワーカースレッドだけが持つ TLS（ワーカー外は kNotAWorker）
ACS_THREAD_LOCAL u32 tls_worker_index = ThreadPool::kNotAWorker;

// xorshift32（スティーリング先選択用の小さな PRNG）
ACS_FORCEINLINE u32 XorShift32(Atomic<u32>& s) noexcept {
    u32 x = s.Load(MemoryOrder::Relaxed);
    if (x == 0) x = 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.Store(x, MemoryOrder::Relaxed);
    return x;
}

// SubmissionNode を取得（プール優先、枯渇時のみ HeapAlloc）
SubmissionNode* AcquireSubmitNode() noexcept {
    if (g_pool->submit_node_pool) {
        void* p = g_pool->submit_node_pool->Alloc(sizeof(SubmissionNode), alignof(SubmissionNode), SourceLoc::Current());
        if (p) return static_cast<SubmissionNode*>(p);
    }
    // フォールバック: プール枯渇時は Heap から取る
    return static_cast<SubmissionNode*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(SubmissionNode)));
}

// SubmissionNode を返却（プール範囲内ならプール、それ以外なら Heap）
void ReleaseSubmitNode(SubmissionNode* n) noexcept {
    if (!n) return;
    PoolAllocator* pool = g_pool->submit_node_pool;
    if (pool && pool->Contains(n)) {
        pool->Free(n);
    } else {
        ::HeapFree(::GetProcessHeap(), 0, n);
    }
}

// 外部投入キューから 1 件取り出す（取れたら true）
bool TryDrainSubmit(Task& out) noexcept {
    SubmissionQueue& q = g_pool->submit;
    if (!q.lock.TryLock()) return false;
    bool got = false;
    SubmissionNode* free_me = nullptr;
    if (q.head) {
        SubmissionNode* n = q.head;
        out = n->task;
        q.head = n->next;
        if (!q.head) q.tail = nullptr;
        --q.count;
        free_me = n;
        got = true;
    }
    q.lock.Unlock();
    if (free_me) ReleaseSubmitNode(free_me);
    return got;
}

// 自分以外のワーカーから Steal を試みる（ランダム開始 + 巡回）
bool TrySteal(u32 self_index, Task& out) noexcept {
    u32 n = g_pool->worker_count;
    if (n <= 1) return false;
    Worker& self = g_pool->workers[self_index];
    u32 victim = XorShift32(self.rng_state) % n;
    for (u32 i = 0; i < n; ++i) {
        if (victim != self_index) {
            if (g_pool->workers[victim].deque.Steal(out)) return true;
        }
        victim = (victim + 1) % n;
    }
    return false;
}

// タスクを実行し、完了通知も行う
ACS_FORCEINLINE void Execute(const Task& t, u32 worker_index) noexcept {
    if (t.fn) t.fn(t.user, worker_index);
    if (t.counter) t.counter->Done();
}

// ワーカースレッド本体（1: 自 deque pop → 2: 外部キュー drain → 3: steal → 4: park）
void WorkerMain(void* arg) noexcept {
    Worker* w = static_cast<Worker*>(arg);
    tls_worker_index = w->index;
    w->rng_state.Store(0x9E3779B9u ^ (w->index * 2654435761u), MemoryOrder::Relaxed);

    while (g_pool->running.Load(MemoryOrder::Acquire)) {
        Task t {};
        if (w->deque.Pop(t))            { Execute(t, w->index); continue; }
        if (TryDrainSubmit(t))          { Execute(t, w->index); continue; }
        if (TrySteal(w->index, t))      { Execute(t, w->index); continue; }

        // 全て空なら CV で短時間スリープ（NotifyOne で起きる）
        ScopedLock lk(g_pool->wake_lock);
        g_pool->wake_cv.WaitFor(g_pool->wake_lock, 2);
    }
    tls_worker_index = ThreadPool::kNotAWorker;
}

} // namespace

// プール初期化
Result<void> ThreadPool::Init(u32 worker_count) noexcept {
    if (g_pool != nullptr) return ACS_ERR(Threading, 2, "ThreadPool already initialized");
    if (worker_count == 0) worker_count = HardwareConcurrency();
    if (worker_count > kMaxWorkers) worker_count = kMaxWorkers;

    // PoolState をプロセスヒープから 0 クリア確保
    void* mem = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PoolState));
    if (!mem) return ACS_ERR(Memory, 2, "ThreadPool state alloc failed");
    g_pool = ::new (mem) PoolState();

    // ワーカー配列を 64B 境界整列で確保
    usize total = sizeof(Worker) * worker_count + 64;
    void* wmem = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!wmem) {
        g_pool->~PoolState();
        ::HeapFree(::GetProcessHeap(), 0, g_pool);
        g_pool = nullptr;
        return ACS_ERR(Memory, 3, "ThreadPool worker alloc failed");
    }
    uptr aligned = (reinterpret_cast<uptr>(wmem) + 63) & ~uptr{63};
    g_pool->workers = reinterpret_cast<Worker*>(aligned);
    for (u32 i = 0; i < worker_count; ++i) {
        ::new (&g_pool->workers[i]) Worker();
        g_pool->workers[i].index = i;
    }
    g_pool->worker_count = worker_count;

    // 外部投入ノード用のプールを構築（HeapAlloc syscall を回避）
    void* pool_mem = ::HeapAlloc(::GetProcessHeap(), 0, sizeof(PoolAllocator));
    if (!pool_mem) {
        ::HeapFree(::GetProcessHeap(), 0, wmem);
        g_pool->~PoolState();
        ::HeapFree(::GetProcessHeap(), 0, g_pool);
        g_pool = nullptr;
        return ACS_ERR(Memory, 7, "ThreadPool submit pool alloc failed");
    }
    g_pool->submit_node_pool = ::new (pool_mem) PoolAllocator(
        sizeof(SubmissionNode), kSubmitNodePoolCount, alignof(SubmissionNode));

    g_pool->running.Store(1, MemoryOrder::Release);

    // ワーカースレッド起動
    for (u32 i = 0; i < worker_count; ++i) {
        ThreadConfig cfg {};
        cfg.name = L"acs::ThreadPool worker";
        auto r = Thread::Spawn(&WorkerMain, &g_pool->workers[i], cfg);
        if (r.IsErr()) {
            // 失敗時のロールバック
            g_pool->running.Store(0, MemoryOrder::Release);
            g_pool->wake_cv.NotifyAll();
            for (u32 j = 0; j < i; ++j) g_pool->threads[j].Join();
            g_pool->submit_node_pool->~PoolAllocator();
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

void ThreadPool::Shutdown() noexcept {
    if (!g_pool) return;
    g_pool->running.Store(0, MemoryOrder::Release);
    g_pool->wake_cv.NotifyAll();
    for (u32 i = 0; i < g_pool->worker_count; ++i) g_pool->threads[i].Join();

    // 残った投入ノードをすべて返却
    {
        ScopedLock lk(g_pool->submit.lock);
        SubmissionNode* n = g_pool->submit.head;
        while (n) {
            SubmissionNode* nx = n->next;
            ReleaseSubmitNode(n);
            n = nx;
        }
        g_pool->submit.head = nullptr;
        g_pool->submit.tail = nullptr;
        g_pool->submit.count = 0;
    }

    // ノードプール破棄
    if (g_pool->submit_node_pool) {
        g_pool->submit_node_pool->~PoolAllocator();
        ::HeapFree(::GetProcessHeap(), 0, g_pool->submit_node_pool);
        g_pool->submit_node_pool = nullptr;
    }

    // ワーカー破棄
    Worker* base = g_pool->workers;
    for (u32 i = 0; i < g_pool->worker_count; ++i) base[i].~Worker();
    g_pool->workers = nullptr;
    g_pool->worker_count = 0;

    g_pool->~PoolState();
    ::HeapFree(::GetProcessHeap(), 0, g_pool);
    g_pool = nullptr;
}

u32 ThreadPool::WorkerCount() noexcept {
    return g_pool ? g_pool->worker_count : 0;
}

u32 ThreadPool::CurrentWorkerIndex() noexcept {
    return tls_worker_index;
}

// タスク投入（自ワーカーなら deque、外部ならグローバルキュー経由）
Result<void> ThreadPool::Submit(const Task& t) noexcept {
    if (!g_pool) return ACS_ERR(Threading, 3, "ThreadPool not initialized");
    if (!t.fn)   return ACS_ERR(Threading, 4, "Task fn is null");

    if (t.counter) t.counter->Add(1);

    // 自ワーカーの deque へ Push を試みる（ローカリティ最適）
    u32 wi = tls_worker_index;
    if (wi != kNotAWorker) {
        if (g_pool->workers[wi].deque.Push(t)) {
            g_pool->wake_cv.NotifyOne();
            return Ok();
        }
        // 自 deque が満杯ならグローバルキューにフォールバック
    }

    // 外部キューにエンキュー（HeapAlloc 回避のためノードプールから取る）
    SubmissionNode* node = AcquireSubmitNode();
    if (!node) {
        if (t.counter) t.counter->Done();
        return ACS_ERR(Memory, 4, "Submission node alloc failed");
    }
    node->next = nullptr;
    node->task = t;
    {
        ScopedLock lk(g_pool->submit.lock);
        if (g_pool->submit.tail) g_pool->submit.tail->next = node;
        else                     g_pool->submit.head       = node;
        g_pool->submit.tail = node;
        ++g_pool->submit.count;
    }
    g_pool->wake_cv.NotifyOne();
    return Ok();
}

// counter が 0 になるまで待機（待機中もスティーリング、無作業なら指数バックオフ）
void ThreadPool::Wait(CompletionCounter& counter) noexcept {
    if (!g_pool) return;
    u32 self = tls_worker_index;
    u32 idle_iters = 0;  // 連続で仕事を見つけられなかった回数

    while (!counter.Finished()) {
        Task t {};
        bool got = false;
        if (self != kNotAWorker) {
            if (g_pool->workers[self].deque.Pop(t)) got = true;
        }
        if (!got && TryDrainSubmit(t)) got = true;
        if (!got && self != kNotAWorker && TrySteal(self, t)) got = true;
        if (!got && self == kNotAWorker) {
            // 外部スレッドからの Wait — 全ワーカー deque を順に Steal 試行
            for (u32 i = 0; i < g_pool->worker_count; ++i) {
                if (g_pool->workers[i].deque.Steal(t)) { got = true; break; }
            }
        }

        if (got) {
            Execute(t, self == kNotAWorker ? 0 : self);
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
struct PFContext {
    void (*body)(u32 i, u32 worker_index, void* user);
    void* user;
    u32 begin;
    u32 end;
};

// 1 チャンクを順次実行するアダプタ
void PFRangeFn(void* arg, u32 worker_index) noexcept {
    auto* r = static_cast<PFContext*>(arg);
    for (u32 i = r->begin; i < r->end; ++i) r->body(i, worker_index, r->user);
}
} // namespace

// 並列 for ループ（[begin, end) を grain で分割して投入 → Wait）
Result<void> ThreadPool::ParallelFor(u32 begin, u32 end, u32 grain,
                                     void (*body)(u32, u32, void*), void* user) noexcept {
    if (!g_pool) return ACS_ERR(Threading, 5, "ThreadPool not initialized");
    if (!body)   return ACS_ERR(Threading, 6, "ParallelFor body is null");
    if (begin >= end) return Ok();
    if (grain == 0)   grain = 1;

    u32 total = end - begin;
    u32 chunks = (total + grain - 1) / grain;

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

        Task t { &PFRangeFn, &ranges[c], &counter };
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
