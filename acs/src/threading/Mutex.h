// ACS Threading — Exclusive mutex (Win32 SRWLOCK exclusive).
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class Mutex {
public:
    Mutex() noexcept;
    ~Mutex() noexcept = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock()    noexcept;
    bool TryLock() noexcept;
    void Unlock()  noexcept;

private:
    // SRWLOCK is opaque void*-sized — declare as such to avoid <windows.h>.
    void* _srw[1];
};

} // namespace acs
