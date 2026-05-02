#include "threading/RwLock.h"
#include "foundation/Platform.h"

namespace acs {

RwLock::RwLock() noexcept {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

void RwLock::LockShared()    noexcept { AcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])); }
bool RwLock::TryLockShared() noexcept { return TryAcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0; }
void RwLock::UnlockShared()  noexcept { ReleaseSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])); }

void RwLock::LockExclusive()    noexcept { AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])); }
bool RwLock::TryLockExclusive() noexcept { return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0; }
void RwLock::UnlockExclusive()  noexcept { ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])); }

} // namespace acs
