// SPDX-License-Identifier: Apache-2.0
// AudioEngine の共有操作と別スレッド Shutdown の寿命契約を検証する。
#include "audio/AudioEngine.h"
#include "foundation/Move.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

namespace {

constexpr u32 kWaitTimeoutMilliseconds = 3000u;

struct FAudioOperationContext {
    CAudioEngine* Engine = nullptr;
    TAtomic<u32> Finished{0u};
    u32 ActiveCount = 0u;
};

struct FAudioShutdownContext {
    CAudioEngine* Engine = nullptr;
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

bool WaitForShutdownRequest(const CAudioEngine& Engine) noexcept
{
    for (u32 Elapsed = 0u; Elapsed < kWaitTimeoutMilliseconds; ++Elapsed)
    {
        if (Engine.IsShutdownRequestedForTesting())
        {
            return true;
        }
        SleepMs(1u);
    }
    return false;
}

void ActiveCountThread(void* UserData)
{
    auto* const Context = static_cast<FAudioOperationContext*>(UserData);
    Context->ActiveCount = Context->Engine->ActiveCount();
    Context->Finished.Store(1u, EMemoryOrder::Release);
}

void ShutdownThread(void* UserData)
{
    auto* const Context = static_cast<FAudioShutdownContext*>(UserData);
    Context->Engine->Shutdown();
    Context->Finished.Store(1u, EMemoryOrder::Release);
}

} // namespace

int main()
{
    CAudioEngine Engine;
    if (Engine.InitializeLifecycleTestState().IsErr())
    {
        return 1;
    }

    TAtomic<u32> GateEntered{0u};
    TAtomic<u32> GateRelease{0u};
    CAudioEngine::ConfigureLifecycleOperationTestGate(&GateEntered, &GateRelease);

    i32 ExitCode = 0;
    FAudioOperationContext FirstOperation;
    FirstOperation.Engine = &Engine;
    FThread FirstThread;
    TResult<FThread> FirstResult = FThread::Spawn(&ActiveCountThread, &FirstOperation);
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

    // Init と異なるスレッドからでも cookie を解除して全状態を破棄できる。
    FAudioShutdownContext ShutdownContext;
    ShutdownContext.Engine = &Engine;
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

    if (ExitCode == 0 && !WaitForShutdownRequest(Engine))
    {
        ExitCode = 5;
    }

    FAudioOperationContext SecondOperation;
    SecondOperation.Engine = &Engine;
    FThread SecondThread;
    if (ExitCode == 0)
    {
        TResult<FThread> SecondResult = FThread::Spawn(&ActiveCountThread, &SecondOperation);
        if (SecondResult.IsErr())
        {
            ExitCode = 6;
        }
        else
        {
            SecondThread = Move(SecondResult.Value());
        }
    }

    // Shutdown 要求後の新規操作は共有ロックへ入らず即座に拒否される。
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

    // 失敗経路でも待機中の全スレッドを必ず解放し、テスト自身が状態を残さない。
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
        Engine.Shutdown();
    }
    CAudioEngine::ConfigureLifecycleOperationTestGate(nullptr, nullptr);

    if (ExitCode == 0 && ShutdownContext.Finished.Load(EMemoryOrder::Acquire) == 0u)
    {
        ExitCode = 10;
    }
    if (ExitCode == 0 && Engine.HasLifecycleStateForTesting())
    {
        ExitCode = 11;
    }
    return ExitCode;
}
