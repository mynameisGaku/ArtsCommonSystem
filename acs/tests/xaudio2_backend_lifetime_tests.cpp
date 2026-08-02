// SPDX-License-Identifier: Apache-2.0
// XAudio2Backend の共有操作と別スレッド Shutdown の寿命契約を検証する。
#include "foundation/Move.h"
#include "gameframework/audio_backend/XAudio2Backend.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;
using namespace acs::game;

namespace {

constexpr u32 kWaitTimeoutMilliseconds = 3000u;

struct FBackendOperationContext {
    CXAudio2Backend* Backend = nullptr;
    TAtomic<u32> Finished{0u};
    u32 ActiveVoiceCount = 0u;
};

struct FBackendShutdownContext {
    CXAudio2Backend* Backend = nullptr;
    TAtomic<u32> Finished{0u};
};

bool WaitForValue(const TAtomic<u32>& Value, u32 Expected) noexcept
{
    for (u32 Elapsed = 0u; Elapsed < kWaitTimeoutMilliseconds; ++Elapsed)
    {
        if (Value.Load(EMemoryOrder::Acquire) == Expected)
        {
            return true;
        }
        SleepMs(1u);
    }
    return false;
}

bool WaitForShutdownRequest(const CXAudio2Backend& Backend) noexcept
{
    for (u32 Elapsed = 0u; Elapsed < kWaitTimeoutMilliseconds; ++Elapsed)
    {
        if (Backend.IsShutdownRequestedForTesting())
        {
            return true;
        }
        SleepMs(1u);
    }
    return false;
}

void ActiveVoiceCountThread(void* UserData)
{
    auto* const Context = static_cast<FBackendOperationContext*>(UserData);
    Context->ActiveVoiceCount = Context->Backend->ActiveVoiceCount();
    Context->Finished.Store(1u, EMemoryOrder::Release);
}

void ShutdownThread(void* UserData)
{
    auto* const Context = static_cast<FBackendShutdownContext*>(UserData);
    Context->Backend->Shutdown();
    Context->Finished.Store(1u, EMemoryOrder::Release);
}

} // namespace

int main()
{
    CXAudio2Backend Backend;
    if (Backend.InitializeLifecycleTestState().IsErr())
    {
        return 1;
    }

    TAtomic<u32> GateEntered{0u};
    TAtomic<u32> GateRelease{0u};
    CXAudio2Backend::ConfigureLifecycleOperationTestGate(&GateEntered, &GateRelease);

    i32 ExitCode = 0;
    FBackendOperationContext FirstOperation;
    FirstOperation.Backend = &Backend;
    FThread FirstThread;
    TResult<FThread> FirstResult = FThread::Spawn(&ActiveVoiceCountThread, &FirstOperation);
    if (FirstResult.IsErr())
    {
        ExitCode = 2;
    }
    else
    {
        FirstThread = Move(FirstResult.Value());
    }

    if (ExitCode == 0 && !WaitForValue(GateEntered, 1u))
    {
        ExitCode = 3;
    }

    FBackendShutdownContext ShutdownContext;
    ShutdownContext.Backend = &Backend;
    FThread ShutdownWorker;
    if (ExitCode == 0)
    {
        TResult<FThread> ShutdownResult = FThread::Spawn(&ShutdownThread, &ShutdownContext);
        if (ShutdownResult.IsErr())
        {
            ExitCode = 4;
        }
        else
        {
            ShutdownWorker = Move(ShutdownResult.Value());
        }
    }

    if (ExitCode == 0 && !WaitForShutdownRequest(Backend))
    {
        ExitCode = 5;
    }

    FBackendOperationContext SecondOperation;
    SecondOperation.Backend = &Backend;
    FThread SecondThread;
    if (ExitCode == 0)
    {
        TResult<FThread> SecondResult = FThread::Spawn(&ActiveVoiceCountThread, &SecondOperation);
        if (SecondResult.IsErr())
        {
            ExitCode = 6;
        }
        else
        {
            SecondThread = Move(SecondResult.Value());
        }
    }

    if (ExitCode == 0 && !WaitForValue(SecondOperation.Finished, 1u))
    {
        ExitCode = 7;
    }
    if (ExitCode == 0 && ShutdownContext.Finished.Load(EMemoryOrder::Acquire) != 0u)
    {
        ExitCode = 8;
    }
    if (ExitCode == 0 && FirstOperation.Finished.Load(EMemoryOrder::Acquire) != 0u)
    {
        ExitCode = 9;
    }

    GateRelease.Store(1u, EMemoryOrder::Release);
    if (FirstThread.Joinable())
    {
        FirstThread.Join();
    }
    if (SecondThread.Joinable())
    {
        SecondThread.Join();
    }
    if (ShutdownWorker.Joinable())
    {
        ShutdownWorker.Join();
    }
    else
    {
        Backend.Shutdown();
    }
    CXAudio2Backend::ConfigureLifecycleOperationTestGate(nullptr, nullptr);

    if (ExitCode == 0 && ShutdownContext.Finished.Load(EMemoryOrder::Acquire) == 0u)
    {
        ExitCode = 10;
    }
    if (ExitCode == 0 && Backend.HasLifecycleStateForTesting())
    {
        ExitCode = 11;
    }
    return ExitCode;
}
