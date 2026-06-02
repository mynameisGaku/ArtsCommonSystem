// SPDX-License-Identifier: Apache-2.0
// FJobGraph — 依存関係付き並列タスクスケジューラ
//
// 使い方:
//   FJobGraph g;
//   auto loadA  = g.Add(&LoadAssetA, &ctx);
//   auto loadB  = g.Add(&LoadAssetB, &ctx);
//   auto build  = g.Add(&BuildScene, &ctx);
//   auto upload = g.Add(&UploadGpu,  &ctx);
//
//   build.DependOn(loadA);
//   build.DependOn(loadB);
//   upload.DependOn(build);
//
//   g.Submit();         // 投入 (依存 0 の job から走り出す)
//   g.Wait();           // 全 job 完了まで block
//
// 実装:
//   ・FThreadPool 上で動く。各 job は完了時に「dependents の deps_remaining を
//     atomic decrement して 0 になったら FThreadPool::Submit」する fan-out 方式
//   ・グラフは Submit 後に変更不可 (Add は Submit より前のみ)
//   ・Reset() で再利用可能 (依存関係はそのまま、deps_remaining だけ復元)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "threading/Atomic.h"
#include "threading/ThreadPool.h"

namespace acs {

class FJobGraph;

// Job のハンドル — FJobGraph::Add の戻り値 / DependOn のキー
struct JobHandle {
    FJobGraph* graph = nullptr;
    u32       index = 0xFFFFFFFFu;

    bool IsValid() const noexcept { return graph != nullptr && index != 0xFFFFFFFFu; }

    // self が完了するまで other は走らない (other.DependOn(self) と等価)
    void DependOn(JobHandle upstream) noexcept;
};

// Job 関数 (FThreadPool::TaskFn と同形式)
using JobFn = void (*)(void* user, u32 worker_index);

class FJobGraph {
public:
    FJobGraph() noexcept = default;
    ~FJobGraph() noexcept;   // new した Job* を解放する (default だとリークしていた)

    FJobGraph(const FJobGraph&) = delete;
    FJobGraph& operator=(const FJobGraph&) = delete;

    // Job を追加 (Submit 前のみ呼べる)
    JobHandle Add(JobFn fn, void* user) noexcept;

    // upstream → downstream の依存関係を追加
    void AddDependency(JobHandle upstream, JobHandle downstream) noexcept;

    // 全 job を FThreadPool に投入。依存 0 の job が即座に走り始める。
    TResult<void> Submit() noexcept;

    // 全 job 完了まで block (待機中もスティーリングに参加)
    void Wait() noexcept;

    // 同じグラフを再実行する場合に呼ぶ (deps_remaining を初期値に戻す)
    void Reset() noexcept;

    // 統計
    u32 JobCount() const noexcept { return static_cast<u32>(m_Jobs.Size()); }

private:
    friend struct JobHandle;

    // Job の static 関数: FThreadPool に渡す TaskFn の thunk
    static void JobThunk(void* user, u32 worker_index) noexcept;

    struct Job {
        JobFn          fn               = nullptr;
        void*          user             = nullptr;
        TAtomic<u32>    deps_remaining   {0};
        u32            initial_deps     = 0;     // Reset 用に保存
        TArray<u32>     dependents;               // この job が完了すると起動する job の index 群
        FJobGraph*      owner            = nullptr;
    };

    TArray<Job*>        m_Jobs;
    CompletionCounter  m_Counter;
    bool               m_bSubmitted       = false;
};

} // namespace acs
