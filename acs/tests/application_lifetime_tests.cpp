// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "app/Application.h"
#include "foundation/Log.h"
#include "foundation/StackTrace.h"
#include "memory/MemorySystem.h"
#include "memory/UniquePtr.h"
#include "threading/ThreadPool.h"

using namespace acs;

namespace acs::app_internal {

/** GPU を起動せず FApplication の基盤寿命ガードだけを検証するテスト用窓口。 */
class FApplicationTestAccess {
public:
    /**
     * Run と同じ所有権判定で Logger、MemorySystem、ThreadPool を初期化する。
     *
     * @param application 検証対象。
     * @param memory_config MemorySystem の設定。
     * @return 全基盤が利用可能になれば true。
     */
    static bool InitializeFoundation(FApplication& application, const FMemorySystemConfig& memory_config) noexcept
    {
        FLogConfig log_config{};
        log_config.console = false;
        log_config.debug_output = false;
        application.m_RuntimeFoundationLifetime.InitializeLogger(log_config);

        const auto memory_result = application.m_RuntimeFoundationLifetime.InitializeMemorySystem(memory_config);
        if (memory_result.IsErr()) return false;

        const auto thread_result = application.m_RuntimeFoundationLifetime.InitializeThreadPool(1);
        return thread_result.IsOk();
    }
};

} // acs::app_internal 名前空間

namespace {

/** 派生オブジェクトの各破棄段階で観測した基盤状態。 */
struct FApplicationLifetimeState {
    bool derived_destructor_called = false;
    bool renderer_alive = false;
    bool allocation_destructor_called = false;
    bool memory_alive_during_allocation_destructor = false;
};

/** 派生メンバの破棄時点で MemorySystem が生存していることを記録する。 */
class FAllocationLifetimeProbe {
public:
    explicit FAllocationLifetimeProbe(FApplicationLifetimeState* state) noexcept : m_State(state)
    {
    }

    ~FAllocationLifetimeProbe() noexcept
    {
        m_State->allocation_destructor_called = true;
        m_State->memory_alive_during_allocation_destructor = FMemorySystem::Get(ESegment::Default) != nullptr;
    }

private:
    FApplicationLifetimeState* m_State = nullptr;
};

/** GPU を初期化せず、派生メンバと基底メンバの破棄順だけを観測するアプリ。 */
class FLifetimeProbeApplication final : public FApplication {
public:
    explicit FLifetimeProbeApplication(FApplicationLifetimeState* state) noexcept : m_State(state)
    {
    }

    ~FLifetimeProbeApplication() noexcept override
    {
        m_State->derived_destructor_called = true;
        m_State->renderer_alive = GetRenderer().Device() == nullptr;
    }

    /** 現在の既定アロケータから派生所有メンバを生成する。 */
    bool CreateAllocationProbe(FAllocator* expected_allocator) noexcept
    {
        m_AllocationProbe = MakeUnique<FAllocationLifetimeProbe>(m_State);
        return m_AllocationProbe.Get() != nullptr && m_AllocationProbe.GetAllocator() == expected_allocator;
    }

private:
    FApplicationLifetimeState* m_State = nullptr;
    TUniquePtr<FAllocationLifetimeProbe> m_AllocationProbe;
};

FMemorySystemConfig ApplicationMemoryConfig() noexcept
{
    FMemorySystemConfig config = FMemorySystem::DefaultConfig();
    config.install_as_default_allocator = true;
    return config;
}

} // 無名名前空間

ACS_TEST(FApplicationLifetime, OwnedFoundationOutlivesDerivedMembers)
{
    FThreadPool::Shutdown();
    FMemorySystem::Shutdown();
    FStackTrace::ShutdownSymbolResolver();
    EXPECT_TRUE(FLogger::IsInitialized());

    FApplicationLifetimeState state{};
    {
        FLifetimeProbeApplication application(&state);
        EXPECT_TRUE(
            acs::app_internal::FApplicationTestAccess::InitializeFoundation(application, ApplicationMemoryConfig()));

        FAllocator* const default_allocator = FMemorySystem::Get(ESegment::Default);
        EXPECT_TRUE(default_allocator != nullptr);
        EXPECT_TRUE(application.CreateAllocationProbe(default_allocator));

        FStackTrace trace;
        trace.Capture();
        trace.Resolve();
        EXPECT_TRUE(FStackTrace::IsSymbolResolverInitialized());
    }

    EXPECT_TRUE(state.derived_destructor_called);
    EXPECT_TRUE(state.renderer_alive);
    EXPECT_TRUE(state.allocation_destructor_called);
    EXPECT_TRUE(state.memory_alive_during_allocation_destructor);
    EXPECT_TRUE(FMemorySystem::Get(ESegment::Default) == nullptr);
    EXPECT_EQ(FThreadPool::WorkerCount(), 0u);
    EXPECT_TRUE(!FStackTrace::IsSymbolResolverInitialized());
    EXPECT_TRUE(FLogger::IsInitialized());
}

ACS_TEST(FApplicationLifetime, ExternallyOwnedFoundationSurvivesApplication)
{
    FThreadPool::Shutdown();
    FMemorySystem::Shutdown();
    FStackTrace::ShutdownSymbolResolver();

    FStackTrace external_trace;
    external_trace.Capture();
    external_trace.Resolve();
    EXPECT_TRUE(FStackTrace::IsSymbolResolverInitialized());

    const FMemorySystemConfig memory_config = ApplicationMemoryConfig();
    EXPECT_TRUE(FMemorySystem::Init(memory_config).IsOk());
    EXPECT_TRUE(FThreadPool::Init(1).IsOk());

    FApplicationLifetimeState state{};
    {
        FLifetimeProbeApplication application(&state);
        EXPECT_TRUE(acs::app_internal::FApplicationTestAccess::InitializeFoundation(application, memory_config));
        EXPECT_TRUE(application.CreateAllocationProbe(FMemorySystem::Get(ESegment::Default)));
    }

    EXPECT_TRUE(state.allocation_destructor_called);
    EXPECT_TRUE(state.memory_alive_during_allocation_destructor);
    EXPECT_TRUE(FMemorySystem::Get(ESegment::Default) != nullptr);
    EXPECT_EQ(FThreadPool::WorkerCount(), 1u);
    EXPECT_TRUE(FStackTrace::IsSymbolResolverInitialized());
    EXPECT_TRUE(FLogger::IsInitialized());

    FThreadPool::Shutdown();
    FMemorySystem::Shutdown();
    FStackTrace::ShutdownSymbolResolver();
}

/** ムーブ構築とムーブ代入が最小化状態を正しく移すことを確認する。 */
ACS_TEST(FApplicationLifetime, WindowMovePreservesMinimizedState)
{
    /** 最小化状態を持つ移動元ウィンドウ。 */
    FWindow source;
    source.UpdateSize_Internal(0, 0, true);
    EXPECT_TRUE(source.IsMinimized());

    /** 移動構築後のウィンドウ。 */
    FWindow moved(Move(source));
    EXPECT_TRUE(moved.IsMinimized());
    EXPECT_TRUE(!source.IsMinimized());

    /** 移動代入先のウィンドウ。 */
    FWindow assigned;
    assigned = Move(moved);
    EXPECT_TRUE(assigned.IsMinimized());
    EXPECT_TRUE(!moved.IsMinimized());

    assigned.UpdateSize_Internal(1280, 720, false);
    EXPECT_TRUE(!assigned.IsMinimized());
    EXPECT_EQ(assigned.Width(), 1280u);
    EXPECT_EQ(assigned.Height(), 720u);
}
