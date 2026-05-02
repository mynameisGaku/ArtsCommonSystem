// ACS Threading — Condition variable (Win32 CONDITION_VARIABLE).
#pragma once

#include "foundation/Types.h"
#include "threading/Mutex.h"

namespace acs {

class ConditionVar {
public:
    ConditionVar() noexcept;
    ~ConditionVar() noexcept = default;

    ConditionVar(const ConditionVar&) = delete;
    ConditionVar& operator=(const ConditionVar&) = delete;

    // Wait must be called with `m` already locked. Returns true on signal,
    // false on timeout.
    void Wait(Mutex& m) noexcept;
    bool WaitFor(Mutex& m, u32 timeout_ms) noexcept;

    void NotifyOne() noexcept;
    void NotifyAll() noexcept;

private:
    void* _cv[1];
};

} // namespace acs
