// ACS Threading — RAII lock guards (no <mutex>).
#pragma once

#include "threading/Mutex.h"
#include "threading/RwLock.h"

namespace acs {

class ScopedLock {
public:
    explicit ScopedLock(Mutex& m) noexcept : _m(m) { _m.Lock(); }
    ~ScopedLock() noexcept { _m.Unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
private:
    Mutex& _m;
};

class ScopedSharedLock {
public:
    explicit ScopedSharedLock(RwLock& r) noexcept : _r(r) { _r.LockShared(); }
    ~ScopedSharedLock() noexcept { _r.UnlockShared(); }
    ScopedSharedLock(const ScopedSharedLock&) = delete;
    ScopedSharedLock& operator=(const ScopedSharedLock&) = delete;
private:
    RwLock& _r;
};

class ScopedExclusiveLock {
public:
    explicit ScopedExclusiveLock(RwLock& r) noexcept : _r(r) { _r.LockExclusive(); }
    ~ScopedExclusiveLock() noexcept { _r.UnlockExclusive(); }
    ScopedExclusiveLock(const ScopedExclusiveLock&) = delete;
    ScopedExclusiveLock& operator=(const ScopedExclusiveLock&) = delete;
private:
    RwLock& _r;
};

} // namespace acs
