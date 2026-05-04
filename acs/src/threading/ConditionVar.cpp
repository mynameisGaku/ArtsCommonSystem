// =============================================================================
// ACS Threading — ConditionVar 実装
//   Win32: CONDITION_VARIABLE + SRWLOCK
//   POSIX: pthread_cond_t   + pthread_mutex_t
// 両方とも Mutex オブジェクトの先頭バイトが native lock 実体である前提で動く
// (Mutex も同じ二系統実装を持つため、レイアウト互換が保たれる)。
// =============================================================================
#include "threading/ConditionVar.h"
#include "foundation/Compiler.h"

#if ACS_PLATFORM_WINDOWS
    #include "foundation/Platform.h"

    static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(void*) * 8,
                  "CONDITION_VARIABLE too large for inline storage");

    namespace acs {

    ConditionVar::ConditionVar() noexcept {
        InitializeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_impl[0]));
    }
    void ConditionVar::Wait(Mutex& m) noexcept {
        SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&_impl[0]),
                                  reinterpret_cast<SRWLOCK*>(&m), INFINITE, 0);
    }
    bool ConditionVar::WaitFor(Mutex& m, u32 timeout_ms) noexcept {
        BOOL ok = SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&_impl[0]),
                                            reinterpret_cast<SRWLOCK*>(&m),
                                            static_cast<DWORD>(timeout_ms), 0);
        return ok != 0;
    }
    void ConditionVar::NotifyOne() noexcept { WakeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_impl[0])); }
    void ConditionVar::NotifyAll() noexcept { WakeAllConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_impl[0])); }

    } // namespace acs

#elif ACS_PLATFORM_POSIX
    #include <pthread.h>
    #include <time.h>
    #include <errno.h>

    static_assert(sizeof(pthread_cond_t) <= sizeof(void*) * 8,
                  "pthread_cond_t too large for inline storage");

    namespace acs {

    ConditionVar::ConditionVar() noexcept {
        pthread_cond_init(reinterpret_cast<pthread_cond_t*>(&_impl[0]), nullptr);
    }
    void ConditionVar::Wait(Mutex& m) noexcept {
        pthread_cond_wait(reinterpret_cast<pthread_cond_t*>(&_impl[0]),
                          reinterpret_cast<pthread_mutex_t*>(&m));
    }
    bool ConditionVar::WaitFor(Mutex& m, u32 timeout_ms) noexcept {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        // ms を加算
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec  += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
        }
        int r = pthread_cond_timedwait(reinterpret_cast<pthread_cond_t*>(&_impl[0]),
                                       reinterpret_cast<pthread_mutex_t*>(&m),
                                       &ts);
        return r == 0;   // 0=signal、ETIMEDOUT=timeout
    }
    void ConditionVar::NotifyOne() noexcept { pthread_cond_signal(reinterpret_cast<pthread_cond_t*>(&_impl[0])); }
    void ConditionVar::NotifyAll() noexcept { pthread_cond_broadcast(reinterpret_cast<pthread_cond_t*>(&_impl[0])); }

    } // namespace acs

#else
    #error "ACS ConditionVar: unsupported platform"
#endif
