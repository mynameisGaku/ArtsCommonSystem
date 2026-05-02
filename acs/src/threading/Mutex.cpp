#include "threading/Mutex.h"
#include "foundation/Platform.h"

static_assert(sizeof(SRWLOCK) == sizeof(void*),
              "SRWLOCK shape changed — Mutex storage layout must be updated");

namespace acs {

Mutex::Mutex() noexcept {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

void Mutex::Lock() noexcept {
    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

bool Mutex::TryLock() noexcept {
    return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0;
}

void Mutex::Unlock() noexcept {
    ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

} // namespace acs
