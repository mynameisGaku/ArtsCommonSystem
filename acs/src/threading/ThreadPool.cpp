// =============================================================================
// ACS Threading — ThreadPool 実装
// -----------------------------------------------------------------------------
// 構成要素:
//   1. WorkerDeque  — Chase-Lev SPMC deque（ワーカー 1 個につき 1 個）
//   2. SubmissionQueue — 外部スレッドからの投入用 Mutex キュー
//   3. Worker       — ワーカーローカル状態（deque、RNG）
//   4. PoolState    — プール全体の状態
//   5. WorkerMain   — 各ワーカースレッドのメインループ
//   6. TrySteal     — 他ワーカーの deque から奪う
//
// Chase-Lev の特徴:
//   - オーナーの Push / Pop は CAS フリー（最後の 1 個取りだけ CAS）
//   - 他スレッドからの Steal は CAS が必要だが上端のみ操作
//   - 結果として「自分のタスク中心 + 暇なら他から盗む」が低オーバーヘッドで成立
// =============================================================================
#include "threading/ThreadPool.h"
#include "threading/Mutex.h"
#include "threading/ConditionVar.h"
#include "threading/Thread.h"
#include "threading/MemoryOrder.h"
#include "foundation/Platform.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"

#include <intrin.h>

namespace acs {

namespace {

// ---- 定数 ---------------------------------------------------------------
constexpr i64 kDequeCapacity     = 4096;             // ワーカーごとの deque サイズ（2 のべき乗）
constexpr i64 kDequeCapacityMask = kDequeCapacity - 1;
static_assert((kDequeCapacity & kDequeCapacityMask) == 0,
              "Deque capacity must be a power of two");

constexpr u32 kMaxWorkers = 256;  // 上限（Threads[] サイズと一致）

// =============================================================================
// Chase-Lev SPMC deque
// -----------------------------------------------------------------------------
// オーナー: Push / Pop を底（bottom）側で行う。
// 他ワーカー: Steal を頂（top）側で行う。
//
// メモリ順序:
//   - Push の bottom ストアは release（buffer 書き込みより後に publish）
//   - Pop はオーナー専用なので relaxed + HardwareFence で底を変更
//   - Steal は acquire で top と bottom を読み、CAS で奪取
// =============================================================================
struct alignas(64) WorkerDeque {
    Atomic<i64> top    {0};                 // 他スレッドが奪う側
    Atomic<i64> bottom {0};                 // オーナーが追加・取り出す側
    Task        buffer[kDequeCapacity] {};  // タスク本体（値コピー）

    // ---- オーナー専用: 末尾に Push ----
    bool Push(const Task& t) noexcept {
        i64 b = bottom.Load(MemoryOrder::Relaxed);
        i64 tt = top.Load(MemoryOrder::Acquire);
        if (b - tt >= kDequeCapacity) return false;  // 満杯
        buffer[b & kDequeCapacityMask] = t;
        // release ストアにより、上記 buffer 書き込みが他スレッドに見える
        bottom.Store(b + 1, MemoryOrder::Release);
        return true;
    }

    // ---- オーナー専用: 末尾から Pop ----
    bool Pop(Task& out) noexcept {
        i64 b = bottom.Load(MemoryOrder::Relaxed) - 1;
        bottom.Store(b, MemoryOrder::Relaxed);
        HardwareFence();  // これより前の bottom 更新が、これより後の top 読み取りより前に見える
        i64 tt = top.Load(MemoryOrder::Relaxed);
        if (tt <= b) {
            out = buffer[b & kDequeCapacityMask];
            if (tt != b) return true;  // 通常ケース
            // 最後の 1 個 — Steal と競合する可能性あり、CAS で arbitrate
            i64 expected = tt;
            bool ok = top.CompareExchange(expected, tt + 1);
            bottom.Store(b + 1, MemoryOrder::Relaxed);  // 取れても取れなくても底を戻す
            return ok;
        }
        // deque は空 — 底を元に戻す
        bottom.Store(b + 1, MemoryOrder::Relaxed);
        return false;
    }

