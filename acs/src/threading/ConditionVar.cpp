#include "threading/ConditionVar.h"
#include "foundation/Platform.h"

static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*),
              "CONDITION_VARIABLE shape changed — storage layout must be updated");

namespace acs {

ConditionVar::ConditionVar() noexcept {
    InitializeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0]));
}

void ConditionVar::Wait(Mutex& m) noexcept {
    SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0]),
                              reinterpret_cast<SRWLOCK*>(&m), INFINITE, 0);
}

bool ConditionVar::WaitFor(Mutex& m, u32 timeout_ms) noexcept {
    BOOL ok = SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0]),
                                        reinterpret_cast<SRWLOCK*>(&m),
                                        static_cast<DWORD>(timeout_ms), 0);
    return ok != 0;
}

void ConditionVar::NotifyOne() noexcept { WakeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0])); }
void ConditionVar::NotifyAll() noexcept { WakeAllConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0])); }

} // namespace acs
