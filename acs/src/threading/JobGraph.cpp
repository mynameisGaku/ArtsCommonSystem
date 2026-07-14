// SPDX-License-Identifier: Apache-2.0
// FJobGraph 実装
#include "threading/JobGraph.h"
#include "foundation/Log.h"

namespace acs {

/** Add で確保した全 Job を、実行完了を確認してから解放する。 */
FJobGraph::~FJobGraph() noexcept {
    // worker が Job* を参照中のままスコープを抜けても UAF にしない。
    Wait();
    for (usize i = 0; i < m_Jobs.Size(); ++i) delete m_Jobs[i];
    m_Jobs.Clear();
}

/** upstream が完了するまで自身を走らせない依存を追加する。 */
void JobHandle::DependOn(JobHandle upstream) noexcept {
    if (!IsValid() || !upstream.IsValid() || upstream.graph != graph) return;
    graph->AddDependency(upstream, *this);
}

/** ジョブを追加する (Submit 前のみ、戻り値はそのジョブのハンドル)。 */
JobHandle FJobGraph::Add(JobFn fn, void* user) noexcept {
    if (m_bSubmitted || !fn) return JobHandle{};
    auto* j = new Job();
    j->fn    = fn;
    j->user  = user;
    j->owner = this;
    const u32 idx = static_cast<u32>(m_Jobs.Size());
    m_Jobs.PushBack(j);
    return JobHandle{ this, idx };
}

/** upstream → downstream の依存関係を追加する (Submit 前のみ有効)。 */
void FJobGraph::AddDependency(JobHandle upstream, JobHandle downstream) noexcept {
    if (m_bSubmitted) return;
    if (!upstream.IsValid() || !downstream.IsValid()) return;
    if (upstream.graph != this || downstream.graph != this) return;
    if (upstream.index == downstream.index) return;  // self-dep は無視

    Job* const up = m_Jobs[upstream.index];
    up->dependents.PushBack(downstream.index);

    Job* const dn = m_Jobs[downstream.index];
    dn->initial_deps += 1;
    dn->deps_remaining.Store(dn->initial_deps);
}

/** ジョブ本体を実行し、依存先を起動して自身の完了を会計する TaskFn thunk。 */
void FJobGraph::JobThunk(void* user, u32 worker_index) noexcept {
    auto* const j = static_cast<Job*>(user);
    auto* const graph = j->owner;

    // 本体実行
    j->fn(j->user, worker_index);

    // 依存先の deps_remaining を減らし、0 になったものを FThreadPool に投入
    for (usize i = 0; i < j->dependents.Size(); ++i) {
        const u32 dep_idx = j->dependents[i];
        Job* const down = graph->m_Jobs[dep_idx];
        const u32 prev = down->deps_remaining.FetchSub(1);
        if (prev == 1) {
            // 自分が最後の依存を解いた → 起動
            Task t{};
            t.fn      = &FJobGraph::JobThunk;
            t.user    = down;
            t.counter = nullptr;  // counter は submit 時 Add(1) / 完了時 Done(1) で会計
            graph->m_Counter.Add(1);
            if (FThreadPool::Submit(t).IsErr()) {
                // 最後の依存を解いたこのスレッドが一度だけ同期実行する。終了競合でも
                // 枝を欠落させず、呼び出し側による全件再実行も不要になる。
                JobThunk(down, worker_index);
            }
        }
    }

    // 自身の完了を counter に通知
    graph->m_Counter.Done();
}

/** 全ジョブを FThreadPool に投入する (サイクル検知後、依存 0 のジョブから走らせる)。 */
TResult<void> FJobGraph::Submit() noexcept {
    if (m_bSubmitted) return ACS_ERR(Threading, 1, "FJobGraph already submitted");
    if (m_Jobs.Size() == 0) {
        m_bSubmitted = true;
        return Ok();
    }

    // サイクル検知 (Kahn 法、O(N + E))
    {
        const u32 job_count = static_cast<u32>(m_Jobs.Size());
        TArray<u32> remaining;
        remaining.Resize(job_count);
        for (u32 i = 0; i < job_count; ++i) {
            remaining[i] = m_Jobs[i]->deps_remaining.Load(EMemoryOrder::Acquire);
        }
        TArray<u32> queue;
        queue.Reserve(job_count);
        for (u32 i = 0; i < job_count; ++i) if (remaining[i] == 0) queue.PushBack(i);

        u32 visited = 0;
        while (queue.Size() > 0) {
            const u32 idx = queue[queue.Size() - 1];
            queue.PopBack();
            ++visited;
            const auto& deps = m_Jobs[idx]->dependents;
            for (usize k = 0; k < deps.Size(); ++k) {
                const u32 dep_idx = deps[k];
                if (--remaining[dep_idx] == 0) queue.PushBack(dep_idx);
            }
        }
        if (visited != job_count) {
            ACS_LOG_ERROR("FJobGraph::Submit: dependency cycle detected (visited=%u/%u)",
                          visited, job_count);
            return ACS_ERR(Threading, 3, "FJobGraph: dependency cycle");
        }
    }

    u32 entry_count = 0;
    for (usize i = 0; i < m_Jobs.Size(); ++i) {
        if (m_Jobs[i]->initial_deps == 0) ++entry_count;
    }
    if (entry_count == 0) {
        ACS_LOG_ERROR("FJobGraph::Submit: no entry job after cycle check");
        return ACS_ERR(Threading, 2, "FJobGraph: no entry job");
    }

    m_bSubmitted = true;

    // 全 entry と Submit 自身のライフタイム障壁を、最初の task 公開より前に計上する。
    // dependent は実行可能になった時点で個別に Add されるため、全ジョブ数の概算は不要。
    m_Counter.Add(entry_count + 1);

    // 依存 0 の job を FThreadPool に投入
    for (usize i = 0; i < m_Jobs.Size(); ++i) {
        Job* const j = m_Jobs[i];
        // 実行中に依存先が 0 へ遷移しても entry として再投入しない。構築時から
        // 依存 0 だったジョブだけを起点にすることで高速完了時の二重実行を防ぐ。
        if (j->initial_deps == 0) {
            Task t{};
            t.fn      = &FJobGraph::JobThunk;
            t.user    = j;
            t.counter = nullptr;
            auto r = FThreadPool::Submit(t);
            if (r.IsErr()) {
                ACS_LOG_WARN("FJobGraph::Submit: pool submit failed; running entry synchronously: %s",
                             r.Error().message);
                // 失敗した entry だけを一度実行する。既に投入済みの枝と重複せず、
                // Submit を成功扱いにできるため Easy 側の全件フォールバックも起きない。
                JobThunk(j, 0);
            }
        }
    }
    m_Counter.Done(); // Submit が m_Jobs を走査し終えたことを通知
    return Ok();
}

/** 全ジョブの完了までブロックして待つ。 */
void FJobGraph::Wait() noexcept {
    if (!m_bSubmitted) return;
    // m_Counter は submit 時 Add(1) / 完了時 Done(1) で会計済み。0 になるまで待つ。
    FThreadPool::Wait(m_Counter);
}

/** deps_remaining を初期値に戻し、同じグラフを再実行可能にする。 */
void FJobGraph::Reset() noexcept {
    Wait();
    for (usize i = 0; i < m_Jobs.Size(); ++i) {
        m_Jobs[i]->deps_remaining.Store(m_Jobs[i]->initial_deps);
    }
    m_bSubmitted = false;
}

} // namespace acs