    // ---- 他ワーカー: 先頭から Steal ----
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

// =============================================================================
// グローバル投入キュー
// -----------------------------------------------------------------------------
// プール外スレッド（メインスレッド等）からの Submit はここに入る。
// シングル Mutex 保護のリンクトリスト。投入頻度が低い前提なので十分。
// 高頻度になれば Vyukov MPSC に置き換える。
// =============================================================================
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

// =============================================================================
// ワーカーコンテキスト
// =============================================================================
struct alignas(64) Worker {
    WorkerDeque  deque;       // 自分の deque
    u32          index;       // 0..N-1
    Atomic<u32>  rng_state;   // スティーリング先選択用 xorshift32
};

// =============================================================================
// プール全体状態
// =============================================================================
struct PoolState {
    Worker*           workers      = nullptr;
    u32               worker_count = 0;
    Thread            threads[kMaxWorkers];
    Atomic<u32>       running      {0};                    // 1=動作中, 0=停止
    SubmissionQueue   submit;                              // 外部投入キュー
    Mutex             wake_lock;                           // CV ガード用
    ConditionVar      wake_cv;                             // ワーカー起床通知
};

PoolState* g_pool = nullptr;

// 各ワーカースレッドだけが持つ TLS 変数
// （ワーカー外スレッドは kNotAWorker を読む）
ACS_THREAD_LOCAL u32 tls_worker_index = ThreadPool::kNotAWorker;

// xorshift32 — 小さく分岐なしの簡易 PRNG。スティーリング先選択に使用。
ACS_FORCEINLINE u32 XorShift32(Atomic<u32>& s) noexcept {
    u32 x = s.Load(MemoryOrder::Relaxed);
    if (x == 0) x = 0x9E3779B9u;  // 種が 0 なら黄金比固定値
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.Store(x, MemoryOrder::Relaxed);
    return x;
}

// 外部投入キューから 1 件取り出す（取れたら true）。
// TryLock で衝突時は失敗にして他の経路を試す。
bool TryDrainSubmit(Task& out) noexcept {
    SubmissionQueue& q = g_pool->submit;
    if (!q.lock.TryLock()) return false;
    bool got = false;
    if (q.head) {
        SubmissionNode* n = q.head;
        out = n->task;
        q.head = n->next;
        if (!q.head) q.tail = nullptr;
        --q.count;
        ::HeapFree(::GetProcessHeap(), 0, n);
        got = true;
    }
    q.lock.Unlock();
    return got;
}

// 自分以外のワーカーから Steal を試みる。
// 開始位置はランダム、以後リング状に巡回する。
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

// タスクを実行し、完了通知も行う。
ACS_FORCEINLINE void Execute(const Task& t, u32 worker_index) noexcept {
    if (t.fn) t.fn(t.user, worker_index);
    if (t.counter) t.counter->Done();
}

// ワーカースレッドのメインループ:
//   1. 自分の deque から Pop
//   2. 失敗なら 外部キューから Drain
//   3. それでも失敗なら 他ワーカーから Steal
//   4. 全て失敗なら CV で短時間スリープ
void WorkerMain(void* arg) noexcept {
    Worker* w = static_cast<Worker*>(arg);
    tls_worker_index = w->index;
    // PRNG 種を index で決定（再現性確保）
    w->rng_state.Store(0x9E3779B9u ^ (w->index * 2654435761u), MemoryOrder::Relaxed);

    while (g_pool->running.Load(MemoryOrder::Acquire)) {
        Task t {};
        if (w->deque.Pop(t))            { Execute(t, w->index); continue; }
        if (TryDrainSubmit(t))          { Execute(t, w->index); continue; }
        if (TrySteal(w->index, t))      { Execute(t, w->index); continue; }

        // 全部空だったので CV で 2ms 待機（NotifyOne で起こされる）
        ScopedLock lk(g_pool->wake_lock);
        g_pool->wake_cv.WaitFor(g_pool->wake_lock, 2);
    }
    tls_worker_index = ThreadPool::kNotAWorker;
}

} // namespace

// =============================================================================
// 公開 API
// =============================================================================

Result<void> ThreadPool::Init(u32 worker_count) noexcept {
    if (g_pool != nullptr) return ACS_ERR(Threading, 2, "ThreadPool already initialized");
    if (worker_count == 0) worker_count = HardwareConcurrency();
    if (worker_count > kMaxWorkers) worker_count = kMaxWorkers;

    // PoolState はプロセスヒープから 0 クリア確保
    void* mem = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PoolState));
    if (!mem) return ACS_ERR(Memory, 2, "ThreadPool state alloc failed");
    g_pool = ::new (mem) PoolState();

