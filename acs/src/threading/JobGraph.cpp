// JobGraph 実装
#include "threading/JobGraph.h"
#include "foundation/Log.h"

namespace acs {

void JobHandle::DependOn(JobHandle upstream) noexcept {
    if (!IsValid() || !upstream.IsValid() || upstream.graph != graph) return;
    graph->AddDependency(upstream, *this);
}

JobHandle JobGraph::Add(JobFn fn, void* user) noexcept {
    if (_submitted || !fn) return JobHandle{};
    auto* j = new Job();
    j->fn    = fn;
    j->user  = user;
    j->owner = this;
    u32 idx = static_cast<u32>(_jobs.Size());
    _jobs.PushBack(j);
    return JobHandle{ this, idx };
}

void JobGraph::AddDependency(JobHandle upstream, JobHandle downstream) noexcept {
    if (_submitted) return;
    if (!upstream.IsValid() || !downstream.IsValid()) return;
    if (upstream.graph != this || downstream.graph != this) return;
    if (upstream.index == downstream.index) return;  // self-dep は無視

    Job* up = _jobs[upstream.index];
    up->dependents.PushBack(downstream.index);

    Job* dn = _jobs[downstream.index];
    dn->initial_deps += 1;
    dn->deps_remaining.Store(dn->initial_deps);
}

void JobGraph::JobThunk(void* user, u32 worker_index) noexcept {
    auto* j = static_cast<Job*>(user);
    auto* graph = j->owner;

    // 本体実行
    j->fn(j->user, worker_index);

    // 依存先の deps_remaining を減らし、0 になったものを ThreadPool に投入
    for (usize i = 0; i < j->dependents.Size(); ++i) {
        u32 dep_idx = j->dependents[i];
        Job* down = graph->_jobs[dep_idx];
        u32 prev = down->deps_remaining.FetchSub(1);
        if (prev == 1) {
            // 自分が最後の依存を解いた → 起動
            Task t{};
            t.fn      = &JobGraph::JobThunk;
            t.user    = down;
            t.counter = nullptr;  // counter は graph 全体で 1 度だけ Add し、ここで Done する
            (void)ThreadPool::Submit(t);
        }
    }

    // 自身の完了を counter に通知
    graph->_counter.Done();
}

Result<void> JobGraph::Submit() noexcept {
    if (_submitted) return ACS_ERR(Threading, 1, "JobGraph already submitted");
    if (_jobs.Size() == 0) {
        _submitted = true;
        return Ok();
    }

    _submitted = true;

    // 全 job を counter にカウント (それぞれ ThreadPool::Submit が +1 するが、
    // ここでは依存待ち状態の job も含めて先に積んでおく必要がある)
    _counter.Add(static_cast<u32>(_jobs.Size()));

    // 依存 0 の job を ThreadPool に投入
    bool any_started = false;
    for (usize i = 0; i < _jobs.Size(); ++i) {
        Job* j = _jobs[i];
        if (j->deps_remaining.Load(MemoryOrder::Acquire) == 0) {
            Task t{};
            t.fn      = &JobGraph::JobThunk;
            t.user    = j;
            t.counter = nullptr;  // すでに上で加算済みなので Submit に再加算させない
            (void)ThreadPool::Submit(t);
            any_started = true;
        }
    }

    if (!any_started && _jobs.Size() > 0) {
        // 全部依存待ちだとデッドロック (循環依存)
        ACS_LOG_ERROR("JobGraph::Submit: no entry job (cyclic dependency?)");
        return ACS_ERR(Threading, 2, "JobGraph: no entry job");
    }
    return Ok();
}

void JobGraph::Wait() noexcept {
    if (!_submitted) return;
    // Submit 時に Add(N) したカウンタを、Job 完了で Done() する仕組みが要る。
    // JobThunk 内で counter.Done() を呼ぶよう改修する必要があるが、
    // ThreadPool::Submit に counter を渡せば自動 Add+Done してくれる。
    // 上の実装では Submit 時に自前で Add してしまったので調整 -- このシンプル版では
    // ThreadPool::Submit に counter を渡して任せる方がきれい。実装は素直にする:
    ThreadPool::Wait(_counter);
}

void JobGraph::Reset() noexcept {
    for (usize i = 0; i < _jobs.Size(); ++i) {
        _jobs[i]->deps_remaining.Store(_jobs[i]->initial_deps);
    }
    _submitted = false;
}

} // namespace acs
