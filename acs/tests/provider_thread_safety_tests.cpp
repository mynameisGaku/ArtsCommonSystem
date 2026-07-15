// SPDX-License-Identifier: Apache-2.0
// GameFramework の既定 provider 登録と取得を並行実行する契約試験。
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Move.h"
#include "gameframework/BackendClient.h"
#include "gameframework/CrashReporter.h"
#include "gameframework/MlRuntime.h"
#include "gameframework/OpenXrBridge.h"
#include "gameframework/ScriptHost.h"
#include "gameframework/SteamworksBridge.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;
using namespace acs::game;

namespace {

IBackendClient& ProvideBackendClient() noexcept
{
    return GetBackendStub();
}

IMatchmaker& ProvideMatchmaker() noexcept
{
    return GetMatchmakerStub();
}

ICrashReporterBackend& ProvideCrashReporter() noexcept
{
    return GetCrashStub();
}

IMlRuntime& ProvideMlRuntime() noexcept
{
    return GetMlRuntimeStub();
}

IOpenXrBridge& ProvideOpenXrBridge() noexcept
{
    return GetXrStub();
}

IScriptVm& ProvideScriptVm() noexcept
{
    return GetVmStub();
}

ISteamworksBridge& ProvideSteamworksBridge() noexcept
{
    return SteamworksBridgeStub::GetStub();
}

struct ProviderRaceContext {
    TAtomic<u32> Start{0u};
    TAtomic<u32> FailureCount{0u};
};

void SetEveryProvider(bool bInstall) noexcept
{
    SetBackendClientProvider(bInstall ? &ProvideBackendClient : nullptr);
    SetMatchmakerProvider(bInstall ? &ProvideMatchmaker : nullptr);
    SetCrashReporterProvider(bInstall ? &ProvideCrashReporter : nullptr);
    SetMlRuntimeProvider(bInstall ? &ProvideMlRuntime : nullptr);
    SetOpenXrBridgeProvider(bInstall ? &ProvideOpenXrBridge : nullptr);
    SetScriptVmProvider(bInstall ? &ProvideScriptVm : nullptr);
    SetSteamworksBridgeProvider(bInstall ? &ProvideSteamworksBridge : nullptr);
}

} // namespace

ACS_TEST(GameFramework, DefaultProvidersSupportConcurrentRegistrationAndLookup)
{
    constexpr usize kThreadCount = 8u;
    constexpr usize kIterationCount = 20000u;
    ProviderRaceContext Context;
    FThread Threads[kThreadCount];
    u32 SpawnedThreadCount = 0u;

    for (usize Index = 0u; Index < kThreadCount; ++Index) {
        auto ThreadResult = FThread::Spawn(
            [](void* UserData)
            {
                auto* const RaceContext = static_cast<ProviderRaceContext*>(UserData);
                while (RaceContext->Start.Load(EMemoryOrder::Acquire) == 0u) Yield();

                for (usize Iteration = 0u; Iteration < kIterationCount; ++Iteration) {
                    SetEveryProvider((Iteration & 1u) != 0u);
                    if (&GetDefaultBackendClient() != &GetBackendStub() ||
                        &GetDefaultMatchmaker() != &GetMatchmakerStub() ||
                        &GetDefaultCrashReporter() != &GetCrashStub() ||
                        &GetDefaultMlRuntime() != &GetMlRuntimeStub() ||
                        &GetDefaultOpenXrBridge() != &GetXrStub() ||
                        &GetDefaultScriptVm() != &GetVmStub() ||
                        &GetDefaultSteamworksBridge() != &SteamworksBridgeStub::GetStub()) {
                        RaceContext->FailureCount.FetchAdd(1u);
                    }
                }
            },
            &Context);
        if (ThreadResult.IsOk()) Threads[SpawnedThreadCount++] = Move(ThreadResult.Value());
    }

    EXPECT_EQ(SpawnedThreadCount, static_cast<u32>(kThreadCount));
    Context.Start.Store(1u, EMemoryOrder::Release);
    for (u32 Index = 0u; Index < SpawnedThreadCount; ++Index) Threads[Index].Join();
    SetEveryProvider(false);

    EXPECT_EQ(Context.FailureCount.Load(EMemoryOrder::Acquire), 0u);
}