    // ワーカー配列確保（64 バイト整列のため少し余分に確保して手動アライン）
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
    g_pool->running.Store(1, MemoryOrder::Release);

    // ワーカースレッドを起動
    for (u32 i = 0; i < worker_count; ++i) {
        ThreadConfig cfg {};
        cfg.name = L"acs::ThreadPool worker";
        auto r = Thread::Spawn(&WorkerMain, &g_pool->workers[i], cfg);
        if (r.IsErr()) {
            // 失敗時のロールバック
            g_pool->running.Store(0, MemoryOrder::Release);
            g_pool->wake_cv.NotifyAll();
            for (u32 j = 0; j < i; ++j) g_pool->threads[j].Join();
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
    g_pool->wake_cv.NotifyAll();  // 全ワーカーを起こして終了させる
    for (u32 i = 0; i < g_pool->worker_count; ++i) g_pool->threads[i].Join();

    // 残った投入ノードを破棄
    {
        ScopedLock lk(g_pool->submit.lock);
        SubmissionNode* n = g_pool->submit.head;
        while (n) {
            SubmissionNode* nx = n->next;
            ::HeapFree(::GetProcessHeap(), 0, n);
            n = nx;
        }
        g_pool->submit.head = nullptr;
        g_pool->submit.tail = nullptr;
        g_pool->submit.count = 0;
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

// タスクを投入する。ワーカー内からなら自 deque へ Push（ローカリティ）、
// それ以外は外部キューへ。CV で寝ているワーカーを 1 体起こす。
Result<void> ThreadPool::Submit(const Task& t) noexcept {
    if (!g_pool) return ACS_ERR(Threading, 3, "ThreadPool not initialized");
    if (!t.fn)   return ACS_ERR(Threading, 4, "Task fn is null");

    if (t.counter) t.counter->Add(1);

    // 自ワーカーの deque へ Push を試みる
    u32 wi = tls_worker_index;
    if (wi != kNotAWorker) {
        if (g_pool->workers[wi].deque.Push(t)) {
            g_pool->wake_cv.NotifyOne();
            return Ok();
        }
        // 自 deque が満杯 — グローバルキューにフォールバック
    }

    // 外部キューにエンキュー
    auto* node = static_cast<SubmissionNode*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(SubmissionNode)));
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

// counter が 0 になるまで待機。待機中は他のタスクをスティールして実行。
// 「待つだけ」ではなく「働きながら待つ」のがミソ（Naughty Dog 流）。
void ThreadPool::Wait(CompletionCounter& counter) noexcept {
    if (!g_pool) return;
    u32 self = tls_worker_index;
    while (!counter.Finished()) {
        Task t {};
        bool got = false;
        if (self != kNotAWorker) {
            if (g_pool->workers[self].deque.Pop(t)) got = true;
        }
        if (!got && TryDrainSubmit(t)) got = true;
        if (!got && self != kNotAWorker && TrySteal(self, t)) got = true;
        if (!got) {
            // 外部スレッドからの Wait — 全ワーカー deque を順にスチール試行
            if (self == kNotAWorker) {
                for (u32 i = 0; i < g_pool->worker_count; ++i) {
                    if (g_pool->workers[i].deque.Steal(t)) { got = true; break; }
                }
            }
        }
        if (got) {
            Execute(t, self == kNotAWorker ? 0 : self);
        } else {
            SpinHint();  // PAUSE / YIELD で電力節約
        }
    }
}

// =============================================================================
// ParallelFor の補助構造体
// =============================================================================
namespace {
struct PFContext {
    void (*body)(u32 i, u32 worker_index, void* user);  // ユーザー関数
    void* user;                                          // ユーザーデータ
    u32 begin;                                           // チャンクの開始
    u32 end;                                             // チャンクの終端
};

// 1 チャンクを順次実行するアダプタ
void PFRangeFn(void* arg, u32 worker_index) noexcept {
    auto* r = static_cast<PFContext*>(arg);
    for (u32 i = r->begin; i < r->end; ++i) r->body(i, worker_index, r->user);
}
} // namespace

// 並列 for ループ。
// [begin, end) を grain で分割し、チャンク数だけ Submit して Wait。
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
            // 投入失敗 — 既に投入済みのものは待ってから返す
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
