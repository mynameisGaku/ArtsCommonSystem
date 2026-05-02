// ACS Threading — Thread spawn / join / sleep / yield.
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "threading/ThreadId.h"

namespace acs {

using ThreadEntry = void (*)(void* user);

struct ThreadConfig {
    const wchar_t* name        = nullptr;  // shown in debugger
    u32            stack_bytes = 0;        // 0 = system default
    i32            priority    = 0;        // -2..+2 (THREAD_PRIORITY_*)
    u64            affinity    = 0;        // 0 = no pin; else GROUP_AFFINITY mask
};

class Thread {
public:
    Thread() noexcept = default;
    ~Thread() noexcept;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;

    static Result<Thread> Spawn(ThreadEntry entry, void* user,
                                const ThreadConfig& cfg = {}) noexcept;

    bool Joinable() const noexcept { return _handle != nullptr; }
    void Join() noexcept;
    void Detach() noexcept;

    ThreadId Id() const noexcept { return _id; }

private:
    void*    _handle = nullptr;
    ThreadId _id     = {};
};

void SleepMs(u32 ms) noexcept;
void Yield() noexcept;
u32  HardwareConcurrency() noexcept;

} // namespace acs
