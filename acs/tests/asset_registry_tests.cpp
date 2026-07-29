// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "asset/AssetRegistry.h"
#include "asset/BinaryAsset.h"
#include "memory/SystemAllocator.h"
#include "platform/FileSystem.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"

using namespace acs;

namespace {

constexpr const wchar_t* kAsyncAssetPath = L"acs_asset_registry_lifetime_test.tmp";

class FBlockingAssetLoader final : public IAssetLoader {
public:
    AssetType TypeId() const noexcept override
    {
        return FBinaryAsset::StaticType();
    }
    const char* Extension() const noexcept override
    {
        return "tmp";
    }

    TResult<TSharedPtr<FAsset>> LoadFromBytes(FAssetId, const TArray<byte>&) noexcept override
    {
        entered.FetchAdd(1);
        while (release.Load(EMemoryOrder::Acquire) == 0)
            SleepMs(1);

        auto binary = MakeShared<FBinaryAsset>();
        if (!binary.Get()) return ACS_ERR(Memory, 220, "BlockingAssetLoader allocation failed");
        TSharedPtr<FAsset> asset(Move(binary));
        return TResult<TSharedPtr<FAsset>>(OkInit, Move(asset));
    }

    TAtomic<u32> entered{0};
    TAtomic<u32> release{0};
};

struct FRegistryDeleteContext {
    FAssetRegistry* registry = nullptr;
    TAtomic<u32> finished{0};
};

void DeleteRegistry(void* user) noexcept
{
    auto* context = static_cast<FRegistryDeleteContext*>(user);
    delete context->registry;
    context->finished.Store(1, EMemoryOrder::Release);
}

} // namespace

ACS_TEST(FAssetRegistry, DestructorWaitsForAsyncLoad)
{
    FThreadPool::Shutdown();
    EXPECT_TRUE(FThreadPool::Init(1).IsOk());

    const byte file_data[] = {1, 2, 3, 4};
    EXPECT_TRUE(FFileSystem::WriteAllBytes(kAsyncAssetPath, file_data, sizeof(file_data)).IsOk());

    FBlockingAssetLoader loader;
    auto* registry = new FAssetRegistry();
    EXPECT_TRUE(registry != nullptr);
    if (!registry) {
        FThreadPool::Shutdown();
        (void)FFileSystem::Delete(kAsyncAssetPath);
        return;
    }
    registry->RegisterLoader(&loader);
    FAssetFuture future = registry->LoadAsync(kAsyncAssetPath);
    EXPECT_TRUE(future.Valid());

    for (u32 i = 0; i < 2000 && loader.entered.Load(EMemoryOrder::Acquire) == 0; ++i)
        SleepMs(1);
    EXPECT_EQ(loader.entered.Load(EMemoryOrder::Acquire), 1u);

    FRegistryDeleteContext delete_context{registry};
    auto destroyer = FThread::Spawn(&DeleteRegistry, &delete_context);
    EXPECT_TRUE(destroyer.IsOk());
    SleepMs(20);
    EXPECT_EQ(delete_context.finished.Load(EMemoryOrder::Acquire), 0u);

    loader.release.Store(1, EMemoryOrder::Release);
    if (destroyer.IsOk())
        destroyer.Value().Join();
    else
        DeleteRegistry(&delete_context);

    EXPECT_EQ(delete_context.finished.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_TRUE(future.Get().IsOk());

    FThreadPool::Shutdown();
    (void)FFileSystem::Delete(kAsyncAssetPath);
}

ACS_TEST(FAssetRegistry, ConcurrentSamePathAsyncLoadsShareOneJobAndIdentity)
{
    FThreadPool::Shutdown();
    EXPECT_TRUE(FThreadPool::Init(2).IsOk());

    const byte file_data[] = {9, 8, 7, 6};
    EXPECT_TRUE(FFileSystem::WriteAllBytes(kAsyncAssetPath, file_data, sizeof(file_data)).IsOk());

    FBlockingAssetLoader loader;
    FAssetRegistry registry;
    registry.RegisterLoader(&loader);

    FAssetFuture first = registry.LoadAsync(kAsyncAssetPath);
    for (u32 i = 0; i < 2000 && loader.entered.Load(EMemoryOrder::Acquire) == 0; ++i) {
        SleepMs(1);
    }
    FAssetFuture second = registry.LoadAsync(kAsyncAssetPath);
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_EQ(loader.entered.Load(EMemoryOrder::Acquire), 1u);

    loader.release.Store(1, EMemoryOrder::Release);
    const auto first_result = first.Get();
    const auto second_result = second.Get();
    EXPECT_TRUE(first_result.IsOk());
    EXPECT_TRUE(second_result.IsOk());
    if (first_result.IsOk() && second_result.IsOk()) {
        EXPECT_TRUE(first_result.Value().Get() == second_result.Value().Get());
    }
    EXPECT_EQ(loader.entered.Load(EMemoryOrder::Acquire), 1u);

    registry.Shutdown();
    FThreadPool::Shutdown();
    (void)FFileSystem::Delete(kAsyncAssetPath);
}

ACS_TEST(FAssetRegistry, ShutdownClosesLoadGate)
{
    FBlockingAssetLoader loader;
    FAssetRegistry registry;
    registry.RegisterLoader(&loader);
    registry.Shutdown();

    EXPECT_TRUE(registry.Load(kAsyncAssetPath).IsErr());
    FAssetFuture future = registry.LoadAsync(kAsyncAssetPath);
    EXPECT_TRUE(future.Valid());
    EXPECT_TRUE(future.IsReady());
    EXPECT_TRUE(future.Get().IsErr());
}

ACS_TEST(FAssetRegistry, RestartPreservesInjectedAllocatorAndReopensGate)
{
    FSystemAllocator allocator;
    FBlockingAssetLoader loader;
    FAssetRegistry registry(allocator);

    registry.RegisterLoader(&loader);
    EXPECT_TRUE(allocator.BytesAllocated() > 0u);

    registry.Shutdown();
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
    EXPECT_TRUE(registry.Load(kAsyncAssetPath).IsErr());

    registry.Restart();
    registry.RegisterLoader(&loader);
    EXPECT_TRUE(allocator.BytesAllocated() > 0u);

    registry.Shutdown();
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
}
