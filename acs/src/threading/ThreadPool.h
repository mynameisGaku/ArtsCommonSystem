// =============================================================================
// ACS Threading — ワークスティーリング ThreadPool
// -----------------------------------------------------------------------------
// アーキテクチャ:
//   - 各論理 CPU ごとに 1 ワーカースレッド（CPU 数は HardwareConcurrency 既定）
//   - 各ワーカーは Chase-Lev SPMC deque を所有
//       * オーナー側（Push / Pop）: 通常ケースで CAS 不要
//       * 他ワーカー（Steal）: 上端を CAS で奪取
//   - 外部スレッド（プールメンバ外）からの投入は Mutex 保護のグローバル
//     キューに入れ、いずれかのワーカーが回収する
//   - Wait は「スティーリングに参加」しながら待つ（ヘルプスチール方式）
//     これにより Nested ParallelFor 等でデッドロックしない
//
// タスクは小さな POD（fn / user / counter ポインタ）で、deque スロットに
// 値コピーする。ホットパスでヒープ確保しないため非常に低レイテンシ。
//
// 完了は CompletionCounter で追跡する。null を渡せば fire-and-forget。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "threading/Atomic.h"

namespace acs {

// =============================================================================
// CompletionCounter — タスク群の完了待機用カウンタ
// -----------------------------------------------------------------------------
// Submit 時に Add() され、タスク完了時に Done() される。
// Pending() が 0 になれば全タスク完了。Wait() でブロック可能。
// =============================================================================
class CompletionCounter {
public:
    CompletionCounter() noexcept = default;
    explicit CompletionCounter(u32 initial) noexcept : _v(initial) {}

    // コピー禁止（共有しても同期にならないため）
    CompletionCounter(const CompletionCounter&) = delete;
    CompletionCounter& operator=(const CompletionCounter&) = delete;

    void Add(u32 n = 1) noexcept   { _v.FetchAdd(n); }                       // タスク投入時
    void Done()         noexcept   { _v.FetchSub(1); }                       // タスク完了時
    u32  Pending() const noexcept  { return _v.Load(MemoryOrder::Acquire); } // 残数
    bool Finished() const noexcept { return Pending() == 0; }                // 全完了か

private:
    mutable Atomic<u32> _v {0};
};

// タスク本体の関数型。worker_index は実行中ワーカーの ID（0..N-1）。
using TaskFn = void (*)(void* user, u32 worker_index);

// 1 つのタスク。POD なので deque に値コピーされる。
struct Task {
    TaskFn              fn       = nullptr;  // 実行する関数
    void*               user     = nullptr;  // ユーザーデータ
    CompletionCounter*  counter  = nullptr;  // 完了通知先（任意、null 可）
};

// =============================================================================
// ThreadPool — グローバル共有のワークスティーリングプール
// =============================================================================
class ThreadPool {
public:
    // 初期化。worker_count=0 で論理 CPU 数を採用。
    // 二重 Init はエラー。Shutdown 後は再初期化可能。
    static Result<void> Init(u32 worker_count = 0) noexcept;
    static void         Shutdown() noexcept;

    // 起動中のワーカー数（未初期化なら 0）
    static u32          WorkerCount() noexcept;

    // 呼び出しスレッドがプールワーカーであれば 0..N-1 を返す。
    // ワーカー外なら kNotAWorker (= 0xFFFFFFFF)。
    static u32          CurrentWorkerIndex() noexcept;
    static constexpr u32 kNotAWorker = 0xFFFFFFFFu;

    // タスク投入。counter が非 null なら Submit 時に Add(1)、完了時に Done()。
    // ワーカー内から呼ぶと自分の deque に LIFO で積み（ローカリティ最適）、
    // 外部から呼ぶとグローバルキューを経由する。
    static Result<void> Submit(const Task& t) noexcept;

    // counter が 0 になるまで待つ。待機中はスティーリングに参加するため、
    // 子タスクを生成して待つ Nested 呼び出しでもデッドロックしない。
    static void         Wait(CompletionCounter& counter) noexcept;

    // 並列 for ループ。[begin, end) を grain サイズの塊に分割し、
    // 各チャンクをタスクとして Submit → Wait する。
    // body は (i, worker_index, user) を受け取る関数ポインタ。
    static Result<void> ParallelFor(u32 begin, u32 end, u32 grain,
                                    void (*body)(u32 i, u32 worker_index, void* user),
                                    void* user) noexcept;
};

} // namespace acs
