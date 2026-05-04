// =============================================================================
// ACS Threading — RwLock 実装 (Win32 SRWLOCK / POSIX pthread_rwlock_t)
// =============================================================================
#include "threading/RwLock.h"
#include "foundation/Compiler.h"

#if ACS_PLATFORM_WINDOWS
    #include "foundation/Platform.h"

    namespace acs {

    RwLock::RwLock() noexcept {
        InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&_impl[0]));
    }
    void RwLock::LockShared()    noexcept { AcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_impl[0])); }
    bool RwLock::TryLockShared() noexcept { return TryAcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_impl[0])) != 0; }
    void RwLock::UnlockShared()  noexcept { ReleaseSRWLockShared(reinterpret_cast<SRWLOCK*>(&_impl[0])); }
    void RwLock::LockExclusive()    noexcept { AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_impl[0])); }
    bool RwLock::TryLockExclusive() noexcept { return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_impl[0])) != 0; }
    void RwLock::UnlockExclusive()  noexcept { ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_impl[0])); }

    } // namespace acs

#elif ACS_PLATFORM_POSIX
    #include <pthread.h>

    static_assert(sizeof(pthread_rwlock_t) <= sizeof(void*) * 8,
                  "pthread_rwlock_t too large for RwLock inline storage");

    namespace acs {

    RwLock::RwLock() noexcept {
        pthread_rwlock_init(reinterpret_cast<pthread_rwlock_t*>(&_impl[0]), nullptr);
    }
    void RwLock::LockShared()    noexcept { pthread_rwlock_rdlock(reinterpret_cast<pthread_rwlock_t*>(&_impl[0])); }
    bool RwLock::TryLockShared() noexcept { return pthread_rwlock_tryrdlock(reinterpret_cast<pthread_rwlock_t*>(&_impl[0])) == 0; }
    void RwLock::UnlockShared()  noexcept { pthread_rwlock_unlock(reinterpret_cast<pthread_rwlock_t*>(&_impl[0])); }
    void RwLock::LockExclusive()    noexcept { pthread_rwlock_wrlock(reinterpret_cast<pthread_rwlock_t*>(&_impl[0])); }
    bool RwLock::TryLockExclusive() noexcept { return pthread_rwlock_trywrlock(reinterpret_cast<pthread_rwlock_t*>(&_impl[0])) == 0; }
    void RwLock::UnlockExclusive()  noexcept { pthread_rwlock_unlock(reinterpret_cast<pthread_rwlock_t*>(&_impl[0])); }

    } // namespace acs

#else
    #error "ACS RwLock: unsupported platform"
#endif
