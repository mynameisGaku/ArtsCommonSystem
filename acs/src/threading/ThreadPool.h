// ACS Threading — Work-stealing thread pool.
//
// Architecture:
//   * One pinned worker per logical CPU (configurable).
//   * Each worker owns a fixed-capacity Chase-Lev SPMC deque (Push/Pop on the
//     owner side require no atomics in the common case; Steal is CAS-based).
//   * External submitters drop work into a global Mutex-protected ready queue
//     that any worker can drain.
//   * Waiting participates in stealing (help-stealing) to avoid blocking and
//     to preserve forward progress under nested parallel_for.
//
// Tasks are POD copied by value into deque slots to avoid heap allocation in
// the hot path. Completion is tracked through a caller-supplied
// CompletionCounter; pass nullptr for fire-and-forget semantics.
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "threading/Atomic.h"

namespace acs {

class CompletionCounter {
public:
    CompletionCounter() noexcept = default;
    explicit CompletionCounter(u32 initial) noexcept : _v(initial) {}

    CompletionCounter(const CompletionCounter&) = delete;
    CompletionCounter& operator=(const CompletionCounter&) = delete;

    void Add(u32 n = 1) noexcept   { _v.FetchAdd(n); }
    void Done()         noexcept   { _v.FetchSub(1); }
    u32  Pending() const noexcept  { return _v.Load(MemoryOrder::Acquire); }
    bool Finished() const noexcept { return Pending() == 0; }

private:
    mutable Atomic<u32> _v {0};
};

using TaskFn = void (*)(void* user, u32 worker_index);

struct Task {
    TaskFn              fn       = nullptr;
    void*               user     = nullptr;
    CompletionCounter*  counter  = nullptr;
};

class ThreadPool {
public:
    // Initialize pool with `worker_count` workers (0 -> HardwareConcurrency()).
    // Calling Init() again after Shutdown() is allowed.
    static Result<void> Init(u32 worker_count = 0) noexcept;
    static void         Shutdown() noexcept;

    static u32          WorkerCount() noexcept;

    // Returns the worker index of the calling thread (0..N-1) or kNotAWorker
    // if the calling thread is not one of the pool's workers.
    static u32          CurrentWorkerIndex() noexcept;
    static constexpr u32 kNotAWorker = 0xFFFFFFFFu;

    // Submit a single task. If counter is non-null, it is incremented before
    // submission and decremented when the task finishes.
    static Result<void> Submit(const Task& t) noexcept;

    // Block until counter reaches 0. While blocked, the caller participates in
    // task stealing so deeply nested workloads do not deadlock.
    static void         Wait(CompletionCounter& counter) noexcept;

    // Convenience: parallel_for divides [begin, end) into chunks of `grain`
    // and dispatches them. Caller supplies storage for the per-range function
    // pointer + a pointer-sized user value.
    static Result<void> ParallelFor(u32 begin, u32 end, u32 grain,
                                    void (*body)(u32 i, u32 worker_index, void* user),
                                    void* user) noexcept;
};

} // namespace acs
