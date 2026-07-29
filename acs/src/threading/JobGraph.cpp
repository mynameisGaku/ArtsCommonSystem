// SPDX-License-Identifier: Apache-2.0
// FJobGraph 実装
#include "threading/JobGraph.h"
#include "foundation/Log.h"

namespace acs {

/** 実行完了を確認してから、全 job と所有 callable を解放する。 */
FJobGraph::~FJobGraph() noexcept
{
    Wait();
    while (m_JobCount != 0) {
        /** 末尾から破棄する job。 */
        FJob* const job = JobAt(m_JobCount - 1);
        if (m_JobCount > kInlineJobCapacity) m_OverflowJobs.PopBack();
        --m_JobCount;
        DestroyJob(job);
    }
}

/** graph 末尾へ空の job を追加する。 */
FJobGraph::FJob* FJobGraph::AppendEmptyJob() noexcept
{
    if (m_bSubmitted) return nullptr;

    /** 追加先として確保した job。 */
    FJob* job = nullptr;
    if (m_JobCount < kInlineJobCapacity) {
        /** graph 内の未使用固定領域。 */
        void* const storage = m_InlineJobStorage + sizeof(FJob) * m_JobCount;
        job = ::new (storage) FJob();
        ++m_Diagnostics.inline_job_count;
    } else {
        job = new (std::nothrow) FJob();
        if (!job) return nullptr;
        if (!m_OverflowJobs.TryPushBack(job)) {
            delete job;
            return nullptr;
        }
        ++m_Diagnostics.heap_job_count;
    }

    job->owner = this;
    ++m_JobCount;
    m_TopologyCompiled = false;
    m_TopologyHasCycle = false;
    return job;
}

/** 末尾 job を callable 構築失敗時に取り消す。 */
void FJobGraph::RemoveLastJob(FJob* job) noexcept
{
    if (!job || m_JobCount == 0 || JobAt(m_JobCount - 1) != job) return;
    if (m_JobCount > kInlineJobCapacity) {
        m_OverflowJobs.PopBack();
        --m_Diagnostics.heap_job_count;
    } else {
        --m_Diagnostics.inline_job_count;
    }
    --m_JobCount;
    DestroyJob(job);
    m_TopologyCompiled = false;
    m_TopologyHasCycle = false;
}

/** index の job を返す。 */
FJobGraph::FJob* FJobGraph::JobAt(u32 index) noexcept
{
    if (index >= m_JobCount) return nullptr;
    if (index < kInlineJobCapacity) {
        return reinterpret_cast<FJob*>(m_InlineJobStorage + sizeof(FJob) * index);
    }
    return m_OverflowJobs[index - kInlineJobCapacity];
}

/** index の job を返す const 版。 */
const FJobGraph::FJob* FJobGraph::JobAt(u32 index) const noexcept
{
    return const_cast<FJobGraph*>(this)->JobAt(index);
}

/** job が所有する callable を破棄し、job 自体を正しい確保元へ返す。 */
void FJobGraph::DestroyJob(FJob* job) noexcept
{
    if (!job) return;
    if (job->destroy_callable) {
        job->destroy_callable(job);
        job->destroy_callable = nullptr;
        job->user = nullptr;
    }

    /** 固定 job 領域の先頭アドレス。 */
    const uptr inline_begin = reinterpret_cast<uptr>(m_InlineJobStorage);
    /** 固定 job 領域の終端アドレス。 */
    const uptr inline_end = inline_begin + sizeof(m_InlineJobStorage);
    /** 破棄対象 job のアドレス。 */
    const uptr address = reinterpret_cast<uptr>(job);
    if (address >= inline_begin && address < inline_end) job->~FJob();
    else delete job;
}

/** upstream が完了するまで自身を走らせない依存を追加する。 */
void FJobHandle::DependOn(FJobHandle upstream) noexcept
{
    if (!IsValid() || !upstream.IsValid() || upstream.graph != graph) return;
    graph->AddDependency(upstream, *this);
}

/** raw 関数 pointer と user pointer を job として追加する。 */
FJobHandle FJobGraph::Add(JobFn fn, void* user) noexcept
{
    if (!fn) return {};
    /** raw callback を保持する新規 job。 */
    FJob* const job = AppendEmptyJob();
    if (!job) return {};
    job->fn = fn;
    job->user = user;
    return FJobHandle{this, m_JobCount - 1};
}

/** upstream から downstream への依存を追加する。 */
void FJobGraph::AddDependency(FJobHandle upstream, FJobHandle downstream) noexcept
{
    if (m_bSubmitted) return;
    if (!upstream.IsValid() || !downstream.IsValid()) return;
    if (upstream.graph != this || downstream.graph != this) return;
    if (upstream.index >= m_JobCount || downstream.index >= m_JobCount) return;
    if (upstream.index == downstream.index) return;

    /** 依存先 index を保持する上流 job。 */
    FJob* const up = JobAt(upstream.index);
    /** 未解決依存数を増やす下流 job。 */
    FJob* const down = JobAt(downstream.index);
    up->dependents.PushBack(downstream.index);
    ++down->initial_deps;
    down->deps_remaining.Store(down->initial_deps, EMemoryOrder::Release);
    m_TopologyCompiled = false;
    m_TopologyHasCycle = false;
}

/** job 本体を実行し、依存がすべて解決した後続 job を投入する。 */
void FJobGraph::JobThunk(void* user, u32 worker_index) noexcept
{
    /** 実行対象 job。 */
    auto* const job = static_cast<FJob*>(user);
    /** 後続 job と完了数を所有する graph。 */
    FJobGraph* const graph = job->owner;

    job->fn(job->user, worker_index);

    for (usize i = 0; i < job->dependents.Size(); ++i) {
        /** 現在の job に依存する後続 job。 */
        FJob* const dependent = graph->JobAt(job->dependents[i]);
        /** 減算前の未解決依存数。 */
        const u32 previous = dependent->deps_remaining.FetchSub(1);
        if (previous != 1) continue;

        /** 実行可能になった後続 job の ThreadPool task。 */
        FTask task{};
        task.fn = &FJobGraph::JobThunk;
        task.user = dependent;
        if (FThreadPool::Submit(task).IsErr()) {
            // 最後の依存を解いた実行者が一度だけ同期実行し、枝の欠落を防ぐ。
            JobThunk(dependent, worker_index);
        }
    }
    graph->m_Counter.Done();
}

/** Kahn 法で依存構造を検証し、entry job 群を初回だけキャッシュする。 */
TResult<void> FJobGraph::CompileTopology() noexcept
{
    if (m_TopologyCompiled) {
        if (m_TopologyHasCycle)
            return ACS_ERR(Threading, 3, "FJobGraph: dependency cycle");
        return Ok();
    }

    ++m_Diagnostics.topology_compilations;
    ++m_Diagnostics.submit_full_graph_scans;
    m_EntryJobs.Clear();
    m_TopologyQueue.Clear();

    if (!m_TopologyRemaining.TryResize(m_JobCount) || !m_EntryJobs.TryReserve(m_JobCount) || !m_TopologyQueue.TryReserve(m_JobCount)) {
        return ACS_ERR(Memory, 4, "FJobGraph: topology scratch allocation failed");
    }

    for (u32 i = 0; i < m_JobCount; ++i) {
        /** 依存数をキャッシュへ複製する job。 */
        const FJob* const job = JobAt(i);
        m_TopologyRemaining[i] = job->initial_deps;
        if (job->initial_deps == 0) {
            if (!m_EntryJobs.TryPushBack(i) || !m_TopologyQueue.TryPushBack(i)) {
                return ACS_ERR(Memory, 5, "FJobGraph: topology queue allocation failed");
            }
        }
    }

    /** Kahn 法で取り除けた job 数。 */
    u32 visited = 0;
    while (!m_TopologyQueue.IsEmpty()) {
        /** 今回依存辺を取り除く job index。 */
        const u32 index = m_TopologyQueue[m_TopologyQueue.Size() - 1];
        m_TopologyQueue.PopBack();
        ++visited;

        /** 現在 job に依存する job index 群。 */
        const TArray<u32>& dependents = JobAt(index)->dependents;
        for (usize i = 0; i < dependents.Size(); ++i) {
            /** 未解決依存数を減らす後続 index。 */
            const u32 dependent_index = dependents[i];
            if (--m_TopologyRemaining[dependent_index] == 0 && !m_TopologyQueue.TryPushBack(dependent_index)) {
                return ACS_ERR(Memory, 6, "FJobGraph: topology queue allocation failed");
            }
        }
    }

    m_TopologyCompiled = true;
    m_TopologyHasCycle = visited != m_JobCount;
    if (m_TopologyHasCycle) {
        ACS_LOG_ERROR("FJobGraph::Submit: dependency cycle detected (visited=%u/%u)", visited, m_JobCount);
        return ACS_ERR(Threading, 3, "FJobGraph: dependency cycle");
    }
    return Ok();
}

/** 検証済み entry job 群を FThreadPool へ投入する。 */
TResult<void> FJobGraph::Submit() noexcept
{
    if (m_bSubmitted)
        return ACS_ERR(Threading, 1, "FJobGraph already submitted");
    if (m_JobCount == 0) {
        m_bSubmitted = true;
        return Ok();
    }

    /** 初回構築またはキャッシュ済み依存構造の検証結果。 */
    TResult<void> topology = CompileTopology();
    if (topology.IsErr()) return topology;
    if (m_EntryJobs.IsEmpty())
        return ACS_ERR(Threading, 2, "FJobGraph: no entry job");

    m_bSubmitted = true;
    // 未投入の entry と依存待ち job も先に残数へ含める。これにより実行中の
    // 後続公開と Wait の競合で 0 が一時的に見えることなく、RMW は一回で済む。
    m_Counter.Add(m_JobCount);

    for (usize i = 0; i < m_EntryJobs.Size(); ++i) {
        /** 依存がない実行開始 job。 */
        FJob* const job = JobAt(m_EntryJobs[i]);
        /** ThreadPool へ公開する開始 task。 */
        FTask task{};
        task.fn = &FJobGraph::JobThunk;
        task.user = job;
        if (FThreadPool::Submit(task).IsErr()) {
            // 投入に失敗した entry だけを同期実行し、部分投入との二重実行を防ぐ。
            JobThunk(job, 0);
        }
    }
    return Ok();
}

/** 全 job の完了まで待つ。 */
void FJobGraph::Wait() noexcept
{
    if (m_bSubmitted) FThreadPool::Wait(m_Counter);
}

/** 依存カウンタだけを復元し、キャッシュ済みトポロジーを再利用可能にする。 */
void FJobGraph::Reset() noexcept
{
    Wait();
    for (u32 i = 0; i < m_JobCount; ++i) {
        /** 依存数を初期値へ戻す job。 */
        FJob* const job = JobAt(i);
        job->deps_remaining.Store(job->initial_deps, EMemoryOrder::Release);
        ++m_Diagnostics.reset_job_visits;
    }
    m_bSubmitted = false;
}

} // namespace acs
