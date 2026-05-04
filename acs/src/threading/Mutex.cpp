// =============================================================================
// ACS Threading — Mutex 実装 (Win32 SRWLOCK / POSIX pthread_mutex_t)
// =============================================================================
#include "threading/Mutex.h"
#include "foundation/Compiler.h"

#if ACS_PLATFORM_WINDOWS
    #include "foundation/Platform.h"

    static_assert(sizeof(SRWLOCK) <= sizeof(void*) * 8,
                  "SRWLOCK too large for Mutex inline storage");

    namespace acs {

    Mutex::Mutex() noexcept {
        InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&_impl[0]));
    }
    void Mutex::Lock() noexcept {
        AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_impl[0]));
    }
    bool Mutex::TryLock() noexcept {
        return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_impl[0])) != 0;
    }
    void Mutex::Unlock() noexcept {
        ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_impl[0]));
    }

    } // namespace acs

#elif ACS_PLATFORM_POSIX
    #include <pthread.h>

    static_assert(sizeof(pthread_mutex_t) <= sizeof(void*) * 8,
                  "pthread_mutex_t too large for Mutex inline storage");

    namespace acs {

    Mutex::Mutex() noexcept {
        pthread_mutex_init(reinterpret_cast<pthread_mutex_t*>(&_impl[0]), nullptr);
    }
    void Mutex::Lock() noexcept {
        pthread_mutex_lock(reinterpret_cast<pthread_mutex_t*>(&_impl[0]));
    }
    bool Mutex::TryLock() noexcept {
        return pthread_mutex_trylock(reinterpret_cast<pthread_mutex_t*>(&_impl[0])) == 0;
    }
    void Mutex::Unlock() noexcept {
        pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(&_impl[0]));
    }

    } // namespace acs

#else
    #error "ACS Mutex: unsupported platform"
#endif
