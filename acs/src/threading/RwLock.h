// ACS Threading — Reader/writer lock (Win32 SRWLOCK).
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class RwLock {
public:
    RwLock() noexcept;
    ~RwLock() noexcept = default;

    RwLock(const RwLock&) = delete;
    RwLock& operator=(const RwLock&) = delete;

    void LockShared()    noexcept;
    bool TryLockShared() noexcept;
    void UnlockShared()  noexcept;

    void LockExclusive()    noexcept;
    bool TryLockExclusive() noexcept;
    void UnlockExclusive()  noexcept;

private:
    void* _srw[1];
};

} // namespace acs
