// SPDX-License-Identifier: Apache-2.0
// ワークスチール FThreadPool（Chase-Lev SPMC deque + help-stealing wait）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "threading/Atomic.h"

namespace acs {

// タスク群の完了待機用カウンタ
class CompletionCounter {
public:
    CompletionCounter() noexcept = default;
    explicit CompletionCounter(u32 initial) noexcept : m_V(initial) {}

    // 共有してもカウントが直交しないのでコピー禁止
    CompletionCounter(const CompletionCounter&) = delete;
    CompletionCounter& operator=(const CompletionCounter&) = delete;

    void Add(u32 n = 1) noexcept   { m_V.FetchAdd(n); }                       // タスク投入時
    // タスク完了時。下限 0 で飽和させ、Add より多く Done が呼ばれても 0xFFFFFFFF へ
    // underflow しない (underflow すると Finished() が永久に false → Wait() ハング)。
    void Done() noexcept {
        u32 cur = m_V.Load(EMemoryOrder::Relaxed);
        while (cur != 0) {
            if (m_V.CompareExchange(cur, cur - 1)) return;   // 失敗時 cur は実値に更新される
        }
    }
    u32  Pending() const noexcept  { return m_V.Load(EMemoryOrder::Acquire); } // 残数
    bool Finished() const noexcept { return Pending() == 0; }                // 全完了か

private:
    mutable TAtomic<u32> m_V {0};
};

// タスク本体の関数型（worker_index は実行ワーカーの ID 0..N-1）
using TaskFn = void (*)(void* user, u32 worker_index);

// 1 つのタスク（POD なので deque に値コピーされる）
struct Task {
    TaskFn              fn       = nullptr;  // 実行する関数
    void*               user     = nullptr;  // ユーザーデータ
    CompletionCounter*  counter  = nullptr;  // 完了通知先（任意、null 可）
};

class FThreadPool {
public:
    // 初期化（worker_count=0 で論理 CPU 数を採用、二重 Init はエラー）
    static TResult<void> Init(u32 worker_count = 0) noexcept;
    static void         Shutdown() noexcept;

    // 起動中のワーカー数
    static u32          WorkerCount() noexcept;

    // 呼び出しスレッドがプールワーカなら 0..N-1、それ以外は kNotAWorker
    static u32          CurrentWorkerIndex() noexcept;
    static constexpr u32 kNotAWorker = 0xFFFFFFFFu;

    // タスク投入（counter が非 null なら自動で Add(1) される）
    static TResult<void> Submit(const Task& t) noexcept;

    // counter が 0 になるまで待つ（待機中もスティーリングに参加）
    static void         Wait(CompletionCounter& counter) noexcept;

    // 並列 for ループ（[begin, end) を grain サイズに分割して投入＆Wait）
    static TResult<void> ParallelFor(u32 begin, u32 end, u32 grain,
                                    void (*body)(u32 i, u32 worker_index, void* user),
                                    void* user) noexcept;
};

} // namespace acs
