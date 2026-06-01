// SPDX-License-Identifier: Apache-2.0
// FJobGraph 実装
#include "threading/JobGraph.h"
#include "foundation/Log.h"

namespace acs {

void JobHandle::DependOn(JobHandle upstream) noexcept {
    if (!IsValid() || !upstream.IsValid() || upstream.graph != graph) return;
    graph->AddDependency(upstream, *this);
}

JobHandle FJobGraph::Add(JobFn fn, void* user) noexcept {
    if (m_bSubmitted || !fn) return JobHandle{};
    auto* j = new Job();
    j->fn    = fn;
    j->user  = user;
    j->owner = this;
    u32 idx = static_cast<u32>(m_Jobs.Size());
    m_Jobs.PushBack(j);
    return JobHandle{ this, idx };
}

void FJobGraph::AddDependency(JobHandle upstream, JobHandle downstream) noexcept {
    if (m_bSubmitted) return;
    if (!upstream.IsValid() || !downstream.IsValid()) return;
    if (upstream.graph != this || downstream.graph != this) return;
    if (upstream.index == downstream.index) return;  // self-dep は無視

    Job* up = m_Jobs[upstream.index];
    up->dependents.PushBack(downstream.index);

    Job* dn = m_Jobs[downstream.index];
    dn->initial_deps += 1;
    dn->deps_remaining.Store(dn->initial_deps);
}

void FJobGraph::JobThunk(void* user, u32 worker_index) noexcept {
    auto* j = static_cast<Job*>(user);
    auto* graph = j->owner;

    // 本体実行
    j->fn(j->user, worker_index);

    // 依存先の deps_remaining を減らし、0 になったものを FThreadPool に投入
    for (usize i = 0; i < j->dependents.Size(); ++i) {
        u32 dep_idx = j->dependents[i];
        Job* down = graph->m_Jobs[dep_idx];
        u32 prev = down->deps_remaining.FetchSub(1);
        if (prev == 1) {
            // 自分が最後の依存を解いた → 起動
            Task t{};
            t.fn      = &FJobGraph::JobThunk;
            t.user    = down;
            t.counter = nullptr;  // counter は submit 時 Add(1) / 完了時 Done(1) で会計
            graph->m_Counter.Add(1);
            if (FThreadPool::Submit(t).IsErr()) graph->m_Counter.Done();  // 走らないので打ち消す
        }
    }

    // 自身の完了を counter に通知
    graph->m_Counter.Done();
}

TResult<void> FJobGraph::Submit() noexcept {
    if (m_bSubmitted) return ACS_ERR(Threading, 1, "FJobGraph already submitted");
    if (m_Jobs.Size() == 0) {
        m_bSubmitted = true;
        return Ok();
    }

    // ---- サイクル検知 (Kahn 法、O(N + E)) ----
    {
        const u32 N = static_cast<u32>(m_Jobs.Size());
        TArray<u32> remaining;
        remaining.Resize(N);
        for (u32 i = 0; i < N; ++i) {
            remaining[i] = m_Jobs[i]->deps_remaining.Load(EMemoryOrder::Acquire);
        }
        TArray<u32> queue;
        queue.Reserve(N);
        for (u32 i = 0; i < N; ++i) if (remaining[i] == 0) queue.PushBack(i);

        u32 visited = 0;
        while (queue.Size() > 0) {
            u32 idx = queue[queue.Size() - 1];
            queue.PopBack();
            ++visited;
            const auto& deps = m_Jobs[idx]->dependents;
            for (usize k = 0; k < deps.Size(); ++k) {
                u32 dep_idx = deps[k];
                if (--remaining[dep_idx] == 0) queue.PushBack(dep_idx);
            }
        }
        if (visited != N) {
            ACS_LOG_ERROR("FJobGraph::Submit: dependency cycle detected (visited=%u/%u)",
                          visited, N);
            return ACS_ERR(Threading, 3, "FJobGraph: dependency cycle");
        }
    }

    m_bSubmitted = true;

    // カウンタは「submit 時に Add(1) / 完了時に Done(1)」で会計する。upfront Add(N) +
    // 失敗時の概算巻き戻しは、cascade で実行されるジョブ数を予測できず over/under-Done
    // して underflow → Wait() ハングや早期完了を招くため採用しない。

    // 依存 0 の job を FThreadPool に投入
    bool any_started = false;
    u32 submitted_count = 0;
    for (usize i = 0; i < m_Jobs.Size(); ++i) {
        Job* j = m_Jobs[i];
        if (j->deps_remaining.Load(EMemoryOrder::Acquire) == 0) {
            Task t{};
            t.fn      = &FJobGraph::JobThunk;
            t.user    = j;
            t.counter = nullptr;
            m_Counter.Add(1);                       // この job 1 個ぶんを計上
            auto r = FThreadPool::Submit(t);
            if (r.IsErr()) {
                ACS_LOG_ERROR("FJobGraph::Submit: FThreadPool::Submit failed: %s",
                              r.Error().message);
                m_Counter.Done();                   // この job は走らないので計上を打ち消す
                // 既に submit 済みの entry とその cascade は走り切って各自 Done() するので
                // 巻き戻し不要。未 submit の entry は Add していないので leak しない。
                return r;
            }
            ++submitted_count;
            any_started = true;
        }
    }

    if (!any_started && m_Jobs.Size() > 0) {
        // サイクル検知を抜けてここに来たら、空のグラフ (entry も無い) のはず
        ACS_LOG_ERROR("FJobGraph::Submit: no entry job after cycle check");
        return ACS_ERR(Threading, 2, "FJobGraph: no entry job");
    }
    return Ok();
}

void FJobGraph::Wait() noexcept {
    if (!m_bSubmitted) return;
    // Submit 時に Add(N) したカウンタを、Job 完了で Done() する仕組みが要る。
    // JobThunk 内で counter.Done() を呼ぶよう改修する必要があるが、
    // FThreadPool::Submit に counter を渡せば自動 Add+Done してくれる。
    // 上の実装では Submit 時に自前で Add してしまったので調整 -- このシンプル版では
    // FThreadPool::Submit に counter を渡して任せる方がきれい。実装は素直にする:
    FThreadPool::Wait(m_Counter);
}

void FJobGraph::Reset() noexcept {
    for (usize i = 0; i < m_Jobs.Size(); ++i) {
        m_Jobs[i]->deps_remaining.Store(m_Jobs[i]->initial_deps);
    }
    m_bSubmitted = false;
}

} // namespace acs
